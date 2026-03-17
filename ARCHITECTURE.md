# 아키텍처 설계

## 개요

Chromium 브라우저 프로세스 내부에 MCP 서버를 직접 임베딩한다. 기존 CDP(Chrome DevTools Protocol) 인프라를 재사용하되, 외부에 노출되는 네트워크 포트나 프로세스 플래그 없이 동작한다.

## Chromium 프로세스 구조 (기존)

```
Browser Process (메인)
├── GPU Process
├── Network Service
├── Renderer Process (탭당 1개+)
│   ├── Blink (렌더링)
│   └── V8 (JavaScript)
├── Extension Process
└── DevToolsHttpHandler (← 여기가 --remote-debugging-port)
```

## 수정 대상

### 1. MCP 서버 모듈 추가 위치

```
chrome/browser/
├── mcp/                          ← 신규 디렉토리
│   ├── mcp_server.h/cc           ← MCP 서버 코어
│   ├── mcp_session.h/cc          ← 클라이언트 세션 관리
│   ├── mcp_tool_registry.h/cc    ← MCP 도구 등록
│   ├── mcp_transport_stdio.h/cc  ← stdio 전송 계층
│   ├── mcp_transport_socket.h/cc ← Unix socket 전송 계층
│   ├── tools/                    ← MCP 도구 구현
│   │   ├── navigate_tool.cc
│   │   ├── screenshot_tool.cc
│   │   ├── network_tool.cc
│   │   ├── dom_tool.cc
│   │   ├── javascript_tool.cc
│   │   ├── tab_tool.cc
│   │   └── page_content_tool.cc
│   └── BUILD.gn
```

### 2. CDP 인프라 재사용

Chromium 내부에는 이미 CDP 명령을 처리하는 완전한 인프라가 있다:

```cpp
// 기존 CDP 인프라 (content/browser/devtools/)
DevToolsAgentHost        → 탭/프레임에 대한 디버그 세션
DevToolsSession          → CDP 명령 송수신
protocol::DOMHandler     → DOM 조작
protocol::NetworkHandler → 네트워크 감시
protocol::RuntimeHandler → JS 실행
protocol::PageHandler    → 페이지 제어
```

MCP 서버는 이 CDP 인프라를 **내부 IPC로 직접 호출**한다. 외부에 포트를 여는 `DevToolsHttpHandler`를 우회하고, `DevToolsAgentHost`에 직접 세션을 연결한다.

```cpp
// 핵심 연결 방식 (의사코드)
class McpServer {
  void AttachToTab(content::WebContents* web_contents) {
    auto agent_host = content::DevToolsAgentHost::GetOrCreateFor(web_contents);
    auto session = agent_host->CreateSession(this, /*restricted=*/false);
    // 이제 CDP 명령을 직접 보낼 수 있음
    session->SendProtocolCommand("Network.enable", params);
  }
};
```

### 3. 전송 계층 (Transport)

#### stdio 모드 (기본)

브라우저를 특정 플래그로 실행하면 stdin/stdout으로 MCP JSON-RPC를 주고받는다:

```bash
# 기존 디버그 모드 (탐지됨)
chrome --remote-debugging-port=9222

# Chromium-MCP 방식 (탐지 안 됨)
chromium-mcp --mcp-stdio
```

차이점:
- `--remote-debugging-port`는 TCP 소켓을 열어 외부에서 접근 가능
- `--mcp-stdio`는 부모 프로세스의 stdin/stdout만 사용, 네트워크 노출 없음

**은닉 강화**: 빌드 시 `--mcp-stdio` 플래그 이름을 커스텀 문자열로 변경 가능. 또는 플래그 없이 환경변수(`CHROMIUM_MCP=1`)로 활성화.

#### Unix socket 모드

```bash
chromium-mcp --mcp-socket=/tmp/.chromium-mcp.sock
```

- 파일시스템 권한으로 접근 제어 (소유자만 접속 가능)
- 소켓 파일명을 숨김 파일로 설정
- `netstat`/`lsof`에 TCP 포트로 안 잡힘

### 4. 프로세스 인자 은닉

```cpp
// chrome/app/chrome_main.cc에서 처리 후 argv에서 제거
int main(int argc, char** argv) {
  // MCP 관련 인자 파싱
  bool mcp_enabled = HasSwitch("mcp-stdio") || getenv("CHROMIUM_MCP");

  // argv에서 MCP 관련 인자 제거 → ps aux에 안 보임
  RemoveSwitchFromCommandLine("mcp-stdio");
  RemoveSwitchFromCommandLine("mcp-socket");

  // 이후 일반 Chrome 시작 흐름
  return ChromeMain(argc, argv);
}
```

## 데이터 흐름

### MCP 도구 호출 시

```
AI Client
  │
  │ JSON-RPC (stdio)
  ▼
McpTransportStdio::OnInput()
  │
  │ MCP 프로토콜 파싱
  ▼
McpServer::HandleToolCall("navigate", {url: "..."})
  │
  │ 내부 CDP 호출 (IPC, 네트워크 없음)
  ▼
DevToolsAgentHost::SendProtocolCommand("Page.navigate", ...)
  │
  │ Blink IPC
  ▼
Renderer Process → 실제 페이지 로드
```

### 네트워크 캡처 시

```
McpServer::EnableNetworkCapture()
  │
  │ CDP "Network.enable" (내부 호출)
  ▼
NetworkHandler → 모든 요청/응답 이벤트 수신
  │
  │ 이벤트 버퍼링
  ▼
McpServer::OnNetworkEvent()
  │
  │ MCP 응답으로 변환
  ▼
AI Client ← JSON-RPC 응답
```

## 보안 모델

| 항목 | 설계 |
|------|------|
| 접근 제어 | stdio: 부모 프로세스만 / socket: 파일 권한 (0600) |
| 인증 | 선택적 토큰 기반 (환경변수로 전달) |
| 도구 제한 | 설정 파일로 허용 도구 화이트리스트 |
| 데이터 범위 | 활성 탭만 / 전체 탭 설정 가능 |

## 기존 Chrome 기능 영향

MCP 서버 내장이 브라우저 기능에 미치는 영향:

| 기능 | 영향 |
|------|------|
| 웹 브라우징 | 없음 |
| 확장 프로그램 | 없음 (공존 가능) |
| 북마크/히스토리 | 없음 |
| DevTools (F12) | 없음 (독립적으로 동작) |
| 자동 업데이트 | 자체 업데이트 채널 필요 |
| DRM (Widevine) | 별도 통합 필요 |
| 동기화 | 자체 구현 필요 (Google 동기화 불가) |
