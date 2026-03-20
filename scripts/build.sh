#!/usr/bin/env bash
# build.sh — chromium-mcp가 통합된 Chromium을 빌드하는 스크립트
#
# 사용법:
#   ./scripts/build.sh <chromium_src_path> [gn_args...]
#
# 인자:
#   chromium_src_path : Chromium src/ 디렉토리 경로 (필수)
#   gn_args           : 추가 GN 인자 (선택, 여러 개 지정 가능)
#                       예) is_debug=true enable_nacl=false
#
# 수행 작업:
#   1. Chromium 소스 경로 확인
#   2. MCP 소스가 아직 설치되지 않은 경우 install.sh 자동 실행
#   3. GN으로 out/Default 빌드 디렉토리 설정
#   4. autoninja로 chrome 타겟 빌드
#   5. 빌드 결과물 경로 출력

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

사용법: $(basename "$0") <chromium_src_path> [gn_args...]

chromium-mcp MCP 서버가 통합된 Chromium을 빌드합니다.

인자:
  chromium_src_path   Chromium src/ 디렉토리의 절대 또는 상대 경로
  gn_args             추가 GN 빌드 인자 (선택)

예시:
  # 기본 빌드 (릴리스 모드)
  $(basename "$0") ~/chromium/src

  # 디버그 빌드
  $(basename "$0") ~/chromium/src is_debug=true

  # 컴포넌트 빌드 (빌드 속도 향상)
  $(basename "$0") ~/chromium/src is_component_build=true

빌드 결과물:
  <chromium_src_path>/out/Default/chrome

EOF
}

# ─── 인자 확인 ───────────────────────────────────────────────────────────────
if [[ $# -lt 1 ]]; then
  error "Chromium 소스 경로가 지정되지 않았습니다."
  usage
  exit 1
fi

CHROMIUM_SRC="$(realpath "$1")"
shift  # 첫 번째 인자(경로) 제거 — 나머지는 GN 인자로 처리

# 추가 GN 인자 수집
EXTRA_GN_ARGS=("$@")

# chromium-mcp 저장소 루트 (이 스크립트 위치 기준)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_SCRIPT="${REPO_ROOT}/scripts/install.sh"

# GN 빌드 출력 디렉토리
BUILD_DIR="${CHROMIUM_SRC}/out/Default"

# ─── 경로 유효성 검사 ─────────────────────────────────────────────────────────
if [[ ! -d "${CHROMIUM_SRC}" ]]; then
  error "디렉토리를 찾을 수 없습니다: ${CHROMIUM_SRC}"
  exit 1
fi

if [[ ! -d "${CHROMIUM_SRC}/chrome" ]] || [[ ! -d "${CHROMIUM_SRC}/content" ]]; then
  error "'${CHROMIUM_SRC}'는 유효한 Chromium src/ 디렉토리가 아닙니다."
  exit 1
fi

# ─── 필수 도구 확인 ───────────────────────────────────────────────────────────
info "필수 빌드 도구를 확인합니다..."

# gn 확인 (GN 빌드 시스템)
if ! command -v gn &>/dev/null; then
  # Chromium depot_tools 경로에서 찾기 시도
  if [[ -x "${CHROMIUM_SRC}/../depot_tools/gn" ]]; then
    export PATH="${CHROMIUM_SRC}/../depot_tools:${PATH}"
    success "depot_tools에서 gn을 찾았습니다."
  else
    error "gn을 찾을 수 없습니다."
    error "depot_tools를 설치하고 PATH에 추가하세요:"
    error "  https://commondatastorage.googleapis.com/chrome-infra-docs/flat/depot_tools/docs/html/depot_tools_tutorial.html"
    exit 1
  fi
else
  success "gn 확인: $(command -v gn)"
fi

# autoninja 또는 ninja 확인
if command -v autoninja &>/dev/null; then
  NINJA_CMD="autoninja"
  success "빌드 도구: autoninja"
elif command -v ninja &>/dev/null; then
  NINJA_CMD="ninja"
  warn "autoninja를 찾을 수 없어 ninja를 사용합니다."
  warn "빌드 속도 향상을 위해 depot_tools의 autoninja 사용을 권장합니다."
else
  error "ninja 또는 autoninja를 찾을 수 없습니다."
  error "depot_tools를 설치하세요."
  exit 1
fi

# ─── MCP 소스 설치 확인 ───────────────────────────────────────────────────────
MCP_TARGET="${CHROMIUM_SRC}/chrome/browser/mcp"

if [[ ! -d "${MCP_TARGET}" ]]; then
  info "MCP 소스가 설치되지 않았습니다. install.sh를 실행합니다..."

  if [[ ! -x "${INSTALL_SCRIPT}" ]]; then
    error "install.sh를 찾을 수 없거나 실행 권한이 없습니다: ${INSTALL_SCRIPT}"
    exit 1
  fi

  if ! "${INSTALL_SCRIPT}" "${CHROMIUM_SRC}"; then
    error "install.sh 실행에 실패했습니다."
    exit 1
  fi
else
  # BUILD.gn 존재 여부로 설치 완료 여부 재확인
  if [[ ! -f "${MCP_TARGET}/BUILD.gn" ]]; then
    warn "MCP 디렉토리는 존재하지만 BUILD.gn이 없습니다. install.sh를 다시 실행합니다..."
    "${INSTALL_SCRIPT}" "${CHROMIUM_SRC}" || true
  else
    success "MCP 소스가 이미 설치되어 있습니다: ${MCP_TARGET}"
  fi
fi

# ─── GN 빌드 디렉토리 설정 ────────────────────────────────────────────────────
info "GN 빌드 설정을 생성합니다..."
info "  빌드 디렉토리: ${BUILD_DIR}"

# 기본 GN 인자 구성
# MCP 관련 플래그 활성화 + 기본 릴리스 설정
DEFAULT_GN_ARGS=(
  "is_debug=false"
  "is_component_build=false"
  "symbol_level=0"          # 심볼 제거로 빌드 속도 향상
  "enable_nacl=false"       # NaCl 비활성화로 빌드 시간 단축
)

# 사용자 지정 GN 인자를 기본값 뒤에 추가 (오버라이드 허용)
ALL_GN_ARGS=("${DEFAULT_GN_ARGS[@]}" "${EXTRA_GN_ARGS[@]}")

# GN 인자를 공백으로 구분된 단일 문자열로 변환
GN_ARGS_STR="${ALL_GN_ARGS[*]}"

info "  GN 인자: ${GN_ARGS_STR}"

# GN 설정 생성
if ! (cd "${CHROMIUM_SRC}" && gn gen "${BUILD_DIR}" --args="${GN_ARGS_STR}"); then
  error "GN 설정 생성에 실패했습니다."
  error "GN 인자를 확인하고 다시 시도하세요."
  exit 1
fi

success "GN 설정 완료"

# ─── 빌드 실행 ───────────────────────────────────────────────────────────────
info "Chromium 빌드를 시작합니다..."
info "  타겟: chrome"
info "  명령: ${NINJA_CMD} -C ${BUILD_DIR} chrome"
info ""
info "빌드는 처음 실행 시 수 시간이 걸릴 수 있습니다."
info "증분 빌드는 변경된 파일만 재컴파일하여 훨씬 빠릅니다."
echo ""

BUILD_START=$(date +%s)

if ! (cd "${CHROMIUM_SRC}" && ${NINJA_CMD} -C "${BUILD_DIR}" chrome); then
  error "빌드에 실패했습니다."
  error "위의 오류 메시지를 확인하세요."
  exit 1
fi

BUILD_END=$(date +%s)
BUILD_DURATION=$((BUILD_END - BUILD_START))
BUILD_MINUTES=$((BUILD_DURATION / 60))
BUILD_SECONDS=$((BUILD_DURATION % 60))

echo ""
success "=== Chromium 빌드 완료 ==="
echo ""
info "빌드 시간: ${BUILD_MINUTES}분 ${BUILD_SECONDS}초"
echo ""

# ─── 빌드 결과물 경로 출력 ────────────────────────────────────────────────────
info "빌드 결과물:"

# 플랫폼별 실행파일 이름 결정
case "$(uname -s)" in
  Darwin)
    # macOS: .app 번들 형식
    CHROME_BIN="${BUILD_DIR}/Chromium.app/Contents/MacOS/Chromium"
    CHROME_APP="${BUILD_DIR}/Chromium.app"
    if [[ -d "${CHROME_APP}" ]]; then
      success "  앱 번들:   ${CHROME_APP}"
      success "  실행파일:  ${CHROME_BIN}"
    fi
    ;;
  Linux)
    CHROME_BIN="${BUILD_DIR}/chrome"
    if [[ -f "${CHROME_BIN}" ]]; then
      success "  실행파일:  ${CHROME_BIN}"
    fi
    ;;
  *)
    warn "알 수 없는 플랫폼입니다. 결과물을 수동으로 확인하세요: ${BUILD_DIR}"
    ;;
esac

# ─── macOS 코드 서명 ─────────────────────────────────────────────────────────
if [[ "$(uname -s)" == "Darwin" ]] && [[ -d "${CHROME_APP}" ]]; then
  CODESIGN_IDENTITY="${CHROMIUM_CODESIGN_IDENTITY:-Chromium Dev}"
  if security find-identity -v -p codesigning | grep -q "${CODESIGN_IDENTITY}"; then
    info "코드 서명 중... (${CODESIGN_IDENTITY})"
    if codesign --force --deep -s "${CODESIGN_IDENTITY}" "${CHROME_APP}" 2>&1; then
      success "코드 서명 완료"
    else
      warn "코드 서명 실패 — 키체인 접근을 확인하세요."
    fi
  else
    warn "코드 서명 인증서 '${CODESIGN_IDENTITY}'를 찾을 수 없습니다."
    warn "키체인 접근 앱에서 자체 서명 인증서를 생성하세요."
    warn "또는 환경변수로 지정: CHROMIUM_CODESIGN_IDENTITY=\"인증서 이름\""
  fi
fi

echo ""
info "MCP 서버 테스트:"
info "  stdio 모드: ${CHROME_BIN:-${BUILD_DIR}/chrome} --mcp-stdio --headless"
info "  소켓 모드:  ${CHROME_BIN:-${BUILD_DIR}/chrome} --mcp-socket=/tmp/mcp.sock --headless"
info "  환경변수:   CHROMIUM_MCP=1 ${CHROME_BIN:-${BUILD_DIR}/chrome} --headless"
echo ""
