// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_SELECT_OPTION_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_SELECT_OPTION_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// SelectOptionTool: <select> 요소에서 옵션을 선택하는 도구.
//
// Runtime.evaluate를 사용해 JavaScript를 실행하여 값을 설정하고
// 'change' 이벤트를 버블링하며 발생시킨다.
//
// 요소 탐색 방법:
//   role/name, text, selector, xpath, ref 등 통합 로케이터 지원.
//   로케이터가 없으면 기존처럼 selector(CSS) 파라미터로 직접 querySelector 실행.
//
// 세 가지 선택 방식:
//   1. value: option 요소의 value 속성으로 단일 선택
//      (사일런트 실패 방지: selectedIndex 확인으로 존재하지 않는 value 감지)
//   2. text: option 요소의 텍스트 내용으로 단일 선택
//   3. values: value 배열로 다중 선택 (multiple select 요소)
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

  // JavaScript 문자열 이스케이프 (single quote 이스케이프)
  static std::string EscapeJsString(const std::string& str);

  base::WeakPtrFactory<SelectOptionTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_SELECT_OPTION_TOOL_H_
