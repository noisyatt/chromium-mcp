# MCP 리팩토링 계획 (v2 — 크로스체크 반영)

## Context

Chromium-MCP가 멀티클라이언트 소켓을 지원하게 되었지만, 핵심 구조에 두 가지 근본 결함이 있다:

1. **McpServer**: `active_web_contents_` 전역 단일 포인터로 인해 모든 클라이언트가 같은 탭을 공유
2. **데몬**: `ensure_chromium()`에 소유권 없이 아무 스레드나 Chromium 시작 가능 → race condition

사용자 요구사항:
- 기존 유저 탭/창 접근 가능
- Claude 세션별 독립 탭 배정
- MCP 호출/브라우저 조작 오류 원천 차단

---

## 크로스체크에서 발견된 핵심 이슈 (v1 대비 변경점)

| # | 이슈 | 심각도 | 조치 |
|---|------|--------|------|
| 1 | ToolExecutionContext는 현 시점 과설계 (YAGNI) | 중 | **Phase A-2 삭제**, client_id는 클로저 캡처로 전달 |
| 2 | requires_session()=false 즉시 가능 3개, bookmark/history는 내부 수정 필요 | 중 | 5개→3개로 축소, bookmark/history는 Phase 2에서 처리 |
| 3 | current_client_id_ 멤버 패턴 비동기 안전하지 않음 | 높 | **폐기**, 기존 클로저 캡처 패턴(562-572행) 확장 |
| 4 | 탭 닫힘 시 assigned_tab 댕글링 포인터 | 높 | DetachFromWebContents()에 assigned_tab null 처리 추가 |
| 5 | tab_to_clients_ 역방향 매핑 과잉 | 중 | **삭제**, client_states_ 순회(최대 5개)로 충분 |
| 6 | STARTING→STOPPED fallback 경로 미명시 | 높 | _do_start() 실패 시 반드시 STOPPED 복귀 |
| 7 | 데몬 shutdown 시 스레드/프로세스 정리 없음 | 높 | shutdown()에 Popen.terminate()/kill() 추가 |
| 8 | 사용자 수동 종료 vs 크래시 구분 없음 | 중 | exit code 0=정상종료(재시작 안 함), non-zero=크래시(재시작) |
| 9 | flapping debounce 없음 | 중 | 소켓 끊김 감지 시 5초 grace period 후 전이 |
| 10 | pgrep 부정확 | 중 | Popen.poll()로 대체 (자식 프로세스 직접 추적) |
| 11 | 도구 수 39개→실제 33개 | 낮 | 수치 수정 |

---

## Phase 1: McpTool requires_session() (최소 변경)

**변경 파일 3개**

### 1-1. McpTool 인터페이스

**`src/mcp_tool_registry.h`**:
```cpp
virtual bool requires_session() const { return true; }
```

**`src/mcp_tool_registry.cc`** — DispatchToolCall():
```cpp
if (!session && tool->requires_session()) {
    // session 필요 도구만 차단
}
```

### 1-2. 오버라이드 (3개 도구 — session을 전혀 사용하지 않는 것만)

- `tools/tab_tool.h` → `return false;`
- `tools/clipboard_tool.h` → `return false;`
- `tools/browser_info_tool.h` → `return false;`

> bookmark/history는 내부 null guard 수정이 필요하므로 Phase 2에서 처리

**빌드**: ~15초 (3개 .o + 링크)

---

## Phase 2: McpServer 세션 라우팅

**변경 파일 2개 (핵심)**

### 2-1. ClientState 확장

**`src/mcp_server.h`**:
```cpp
struct ClientState {
    HandshakeState handshake_state = HandshakeState::kNotStarted;
    std::string client_name, client_version, protocol_version;
    raw_ptr<content::WebContents> assigned_tab = nullptr;  // 신규
};
```

- `active_web_contents_` 제거
- ~~tab_to_clients_ 역방향 매핑~~ → 불필요 (client_states_ 순회)

### 2-2. GetSessionForClient(client_id)

**`src/mcp_server.cc`**:
```cpp
McpSession* McpServer::GetSessionForClient(int client_id) {
    auto it = client_states_.find(client_id);
    if (it == client_states_.end() || !it->second.assigned_tab) return nullptr;
    auto sit = sessions_.find(it->second.assigned_tab);
    if (sit == sessions_.end()) return AttachToWebContents(it->second.assigned_tab);
    return sit->second.get();
}
```

### 2-3. HandleToolsCall() 변경

인라인 도구(tools_ 맵) 실행 전 세션을 클로저에 캡처:
```cpp
// ~~current_client_id_ 패턴~~ → 폐기
// 대신 기존 클로저 캡처 패턴(562-572행) 확장
McpSession* session = GetSessionForClient(client_id);
// session을 BindOnce로 캡처하여 인라인 핸들러에 전달
```

### 2-4. 탭 배정 정책

| 시점 | 동작 |
|------|------|
| HandleInitialized() | 현재 활성 탭 자동 배정 |
| tabs new | 새 탭을 호출 클라이언트에 배정 |
| tabs select | 해당 클라이언트의 assigned_tab만 변경 |
| OnClientDisconnected() | assigned_tab = nullptr |
| **DetachFromWebContents()** | **해당 탭을 가진 모든 클라이언트의 assigned_tab = nullptr** |

### 2-5. DetachFromWebContents() 보완 (크로스체크 #4)

```cpp
void McpServer::DetachFromWebContents(WebContents* wc) {
    // 기존: sessions_ 제거
    sessions_.erase(wc);
    // 신규: 해당 탭을 assigned_tab으로 가진 클라이언트 정리
    for (auto& [id, state] : client_states_) {
        if (state.assigned_tab == wc) {
            state.assigned_tab = nullptr;
        }
    }
}
```

### 2-6. active_web_contents_ 참조 전수 교체

- HandleToolsCall() 자동 attach → GetSessionForClient()
- HandleInitialized() → assigned_tab 배정
- ExecuteBrowserInfo() 부수효과 → 제거
- DetachFromWebContents() → assigned_tab 정리로 대체
- Shutdown() → client_states_ clear로 대체

### 2-7. tabs select에서 AssignTabToClient()

TabsTool은 현재 `McpSession* session`만 받고 `client_id`를 모름.
**해법**: HandleToolsCall()에서 tabs select 결과 콜백을 래핑하여, 도구 결과에서 선택된 tabId를 확인하고 assigned_tab 갱신.

또는 인라인 tools_ 맵에 "tabs" 핸들러를 다시 등록하되, client_id를 클로저에 캡처하여 전달.

**빌드**: ~30초 (mcp_server.o + 링크)

---

## Phase 3: 데몬 ChromiumManager

**변경 파일 1개**: `scripts/chromium-mcp-daemon.py`

### 3-1. ChromiumManager 상태 머신

```
STOPPED ──→ STARTING ──→ READY
   ↑                        │
   │   실패/크래시(non-zero) │
   └────────────────────────┘

   사용자 종료(exit 0) → STOPPED 유지 (재시작 안 함)
```

### 3-2. 핵심 설계 원칙

- `subprocess.Popen()`은 `_do_start()`에서만 호출 (단일 소유자)
- `threading.Condition`으로 대기/통보
- `_trigger_start()` 진입 시 `if state != STOPPED: return` 가드 (이중 시작 방지)
- `_do_start()` 실패 시 반드시 STOPPED 복귀 (fallback 경로)
- `pgrep` → `Popen.poll()` (자식 프로세스 직접 추적 + exit code 확인)
- 소켓 끊김 감지 시 **5초 grace period** 후 STOPPED 전이 (flapping 방지)
- 재시도 3회, 재시작 간 최소 cooldown 30초

### 3-3. shutdown() 정리 (크로스체크 #7)

```python
def shutdown(self):
    with self._condition:
        self._running = False
        self._state = STOPPING
        self._condition.notify_all()
    # Popen 프로세스 정리
    if self._process and self._process.poll() is None:
        self._process.terminate()
        try:
            self._process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self._process.kill()
```

### 3-4. 사용자 종료 구분 (크로스체크 #8)

```python
def _monitor_loop(self):
    while self._running:
        time.sleep(MONITOR_INTERVAL)
        if self._process and self._process.poll() is not None:
            exit_code = self._process.returncode
            if exit_code == 0:
                log.info('Chromium 정상 종료 — 재시작 안 함')
                # STOPPED 유지
            else:
                log.warning(f'Chromium 크래시 (exit={exit_code}) — 재시작')
                self._trigger_start()
```

### 3-5. 삭제되는 코드

- `ensure_chromium()`, `launch_chromium()`, `wait_for_chromium_socket()`, `is_chromium_running()`, `monitor_chromium()` → 전부 ChromiumManager 내부로 흡수

**빌드**: 불필요 (Python)

---

## 구현 순서

```
Phase 1: requires_session (3파일, ~15초 빌드)
  ↓
Phase 2: 서버 세션 라우팅 (2파일, ~30초 빌드)
  ↓  ← 테스트: 멀티 클라이언트 독립 탭
Phase 3: 데몬 ChromiumManager (1파일, 빌드 불필요)
  ↓  ← 테스트: 동시 접속 + 크래시 복구
커밋 & 푸시
```

---

## 검증 방법

1. **빌드**: `autoninja -C out/Default chrome`
2. **단일 클라이언트**: 소켓 → initialize → tabs list → tabs new → navigate
3. **멀티 클라이언트 독립 탭**: 3개 동시 접속 → 각각 다른 URL → 서로 간섭 없는지
4. **탭 닫힘 안전성**: 클라이언트 A의 assigned_tab을 UI에서 닫기 → A가 다음 요청 시 에러 없이 새 탭 자동 배정
5. **데몬 race condition**: Chromium 미실행 + 5개 동시 접속 → Popen 1회만
6. **크래시 복구**: `kill -9 Chromium` → 자동 재시작 → 재연결
7. **수동 종료**: Chromium Cmd+Q → 재시작 안 됨
