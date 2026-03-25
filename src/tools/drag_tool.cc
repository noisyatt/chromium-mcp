// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/drag_tool.h"

#include <cmath>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

namespace {

// 선형 보간: t=0이면 start, t=1이면 end 반환 (0 <= t <= 1)
double Lerp(double start, double end, double t) {
  return start + (end - start) * t;
}

// Input.dispatchMouseEvent 파라미터 Dict 생성
base::DictValue MakeMouseEventParams(const std::string& type,
                                       double x,
                                       double y,
                                       const std::string& button,
                                       int buttons) {
  base::DictValue params;
  params.Set("type", type);
  params.Set("x", x);
  params.Set("y", y);
  params.Set("button", button);
  params.Set("buttons", buttons);
  params.Set("clickCount", type == "mousePressed" ? 1 : 0);
  params.Set("modifiers", 0);
  return params;
}

}  // namespace

// ============================================================
// DragTool 구현
// ============================================================

DragTool::DragTool() = default;
DragTool::~DragTool() = default;

std::string DragTool::name() const {
  return "drag";
}

std::string DragTool::description() const {
  return "드래그 앤 드롭을 시뮬레이션합니다. "
         "시작점에서 끝점까지 선형 보간으로 마우스를 이동시킵니다. "
         "startRole/startName/startText 또는 startSelector로 시작 요소를, "
         "endRole/endName/endText 또는 endSelector로 끝 요소를 지정하거나, "
         "startX/startY, endX/endY로 좌표를 직접 지정할 수 있습니다.";
}

base::DictValue DragTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // ---- 시작점 로케이터 파라미터 ----

  base::DictValue start_role_prop;
  start_role_prop.Set("type", "string");
  start_role_prop.Set("description",
                       "드래그 시작 요소의 ARIA 역할. startName과 함께 사용합니다.");
  properties.Set("startRole", std::move(start_role_prop));

  base::DictValue start_name_prop;
  start_name_prop.Set("type", "string");
  start_name_prop.Set("description",
                       "드래그 시작 요소의 접근성 이름. startRole과 함께 사용합니다.");
  properties.Set("startName", std::move(start_name_prop));

  base::DictValue start_text_prop;
  start_text_prop.Set("type", "string");
  start_text_prop.Set("description",
                       "드래그 시작 요소의 표시 텍스트.");
  properties.Set("startText", std::move(start_text_prop));

  // startSelector: 드래그 시작 요소의 CSS 셀렉터
  base::DictValue start_selector_prop;
  start_selector_prop.Set("type", "string");
  start_selector_prop.Set("description",
                           "드래그를 시작할 요소의 CSS 셀렉터. "
                           "지정 시 요소의 중심 좌표에서 드래그가 시작됩니다.");
  properties.Set("startSelector", std::move(start_selector_prop));

  // ---- 끝점 로케이터 파라미터 ----

  base::DictValue end_role_prop;
  end_role_prop.Set("type", "string");
  end_role_prop.Set("description",
                     "드래그 끝(드롭) 요소의 ARIA 역할. endName과 함께 사용합니다.");
  properties.Set("endRole", std::move(end_role_prop));

  base::DictValue end_name_prop;
  end_name_prop.Set("type", "string");
  end_name_prop.Set("description",
                     "드래그 끝(드롭) 요소의 접근성 이름. endRole과 함께 사용합니다.");
  properties.Set("endName", std::move(end_name_prop));

  base::DictValue end_text_prop;
  end_text_prop.Set("type", "string");
  end_text_prop.Set("description",
                     "드래그 끝(드롭) 요소의 표시 텍스트.");
  properties.Set("endText", std::move(end_text_prop));

  // endSelector: 드롭 대상 요소의 CSS 셀렉터
  base::DictValue end_selector_prop;
  end_selector_prop.Set("type", "string");
  end_selector_prop.Set("description",
                         "드래그를 끝낼(드롭할) 요소의 CSS 셀렉터. "
                         "지정 시 요소의 중심 좌표에서 마우스가 릴리즈됩니다.");
  properties.Set("endSelector", std::move(end_selector_prop));

  // ---- 좌표 직접 지정 파라미터 ----

  // startX: 드래그 시작 X 좌표
  base::DictValue start_x_prop;
  start_x_prop.Set("type", "number");
  start_x_prop.Set("description", "드래그 시작 X 좌표 (픽셀). startSelector가 없을 때 사용.");
  properties.Set("startX", std::move(start_x_prop));

  // startY: 드래그 시작 Y 좌표
  base::DictValue start_y_prop;
  start_y_prop.Set("type", "number");
  start_y_prop.Set("description", "드래그 시작 Y 좌표 (픽셀). startSelector가 없을 때 사용.");
  properties.Set("startY", std::move(start_y_prop));

  // endX: 드래그 끝 X 좌표
  base::DictValue end_x_prop;
  end_x_prop.Set("type", "number");
  end_x_prop.Set("description", "드래그 끝(드롭) X 좌표 (픽셀). endSelector가 없을 때 사용.");
  properties.Set("endX", std::move(end_x_prop));

  // endY: 드래그 끝 Y 좌표
  base::DictValue end_y_prop;
  end_y_prop.Set("type", "number");
  end_y_prop.Set("description", "드래그 끝(드롭) Y 좌표 (픽셀). endSelector가 없을 때 사용.");
  properties.Set("endY", std::move(end_y_prop));

  // steps: 이동 단계 수 (선형 보간 횟수)
  base::DictValue steps_prop;
  steps_prop.Set("type", "number");
  steps_prop.Set("default", 10);
  steps_prop.Set("description",
                 "시작점에서 끝점까지의 이동 단계 수. "
                 "클수록 부드러운 드래그이나 속도가 느려짐. 기본값: 10.");
  properties.Set("steps", std::move(steps_prop));

  schema.Set("properties", std::move(properties));

  // 모든 파라미터 선택적 (런타임 검증)
  base::ListValue required;
  schema.Set("required", std::move(required));

  return schema;
}

void DragTool::Execute(const base::DictValue& arguments,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback) {
  // steps 파라미터 (기본값: 10, 최소: 1)
  std::optional<double> steps_opt = arguments.FindDouble("steps");
  int steps = static_cast<int>(steps_opt.value_or(10.0));
  if (steps < 1) {
    steps = 1;
  }

  // 시작점 로케이터 파라미터 구성
  const std::string* start_role = arguments.FindString("startRole");
  const std::string* start_name = arguments.FindString("startName");
  const std::string* start_text = arguments.FindString("startText");
  const std::string* start_selector = arguments.FindString("startSelector");

  const bool has_start_locator =
      (start_role && !start_role->empty()) ||
      (start_name && !start_name->empty()) ||
      (start_text && !start_text->empty()) ||
      (start_selector && !start_selector->empty());

  // 끝점 로케이터 파라미터 구성
  const std::string* end_role = arguments.FindString("endRole");
  const std::string* end_name = arguments.FindString("endName");
  const std::string* end_text = arguments.FindString("endText");
  const std::string* end_selector = arguments.FindString("endSelector");

  const bool has_end_locator =
      (end_role && !end_role->empty()) ||
      (end_name && !end_name->empty()) ||
      (end_text && !end_text->empty()) ||
      (end_selector && !end_selector->empty());

  if (has_start_locator && has_end_locator) {
    // 로케이터 모드: start/end 각각 ElementLocator 어댑터 파라미터 구성

    // 시작 요소 파라미터: startRole→role, startName→name, startText→text
    base::Value::Dict start_params;
    if (start_role && !start_role->empty()) {
      start_params.Set("role", *start_role);
    }
    if (start_name && !start_name->empty()) {
      start_params.Set("name", *start_name);
    }
    if (start_text && !start_text->empty()) {
      start_params.Set("text", *start_text);
    }
    if (start_selector && !start_selector->empty()) {
      start_params.Set("selector", *start_selector);
    }
    // exact 파라미터 전달
    std::optional<bool> exact = arguments.FindBool("exact");
    if (exact.has_value()) {
      start_params.Set("exact", *exact);
    }

    // 끝 요소 파라미터: endRole→role, endName→name, endText→text
    base::Value::Dict end_params;
    if (end_role && !end_role->empty()) {
      end_params.Set("role", *end_role);
    }
    if (end_name && !end_name->empty()) {
      end_params.Set("name", *end_name);
    }
    if (end_text && !end_text->empty()) {
      end_params.Set("text", *end_text);
    }
    if (end_selector && !end_selector->empty()) {
      end_params.Set("selector", *end_selector);
    }
    if (exact.has_value()) {
      end_params.Set("exact", *exact);
    }

    LOG(INFO) << "[DragTool] 로케이터 모드: steps=" << steps;

    start_locator_.Locate(
        session, start_params,
        base::BindOnce(&DragTool::OnStartLocated, weak_factory_.GetWeakPtr(),
                       std::move(end_params), steps, session,
                       std::move(callback)));
    return;
  }

  // 좌표 직접 지정 모드
  std::optional<double> start_x_opt = arguments.FindDouble("startX");
  std::optional<double> start_y_opt = arguments.FindDouble("startY");
  std::optional<double> end_x_opt = arguments.FindDouble("endX");
  std::optional<double> end_y_opt = arguments.FindDouble("endY");

  if (!start_x_opt.has_value() || !start_y_opt.has_value() ||
      !end_x_opt.has_value() || !end_y_opt.has_value()) {
    LOG(WARNING) << "[DragTool] startRole/startName 등 로케이터 또는 "
                    "startX/startY/endX/endY 파라미터가 필요합니다.";
    std::move(callback).Run(
        MakeErrorResult("startRole/startName/startText/startSelector 또는 "
                        "startX/startY/endX/endY 파라미터가 필요합니다."));
    return;
  }

  LOG(INFO) << "[DragTool] 좌표 모드: start=(" << *start_x_opt << ","
            << *start_y_opt << ") end=(" << *end_x_opt << "," << *end_y_opt
            << ") steps=" << steps;

  StartDragSequence(*start_x_opt, *start_y_opt, *end_x_opt, *end_y_opt,
                    steps, session, std::move(callback));
}

// 시작 요소 로케이터 콜백
void DragTool::OnStartLocated(base::Value::Dict end_params,
                              int steps,
                              McpSession* session,
                              base::OnceCallback<void(base::Value)> callback,
                              std::optional<ElementLocator::Result> result,
                              std::string error) {
  if (!error.empty()) {
    LOG(WARNING) << "[DragTool] 시작 요소 로케이터 실패: " << error;
    std::move(callback).Run(MakeErrorResult("시작 요소를 찾을 수 없습니다: " + error));
    return;
  }

  LOG(INFO) << "[DragTool] 시작 좌표: (" << result->x << ", " << result->y << ")";
  double start_x = result->x;
  double start_y = result->y;

  end_locator_.Locate(
      session, end_params,
      base::BindOnce(&DragTool::OnEndLocated, weak_factory_.GetWeakPtr(),
                     start_x, start_y, steps, session, std::move(callback)));
}

// 끝 요소 로케이터 콜백
void DragTool::OnEndLocated(double start_x,
                            double start_y,
                            int steps,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback,
                            std::optional<ElementLocator::Result> result,
                            std::string error) {
  if (!error.empty()) {
    LOG(WARNING) << "[DragTool] 끝 요소 로케이터 실패: " << error;
    std::move(callback).Run(MakeErrorResult("끝 요소를 찾을 수 없습니다: " + error));
    return;
  }

  LOG(INFO) << "[DragTool] 끝 좌표: (" << result->x << ", " << result->y << ")";
  StartDragSequence(start_x, start_y, result->x, result->y, steps, session,
                    std::move(callback));
}

// 시작/끝 좌표 확보 후 드래그 시퀀스 시작 (mousePressed)
void DragTool::StartDragSequence(
    double start_x,
    double start_y,
    double end_x,
    double end_y,
    int steps,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // Step 1: 시작점에서 mousePressed (왼쪽 버튼)
  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mousePressed", start_x, start_y, "left", 1),
      base::BindOnce(&DragTool::OnMousePressed,
                     weak_factory_.GetWeakPtr(),
                     start_x, start_y, end_x, end_y, steps, 0,
                     session, std::move(callback)));
}

// mousePressed 완료 후 첫 번째 mouseMoved 발송
void DragTool::OnMousePressed(
    double start_x,
    double start_y,
    double end_x,
    double end_y,
    int steps,
    int current_step,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (mcp::HandleCdpError(response, "Input.dispatchMouseEvent(mousePressed)",
                           callback)) {
    return;
  }

  LOG(INFO) << "[DragTool] mousePressed 완료, mouseMoved 시퀀스 시작";
  DispatchNextMove(start_x, start_y, end_x, end_y, steps, 0,
                   session, std::move(callback));
}

// mouseMoved 반복 발송 (선형 보간)
void DragTool::DispatchNextMove(
    double start_x,
    double start_y,
    double end_x,
    double end_y,
    int steps,
    int current_step,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (current_step >= steps) {
    LOG(INFO) << "[DragTool] mouseMoved 시퀀스 완료, mouseReleased 발송";
    DispatchMouseReleased(end_x, end_y, session, std::move(callback));
    return;
  }

  // 선형 보간으로 현재 단계의 좌표 계산
  double t = static_cast<double>(current_step + 1) / static_cast<double>(steps);
  double move_x = Lerp(start_x, end_x, t);
  double move_y = Lerp(start_y, end_y, t);

  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseMoved", move_x, move_y, "left", 1),
      base::BindOnce(&DragTool::OnMouseMoved,
                     weak_factory_.GetWeakPtr(),
                     start_x, start_y, end_x, end_y, steps, current_step + 1,
                     session, std::move(callback)));
}

// mouseMoved 완료 콜백
void DragTool::OnMouseMoved(
    double start_x,
    double start_y,
    double end_x,
    double end_y,
    int steps,
    int current_step,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (mcp::HandleCdpError(response, "Input.dispatchMouseEvent(mouseMoved)",
                           callback)) {
    return;
  }

  DispatchNextMove(start_x, start_y, end_x, end_y, steps, current_step,
                   session, std::move(callback));
}

// mouseReleased 발송
void DragTool::DispatchMouseReleased(
    double end_x,
    double end_y,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseReleased", end_x, end_y, "left", 0),
      base::BindOnce(&DragTool::OnMouseReleased,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// mouseReleased 완료 콜백
void DragTool::OnMouseReleased(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (mcp::HandleCdpError(response, "Input.dispatchMouseEvent(mouseReleased)",
                           callback)) {
    return;
  }
  LOG(INFO) << "[DragTool] 드래그 앤 드롭 완료";
  std::move(callback).Run(
      MakeSuccessResult("드래그 앤 드롭이 성공적으로 완료되었습니다."));
}

}  // namespace mcp
