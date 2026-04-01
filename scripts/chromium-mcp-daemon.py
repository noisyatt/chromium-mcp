#!/usr/bin/env python3
"""
chromium-mcp-daemon.py
Chromium MCP 풀 데몬

역할:
  1. /tmp/.chromium-mcp-proxy.sock(기본)에서 다중 클라이언트 연결 수락
  2. 인스턴스 풀에서 LRU 기반으로 백엔드 인스턴스 배정
  3. 클라이언트 ↔ 백엔드 소켓 바이트 단위 양방향 relay
  4. 유휴 인스턴스 주기 healthcheck

호환성:
  - ~/.chromium-mcp/pool.json 이 없으면 기존 단일 로컬 인스턴스 모드로 동작한다.
"""

from __future__ import annotations

import glob
import json
import logging
import os
import shlex
import signal
import socket
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from enum import Enum, auto

try:
    import paramiko  # type: ignore
except ImportError:  # pragma: no cover - 선택적 의존성
    paramiko = None

CHROMIUM_PATH = os.environ.get(
    'CHROMIUM_MCP_BROWSER_PATH',
    '/Users/daniel/chromium/src/out/Default/Chromium.app/Contents/MacOS/Chromium'
)
CHROMIUM_SOCKET = '/tmp/.chromium-mcp.sock'
DEFAULT_PROXY_SOCKET = '/tmp/.chromium-mcp-proxy.sock'
PID_FILE = '/tmp/chromium-mcp-daemon.pid'
LOG_FILE = '/tmp/chromium-mcp-daemon.log'
POOL_CONFIG_FILE = os.path.expanduser('~/.chromium-mcp/pool.json')

MONITOR_INTERVAL = 10
SOCKET_WAIT = 20
MAX_RETRY = 3
RETRY_DELAY = 2
GRACE_PERIOD = 5
COOLDOWN = 30
SSH_CONNECT_TIMEOUT = 10

GOOGLE_API_ENV = os.path.expanduser('~/.chromium-mcp/google-api.env')

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
_server_socket: socket.socket | None = None
_listen_socket_path = DEFAULT_PROXY_SOCKET


def _load_google_api_env() -> dict[str, str]:
    """~/.chromium-mcp/google-api.env 에서 KEY=VALUE 쌍을 읽어 dict로 반환."""
    result: dict[str, str] = {}
    if not os.path.isfile(GOOGLE_API_ENV):
        return result
    with open(GOOGLE_API_ENV) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if '=' in line:
                k, v = line.split('=', 1)
                result[k.strip()] = v.strip()
    return result


# ---------------------------------------------------------------------------
# ChromiumManager — 로컬 Chromium 생명주기 관리 (기존 구조 유지)
# ---------------------------------------------------------------------------


class State(Enum):
    STOPPED = auto()
    STARTING = auto()
    READY = auto()
    STOPPING = auto()


class ChromiumManager:
    """로컬 Chromium 프로세스 생애주기를 단일 상태머신으로 관리한다."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        self._state = State.STOPPED
        self._proc: subprocess.Popen | None = None

    def wait_ready(self, timeout: float = 30.0) -> bool:
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
                    self._trigger_start()
                self._cond.wait(timeout=min(remaining, 1.0))

    def shutdown(self) -> None:
        with self._cond:
            if self._state == State.STOPPING:
                return
            log.info('ChromiumManager: STOPPING')
            self._state = State.STOPPING
            self._cond.notify_all()
            proc = self._proc
        if proc is not None:
            _fix_exit_type()
            try:
                proc.terminate()
                proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                proc.kill()
            except Exception as e:
                log.warning(f'프로세스 종료 중 오류: {e}')

    def _trigger_start(self) -> None:
        if self._state != State.STOPPED:
            return
        log.info('ChromiumManager: STOPPED → STARTING')
        self._state = State.STARTING
        self._cond.notify_all()
        threading.Thread(target=self._do_start, daemon=True).start()

    def _set_state(self, new_state: State) -> None:
        with self._cond:
            self._state = new_state
            self._cond.notify_all()

    def _do_start(self) -> None:
        for attempt in range(1, MAX_RETRY + 1):
            log.info(f'Chromium 시작 시도 {attempt}/{MAX_RETRY}: {CHROMIUM_PATH}')
            try:
                env = os.environ.copy()
                env.update(_load_google_api_env())
                proc = subprocess.Popen(
                    [CHROMIUM_PATH,
                     '--no-first-run',
                     '--disable-default-apps',
                     '--no-default-browser-check',
                     '--homepage=about:blank',
                     '--homepage=about:blank',
                    ],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    env=env
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

            if self._wait_socket(proc):
                with self._cond:
                    if self._state == State.STOPPING:
                        proc.terminate()
                        return
                    self._proc = proc
                    self._state = State.READY
                    self._cond.notify_all()
                log.info('ChromiumManager: STARTING → READY')
                threading.Thread(target=self._monitor_loop, daemon=True).start()
                return

            log.warning('소켓 대기 타임아웃, 프로세스 종료 후 재시도')
            try:
                proc.terminate()
                proc.wait(timeout=3)
            except Exception:
                pass
            if attempt < MAX_RETRY:
                time.sleep(RETRY_DELAY)

        log.error('Chromium 시작 최종 실패 — STOPPED 복귀')
        self._set_state(State.STOPPED)

    def _wait_socket(self, proc: subprocess.Popen) -> bool:
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

    def _monitor_loop(self) -> None:
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
                # 프로세스가 살아있으면 소켓 체크 불필요
                # (풀 세션이 소켓을 점유 중이면 연결 테스트가 실패할 수 있음)
                continue

            if exit_code in (0, -15):
                log.info(f'Chromium 정상 종료 (exit={exit_code}) — 재시작 안 함')
                with self._cond:
                    if self._state == State.READY:
                        self._state = State.STOPPED
                        self._proc = None
                        self._cond.notify_all()
                return

            log.warning(f'Chromium 크래시 감지 (exit={exit_code}) — {COOLDOWN}초 후 재시작')
            with self._cond:
                if self._state == State.READY:
                    self._state = State.STOPPED
                    self._proc = None
                    self._cond.notify_all()
            time.sleep(COOLDOWN)
            with self._cond:
                if self._state == State.STOPPED:
                    self._trigger_start()
            return

    def _socket_alive(self) -> bool:
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
# Pool 설정/런타임 모델
# ---------------------------------------------------------------------------


class InstanceState(str, Enum):
    UP_IDLE = 'UP_IDLE'
    UP_BUSY = 'UP_BUSY'
    DOWN = 'DOWN'


@dataclass
class InstanceSpec:
    id: str
    transport: str
    capacity: int = 1
    enabled: bool = True
    socket: str = CHROMIUM_SOCKET
    host: str = ''
    user: str = 'root'
    key_path: str = '~/.ssh/proxmox'
    jump_host: str = ''
    remote_socket: str = CHROMIUM_SOCKET


@dataclass
class PoolConfig:
    listen_socket: str = DEFAULT_PROXY_SOCKET
    assignment_strategy: str = 'least_recently_used'
    acquire_timeout_sec: float = 20.0
    healthcheck_interval_sec: float = 30.0
    healthcheck_failure_threshold: int = 3
    healthcheck_cooldown_sec: float = 60.0
    instances: list[InstanceSpec] = field(default_factory=list)


@dataclass
class InstanceRuntime:
    spec: InstanceSpec
    state: InstanceState = InstanceState.UP_IDLE
    active_sessions: int = 0
    consecutive_failures: int = 0
    last_assigned_at: float = 0.0
    last_healthy_at: float = 0.0
    cooldown_until: float = 0.0
    skip_reason: str = ''


class SSHSocketConnection:
    """paramiko 채널을 소켓과 유사한 recv/sendall 인터페이스로 감싼다."""

    def __init__(self, ssh_client, channel, jump_client=None):
        self._ssh_client = ssh_client
        self._channel = channel
        self._jump_client = jump_client
        self._closed = False

    def recv(self, size: int) -> bytes:
        return self._channel.recv(size)

    def sendall(self, data: bytes) -> None:
        view = memoryview(data)
        while view:
            sent = self._channel.send(view)
            if sent <= 0:
                raise OSError('SSH 채널 송신 실패')
            view = view[sent:]

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        for obj in (self._channel, self._ssh_client, self._jump_client):
            if obj is None:
                continue
            try:
                obj.close()
            except Exception:
                pass


class InstancePool:
    """인스턴스 풀의 상태/배정/헬스체크를 관리한다."""

    def __init__(self, cfg: PoolConfig, chromium_manager: ChromiumManager):
        self._cfg = cfg
        self._chromium_manager = chromium_manager
        self._cond = threading.Condition()
        self._running = True
        self._instances: list[InstanceRuntime] = [
            InstanceRuntime(spec=spec) for spec in cfg.instances
        ]
        self._init_instance_states()
        self._health_thread = threading.Thread(target=self._healthcheck_loop, daemon=True)
        self._health_thread.start()

    @property
    def listen_socket(self) -> str:
        return self._cfg.listen_socket

    @property
    def acquire_timeout_sec(self) -> float:
        return self._cfg.acquire_timeout_sec

    def stop(self) -> None:
        with self._cond:
            self._running = False
            self._cond.notify_all()
        self._health_thread.join(timeout=2.0)

    def acquire(self, timeout: float | None = None) -> InstanceRuntime | None:
        deadline = time.monotonic() + (timeout or self._cfg.acquire_timeout_sec)
        with self._cond:
            while self._running:
                now = time.monotonic()
                self._refresh_cooldown_locked(now)
                candidates = self._select_candidates_locked()
                if candidates:
                    chosen = min(candidates, key=lambda rt: (rt.last_assigned_at, rt.spec.id))
                    chosen.active_sessions += 1
                    chosen.last_assigned_at = now
                    if chosen.active_sessions >= chosen.spec.capacity:
                        chosen.state = InstanceState.UP_BUSY
                    else:
                        chosen.state = InstanceState.UP_IDLE
                    log.info(
                        f'인스턴스 배정: {chosen.spec.id} '
                        f'(active={chosen.active_sessions}/{chosen.spec.capacity})'
                    )
                    return chosen

                remain = deadline - now
                if remain <= 0:
                    return None
                self._cond.wait(timeout=min(remain, 1.0))
        return None

    def release(self, runtime: InstanceRuntime) -> None:
        with self._cond:
            if runtime.active_sessions > 0:
                runtime.active_sessions -= 1
            if runtime.state != InstanceState.DOWN and runtime.active_sessions < runtime.spec.capacity:
                runtime.state = InstanceState.UP_IDLE
            self._cond.notify_all()
        log.info(
            f'인스턴스 반납: {runtime.spec.id} '
            f'(active={runtime.active_sessions}/{runtime.spec.capacity})'
        )

    def connect_backend(self, runtime: InstanceRuntime, for_healthcheck: bool = False):
        spec = runtime.spec
        if spec.transport == 'unix':
            if self._is_local_manager_instance(spec):
                if for_healthcheck:
                    # healthcheck에서는 Chromium을 새로 시작하지 않고 소켓 존재만 확인
                    if not os.path.exists(spec.socket):
                        raise RuntimeError('로컬 소켓 없음 (Chromium 미실행)')
                else:
                    if not self._chromium_manager.wait_ready(timeout=SOCKET_WAIT):
                        raise RuntimeError('로컬 Chromium 준비 실패')
            return _connect_unix_with_retry(spec.socket, retries=1 if for_healthcheck else MAX_RETRY)

        if spec.transport == 'ssh-unix':
            return _connect_remote_via_paramiko(spec)

        raise RuntimeError(f'지원하지 않는 transport: {spec.transport}')

    def mark_connect_success(self, runtime: InstanceRuntime) -> None:
        with self._cond:
            runtime.consecutive_failures = 0
            runtime.last_healthy_at = time.monotonic()
            if runtime.active_sessions < runtime.spec.capacity and runtime.state != InstanceState.DOWN:
                runtime.state = InstanceState.UP_IDLE
            self._cond.notify_all()

    def mark_connect_failure(self, runtime: InstanceRuntime, reason: str) -> None:
        self._mark_failure(runtime, f'연결 실패: {reason}')

    def _init_instance_states(self) -> None:
        for runtime in self._instances:
            spec = runtime.spec
            if spec.capacity <= 0:
                spec.capacity = 1
            if not spec.enabled:
                runtime.state = InstanceState.DOWN
                runtime.skip_reason = 'enabled=false'
                log.info(f'인스턴스 비활성화: {spec.id}')
                continue
            if spec.transport not in ('unix', 'ssh-unix'):
                runtime.state = InstanceState.DOWN
                runtime.skip_reason = f'unsupported transport={spec.transport}'
                log.warning(f'인스턴스 스킵: {spec.id} ({runtime.skip_reason})')
                continue
            if spec.transport == 'ssh-unix' and paramiko is None:
                runtime.state = InstanceState.DOWN
                runtime.skip_reason = 'paramiko 미설치'
                log.warning(f'원격 인스턴스 스킵: {spec.id} ({runtime.skip_reason})')
                continue
            runtime.state = InstanceState.UP_IDLE
            log.info(f'인스턴스 등록: {spec.id} ({spec.transport})')

    def _select_candidates_locked(self) -> list[InstanceRuntime]:
        return [
            rt for rt in self._instances
            if rt.state == InstanceState.UP_IDLE
            and not rt.skip_reason
            and rt.active_sessions < rt.spec.capacity
        ]

    def _refresh_cooldown_locked(self, now: float) -> None:
        for runtime in self._instances:
            if runtime.state != InstanceState.DOWN:
                continue
            if runtime.cooldown_until <= 0 or now < runtime.cooldown_until:
                continue
            if runtime.skip_reason:
                continue
            runtime.state = InstanceState.UP_IDLE
            runtime.consecutive_failures = 0
            runtime.cooldown_until = 0
            log.info(f'인스턴스 cooldown 해제: {runtime.spec.id}')
            self._cond.notify_all()

    def _mark_failure(self, runtime: InstanceRuntime, reason: str) -> None:
        with self._cond:
            runtime.consecutive_failures += 1
            fail = runtime.consecutive_failures
            threshold = self._cfg.healthcheck_failure_threshold
            if fail >= threshold:
                runtime.state = InstanceState.DOWN
                runtime.cooldown_until = time.monotonic() + self._cfg.healthcheck_cooldown_sec
                log.warning(
                    f'인스턴스 DOWN 전이: {runtime.spec.id} '
                    f'(fail={fail}/{threshold}, cooldown={self._cfg.healthcheck_cooldown_sec}s, reason={reason})'
                )
            else:
                if runtime.active_sessions < runtime.spec.capacity and runtime.state != InstanceState.DOWN:
                    runtime.state = InstanceState.UP_IDLE
                log.warning(
                    f'인스턴스 실패 누적: {runtime.spec.id} '
                    f'(fail={fail}/{threshold}, reason={reason})'
                )
            self._cond.notify_all()

    def _healthcheck_loop(self) -> None:
        while True:
            with self._cond:
                if not self._running:
                    return
                interval = max(1.0, self._cfg.healthcheck_interval_sec)
            time.sleep(interval)

            with self._cond:
                if not self._running:
                    return
                now = time.monotonic()
                self._refresh_cooldown_locked(now)
                targets = [
                    rt for rt in self._instances
                    if rt.state == InstanceState.UP_IDLE
                    and not rt.skip_reason
                    and rt.active_sessions == 0
                    # 로컬 매니저 인스턴스는 lazy-start 대상이므로
                    # healthcheck로 DOWN 전이시키지 않는다.
                    and not self._is_local_manager_instance(rt.spec)
                ]

            for runtime in targets:
                if not self._running:
                    return
                conn = None
                try:
                    conn = self.connect_backend(runtime, for_healthcheck=True)
                    self.mark_connect_success(runtime)
                except Exception as e:
                    self._mark_failure(runtime, f'healthcheck 실패: {e}')
                finally:
                    _close_quietly(conn)

    @staticmethod
    def _is_local_manager_instance(spec: InstanceSpec) -> bool:
        if spec.transport != 'unix':
            return False
        return os.path.abspath(spec.socket) == os.path.abspath(CHROMIUM_SOCKET)


# ---------------------------------------------------------------------------
# 소켓/SSH 유틸리티
# ---------------------------------------------------------------------------


def _close_quietly(obj) -> None:
    if obj is None:
        return
    try:
        obj.close()
    except Exception:
        pass


def _bridge(src, dst) -> None:
    """src → dst 단방향 relay. MCP 프레이밍은 그대로 통과시킨다."""
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except Exception:
        pass
    finally:
        _close_quietly(src)
        _close_quietly(dst)


def _connect_unix_with_retry(path: str, retries: int = MAX_RETRY):
    last_err: Exception | None = None
    for attempt in range(1, retries + 1):
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(2.0)
            sock.connect(path)
            sock.settimeout(None)
            return sock
        except Exception as e:
            last_err = e
            if attempt < retries:
                time.sleep(RETRY_DELAY)
    raise RuntimeError(f'unix 소켓 연결 실패: {path}: {last_err}')


def _resolve_host_config(host_alias: str, fallback_user: str, fallback_key: str) -> tuple[str, str, str]:
    hostname = host_alias
    user = fallback_user
    key_path = os.path.expanduser(fallback_key)

    if paramiko is None:
        return hostname, user, key_path

    ssh_config_path = os.path.expanduser('~/.ssh/config')
    if not os.path.isfile(ssh_config_path):
        return hostname, user, key_path

    try:
        ssh_cfg = paramiko.SSHConfig()
        with open(ssh_config_path) as f:
            ssh_cfg.parse(f)
        entry = ssh_cfg.lookup(host_alias)
        hostname = entry.get('hostname', hostname)
        if not fallback_user:
            user = entry.get('user', user)
        if not fallback_key:
            identity = entry.get('identityfile')
            if isinstance(identity, list) and identity:
                key_path = os.path.expanduser(identity[0])
            elif isinstance(identity, str):
                key_path = os.path.expanduser(identity)
    except Exception as e:
        log.warning(f'SSH config 파싱 실패({host_alias}): {e}')

    return hostname, user, key_path


def _connect_remote_via_paramiko(spec: InstanceSpec):
    if paramiko is None:
        raise RuntimeError('paramiko 미설치')

    target_host, target_user, target_key = _resolve_host_config(spec.host, spec.user, spec.key_path)

    ssh = paramiko.SSHClient()
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    jump_ssh = None
    try:
        if spec.jump_host:
            jump_host, jump_user, jump_key = _resolve_host_config(spec.jump_host, spec.user, spec.key_path)
            jump_ssh = paramiko.SSHClient()
            jump_ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
            jump_ssh.connect(
                jump_host,
                username=jump_user,
                key_filename=jump_key,
                timeout=SSH_CONNECT_TIMEOUT
            )
            jump_transport = jump_ssh.get_transport()
            if jump_transport is None:
                raise RuntimeError(f'jump host transport 없음: {spec.jump_host}')
            proxy_channel = jump_transport.open_channel(
                'direct-tcpip',
                (target_host, 22),
                ('127.0.0.1', 0)
            )
            ssh.connect(
                target_host,
                username=target_user,
                key_filename=target_key,
                sock=proxy_channel,
                timeout=SSH_CONNECT_TIMEOUT
            )
        else:
            ssh.connect(
                target_host,
                username=target_user,
                key_filename=target_key,
                timeout=SSH_CONNECT_TIMEOUT
            )

        transport = ssh.get_transport()
        if transport is None:
            raise RuntimeError(f'SSH transport 없음: {spec.id}')

        channel = transport.open_session()
        remote_socket_quoted = shlex.quote(spec.remote_socket)
        channel.exec_command(f'socat STDIO UNIX-CONNECT:{remote_socket_quoted}')
        return SSHSocketConnection(ssh_client=ssh, channel=channel, jump_client=jump_ssh)
    except Exception:
        _close_quietly(ssh)
        _close_quietly(jump_ssh)
        raise


# ---------------------------------------------------------------------------
# 종료 시 exit_type 정상화
# ---------------------------------------------------------------------------


def _fix_exit_type() -> None:
    prefs_pattern = os.path.expanduser(
        '~/Library/Application Support/Chromium/*/Preferences'
    )
    for prefs_path in glob.glob(prefs_pattern):
        try:
            with open(prefs_path) as f:
                prefs = json.load(f)
            if prefs.get('profile', {}).get('exit_type') != 'Normal':
                prefs.setdefault('profile', {})['exit_type'] = 'Normal'
                with open(prefs_path, 'w') as f:
                    json.dump(prefs, f)
                log.info(f'exit_type → Normal: {prefs_path}')
        except Exception as e:
            log.warning(f'exit_type 수정 실패: {prefs_path}: {e}')


# ---------------------------------------------------------------------------
# pool.json 로드
# ---------------------------------------------------------------------------


def _default_pool_config() -> PoolConfig:
    return PoolConfig(
        listen_socket=DEFAULT_PROXY_SOCKET,
        assignment_strategy='least_recently_used',
        acquire_timeout_sec=20.0,
        healthcheck_interval_sec=30.0,
        healthcheck_failure_threshold=3,
        healthcheck_cooldown_sec=60.0,
        instances=[
            InstanceSpec(
                id='local-default',
                transport='unix',
                socket=CHROMIUM_SOCKET,
                capacity=1,
                enabled=True
            )
        ]
    )


def load_pool_config() -> PoolConfig:
    """~/.chromium-mcp/pool.json 로드. 없으면 단일 로컬 기본값."""
    default_cfg = _default_pool_config()
    if not os.path.isfile(POOL_CONFIG_FILE):
        log.info('pool.json 없음 — 단일 로컬 인스턴스 모드로 동작')
        return default_cfg

    try:
        with open(POOL_CONFIG_FILE) as f:
            raw = json.load(f)
    except Exception as e:
        log.error(f'pool.json 로드 실패({POOL_CONFIG_FILE}): {e} — 기본값 사용')
        return default_cfg

    listen_socket = raw.get('listen_socket', default_cfg.listen_socket)
    assignment = raw.get('assignment', {})
    healthcheck = raw.get('healthcheck', {})

    cfg = PoolConfig(
        listen_socket=listen_socket,
        assignment_strategy=assignment.get('strategy', 'least_recently_used'),
        acquire_timeout_sec=float(assignment.get('acquire_timeout_sec', 20)),
        healthcheck_interval_sec=float(healthcheck.get('interval_sec', 30)),
        healthcheck_failure_threshold=int(healthcheck.get('failure_threshold', 3)),
        healthcheck_cooldown_sec=float(healthcheck.get('cooldown_sec', 60)),
        instances=[]
    )

    if cfg.assignment_strategy != 'least_recently_used':
        log.warning(
            f'지원하지 않는 assignment.strategy={cfg.assignment_strategy}. '
            'least_recently_used로 동작한다.'
        )

    raw_instances = raw.get('instances', [])
    for item in raw_instances:
        transport = str(item.get('transport', 'unix'))
        spec = InstanceSpec(
            id=str(item.get('id', f'instance-{len(cfg.instances) + 1}')),
            transport=transport,
            capacity=max(1, int(item.get('capacity', 1))),
            enabled=bool(item.get('enabled', True)),
            socket=str(item.get('socket', CHROMIUM_SOCKET)),
            host=str(item.get('host', '')),
            user=str(item.get('user', 'root')),
            key_path=str(item.get('key_path', '~/.ssh/proxmox')),
            jump_host=str(item.get('jump_host', '')),
            remote_socket=str(item.get('remote_socket', CHROMIUM_SOCKET))
        )
        cfg.instances.append(spec)

    if not cfg.instances:
        log.warning('pool.json instances가 비어 있음 — 단일 로컬 인스턴스 fallback 사용')
        cfg.instances = default_cfg.instances
    elif paramiko is None and not any(
        inst.enabled and inst.transport == 'unix'
        for inst in cfg.instances
    ):
        # paramiko 미설치 환경에서는 원격 ssh-unix를 사용할 수 없으므로
        # 최소한의 하위 호환을 위해 로컬 기본 인스턴스를 추가한다.
        log.warning('paramiko 미설치 + 로컬 인스턴스 없음 — local-default 추가')
        cfg.instances.append(default_cfg.instances[0])

    return cfg


# ---------------------------------------------------------------------------
# 클라이언트 핸들러
# ---------------------------------------------------------------------------


def handle_client(client_sock: socket.socket, pool: InstancePool) -> None:
    """새 클라이언트 연결을 풀 인스턴스에 배정하고 raw relay를 수행한다."""
    addr = id(client_sock)
    runtime: InstanceRuntime | None = None
    backend = None

    log.info(f'[{addr}] 클라이언트 연결')
    try:
        runtime = pool.acquire(timeout=pool.acquire_timeout_sec)
        if runtime is None:
            log.error(f'[{addr}] 인스턴스 배정 실패 (timeout={pool.acquire_timeout_sec}s)')
            return

        try:
            backend = pool.connect_backend(runtime, for_healthcheck=False)
            pool.mark_connect_success(runtime)
            log.info(f'[{addr}] 세션 배정 완료 → {runtime.spec.id}')
        except Exception as e:
            pool.mark_connect_failure(runtime, str(e))
            log.error(f'[{addr}] 백엔드 연결 실패 ({runtime.spec.id}): {e}')
            return

        t1 = threading.Thread(target=_bridge, args=(client_sock, backend), daemon=True)
        t2 = threading.Thread(target=_bridge, args=(backend, client_sock), daemon=True)
        t1.start()
        t2.start()
        t1.join()
        t2.join()
    except Exception as e:
        log.warning(f'[{addr}] 핸들러 예외: {e}')
    finally:
        _close_quietly(client_sock)
        _close_quietly(backend)
        if runtime is not None:
            pool.release(runtime)
        log.info(f'[{addr}] 연결 종료')


# ---------------------------------------------------------------------------
# 데몬 진입점
# ---------------------------------------------------------------------------


def write_pid() -> None:
    with open(PID_FILE, 'w') as f:
        f.write(str(os.getpid()))


def _cleanup_files() -> None:
    if os.path.exists(_listen_socket_path):
        try:
            os.unlink(_listen_socket_path)
        except Exception:
            pass
    if os.path.exists(PID_FILE):
        try:
            os.unlink(PID_FILE)
        except Exception:
            pass


def _handle_signal(signum=None, frame=None) -> None:
    global _running
    _running = False
    log.info(f'시그널 수신({signum}) — 데몬 종료 시작')
    if _server_socket is not None:
        try:
            _server_socket.close()
        except Exception:
            pass


def _check_already_running() -> bool:
    """기존 데몬이 실행 중이면 True 반환."""
    if not os.path.isfile(PID_FILE):
        return False
    try:
        with open(PID_FILE) as f:
            old_pid = int(f.read().strip())
        os.kill(old_pid, 0)  # 프로세스 존재 확인
        log.warning(f'데몬이 이미 실행 중 (PID={old_pid}). 종료합니다.')
        return True
    except (ValueError, OSError):
        # PID 파일은 있지만 프로세스가 없음 — 정리
        return False


def main() -> None:
    global _server_socket, _listen_socket_path

    signal.signal(signal.SIGTERM, _handle_signal)
    signal.signal(signal.SIGINT, _handle_signal)

    if _check_already_running():
        sys.exit(0)

    write_pid()
    log.info(f'=== Chromium MCP Pool 데몬 시작 (PID={os.getpid()}) ===')

    cfg = load_pool_config()
    _listen_socket_path = cfg.listen_socket or DEFAULT_PROXY_SOCKET
    manager = ChromiumManager()
    pool = InstancePool(cfg, manager)

    if os.path.exists(_listen_socket_path):
        os.unlink(_listen_socket_path)

    _server_socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    _server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    _server_socket.bind(_listen_socket_path)
    os.chmod(_listen_socket_path, 0o600)
    _server_socket.listen(64)
    log.info(f'프론트 소켓 대기: {_listen_socket_path}')

    try:
        while _running:
            try:
                _server_socket.settimeout(1.0)
                client_sock, _ = _server_socket.accept()
            except socket.timeout:
                continue
            except Exception as e:
                if _running:
                    log.error(f'accept 오류: {e}')
                break

            threading.Thread(
                target=handle_client,
                args=(client_sock, pool),
                daemon=True
            ).start()
    finally:
        pool.stop()
        manager.shutdown()
        if _server_socket is not None:
            _close_quietly(_server_socket)
            _server_socket = None
        _cleanup_files()
        log.info('데몬 종료 완료')


if __name__ == '__main__':
    main()
