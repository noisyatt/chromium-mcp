#!/bin/bash
# Chromium GCP 빌드 매니저 v2 — tmux 2-window 구조
# window 0 (work): 실제 작업 실행
# window 1 (mon):  로그 폴링 → 상태 파일 갱신
#
# 사용: bash /tmp/gcp-build-manager.sh

set -euo pipefail
export PATH="$HOME/depot_tools:$PATH"

TAG="146.0.7680.172"
CHROMIUM_DIR="$HOME/chromium"
SRC_DIR="$CHROMIUM_DIR/src"
LOG="/tmp/build-logs"
STATE="/tmp/build-state"

mkdir -p "$LOG"
echo "idle" > "$STATE"

log() { echo "[$(date '+%H:%M:%S')] $*" >> "$LOG/pipeline.log"; echo "$*"; }

# ===================================================================
# 모니터 (window 1에서 실행) — 로그 파일 폴링으로 진행률 추적
# ===================================================================
run_monitor() {
    while true; do
        phase=$(cat "$STATE" 2>/dev/null | cut -d: -f1)
        case "$phase" in
            fetch)
                # git clone 진행률: fetch.log에서 마지막 퍼센트 추출
                pct=$(grep -oP '\d+(?=%)' "$LOG/fetch.log" 2>/dev/null | tail -1)
                detail=$(tail -1 "$LOG/fetch.log" 2>/dev/null | tr '\r' '\n' | tail -1 | head -c 80)
                echo "fetch:${pct:-0}% ${detail}" > "$STATE"
                ;;
            sync)
                # gclient sync: 로그에서 현재 처리 중인 dep 추출
                last=$(grep -E 'Syncing projects:|______' "$LOG/sync.log" 2>/dev/null | tail -1 | head -c 100)
                pct=$(grep -oP '\d+(?=%)' "$LOG/sync.log" 2>/dev/null | tail -1)
                disk=$(df -h / | tail -1 | awk '{print $3"/"$2}')
                echo "sync:${pct:-?}% disk=${disk} ${last:0:60}" > "$STATE"
                ;;
            hooks)
                last=$(tail -1 "$LOG/hooks.log" 2>/dev/null | head -c 100)
                echo "hooks:${last}" > "$STATE"
                ;;
            build)
                # ninja: [current/total] 형태
                progress=$(grep -oP '\[\d+/\d+\]' "$LOG/build.log" 2>/dev/null | tail -1)
                file=$(grep -oP '\S+\.cc$' "$LOG/build.log" 2>/dev/null | tail -1)
                if [ -n "$progress" ]; then
                    cur=$(echo "$progress" | grep -oP '(?<=\[)\d+')
                    tot=$(echo "$progress" | grep -oP '\d+(?=\])')
                    pct=$((cur * 100 / tot))
                    echo "build:${progress} ${pct}% ${file:-}" > "$STATE"
                fi
                ;;
        esac
        sleep 5
    done
}

# ===================================================================
# 메인 파이프라인 (window 0에서 실행)
# ===================================================================
run_pipeline() {
    # --- Phase 1: fetch ---
    echo "fetch:시작" > "$STATE"
    log "Phase 1: shallow clone"
    mkdir -p "$CHROMIUM_DIR" && cd "$CHROMIUM_DIR"

    cat > .gclient << 'EOF'
solutions = [
  {
    "name": "src",
    "url": "https://chromium.googlesource.com/chromium/src.git",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
target_os = ["linux"]
EOF

    # git clone — stderr(진행률)도 파일로 리다이렉트
    git clone --depth=1 --no-tags --progress \
        https://chromium.googlesource.com/chromium/src.git \
        > "$LOG/fetch.log" 2>&1
    log "clone 완료"

    cd "$SRC_DIR"
    echo "fetch:tag checkout" > "$STATE"
    git fetch origin "+refs/tags/${TAG}:refs/tags/${TAG}" --depth=1 >> "$LOG/fetch.log" 2>&1
    git checkout "$TAG" >> "$LOG/fetch.log" 2>&1
    log "Phase 1 완료: $TAG"

    # --- Phase 2: gclient sync ---
    echo "sync:시작" > "$STATE"
    log "Phase 2: gclient sync"
    cd "$CHROMIUM_DIR"
    gclient sync --nohooks --no-history -j4 > "$LOG/sync.log" 2>&1
    log "Phase 2 완료"

    # --- Phase 3: hooks ---
    echo "hooks:시작" > "$STATE"
    log "Phase 3: hooks"

    echo "hooks:install-build-deps" > "$STATE"
    sudo "$SRC_DIR/build/install-build-deps.sh" --no-prompt > "$LOG/deps.log" 2>&1 || true

    echo "hooks:gclient runhooks" > "$STATE"
    cd "$CHROMIUM_DIR"
    gclient runhooks > "$LOG/hooks.log" 2>&1
    log "Phase 3 완료"

    # --- Phase 4: patch ---
    echo "patch:대기" > "$STATE"
    log "Phase 4: MCP 패치 대기 (수동)"
    # 패치 적용은 별도 명령으로
    # 여기서 멈춤 — 패치 후 do_build.sh 실행

    echo "done:패치 대기중" > "$STATE"
    log "파이프라인: 패치 대기 상태"
}

# ===================================================================
# 엔트리포인트: tmux 세션 생성
# ===================================================================
case "${1:-start}" in
    start)
        tmux kill-session -t build 2>/dev/null || true
        # window 0: 파이프라인
        tmux new-session -d -s build -n work \
            "bash /tmp/gcp-build-manager.sh pipeline"
        # window 1: 모니터
        tmux new-window -t build -n mon \
            "bash /tmp/gcp-build-manager.sh monitor"
        echo "tmux 세션 'build' 시작됨 (work + mon)"
        tmux list-windows -t build
        ;;
    pipeline)
        run_pipeline
        ;;
    monitor)
        run_monitor
        ;;
    *)
        echo "Usage: $0 [start|pipeline|monitor]"
        ;;
esac
