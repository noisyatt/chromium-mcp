#!/usr/bin/env python3
"""
chromium-mcp-client.py
stdio ↔ Chromium MCP 소켓 브릿지 (프레이밍 변환 포함)

Claude CLI (raw JSON, 줄바꿈 구분) ↔ Chromium MCP (Content-Length 프레이밍)

== 환경변수 ==
로컬 모드 (기본):
  CHROMIUM_MCP_SOCKET  — 커스텀 소켓 경로 (기본: /tmp/.chromium-mcp.sock)

원격 모드:
  CHROMIUM_MCP_HOST    — 원격 호스트 IP/hostname
  CHROMIUM_MCP_USER    — SSH 유저 (기본: root)
  CHROMIUM_MCP_SSH_KEY — SSH 키 경로 (기본: ~/.ssh/proxmox)
  CHROMIUM_MCP_SSH_JUMP — SSH 점프호스트 (예: proxmox)
  CHROMIUM_MCP_REMOTE_SOCKET — 원격 소켓 경로 (기본: /tmp/.chromium-mcp.sock)
"""
import os
import select
import signal
import socket
import subprocess
import sys
import time

CHROMIUM_SOCKET = os.environ.get('CHROMIUM_MCP_SOCKET', '/tmp/.chromium-mcp.sock')
PROXY_SOCKET = '/tmp/.chromium-mcp-proxy.sock'
DAEMON_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chromium-mcp-daemon.py')
BUF_SIZE = 65536
CONNECT_TIMEOUT = 30
SSH_RECONNECT_DELAY = 3
SSH_MAX_RETRIES = 5

_running = True


def _handle_signal(signum, frame):
    global _running
    _running = False


def _is_remote_mode() -> bool:
    return bool(os.environ.get('CHROMIUM_MCP_HOST'))


def connect_remote_ssh():
    """paramiko로 SSH 연결 후 원격 Unix socket 채널 반환."""
    try:
        import paramiko
    except ImportError:
        sys.stderr.write("[chromium-mcp-client] paramiko 미설치. pip install paramiko\n")
        sys.exit(1)

    host = os.environ['CHROMIUM_MCP_HOST']
    user = os.environ.get('CHROMIUM_MCP_USER', 'root')
    key_path = os.path.expanduser(os.environ.get('CHROMIUM_MCP_SSH_KEY', '~/.ssh/proxmox'))
    jump_host = os.environ.get('CHROMIUM_MCP_SSH_JUMP', '')
    remote_socket = os.environ.get('CHROMIUM_MCP_REMOTE_SOCKET', '/tmp/.chromium-mcp.sock')

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    # 점프호스트 경유
    proxy_channel = None
    if jump_host:
        jump_ssh = paramiko.SSHClient()
        jump_ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        # SSH config에서 점프호스트 정보 읽기
        ssh_config = paramiko.SSHConfig()
        config_path = os.path.expanduser('~/.ssh/config')
        if os.path.exists(config_path):
            with open(config_path) as f:
                ssh_config.parse(f)
        jump_cfg = ssh_config.lookup(jump_host)
        jump_hostname = jump_cfg.get('hostname', jump_host)
        jump_user = jump_cfg.get('user', 'root')
        jump_key = jump_cfg.get('identityfile', [key_path])
        if isinstance(jump_key, list):
            jump_key = os.path.expanduser(jump_key[0])

        jump_ssh.connect(jump_hostname, username=jump_user, key_filename=jump_key, timeout=10)
        transport = jump_ssh.get_transport()
        proxy_channel = transport.open_channel(
            'direct-tcpip', (host, 22), ('127.0.0.1', 0)
        )
        ssh.connect(host, username=user, key_filename=key_path, sock=proxy_channel, timeout=10)
    else:
        ssh.connect(host, username=user, key_filename=key_path, timeout=10)

    # 원격 Unix socket 접근: socat으로 소켓을 stdin/stdout에 연결
    channel = ssh.get_transport().open_session()
    channel.exec_command(f'socat STDIO UNIX-CONNECT:{remote_socket}')
    return ssh, channel


def connect_remote_with_retry():
    """SSH 연결 + 재시도 로직."""
    for attempt in range(1, SSH_MAX_RETRIES + 1):
        try:
            ssh, channel = connect_remote_ssh()
            sys.stderr.write(f"[chromium-mcp-client] 원격 연결 성공: {os.environ['CHROMIUM_MCP_HOST']}\n")
            return ssh, channel
        except Exception as e:
            sys.stderr.write(f"[chromium-mcp-client] SSH 연결 실패 ({attempt}/{SSH_MAX_RETRIES}): {e}\n")
            if attempt < SSH_MAX_RETRIES:
                time.sleep(SSH_RECONNECT_DELAY)
    sys.stderr.write("[chromium-mcp-client] SSH 연결 최대 재시도 초과\n")
    sys.exit(1)


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


def _send_to_channel(channel, data: bytes):
    """paramiko 채널 또는 소켓에 데이터 전송."""
    if hasattr(channel, 'sendall'):
        channel.sendall(data)
    else:
        # paramiko Channel은 sendall이 없을 수 있음
        while data:
            sent = channel.send(data)
            if sent <= 0:
                raise OSError("채널 전송 실패")
            data = data[sent:]


def _recv_from_channel(channel, size: int) -> bytes:
    """paramiko 채널 또는 소켓에서 데이터 수신."""
    if hasattr(channel, 'recv'):
        return channel.recv(size)
    return b''


def proxy_loop(sock_or_channel) -> None:
    """
    stdin(raw JSON) ↔ socket/channel(Content-Length framed) 양방향 변환 프록시.

    stdin → socket: raw JSON에 Content-Length 헤더를 붙여서 전송
    socket → stdout: Content-Length 프레이밍을 파싱하고 바디만 stdout에 출력

    sock_or_channel: socket.socket (로컬) 또는 paramiko.Channel (원격)
    """
    stdin_fd = sys.stdin.buffer.fileno()
    stdout_fd = sys.stdout.buffer.fileno()

    ch = sock_or_channel

    # stdin 쪽 미처리 버퍼 (줄 단위 파싱용)
    stdin_buf = b''
    # socket 쪽 미처리 버퍼 (Content-Length 파싱용)
    sock_buf = b''

    while _running:
        try:
            readable, _, exceptional = select.select(
                [stdin_fd, ch], [], [stdin_fd, ch], 1.0
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
                        _send_to_channel(ch, framed)
                    except OSError:
                        return

                # 줄바꿈 없이 들어온 데이터도 처리 (단일 JSON)
                if stdin_buf and stdin_buf.strip():
                    # 아직 줄바꿈이 안 왔을 수 있으므로 대기
                    # 단, select 타임아웃 후에도 남아있으면 처리
                    pass

            elif fd == ch:
                # === socket → stdout (Content-Length 프레이밍 제거) ===
                try:
                    data = _recv_from_channel(ch, BUF_SIZE)
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
                _send_to_channel(ch, framed)
            except OSError:
                return


def main() -> None:
    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    if _is_remote_mode():
        ssh, channel = connect_remote_with_retry()
        try:
            proxy_loop(channel)
        finally:
            try:
                channel.close()
                ssh.close()
            except Exception:
                pass
    else:
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
