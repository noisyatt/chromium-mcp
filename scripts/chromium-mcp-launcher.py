#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
chromium-mcp-launcher.py — Chromium-MCP MCP 프록시 런처

MCP 클라이언트(Claude Code 등)와 Chromium-MCP 브라우저 인스턴스 사이에서
중간 프록시 역할을 수행하는 스크립트입니다.

동작 흐름:
  1. ~/.chromium-mcp/instance.lock 파일을 확인
  2. 기존 인스턴스가 살아있으면 → 해당 Unix socket 에 연결하여 stdin↔socket 프록시
  3. 기존 인스턴스가 없으면 → Chromium-MCP 를 --mcp-socket 모드로 자동 실행,
     소켓이 준비될 때까지 대기한 뒤 연결
  4. SIGTERM/SIGINT 수신 시: 프록시만 종료, 브라우저 프로세스는 유지

lock 파일 형식 (~/.chromium-mcp/instance.lock):
  {
    "pid": 12345,
    "socket": "/tmp/.chromium-mcp.sock",
    "started_at": "2024-01-01T00:00:00.000000"
  }

환경변수:
  CHROMIUM_MCP_BROWSER_PATH  — Chromium-MCP 바이너리 경로
  CHROMIUM_MCP_SOCKET_PATH   — Unix socket 경로

Claude Code 설정 예시 (~/.claude.json 또는 프로젝트 .claude.json):
  {
    "mcpServers": {
      "chromium-mcp": {
        "command": "python3",
        "args": ["/path/to/chromium-mcp-launcher.py"]
      }
    }
  }
"""

import argparse
import datetime
import errno
import json
import os
import select
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# 상수 정의
# ---------------------------------------------------------------------------

# lock 파일 기본 디렉토리 및 경로
LOCK_DIR = Path.home() / ".chromium-mcp"
LOCK_FILE = LOCK_DIR / "instance.lock"

# 설정 파일 기본 경로
CONFIG_FILE = LOCK_DIR / "config.json"

# Unix socket 기본 경로
DEFAULT_SOCKET_PATH = "/tmp/.chromium-mcp.sock"

# 브라우저 시작 대기 기본 최대 시간 (초)
DEFAULT_LAUNCH_TIMEOUT = 30

# 소켓 연결 가능 여부 폴링 간격 (초)
SOCKET_POLL_INTERVAL = 0.5

# stdin/socket 프록시 버퍼 크기 (바이트)
PROXY_BUFFER_SIZE = 65536

# 프록시 select() 타임아웃 (초) — 종료 신호 감지 주기
SELECT_TIMEOUT = 1.0

# Chromium-MCP 바이너리 기본 탐색 경로 목록 (macOS / Linux)
DEFAULT_BROWSER_PATHS = [
    "/Applications/Chromium-MCP.app/Contents/MacOS/Chromium",
    "/Applications/Chromium.app/Contents/MacOS/Chromium",
    "/usr/local/bin/chromium-mcp",
    "/usr/bin/chromium-mcp",
    "/usr/local/bin/chromium",
    "/usr/bin/chromium",
]


# ---------------------------------------------------------------------------
# 로깅 유틸리티 (stderr 전용 — stdout 은 MCP JSON-RPC 전용)
# ---------------------------------------------------------------------------

def _log(msg: str) -> None:
    """표준 오류(stderr)에 진단 메시지를 출력한다."""
    print(f"[chromium-mcp-launcher] {msg}", file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# 설정 클래스
# ---------------------------------------------------------------------------

class LauncherConfig:
    """런처 동작 설정을 담는 클래스."""

    def __init__(self):
        # Chromium-MCP 바이너리 경로 (빈 문자열이면 자동 탐색)
        self.browser_path: str = ""
        # 사용할 Unix socket 경로
        self.socket_path: str = DEFAULT_SOCKET_PATH
        # 기존 인스턴스가 없을 때 자동 실행 여부
        self.auto_launch: bool = True
        # 브라우저 시작 대기 최대 시간 (초)
        self.launch_timeout: int = DEFAULT_LAUNCH_TIMEOUT
        # 브라우저에 추가로 전달할 인수
        self.extra_args: list = []

    @classmethod
    def from_file(cls, config_path: Path) -> "LauncherConfig":
        """JSON 설정 파일에서 설정을 로드한다. 파일이 없으면 기본값을 반환한다."""
        cfg = cls()
        if not config_path.exists():
            return cfg
        try:
            with open(config_path, "r", encoding="utf-8") as f:
                data = json.load(f)
            cfg.browser_path   = data.get("browser_path",   cfg.browser_path)
            cfg.socket_path    = data.get("socket_path",    cfg.socket_path)
            cfg.auto_launch    = data.get("auto_launch",    cfg.auto_launch)
            cfg.launch_timeout = data.get("launch_timeout", cfg.launch_timeout)
            cfg.extra_args     = data.get("extra_args",     cfg.extra_args)
        except (json.JSONDecodeError, OSError) as exc:
            _log(f"설정 파일 로드 실패 ({config_path}): {exc}")
        return cfg


# ---------------------------------------------------------------------------
# lock 파일 관련 함수
# ---------------------------------------------------------------------------

def _ensure_lock_dir() -> None:
    """lock 파일 디렉토리가 없으면 생성한다."""
    LOCK_DIR.mkdir(parents=True, exist_ok=True)


def read_lock_file() -> dict | None:
    """
    lock 파일을 읽어 딕셔너리로 반환한다.
    파일이 없거나 파싱 오류 시 None 을 반환한다.
    """
    if not LOCK_FILE.exists():
        return None
    try:
        with open(LOCK_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    except (json.JSONDecodeError, OSError):
        return None


def write_lock_file(pid: int, socket_path: str) -> None:
    """
    lock 파일을 작성한다.
    형식: {"pid": N, "socket": "...", "started_at": "ISO시간"}
    """
    _ensure_lock_dir()
    lock_data = {
        "pid": pid,
        "socket": socket_path,
        "started_at": datetime.datetime.now().isoformat(),
    }
    try:
        with open(LOCK_FILE, "w", encoding="utf-8") as f:
            json.dump(lock_data, f, ensure_ascii=False, indent=2)
    except OSError as exc:
        _log(f"lock 파일 쓰기 실패: {exc}")


def remove_lock_file() -> None:
    """lock 파일을 삭제한다. 파일이 없어도 오류를 무시한다."""
    try:
        LOCK_FILE.unlink(missing_ok=True)
    except OSError:
        pass


# ---------------------------------------------------------------------------
# 프로세스 / 소켓 상태 확인
# ---------------------------------------------------------------------------

def is_process_alive(pid: int) -> bool:
    """
    해당 PID 의 프로세스가 살아있는지 확인한다.
    kill(pid, 0) 으로 시그널을 보내지 않고 존재 여부만 검사한다.
    """
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError as exc:
        # EPERM: 프로세스는 존재하나 권한 없음 → 살아있음으로 간주
        if exc.errno == errno.EPERM:
            return True
        # ESRCH: 프로세스 없음
        return False


def is_socket_connectable(socket_path: str) -> bool:
    """
    지정한 Unix domain socket 에 실제로 연결할 수 있는지 테스트한다.
    연결 성공 시 즉시 소켓을 닫아 서버에 영향을 주지 않는다.
    """
    if not socket_path:
        return False
    probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    probe.settimeout(0.5)
    try:
        probe.connect(socket_path)
        return True
    except (socket.error, OSError):
        return False
    finally:
        probe.close()


# ---------------------------------------------------------------------------
# 기존 인스턴스 탐색
# ---------------------------------------------------------------------------

def find_running_instance() -> str | None:
    """
    lock 파일을 읽어 유효한 기존 인스턴스를 탐색한다.
    살아있는 인스턴스의 소켓 경로를 반환하거나, 없으면 None 을 반환한다.
    """
    lock_data = read_lock_file()
    if lock_data is None:
        _log("lock 파일 없음 — 기존 인스턴스 없음")
        return None

    pid         = lock_data.get("pid")
    socket_path = lock_data.get("socket", "")
    started_at  = lock_data.get("started_at", "(알 수 없음)")

    if not isinstance(pid, int) or not socket_path:
        _log("lock 파일 형식 오류 — 무시함")
        remove_lock_file()
        return None

    # PID 생존 확인
    if not is_process_alive(pid):
        _log(f"기존 인스턴스 PID {pid} 는 이미 종료됨 (시작: {started_at})")
        remove_lock_file()
        return None

    # 소켓 연결 가능 여부 확인
    if not is_socket_connectable(socket_path):
        _log(f"PID {pid} 는 살아있으나 소켓 {socket_path} 에 연결 불가")
        _log("브라우저가 MCP 서버를 아직 시작하지 않았거나 소켓 경로가 변경되었을 수 있음")
        return None

    _log(f"기존 인스턴스 발견 — PID {pid}, 소켓: {socket_path}, 시작: {started_at}")
    return socket_path


# ---------------------------------------------------------------------------
# 브라우저 경로 탐색
# ---------------------------------------------------------------------------

def find_browser_path(config: LauncherConfig) -> str | None:
    """
    Chromium-MCP 바이너리 경로를 결정한다.
    우선순위: 환경변수 → 인수/설정 파일 → 기본 경로 목록 순으로 탐색한다.
    """
    # 1순위: 환경변수
    env_path = os.environ.get("CHROMIUM_MCP_BROWSER_PATH", "").strip()
    if env_path:
        if os.path.isfile(env_path) and os.access(env_path, os.X_OK):
            _log(f"브라우저 경로 (환경변수): {env_path}")
            return env_path
        _log(f"환경변수 CHROMIUM_MCP_BROWSER_PATH 경로 접근 불가: {env_path}")

    # 2순위: 설정/인수에서 지정한 경로
    if config.browser_path:
        if os.path.isfile(config.browser_path) and os.access(config.browser_path, os.X_OK):
            _log(f"브라우저 경로 (설정): {config.browser_path}")
            return config.browser_path
        _log(f"지정된 브라우저 경로 접근 불가: {config.browser_path}")

    # 3순위: 기본 경로 목록 순차 탐색
    for candidate in DEFAULT_BROWSER_PATHS:
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            _log(f"브라우저 발견 (기본 경로): {candidate}")
            return candidate

    return None


# ---------------------------------------------------------------------------
# 브라우저 자동 실행
# ---------------------------------------------------------------------------

def launch_browser(config: LauncherConfig, socket_path: str) -> subprocess.Popen | None:
    """
    Chromium-MCP 를 --mcp-socket 모드로 실행하고 Popen 객체를 반환한다.
    실패 시 None 을 반환한다.
    """
    browser_path = find_browser_path(config)
    if browser_path is None:
        _log("Chromium-MCP 바이너리를 찾을 수 없음")
        _log("다음 중 하나를 설정하세요:")
        _log("  - 환경변수: CHROMIUM_MCP_BROWSER_PATH")
        _log("  - 설정 파일: ~/.chromium-mcp/config.json 의 browser_path")
        _log("  - 옵션: --browser-path")
        return None

    # 실행 명령 구성
    cmd = [
        browser_path,
        f"--mcp-socket={socket_path}",
        "--no-first-run",          # 첫 실행 마법사 건너뜀
        "--disable-default-apps",  # 기본 앱 비활성화
    ]
    # 설정 파일에 지정된 추가 인수 병합
    cmd.extend(config.extra_args)

    _log(f"브라우저 실행 명령: {' '.join(cmd)}")

    try:
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            # 새 세션으로 분리: 프록시가 종료되어도 브라우저는 계속 실행
            start_new_session=True,
        )
        _log(f"브라우저 시작됨 — PID {proc.pid}")

        # lock 파일 작성 (브라우저가 스스로 쓰지 않는 경우를 대비)
        write_lock_file(proc.pid, socket_path)

        return proc
    except OSError as exc:
        _log(f"브라우저 실행 실패: {exc}")
        return None


def wait_for_socket(socket_path: str, timeout: int) -> bool:
    """
    소켓이 생성되고 연결 가능해질 때까지 폴링으로 대기한다.
    timeout 초 내에 연결 가능하면 True, 시간 초과 시 False 를 반환한다.
    """
    _log(f"소켓 준비 대기: {socket_path} (최대 {timeout}초)")
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        if is_socket_connectable(socket_path):
            _log("소켓 준비 완료")
            return True
        time.sleep(SOCKET_POLL_INTERVAL)

    _log(f"소켓 대기 타임아웃 ({timeout}초 초과)")
    return False


# ---------------------------------------------------------------------------
# 소켓 경로 결정
# ---------------------------------------------------------------------------

def determine_socket_path(config: LauncherConfig) -> str:
    """
    사용할 Unix socket 경로를 결정한다.
    우선순위: 환경변수 → 설정 파일/인수 → 기본값(/tmp/.chromium-mcp.sock)
    """
    # 1순위: 환경변수
    env_socket = os.environ.get("CHROMIUM_MCP_SOCKET_PATH", "").strip()
    if env_socket:
        return env_socket

    # 2순위: 설정 파일 또는 --socket-path 인수
    if config.socket_path:
        return config.socket_path

    # 3순위: 하드코딩 기본값
    return DEFAULT_SOCKET_PATH


# ---------------------------------------------------------------------------
# stdin ↔ socket 양방향 프록시 (select 기반, 단일 스레드)
# ---------------------------------------------------------------------------

class StdioSocketProxy:
    """
    표준 입력(stdin) 과 Unix domain socket 사이에서 데이터를 양방향 중계하는
    select 기반 단일 스레드 프록시 클래스.

    MCP 프로토콜은 JSON-RPC over stdio 이므로
    stdout 이 오염되지 않도록 모든 진단 출력은 stderr 로만 내보낸다.
    """

    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._running = False

    def run(self) -> None:
        """
        select() 루프를 돌며 stdin → socket, socket → stdout 방향으로
        데이터를 중계한다. 어느 한쪽이 닫히면 루프를 종료한다.
        """
        self._running = True

        # stdin 의 원시(binary) 파일 디스크립터
        stdin_fd  = sys.stdin.buffer.fileno()
        stdout_buf = sys.stdout.buffer

        _log("프록시 루프 시작 (select 기반)")

        while self._running:
            try:
                # stdin 과 socket 양쪽에서 읽을 데이터 대기
                readable, _, exceptional = select.select(
                    [stdin_fd, self._sock],   # 읽기 감시 대상
                    [],                        # 쓰기 감시 없음
                    [stdin_fd, self._sock],    # 오류 감시
                    SELECT_TIMEOUT,
                )
            except (OSError, ValueError):
                # fd 가 닫혔거나 유효하지 않을 때 발생
                break

            # 오류 조건 감지
            if exceptional:
                _log("select 오류 조건 감지 — 프록시 종료")
                break

            for fd in readable:
                if fd is self._sock:
                    # socket → stdout
                    try:
                        data = self._sock.recv(PROXY_BUFFER_SIZE)
                    except OSError as exc:
                        _log(f"소켓 수신 오류: {exc}")
                        self._running = False
                        break
                    if not data:
                        # 브라우저 측에서 연결 종료
                        _log("소켓 EOF — 브라우저가 연결을 닫음")
                        self._running = False
                        break
                    try:
                        stdout_buf.write(data)
                        stdout_buf.flush()
                    except (OSError, BrokenPipeError) as exc:
                        _log(f"stdout 쓰기 오류: {exc}")
                        self._running = False
                        break

                elif fd == stdin_fd:
                    # stdin → socket
                    try:
                        data = sys.stdin.buffer.read1(PROXY_BUFFER_SIZE)
                    except OSError as exc:
                        _log(f"stdin 읽기 오류: {exc}")
                        self._running = False
                        break
                    if not data:
                        # MCP 클라이언트가 stdin 을 닫음
                        _log("stdin EOF — MCP 클라이언트가 연결을 닫음")
                        self._running = False
                        break
                    try:
                        self._sock.sendall(data)
                    except (OSError, BrokenPipeError) as exc:
                        _log(f"소켓 송신 오류: {exc}")
                        self._running = False
                        break

        _log("프록시 루프 종료")

    def stop(self) -> None:
        """외부에서 프록시를 중단시킨다 (시그널 핸들러 등에서 호출)."""
        self._running = False
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# 전역 프록시 참조 (시그널 핸들러에서 접근)
# ---------------------------------------------------------------------------

_proxy_instance: StdioSocketProxy | None = None


def _handle_signal(signum: int, frame) -> None:
    """
    SIGTERM 또는 SIGINT 수신 시 프록시만 정상 종료한다.
    브라우저 프로세스는 독립 세션으로 실행 중이므로 영향을 받지 않는다.
    """
    sig_name = signal.Signals(signum).name
    _log(f"시그널 {sig_name} 수신 — 프록시 종료 중 (브라우저는 계속 실행됨)")
    if _proxy_instance is not None:
        _proxy_instance.stop()


# ---------------------------------------------------------------------------
# 진입점
# ---------------------------------------------------------------------------

def main() -> int:
    global _proxy_instance

    # ------------------------------------------------------------------
    # 커맨드라인 인수 파싱
    # ------------------------------------------------------------------
    parser = argparse.ArgumentParser(
        prog="chromium-mcp-launcher",
        description="Chromium-MCP MCP 프록시 런처",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "환경변수:\n"
            "  CHROMIUM_MCP_BROWSER_PATH  Chromium-MCP 바이너리 경로\n"
            "  CHROMIUM_MCP_SOCKET_PATH   Unix socket 경로\n"
        ),
    )
    parser.add_argument(
        "--browser-path",
        default="",
        metavar="PATH",
        help=(
            "Chromium-MCP 바이너리 경로 "
            "(환경변수 CHROMIUM_MCP_BROWSER_PATH 로도 지정 가능)"
        ),
    )
    parser.add_argument(
        "--socket-path",
        default="",
        metavar="PATH",
        help=(
            f"Unix socket 경로 (기본: {DEFAULT_SOCKET_PATH}) "
            "(환경변수 CHROMIUM_MCP_SOCKET_PATH 로도 지정 가능)"
        ),
    )
    parser.add_argument(
        "--launch-timeout",
        type=int,
        default=0,
        metavar="N",
        help=f"브라우저 시작 대기 최대 시간 (초, 기본: {DEFAULT_LAUNCH_TIMEOUT})",
    )
    parser.add_argument(
        "--no-auto-launch",
        action="store_true",
        default=False,
        help="기존 인스턴스가 없을 때 자동 실행을 비활성화한다",
    )
    parser.add_argument(
        "--config",
        default=str(CONFIG_FILE),
        metavar="FILE",
        help=f"JSON 설정 파일 경로 (기본: {CONFIG_FILE})",
    )

    args = parser.parse_args()

    # ------------------------------------------------------------------
    # 설정 파일 로드 후 커맨드라인 인수로 오버라이드
    # ------------------------------------------------------------------
    config = LauncherConfig.from_file(Path(args.config))

    if args.browser_path:
        config.browser_path = args.browser_path

    if args.socket_path:
        config.socket_path = args.socket_path

    if args.launch_timeout > 0:
        config.launch_timeout = args.launch_timeout

    if args.no_auto_launch:
        config.auto_launch = False

    # ------------------------------------------------------------------
    # 시그널 핸들러 등록
    # ------------------------------------------------------------------
    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    # ------------------------------------------------------------------
    # 1단계: 실행 중인 기존 인스턴스 확인
    # ------------------------------------------------------------------
    socket_path = find_running_instance()

    if socket_path is None:
        # ------------------------------------------------------------------
        # 2단계: 기존 인스턴스 없음
        # ------------------------------------------------------------------
        if not config.auto_launch:
            _log("--no-auto-launch 가 설정되어 있어 자동 실행하지 않음")
            _log("Chromium-MCP 를 먼저 실행한 뒤 다시 연결하세요")
            return 1

        # 소켓 경로 결정 (환경변수 > 설정 > 기본값)
        socket_path = determine_socket_path(config)
        _log(f"새 Chromium-MCP 인스턴스를 시작합니다 (소켓: {socket_path})")

        browser_proc = launch_browser(config, socket_path)
        if browser_proc is None:
            _log("브라우저 자동 실행에 실패했습니다")
            return 1

        # 소켓이 준비될 때까지 대기
        if not wait_for_socket(socket_path, config.launch_timeout):
            _log(f"브라우저 시작 타임아웃 ({config.launch_timeout}초)")
            _log("브라우저가 실행 중인지, 경로가 올바른지 확인하세요")
            return 1

    # ------------------------------------------------------------------
    # 3단계: Unix socket 에 연결
    # ------------------------------------------------------------------
    _log(f"소켓 연결 중: {socket_path}")
    client_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        client_sock.connect(socket_path)
    except OSError as exc:
        _log(f"소켓 연결 실패: {exc}")
        return 1

    _log("소켓 연결 성공 — MCP 프록시를 시작합니다")

    # ------------------------------------------------------------------
    # 4단계: stdin ↔ socket 양방향 프록시 실행 (블로킹)
    # ------------------------------------------------------------------
    proxy = StdioSocketProxy(client_sock)
    _proxy_instance = proxy
    proxy.run()

    # 정상 종료 처리
    try:
        client_sock.close()
    except OSError:
        pass

    _log("Chromium-MCP 프록시 정상 종료")
    return 0


if __name__ == "__main__":
    sys.exit(main())
