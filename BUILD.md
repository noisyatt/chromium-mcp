# Chromium 빌드 가이드

## 시스템 요구사항

### macOS

| 항목 | 요구사항 |
|------|---------|
| OS | macOS 12+ (Monterey 이상) |
| Xcode | 15.0+ |
| 디스크 | **최소 100GB** (소스 ~40GB + 빌드 ~60GB) |
| RAM | 16GB+ (32GB 권장) |
| CPU | Apple Silicon 또는 Intel (Apple Silicon 빌드 시간 ~1시간, Intel ~2-3시간) |

### 필수 도구

```bash
# Xcode Command Line Tools
xcode-select --install

# depot_tools (Chromium 빌드 시스템)
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$PATH:$(pwd)/depot_tools"
```

## 소스 가져오기

```bash
mkdir ~/chromium && cd ~/chromium

# 소스 가져오기 (약 30-60분)
fetch --nohooks chromium

# 의존성 설치
cd src
gclient runhooks
```

### 특정 버전 기반으로 작업

```bash
# 안정 채널 최신 버전 확인
git tag -l '1*' | sort -V | tail -5

# 특정 버전 체크아웃 (예: 134.0.6998.0)
git checkout tags/134.0.6998.0 -b chromium-mcp

# 의존성 동기화
gclient sync --with_branch_heads --with_tags
```

## 빌드 설정

### GN 설정 파일 생성

```bash
gn gen out/Default --args='
  is_debug = false
  is_component_build = false
  symbol_level = 0
  blink_symbol_level = 0
  target_cpu = "arm64"
  chrome_pgo_phase = 0
  proprietary_codecs = true
  ffmpeg_branding = "Chrome"
  enable_nacl = false
  enable_widevine = true
  is_official_build = false
'
```

#### 빌드 인자 설명

| 인자 | 값 | 설명 |
|------|---|------|
| `is_debug` | `false` | 릴리즈 빌드 (속도 최적화) |
| `target_cpu` | `"arm64"` | Apple Silicon용. Intel은 `"x64"` |
| `proprietary_codecs` | `true` | H.264/AAC 등 코덱 포함 |
| `enable_widevine` | `true` | DRM 콘텐츠 재생 (넷플릭스 등) |
| `symbol_level` | `0` | 디버그 심볼 제거 (빌드 크기 축소) |

### 커스텀 MCP 모듈 추가

`BUILD.gn` 수정이 필요한 파일들:

```
chrome/browser/BUILD.gn          ← mcp/ 디렉토리 추가
chrome/browser/mcp/BUILD.gn      ← MCP 모듈 빌드 정의 (신규)
chrome/app/chrome_main_delegate.cc ← MCP 서버 초기화 코드 추가
```

#### chrome/browser/mcp/BUILD.gn (신규)

```python
source_set("mcp") {
  sources = [
    "mcp_server.cc",
    "mcp_server.h",
    "mcp_session.cc",
    "mcp_session.h",
    "mcp_tool_registry.cc",
    "mcp_tool_registry.h",
    "mcp_transport_stdio.cc",
    "mcp_transport_stdio.h",
    "mcp_transport_socket.cc",
    "mcp_transport_socket.h",
    "tools/navigate_tool.cc",
    "tools/screenshot_tool.cc",
    "tools/network_tool.cc",
    "tools/dom_tool.cc",
    "tools/javascript_tool.cc",
    "tools/tab_tool.cc",
    "tools/page_content_tool.cc",
  ]

  deps = [
    "//base",
    "//content/public/browser",
    "//content/browser/devtools:devtools",
    "//third_party/inspector_protocol:crdtp",
  ]
}
```

## 빌드 실행

```bash
# 전체 빌드 (초회: 1-3시간)
autoninja -C out/Default chrome

# MCP 모듈만 빌드 (수정 후 증분 빌드: 수 분)
autoninja -C out/Default chrome/browser/mcp:mcp
```

### 빌드 속도 최적화

```bash
# ccache 사용 (재빌드 속도 대폭 향상)
brew install ccache
export CCACHE_CPP2=yes
export CCACHE_SLOPPINESS=time_macros

# GN 설정에 추가
# cc_wrapper = "ccache"
```

## 실행 및 테스트

```bash
# 일반 실행
./out/Default/Chromium.app/Contents/MacOS/Chromium

# MCP stdio 모드
./out/Default/Chromium.app/Contents/MacOS/Chromium --mcp-stdio

# MCP socket 모드
./out/Default/Chromium.app/Contents/MacOS/Chromium --mcp-socket=/tmp/.chromium-mcp.sock

# 환경변수 방식 (프로세스 인자에 안 보임)
CHROMIUM_MCP=1 ./out/Default/Chromium.app/Contents/MacOS/Chromium
```

## 앱 번들 패키징

```bash
# macOS .app 번들 생성
autoninja -C out/Default chrome/installer/mac

# 결과물
ls out/Default/Chromium.app
```

### 기본 브라우저 등록

`out/Default/Chromium.app/Contents/Info.plist`에 이미 HTTP/HTTPS 스킴 핸들러가 선언되어 있다:

```xml
<key>CFBundleURLTypes</key>
<array>
  <dict>
    <key>CFBundleURLSchemes</key>
    <array>
      <string>http</string>
      <string>https</string>
    </array>
  </dict>
</array>
```

앱을 `/Applications`로 복사한 뒤 시스템 설정 → 기본 브라우저에서 선택 가능.

## 업데이트 관리

```bash
# upstream Chromium 변경사항 가져오기
git fetch origin
git rebase origin/main chromium-mcp

# 충돌 해결 후
gclient sync
autoninja -C out/Default chrome
```

### 권장 업데이트 주기

- **보안 패치**: Chromium stable 릴리즈 따라 즉시 (2-4주마다)
- **기능 업데이트**: 필요 시
- MCP 코드가 `chrome/browser/mcp/`에 격리되어 있으므로 rebase 충돌 최소화
