#!/usr/bin/env python3
"""
chromium-mcp-client.py
stdio ↔ Chromium MCP Unix socket 브릿지 (프레이밍 변환 포함)

Claude CLI(raw JSON, 줄바꿈 구분) ↔ Chromium MCP(Content-Length 프레이밍)

환경변수:
  CHROMIUM_MCP_SOCKET  — 커스텀 소켓 경로
                         (기본 후보: /tmp/.chromium-mcp-proxy.sock, /tmp/.chromium-mcp.sock)
"""

from __future__ import annotations

import json
import os
import select
import signal
import socket
import subprocess
import sys
import time

DEFAULT_CHROMIUM_SOCKET = '/tmp/.chromium-mcp.sock'
DEFAULT_PROXY_SOCKET = '/tmp/.chromium-mcp-proxy.sock'
CUSTOM_SOCKET = os.environ.get('CHROMIUM_MCP_SOCKET', '').strip()
DAEMON_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chromium-mcp-daemon.py')
BUF_SIZE = 65536
CONNECT_TIMEOUT = 30

_IMAGE_PREFIXES = (b'iVBORw0KGgo', b'/9j/', b'R0lGOD', b'UklGR', b'Qk')

_running = True


def _unwrap_protocol_response(result: dict) -> dict | None:
    """C++ NormalizeToolResult가 프로토콜 응답(initialize, tools/list)까지
    content[]로 래핑해버린 경우, 원래 형태로 복원한다.
    복원할 수 없으면 None을 반환한다."""
    content = result.get('content')
    if not isinstance(content, list) or len(content) != 1:
        return None
    item = content[0]
    if item.get('type') != 'text':
        return None
    text = item.get('text', '')
    try:
        inner = json.loads(text)
    except (json.JSONDecodeError, ValueError):
        return None
    if not isinstance(inner, dict):
        return None
    # initialize 응답: protocolVersion 키 존재
    if 'protocolVersion' in inner:
        return inner
    # tools/list 응답: tools 키 존재
    if 'tools' in inner:
        return inner
    return None


def _normalize_mcp_response(body: bytes) -> bytes:
    """MCP 응답 정규화:
    - 프로토콜 응답(initialize, tools/list)이 content[]로 잘못 래핑된 경우 → 언래핑
    - 도구 응답에 content[]가 없는 경우 → 래핑
    """
    try:
        msg = json.loads(body)
    except (json.JSONDecodeError, ValueError):
        return body

    result = msg.get('result')
    if not isinstance(result, dict):
        return body

    # content[]가 이미 있는 경우
    if isinstance(result.get('content'), list):
        # 프로토콜 응답이 잘못 래핑된 경우 복원
        unwrapped = _unwrap_protocol_response(result)
        if unwrapped is not None:
            msg['result'] = unwrapped
            return json.dumps(msg, ensure_ascii=False).encode('utf-8')
        return body

    is_error = result.get('isError', False)

    # base64 이미지 감지
    data = result.get('data')
    if isinstance(data, str) and len(data) > 100:
        data_bytes = data.encode('ascii', errors='ignore')
        if any(data_bytes.startswith(p) for p in _IMAGE_PREFIXES):
            result_new = {
                'content': [{'type': 'image', 'data': data, 'mimeType': 'image/png'}]
            }
            msg['result'] = result_new
            return json.dumps(msg, ensure_ascii=False).encode('utf-8')

    # 일반 텍스트 래핑
    text = json.dumps(result, ensure_ascii=False)
    result_new = {
        'content': [{'type': 'text', 'text': text}]
    }
    if is_error:
        result_new['isError'] = True
    msg['result'] = result_new
    return json.dumps(msg, ensure_ascii=False).encode('utf-8')


def _handle_signal(signum, frame):
    global _running
    _running = False


def _socket_candidates() -> list[str]:
    """연결 시도할 소켓 경로 목록(중복 제거)을 반환한다."""
    candidates: list[str] = []
    for path in (CUSTOM_SOCKET, DEFAULT_PROXY_SOCKET, DEFAULT_CHROMIUM_SOCKET):
        if not path:
            continue
        if path not in candidates:
            candidates.append(path)
    return candidates


def connect_chromium_socket() -> socket.socket:
    """로컬 프록시/직접 소켓에 연결한다. 없으면 데몬을 시작한 뒤 재시도한다."""
    for path in _socket_candidates():
        if os.path.exists(path):
            try:
                sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                sock.settimeout(2)
                sock.connect(path)
                sock.settimeout(None)
                return sock
            except Exception:
                pass

    # 데몬 시작 후 재시도
    subprocess.Popen(
        [sys.executable, DAEMON_SCRIPT],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )

    deadline = time.time() + CONNECT_TIMEOUT
    while time.time() < deadline:
        for path in _socket_candidates():
            if os.path.exists(path):
                try:
                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sock.settimeout(2)
                    sock.connect(path)
                    sock.settimeout(None)
                    return sock
                except Exception:
                    pass
        time.sleep(0.5)

    sys.stderr.write('[chromium-mcp-client] 소켓 연결 실패\n')
    sys.exit(1)


def proxy_loop(sock: socket.socket) -> None:
    """
    stdin(raw JSON) ↔ socket(Content-Length framed) 양방향 변환 프록시.

    stdin → socket: raw JSON에 Content-Length 헤더를 붙여 전송
    socket → stdout: Content-Length 프레이밍을 파싱하고 body만 출력
    """
    stdin_fd = sys.stdin.buffer.fileno()
    stdout_fd = sys.stdout.buffer.fileno()

    stdin_buf = b''
    sock_buf = b''

    while _running:
        try:
            readable, _, exceptional = select.select(
                [stdin_fd, sock], [], [stdin_fd, sock], 1.0
            )
        except (ValueError, OSError):
            break
        if exceptional:
            break

        for fd in readable:
            if fd == stdin_fd:
                try:
                    data = os.read(stdin_fd, BUF_SIZE)
                except OSError:
                    return
                if not data:
                    return

                stdin_buf += data

                while b'\n' in stdin_buf:
                    line, stdin_buf = stdin_buf.split(b'\n', 1)
                    line = line.strip()
                    if not line:
                        continue
                    framed = (
                        b'Content-Length: ' + str(len(line)).encode() +
                        b'\r\n\r\n' + line
                    )
                    try:
                        sock.sendall(framed)
                    except OSError:
                        return

            elif fd == sock:
                try:
                    data = sock.recv(BUF_SIZE)
                except OSError:
                    return
                if not data:
                    return

                sock_buf += data

                while sock_buf:
                    header_end = sock_buf.find(b'\r\n\r\n')
                    if header_end < 0:
                        break

                    header_section = sock_buf[:header_end]
                    content_length = None
                    for hdr_line in header_section.split(b'\r\n'):
                        if hdr_line.lower().startswith(b'content-length:'):
                            try:
                                content_length = int(hdr_line.split(b':', 1)[1].strip())
                            except ValueError:
                                pass

                    if content_length is None:
                        break

                    body_start = header_end + 4
                    body_end = body_start + content_length
                    if len(sock_buf) < body_end:
                        break

                    body = sock_buf[body_start:body_end]
                    sock_buf = sock_buf[body_end:]

                    # MCP 응답 정규화: content[] 형식 보장
                    body = _normalize_mcp_response(body)

                    try:
                        os.write(stdout_fd, body + b'\n')
                    except OSError:
                        return

        # 줄바꿈 없이 남은 단일 JSON은 타임아웃 시 프레이밍 처리
        if not readable and stdin_buf.strip():
            line = stdin_buf.strip()
            stdin_buf = b''
            framed = (
                b'Content-Length: ' + str(len(line)).encode() +
                b'\r\n\r\n' + line
            )
            try:
                sock.sendall(framed)
            except OSError:
                return


def main() -> None:
    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    sock = connect_chromium_socket()
    try:
        proxy_loop(sock)
    finally:
        try:
            sock.close()
        except Exception:
            pass


if __name__ == '__main__':
    main()
