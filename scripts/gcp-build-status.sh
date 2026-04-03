#!/bin/bash
# Chromium GCP 빌드 상태 추적 스크립트
# 용도: gclient sync → build 전 과정 모니터링
# 사용: VM 내에서 bash /tmp/gcp-build-status.sh

set -euo pipefail
CHROMIUM_DIR="${HOME}/chromium"
SRC_DIR="${CHROMIUM_DIR}/src"

# --- 1. 단계 판별 ---
phase="unknown"
if pgrep -f 'gclient.py sync' >/dev/null 2>&1; then
    phase="gclient-sync"
elif pgrep -f 'ninja\|autoninja\|siso' >/dev/null 2>&1; then
    phase="build"
elif [ -f "${SRC_DIR}/out/Default/chrome" ]; then
    phase="done"
else
    phase="idle"
fi
echo "단계: ${phase}"

# --- 2. 디스크 ---
echo "디스크: $(df -h / | tail -1 | awk '{print $3"/"$2, $5}')"

# --- 3. 단계별 상세 ---
case "$phase" in
    gclient-sync)
        # 3a. git deps 진행률
        total_deps=$(python3 -c "
import ast, sys
try:
    with open('${SRC_DIR}/DEPS') as f:
        content = f.read()
    # deps 딕셔너리의 키 수 추출
    start = content.find('deps = {')
    if start < 0:
        print(0)
        sys.exit()
    depth = 0
    end = start
    for i, c in enumerate(content[start:], start):
        if c == '{': depth += 1
        elif c == '}': depth -= 1
        if depth == 0:
            end = i + 1
            break
    deps_str = content[start+7:end]
    deps = ast.literal_eval(deps_str)
    print(len(deps))
except:
    print(0)
" 2>/dev/null)
        synced_repos=$(find "${SRC_DIR}" -maxdepth 4 -name '.git' -type d 2>/dev/null | wc -l)
        if [ "${total_deps}" -gt 0 ]; then
            git_pct=$((synced_repos * 100 / total_deps))
            echo "git deps: ${synced_repos}/${total_deps} (${git_pct}%)"
        else
            echo "git deps: ${synced_repos}개 (총수 파싱 불가)"
        fi

        # 3b. cipd 패키지
        total_cipd=$(grep -c "'packages'" "${SRC_DIR}/DEPS" 2>/dev/null || echo 0)
        done_cipd=$(find "${SRC_DIR}" -maxdepth 5 -name '.cipd' -type d 2>/dev/null | wc -l)
        if [ "${total_cipd}" -gt 0 ]; then
            cipd_pct=$((done_cipd * 100 / total_cipd))
            echo "cipd: ${done_cipd}/${total_cipd} (${cipd_pct}%)"
        fi

        # 3c. gsutil 다운로드 (현재 진행 중)
        gsutil_count=$(pgrep -f 'gsutil' 2>/dev/null | wc -l)
        echo "gsutil 병렬: ${gsutil_count}개"
        if [ "${gsutil_count}" -gt 0 ]; then
            pgrep -f 'gsutil.*cp gs://' 2>/dev/null | head -3 | while read pid; do
                target=$(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null | grep -oP '/home/\S+' | tail -1)
                if [ -n "$target" ]; then
                    name=$(echo "$target" | sed "s|${SRC_DIR}/||")
                    if [ -f "$target" ]; then
                        size=$(du -sh "$target" 2>/dev/null | awk '{print $1}')
                        echo "  ↓ ${name} (${size})"
                    else
                        echo "  ↓ ${name} (시작중)"
                    fi
                fi
            done
        fi

        # 3d. hooks 진행
        total_hooks=$(grep -c "'action'" "${SRC_DIR}/DEPS" 2>/dev/null || echo 0)
        echo "hooks: ${total_hooks}개 예정 (--nohooks면 미실행)"
        ;;

    build)
        # 빌드 진행률 (ninja 로그에서 추출)
        ninja_log="${SRC_DIR}/out/Default/.ninja_log"
        if [ -f "$ninja_log" ]; then
            total_targets=$(wc -l < "$ninja_log")
            echo "ninja 타겟: ${total_targets}개 완료"
        fi

        # ninja 진행 상태 (프로세스 출력에서)
        ninja_pid=$(pgrep -f 'ninja|siso' | head -1)
        if [ -n "$ninja_pid" ]; then
            echo "ninja PID: ${ninja_pid}"
            # CPU/메모리
            cpu_mem=$(ps -p "$ninja_pid" -o %cpu,%mem --no-headers 2>/dev/null)
            echo "CPU/MEM: ${cpu_mem}"
        fi

        # 현재 컴파일 중인 파일
        compiling=$(ps aux | grep '[c]lang++\|[c]lang ' | grep -oP '\S+\.cc' | tail -3)
        if [ -n "$compiling" ]; then
            echo "컴파일 중:"
            echo "$compiling" | while read f; do echo "  ${f}"; done
        fi
        ;;

    done)
        binary="${SRC_DIR}/out/Default/chrome"
        size=$(du -sh "$binary" 2>/dev/null | awk '{print $1}')
        date=$(stat -c '%y' "$binary" 2>/dev/null | cut -d. -f1)
        echo "바이너리: ${size} (${date})"
        ;;

    idle)
        echo "대기 중 (gclient sync/build 미실행)"
        ;;
esac

# --- 4. 시스템 상태 ---
load=$(uptime | awk -F'load average:' '{print $2}' | xargs)
echo "로드: ${load}"
