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

namespace mcp {

// WaitTool: 특정 조건이 충족될 때까지 대기하는 도구.
//
// 대기 유형 (type 파라미터):
//   - time        : 지정된 시간(duration ms)만큼 단순 대기
//   - text        : 페이지에 특정 텍스트가 나타날 때까지 폴링 대기
//   - textGone    : 페이지에서 특정 텍스트가 사라질 때까지 폴링 대기
//   - selector    : CSS 선택자에 해당하는 요소가 DOM에 나타날 때까지 폴링 대기
//   - navigation  : Page.loadEventFired 이벤트 대기 (페이지 로드 완료)
//   - networkIdle : 일정 시간(idleTime ms) 동안 XHR/Fetch 요청이 없으면 완료
//
// 구현 방식:
//   - time: base::OneShotTimer로 duration ms 후 완료.
//   - text/textGone/selector: base::RepeatingTimer로 pollingInterval ms마다 폴링.
//     Runtime.evaluate를 호출해 조건을 확인하고, 충족 시 완료.
//   - navigation: Page.loadEventFired 이벤트를 일회성으로 수신.
//   - networkIdle: Network.requestWillBeSent / Network.loadingFinished 이벤트를
//     모니터링하여 진행 중인 요청이 0개인 상태가 idleTime ms 이상 유지되면 완료.
//   - 모든 유형에서 timeout 초과 시 오류를 반환한다.
class WaitTool : public McpTool {
 public:
  WaitTool();
  ~WaitTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 지정 시간만큼 단순 대기
  void WaitForTime(int duration_ms,
                   base::OnceCallback<void(base::Value)> callback);

  // 텍스트가 나타날 때까지 폴링
  void WaitForText(const std::string& text,
                   bool wait_gone,  // true이면 텍스트가 사라질 때까지 대기
                   int timeout_ms,
                   int interval_ms,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // CSS 선택자에 해당하는 요소가 나타날 때까지 폴링
  void WaitForSelector(const std::string& selector,
                       int timeout_ms,
                       int interval_ms,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback);

  // JavaScript 표현식이 truthy를 반환할 때까지 폴링 (내부 공통 구현)
  void WaitForCondition(const std::string& js_expression,
                        const std::string& condition_label,
                        int timeout_ms,
                        int interval_ms,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  // Page.loadEventFired 이벤트 대기
  void WaitForNavigation(int timeout_ms,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  // Network idle 대기: 진행 중인 요청이 없는 상태가 idle_time_ms 이상 유지
  void WaitForNetworkIdle(int timeout_ms,
                          int idle_time_ms,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback);

  // 폴링 상태를 관리하는 컨텍스트 구조체
  struct PollContext {
    std::string condition_label;     // 로그/에러 메시지용 설명
    std::string js_expression;       // 평가할 JS 표현식
    int timeout_ms;
    int elapsed_ms = 0;
    int interval_ms;
    McpSession* session;             // raw pointer (생명주기 주의)
    base::OnceCallback<void(base::Value)> callback;
    bool completed = false;          // 중복 완료 방지
  };

  // 폴링 타이머 콜백: Runtime.evaluate로 조건 평가
  void OnPollTimer(std::shared_ptr<PollContext> ctx);

  // Runtime.evaluate 응답 처리
  void OnEvaluateResponse(std::shared_ptr<PollContext> ctx,
                          base::Value response);

  // timeout 초과 처리
  void OnTimeout(std::shared_ptr<PollContext> ctx);

  // 폴링 타이머 (text/textGone/selector 대기)
  base::RepeatingTimer poll_timer_;

  // timeout 타이머 (모든 폴링 유형 공용)
  base::OneShotTimer timeout_timer_;

  // 단순 시간 대기 타이머 (type=time 전용)
  base::OneShotTimer time_wait_timer_;

  // networkIdle 상태 타이머 (idle 지속 시간 측정)
  base::OneShotTimer network_idle_timer_;

  base::WeakPtrFactory<WaitTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_WAIT_TOOL_H_
