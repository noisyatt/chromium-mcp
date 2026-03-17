// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_SELECT_OPTION_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_SELECT_OPTION_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// SelectOptionTool: <select> 요소에서 옵션을 선택하는 도구.
//
// Runtime.evaluate를 사용해 JavaScript를 실행하여 값을 설정하고
// 'change' 이벤트를 버블링하며 발생시킨다.
//
// 세 가지 선택 방식:
//   1. value: option 요소의 value 속성으로 단일 선택
//   2. text: option 요소의 텍스트 내용으로 단일 선택
//   3. values: value 배열로 다중 선택 (multiple select 요소)
//
// 실행 흐름:
//   1. JS 코드를 문자열로 조합 (selector + value/text/values)
//   2. Runtime.evaluate 로 실행
//   3. 결과 확인 후 MCP 응답 반환
class SelectOptionTool : public McpTool {
 public:
  SelectOptionTool();
  ~SelectOptionTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // Runtime.evaluate 완료 콜백
  void OnEvaluateComplete(base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // CDP 에러 처리 헬퍼
  static bool HandleCdpError(const base::Value& response,
                              const std::string& step_name,
                              base::OnceCallback<void(base::Value)>& callback);

  // JavaScript 문자열 이스케이프 (single quote 이스케이프)
  static std::string EscapeJsString(const std::string& str);

  base::WeakPtrFactory<SelectOptionTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_SELECT_OPTION_TOOL_H_
