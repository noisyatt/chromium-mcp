// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/mouse_tool.h"

#include <cmath>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

namespace {

// ============================================================
// MCP 응답 헬퍼
// ============================================================

// MCP 성공 응답 Value 생성
base::Value MakeSuccessResult(const std::string& message) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", message);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", false);
  return base::Value(std::move(result));
}

// MCP 에러 응답 Value 생성
base::Value MakeErrorResult(const std::string& message) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", message);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", true);
  return base::Value(std::move(result));
}

// ============================================================
// CDP 응답 파싱 헬퍼
// ============================================================

// CDP 응답에 "error" 키가 있는지 확인한다.
bool HasCdpError(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return true;
  }
  return dict->Find("error") != nullptr;
}

// CDP 응답에서 에러 메시지를 추출한다.
std::string ExtractCdpErrorMessage(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return "CDP 응답이 Dict 형식이 아님";
  }
  const base::DictValue* error = dict->FindDict("error");
  if (!error) {
    return "알 수 없는 CDP 에러";
  }
  const std::string* msg = error->FindString("message");
  if (!msg) {
    return "에러 메시지 없음";
  }
  return *msg;
}

// ============================================================
// 마우스 이벤트 파라미터 생성 헬퍼
// ============================================================

// button 이름을 CDP buttons 비트마스크로 변환한다.
// CDP 규격: left=1, right=2, middle=4
int ButtonNameToButtons(const std::string& button_name) {
  if (button_name == "right") {
    return 2;
  }
  if (button_name == "middle") {
    return 4;
  }
  // 기본값: left=1
  return 1;
}

// Input.dispatchMouseEvent 파라미터 Dict를 생성한다.
// type     : "mouseMoved" / "mousePressed" / "mouseReleased"
// x, y     : 이벤트 발생 좌표 (픽셀)
// button   : "none" / "left" / "right" / "middle"
// buttons  : 현재 눌린 버튼 비트마스크 (0=없음, 1=left, 2=right, 4=middle)
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
  // mousePressed 이벤트만 clickCount=1, 나머지는 0
  params.Set("clickCount", type == "mousePressed" ? 1 : 0);
  params.Set("modifiers", 0);
  return params;
}

// ============================================================
// 선형 보간 헬퍼
// ============================================================

// 선형 보간: t=0이면 start, t=1이면 end 반환 (0 <= t <= 1)
double Lerp(double start, double end, double t) {
  return start + (end - start) * t;
}

}  // namespace

// ============================================================
// MouseTool 구현
// ============================================================

MouseTool::MouseTool() = default;
MouseTool::~MouseTool() = default;

std::string MouseTool::name() const {
  return "mouse";
}

std::string MouseTool::description() const {
  return "마우스 이동, 호버, 드래그 앤 드롭을 시뮬레이션합니다. "
         "action='move'는 목표 좌표까지 선형 보간으로 자연스럽게 이동하고, "
         "action='hover'는 이동 후 정지하며, "
         "action='drag'는 시작점에서 끝점까지 마우스 버튼을 누른 채 이동합니다. "
         "steps 파라미터로 이동 단계 수를 조절할 수 있습니다.";
}

base::DictValue MouseTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: 동작 유형 (move / hover / drag)
  base::DictValue action_prop;
  action_prop.Set("type", "string");
  {
    base::ListValue action_enum;
    action_enum.Append("move");
    action_enum.Append("hover");
    action_enum.Append("drag");
    action_prop.Set("enum", std::move(action_enum));
  }
  action_prop.Set(
      "description",
      "동작 유형. "
      "move: startX/startY에서 x/y까지 보간하며 mouseMoved 이벤트 연속 발송. "
      "hover: x/y 좌표로 단일 mouseMoved 이벤트 발송 후 정지. "
      "drag: startX/startY에서 mousePressed 후 x/y까지 보간 이동, "
      "마지막에 mouseReleased 발송.");
  properties.Set("action", std::move(action_prop));

  // x: 목표 X 좌표 (move/hover 도착점, drag 끝점)
  base::DictValue x_prop;
  x_prop.Set("type", "number");
  x_prop.Set("description",
             "목표 X 좌표 (픽셀). "
             "move/hover에서는 도착점, drag에서는 드롭 위치 X 좌표.");
  properties.Set("x", std::move(x_prop));

  // y: 목표 Y 좌표 (move/hover 도착점, drag 끝점)
  base::DictValue y_prop;
  y_prop.Set("type", "number");
  y_prop.Set("description",
             "목표 Y 좌표 (픽셀). "
             "move/hover에서는 도착점, drag에서는 드롭 위치 Y 좌표.");
  properties.Set("y", std::move(y_prop));

  // startX: 이동/드래그 시작 X 좌표
  base::DictValue start_x_prop;
  start_x_prop.Set("type", "number");
  start_x_prop.Set("description",
                   "이동 시작 X 좌표 (픽셀). "
                   "action=drag 일 때 필수. move에서는 생략 시 0.");
  properties.Set("startX", std::move(start_x_prop));

  // startY: 이동/드래그 시작 Y 좌표
  base::DictValue start_y_prop;
  start_y_prop.Set("type", "number");
  start_y_prop.Set("description",
                   "이동 시작 Y 좌표 (픽셀). "
                   "action=drag 일 때 필수. move에서는 생략 시 0.");
  properties.Set("startY", std::move(start_y_prop));

  // steps: 보간 단계 수
  base::DictValue steps_prop;
  steps_prop.Set("type", "number");
  steps_prop.Set("default", 10);
  steps_prop.Set("description",
                 "시작점에서 끝점까지의 이동 단계 수. "
                 "클수록 부드러운 이동이나 CDP 명령이 많아짐. 기본값: 10.");
  properties.Set("steps", std::move(steps_prop));

  // button: drag 시 사용할 마우스 버튼 (기본값: left)
  base::DictValue button_prop;
  button_prop.Set("type", "string");
  {
    base::ListValue button_enum;
    button_enum.Append("left");
    button_enum.Append("right");
    button_enum.Append("middle");
    button_prop.Set("enum", std::move(button_enum));
  }
  button_prop.Set("default", "left");
  button_prop.Set("description",
                  "드래그에 사용할 마우스 버튼. 기본값: left. "
                  "CDP 버튼 번호: left=1, right=2, middle=4.");
  properties.Set("button", std::move(button_prop));

  schema.Set("properties", std::move(properties));

  // action, x, y는 필수
  base::ListValue required;
  required.Append("action");
  required.Append("x");
  required.Append("y");
  schema.Set("required", std::move(required));

  return schema;
}

// Execute: action 파라미터를 읽어 적절한 내부 메서드로 분기한다.
void MouseTool::Execute(const base::DictValue& arguments,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback) {
  const std::string* action = arguments.FindString("action");
  if (!action || action->empty()) {
    LOG(WARNING) << "[MouseTool] action 파라미터가 필요합니다.";
    std::move(callback).Run(
        MakeErrorResult("action 파라미터가 필요합니다. (move/hover/drag)"));
    return;
  }

  // 목표 좌표 추출 (x, y는 모든 모드에서 필수)
  std::optional<double> x_opt = arguments.FindDouble("x");
  std::optional<double> y_opt = arguments.FindDouble("y");
  if (!x_opt.has_value() || !y_opt.has_value()) {
    LOG(WARNING) << "[MouseTool] x, y 파라미터가 필요합니다.";
    std::move(callback).Run(MakeErrorResult("x, y 파라미터가 필요합니다."));
    return;
  }
  double to_x = *x_opt;
  double to_y = *y_opt;

  // steps 파라미터 (기본값: 10, 최소: 1)
  std::optional<double> steps_opt = arguments.FindDouble("steps");
  int steps = static_cast<int>(steps_opt.value_or(10.0));
  if (steps < 1) {
    steps = 1;
  }

  if (*action == "move") {
    // move 모드: 시작 좌표(startX/startY)에서 목표 좌표(x/y)까지 보간 이동
    double from_x = arguments.FindDouble("startX").value_or(0.0);
    double from_y = arguments.FindDouble("startY").value_or(0.0);

    LOG(INFO) << "[MouseTool] move: (" << from_x << "," << from_y
              << ") → (" << to_x << "," << to_y << ") steps=" << steps;

    ExecuteMove(from_x, from_y, to_x, to_y, steps, session,
                std::move(callback));

  } else if (*action == "hover") {
    // hover 모드: 목표 좌표로 이동 후 정지 (단순 mouseMoved 이벤트)
    // 내부적으로 from/to를 동일하게 설정하여 steps=1 효과
    LOG(INFO) << "[MouseTool] hover: (" << to_x << "," << to_y << ")";

    ExecuteMove(to_x, to_y, to_x, to_y, 1, session, std::move(callback));

  } else if (*action == "drag") {
    // drag 모드: mousePressed → 보간 mouseMoved → mouseReleased
    std::optional<double> start_x_opt = arguments.FindDouble("startX");
    std::optional<double> start_y_opt = arguments.FindDouble("startY");
    if (!start_x_opt.has_value() || !start_y_opt.has_value()) {
      LOG(WARNING) << "[MouseTool] drag 모드에서는 startX/startY 파라미터가 필요합니다.";
      std::move(callback).Run(
          MakeErrorResult("drag 모드에서는 startX/startY 파라미터가 필요합니다."));
      return;
    }
    double start_x = *start_x_opt;
    double start_y = *start_y_opt;

    // button 파라미터 (기본값: left)
    const std::string* button_str = arguments.FindString("button");
    std::string button_name =
        (button_str && !button_str->empty()) ? *button_str : "left";

    LOG(INFO) << "[MouseTool] drag: (" << start_x << "," << start_y
              << ") → (" << to_x << "," << to_y << ") steps=" << steps
              << " button=" << button_name;

    ExecuteDrag(start_x, start_y, to_x, to_y, steps, button_name, session,
                std::move(callback));

  } else {
    LOG(WARNING) << "[MouseTool] 알 수 없는 action 값: " << *action;
    std::move(callback).Run(
        MakeErrorResult(
            "action은 'move', 'hover', 'drag' 중 하나여야 합니다."));
  }
}

// ============================================================
// move / hover 내부 구현
// ============================================================

// ExecuteMove: 선형 보간 이동 시퀀스를 시작한다.
// 첫 번째 단계(step 0)부터 DispatchNextMove를 호출한다.
void MouseTool::ExecuteMove(double from_x,
                            double from_y,
                            double to_x,
                            double to_y,
                            int steps,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback) {
  DispatchNextMove(from_x, from_y, to_x, to_y, steps, /*current_step=*/0,
                   session, std::move(callback));
}

// DispatchNextMove: current_step 번째 mouseMoved 이벤트를 발송한다.
// current_step >= total_steps이면 최종 목표 좌표로 마지막 이벤트를 발송한다.
void MouseTool::DispatchNextMove(double from_x,
                                 double from_y,
                                 double to_x,
                                 double to_y,
                                 int total_steps,
                                 int current_step,
                                 McpSession* session,
                                 base::OnceCallback<void(base::Value)> callback) {
  if (current_step >= total_steps) {
    // 보간 단계 모두 완료 — 정확한 목표 좌표로 최종 mouseMoved 발송
    LOG(INFO) << "[MouseTool] 보간 완료, 최종 위치 (" << to_x << "," << to_y
              << ") mouseMoved 발송";

    session->SendCdpCommand(
        "Input.dispatchMouseEvent",
        MakeMouseEventParams("mouseMoved", to_x, to_y, "none", 0),
        base::BindOnce(&MouseTool::OnMoveCompleted,
                       weak_factory_.GetWeakPtr(),
                       std::move(callback)));
    return;
  }

  // 선형 보간으로 현재 단계의 중간 좌표 계산
  // t 범위: 1/(total_steps+1) ~ total_steps/(total_steps+1)
  // (목표 좌표는 OnMoveCompleted에서 따로 발송하므로 t=1에 도달하지 않음)
  double t = static_cast<double>(current_step + 1) /
             static_cast<double>(total_steps + 1);
  double move_x = Lerp(from_x, to_x, t);
  double move_y = Lerp(from_y, to_y, t);

  LOG(INFO) << "[MouseTool] mouseMoved step " << (current_step + 1)
            << "/" << total_steps
            << " t=" << t
            << " → (" << move_x << "," << move_y << ")";

  // buttons=0: 버튼이 눌리지 않은 상태로 이동 (move/hover)
  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseMoved", move_x, move_y, "none", 0),
      base::BindOnce(&MouseTool::OnMoveStepped,
                     weak_factory_.GetWeakPtr(),
                     from_x, from_y, to_x, to_y,
                     total_steps, current_step + 1,
                     session, std::move(callback)));
}

// OnMoveStepped: 한 단계 mouseMoved 완료 후 다음 단계를 재귀 호출한다.
void MouseTool::OnMoveStepped(double from_x,
                               double from_y,
                               double to_x,
                               double to_y,
                               int total_steps,
                               int current_step,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback,
                               base::Value response) {
  if (HandleCdpError(response,
                     "Input.dispatchMouseEvent(mouseMoved)", callback)) {
    return;
  }
  // 다음 보간 단계 발송
  DispatchNextMove(from_x, from_y, to_x, to_y, total_steps, current_step,
                   session, std::move(callback));
}

// OnMoveCompleted: 최종 mouseMoved 완료 후 성공 응답을 반환한다.
void MouseTool::OnMoveCompleted(base::OnceCallback<void(base::Value)> callback,
                                base::Value response) {
  if (HandleCdpError(response,
                     "Input.dispatchMouseEvent(mouseMoved/final)", callback)) {
    return;
  }
  LOG(INFO) << "[MouseTool] 마우스 이동 완료";
  std::move(callback).Run(
      MakeSuccessResult("마우스 이동이 성공적으로 완료되었습니다."));
}

// ============================================================
// drag 내부 구현
// ============================================================

// ExecuteDrag: 드래그 시퀀스를 시작한다.
// 시작점에서 mousePressed 이벤트를 먼저 발송한다.
void MouseTool::ExecuteDrag(double start_x,
                            double start_y,
                            double end_x,
                            double end_y,
                            int steps,
                            const std::string& button,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback) {
  int button_buttons = ButtonNameToButtons(button);

  LOG(INFO) << "[MouseTool] mousePressed 발송: ("
            << start_x << "," << start_y
            << ") button=" << button
            << " buttons=" << button_buttons;

  // Step 1: 시작점에서 마우스 버튼 누르기
  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mousePressed", start_x, start_y, button,
                           button_buttons),
      base::BindOnce(&MouseTool::OnDragPressed,
                     weak_factory_.GetWeakPtr(),
                     start_x, start_y, end_x, end_y,
                     steps, button_buttons, button,
                     session, std::move(callback)));
}

// OnDragPressed: mousePressed 완료 후 보간 이동(mouseMoved) 시퀀스를 시작한다.
void MouseTool::OnDragPressed(double start_x,
                               double start_y,
                               double end_x,
                               double end_y,
                               int total_steps,
                               int button_buttons,
                               const std::string& button_name,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback,
                               base::Value response) {
  if (HandleCdpError(response,
                     "Input.dispatchMouseEvent(mousePressed)", callback)) {
    return;
  }
  LOG(INFO) << "[MouseTool] mousePressed 완료, 드래그 이동 시작 (steps="
            << total_steps << ")";

  // Step 2: 버튼 누른 채로 보간 이동 시작
  DispatchDragMove(start_x, start_y, end_x, end_y, total_steps,
                   /*current_step=*/0,
                   button_buttons, button_name, session, std::move(callback));
}

// DispatchDragMove: current_step 번째 드래그 중 mouseMoved 이벤트를 발송한다.
// 모든 단계가 완료되면 DispatchDragRelease를 호출한다.
void MouseTool::DispatchDragMove(double start_x,
                                  double start_y,
                                  double end_x,
                                  double end_y,
                                  int total_steps,
                                  int current_step,
                                  int button_buttons,
                                  const std::string& button_name,
                                  McpSession* session,
                                  base::OnceCallback<void(base::Value)> callback) {
  if (current_step >= total_steps) {
    // 모든 보간 단계 완료 → mouseReleased 발송
    LOG(INFO) << "[MouseTool] 드래그 이동 완료, mouseReleased 발송";
    DispatchDragRelease(end_x, end_y, button_name, session,
                        std::move(callback));
    return;
  }

  // 선형 보간으로 현재 단계의 좌표 계산
  // t: 1/total_steps 단위로 증가하여 start에서 end까지 균등 분배
  double t = static_cast<double>(current_step + 1) /
             static_cast<double>(total_steps);
  double move_x = Lerp(start_x, end_x, t);
  double move_y = Lerp(start_y, end_y, t);

  LOG(INFO) << "[MouseTool] 드래그 mouseMoved step " << (current_step + 1)
            << "/" << total_steps
            << " → (" << move_x << "," << move_y << ")";

  // button_buttons 플래그 유지: 버튼이 눌린 상태로 이동
  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseMoved", move_x, move_y, button_name,
                           button_buttons),
      base::BindOnce(&MouseTool::OnDragMoved,
                     weak_factory_.GetWeakPtr(),
                     start_x, start_y, end_x, end_y,
                     total_steps, current_step + 1,
                     button_buttons, button_name,
                     session, std::move(callback)));
}

// OnDragMoved: 드래그 중 한 단계 mouseMoved 완료 후 다음 단계로 진행한다.
void MouseTool::OnDragMoved(double start_x,
                             double start_y,
                             double end_x,
                             double end_y,
                             int total_steps,
                             int current_step,
                             int button_buttons,
                             const std::string& button_name,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback,
                             base::Value response) {
  if (HandleCdpError(response,
                     "Input.dispatchMouseEvent(mouseMoved/drag)", callback)) {
    return;
  }
  // 다음 드래그 이동 단계 발송
  DispatchDragMove(start_x, start_y, end_x, end_y, total_steps, current_step,
                   button_buttons, button_name, session, std::move(callback));
}

// DispatchDragRelease: 드래그 끝점에서 mouseReleased 이벤트를 발송한다.
void MouseTool::DispatchDragRelease(double end_x,
                                     double end_y,
                                     const std::string& button_name,
                                     McpSession* session,
                                     base::OnceCallback<void(base::Value)> callback) {
  LOG(INFO) << "[MouseTool] mouseReleased 발송: ("
            << end_x << "," << end_y
            << ") button=" << button_name;

  // buttons=0: 릴리즈 후 버튼이 눌리지 않은 상태
  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseReleased", end_x, end_y, button_name,
                           /*buttons=*/0),
      base::BindOnce(&MouseTool::OnDragReleased,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// OnDragReleased: mouseReleased 완료 후 성공 응답을 반환한다.
void MouseTool::OnDragReleased(base::OnceCallback<void(base::Value)> callback,
                                base::Value response) {
  if (HandleCdpError(response,
                     "Input.dispatchMouseEvent(mouseReleased)", callback)) {
    return;
  }
  LOG(INFO) << "[MouseTool] 드래그 앤 드롭 완료";
  std::move(callback).Run(
      MakeSuccessResult("드래그 앤 드롭이 성공적으로 완료되었습니다."));
}

// ============================================================
// 정적 헬퍼
// ============================================================

// HandleCdpError: CDP 응답에 에러가 있으면 에러 콜백을 실행하고 true를 반환한다.
// 에러가 없으면 false를 반환하며 콜백을 건드리지 않는다.
// NOLINTNEXTLINE(runtime/references)
bool MouseTool::HandleCdpError(
    const base::Value& response,
    const std::string& step_name,
    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[MouseTool] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(
      MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

}  // namespace mcp
