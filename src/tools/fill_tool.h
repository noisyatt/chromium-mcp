// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_FILL_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_FILL_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/tools/actionability_checker.h"
#include "chrome/browser/mcp/tools/box_model_util.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// FillTool: 통합 로케이터(ElementLocator) + ActionabilityChecker를 사용하는
// 입력 도구.
//
// 파라미터:
//   로케이터 (하나 이상 필요):
//     role, name, text, selector, xpath, ref, exact
//   auto-wait:
//     timeout (ms, 기본 5000), force (bool, 기본 false)
//   fill 전용:
//     value (string, 필수)
//
// 실행 흐름:
//   ActionabilityChecker::VerifyAndLocate(kFill) → OnActionable
//   → DOM.focus(backendNodeId) → OnFocused
//   → Input.dispatchKeyEvent(keyDown, Cmd+A, modifier=4) → OnSelectAllKeyDown
//   → Input.dispatchKeyEvent(keyUp,  Cmd+A, modifier=4) → OnSelectAllKeyUp
//   → Input.dispatchKeyEvent(keyDown, Delete, vk=46)    → OnDeleteKeyDown
//   → Input.dispatchKeyEvent(keyUp,   Delete, vk=46)    → OnDeleteKeyUp
//   → Input.insertText({text: value})                   → OnInsertText → 완료
//
// 핵심 개선:
//   - isTrusted: true — CDP Input 도메인 사용 (Runtime.evaluate 방식 대비 신뢰 이벤트)
//   - macOS Cmd+A — modifier=4 (Meta) + commands: ["selectAll"]
//   - Delete keyUp 추가 — 기존 dom_tool.cc 버그 수정
//   - ActionabilityChecker kFill — EDITABLE 체크 포함 (readonly 요소 방지)
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
  // ActionabilityChecker 콜백: 요소 actionable 확인 후 DOM.focus 발송
  void OnActionable(const std::string& value,
                    McpSession* session,
                    base::OnceCallback<void(base::Value)> callback,
                    ElementLocator::Result result,
                    std::string error);

  // DOM.focus 완료 → Cmd+A keyDown 발송
  void OnFocused(const std::string& value,
                 McpSession* session,
                 base::OnceCallback<void(base::Value)> callback,
                 base::Value response);

  // Cmd+A keyDown 완료 → Cmd+A keyUp 발송
  void OnSelectAllKeyDown(const std::string& value,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // Cmd+A keyUp 완료 → Delete keyDown 발송
  void OnSelectAllKeyUp(const std::string& value,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback,
                        base::Value response);

  // Delete keyDown 완료 → Delete keyUp 발송
  void OnDeleteKeyDown(const std::string& value,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // Delete keyUp 완료 → Input.insertText 발송
  void OnDeleteKeyUp(const std::string& value,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback,
                     base::Value response);

  // Input.insertText 완료 → 성공 결과 반환
  void OnInsertText(base::OnceCallback<void(base::Value)> callback,
                    base::Value response);

  // ActionabilityChecker 인스턴스 (per-Execute, stateless)
  ActionabilityChecker actionability_checker_;

  base::WeakPtrFactory<FillTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_FILL_TOOL_H_
