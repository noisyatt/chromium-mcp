#!/usr/bin/env python3
"""
chromium-mcp-client.py
stdio ↔ Chromium MCP 소켓 브릿지 (프레이밍 변환 포함)

Claude CLI (raw JSON, 줄바꿈 구분) ↔ Chromium MCP (Content-Length 프레이밍)
"""
import os
import select
import signal
import socket
import subprocess
import sys
import time

CHROMIUM_SOCKET = '/tmp/.chromium-mcp.sock'
PROXY_SOCKET = '/tmp/.chromium-mcp-proxy.sock'
DAEMON_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chromium-mcp-daemon.py')
BUF_SIZE = 65536
CONNECT_TIMEOUT = 30

_running = True


def _handle_signal(signum, frame):
    global _running
    _running = False


def connect_chromium_socket() -> socket.socket:
    """데몬 프록시 우선 연결. 실패 시 Chromium 직접, 최후에 데몬 시작."""
    for path in [PROXY_SOCKET, CHROMIUM_SOCKET]:
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
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    deadline = time.time() + CONNECT_TIMEOUT
    while time.time() < deadline:
        for path in [CHROMIUM_SOCKET, PROXY_SOCKET]:
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
    sys.exit(1)


def proxy_loop(sock: socket.socket) -> None:
    """
    stdin(raw JSON) ↔ socket(Content-Length framed) 양방향 변환 프록시.

    stdin → socket: raw JSON에 Content-Length 헤더를 붙여서 전송
    socket → stdout: Content-Length 프레이밍을 파싱하고 바디만 stdout에 출력
    """
    stdin_fd = sys.stdin.buffer.fileno()
    stdout_fd = sys.stdout.buffer.fileno()

    # stdin 쪽 미처리 버퍼 (줄 단위 파싱용)
    stdin_buf = b''
    # socket 쪽 미처리 버퍼 (Content-Length 파싱용)
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
                # === stdin → socket (Content-Length 프레이밍 추가) ===
                try:
                    data = os.read(stdin_fd, BUF_SIZE)
                except OSError:
                    return
                if not data:
                    return

                stdin_buf += data

                # 완전한 JSON 메시지를 줄 단위로 추출
                while b'\n' in stdin_buf:
                    line, stdin_buf = stdin_buf.split(b'\n', 1)
                    line = line.strip()
                    if not line:
                        continue
                    # Content-Length 프레이밍 추가
                    framed = (
                        b'Content-Length: ' + str(len(line)).encode() +
                        b'\r\n\r\n' + line
                    )
                    try:
                        sock.sendall(framed)
                    except OSError:
                        return

                # 줄바꿈 없이 들어온 데이터도 처리 (단일 JSON)
                if stdin_buf and stdin_buf.strip():
                    # 아직 줄바꿈이 안 왔을 수 있으므로 대기
                    # 단, select 타임아웃 후에도 남아있으면 처리
                    pass

            elif fd == sock:
                # === socket → stdout (Content-Length 프레이밍 제거) ===
                try:
                    data = sock.recv(BUF_SIZE)
                except OSError:
                    return
                if not data:
                    return

                sock_buf += data

                # Content-Length 프레이밍 파싱
                while sock_buf:
                    # 헤더 찾기
                    header_end = sock_buf.find(b'\r\n\r\n')
                    if header_end < 0:
                        break

                    header_section = sock_buf[:header_end]
                    content_length = None
                    for hdr_line in header_section.split(b'\r\n'):
                        if hdr_line.lower().startswith(b'content-length:'):
                            try:
                                content_length = int(hdr_line.split(b':',1)[1].strip())
                            except ValueError:
                                pass

                    if content_length is None:
                        break

                    body_start = header_end + 4  # \r\n\r\n 이후
                    body_end = body_start + content_length

                    if len(sock_buf) < body_end:
                        break  # 아직 바디 전체가 안 왔음

                    body = sock_buf[body_start:body_end]
                    sock_buf = sock_buf[body_end:]

                    # stdout에 raw JSON + 줄바꿈으로 출력
                    try:
                        os.write(stdout_fd, body + b'\n')
                    except OSError:
                        return

        # select 타임아웃 시 stdin_buf에 줄바꿈 없이 남은 데이터 처리
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
