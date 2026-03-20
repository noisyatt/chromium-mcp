#!/usr/bin/env python3
"""
chromium-mcp-client.py
stdio ↔ 데몬 프록시 소켓 브릿지

.mcp.json에서 command로 사용.
데몬이 없으면 자동 기동.
"""
import socket
import subprocess
import sys
import os
import time
import threading

PROXY_SOCKET = '/tmp/.chromium-mcp-proxy.sock'
DAEMON_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'chromium-mcp-daemon.py')
DAEMON_WAIT   = 20  # 초


def is_daemon_ready() -> bool:
    """proxy 소켓에 연결 가능한지 확인"""
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
    """데몬 실행 후 소켓 대기"""
    sys.stderr.write('[client] 데몬 시작\n')
    subprocess.Popen(
        [sys.executable, DAEMON_SCRIPT],
        stdout=subprocess.DEVNULL,
        stderr=open('/tmp/chromium-mcp-daemon.log', 'a'),
        start_new_session=True  # 클로드에서 분리
    )
    deadline = time.time() + DAEMON_WAIT
    while time.time() < deadline:
        if is_daemon_ready():
            sys.stderr.write('[client] 데몬 준비 완료\n')
            return True
        time.sleep(0.5)
    sys.stderr.write('[client] 데몬 시작 타임아웃\n')
    return False


def connect_to_daemon() -> socket.socket:
    """데몬에 연결. 필요하면 데몬 실행."""
    if not is_daemon_ready():
        if not start_daemon():
            sys.stderr.write('[client] 데몬 연결 실패\n')
            sys.exit(1)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(PROXY_SOCKET)
    return sock


def stdin_to_socket(sock: socket.socket) -> None:
    """stdin(binary) → socket"""
    try:
        while True:
            data = sys.stdin.buffer.read1(65536)  # 블로킹 read
            if not data:
                break
            sock.sendall(data)
    except Exception:
        pass
    finally:
        try: sock.shutdown(socket.SHUT_WR)
        except Exception: pass


def socket_to_stdout(sock: socket.socket) -> None:
    """socket → stdout(binary)"""
    try:
        while True:
            data = sock.recv(65536)
            if not data:
                break
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
    except Exception:
        pass


def main() -> None:
    sock = connect_to_daemon()
    t1 = threading.Thread(target=stdin_to_socket,  args=(sock,), daemon=True)
    t2 = threading.Thread(target=socket_to_stdout, args=(sock,), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()


if __name__ == '__main__':
    main()
