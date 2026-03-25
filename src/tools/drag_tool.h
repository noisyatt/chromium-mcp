// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_DRAG_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_DRAG_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/tools/element_locator.h"

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
// 세 가지 사용 방법:
//   1. role/name, text 등 로케이터: startRole/startName/startText + endRole/endName/endText
//   2. selector 지정: startSelector/endSelector로 요소를 찾아 중심좌표 사용
//   3. 좌표 직접 지정: startX/startY, endX/endY 사용
//
// 로케이터 모드 실행 흐름:
//   ElementLocator::Locate(start) → OnStartLocated
//   → ElementLocator::Locate(end) → OnEndLocated
//   → StartDragSequence
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
  // 시작 요소 로케이터 콜백
  void OnStartLocated(base::DictValue end_params,
                      int steps,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback,
                      std::optional<ElementLocator::Result> result,
                      std::string error);

  // 끝 요소 로케이터 콜백
  void OnEndLocated(double start_x,
                    double start_y,
                    int steps,
                    McpSession* session,
                    base::OnceCallback<void(base::Value)> callback,
                    std::optional<ElementLocator::Result> result,
                    std::string error);

  // 시작/끝 좌표 확보 후 실제 드래그 시퀀스 시작
  void StartDragSequence(double start_x,
                         double start_y,
                         double end_x,
                         double end_y,
                         int steps,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  // 드래그 시퀀스 Step 1: mousePressed 완료 콜백
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
  void DispatchNextMove(double start_x,
                        double start_y,
                        double end_x,
                        double end_y,
                        int steps,
                        int current_step,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  // mouseMoved 완료 콜백
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

  // start / end 각각 별도의 ElementLocator 인스턴스 사용
  ElementLocator start_locator_;
  ElementLocator end_locator_;

  base::WeakPtrFactory<DragTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_DRAG_TOOL_H_
