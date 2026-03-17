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

// ClickTool: CSS 셀렉터 또는 ref로 DOM 요소를 클릭하는 도구.
//
// 실행 흐름:
//   1. DOM.querySelector  → nodeId 획득
//   2. DOM.getBoxModel    → 요소의 bounding box 좌표 계산 (중심점)
//   3. Input.dispatchMouseEvent (mousePressed)
//   4. Input.dispatchMouseEvent (mouseReleased)
//   5. waitForNavigation == true 이면 Page.loadEventFired 대기
class ClickTool : public McpTool {
 public:
  ClickTool();
  ~ClickTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // Step 1: DOM.getDocument 로 루트 nodeId를 확보한 후
  //         DOM.querySelector 를 호출한다.
  void GetDocumentRoot(const std::string& selector,
                       const std::string& button,
                       bool wait_for_navigation,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback);

  // Step 2: DOM.getDocument 응답을 받아 DOM.querySelector 호출.
  void OnGetDocumentRoot(const std::string& selector,
                         const std::string& button,
                         bool wait_for_navigation,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback,
                         base::Value response);

  // Step 3: DOM.querySelector 응답을 받아 nodeId로 DOM.getBoxModel 호출.
  void OnQuerySelector(const std::string& button,
                       bool wait_for_navigation,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // Step 4: DOM.getBoxModel 응답을 받아 중심 좌표를 계산하고
  //         Input.dispatchMouseEvent (mousePressed) 를 발송한다.
  void OnGetBoxModel(const std::string& button,
                     bool wait_for_navigation,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback,
                     base::Value response);

  // Step 5: mousePressed 완료 후 mouseReleased 를 발송한다.
  void OnMousePressed(double x,
                      double y,
                      const std::string& button,
                      bool wait_for_navigation,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback,
                      base::Value response);

  // Step 6: mouseReleased 완료.
  //         wait_for_navigation == true 이면 Page.loadEventFired 대기,
  //         아니면 즉시 성공 결과를 반환한다.
  void OnMouseReleased(bool wait_for_navigation,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // Step 7 (optional): Page.loadEventFired 이벤트 수신 후 결과 반환.
  void OnLoadEventFired(base::OnceCallback<void(base::Value)> callback,
                        base::Value response);

  // CDP 에러 응답인지 확인하고, 에러이면 MCP 오류 결과를 callback으로 전달.
  // 반환값: 에러가 있으면 true (이후 처리 불필요).
  static bool HandleCdpError(const base::Value& response,
                              const std::string& step_name,
                              base::OnceCallback<void(base::Value)>& callback);

  base::WeakPtrFactory<ClickTool> weak_factory_{this};
};

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
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
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
