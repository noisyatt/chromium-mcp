#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
chromium-mcp-launcher.py

얇은 bootstrap 런처:
  1. Pool Daemon 생존 확인 (PID 파일 + 소켓)
  2. 미기동 시 daemon 시작
  3. client.py 실행 (Pool 소켓으로 연결)
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

PID_FILE = '/tmp/chromium-mcp-daemon.pid'
DEFAULT_POOL_SOCKET = '/tmp/.chromium-mcp-proxy.sock'
POOL_CONFIG_FILE = Path.home() / '.chromium-mcp' / 'pool.json'
DAEMON_READY_TIMEOUT = 20
SOCKET_POLL_INTERVAL = 0.5

SCRIPT_DIR = Path(__file__).resolve().parent
DAEMON_SCRIPT = SCRIPT_DIR / 'chromium-mcp-daemon.py'
CLIENT_SCRIPT = SCRIPT_DIR / 'chromium-mcp-client.py'


def _log(msg: str) -> None:
    print(f'[chromium-mcp-launcher] {msg}', file=sys.stderr, flush=True)


def _read_daemon_pid() -> int | None:
    if not os.path.isfile(PID_FILE):
        return None
    try:
        with open(PID_FILE) as f:
            return int(f.read().strip())
    except Exception:
        return None


def _is_process_alive(pid: int) -> bool:
    if pid <= 0:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def _is_socket_connectable(sock_path: str) -> bool:
    if not sock_path or not os.path.exists(sock_path):
        return False
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(0.5)
    try:
        sock.connect(sock_path)
        return True
    except Exception:
        return False
    finally:
        try:
            sock.close()
        except Exception:
            pass


def _load_pool_socket() -> str:
    """pool.json의 listen_socket을 우선 사용하고, 없으면 기본값을 쓴다."""
    if not POOL_CONFIG_FILE.is_file():
        return DEFAULT_POOL_SOCKET

    try:
        raw = json.loads(POOL_CONFIG_FILE.read_text())
        sock = str(raw.get('listen_socket', '')).strip()
        if sock:
            return sock
    except Exception as e:
        _log(f'pool.json 파싱 실패: {e} (기본 소켓 사용)')

    return DEFAULT_POOL_SOCKET


def _is_daemon_healthy(pool_socket: str) -> bool:
    pid = _read_daemon_pid()
    if pid is None:
        return False
    if not _is_process_alive(pid):
        return False
    return _is_socket_connectable(pool_socket)


def _start_daemon() -> None:
    _log('Pool Daemon 시작')
    subprocess.Popen(
        [sys.executable, str(DAEMON_SCRIPT)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
    )


def _ensure_daemon(pool_socket: str) -> bool:
    if _is_daemon_healthy(pool_socket):
        _log('Pool Daemon 이미 실행 중')
        return True

    _start_daemon()
    deadline = time.monotonic() + DAEMON_READY_TIMEOUT
    while time.monotonic() < deadline:
        if _is_daemon_healthy(pool_socket):
            _log('Pool Daemon 준비 완료')
            return True
        time.sleep(SOCKET_POLL_INTERVAL)

    _log('Pool Daemon 준비 타임아웃')
    return False


def main() -> int:
    parser = argparse.ArgumentParser(
        prog='chromium-mcp-launcher',
        description='Chromium MCP Pool launcher'
    )
    parser.add_argument(
        '--pool-socket',
        default='',
        help=f'Pool 프론트 소켓 경로 (기본: pool.json 또는 {DEFAULT_POOL_SOCKET})'
    )
    args = parser.parse_args()

    pool_socket = args.pool_socket.strip() or _load_pool_socket()
    _log(f'대상 Pool 소켓: {pool_socket}')

    if not _ensure_daemon(pool_socket):
        return 1

    # client.py가 대상 소켓을 우선 시도하도록 환경변수 주입
    env = os.environ.copy()
    env['CHROMIUM_MCP_SOCKET'] = pool_socket

    proc = subprocess.run([sys.executable, str(CLIENT_SCRIPT)], env=env)
    return proc.returncode


if __name__ == '__main__':
    sys.exit(main())
