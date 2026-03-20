#!/usr/bin/env python3
"""
chromium-mcp-daemon.py
Chromium MCP 데몬 — 로그인 시 자동 시작, 항시 실행

역할:
  1. Chromium 실행 유지 (크래시 감지 → 자동 재시작)
  2. /tmp/.chromium-mcp-proxy.sock 에서 클라이언트 연결 수락
  3. 각 클라이언트를 Chromium MCP 소켓에 독립 연결로 브릿지
"""
import socket
import subprocess
import threading
import os
import sys
import time
import logging
import signal
from enum import Enum, auto

CHROMIUM_PATH    = os.environ.get(
    'CHROMIUM_MCP_BROWSER_PATH',
    '/Users/daniel/chromium/src/out/Default/Chromium.app/Contents/MacOS/Chromium'
)
CHROMIUM_SOCKET  = '/tmp/.chromium-mcp.sock'
PROXY_SOCKET     = '/tmp/.chromium-mcp-proxy.sock'
PID_FILE         = '/tmp/chromium-mcp-daemon.pid'
LOG_FILE         = '/tmp/chromium-mcp-daemon.log'
MONITOR_INTERVAL = 10   # 초 — 프로세스 폴링 주기
SOCKET_WAIT      = 20   # 초 — Chromium 소켓 대기 최대 시간
MAX_RETRY        = 3    # 시작 재시도 횟수
RETRY_DELAY      = 2    # 재시도 간격(초)
GRACE_PERIOD     = 5    # 소켓 끊김 → STOPPED 전이 유예(초)
COOLDOWN         = 30   # 연속 크래시 후 재시작 대기(초)

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


# ---------------------------------------------------------------------------
# ChromiumManager — Actor / State Machine
# ---------------------------------------------------------------------------

class State(Enum):
    STOPPED  = auto()
    STARTING = auto()
    READY    = auto()
    STOPPING = auto()


class ChromiumManager:
    """Chromium 프로세스 생애주기를 단일 스레드-안전 상태머신으로 관리한다.

    상태 전이:
        STOPPED → (trigger_start) → STARTING → READY
        READY   → (crash)         → STOPPED  → (auto restart)
        *       → (shutdown)      → STOPPING
    """

    def __init__(self) -> None:
        self._lock      = threading.Lock()
        self._cond      = threading.Condition(self._lock)
        self._state     = State.STOPPED
        self._proc: subprocess.Popen | None = None

    # ------------------------------------------------------------------
    # 공개 API
    # ------------------------------------------------------------------

    def wait_ready(self, timeout: float = 30.0) -> bool:
        """READY 상태가 될 때까지 대기한다.

        - STOPPED → 자동으로 _trigger_start() 호출
        - STARTING → Condition.wait()로 블로킹
        - 반환값: True(READY 도달) / False(타임아웃 또는 STOPPING)
        """
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                if self._state == State.READY:
                    return True
                if self._state in (State.STOPPING,):
                    return False
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    log.error('wait_ready 타임아웃')
                    return False
                if self._state == State.STOPPED:
                    self._trigger_start()   # 락 보유 상태에서 호출 (내부에서 스레드 분기)
                self._cond.wait(timeout=min(remaining, 1.0))

    def shutdown(self) -> None:
        """데몬 종료 시 Chromium 프로세스를 정리한다."""
        with self._cond:
            if self._state == State.STOPPING:
                return
            log.info('ChromiumManager: STOPPING')
            self._state = State.STOPPING
            self._cond.notify_all()
            proc = self._proc
        if proc is not None:
            try:
                proc.terminate()
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            except Exception as e:
                log.warning(f'프로세스 종료 중 오류: {e}')

    # ------------------------------------------------------------------
    # 내부 전이 메서드 (모두 _lock 보유 상태에서 호출 가능)
    # ------------------------------------------------------------------

    def _trigger_start(self) -> None:
        """STOPPED 상태일 때만 시작 스레드를 분기한다 (이중 시작 방지)."""
        if self._state != State.STOPPED:
            return
        log.info('ChromiumManager: STOPPED → STARTING')
        self._state = State.STARTING
        self._cond.notify_all()
        threading.Thread(target=self._do_start, daemon=True).start()

    def _set_state(self, new_state: State) -> None:
        """락 보유 여부와 무관하게 안전하게 상태를 전이한다."""
        with self._cond:
            self._state = new_state
            self._cond.notify_all()

    # ------------------------------------------------------------------
    # 시작 스레드
    # ------------------------------------------------------------------

    def _do_start(self) -> None:
        """별도 스레드에서 실행. Popen은 이 메서드에서만 호출된다."""
        for attempt in range(1, MAX_RETRY + 1):
            log.info(f'Chromium 시작 시도 {attempt}/{MAX_RETRY}: {CHROMIUM_PATH}')
            try:
                proc = subprocess.Popen(
                    [CHROMIUM_PATH, '--no-first-run', '--disable-default-apps'],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL
                )
            except FileNotFoundError:
                log.error(f'Chromium 바이너리 없음: {CHROMIUM_PATH}')
                self._set_state(State.STOPPED)
                return
            except Exception as e:
                log.error(f'Popen 실패: {e}')
                if attempt < MAX_RETRY:
                    time.sleep(RETRY_DELAY)
                continue

            # 소켓 준비 대기
            if self._wait_socket(proc):
                with self._cond:
                    if self._state == State.STOPPING:
                        proc.terminate()
                        return
                    self._proc  = proc
                    self._state = State.READY
                    self._cond.notify_all()
                log.info('ChromiumManager: STARTING → READY')
                threading.Thread(target=self._monitor_loop, daemon=True).start()
                return

            # 소켓 타임아웃 — 프로세스 종료 후 재시도
            log.warning('소켓 대기 타임아웃, 프로세스 종료 후 재시도')
            try:
                proc.terminate()
                proc.wait(timeout=3)
            except Exception:
                pass
            if attempt < MAX_RETRY:
                time.sleep(RETRY_DELAY)

        # 모든 시도 실패 → STOPPED 복귀
        log.error('Chromium 시작 최종 실패 — STOPPED 복귀')
        self._set_state(State.STOPPED)

    def _wait_socket(self, proc: subprocess.Popen) -> bool:
        """소켓이 연결 가능해질 때까지 대기. 프로세스가 죽으면 즉시 False."""
        deadline = time.monotonic() + SOCKET_WAIT
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                log.warning(f'Chromium이 소켓 준비 전 종료 (exit={proc.returncode})')
                return False
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

    # ------------------------------------------------------------------
    # 모니터 루프 (READY 상태에서 실행)
    # ------------------------------------------------------------------

    def _monitor_loop(self) -> None:
        """Popen.poll()로 프로세스 상태를 주기적으로 확인한다."""
        while True:
            with self._cond:
                if self._state != State.READY:
                    return
                proc = self._proc

            time.sleep(MONITOR_INTERVAL)

            if proc is None:
                return

            exit_code = proc.poll()
            if exit_code is None:
                # 프로세스 살아있음 — 소켓도 확인
                if not self._socket_alive():
                    log.warning(f'소켓 끊김 감지 — {GRACE_PERIOD}초 유예 대기')
                    time.sleep(GRACE_PERIOD)
                    if not self._socket_alive():
                        log.warning('소켓 유예 후 복구 실패 → STOPPED 전이, 재시작')
                        with self._cond:
                            if self._state == State.READY:
                                self._state = State.STOPPED
                                self._proc  = None
                                self._cond.notify_all()
                                self._trigger_start()
                        return
                continue

            # 프로세스 종료됨
            if exit_code == 0:
                log.info('Chromium 정상 종료 (exit=0) — 재시작 안 함')
                with self._cond:
                    if self._state == State.READY:
                        self._state = State.STOPPED
                        self._proc  = None
                        self._cond.notify_all()
                return
            else:
                log.warning(f'Chromium 크래시 감지 (exit={exit_code}) — {COOLDOWN}초 후 재시작')
                with self._cond:
                    if self._state == State.READY:
                        self._state = State.STOPPED
                        self._proc  = None
                        self._cond.notify_all()
                time.sleep(COOLDOWN)
                with self._cond:
                    if self._state == State.STOPPED:
                        self._trigger_start()
                return

    def _socket_alive(self) -> bool:
        """Chromium MCP 소켓에 연결 가능한지 확인한다."""
        if not os.path.exists(CHROMIUM_SOCKET):
            return False
        try:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(1)
            s.connect(CHROMIUM_SOCKET)
            s.close()
            return True
        except Exception:
            return False


# ---------------------------------------------------------------------------
# 유틸리티
# ---------------------------------------------------------------------------

def _bridge(src: socket.socket, dst: socket.socket) -> None:
    """src → dst 단방향 데이터 중계 (블로킹)."""
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


def _connect_chromium_with_retry() -> socket.socket | None:
    """Chromium 소켓에 최대 MAX_RETRY회 연결을 시도한다."""
    for attempt in range(1, MAX_RETRY + 1):
        try:
            cr_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            cr_sock.connect(CHROMIUM_SOCKET)
            return cr_sock
        except Exception as e:
            log.warning(f'Chromium 소켓 연결 시도 {attempt}/{MAX_RETRY} 실패: {e}')
            if attempt < MAX_RETRY:
                time.sleep(RETRY_DELAY)
    return None


# ---------------------------------------------------------------------------
# 클라이언트 핸들러
# ---------------------------------------------------------------------------

def handle_client(client_sock: socket.socket, manager: ChromiumManager) -> None:
    """클라이언트 연결 처리 — Chromium 소켓과 브릿지."""
    addr = id(client_sock)
    log.info(f'[{addr}] 클라이언트 연결')

    if not manager.wait_ready(timeout=30):
        log.error(f'[{addr}] Chromium 준비 실패, 연결 거부')
        client_sock.close()
        return

    cr_sock = _connect_chromium_with_retry()
    if cr_sock is None:
        log.error(f'[{addr}] Chromium 소켓 연결 최종 실패')
        client_sock.close()
        return

    log.info(f'[{addr}] 브릿지 시작 client ↔ Chromium')
    t1 = threading.Thread(target=_bridge, args=(client_sock, cr_sock), daemon=True)
    t2 = threading.Thread(target=_bridge, args=(cr_sock, client_sock), daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    log.info(f'[{addr}] 브릿지 종료')


# ---------------------------------------------------------------------------
# 데몬 진입점
# ---------------------------------------------------------------------------

def write_pid() -> None:
    with open(PID_FILE, 'w') as f:
        f.write(str(os.getpid()))


def cleanup(signum=None, frame=None) -> None:
    global _running
    _running = False
    log.info('데몬 종료 중...')
    if os.path.exists(PROXY_SOCKET):
        os.unlink(PROXY_SOCKET)
    if os.path.exists(PID_FILE):
        os.unlink(PID_FILE)
    sys.exit(0)


def main() -> None:
    signal.signal(signal.SIGTERM, cleanup)
    signal.signal(signal.SIGINT, cleanup)

    write_pid()
    log.info(f'=== Chromium MCP 데몬 시작 (PID={os.getpid()}) ===')

    manager = ChromiumManager()

    # 기존 프록시 소켓 제거
    if os.path.exists(PROXY_SOCKET):
        os.unlink(PROXY_SOCKET)

    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(PROXY_SOCKET)
    os.chmod(PROXY_SOCKET, 0o600)
    server.listen(20)
    log.info(f'프록시 소켓 대기: {PROXY_SOCKET}')

    # 시작 시 Chromium 사전 워밍 (백그라운드)
    threading.Thread(target=manager.wait_ready, kwargs={'timeout': SOCKET_WAIT}, daemon=True).start()

    try:
        while _running:
            try:
                server.settimeout(1.0)
                client_sock, _ = server.accept()
            except socket.timeout:
                continue
            except Exception as e:
                if _running:
                    log.error(f'accept 오류: {e}')
                break
            threading.Thread(
                target=handle_client,
                args=(client_sock, manager),
                daemon=True
            ).start()
    finally:
        manager.shutdown()
        server.close()
        cleanup()


if __name__ == '__main__':
    main()
