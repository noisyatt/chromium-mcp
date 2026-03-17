// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_DRAG_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_DRAG_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// DragTool: 드래그 앤 드롭을 시뮬레이션하는 도구.
//
// 시작점에서 끝점까지 선형 보간(linear interpolation)으로 마우스를 이동시키며
// 실제 드래그 앤 드롭 동작을 재현한다.
//
// CDP 이벤트 시퀀스:
//   1. mousePressed (시작점, button=left)
//   2. mouseMoved   (시작점 → 끝점 선형 보간, steps 횟수만큼 반복)
//   3. mouseReleased (끝점)
//
// 두 가지 사용 방법:
//   1. selector 지정: startSelector/endSelector로 요소를 찾아 중심좌표 사용
//   2. 좌표 직접 지정: startX/startY, endX/endY 사용
//
// 실행 흐름 (selector 지정 시):
//   1-a. DOM.getDocument + DOM.querySelector(start) + DOM.getBoxModel(start)
//   1-b. DOM.querySelector(end) + DOM.getBoxModel(end)
//   2. mousePressed at start
//   3. mouseMoved × steps (선형 보간)
//   4. mouseReleased at end
class DragTool : public McpTool {
 public:
  DragTool();
  ~DragTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 시작/끝 좌표 확보 후 실제 드래그 시퀀스 시작
  void StartDragSequence(double start_x,
                         double start_y,
                         double end_x,
                         double end_y,
                         int steps,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  // Step A: 시작 요소의 DOM.getDocument 호출
  void GetDocumentRootForStart(const std::string& start_selector,
                               const std::string& end_selector,
                               int steps,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback);

  // Step B: getDocument 응답 후 startSelector로 DOM.querySelector 호출
  void OnGetDocumentRootForStart(const std::string& start_selector,
                                 const std::string& end_selector,
                                 int steps,
                                 McpSession* session,
                                 base::OnceCallback<void(base::Value)> callback,
                                 base::Value response);

  // Step C: startSelector querySelector 응답 후 DOM.getBoxModel 호출
  void OnQuerySelectorStart(const std::string& end_selector,
                            int steps,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback,
                            base::Value response);

  // Step D: start BoxModel 응답 후 endSelector로 DOM.querySelector 호출
  void OnGetBoxModelStart(const std::string& end_selector,
                          int steps,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // Step E: endSelector querySelector 응답 후 end DOM.getBoxModel 호출
  void OnQuerySelectorEnd(double start_x,
                          double start_y,
                          int steps,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // Step F: end BoxModel 응답 후 드래그 시퀀스 시작
  void OnGetBoxModelEnd(double start_x,
                        double start_y,
                        int steps,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback,
                        base::Value response);

  // 드래그 시퀀스 Step 1: mousePressed 발송
  void OnMousePressed(double start_x,
                      double start_y,
                      double end_x,
                      double end_y,
                      int steps,
                      int current_step,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback,
                      base::Value response);

  // 드래그 시퀀스 Step 2: mouseMoved 반복 발송 (선형 보간)
  // current_step: 0부터 시작, steps에 도달하면 mouseReleased로 전환
  void DispatchNextMove(double start_x,
                        double start_y,
                        double end_x,
                        double end_y,
                        int steps,
                        int current_step,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  // mouseMoved 완료 콜백 — 다음 step으로 진행하거나 mouseReleased로 전환
  void OnMouseMoved(double start_x,
                    double start_y,
                    double end_x,
                    double end_y,
                    int steps,
                    int current_step,
                    McpSession* session,
                    base::OnceCallback<void(base::Value)> callback,
                    base::Value response);

  // 드래그 시퀀스 Step 3: mouseReleased 발송
  void DispatchMouseReleased(double end_x,
                             double end_y,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback);

  // mouseReleased 완료 콜백
  void OnMouseReleased(base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // CDP 에러 처리 헬퍼
  static bool HandleCdpError(const base::Value& response,
                              const std::string& step_name,
                              base::OnceCallback<void(base::Value)>& callback);

  base::WeakPtrFactory<DragTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_DRAG_TOOL_H_
