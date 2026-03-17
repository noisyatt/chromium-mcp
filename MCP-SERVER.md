# 내장 MCP 서버 설계 명세

## MCP 프로토콜 개요

MCP(Model Context Protocol)는 AI 클라이언트와 도구 서버 간의 JSON-RPC 2.0 기반 프로토콜이다.

```json
// 요청
{"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "navigate", "arguments": {"url": "https://example.com"}}}

// 응답
{"jsonrpc": "2.0", "id": 1, "result": {"content": [{"type": "text", "text": "페이지 로드 완료"}]}}
```

## 서버 라이프사이클

```
브라우저 시작
  │
  ├─ MCP 플래그 감지? ──No──► 일반 브라우저로 동작
  │
  Yes
  │
  ▼
McpServer::Initialize()
  │
  ├─ stdio 모드: stdin/stdout 스레드 시작
  └─ socket 모드: Unix socket 리스닝 시작
  │
  ▼
MCP handshake (initialize → initialized)
  │
  ▼
도구 호출 대기 루프
  │
  ▼
브라우저 종료 시 McpServer::Shutdown()
```

## 도구 목록

### 페이지 제어

#### `navigate`
```json
{
  "name": "navigate",
  "description": "URL로 이동하거나 뒤로/앞으로 탐색",
  "inputSchema": {
    "type": "object",
    "properties": {
      "url": {"type": "string", "description": "이동할 URL"},
      "action": {"type": "string", "enum": ["url", "back", "forward", "reload"]}
    }
  }
}
```

내부 구현: `DevToolsAgentHost` → `Page.navigate` CDP 명령

#### `screenshot`
```json
{
  "name": "screenshot",
  "description": "현재 페이지 또는 특정 요소의 스크린샷",
  "inputSchema": {
    "type": "object",
    "properties": {
      "fullPage": {"type": "boolean", "default": false},
      "selector": {"type": "string", "description": "CSS 선택자 (특정 요소만)"},
      "format": {"type": "string", "enum": ["png", "jpeg"], "default": "png"}
    }
  }
}
```

내부 구현: `Page.captureScreenshot` CDP 명령

#### `page_content`
```json
{
  "name": "page_content",
  "description": "페이지의 접근성 트리 또는 HTML 반환",
  "inputSchema": {
    "type": "object",
    "properties": {
      "mode": {"type": "string", "enum": ["accessibility", "html", "text"]},
      "selector": {"type": "string", "description": "특정 요소만 (CSS 선택자)"}
    }
  }
}
```

내부 구현:
- `accessibility` 모드: `Accessibility.getFullAXTree` CDP 명령
- `html` 모드: `DOM.getOuterHTML` CDP 명령
- `text` 모드: `Runtime.evaluate` + `document.body.innerText`

### DOM 조작

#### `click`
```json
{
  "name": "click",
  "description": "요소 클릭",
  "inputSchema": {
    "type": "object",
    "properties": {
      "selector": {"type": "string", "description": "CSS 선택자"},
      "ref": {"type": "string", "description": "접근성 트리의 ref ID"},
      "button": {"type": "string", "enum": ["left", "right", "middle"]},
      "waitForNavigation": {"type": "boolean", "default": false}
    }
  }
}
```

#### `fill`
```json
{
  "name": "fill",
  "description": "입력 필드에 값 입력",
  "inputSchema": {
    "type": "object",
    "properties": {
      "selector": {"type": "string"},
      "ref": {"type": "string"},
      "value": {"type": "string"}
    }
  }
}
```

#### `evaluate`
```json
{
  "name": "evaluate",
  "description": "JavaScript 코드 실행",
  "inputSchema": {
    "type": "object",
    "properties": {
      "expression": {"type": "string", "description": "실행할 JS 코드"},
      "awaitPromise": {"type": "boolean", "default": true}
    },
    "required": ["expression"]
  }
}
```

내부 구현: `Runtime.evaluate` CDP 명령. 중요: `Runtime.enable`을 호출하지 않고 `Runtime.evaluate`만 직접 호출하여 탐지 신호 최소화.

### 네트워크

#### `network_capture`
```json
{
  "name": "network_capture",
  "description": "네트워크 요청 캡처 시작/중지",
  "inputSchema": {
    "type": "object",
    "properties": {
      "action": {"type": "string", "enum": ["start", "stop"]},
      "includeResponseBody": {"type": "boolean", "default": false},
      "filter": {
        "type": "object",
        "properties": {
          "urlPattern": {"type": "string"},
          "resourceTypes": {"type": "array", "items": {"type": "string"}}
        }
      }
    },
    "required": ["action"]
  }
}
```

내부 구현:
- `Network.enable` → 모든 요청/응답 이벤트 수신
- `Network.getResponseBody` → 응답 본문 획득
- **핵심 차이**: 확장의 `chrome.debugger`와 달리 **노란 배너 없음** (내부 호출이므로)

#### `network_requests`
```json
{
  "name": "network_requests",
  "description": "현재까지 캡처된 네트워크 요청 목록 반환",
  "inputSchema": {
    "type": "object",
    "properties": {
      "includeStatic": {"type": "boolean", "default": false}
    }
  }
}
```

### 탭 관리

#### `tabs`
```json
{
  "name": "tabs",
  "description": "탭 목록 조회, 생성, 닫기, 전환",
  "inputSchema": {
    "type": "object",
    "properties": {
      "action": {"type": "string", "enum": ["list", "new", "close", "select"]},
      "tabId": {"type": "number"},
      "url": {"type": "string"}
    },
    "required": ["action"]
  }
}
```

내부 구현: `TabStripModel` API 직접 호출 (CDP 경유 불필요)

### 브라우저 정보

#### `browser_info`
```json
{
  "name": "browser_info",
  "description": "브라우저 버전, 활성 탭 등 정보 반환",
  "inputSchema": {
    "type": "object",
    "properties": {}
  }
}
```

## 내부 구현 핵심

### CDP 직접 호출 vs 외부 CDP

```
외부 CDP (기존 방식):
  AI → WebSocket → DevToolsHttpHandler → DevToolsAgentHost → Renderer
  문제: WebSocket 포트 노출, 디버그 플래그 필요

내부 CDP (Chromium-MCP):
  MCP 서버 → DevToolsAgentHost::CreateSession() → Renderer
  장점: 네트워크 노출 없음, 플래그 최소화
```

### Runtime.enable 회피

봇 탐지의 핵심 신호인 `Runtime.enable`을 회피하는 전략:

```cpp
// 방법 1: Runtime.evaluate만 직접 호출 (Runtime.enable 없이)
// Chromium 내부에서는 Runtime 도메인을 enable하지 않고도
// evaluate를 호출할 수 있다 (외부 CDP에서는 불가능)

void McpJavaScriptTool::Execute(const std::string& expression) {
  auto* host = GetDevToolsAgentHost();
  // Runtime.enable을 보내지 않고 직접 evaluate
  host->GetSession()->SendRawCommand(
    "Runtime.evaluate",
    R"({"expression": ")" + expression + R"(", "returnByValue": true})"
  );
}
```

```cpp
// 방법 2: Isolated World에서 실행
// 페이지 JS 컨텍스트와 완전히 분리된 공간에서 실행
void McpJavaScriptTool::ExecuteIsolated(const std::string& expression) {
  auto* host = GetDevToolsAgentHost();
  // Page.createIsolatedWorld로 별도 컨텍스트 생성
  // 이 컨텍스트에서의 Runtime 활동은 페이지 JS에서 감지 불가
}
```

### 네트워크 캡처 (배너 없음)

확장의 `chrome.debugger`는 브라우저 UI를 통해 연결하므로 노란 배너가 표시된다. 내부 MCP 서버는 `DevToolsAgentHost`에 직접 세션을 생성하므로:

1. 배너 표시 코드가 트리거되지 않음 (배너는 Extension API 레이어에서 표시)
2. 응답 body를 포함한 전체 네트워크 데이터 접근 가능
3. `navigator.webdriver` 플래그 미설정

## 설정 파일

```json
// ~/.chromium-mcp/config.json
{
  "transport": "stdio",
  "socket_path": "/tmp/.chromium-mcp.sock",
  "auth_token": null,
  "allowed_tools": ["*"],
  "auto_start": false,
  "max_response_body_size": 10485760,
  "log_level": "warn"
}
```

| 항목 | 기본값 | 설명 |
|------|--------|------|
| `transport` | `"stdio"` | 전송 방식 (`stdio`, `socket`) |
| `auth_token` | `null` | 인증 토큰 (null이면 비활성) |
| `allowed_tools` | `["*"]` | 허용할 도구 목록 |
| `auto_start` | `false` | 브라우저 시작 시 자동으로 MCP 서버 시작 |
| `max_response_body_size` | 10MB | 네트워크 응답 body 최대 크기 |
