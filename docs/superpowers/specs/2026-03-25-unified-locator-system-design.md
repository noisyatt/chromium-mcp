# Chromium-MCP 통합 로케이터 시스템 설계

## 개요

CSS 셀렉터 전용이던 요소 탐색을 **접근성 트리(AX Tree) 기반 통합 로케이터**로 전면 재설계한다. 모든 액션 도구가 하나의 `ElementLocator`를 공유하고, auto-wait + actionability 검증을 거친 뒤 동작을 실행한다. 레거시 인라인 도구(mcp_server.cc)를 클래스 기반으로 완전 마이그레이션한다.

## 목표

1. **텍스트/역할 기반 요소 탐색** — `click({role: "button", name: "로그인"})` 한 번으로 완결
2. **스텔스 강화** — `Runtime.evaluate` 의존 최소화, CDP 네이티브 도메인(DOM/Input/Accessibility) 직접 사용으로 `isTrusted: true` 이벤트 보장
3. **안정성 향상** — auto-wait, actionability 검증, 자동 scrollIntoView
4. **코드 단일화** — 레거시/클래스 이중 경로 제거, 공유 유틸 통합

## 비목표

- Shadow DOM 피어싱 (향후 별도 설계)
- iframe 간 요소 탐색 (향후 별도 설계)
- IME 컴포지션 이벤트 구현

## 전제 조건

### Accessibility 도메인 활성화

`Accessibility.queryAXTree`를 사용하려면 `Accessibility.enable()`이 선행되어야 한다. 이 명령은 세션 attach 시 1회 호출하며, AXNodeId의 세션 내 일관성을 보장한다. `Accessibility.enable()`은 페이지 JS에 어떤 흔적도 남기지 않으므로 스텔스에 영향 없음.

**호출 시점**: `McpSession::OnAttached()` 내에서 `Page.enable`, `DOM.enable`과 함께 호출.

### queryAXTree CDP 프로토콜 정의

```
Accessibility.queryAXTree(
  nodeId?: DOM.NodeId,              // 검색 범위 루트 (선택)
  backendNodeId?: DOM.BackendNodeId, // 검색 범위 루트 (선택)
  objectId?: Runtime.RemoteObjectId, // 검색 범위 루트 (선택)
  accessibleName?: string,          // 접근성 이름 필터 (정확 일치)
  role?: string                     // ARIA 역할 필터 (정확 일치)
)
→ { nodes: AXNode[] }
```

각 AXNode에 `backendDOMNodeId` 필드가 포함되어 DOM 노드와 직접 브릿지 가능. Chromium M100+에서 안정적으로 지원됨 (experimental 도메인이지만 Chromium 내장이므로 문제없음).

---

## Section 1: 통합 로케이터 (ElementLocator)

### 로케이터 타입 5종

| 타입 | 파라미터 | CDP 명령 | 설명 |
|------|----------|----------|------|
| role/name | `role`, `name`, `exact` | `Accessibility.queryAXTree` | 접근성 역할+이름 기반. 핵심 신규 |
| text | `text`, `exact` | `Accessibility.queryAXTree(accessibleName)` | 텍스트 내용 기반 |
| selector | `selector` | `DOM.querySelector` | CSS 셀렉터. 기존 호환 |
| xpath | `xpath` | `DOM.performSearch` | XPath 표현식 |
| ref | `ref` | 직접 backendDOMNodeId 사용 | AX 노드 직접 참조 |

### 파라미터 스키마 (모든 액션 도구 공통)

```json
{
  "selector": {"type": "string", "description": "CSS selector"},
  "xpath":    {"type": "string", "description": "XPath expression"},
  "role":     {"type": "string", "description": "ARIA role (button, link, textbox...)"},
  "name":     {"type": "string", "description": "Accessible name (role과 함께 사용)"},
  "text":     {"type": "string", "description": "요소의 텍스트 내용"},
  "ref":      {"type": "integer", "description": "backendDOMNodeId (정수)"},
  "exact":    {"type": "boolean", "description": "텍스트 정확 일치 여부, 기본 false"}
}
```

우선순위: `role/name` > `text` > `selector` > `xpath` > `ref` (복수 지정 시 첫 매칭 사용)

### exact 파라미터와 부분 일치 전략

`queryAXTree`의 `accessibleName` 파라미터는 **정확 일치만 지원**한다.

- `exact: true` (기본 false) → `queryAXTree(accessibleName=text)` 직접 사용
- `exact: false` → `Accessibility.getFullAXTree()` 호출 후 클라이언트 사이드에서 `contains` 필터링. 성능을 위해 `depth` 파라미터로 트리 깊이 제한 가능.

`exact: false`의 비용이 크므로, 가능하면 `role` + `name` 조합의 정확 매칭을 권장한다.

### 복수 결과 처리 규칙

`queryAXTree`가 여러 노드를 반환할 때:

1. **visible 요소 우선** — `DOM.getBoxModel` 성공하는 첫 번째 노드 선택
2. **모두 visible이면** — DOM 순서상 첫 번째 선택
3. **모두 non-visible이면** — 첫 번째 선택 (actionability 체크에서 재시도/실패 처리)

find 도구는 복수 결과를 모두 반환. 액션 도구(click, fill 등)는 위 규칙으로 단일 요소 선택.

### Callback 규약

- 성공: `{Result값, ""}` (error 빈 문자열)
- 실패: `{std::nullopt, "에러 메시지"}`
- Result와 error가 동시에 유효한 경우는 없음

### 내부 해석 흐름

```
파라미터 수신
  ├─ role/name 있음? → Accessibility.queryAXTree(role, accessibleName)
  │                     → backendDOMNodeId 획득
  ├─ text 있음?     → Accessibility.queryAXTree(accessibleName=text)
  │                     → backendDOMNodeId 획득
  ├─ selector 있음? → DOM.querySelector(selector) → nodeId 획득
  ├─ xpath 있음?    → DOM.performSearch(xpath) → nodeId 획득
  └─ ref 있음?      → 직접 backendDOMNodeId 사용
          │
          ▼
    ResolveToCoordinates()
      → DOM.getBoxModel(backendNodeId)
      → ExtractBoxModelCenter() (단일 공유 구현)
      → {x, y, nodeId, backendNodeId}
```

### queryAXTree 활용 근거

- Chromium이 ACCNAME-1.1 사양에 따라 Accessible Name을 이미 계산 → 클라이언트 구현 불필요
- 중첩 텍스트, `aria-label`, `alt`, `title`, `placeholder` 자동 통합
- `backendDOMNodeId` 필드로 AX 노드 → DOM 노드 브릿지 제공
- 페이지 JS 컨텍스트를 거치지 않음 → 스텔스 안전

### C++ 클래스 구조

```cpp
// src/tools/element_locator.h
class ElementLocator {
 public:
  struct Result {
    int backend_node_id;
    int node_id;
    double x, y;          // BoxModel 중심 좌표
    std::string role;
    std::string name;
  };

  using Callback = base::OnceCallback<void(std::optional<Result>, std::string error)>;

  void Locate(McpSession* session,
              const base::Value::Dict& params,
              Callback callback);

 private:
  void LocateByRole(McpSession*, const std::string& role,
                    const std::string& name, bool exact, Callback);
  void LocateByText(McpSession*, const std::string& text, bool exact, Callback);
  void LocateBySelector(McpSession*, const std::string& selector, Callback);
  void LocateByXPath(McpSession*, const std::string& xpath, Callback);
  void LocateByRef(McpSession*, int backend_node_id, Callback);

  void ResolveToCoordinates(McpSession*, int backend_node_id, Callback);
};
```

---

## Section 2: Auto-Wait + Actionability 검증

### Actionability 체크 항목

| 체크 | 설명 | CDP 명령 | JS 필요 |
|------|------|----------|:---:|
| EXISTS | DOM에 존재하는가 | `queryAXTree` or `querySelector` | 없음 |
| VISIBLE | 렌더링되고 보이는가 | `DOM.getBoxModel` 성공 여부 | 없음 |
| STABLE | 좌표가 안정적인가 (애니메이션 중 아닌가) | `DOM.getBoxModel` 2회 비교, 50ms 간격 | 없음 |
| ENABLED | disabled 속성 없는가 | AXNode properties `disabled` 확인 | 없음 |
| EDITABLE | readonly 아닌가 | AXNode properties `readonly` 확인 | 없음 |
| IN_VIEWPORT | 화면 안에 있는가 | `DOM.getBoxModel` 좌표 vs viewport 크기 비교 | 없음 |
| scrollIntoView | 뷰포트 밖이면 자동 스크롤 | `DOM.scrollIntoViewIfNeeded(backendNodeId)` | 없음 |

전부 CDP 네이티브 — `Runtime.evaluate` 제로.

### 액션별 필요 체크

| 체크 | click | fill | hover | scroll | drag | select_option | file_upload |
|------|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| EXISTS | O | O | O | O | O | O | O |
| VISIBLE | O | O | O | - | O | O | O |
| STABLE | O | O | - | - | O | - | - |
| ENABLED | O | O | - | - | - | O | O |
| EDITABLE | - | O | - | - | - | - | - |
| IN_VIEWPORT | O | O | O | O | O | - | - |

### Auto-Wait 루프

```
시작 (timeout 기본 5000ms, 설정 가능)
  ├─→ Locate element (ElementLocator)
  │     실패? → 200ms 후 재시도 → timeout? → 에러 반환
  ├─→ Actionability 체크
  │     실패? → 200ms 후 재시도 → timeout? → 에러 반환
  ├─→ IN_VIEWPORT 실패?
  │     → DOM.scrollIntoViewIfNeeded(backendNodeId)
  │     → 좌표 재계산
  ├─→ STABLE 체크 (좌표 2회 측정, 50ms 간격)
  │     불일치? → 200ms 후 재시도
  └─→ 모든 체크 통과 → 실제 액션 실행
```

### 공통 추가 파라미터

```json
{
  "timeout": {"type": "number", "description": "ms, 기본 5000, 0이면 재시도 없이 1회만 체크"},
  "force":   {"type": "boolean", "description": "true면 actionability 체크 전체 스킵 (locate만 수행), 기본 false"}
}
```

### timeout vs force 동작 구분

| 설정 | Locate | Actionability 체크 | 재시도 |
|------|:---:|:---:|:---:|
| `timeout: 5000` (기본) | O | O | 200ms 간격, 5초까지 |
| `timeout: 0` | O | O | 없음 — 1회 체크 후 즉시 성공/실패 |
| `force: true` | O | 스킵 | 없음 |

### STABLE 체크 세부 규칙

- `DOM.getBoxModel` 2회 측정, 50ms 간격
- 좌표 차이 **2px 이내**면 안정으로 판정
- 불안정 시 200ms 후 재측정 (timeout 내에서)

### C++ 클래스 구조

```cpp
// src/tools/actionability_checker.h
class ActionabilityChecker {
 public:
  enum class ActionType {
    kClick, kFill, kHover, kScroll, kDrag, kSelectOption, kFileUpload
  };

  struct Options {
    int timeout_ms = 5000;
    int poll_interval_ms = 200;
    bool force = false;
  };

  using Callback = base::OnceCallback<void(ElementLocator::Result, std::string error)>;

  void VerifyAndLocate(McpSession* session,
                       const base::Value::Dict& params,
                       ActionType action,
                       Options options,
                       Callback callback);

 private:
  void PollLoop();
  void CheckVisible(int backend_node_id);
  void CheckStable(int backend_node_id);    // getBoxModel 2회 비교
  void CheckEnabled(int backend_node_id);   // AX properties
  void ScrollIntoView(int backend_node_id); // scrollIntoViewIfNeeded

  // per-request 컨텍스트: 동시 요청 간 타이머 충돌 방지
  struct PollContext {
    ElementLocator locator;
    base::OneShotTimer poll_timer;
    base::OneShotTimer timeout_timer;
    Callback callback;
    McpSession* session;
    ActionType action;
    base::Value::Dict params;
  };

  void StartPoll(std::shared_ptr<PollContext> ctx);
};
```

---

## Section 3: 레거시 마이그레이션

### 현재 이중 구현 현황

레거시(mcp_server.cc `RegisterXxxTool` + `ExecuteXxx`) 9개 도구가 `tools_` 맵에 등록되어 우선 디스패치됨. 클래스 기반(tools/ 디렉토리)은 `tool_registry_`에 등록되어 폴백으로만 동작.

### 마이그레이션 대상

| 도구 | 현재 활성 | 변경 |
|------|-----------|------|
| navigate | 레거시 | 기존 NavigateTool 클래스 활성화 |
| screenshot | 레거시 | 기존 ScreenshotTool 클래스 활성화 |
| page_content | 레거시 | 기존 PageContentTool 클래스 활성화 |
| click | 레거시 | ClickTool 재작성 (로케이터+actionability 통합) |
| fill | 레거시 | FillTool 재작성 (동일) |
| evaluate | 레거시 | 기존 JavaScriptTool 클래스 활성화 |
| network_capture | 레거시 | 기존 NetworkCaptureTool 클래스 활성화 |
| network_requests | 레거시 | 기존 NetworkRequestsTool 클래스 활성화 |
| browser_info | 레거시 | BrowserInfoTool 클래스로 전환 (이미 활성) |

### mcp_server.cc 변경

- `RegisterXxxTool()` 9개 메서드 전부 제거
- `ExecuteXxx()` 핸들러 전부 제거
- `tools_` 맵 (`std::unordered_map<std::string, McpToolDefinition>`) 제거
- `HandleToolsCall()`에서 레거시 경로 제거, `tool_registry_->DispatchToolCall()` 단일 경로만 유지
- `HandleToolsList()`에서 레거시 직렬화 제거

### 마이그레이션 전환 순서 (원자적 전환)

레거시/신규 전환은 도구별로 수행하되, 각 도구의 전환은 단일 커밋으로 원자적으로 처리:

1. 신규 클래스를 `tool_registry_`에 등록
2. 레거시 `RegisterXxxTool()` 호출 제거 (같은 커밋)
3. `tools_` 맵에서 해당 도구 항목 자동 제거 (등록 안 되므로)

전환 순서:
- Phase 1: 단순 전환 (navigate, screenshot, page_content, evaluate, network_capture, network_requests) — 기존 클래스가 이미 완성 상태
- Phase 2: 재작성 (click, fill, find) — ElementLocator + ActionabilityChecker 의존
- Phase 3: 수정 (hover, scroll, drag, select_option, file_upload, wait, element, element_info) — 로케이터 교체

### 버그 수정 추가 항목

| 버그 | 파일 | 설명 |
|------|------|------|
| FillTool macOS Ctrl+A → Cmd+A | dom_tool.cc | macOS에서 modifier=4(Meta) 사용 |
| `Page.loadEventFired` 수정 방법 | dom_tool.cc | `RegisterCdpEventHandler` + timeout 패턴 (wait_tool.cc 414행 참고) |

### 새 파일 구조

```
src/tools/
├── element_locator.h/cc        ← 신규
├── actionability_checker.h/cc  ← 신규
├── box_model_util.h/cc         ← 신규 (ExtractBoxModelCenter 공유)
├── click_tool.h/cc             ← 재작성
├── fill_tool.h/cc              ← 재작성
├── find_tool.h/cc              ← 재작성
├── hover_tool.h/cc             ← 수정 (로케이터 교체)
├── scroll_tool.h/cc            ← 수정
├── drag_tool.h/cc              ← 수정
├── select_option_tool.h/cc     ← 수정
├── file_upload_tool.h/cc       ← 수정
├── wait_tool.h/cc              ← 수정
├── element_tool.h/cc           ← 수정
├── element_info_tool.h/cc      ← 수정
├── keyboard_tool.h/cc          ← 경미 수정
└── (나머지 이미 활성인 도구들 유지)
```

### 버그 수정 포함 목록

| 버그 | 파일 | 설명 |
|------|------|------|
| `isTrusted: false` 이벤트 | mcp_server.cc fill | 레거시 제거로 해소 |
| `Page.loadEventFired` 잘못된 CDP 호출 | dom_tool.cc | 이벤트 핸들러로 수정 |
| Delete keyUp 누락 | dom_tool.cc fill | keyUp 추가 |
| ExtractBoxModelCenter 4중 복사 | 4개 파일 | box_model_util.cc로 통합 |
| DOM.getDocument 이중 호출 | drag_tool.cc | rootNodeId 캐싱 |
| performSearch 세션 누수 | find_tool.cc | discardSearchResults 추가 |
| wait selector가 가시성 무시 | wait_tool.cc | actionability 연동 |
| wait 타이머 인스턴스 공유 충돌 | wait_tool.cc | per-request 타이머로 전환 |
| select_option 사일런트 실패 | select_option_tool.cc | selectedIndex 검증 추가 |
| selector injection 취약점 | mcp_server.cc fill | 레거시 제거로 해소 |
| download JS 이스케이프 불완전 | download_tool.cc | 큰따옴표 이스케이프 추가 |
| dialog autoHandle 실패 무시 | dialog_tool.cc | 에러 콜백 전파 |

---

## Section 4: find 도구 강화

### 변경 전후

| 항목 | 현재 | 변경 후 |
|------|------|---------|
| role/name 검색 | 미지원 | `Accessibility.queryAXTree` |
| text 검색 | `DOM.performSearch` (매칭 제어 불가) | `queryAXTree(accessibleName)` |
| 반환 형식 | nodeType, attributes만 | role, name, visible, enabled, boundingBox 포함 |
| innerText 반환 | 미동작 (describeNode 한계) | name 필드로 대체 (AX 계산값) |
| 검색세션 정리 | 누수 | `DOM.discardSearchResults` 호출 |

### 반환 형식

```json
{
  "total": 3,
  "items": [
    {
      "index": 0,
      "backendNodeId": 142,
      "tag": "button",
      "role": "button",
      "name": "로그인",
      "description": "",
      "visible": true,
      "enabled": true,
      "boundingBox": {"x": 320, "y": 180, "width": 80, "height": 36},
      "attributes": {"class": "btn-primary", "type": "submit"}
    }
  ]
}
```

### find 파라미터 스키마

find 도구도 공통 로케이터 파라미터를 직접 사용한다. `type`은 명시적 오버라이드 역할만 한다:

```json
{
  "role":     {"type": "string", "description": "ARIA role 필터"},
  "name":     {"type": "string", "description": "Accessible name 필터"},
  "text":     {"type": "string", "description": "텍스트 내용으로 검색"},
  "selector": {"type": "string", "description": "CSS 셀렉터"},
  "xpath":    {"type": "string", "description": "XPath 표현식"},
  "exact":    {"type": "boolean", "description": "정확 일치 여부, 기본 false"},
  "visible":  {"type": "boolean", "description": "보이는 요소만 필터"},
  "enabled":  {"type": "boolean", "description": "활성 요소만 필터"},
  "limit":    {"type": "number", "description": "최대 반환 수, 기본 10"}
}
```

액션 도구와 동일한 로케이터 인터페이스를 사용하되, 복수 결과를 모두 반환하는 점만 다르다.

### CDP 매핑

| 반환 필드 | CDP 소스 | JS 필요 |
|-----------|----------|:---:|
| backendNodeId | `queryAXTree` 결과 직접 | 없음 |
| role, name, description | AXNode 직접 | 없음 |
| visible | `DOM.getBoxModel` 성공 여부 | 없음 |
| enabled | AXNode properties `disabled` 반전 | 없음 |
| boundingBox | `DOM.getBoxModel` content quad → rect | 없음 |
| tag, attributes | `DOM.describeNode(backendNodeId)` | 없음 |

---

## 전체 도구 파라미터 변경 요약

### 공통 추가 (요소 대상 도구 전체)

```
+ role, name, text, exact     (로케이터)
+ timeout, force              (auto-wait)
  selector, xpath             (기존 유지)
```

### 도구별 변경

| 도구 | 변경 유형 | 핵심 변경 |
|------|-----------|-----------|
| click | 재작성 | 레거시→클래스, 로케이터+actionability |
| fill | 재작성 | 레거시→클래스, isTrusted 개선 |
| hover | 수정 | 로케이터 교체 |
| scroll | 수정 | 로케이터 교체 |
| drag | 수정 | 시작/끝 각각 로케이터, getDocument 이중호출 제거 |
| select_option | 수정 | 로케이터 교체, 사일런트 실패 수정 |
| file_upload | 수정 | 로케이터 교체 |
| find | 재작성 | queryAXTree 통합, 반환형 개선 |
| wait | 수정 | 가시성 조건 추가, 타이머 버그 수정 |
| element | 수정 | 로케이터 교체 |
| element_info | 수정 | 로케이터 교체 |
| keyboard | 경미 | modifier 단독 press 지원 |
| navigate | 전환 | 레거시→기존 클래스 활성화 |
| screenshot | 전환 | 레거시→기존 클래스 활성화 |
| page_content | 전환 | 레거시→기존 클래스 활성화 |
| evaluate | 전환 | 레거시→기존 클래스 활성화 |
| network_capture | 전환 | 레거시→기존 클래스 활성화 |
| network_requests | 전환 | 레거시→기존 클래스 활성화 |

### 하위 호환성

- `selector` 파라미터는 모든 도구에서 그대로 동작 — 기존 호출 깨지지 않음
- 새 파라미터(`role`, `name`, `text`)는 전부 선택적
- `timeout` 기본값 5000ms, `force` 기본값 false
- **동작 변경 고지**: 기존에는 요소 미발견 시 즉시 에러를 반환했지만, 리팩토링 후에는 기본 5초간 auto-wait 후 에러 반환. 즉시 실패가 필요하면 `timeout: 0` 사용.
- find 도구 반환 형식 변경: 기존 `tagName`/`localName` 필드는 `tag`로 통합, `role`/`name`/`visible`/`enabled`/`boundingBox` 추가
