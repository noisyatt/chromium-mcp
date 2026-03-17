// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_MOUSE_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_MOUSE_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// MouseTool: 마우스 이동, 호버, 드래그 앤 드롭을 시뮬레이션하는 도구.
//
// 세 가지 동작 모드:
//   - action="move"  : 현재 위치에서 목표 좌표(x, y)까지 선형 보간으로 이동
//   - action="hover" : 목표 좌표(x, y)로 이동 후 정지 (mouseMoved 이벤트)
//   - action="drag"  : 시작점(startX, startY)에서 끝점(x, y)까지 드래그
//                      mousePressed → mouseMoved(steps개) → mouseReleased
//
// 좌표 보간:
//   steps 파라미터로 이동을 여러 단계로 나눠 자연스러운 마우스 이동을 시뮬레이션한다.
//   각 단계의 좌표는 선형 보간(Lerp)으로 계산된다.
//
// button 파라미터:
//   drag 모드에서 사용할 마우스 버튼 (left/right/middle, 기본값: left).
//   CDP 버튼 번호: left=1, right=2, middle=4
class MouseTool : public McpTool {
 public:
  MouseTool();
  ~MouseTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // move/hover 모드: 시작 좌표에서 목표 좌표까지 보간하며 mouseMoved 발송
  // from_x, from_y 가 0,0 이면 첫 번째 이동 이벤트만 발송
  void ExecuteMove(double from_x,
                   double from_y,
                   double to_x,
                   double to_y,
                   int steps,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // drag 모드: mousePressed → (보간 이동) → mouseReleased 시퀀스
  void ExecuteDrag(double start_x,
                   double start_y,
                   double end_x,
                   double end_y,
                   int steps,
                   const std::string& button,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // move/hover: 보간 단계별 mouseMoved 발송
  void DispatchNextMove(double from_x,
                        double from_y,
                        double to_x,
                        double to_y,
                        int total_steps,
                        int current_step,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  // move 단계 완료 콜백
  void OnMoveStepped(double from_x,
                     double from_y,
                     double to_x,
                     double to_y,
                     int total_steps,
                     int current_step,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback,
                     base::Value response);

  // move/hover 마지막 이벤트 완료 콜백
  void OnMoveCompleted(base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // drag: mousePressed 완료 콜백
  void OnDragPressed(double start_x,
                     double start_y,
                     double end_x,
                     double end_y,
                     int total_steps,
                     int button_buttons,
                     const std::string& button_name,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback,
                     base::Value response);

  // drag: 보간 단계별 mouseMoved 발송
  void DispatchDragMove(double start_x,
                        double start_y,
                        double end_x,
                        double end_y,
                        int total_steps,
                        int current_step,
                        int button_buttons,
                        const std::string& button_name,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  // drag: mouseMoved 단계 완료 콜백
  void OnDragMoved(double start_x,
                   double start_y,
                   double end_x,
                   double end_y,
                   int total_steps,
                   int current_step,
                   int button_buttons,
                   const std::string& button_name,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback,
                   base::Value response);

  // drag: mouseReleased 발송
  void DispatchDragRelease(double end_x,
                           double end_y,
                           const std::string& button_name,
                           McpSession* session,
                           base::OnceCallback<void(base::Value)> callback);

  // drag: mouseReleased 완료 콜백
  void OnDragReleased(base::OnceCallback<void(base::Value)> callback,
                      base::Value response);

  // CDP 에러 처리 헬퍼
  static bool HandleCdpError(const base::Value& response,
                              const std::string& step_name,
                              base::OnceCallback<void(base::Value)>& callback);

  base::WeakPtrFactory<MouseTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_MOUSE_TOOL_H_
