// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_CLICK_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_CLICK_TOOL_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/tools/actionability_checker.h"
#include "chrome/browser/mcp/tools/box_model_util.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// ClickTool: 통합 로케이터(ElementLocator) + ActionabilityChecker를 사용하는
// 클릭 도구.
//
// 파라미터:
//   로케이터 (하나 이상 필요):
//     role, name, text, selector, xpath, ref, exact
//   auto-wait:
//     timeout (ms, 기본 5000), force (bool, 기본 false)
//   click 전용:
//     button ("left"/"right"/"middle", 기본 "left")
//     waitForNavigation (bool, 기본 false)
//
// 실행 흐름:
//   ActionabilityChecker::VerifyAndLocate → OnActionable
//   → Input.dispatchMouseEvent(mousePressed) → OnMousePressed
//   → Input.dispatchMouseEvent(mouseReleased) → OnMouseReleased
//   → (waitForNavigation ? WaitForLoad() : Complete)
class ClickTool : public McpTool {
 public:
  ClickTool();
  ~ClickTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // ActionabilityChecker 콜백: 요소가 actionable 상태임이 확인된 후 호출
  void OnActionable(const std::string& button,
                    bool wait_for_navigation,
                    McpSession* session,
                    base::OnceCallback<void(base::Value)> callback,
                    ElementLocator::Result result,
                    std::string error);

  // mousePressed 완료 후 mouseReleased 발송
  void OnMousePressed(double x,
                      double y,
                      const std::string& button,
                      bool wait_for_navigation,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback,
                      base::Value response);

  // mouseReleased 완료 후 waitForNavigation 처리 또는 즉시 완료
  void OnMouseReleased(bool wait_for_navigation,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // waitForNavigation=true 시 Page.loadEventFired 이벤트 대기 설정
  void WaitForLoad(McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // Page.loadEventFired 이벤트 수신 처리 (shared_ptr로 중복 호출 방지)
  void OnLoadEventFired(
      McpSession* session,
      std::shared_ptr<base::OnceCallback<void(base::Value)>> shared_callback,
      std::shared_ptr<base::OneShotTimer> timer,
      const std::string& event_name,
      const base::DictValue& params);

  // waitForNavigation 타임아웃 (5초) — per-request 타이머 수신
  void OnLoadTimeout(
      McpSession* session,
      std::shared_ptr<base::OnceCallback<void(base::Value)>> shared_callback,
      std::shared_ptr<base::OneShotTimer> timer);

  // ActionabilityChecker 인스턴스 (per-Execute, stateless)
  ActionabilityChecker actionability_checker_;

  base::WeakPtrFactory<ClickTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_CLICK_TOOL_H_
