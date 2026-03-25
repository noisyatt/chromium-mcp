# 통합 로케이터 시스템 구현 계획

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 모든 MCP 도구의 요소 탐색을 AX Tree 기반 통합 로케이터로 전환하고, auto-wait + actionability 검증을 추가하며, 레거시 인라인 도구를 클래스 기반으로 완전 마이그레이션한다.

**Architecture:** `ElementLocator`가 role/name/text/selector/xpath/ref 6종 로케이터를 통합 처리하고, `ActionabilityChecker`가 per-request 컨텍스트로 auto-wait + 가시성/활성 검증을 수행한다. 모든 액션 도구가 이 두 클래스를 공유하며, mcp_server.cc의 레거시 인라인 도구 9개를 tool_registry_ 단일 경로로 전환한다.

**Tech Stack:** C++ (Chromium base library), CDP (Chrome DevTools Protocol — DOM, Input, Accessibility 도메인)

**Spec:** `docs/superpowers/specs/2026-03-25-unified-locator-system-design.md`

---

## 파일 구조

### 신규 생성

| 파일 | 책임 |
|------|------|
| `src/tools/box_model_util.h` | ExtractBoxModelCenter, MakeSuccessResult, MakeErrorResult, HasCdpError 등 공유 유틸 선언 |
| `src/tools/box_model_util.cc` | 공유 유틸 구현 (4개 도구에서 복사된 코드 통합) |
| `src/tools/element_locator.h` | ElementLocator 클래스 선언 |
| `src/tools/element_locator.cc` | 5종 로케이터 구현 (queryAXTree, querySelector, performSearch, ref) |
| `src/tools/actionability_checker.h` | ActionabilityChecker 클래스 선언 (per-request PollContext) |
| `src/tools/actionability_checker.cc` | auto-wait 루프, 가시성/활성/안정성 체크 구현 |

### 주요 수정

| 파일 | 변경 내용 |
|------|-----------|
| `src/mcp_server.cc` | 레거시 RegisterXxxTool 9개 제거, tools_ 맵 제거, 단일 경로 전환 |
| `src/mcp_server.h` | tools_ 맵, RegisterXxxTool, ExecuteXxx 선언 제거 |
| `src/mcp_session.cc` | Accessibility.enable() 호출 추가 |
| `src/BUILD.gn` | 신규 파일 6개 등록 |
| `src/tools/click_tool.h/cc` | 재작성 (dom_tool.cc ClickTool → 독립 파일, locator+checker 통합) |
| `src/tools/fill_tool.h/cc` | 재작성 (dom_tool.cc FillTool → 독립 파일, locator+checker 통합) |
| `src/tools/find_tool.h/cc` | 재작성 (queryAXTree 통합, 반환형 개선) |
| `src/tools/hover_tool.cc` | ExtractBoxModelCenter → box_model_util, locator 교체 |
| `src/tools/scroll_tool.cc` | 동일 |
| `src/tools/drag_tool.cc` | 동일 + getDocument 이중호출 제거 |
| `src/tools/select_option_tool.cc` | locator 교체 + 사일런트 실패 수정 |
| `src/tools/file_upload_tool.cc` | locator 교체 |
| `src/tools/wait_tool.cc` | 가시성 체크 연동, per-request 타이머 |
| `src/tools/element_tool.cc` | locator 교체 |
| `src/tools/element_info_tool.cc` | locator 교체 |
| `src/tools/dom_tool.h/cc` | ClickTool/FillTool 제거 (독립 파일로 이동) |

---

## Task 1: box_model_util 공유 유틸 추출

**Files:**
- Create: `src/tools/box_model_util.h`
- Create: `src/tools/box_model_util.cc`
- Modify: `src/BUILD.gn`

- [ ] **Step 1: box_model_util.h 작성**

```cpp
// src/tools/box_model_util.h
#ifndef CHROME_BROWSER_MCP_TOOLS_BOX_MODEL_UTIL_H_
#define CHROME_BROWSER_MCP_TOOLS_BOX_MODEL_UTIL_H_

#include "base/values.h"

namespace mcp {

// CDP 응답에서 에러 여부 확인
bool HasCdpError(const base::Value& response);

// CDP 에러 메시지 추출
std::string ExtractCdpErrorMessage(const base::Value& response);

// MCP 결과 생성 헬퍼
base::Value MakeSuccessResult(const std::string& message);
base::Value MakeErrorResult(const std::string& message);
base::Value MakeJsonResult(base::Value::Dict result_dict);

// DOM.getDocument 응답에서 rootNodeId 추출
int ExtractRootNodeId(const base::Value& response);

// DOM.querySelector 응답에서 nodeId 추출
int ExtractNodeId(const base::Value& response);

// DOM.getBoxModel 응답에서 content quad 중심 좌표 추출
bool ExtractBoxModelCenter(const base::Value& response, double* out_x, double* out_y);

// DOM.getBoxModel 응답에서 boundingBox {x, y, width, height} 추출
bool ExtractBoundingBox(const base::Value& response, base::Value::Dict* out_rect);

// HandleCdpError: CDP 에러 시 callback 호출 후 true 반환
// NOLINTNEXTLINE(runtime/references)
bool HandleCdpError(const base::Value& response,
                    const std::string& step_name,
                    base::OnceCallback<void(base::Value)>& callback);

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_BOX_MODEL_UTIL_H_
```

- [ ] **Step 2: box_model_util.cc 구현**

hover_tool.cc의 익명 namespace 유틸 (L17-128)을 복사하여 `mcp::` namespace 함수로 변환. `ExtractBoundingBox` 추가 (content quad → {x, y, width, height} rect 변환).

- [ ] **Step 3: BUILD.gn에 등록**

`src/BUILD.gn`의 `sources` 배열에 추가:
```
"tools/box_model_util.cc",
"tools/box_model_util.h",
```

- [ ] **Step 4: 빌드 확인**

Run: `cd /Users/daniel/chromium/src && autoninja -C out/Default chrome/browser/mcp:mcp`
Expected: 빌드 성공

- [ ] **Step 5: 커밋**

```bash
git add src/tools/box_model_util.h src/tools/box_model_util.cc src/BUILD.gn
git commit -m "리팩토링: box_model_util 공유 유틸 추출 (4중 복사 제거 준비)"
```

---

## Task 2: ElementLocator 구현

**Files:**
- Create: `src/tools/element_locator.h`
- Create: `src/tools/element_locator.cc`
- Modify: `src/mcp_session.cc` (Accessibility.enable 추가)
- Modify: `src/BUILD.gn`

- [ ] **Step 1: element_locator.h 작성**

스펙 Section 1의 C++ 클래스 구조 그대로 구현. `Result` 구조체에 `backend_node_id`, `node_id`, `x`, `y`, `role`, `name` 포함. `Callback`은 `base::OnceCallback<void(std::optional<Result>, std::string)>`.

- [ ] **Step 2: element_locator.cc — LocateByRole 구현**

`Accessibility.queryAXTree(role, accessibleName)` → `nodes` 배열 순회 → 첫 visible 노드의 `backendDOMNodeId` 추출 → `ResolveToCoordinates`. 복수 결과 처리 규칙 (스펙 82-90행) 적용.

- [ ] **Step 3: element_locator.cc — LocateByText 구현**

`exact: true` → `queryAXTree(accessibleName=text)`, `exact: false` → `getFullAXTree()` 후 클라이언트 사이드 contains 필터.

- [ ] **Step 4: element_locator.cc — LocateBySelector 구현**

기존 패턴 유지: `DOM.getDocument(depth:0)` → `DOM.querySelector(selector)` → `DOM.describeNode(nodeId)` → `backendNodeId` 추출 → `ResolveToCoordinates(backendNodeId)`. **주의**: querySelector는 `nodeId`를 반환하지만 ResolveToCoordinates는 `backendNodeId`가 필요하므로 `DOM.describeNode` 변환 단계 필수.

- [ ] **Step 5: element_locator.cc — LocateByXPath 구현**

`DOM.performSearch(query)` → `DOM.getSearchResults` → 첫 nodeId → `DOM.describeNode(nodeId)` → `backendNodeId` 추출 → `ResolveToCoordinates(backendNodeId)`. **`DOM.discardSearchResults(searchId)` 호출 추가** (세션 누수 수정). nodeId→backendNodeId 변환이 LocateBySelector와 동일 패턴.

- [ ] **Step 6: element_locator.cc — LocateByRef 구현**

`backendNodeId` 직접 → `ResolveToCoordinates`.

- [ ] **Step 7: element_locator.cc — ResolveToCoordinates 구현**

`DOM.getBoxModel({backendNodeId})` → `ExtractBoxModelCenter` (box_model_util 사용).

- [ ] **Step 8: element_locator.cc — Locate 디스패치 메서드**

파라미터에서 우선순위 (`role/name` > `text` > `selector` > `xpath` > `ref`) 에 따라 적절한 LocateBy\* 호출.

- [ ] **Step 9: mcp_session.cc에 Accessibility.enable() 추가**

`McpSession::Attach()` (또는 `OnAttached()`) 에서 `Page.enable`, `DOM.enable` 호출하는 곳 찾아서 `Accessibility.enable` 추가.

- [ ] **Step 10: BUILD.gn 등록**

```
"tools/element_locator.cc",
"tools/element_locator.h",
```

- [ ] **Step 11: 빌드 확인**

Run: `cd /Users/daniel/chromium/src && autoninja -C out/Default chrome/browser/mcp:mcp`

- [ ] **Step 12: 커밋**

```bash
git add src/tools/element_locator.h src/tools/element_locator.cc src/mcp_session.cc src/BUILD.gn
git commit -m "신규: ElementLocator — AX Tree 기반 통합 요소 탐색"
```

---

## Task 3: ActionabilityChecker 구현

**Files:**
- Create: `src/tools/actionability_checker.h`
- Create: `src/tools/actionability_checker.cc`
- Modify: `src/BUILD.gn`

- [ ] **Step 1: actionability_checker.h 작성**

스펙 Section 2의 C++ 구조. `PollContext`를 `shared_ptr`로 관리. `ActionType` enum, `Options` struct 포함.

- [ ] **Step 2: actionability_checker.cc — VerifyAndLocate 진입점**

`force: true` → locator.Locate만 수행 후 즉시 반환. 그 외 → `StartPoll` 호출.

- [ ] **Step 3: actionability_checker.cc — StartPoll 루프**

1. `locator.Locate` → 실패 시 poll_timer 200ms 재시도 (timeout 내)
2. `CheckVisible` → `DOM.getBoxModel` 성공 여부
3. `CheckEnabled` → `Accessibility.getPartialAXTree(backendNodeId)` → properties에서 disabled 확인
4. `CheckEditable` (fill 전용) → AXNode properties에서 `readonly` 확인
5. `CheckStable` → getBoxModel 2회, 50ms 간격, 2px 임계값
6. viewport 체크 → 실패 시 `DOM.scrollIntoViewIfNeeded(backendNodeId)`

`poll_timer`는 `base::OneShotTimer`를 매 사이클마다 재시작하는 패턴으로 구현 (RepeatingTimer 아님 — 각 사이클에서 비동기 체크 완료 후 다음 사이클 시작).
6. 모든 통과 → callback 호출

- [ ] **Step 4: actionability_checker.cc — timeout 처리**

`timeout_timer` → `timeout_ms` 후 "Timeout waiting for element" 에러. `timeout: 0` → 재시도 없이 1회만 체크.

- [ ] **Step 5: BUILD.gn 등록**

```
"tools/actionability_checker.cc",
"tools/actionability_checker.h",
```

- [ ] **Step 6: 빌드 확인**

Run: `cd /Users/daniel/chromium/src && autoninja -C out/Default chrome/browser/mcp:mcp`

- [ ] **Step 7: 커밋**

```bash
git add src/tools/actionability_checker.h src/tools/actionability_checker.cc src/BUILD.gn
git commit -m "신규: ActionabilityChecker — auto-wait + 가시성/활성 검증"
```

---

## Task 4: Phase 1 레거시 마이그레이션 (단순 전환 6개)

**Files:**
- Modify: `src/mcp_server.cc` (L794-802 제거, L812-851 등록 추가)
- Modify: `src/mcp_server.h` (RegisterXxxTool/ExecuteXxx 선언 제거)

대상: navigate, screenshot, page_content, evaluate, network_capture, network_requests, tabs (7개)

이 7개 도구는 클래스 기반 구현이 이미 완성되어 있으므로 (NavigateTool, ScreenshotTool, PageContentTool, JavaScriptTool, NetworkCaptureTool, NetworkRequestsTool, TabsTool) 등록만 전환하면 됨. TabsTool은 이미 tool_registry_에 등록되어 있으므로 레거시 RegisterTabsTool + ExecuteTabs만 제거.

- [ ] **Step 1: mcp_server.cc — 7개 레거시 등록 제거**

L794-796, L799, L800-801에서 해당 `RegisterXxxTool()` 호출 제거. `RegisterTabsTool()` 호출도 제거 (TabsTool은 이미 tool_registry_에 등록되어 있으므로).

- [ ] **Step 2: mcp_server.cc — tool_registry_ 등록 추가**

`RegisterBuiltinTools()` 내 적절한 위치에:
```cpp
tool_registry_->RegisterTool(std::make_unique<NavigateTool>());
tool_registry_->RegisterTool(std::make_unique<ScreenshotTool>());
tool_registry_->RegisterTool(std::make_unique<PageContentTool>());
tool_registry_->RegisterTool(std::make_unique<JavaScriptTool>());
tool_registry_->RegisterTool(std::make_unique<NetworkCaptureTool>());
tool_registry_->RegisterTool(std::make_unique<NetworkRequestsTool>());
```

- [ ] **Step 3: mcp_server.h — 선언 제거**

`RegisterNavigateTool`, `RegisterScreenshotTool`, `RegisterPageContentTool`, `RegisterEvaluateTool`, `RegisterNetworkCaptureTool`, `RegisterNetworkRequestsTool`, `RegisterTabsTool` 선언 제거.
`ExecuteNavigate`, `ExecuteScreenshot`, `ExecutePageContent`, `ExecuteEvaluate`, `ExecuteNetworkCapture`, `ExecuteNetworkRequests`, `ExecuteTabs` 선언 제거.

- [ ] **Step 4: mcp_server.cc — 해당 ExecuteXxx 구현 코드 제거**

`ExecuteNavigate`, `ExecuteScreenshot`, `ExecutePageContent`, `ExecuteEvaluate`, `ExecuteNetworkCapture`, `ExecuteNetworkRequests`, `ExecuteTabs` 메서드 본문 전부 제거.

- [ ] **Step 5: mcp_server.cc — 해당 RegisterXxxTool 구현 코드 제거**

7개 메서드 본문 전부 제거.

- [ ] **Step 6: 빌드 확인**

Run: `cd /Users/daniel/chromium/src && autoninja -C out/Default chrome/browser/mcp:mcp`

- [ ] **Step 7: 커밋**

```bash
git add src/mcp_server.cc src/mcp_server.h
git commit -m "마이그레이션 Phase 1: 7개 레거시 도구 클래스 전환 (navigate, screenshot, page_content, evaluate, network_*, tabs)"
```

---

## Task 5: ClickTool 재작성

**Files:**
- Create: `src/tools/click_tool.h` (독립 파일 — dom_tool에서 분리)
- Create: `src/tools/click_tool.cc`
- Modify: `src/mcp_server.cc` (RegisterClickTool 제거, ClickTool 등록)
- Modify: `src/mcp_server.h` (선언 제거)
- Modify: `src/tools/dom_tool.h` (ClickTool 클래스 제거)
- Modify: `src/tools/dom_tool.cc` (ClickTool 구현 제거)
- Modify: `src/BUILD.gn`

- [ ] **Step 1: click_tool.h 작성**

McpTool 상속. `Execute()`에서 `ActionabilityChecker::VerifyAndLocate` → `PerformClick` 체인. 파라미터: 공통 로케이터(role/name/text/selector/xpath/ref/exact) + timeout/force + button + waitForNavigation.

- [ ] **Step 2: click_tool.cc — Execute 구현**

```
ActionabilityChecker::VerifyAndLocate(params, kClick, options, callback)
  → PerformClick(result.x, result.y, button)
    → Input.dispatchMouseEvent(mousePressed)
    → Input.dispatchMouseEvent(mouseReleased)
```

waitForNavigation: `RegisterCdpEventHandler("Page.loadEventFired")` + timeout 패턴 (wait_tool.cc 414행 참고). 기존 dom_tool.cc의 잘못된 SendCdpCommand 방식 수정.

- [ ] **Step 3: mcp_server.cc — RegisterClickTool 제거, ClickTool 등록**

- L797 `RegisterClickTool()` 호출 제거
- `tool_registry_->RegisterTool(std::make_unique<ClickTool>())` 추가
- RegisterClickTool 메서드 본문 + ExecuteClick 메서드 본문 제거
- mcp_server.h에서 선언 제거
- `#include "chrome/browser/mcp/tools/click_tool.h"` 추가

- [ ] **Step 4: dom_tool.h/cc에서 ClickTool 클래스 제거 (Step 5와 동일 커밋 필수)**

`dom_tool.h`의 ClickTool 클래스 선언과 `dom_tool.cc`의 ClickTool 구현 전부 제거. FillTool은 Task 6에서 처리. **주의: 새 click_tool.h와 dom_tool.h의 ClickTool이 동시에 존재하면 ODR(One Definition Rule) 위반. 반드시 제거와 추가가 같은 빌드에서 원자적으로 수행되어야 함.**

- [ ] **Step 5: BUILD.gn — click_tool 등록 (Step 4와 동일 커밋)**

```
"tools/click_tool.cc",
"tools/click_tool.h",
```

- [ ] **Step 6: 빌드 확인**

Run: `cd /Users/daniel/chromium/src && autoninja -C out/Default chrome/browser/mcp:mcp`

- [ ] **Step 7: 커밋**

```bash
git add src/tools/click_tool.h src/tools/click_tool.cc src/tools/dom_tool.h src/tools/dom_tool.cc src/mcp_server.cc src/mcp_server.h src/BUILD.gn
git commit -m "재작성: ClickTool — 통합 로케이터 + actionability + waitForNavigation 수정"
```

---

## Task 6: FillTool 재작성

**Files:**
- Create: `src/tools/fill_tool.h`
- Create: `src/tools/fill_tool.cc`
- Modify: `src/mcp_server.cc`
- Modify: `src/mcp_server.h`
- Modify: `src/tools/dom_tool.h/cc` (FillTool 클래스 제거)
- Modify: `src/BUILD.gn`

- [ ] **Step 1: fill_tool.h 작성**

McpTool 상속. 공통 로케이터 + timeout/force + value 파라미터.

- [ ] **Step 2: fill_tool.cc — Execute 구현**

```
ActionabilityChecker::VerifyAndLocate(params, kFill, options, callback)
  → DOM.focus(backendNodeId)
  → Cmd+A (macOS: modifier=4, 다른 플랫폼: Ctrl modifier=2) keyDown → keyUp
  → Delete keyDown → Delete keyUp (기존 keyUp 누락 수정)
  → Input.insertText(value)
```

레거시의 `Runtime.evaluate + el.value` 방식 완전 대체. `isTrusted: true` 이벤트 보장.

- [ ] **Step 3: mcp_server.cc — RegisterFillTool 제거, FillTool 등록**

L798 제거, tool_registry_ 등록, 선언/구현 제거.

- [ ] **Step 4: dom_tool.h/cc에서 FillTool 클래스 제거**

dom_tool에 남아있는 코드가 없으면 dom_tool.h/cc 자체를 BUILD.gn에서 제거 가능.

- [ ] **Step 5: BUILD.gn 등록**

```
"tools/fill_tool.cc",
"tools/fill_tool.h",
```

- [ ] **Step 6: 빌드 확인 + 커밋**

```bash
git commit -m "재작성: FillTool — 통합 로케이터 + isTrusted + macOS Cmd+A + Delete keyUp 수정"
```

---

## Task 7: FindTool 재작성

**Files:**
- Modify: `src/tools/find_tool.h`
- Modify: `src/tools/find_tool.cc`

- [ ] **Step 1: find_tool.h — 파라미터 확장**

기존 `type`/`query` 대신 공통 로케이터(role/name/text/selector/xpath) + exact/visible/enabled/limit 파라미터.

- [ ] **Step 2: find_tool.cc — role/name 검색 경로**

`Accessibility.queryAXTree(role, accessibleName)` → nodes 배열 순회 → 각 노드에 대해 `DOM.describeNode(backendNodeId)` + `DOM.getBoxModel(backendNodeId)` 로 tag/attributes/visible/boundingBox 수집.

- [ ] **Step 3: find_tool.cc — text 검색 경로**

`exact: true` → `queryAXTree(accessibleName=text)`, `exact: false` → `getFullAXTree` + contains 필터. visible/enabled 후필터 적용.

- [ ] **Step 4: find_tool.cc — selector/xpath 경로 유지 + discardSearchResults 추가**

xpath 경로: `DOM.performSearch` 후 반드시 `DOM.discardSearchResults(searchId)` 호출.

- [ ] **Step 5: find_tool.cc — 반환 형식 변경**

각 결과 아이템: `{index, backendNodeId, tag, role, name, description, visible, enabled, boundingBox, attributes}`.

- [ ] **Step 6: 빌드 확인 + 커밋**

```bash
git commit -m "재작성: FindTool — queryAXTree 통합, 반환형 개선, 검색세션 누수 수정"
```

---

## Task 8: Phase 3 기존 도구 로케이터 교체

**Files:**
- Modify: `src/tools/hover_tool.cc` — 자체 ExtractBoxModelCenter 제거, element_locator 사용
- Modify: `src/tools/scroll_tool.cc` — 동일
- Modify: `src/tools/drag_tool.cc` — 동일 + DOM.getDocument 이중호출 제거
- Modify: `src/tools/select_option_tool.cc` — locator 교체 + 사일런트 실패 수정
- Modify: `src/tools/file_upload_tool.cc` — locator 교체
- Modify: `src/tools/element_tool.cc` — locator 교체
- Modify: `src/tools/element_info_tool.cc` — locator 교체

- [ ] **Step 1: hover_tool — 로케이터 교체**

기존 4단계 CDP 체인(getDocument→querySelector→getBoxModel→dispatch)을 `ElementLocator::Locate()` → `DispatchHover(result.x, result.y)` 2단계로 단순화. 로컬 `ExtractBoxModelCenter` 제거, `box_model_util.h` include.

input_schema에 role/name/text/exact 파라미터 추가. 기존 selector/x/y 유지.

- [ ] **Step 2: scroll_tool — 로케이터 교체**

hover와 동일 패턴. selector 모드를 ElementLocator로 교체.

- [ ] **Step 3: drag_tool — 로케이터 교체 + getDocument 이중호출 제거**

start/end 각각 ElementLocator 호출. 기존의 `DOM.getDocument` 2회 호출 제거 (locator 내부에서 처리).
input_schema에 startRole/startName/startText + endRole/endName/endText 파라미터 추가. **어댑터 로직 필요**: `startRole`/`startName` 등을 `{role, name, text}` dict로 변환하여 `ElementLocator::Locate()`에 전달. end도 동일 패턴.

- [ ] **Step 4: select_option_tool — 로케이터 교체 + 사일런트 실패 수정**

`document.querySelector` JS를 `ElementLocator::Locate` → backendNodeId 기반으로 교체. value 모드에서 `selectedIndex` 확인 코드 추가 (존재하지 않는 value 설정 시 에러 반환).

- [ ] **Step 5: file_upload_tool — 로케이터 교체**

기존 `DOM.getDocument→querySelector` 체인을 `ElementLocator::Locate` → backendNodeId로 교체.

- [ ] **Step 6: element_tool — 로케이터 교체**

기존 `DOM.getDocument→querySelector` 체인을 `ElementLocator::Locate`로 교체. input_schema에 공통 로케이터 파라미터 추가.

- [ ] **Step 7: element_info_tool — 로케이터 교체**

element_tool과 동일 패턴.

- [ ] **Step 8: 빌드 확인**

Run: `cd /Users/daniel/chromium/src && autoninja -C out/Default chrome/browser/mcp:mcp`

- [ ] **Step 9: 커밋**

```bash
git commit -m "리팩토링: 7개 도구 로케이터 교체 (hover, scroll, drag, select_option, file_upload, element, element_info)"
```

---

## Task 9: WaitTool actionability 연동 + 타이머 수정

**Files:**
- Modify: `src/tools/wait_tool.h`
- Modify: `src/tools/wait_tool.cc`

- [ ] **Step 1: wait_tool — selector 대기에 가시성 체크 추가**

기존 `!!document.querySelector(...)` JS를 `ElementLocator::Locate` + `DOM.getBoxModel` 성공 여부로 교체. `visible: true` 옵션 지원.

- [ ] **Step 2: wait_tool — per-request 타이머 전환**

멤버 변수 `poll_timer_`, `timeout_timer_` 등을 per-request 구조체 내부로 이동. 동시 wait 요청 간 충돌 방지.

- [ ] **Step 3: 빌드 확인 + 커밋**

```bash
git commit -m "수정: WaitTool — 가시성 체크 연동 + per-request 타이머 전환"
```

---

## Task 10: 레거시 완전 제거 + 단일 경로 전환

**Files:**
- Modify: `src/mcp_server.cc`
- Modify: `src/mcp_server.h`

- [ ] **Step 1: RegisterBrowserInfoTool 제거**

L802 `RegisterBrowserInfoTool()` 호출 제거. BrowserInfoTool은 이미 tool_registry_에 등록되어 있으므로 (이미 활성 상태) 추가 등록 불필요. 선언/구현 제거.

- [ ] **Step 2: tools_ 맵 및 관련 멤버 제거**

`mcp_server.h` L337: `std::unordered_map<std::string, McpToolDefinition> tools_;` 제거.
`McpToolDefinition` struct가 tools_ 전용이면 함께 제거.
`current_inline_client_id_` 멤버 변수 (L334) 제거 — 레거시 핸들러에서만 사용하던 변수. 이 변수를 참조하는 다른 코드(`GetActiveSession()` 등)가 있으면 함께 수정/제거.

- [ ] **Step 3: HandleToolsCall — 레거시 경로 제거**

L655-664의 `tools_.find` 분기 삭제. `tool_registry_->DispatchToolCall()` 단일 경로만 유지.

- [ ] **Step 4: HandleToolsList — 레거시 경로 제거**

L541-548의 `tools_` 순회 삭제. `tool_registry_->BuildToolListResponse()` 단일 호출만 유지.

- [ ] **Step 5: RegisterBuiltinTools 정리**

더 이상 인라인 등록이 없으므로, 함수 이름을 `InitializeToolRegistry` 등으로 변경 고려 (선택).

- [ ] **Step 6: 빌드 확인**

Run: `cd /Users/daniel/chromium/src && autoninja -C out/Default chrome/browser/mcp:mcp`

- [ ] **Step 7: 커밋**

```bash
git commit -m "마이그레이션 완료: 레거시 인라인 도구 전부 제거, tool_registry_ 단일 경로"
```

---

## Task 11: 기타 버그 수정

**Files:**
- Modify: `src/tools/download_tool.cc` (JS 이스케이프)
- Modify: `src/tools/dialog_tool.cc` (autoHandle 에러 전파)
- Modify: `src/tools/keyboard_tool.cc` (modifier 단독 press)

- [ ] **Step 1: download_tool — JS 문자열 이스케이프에 큰따옴표 추가**

`download_tool.cc`에서 JS 코드 문자열을 구성하는 부분을 찾아 `"` → `\"` 이스케이프 추가. 함수명은 실제 코드에서 확인 필요 (인라인 이스케이프 처리일 수 있음).

- [ ] **Step 2: dialog_tool — autoHandle 실패 시 에러 콜백 전파**

`OnJavaScriptDialogOpening`의 auto-handle 분기에서 `SendCdpCommand` 콜백을 fire-and-forget이 아닌 에러 확인 콜백으로 변경.

- [ ] **Step 3: keyboard_tool — modifier 단독 press 지원**

`KeyNameToVirtualKeyCode`에 `Shift`(16), `Control`(17), `Alt`(18), `Meta`(91) 매핑 추가.

- [ ] **Step 4: 빌드 확인 + 커밋**

```bash
git commit -m "버그 수정: download 이스케이프, dialog 에러 전파, keyboard modifier 키"
```

---

## Task 12: 통합 테스트 + INSTRUCTION.md 업데이트

**Files:**
- Modify: `INSTRUCTION.md` (새 파라미터 문서화)
- Modify: `MCP-SERVER.md` (도구 스키마 업데이트)

- [ ] **Step 1: 데몬 + Chromium 실행하여 기본 동작 확인**

빌드된 Chromium으로 MCP 소켓 연결 → `tools/list` 호출 → 35개 도구 모두 반환 확인. 레거시 도구 이름(click, fill, navigate 등)이 정상 노출 확인.

- [ ] **Step 2: role/name 기반 클릭 테스트**

Google 검색 페이지에서:
```json
{"tool": "click", "arguments": {"role": "button", "name": "Google 검색"}}
```

- [ ] **Step 3: text 기반 클릭 테스트**

```json
{"tool": "click", "arguments": {"text": "Google 검색"}}
```

- [ ] **Step 4: 기존 selector 호환 테스트**

```json
{"tool": "click", "arguments": {"selector": "input[name='btnK']"}}
```

- [ ] **Step 5: auto-wait 동작 확인**

SPA 페이지에서 동적 렌더링 후 나타나는 요소에 대해 `role`+`name` 으로 클릭 → 5초 내 성공 확인.

- [ ] **Step 6: find 도구 개선 확인**

```json
{"tool": "find", "arguments": {"role": "button"}}
```
→ 반환에 `role`, `name`, `visible`, `enabled`, `boundingBox` 포함 확인.

- [ ] **Step 7: INSTRUCTION.md 업데이트**

새 로케이터 파라미터(role, name, text, exact, timeout, force) 사용법 문서화. 기존 selector 호환성 유지 명시.

- [ ] **Step 8: MCP-SERVER.md 업데이트**

도구별 파라미터 스키마 업데이트 (공통 로케이터 + timeout/force 추가).

- [ ] **Step 9: 커밋**

```bash
git commit -m "문서: 통합 로케이터 파라미터 + 테스트 결과 업데이트"
```
