// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_SCROLL_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_SCROLL_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// ScrollTool: 페이지 또는 특정 요소에서 스크롤을 발생시키는 도구.
//
// 사용 방법:
//   1. direction 지정: direction(up/down/left/right) + amount(틱 수)로 스크롤
//   2. 좌표 직접 지정: x, y, deltaX, deltaY 파라미터 사용
//   3. selector 지정: selector 파라미터로 요소를 찾아 중심 좌표에서 스크롤
//   4. toTop/toBottom: Runtime.evaluate로 window.scrollTo 호출
//
// direction 모드에서 스크롤 양 계산:
//   각 틱당 픽셀 수 = kScrollPixelsPerTick (기본 120px).
//   amount=3이면 총 360px 스크롤.
//   down이면 deltaY=-360 (아래로), up이면 deltaY=+360 (위로).
//
// selector 모드 실행 흐름:
//   1. DOM.getDocument  → rootNodeId 획득
//   2. DOM.querySelector → nodeId 획득
//   3. DOM.getBoxModel  → 요소 중심 좌표 계산
//   4. Input.dispatchMouseEvent (mouseWheel) → 스크롤 발송
class ScrollTool : public McpTool {
 public:
  ScrollTool();
  ~ScrollTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // selector 없이 좌표로 바로 스크롤 (mouseWheel 이벤트)
  void DispatchScroll(double x,
                      double y,
                      double delta_x,
                      double delta_y,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback);

  // toTop/toBottom: Runtime.evaluate로 window.scrollTo 실행
  void EvaluateScroll(const std::string& expression,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback);

  // Runtime.evaluate 완료 콜백
  void OnEvaluateScroll(base::OnceCallback<void(base::Value)> callback,
                        base::Value response);

  // selector 모드: DOM.getDocument 호출
  void GetDocumentRoot(const std::string& selector,
                       double delta_x,
                       double delta_y,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback);

  // DOM.getDocument 응답 후 DOM.querySelector 호출
  void OnGetDocumentRoot(const std::string& selector,
                         double delta_x,
                         double delta_y,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback,
                         base::Value response);

  // DOM.querySelector 응답 후 DOM.getBoxModel 호출
  void OnQuerySelector(double delta_x,
                       double delta_y,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // DOM.getBoxModel 응답 후 중심좌표로 스크롤 발송
  void OnGetBoxModel(double delta_x,
                     double delta_y,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback,
                     base::Value response);

  // Input.dispatchMouseEvent(mouseWheel) 완료 콜백
  void OnScrollDispatched(base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // CDP 에러 처리 헬퍼
  static bool HandleCdpError(const base::Value& response,
                              const std::string& step_name,
                              base::OnceCallback<void(base::Value)>& callback);

  base::WeakPtrFactory<ScrollTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_SCROLL_TOOL_H_
