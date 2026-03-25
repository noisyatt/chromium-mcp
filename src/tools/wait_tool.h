// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_WAIT_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_WAIT_TOOL_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// WaitTool: 특정 조건이 충족될 때까지 대기하는 도구.
//
// 대기 유형 (type 파라미터):
//   - time        : 지정된 시간(duration ms)만큼 단순 대기
//   - text        : 페이지에 특정 텍스트가 나타날 때까지 폴링 대기
//   - textGone    : 페이지에서 특정 텍스트가 사라질 때까지 폴링 대기
//   - selector    : 요소가 DOM에 나타나고 가시 상태가 될 때까지 폴링 대기
//                   (role/name/text/selector/xpath 로케이터 모두 지원)
//   - navigation  : Page.loadEventFired 이벤트 대기 (페이지 로드 완료)
//   - networkIdle : 일정 시간(idleTime ms) 동안 XHR/Fetch 요청이 없으면 완료
//
// 구현 방식:
//   - time: WaitContext 내 OneShotTimer로 duration ms 후 완료.
//   - text/textGone: WaitContext 내 RepeatingTimer로 폴링, Runtime.evaluate 조건 확인.
//   - selector: WaitContext 내 RepeatingTimer로 폴링, ElementLocator::Locate() +
//     DOM.getBoxModel 성공 여부로 가시성 확인. visible=false 이면 DOM 존재만 확인.
//   - navigation: Page.loadEventFired 이벤트를 일회성으로 수신.
//   - networkIdle: Network.requestWillBeSent / Network.loadingFinished 이벤트 모니터링.
//   - 모든 유형: WaitContext 내 timeout 타이머로 관리, 동시 요청 간 충돌 없음.
class WaitTool : public McpTool {
 public:
  WaitTool();
  ~WaitTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // -----------------------------------------------------------------------
  // per-request 타이머 컨텍스트 — 동시 요청 간 타이머 충돌 방지
  // -----------------------------------------------------------------------

  // 폴링 기반 대기에서 공유하는 컨텍스트 구조체
  struct WaitContext {
    WaitContext();
    ~WaitContext();

    std::string condition_label;  // 로그/에러 메시지용 설명
    std::string js_expression;    // 평가할 JS 표현식 (text/textGone 전용)
    bool use_locator = false;     // true이면 ElementLocator로 가시성 확인
    int timeout_ms;
    int elapsed_ms = 0;
    int interval_ms;
    McpSession* session;          // raw pointer (생명주기 주의)
    base::OnceCallback<void(base::Value)> callback;
    bool completed = false;       // 중복 완료 방지

    // per-request 타이머 (멤버로 이동하여 동시 요청 충돌 방지)
    base::RepeatingTimer poll_timer;
    base::OneShotTimer timeout_timer;

    // ElementLocator 인스턴스 (selector 모드에서 per-request로 생성)
    std::unique_ptr<ElementLocator> locator;
    // 로케이터 파라미터 저장 (폴링 시 반복 사용)
    base::Value::Dict locator_params;
    // visible=false 이면 DOM 존재만 확인 (박스 모델 없어도 통과)
    bool require_visible = true;
  };

  // -----------------------------------------------------------------------
  // 대기 유형별 진입점
  // -----------------------------------------------------------------------

  void WaitForTime(int duration_ms,
                   base::OnceCallback<void(base::Value)> callback);

  void WaitForText(const std::string& text,
                   bool wait_gone,
                   int timeout_ms,
                   int interval_ms,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // selector 모드: ElementLocator + DOM.getBoxModel 가시성 확인
  void WaitForSelector(base::Value::Dict locator_params,
                       bool require_visible,
                       int timeout_ms,
                       int interval_ms,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback);

  // JS 표현식 폴링 공통 구현 (text/textGone)
  void WaitForCondition(const std::string& js_expression,
                        const std::string& condition_label,
                        int timeout_ms,
                        int interval_ms,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  void WaitForNavigation(int timeout_ms,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  void WaitForNetworkIdle(int timeout_ms,
                          int idle_time_ms,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback);

  // -----------------------------------------------------------------------
  // 폴링 콜백 (JS 조건 평가)
  // -----------------------------------------------------------------------

  void OnPollTimer(std::shared_ptr<WaitContext> ctx);
  void OnEvaluateResponse(std::shared_ptr<WaitContext> ctx,
                          base::Value response);
  void OnTimeout(std::shared_ptr<WaitContext> ctx);

  // -----------------------------------------------------------------------
  // selector 폴링 콜백 (ElementLocator 결과)
  // -----------------------------------------------------------------------

  void OnSelectorPollTimer(std::shared_ptr<WaitContext> ctx);
  void OnLocateResult(std::shared_ptr<WaitContext> ctx,
                      std::optional<ElementLocator::Result> result,
                      std::string error);

  base::WeakPtrFactory<WaitTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_WAIT_TOOL_H_
