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

# ─── 패치 적용 (직접 코드 삽입 방식) ──────────────────────────────────────────
# 패치 섹션에서는 개별 실패를 경고로 처리하므로 set -e를 임시 해제
set +e
info "Chromium 소스에 MCP 통합 코드를 삽입합니다..."

PATCH_FAILED=false

# --- 1. chrome/BUILD.gn : public_deps에 "//chrome/browser/mcp" 추가 ---
# chrome/browser/BUILD.gn이 아닌 chrome/BUILD.gn에 추가하여 순환 의존 방지
CHROME_GN="${CHROMIUM_SRC}/chrome/BUILD.gn"
if [[ -f "${CHROME_GN}" ]]; then
  if grep -q '//chrome/browser/mcp' "${CHROME_GN}"; then
    success "  chrome/BUILD.gn: MCP 의존성 이미 존재"
  else
    if grep -q '"//chrome/browser",' "${CHROME_GN}"; then
      awk 'BEGIN{done=0} /\"\/\/chrome\/browser\",/ && done==0 {
        print; print "    \"//chrome/browser/mcp\","; done=1; next
      } {print}' "${CHROME_GN}" > "${CHROME_GN}.tmp" && mv "${CHROME_GN}.tmp" "${CHROME_GN}"
      success "  chrome/BUILD.gn: MCP 의존성 추가 완료"
    else
      warn "  chrome/BUILD.gn: '//chrome/browser' 패턴을 찾을 수 없습니다"
      PATCH_FAILED=true
    fi
  fi
else
  error "  chrome/BUILD.gn 파일을 찾을 수 없습니다"
  PATCH_FAILED=true
fi

# --- 2. chrome/common/chrome_switches.h : kMcpStdio, kMcpSocket 선언 ---
SWITCHES_H="${CHROMIUM_SRC}/chrome/common/chrome_switches.h"
if [[ -f "${SWITCHES_H}" ]]; then
  if grep -q 'kMcpStdio' "${SWITCHES_H}"; then
    success "  chrome_switches.h: MCP 스위치 이미 존재"
  else
    # "}  // namespace switches" 앞에 삽입
    sed -i '' '/}  \/\/ namespace switches/i\
\
// MCP(Model Context Protocol) 서버 스위치 (chromium-mcp)\
extern const char kMcpStdio[];\
extern const char kMcpSocket[];
' "${SWITCHES_H}"
    success "  chrome_switches.h: MCP 스위치 선언 추가 완료"
  fi
else
  error "  chrome_switches.h 파일을 찾을 수 없습니다"
  PATCH_FAILED=true
fi

# --- 3. chrome/common/chrome_switches.cc : kMcpStdio, kMcpSocket 정의 ---
SWITCHES_CC="${CHROMIUM_SRC}/chrome/common/chrome_switches.cc"
if [[ -f "${SWITCHES_CC}" ]]; then
  if grep -q 'kMcpStdio' "${SWITCHES_CC}"; then
    success "  chrome_switches.cc: MCP 스위치 이미 존재"
  else
    sed -i '' '/}  \/\/ namespace switches/i\
\
// MCP(Model Context Protocol) 서버 스위치 정의 (chromium-mcp)\
const char kMcpStdio[] = "mcp-stdio";\
const char kMcpSocket[] = "mcp-socket";
' "${SWITCHES_CC}"
    success "  chrome_switches.cc: MCP 스위치 정의 추가 완료"
  fi
else
  error "  chrome_switches.cc 파일을 찾을 수 없습니다"
  PATCH_FAILED=true
fi

# --- 4. chrome/app/chrome_main_delegate.cc : MCP 초기화 코드 삽입 ---
DELEGATE_CC="${CHROMIUM_SRC}/chrome/app/chrome_main_delegate.cc"
if [[ -f "${DELEGATE_CC}" ]]; then
  if grep -q 'InitializeMcpIfNeeded' "${DELEGATE_CC}"; then
    success "  chrome_main_delegate.cc: MCP 초기화 코드 이미 존재"
  else
    # 4a. #include 추가 — corresponding header 뒤, C 헤더 블록에 <cstdlib>, 프로젝트 헤더에 mcp_server.h
    # <cstdlib>은 기존 C 시스템 헤더 근처에 삽입
    if ! grep -q '<cstdlib>' "${DELEGATE_CC}"; then
      sed -i '' '/#include <stddef.h>/a\
#include <cstdlib>
' "${DELEGATE_CC}"
    fi
    # mcp_server.h는 다른 chrome/browser/ 헤더 근처에 삽입
    if ! grep -q 'mcp_server.h' "${DELEGATE_CC}"; then
      sed -i '' '/#include "chrome\/app\/chrome_main_delegate.h"/a\
#include "chrome/browser/mcp/mcp_server.h"
' "${DELEGATE_CC}"
    fi

    # 4b. InitializeMcpIfNeeded() 함수 정의 — 첫 번째 익명 namespace 닫는 줄 앞에 삽입
    # awk로 첫 매칭만 처리 (macOS 호환)
    awk '
      /^}  \/\/ namespace$/ && !func_inserted {
        print ""
        print "void InitializeMcpIfNeeded() {"
        print "  auto* cmd = base::CommandLine::ForCurrentProcess();"
        print "  bool mcp_stdio = cmd->HasSwitch(\"mcp-stdio\");"
        print "  bool mcp_env = (!!getenv(\"CHROMIUM_MCP\"));"
        print "  std::string socket_path = cmd->GetSwitchValueASCII(\"mcp-socket\");"
        print "  bool mcp_socket = !socket_path.empty();"
        print "  if (!mcp_stdio && !mcp_env && !mcp_socket) return;"
        print "  cmd->RemoveSwitch(\"mcp-stdio\");"
        print "  cmd->RemoveSwitch(\"mcp-socket\");"
        print "  unsetenv(\"CHROMIUM_MCP\");"
        print "  auto* server = mcp::McpServer::GetInstance();"
        print "  if (mcp_stdio || mcp_env) {"
        print "    server->StartWithStdio();"
        print "  } else {"
        print "    server->StartWithSocket(socket_path);"
        print "  }"
        print "}"
        print ""
        func_inserted = 1
      }
      { print }
    ' "${DELEGATE_CC}" > "${DELEGATE_CC}.tmp" && mv "${DELEGATE_CC}.tmp" "${DELEGATE_CC}"

    # 4c. PostEarlyInitialization 함수 내 return std::nullopt; 앞에 MCP 호출 삽입
    # PostEarlyInitialization 함수를 찾고, 그 안의 마지막 return std::nullopt; 앞에 삽입
    awk '
      /ChromeMainDelegate::PostEarlyInitialization/ { in_target_func = 1 }
      in_target_func && /return std::nullopt;/ { last_return_line = NR }
      in_target_func && /^}/ { in_target_func = 0 }
      { lines[NR] = $0 }
      END {
        for (i = 1; i <= NR; i++) {
          if (i == last_return_line) {
            print "  // MCP 서버 조건부 초기화 (chromium-mcp)"
            print "  InitializeMcpIfNeeded();"
            print ""
          }
          print lines[i]
        }
      }
    ' "${DELEGATE_CC}" > "${DELEGATE_CC}.tmp" && mv "${DELEGATE_CC}.tmp" "${DELEGATE_CC}"

    # 삽입 검증
    if grep -q 'InitializeMcpIfNeeded' "${DELEGATE_CC}"; then
      success "  chrome_main_delegate.cc: MCP 초기화 코드 삽입 완료"
    else
      warn "  chrome_main_delegate.cc: MCP 초기화 코드 삽입 검증 실패"
      PATCH_FAILED=true
    fi
  fi
else
  error "  chrome_main_delegate.cc 파일을 찾을 수 없습니다"
  PATCH_FAILED=true
fi

if [[ "${PATCH_FAILED}" == "true" ]]; then
  warn ""
  warn "일부 패치가 자동 적용되지 않았습니다. 수동 확인 필요:"
  warn "  - chrome/browser/BUILD.gn       : deps에 '//chrome/browser/mcp' 추가"
  warn "  - chrome/app/chrome_main_delegate.cc : InitializeMcpIfNeeded() 호출 추가"
  warn "  - chrome/common/chrome_switches.h/cc : kMcpStdio, kMcpSocket 등록"
else
  success "모든 MCP 통합 코드 삽입 완료"
fi

# set -e 복원
set -e

# ─── 완료 메시지 ─────────────────────────────────────────────────────────────
echo ""
success "=== chromium-mcp 설치 완료 ==="
echo ""
info "다음 단계:"
info "  빌드하려면: ./scripts/build.sh ${CHROMIUM_SRC}"
echo ""
