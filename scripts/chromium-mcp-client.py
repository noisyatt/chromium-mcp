#!/usr/bin/env python3
"""
chromium-mcp-client.py
stdio ↔ 데몬 프록시 소켓 브릿지

.mcp.json에서 command로 사용.
데몬이 없으면 자동 기동.
"""
import os
import select
import signal
import socket
import subprocess
import sys
import time

PROXY_SOCKET = '/tmp/.chromium-mcp-proxy.sock'
DAEMON_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chromium-mcp-daemon.py')
DAEMON_WAIT = 20  # 초
BUF_SIZE = 65536

_running = True


def _handle_signal(signum, frame):
    global _running
    _running = False


def is_daemon_ready() -> bool:
    if not os.path.exists(PROXY_SOCKET):
        return False
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(1)
        s.connect(PROXY_SOCKET)
        s.close()
        return True
    except Exception:
        return False


def start_daemon() -> bool:
    sys.stderr.write('[client] 데몬 시작\n')
    subprocess.Popen(
        [sys.executable, DAEMON_SCRIPT],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )
    deadline = time.time() + DAEMON_WAIT
    while time.time() < deadline:
        if is_daemon_ready():
            sys.stderr.write('[client] 데몬 준비 완료\n')
            return True
        time.sleep(0.5)
    sys.stderr.write('[client] 데몬 시작 타임아웃\n')
    return False


def connect_to_daemon() -> socket.socket:
    if not is_daemon_ready():
        if not start_daemon():
            sys.stderr.write('[client] 데몬 연결 실패\n')
            sys.exit(1)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(PROXY_SOCKET)
    return sock


def proxy_loop(sock: socket.socket) -> None:
    """select 기반 stdin ↔ socket 양방향 프록시"""
    stdin_fd = sys.stdin.buffer.fileno()
    stdout_fd = sys.stdout.buffer.fileno()

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
                try:
                    sock.sendall(data)
                except OSError:
                    return

            elif fd == sock:
                try:
                    data = sock.recv(BUF_SIZE)
                except OSError:
                    return
                if not data:
                    return
                try:
                    os.write(stdout_fd, data)
                except OSError:
                    return


def main() -> None:
    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    sock = connect_to_daemon()
    try:
        proxy_loop(sock)
    finally:
        try:
            sock.close()
        except Exception:
            pass


if __name__ == '__main__':
    main()
