// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_WINDOW_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_WINDOW_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 브라우저 윈도우 크기 조절, 위치 이동, 상태 변경 도구.
//
// CDP Browser 도메인을 활용한다:
//   - Browser.getWindowForTarget : 현재 탭이 속한 윈도우 ID 조회
//   - Browser.getWindowBounds    : 윈도우 위치/크기/상태 조회
//   - Browser.setWindowBounds    : 윈도우 위치/크기/상태 설정
//
// 지원 action:
//   - resize    : 윈도우 크기를 width × height 로 변경
//   - move      : 윈도우 위치를 (x, y)로 이동
//   - minimize  : 윈도우 최소화
//   - maximize  : 윈도우 최대화
//   - fullscreen: 전체 화면 전환
//   - restore   : 최소화/최대화/전체화면 해제 (일반 크기 복원)
//   - getBounds : 현재 윈도우 위치·크기·상태 반환
//   - list      : 열린 모든 윈도우 목록 반환
//
// windowId 파라미터가 없으면 현재 세션의 탭이 속한 윈도우를
// Browser.getWindowForTarget으로 자동 조회한다.
class WindowTool : public McpTool {
 public:
  WindowTool();
  ~WindowTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // windowId를 알고 있는 경우 action을 바로 실행한다.
  // windowId를 모르는 경우 Browser.getWindowForTarget으로 먼저 조회한다.
  void ExecuteWithWindowId(int window_id,
                           const std::string& action,
                           const base::Value::Dict& arguments,
                           McpSession* session,
                           base::OnceCallback<void(base::Value)> callback);

  // Browser.getWindowForTarget 응답 처리.
  // 응답에서 windowId를 추출하여 ExecuteWithWindowId를 호출한다.
  void OnGetWindowForTarget(const std::string& action,
                            // arguments를 복사해 캡처한다.
                            base::Value::Dict arguments_copy,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback,
                            base::Value response);

  // action=resize: Browser.setWindowBounds(windowState="normal", width, height)
  void ExecuteResize(int window_id,
                     int width,
                     int height,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // action=move: Browser.setWindowBounds(windowState="normal", left, top)
  void ExecuteMove(int window_id,
                   int x,
                   int y,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // action=minimize/maximize/fullscreen/restore:
  //   Browser.setWindowBounds(windowState="minimized"|"maximized"|"fullscreen"|"normal")
  void ExecuteSetState(int window_id,
                       const std::string& window_state,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback);

  // action=getBounds: Browser.getWindowBounds
  void ExecuteGetBounds(int window_id,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  // action=list: Browser.getWindowForTarget + 목록 구성
  // (현재 CDP는 윈도우 목록 API가 없으므로 현재 윈도우 정보만 반환)
  void ExecuteList(McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // Browser.setWindowBounds 응답을 처리하여 성공/실패 결과를 반환한다.
  void OnSetWindowBoundsResponse(base::OnceCallback<void(base::Value)> callback,
                                 const std::string& action,
                                 base::Value response);

  // Browser.getWindowBounds 응답을 처리하여 bounds 정보를 반환한다.
  void OnGetWindowBoundsResponse(base::OnceCallback<void(base::Value)> callback,
                                 base::Value response);

  // Browser.getWindowForTarget 응답에서 windowId를 추출하는 헬퍼.
  // 실패 시 -1을 반환한다.
  static int ExtractWindowId(const base::Value& response);

  base::WeakPtrFactory<WindowTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_WINDOW_TOOL_H_
