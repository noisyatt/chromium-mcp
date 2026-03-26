// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_DRAG_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_DRAG_TOOL_H_

#include <memory>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// DragTool: 드래그 앤 드롭을 시뮬레이션하는 도구.
//
// HTML5 DnD를 지원하기 위해 Playwright 패턴을 사용한다:
//   1. mousePressed (시작점)
//   2. Input.setInterceptDrags(true) — OS DnD 제스처를 CDP가 가로챔
//   3. mouseMoved × steps — drag threshold 초과 시 Input.dragIntercepted 이벤트 발생
//   4. Input.setInterceptDrags(false)
//   5. Input.dispatchDragEvent(dragEnter/dragOver/drop) — HTML5 DnD 이벤트 발화
//   6. mouseReleased (끝점)
class DragTool : public McpTool {
 public:
  DragTool();
  ~DragTool() override;

  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 드래그 시퀀스 전체에서 공유되는 컨텍스트
  struct DragContext {
    DragContext();
    ~DragContext();

    double start_x = 0;
    double start_y = 0;
    double end_x = 0;
    double end_y = 0;
    int steps = 10;
    McpSession* session = nullptr;
    base::OnceCallback<void(base::Value)> callback;

    // HTML5 DnD 지원
    bool drag_intercepted = false;
    base::DictValue drag_data;  // Input.dragIntercepted에서 캡처한 DragData
  };

  // 로케이터 콜백
  void OnStartLocated(base::DictValue end_params,
                      int steps,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback,
                      std::optional<ElementLocator::Result> result,
                      std::string error);

  void OnEndLocated(double start_x,
                    double start_y,
                    int steps,
                    McpSession* session,
                    base::OnceCallback<void(base::Value)> callback,
                    std::optional<ElementLocator::Result> result,
                    std::string error);

  // 드래그 시퀀스
  void StartDragSequence(double start_x, double start_y,
                         double end_x, double end_y,
                         int steps, McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  // Step 1: mousePressed → Step 2
  void OnMousePressed(std::shared_ptr<DragContext> ctx, base::Value response);

  // Step 2: mouseMoved 반복
  void DispatchNextMove(std::shared_ptr<DragContext> ctx, int current_step);
  void OnMouseMoved(std::shared_ptr<DragContext> ctx, int current_step,
                    base::Value response);

  // Step 3: mouseReleased (마우스 캡처 해제)
  void OnAllMovesDone(std::shared_ptr<DragContext> ctx);
  void DispatchMouseReleased(std::shared_ptr<DragContext> ctx);
  void OnMouseReleased(std::shared_ptr<DragContext> ctx, base::Value response);

  // Step 4: JS DragEvent 디스패치 → 완료
  void DispatchJsDragEvents(std::shared_ptr<DragContext> ctx);
  void OnDragEventsDispatched(std::shared_ptr<DragContext> ctx,
                              base::Value response);

  ElementLocator start_locator_;
  ElementLocator end_locator_;

  base::WeakPtrFactory<DragTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_DRAG_TOOL_H_
