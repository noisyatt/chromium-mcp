// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_HOVER_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_HOVER_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// HoverTool: 마우스 커서를 요소 위에 위치시켜 호버 이벤트를 발생시키는 도구.
//
// 로케이터 우선순위: role/name > text > selector > xpath > ref
// 좌표 직접 지정: x, y 파라미터 사용
//
// 실행 흐름 (로케이터 지정 시):
//   ElementLocator::Locate() → DispatchHover(result.x, result.y)
//
// 실행 흐름 (좌표 직접 지정 시):
//   Input.dispatchMouseEvent (mouseMoved) → 호버 이벤트 발송
class HoverTool : public McpTool {
 public:
  HoverTool();
  ~HoverTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 좌표로 직접 mouseMoved 이벤트 발송
  void DispatchHover(double x,
                     double y,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // ElementLocator 콜백: 좌표 해상도 완료 후 호버 발송
  void OnLocated(McpSession* session,
                 base::OnceCallback<void(base::Value)> callback,
                 std::optional<ElementLocator::Result> result,
                 std::string error);

  // mouseMoved 완료 콜백
  void OnHoverDispatched(base::OnceCallback<void(base::Value)> callback,
                         base::Value response);

  ElementLocator locator_;

  base::WeakPtrFactory<HoverTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_HOVER_TOOL_H_
