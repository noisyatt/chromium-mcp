#!/usr/bin/env bash
# install.sh — chromium-mcp 포크를 Chromium 소스 트리에 설치하는 스크립트
#
# 사용법:
#   ./scripts/install.sh <chromium_src_path>
#
# 인자:
#   chromium_src_path : Chromium 소스의 최상위 src/ 디렉토리 경로
#                       예) /home/user/chromium/src
#
# 수행 작업:
#   1. 소스 파일(src/) → chromium_src/chrome/browser/mcp/ 복사
#   2. patches/ 디렉토리의 .patch 파일을 순서대로 git apply 적용
#
# 오류 시:
#   - 패치 적용 실패는 수동 적용 안내 메시지를 출력하고 계속 진행합니다.
#   - 소스 복사 실패는 즉시 종료합니다.

set -euo pipefail

# ─── 색상 출력 헬퍼 ───────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

info()    { echo -e "${BLUE}[INFO]${NC}  $*"; }
success() { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }

# ─── 사용법 출력 ─────────────────────────────────────────────────────────────
usage() {
  cat <<EOF

사용법: $(basename "$0") <chromium_src_path>

chromium-mcp MCP 서버 소스를 Chromium 소스 트리에 설치합니다.

인자:
  chromium_src_path   Chromium src/ 디렉토리의 절대 또는 상대 경로
                      예) /home/user/chromium/src

예시:
  $(basename "$0") ~/chromium/src
  $(basename "$0") /opt/chromium/src

EOF
}

# ─── 인자 확인 ───────────────────────────────────────────────────────────────
if [[ $# -lt 1 ]]; then
  error "Chromium 소스 경로가 지정되지 않았습니다."
  usage
  exit 1
fi

CHROMIUM_SRC="$(realpath "$1")"

# chromium-mcp 저장소 루트 (이 스크립트 위치 기준)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${REPO_ROOT}/src"
PATCHES_DIR="${REPO_ROOT}/patches"

# ─── 경로 유효성 검사 ─────────────────────────────────────────────────────────
if [[ ! -d "${CHROMIUM_SRC}" ]]; then
  error "디렉토리를 찾을 수 없습니다: ${CHROMIUM_SRC}"
  exit 1
fi

# Chromium 소스 디렉토리 여부 확인 (chrome/ 또는 content/ 존재 확인)
if [[ ! -d "${CHROMIUM_SRC}/chrome" ]] || [[ ! -d "${CHROMIUM_SRC}/content" ]]; then
  error "'${CHROMIUM_SRC}'는 유효한 Chromium src/ 디렉토리가 아닙니다."
  error "chrome/ 및 content/ 하위 디렉토리가 필요합니다."
  exit 1
fi

# Git 저장소 확인 (패치 적용을 위해 필수)
if [[ ! -d "${CHROMIUM_SRC}/.git" ]]; then
  warn "${CHROMIUM_SRC}는 Git 저장소가 아닙니다."
  warn "패치 적용(git apply)을 건너뜁니다. 수동으로 적용하세요."
  SKIP_PATCHES=true
else
  SKIP_PATCHES=false
fi

# ─── MCP 소스 파일 복사 ───────────────────────────────────────────────────────
MCP_TARGET="${CHROMIUM_SRC}/chrome/browser/mcp"

info "MCP 소스 파일을 복사합니다..."
info "  출처: ${SRC_DIR}"
info "  대상: ${MCP_TARGET}"

# 대상 디렉토리 생성
mkdir -p "${MCP_TARGET}/tools"

# 소스 파일 복사 (tools/ 서브디렉토리 포함)
if ! cp -r "${SRC_DIR}/." "${MCP_TARGET}/"; then
  error "소스 파일 복사에 실패했습니다."
  exit 1
fi

success "소스 파일 복사 완료 → ${MCP_TARGET}"

# ─── 패치 적용 ───────────────────────────────────────────────────────────────
if [[ "${SKIP_PATCHES}" == "true" ]]; then
  warn "Git 저장소 미감지로 패치 적용을 건너뜁니다."
  warn "아래 패치 파일을 수동으로 적용하세요:"
  for patch in "${PATCHES_DIR}"/*.patch; do
    [[ -f "${patch}" ]] && warn "  - ${patch}"
  done
else
  info "패치 파일을 적용합니다..."

  # 패치 적용 실패 여부 추적
  PATCH_FAILED=false

  # 패치 파일을 알파벳 순서로 적용 (적용 순서가 의존성에 영향을 줄 수 있음)
  for patch in $(ls "${PATCHES_DIR}"/*.patch 2>/dev/null | sort); do
    patch_name="$(basename "${patch}")"
    info "  적용 중: ${patch_name}"

    if (cd "${CHROMIUM_SRC}" && git apply --check "${patch}" 2>/dev/null); then
      # 드라이런 성공 → 실제 적용
      if (cd "${CHROMIUM_SRC}" && git apply "${patch}"); then
        success "  패치 적용 성공: ${patch_name}"
      else
        warn "  패치 적용 실패: ${patch_name}"
        PATCH_FAILED=true
      fi
    else
      warn "  패치 적용 불가 (이미 적용되었거나 충돌): ${patch_name}"
      warn "  수동 확인 필요: ${patch}"
      PATCH_FAILED=true
    fi
  done

  if [[ "${PATCH_FAILED}" == "true" ]]; then
    warn ""
    warn "일부 패치가 자동 적용되지 않았습니다."
    warn "patches/ 디렉토리의 .patch 파일을 참고하여 수동으로 변경사항을 적용하세요."
    warn "주요 변경 위치:"
    warn "  - chrome/browser/BUILD.gn       : deps에 '//chrome/browser/mcp' 추가"
    warn "  - chrome/app/chrome_main_delegate.cc : InitializeMcpIfNeeded() 호출 추가"
    warn "  - chrome/common/chrome_switches.h/cc : kMcpStdio, kMcpSocket 등록"
  else
    success "모든 패치 적용 완료"
  fi
fi

# ─── 완료 메시지 ─────────────────────────────────────────────────────────────
echo ""
success "=== chromium-mcp 설치 완료 ==="
echo ""
info "다음 단계:"
info "  빌드하려면: ./scripts/build.sh ${CHROMIUM_SRC}"
echo ""
