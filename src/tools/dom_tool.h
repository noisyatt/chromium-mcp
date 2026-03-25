// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_DOM_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_DOM_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// FillTool: 입력 필드에 텍스트 값을 입력하는 도구.
//
// 실행 흐름:
//   1. DOM.getDocument + DOM.querySelector  → nodeId 획득
//   2. DOM.focus                            → 요소 포커스
//   3. Input.dispatchKeyEvent (Ctrl+A)      → 전체 선택
//   4. Input.dispatchKeyEvent (Delete)      → 기존 값 삭제
//   5. Input.insertText                     → 새 값 삽입
class FillTool : public McpTool {
 public:
  FillTool();
  ~FillTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // Step 1: DOM.getDocument 로 루트 nodeId 확보.
  void GetDocumentRoot(const std::string& selector,
                       const std::string& value,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback);

  // Step 2: DOM.querySelector 호출.
  void OnGetDocumentRoot(const std::string& selector,
                         const std::string& value,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback,
                         base::Value response);

  // Step 3: querySelector 응답으로 nodeId 획득 후 DOM.focus 호출.
  void OnQuerySelector(const std::string& value,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // Step 4: 포커스 완료 후 Ctrl+A (전체 선택) keyDown 발송.
  void OnFocused(const std::string& value,
                 McpSession* session,
                 base::OnceCallback<void(base::Value)> callback,
                 base::Value response);

  // Step 5: Ctrl+A keyUp 발송.
  void OnSelectAllKeyDown(const std::string& value,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // Step 6: Delete key 발송으로 기존 값 삭제.
  void OnSelectAllKeyUp(const std::string& value,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback,
                        base::Value response);

  // Step 7: Delete 완료 후 Input.insertText 로 새 값 삽입.
  void OnDeleteKey(const std::string& value,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback,
                   base::Value response);

  // Step 8: 삽입 완료 — 성공 결과 반환.
  void OnInsertText(base::OnceCallback<void(base::Value)> callback,
                    base::Value response);

  // CDP 에러 처리 헬퍼 (ClickTool과 동일 패턴)
  static bool HandleCdpError(const base::Value& response,
                              const std::string& step_name,
                              base::OnceCallback<void(base::Value)>& callback);

  base::WeakPtrFactory<FillTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_DOM_TOOL_H_
