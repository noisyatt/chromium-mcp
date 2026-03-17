// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_HOVER_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_HOVER_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// HoverTool: 마우스 커서를 요소 위에 위치시켜 호버 이벤트를 발생시키는 도구.
//
// 두 가지 사용 방법:
//   1. selector 지정: CSS 셀렉터로 요소를 찾아 중심 좌표에서 호버
//   2. 좌표 직접 지정: x, y 파라미터로 특정 위치에서 호버
//
// 실행 흐름 (selector 지정 시):
//   1. DOM.getDocument  → rootNodeId 획득
//   2. DOM.querySelector → nodeId 획득
//   3. DOM.getBoxModel  → 요소 중심 좌표 계산
//   4. Input.dispatchMouseEvent (mouseMoved) → 호버 이벤트 발송
//
// 실행 흐름 (좌표 직접 지정 시):
//   1. Input.dispatchMouseEvent (mouseMoved) → 호버 이벤트 발송
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

  // selector 모드: DOM.getDocument 호출
  void GetDocumentRoot(const std::string& selector,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback);

  // DOM.getDocument 응답 후 DOM.querySelector 호출
  void OnGetDocumentRoot(const std::string& selector,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback,
                         base::Value response);

  // DOM.querySelector 응답 후 DOM.getBoxModel 호출
  void OnQuerySelector(McpSession* session,
                       base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // DOM.getBoxModel 응답 후 중심좌표로 호버 발송
  void OnGetBoxModel(McpSession* session,
                     base::OnceCallback<void(base::Value)> callback,
                     base::Value response);

  // mouseMoved 완료 콜백
  void OnHoverDispatched(base::OnceCallback<void(base::Value)> callback,
                         base::Value response);

  // CDP 에러 처리 헬퍼
  static bool HandleCdpError(const base::Value& response,
                              const std::string& step_name,
                              base::OnceCallback<void(base::Value)>& callback);

  base::WeakPtrFactory<HoverTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_HOVER_TOOL_H_
