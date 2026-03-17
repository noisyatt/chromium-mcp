#!/usr/bin/env bash
# chromium-mcp-launcher.sh — Chromium-MCP MCP 프록시 런처 (Shell 버전)
#
# Python 이 없는 환경에서 사용하는 bash 구현 런처입니다.
# Python 버전과 동일한 동작을 수행하되, socat / nc 으로 프록시를 구현합니다.
#
# 프록시 우선순위:
#   1. socat (가장 안정적)
#   2. nc (netcat, BSD/GNU 모두 지원)
#   3. 수동 bash read/write 루프 (fallback)
#
# 사용법:
#   chromium-mcp-launcher.sh [옵션]
#
# 옵션:
#   --browser-path PATH   Chromium-MCP 바이너리 경로
#   --no-auto-launch      브라우저 자동 실행 비활성화
#   --no-keep-alive       프록시 종료 시 브라우저도 종료
#   --socket-path PATH    Unix socket 경로 지정
#   --config PATH         설정 파일 경로 (기본: ~/.chromium-mcp/config.json)
#   --timeout SECS        브라우저 시작 대기 최대 시간 (기본: 30)

set -euo pipefail

# ---------------------------------------------------------------------------
# 상수 / 기본값
# ---------------------------------------------------------------------------

LOCK_DIR="${HOME}/.chromium-mcp"
LOCK_FILE="${LOCK_DIR}/instance.lock"
CONFIG_FILE="${LOCK_DIR}/config.json"

DEFAULT_LAUNCH_TIMEOUT=30
SOCKET_POLL_INTERVAL=0.5

# 기본 브라우저 후보 경로 목록
DEFAULT_BROWSER_PATHS=(
    "/Applications/Chromium-MCP.app/Contents/MacOS/Chromium"
    "/Applications/Chromium.app/Contents/MacOS/Chromium"
    "/usr/local/bin/chromium-mcp"
    "/usr/bin/chromium-mcp"
    "/usr/local/bin/chromium"
    "/usr/bin/chromium"
)

# ---------------------------------------------------------------------------
# 전역 변수 (옵션 파싱 결과)
# ---------------------------------------------------------------------------

BROWSER_PATH=""
AUTO_LAUNCH="true"
KEEP_ALIVE="true"
SOCKET_PATH=""
LAUNCH_TIMEOUT="${DEFAULT_LAUNCH_TIMEOUT}"
CONFIG_PATH="${CONFIG_FILE}"

# 시작된 브라우저 PID (자동 실행한 경우)
BROWSER_PID=""

# ---------------------------------------------------------------------------
# 로깅 (stderr 출력)
# ---------------------------------------------------------------------------

log() {
    echo "[chromium-mcp-launcher] $*" >&2
}

# ---------------------------------------------------------------------------
# 커맨드라인 인수 파싱
# ---------------------------------------------------------------------------

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --browser-path)
                BROWSER_PATH="$2"; shift 2 ;;
            --browser-path=*)
                BROWSER_PATH="${1#*=}"; shift ;;
            --no-auto-launch)
                AUTO_LAUNCH="false"; shift ;;
            --auto-launch)
                AUTO_LAUNCH="true"; shift ;;
            --no-keep-alive)
                KEEP_ALIVE="false"; shift ;;
            --keep-alive)
                KEEP_ALIVE="true"; shift ;;
            --socket-path)
                SOCKET_PATH="$2"; shift 2 ;;
            --socket-path=*)
                SOCKET_PATH="${1#*=}"; shift ;;
            --config)
                CONFIG_PATH="$2"; shift 2 ;;
            --config=*)
                CONFIG_PATH="${1#*=}"; shift ;;
            --timeout)
                LAUNCH_TIMEOUT="$2"; shift 2 ;;
            --timeout=*)
                LAUNCH_TIMEOUT="${1#*=}"; shift ;;
            -h|--help)
                print_usage; exit 0 ;;
            *)
                log "알 수 없는 인수: $1"; shift ;;
        esac
    done
}

print_usage() {
    cat >&2 <<EOF
사용법: $(basename "$0") [옵션]

옵션:
  --browser-path PATH   Chromium-MCP 바이너리 경로
  --no-auto-launch      브라우저 자동 실행 비활성화
  --no-keep-alive       프록시 종료 시 브라우저도 종료
  --socket-path PATH    Unix socket 경로 지정
  --config PATH         설정 파일 경로 (기본: ~/.chromium-mcp/config.json)
  --timeout SECS        브라우저 시작 대기 최대 시간 (기본: 30)
  -h, --help            이 도움말 출력

환경변수:
  CHROMIUM_MCP_BROWSER_PATH   브라우저 바이너리 경로
  CHROMIUM_MCP_SOCKET_PATH    Unix socket 경로
EOF
}

# ---------------------------------------------------------------------------
# 설정 파일 로드 (JSON → 전역 변수)
# ---------------------------------------------------------------------------

load_config() {
    local cfg_file="$1"
    if [[ ! -f "${cfg_file}" ]]; then
        return
    fi

    # python3 로 JSON 파싱 (사용 가능한 경우)
    if command -v python3 &>/dev/null; then
        local val
        val=$(python3 -c "
import json, sys
try:
    d = json.load(open('${cfg_file}'))
    print(d.get('browser_path',''))
    print(d.get('socket_path',''))
    print(str(d.get('auto_launch',True)).lower())
    print(str(d.get('keep_alive',True)).lower())
    print(str(d.get('launch_timeout',${DEFAULT_LAUNCH_TIMEOUT})))
except Exception:
    print('')
    print('')
    print('true')
    print('true')
    print('${DEFAULT_LAUNCH_TIMEOUT}')
" 2>/dev/null)
        IFS=$'\n' read -r -d '' cfg_browser_path cfg_socket_path \
            cfg_auto_launch cfg_keep_alive cfg_timeout \
            <<< "${val}" || true

        # 커맨드라인 인수가 없을 때만 설정 파일 값 적용
        [[ -z "${BROWSER_PATH}" && -n "${cfg_browser_path}" ]] && BROWSER_PATH="${cfg_browser_path}"
        [[ -z "${SOCKET_PATH}"  && -n "${cfg_socket_path}"  ]] && SOCKET_PATH="${cfg_socket_path}"
        [[ "${AUTO_LAUNCH}" == "true"    && -n "${cfg_auto_launch}" ]] && AUTO_LAUNCH="${cfg_auto_launch}"
        [[ "${KEEP_ALIVE}"  == "true"    && -n "${cfg_keep_alive}"  ]] && KEEP_ALIVE="${cfg_keep_alive}"
        [[ "${LAUNCH_TIMEOUT}" == "${DEFAULT_LAUNCH_TIMEOUT}" && -n "${cfg_timeout}" ]] && LAUNCH_TIMEOUT="${cfg_timeout}"
    else
        # python3 없으면 grep 으로 간단히 파싱 (오류 허용)
        local bp
        bp=$(grep -o '"browser_path"\s*:\s*"[^"]*"' "${cfg_file}" 2>/dev/null \
             | sed 's/.*: *"\(.*\)"/\1/' || true)
        [[ -z "${BROWSER_PATH}" && -n "${bp}" ]] && BROWSER_PATH="${bp}"
    fi
}

# ---------------------------------------------------------------------------
# lock 파일 관련 함수
# ---------------------------------------------------------------------------

read_lock_pid() {
    if [[ ! -f "${LOCK_FILE}" ]]; then
        echo ""
        return
    fi
    if command -v python3 &>/dev/null; then
        python3 -c "
import json
try:
    d = json.load(open('${LOCK_FILE}'))
    print(d.get('pid',''))
except Exception:
    print('')
" 2>/dev/null
    else
        grep -o '"pid"\s*:\s*[0-9]*' "${LOCK_FILE}" 2>/dev/null \
            | grep -o '[0-9]*$' || true
    fi
}

read_lock_socket() {
    if [[ ! -f "${LOCK_FILE}" ]]; then
        echo ""
        return
    fi
    if command -v python3 &>/dev/null; then
        python3 -c "
import json
try:
    d = json.load(open('${LOCK_FILE}'))
    print(d.get('socket',''))
except Exception:
    print('')
" 2>/dev/null
    else
        grep -o '"socket"\s*:\s*"[^"]*"' "${LOCK_FILE}" 2>/dev/null \
            | sed 's/.*: *"\(.*\)"/\1/' || true
    fi
}

is_process_alive() {
    local pid="$1"
    if [[ -z "${pid}" || "${pid}" -le 0 ]] 2>/dev/null; then
        return 1
    fi
    # kill -0: 시그널을 보내지 않고 프로세스 존재 여부만 확인
    kill -0 "${pid}" 2>/dev/null
}

is_socket_connectable() {
    local sock_path="$1"
    if [[ -z "${sock_path}" ]]; then
        return 1
    fi

    # socat 을 이용한 연결 테스트
    if command -v socat &>/dev/null; then
        echo "" | socat -T 0.5 - "UNIX-CONNECT:${sock_path}" &>/dev/null
        return $?
    fi

    # python3 fallback
    if command -v python3 &>/dev/null; then
        python3 -c "
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(0.5)
try:
    s.connect('${sock_path}')
    s.close()
    sys.exit(0)
except Exception:
    sys.exit(1)
" 2>/dev/null
        return $?
    fi

    # 소켓 파일 존재 여부만으로 간접 확인 (불완전하지만 fallback)
    [[ -S "${sock_path}" ]]
}

# ---------------------------------------------------------------------------
# 실행 중인 인스턴스 탐색
# ---------------------------------------------------------------------------

find_running_instance() {
    # lock 파일이 없으면 바로 반환
    if [[ ! -f "${LOCK_FILE}" ]]; then
        log "lock 파일 없음 — 실행 중인 인스턴스 없음"
        echo ""
        return
    fi

    local pid sock_path
    pid=$(read_lock_pid)
    sock_path=$(read_lock_socket)

    if [[ -z "${pid}" || -z "${sock_path}" ]]; then
        log "lock 파일 형식 오류"
        echo ""
        return
    fi

    if ! is_process_alive "${pid}"; then
        log "기존 인스턴스 PID ${pid} 는 이미 종료됨"
        rm -f "${LOCK_FILE}"
        echo ""
        return
    fi

    if ! is_socket_connectable "${sock_path}"; then
        log "소켓 ${sock_path} 에 연결 불가"
        echo ""
        return
    fi

    log "실행 중인 인스턴스 발견 (PID ${pid}, 소켓: ${sock_path})"
    echo "${sock_path}"
}

# ---------------------------------------------------------------------------
# 브라우저 경로 탐색
# ---------------------------------------------------------------------------

find_browser_path() {
    # 환경변수 우선
    if [[ -n "${CHROMIUM_MCP_BROWSER_PATH:-}" ]]; then
        if [[ -x "${CHROMIUM_MCP_BROWSER_PATH}" ]]; then
            echo "${CHROMIUM_MCP_BROWSER_PATH}"
            return
        fi
    fi

    # 전역 변수 (설정/인수)
    if [[ -n "${BROWSER_PATH}" ]]; then
        if [[ -x "${BROWSER_PATH}" ]]; then
            echo "${BROWSER_PATH}"
            return
        fi
        log "지정된 브라우저 경로를 찾을 수 없음: ${BROWSER_PATH}"
    fi

    # 기본 경로 목록 탐색
    local path
    for path in "${DEFAULT_BROWSER_PATHS[@]}"; do
        if [[ -x "${path}" ]]; then
            log "브라우저 발견: ${path}"
            echo "${path}"
            return
        fi
    done

    echo ""
}

# ---------------------------------------------------------------------------
# 브라우저 자동 실행
# ---------------------------------------------------------------------------

launch_browser() {
    local socket_path="$1"
    local browser_bin
    browser_bin=$(find_browser_path)

    if [[ -z "${browser_bin}" ]]; then
        log "Chromium-MCP 바이너리를 찾을 수 없음"
        log "환경변수 CHROMIUM_MCP_BROWSER_PATH 를 설정하거나"
        log "~/.chromium-mcp/config.json 의 browser_path 를 지정하세요"
        return 1
    fi

    log "브라우저 실행: ${browser_bin} --mcp-socket=${socket_path}"

    # 브라우저를 백그라운드 독립 프로세스로 실행
    # nohup + disown 으로 현재 셸 종료 시 영향받지 않도록 분리
    nohup "${browser_bin}" \
        "--mcp-socket=${socket_path}" \
        "--no-first-run" \
        "--disable-default-apps" \
        >/dev/null 2>&1 &

    BROWSER_PID=$!
    disown "${BROWSER_PID}" 2>/dev/null || true
    log "브라우저 시작됨 (PID ${BROWSER_PID})"
}

# ---------------------------------------------------------------------------
# 소켓 대기
# ---------------------------------------------------------------------------

wait_for_socket() {
    local socket_path="$1"
    local timeout="$2"
    local elapsed=0

    log "소켓 대기 중: ${socket_path} (최대 ${timeout}초)"

    while (( elapsed < timeout )); do
        if is_socket_connectable "${socket_path}"; then
            log "소켓 연결 가능 확인 (${elapsed}초 후)"
            return 0
        fi
        sleep "${SOCKET_POLL_INTERVAL}"
        elapsed=$(( elapsed + 1 ))
    done

    log "소켓 대기 타임아웃 (${timeout}초 초과)"
    return 1
}

# ---------------------------------------------------------------------------
# 소켓 경로 결정
# ---------------------------------------------------------------------------

determine_socket_path() {
    # 환경변수 우선
    if [[ -n "${CHROMIUM_MCP_SOCKET_PATH:-}" ]]; then
        echo "${CHROMIUM_MCP_SOCKET_PATH}"
        return
    fi

    # 전역 변수 (설정/인수)
    if [[ -n "${SOCKET_PATH}" ]]; then
        echo "${SOCKET_PATH}"
        return
    fi

    # 랜덤 임시 소켓 경로 생성
    local random_suffix
    random_suffix=$(( RANDOM * RANDOM % 90000 + 10000 ))
    echo "/tmp/.chromium-mcp-${random_suffix}.sock"
}

# ---------------------------------------------------------------------------
# stdin↔socket 양방향 프록시
# ---------------------------------------------------------------------------

run_proxy() {
    local socket_path="$1"

    log "프록시 시작: stdin↔${socket_path}"

    # socat 이 있으면 가장 안정적인 방법으로 프록시
    if command -v socat &>/dev/null; then
        log "socat 으로 프록시 실행"
        socat - "UNIX-CONNECT:${socket_path}"
        return $?
    fi

    # python3 fallback (가장 범용적)
    if command -v python3 &>/dev/null; then
        log "python3 로 프록시 실행"
        python3 - "${socket_path}" <<'PYEOF'
import socket, select, sys, os

sock_path = sys.argv[1]
BUF = 65536

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)

stdin_fd  = sys.stdin.buffer.fileno()
stdout_fd = sys.stdout.buffer

try:
    while True:
        rlist, _, _ = select.select([stdin_fd, s], [], [], 5.0)
        for fd in rlist:
            if fd == stdin_fd:
                data = sys.stdin.buffer.read1(BUF)
                if not data:
                    sys.exit(0)
                s.sendall(data)
            else:
                data = s.recv(BUF)
                if not data:
                    sys.exit(0)
                stdout_fd.write(data)
                stdout_fd.flush()
except (KeyboardInterrupt, BrokenPipeError, OSError):
    pass
finally:
    s.close()
PYEOF
        return $?
    fi

    # nc (netcat) fallback — 단방향 파이프 2개로 구현
    # GNU nc 와 BSD nc 모두 지원하는 방식으로 연결
    if command -v nc &>/dev/null; then
        log "nc 로 프록시 실행"
        # 임시 FIFO 사용하여 양방향 구현
        local fifo
        fifo=$(mktemp -u /tmp/.chromium-mcp-fifo-XXXXXX)
        mkfifo "${fifo}"

        # nc 로 소켓 연결: fifo → socket → stdout
        nc -U "${socket_path}" < "${fifo}" &
        local nc_pid=$!

        # stdin → fifo 로 전달
        cat > "${fifo}"

        # 정리
        rm -f "${fifo}"
        wait "${nc_pid}" 2>/dev/null || true
        return 0
    fi

    # 마지막 fallback: bash 직접 소켓 (bash 4.1+ /dev/unix/...)
    if [[ -e "/dev/unix" ]]; then
        log "/dev/unix 를 통한 bash 직접 소켓 프록시 시도"
        exec 3<>"/dev/unix/stream/${socket_path}"
        # stdin → socket
        while IFS= read -r -d '' -n 4096 chunk; do
            printf '%s' "${chunk}" >&3
        done <&0 &
        # socket → stdout
        while IFS= read -r -d '' -n 4096 chunk <&3; do
            printf '%s' "${chunk}"
        done
        exec 3>&-
        return 0
    fi

    log "오류: socat, python3, nc 모두 사용할 수 없습니다"
    log "다음 중 하나를 설치하세요: socat, python3, nc"
    return 1
}

# ---------------------------------------------------------------------------
# 종료 핸들러
# ---------------------------------------------------------------------------

cleanup() {
    local sig="${1:-EXIT}"
    log "종료 신호 수신 (${sig}) — 프록시 종료 중"

    # --no-keep-alive 인 경우만 브라우저 종료
    if [[ "${KEEP_ALIVE}" == "false" && -n "${BROWSER_PID}" ]]; then
        log "브라우저 종료 중 (PID ${BROWSER_PID})"
        kill "${BROWSER_PID}" 2>/dev/null || true
    fi
}

trap 'cleanup EXIT'  EXIT
trap 'cleanup TERM'  TERM
trap 'cleanup INT'   INT

# ---------------------------------------------------------------------------
# 메인 로직
# ---------------------------------------------------------------------------

main() {
    # 커맨드라인 파싱
    parse_args "$@"

    # 설정 파일 로드
    load_config "${CONFIG_PATH}"

    # 1단계: 실행 중인 인스턴스 탐색
    local socket_path
    socket_path=$(find_running_instance)

    if [[ -z "${socket_path}" ]]; then
        # 2단계: 브라우저 자동 실행
        if [[ "${AUTO_LAUNCH}" == "false" ]]; then
            log "--no-auto-launch 설정됨 — 브라우저를 자동 실행하지 않음"
            log "Chromium-MCP 를 먼저 시작하고 다시 연결하세요"
            exit 1
        fi

        socket_path=$(determine_socket_path)
        log "새 인스턴스를 시작합니다 (소켓: ${socket_path})"

        launch_browser "${socket_path}" || exit 1

        # 소켓 준비 대기
        if ! wait_for_socket "${socket_path}" "${LAUNCH_TIMEOUT}"; then
            log "브라우저 시작 타임아웃"
            exit 1
        fi
    fi

    # 3단계: stdin↔socket 프록시 실행
    run_proxy "${socket_path}"
}

main "$@"
