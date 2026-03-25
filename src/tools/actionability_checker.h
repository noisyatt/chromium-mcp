// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_ACTIONABILITY_CHECKER_H_
#define CHROME_BROWSER_MCP_TOOLS_ACTIONABILITY_CHECKER_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

class McpSession;

// ActionabilityChecker: 요소의 동작 가능 여부를 검증하고 자동 대기하는 클래스.
//
// ElementLocator로 요소를 찾은 후, 실제 동작 가능한 상태(가시성, 안정성,
// 활성 여부 등)인지 검증한다. 조건 미충족 시 최대 timeout_ms 동안 폴링하며
// 재시도한다.
//
// 동시 요청 안전성: 모든 상태는 per-request PollContext(shared_ptr)에 보관되어
// ActionabilityChecker 인스턴스 자체는 stateless하다.
//
// 체크 순서:
//   1. VISIBLE   — DOM.getBoxModel 성공 여부
//   2. IN_VIEWPORT — 좌표가 뷰포트 내인지 + 필요 시 scrollIntoViewIfNeeded
//   3. STABLE    — 50ms 간격 좌표 측정 2회, 차이 2px 이내
//   4. ENABLED   — Accessibility.getPartialAXTree → disabled 속성 확인
//   5. EDITABLE  — 같은 AX 응답에서 readonly 속성 확인 (fill 전용)
class ActionabilityChecker {
 public:
  enum class ActionType {
    kClick,
    kFill,
    kHover,
    kScroll,
    kDrag,
    kSelectOption,
    kFileUpload,
  };

  struct Options {
    int timeout_ms = 5000;
    int poll_interval_ms = 200;
    bool force = false;
  };

  // 성공: {Result, ""} / 실패: {nullopt Result, "에러 메시지"}
  using Callback = base::OnceCallback<void(ElementLocator::Result result,
                                           std::string error)>;

  ActionabilityChecker();
  ~ActionabilityChecker();

  // 진입점: 요소를 찾고 actionability를 검증한다.
  // force=true 이면 Locate만 수행하고 체크를 건너뛴다.
  void VerifyAndLocate(McpSession* session,
                       const base::Value::Dict& params,
                       ActionType action,
                       Options options,
                       Callback callback);

 private:
  // -----------------------------------------------------------------------
  // per-request 컨텍스트 (모든 비동기 상태를 보유)
  // -----------------------------------------------------------------------
  struct PollContext {
    PollContext();
    ~PollContext();

    ElementLocator locator;
    base::OneShotTimer poll_timer;
    base::OneShotTimer timeout_timer;
    Callback callback;
    McpSession* session = nullptr;
    ActionType action = ActionType::kClick;
    base::Value::Dict params;
    Options options;
    bool timed_out = false;
  };

  // -----------------------------------------------------------------------
  // 폴링 루프
  // -----------------------------------------------------------------------

  // locator.Locate() 호출 → OnLocateResult
  void StartPoll(std::shared_ptr<PollContext> ctx);

  // Locate 결과 수신
  void OnLocateResult(std::shared_ptr<PollContext> ctx,
                      std::optional<ElementLocator::Result> result,
                      std::string error);

  // -----------------------------------------------------------------------
  // 순차 체크 체인
  // -----------------------------------------------------------------------

  // 1단계: VISIBLE — DOM.getBoxModel 성공 여부
  void CheckVisible(std::shared_ptr<PollContext> ctx,
                    ElementLocator::Result result);
  void OnCheckVisibleResponse(std::shared_ptr<PollContext> ctx,
                              ElementLocator::Result result,
                              base::Value response);

  // 2단계: IN_VIEWPORT + scrollIntoViewIfNeeded
  void CheckInViewport(std::shared_ptr<PollContext> ctx,
                       ElementLocator::Result result);
  void ScrollIntoViewIfNeeded(std::shared_ptr<PollContext> ctx,
                              ElementLocator::Result result);
  void OnScrollResponse(std::shared_ptr<PollContext> ctx,
                        ElementLocator::Result result,
                        base::Value response);

  // 3단계: STABLE — 50ms 간격 2회 측정, 차이 2px 이내
  void CheckStable(std::shared_ptr<PollContext> ctx,
                   ElementLocator::Result result);
  void OnStableSecondMeasure(std::shared_ptr<PollContext> ctx,
                             ElementLocator::Result result,
                             double first_x,
                             double first_y,
                             base::Value response);

  // 4단계: ENABLED + 5단계: EDITABLE
  void CheckEnabled(std::shared_ptr<PollContext> ctx,
                    ElementLocator::Result result);
  void OnCheckEnabledResponse(std::shared_ptr<PollContext> ctx,
                              ElementLocator::Result result,
                              base::Value response);

  // -----------------------------------------------------------------------
  // 완료/재시도/타임아웃
  // -----------------------------------------------------------------------

  // 모든 체크 통과 → callback 성공 호출
  void Complete(std::shared_ptr<PollContext> ctx,
                ElementLocator::Result result);

  // 체크 실패 → timed_out 이면 에러 콜백, 아니면 poll_timer로 재시도
  void RetryOrFail(std::shared_ptr<PollContext> ctx,
                   const std::string& reason);

  // timeout_timer 만료 → timed_out = true
  void OnTimeout(std::shared_ptr<PollContext> ctx);

  // -----------------------------------------------------------------------
  // 액션별 필요 체크 (static)
  // -----------------------------------------------------------------------
  static bool NeedsVisible(ActionType action);
  static bool NeedsInViewport(ActionType action);
  static bool NeedsStable(ActionType action);
  static bool NeedsEnabled(ActionType action);
  static bool NeedsEditable(ActionType action);
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_ACTIONABILITY_CHECKER_H_
