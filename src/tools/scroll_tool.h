// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_SCROLL_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_SCROLL_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// ScrollTool: 페이지 또는 특정 요소에서 스크롤을 발생시키는 도구.
//
// 사용 방법:
//   1. direction 지정: direction(up/down/left/right) + amount(틱 수)로 스크롤
//   2. 좌표 직접 지정: x, y, deltaX, deltaY 파라미터 사용
//   3. 로케이터 지정: role/name/text/selector 등으로 요소를 찾아 중심 좌표에서 스크롤
//   4. toTop/toBottom: Runtime.evaluate로 window.scrollTo 호출
//
// direction 모드에서 스크롤 양 계산:
//   각 틱당 픽셀 수 = kScrollPixelsPerTick (기본 120px).
//   amount=3이면 총 360px 스크롤.
//   down이면 deltaY=-360 (아래로), up이면 deltaY=+360 (위로).
//
// 로케이터 모드 실행 흐름:
//   ElementLocator::Locate() → DispatchScroll(result.x, result.y, ...)
class ScrollTool : public McpTool {
 public:
  ScrollTool();
  ~ScrollTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 좌표로 바로 스크롤 (mouseWheel 이벤트)
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

  // ElementLocator 콜백: 좌표 해상도 완료 후 스크롤 발송
  void OnLocated(double delta_x,
                 double delta_y,
                 McpSession* session,
                 base::OnceCallback<void(base::Value)> callback,
                 std::optional<ElementLocator::Result> result,
                 std::string error);

  // Input.dispatchMouseEvent(mouseWheel) 완료 콜백
  void OnScrollDispatched(base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  ElementLocator locator_;

  base::WeakPtrFactory<ScrollTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_SCROLL_TOOL_H_
