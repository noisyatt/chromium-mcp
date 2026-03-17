// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_NAVIGATE_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_NAVIGATE_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// URL 이동, 뒤로/앞으로/새로고침 등 페이지 탐색을 처리하는 도구.
// CDP Page.navigate / Page.goBack / Page.goForward / Page.reload 를 사용하며,
// Page.loadEventFired 이벤트를 수신해 네비게이션 완료 후 결과를 반환한다.
class NavigateTool : public McpTool {
 public:
  NavigateTool();
  ~NavigateTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // CDP 명령 전송 후 loadEventFired 이벤트 수신을 포함한 완료 처리.
  // |method|: 실행할 CDP 메서드명
  // |params|: CDP 파라미터
  // |callback|: 최종 결과 콜백
  void SendNavigationCommand(const std::string& method,
                             base::DictValue params,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback);

  // CDP 응답을 받아 성공/실패 결과를 구성한 뒤 callback 호출.
  void OnNavigationCommandResponse(
      base::OnceCallback<void(base::Value)> callback,
      base::Value response);

  base::WeakPtrFactory<NavigateTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_NAVIGATE_TOOL_H_
