#!/usr/bin/env python3
"""
chromium-mcp-daemon.py
Chromium MCP 데몬 — 로그인 시 자동 시작, 항시 실행

역할:
  1. Chromium 실행 유지 (크래시 감지 → 자동 재시작)
  2. /tmp/.chromium-mcp-proxy.sock 에서 클라이언트 연결 수락
  3. 각 클라이언트를 Chromium MCP 소켓에 독립 연결로 브릿지
"""
import socket
import subprocess
import threading
import os
import sys
import time
import logging
import signal

CHROMIUM_PATH = os.environ.get(
    'CHROMIUM_MCP_BROWSER_PATH',
    '/Users/daniel/chromium/src/out/Default/Chromium.app/Contents/MacOS/Chromium'
)
CHROMIUM_SOCKET  = '/tmp/.chromium-mcp.sock'
PROXY_SOCKET     = '/tmp/.chromium-mcp-proxy.sock'
PID_FILE         = '/tmp/chromium-mcp-daemon.pid'
LOG_FILE         = '/tmp/chromium-mcp-daemon.log'
MONITOR_INTERVAL = 10   # 초
SOCKET_WAIT      = 20   # Chromium 소켓 대기 최대 시간(초)

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [daemon] %(levelname)s %(message)s',
    handlers=[
        logging.FileHandler(LOG_FILE),
        logging.StreamHandler(sys.stderr)
    ]
)
log = logging.getLogger('chromium-mcp-daemon')

_running = True


def is_chromium_running() -> bool:
    """Chromium 프로세스 실행 여부 확인"""
    try:
        r = subprocess.run(
            ['pgrep', '-f', 'Chromium.app/Contents/MacOS/Chromium'],
            capture_output=True
        )
        return r.returncode == 0
    except Exception:
        return False


def wait_for_chromium_socket(timeout: int = SOCKET_WAIT) -> bool:
    """Chromium MCP 소켓이 연결 가능할 때까지 대기"""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.exists(CHROMIUM_SOCKET):
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(1)
                s.connect(CHROMIUM_SOCKET)
                s.close()
                return True
            except Exception:
                pass
        time.sleep(0.5)
    return False


def launch_chromium() -> bool:
    """Chromium 실행 후 MCP 소켓 준비 대기"""
    log.info(f'Chromium 실행: {CHROMIUM_PATH}')
    try:
        subprocess.Popen(
            [CHROMIUM_PATH, '--no-first-run', '--disable-default-apps'],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
    except FileNotFoundError:
        log.error(f'Chromium 바이너리 없음: {CHROMIUM_PATH}')
        return False
    if wait_for_chromium_socket():
        log.info('Chromium MCP 소켓 준비 완료')
        return True
    log.error('Chromium MCP 소켓 대기 타임아웃')
    return False


def ensure_chromium() -> bool:
    """Chromium이 실행 중이고 소켓이 열려있는지 보장"""
    if wait_for_chromium_socket(timeout=2):
        return True
    if not is_chromium_running():
        return launch_chromium()
    # 프로세스는 있지만 소켓이 없음 → 소켓 대기
    return wait_for_chromium_socket()


def _bridge(src: socket.socket, dst: socket.socket) -> None:
    """src → dst 단방향 데이터 중계 (블로킹)"""
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except Exception:
        pass
    finally:
        try: src.close()
        except Exception: pass
        try: dst.close()
        except Exception: pass


def handle_client(client_sock: socket.socket) -> None:
    """클라이언트 연결 처리 — Chromium 소켓과 브릿지"""
    addr = id(client_sock)
    log.info(f'[{addr}] 클라이언트 연결')

    if not ensure_chromium():
        log.error(f'[{addr}] Chromium 시작 실패, 연결 거부')
        client_sock.close()
        return

    try:
        cr_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        cr_sock.connect(CHROMIUM_SOCKET)
    except Exception as e:
        log.error(f'[{addr}] Chromium 소켓 연결 실패: {e}')
        client_sock.close()
        return

    log.info(f'[{addr}] 브릿지 시작 client ↔ Chromium')
    t1 = threading.Thread(target=_bridge, args=(client_sock, cr_sock), daemon=True)
    t2 = threading.Thread(target=_bridge, args=(cr_sock, client_sock), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    log.info(f'[{addr}] 브릿지 종료')


def monitor_chromium() -> None:
    """Chromium 모니터링 스레드 — 크래시 감지 시 재시작"""
    while _running:
        time.sleep(MONITOR_INTERVAL)
        if not is_chromium_running() and not os.path.exists(CHROMIUM_SOCKET):
            log.warning('Chromium 종료 감지 — 재시작')
            ensure_chromium()


def write_pid() -> None:
    with open(PID_FILE, 'w') as f:
        f.write(str(os.getpid()))


def cleanup(signum=None, frame=None) -> None:
    global _running
    _running = False
    log.info('데몬 종료 중...')
    if os.path.exists(PROXY_SOCKET):
        os.unlink(PROXY_SOCKET)
    if os.path.exists(PID_FILE):
        os.unlink(PID_FILE)
    sys.exit(0)


def main() -> None:
    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGINT, cleanup)

    write_pid()
    log.info(f'=== Chromium MCP 데몬 시작 (PID={os.getpid()}) ===')

    # 시작 시 Chromium 사전 실행
    ensure_chromium()

    # 기존 프록시 소켓 제거
    if os.path.exists(PROXY_SOCKET):
        os.unlink(PROXY_SOCKET)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(PROXY_SOCKET)
    os.chmod(PROXY_SOCKET, 0o600)
    server.listen(20)
    log.info(f'프록시 소켓 대기: {PROXY_SOCKET}')

    # Chromium 모니터링 스레드 시작
    threading.Thread(target=monitor_chromium, daemon=True).start()

    try:
        while _running:
            try:
                server.settimeout(1.0)
                client_sock, _ = server.accept()
            except socket.timeout:
                continue
            except Exception as e:
                if _running:
                    log.error(f'accept 오류: {e}')
                break
            threading.Thread(
                target=handle_client,
                args=(client_sock,),
                daemon=True
            ).start()
    finally:
        server.close()
        cleanup()


if __name__ == '__main__':
    main()
