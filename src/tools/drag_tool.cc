// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/drag_tool.h"

#include <cmath>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

namespace {

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

// DOM.getDocument 응답에서 rootNodeId를 추출한다.
int ExtractRootNodeId(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  const base::DictValue* result = dict ? dict->FindDict("result") : nullptr;
  const base::DictValue* root = result ? result->FindDict("root") : nullptr;
  if (!root) {
    return -1;
  }
  std::optional<int> node_id = root->FindInt("nodeId");
  return node_id.value_or(-1);
}

// DOM.querySelector 응답에서 nodeId를 추출한다.
int ExtractNodeId(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return -1;
  }
  const base::DictValue* result = dict->FindDict("result");
  if (!result) {
    return -1;
  }
  std::optional<int> node_id = result->FindInt("nodeId");
  return node_id.value_or(-1);
}

// DOM.getBoxModel 응답에서 content quad 중심 좌표를 계산한다.
bool ExtractBoxModelCenter(const base::Value& response,
                           double* out_x,
                           double* out_y) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return false;
  }
  const base::DictValue* result = dict->FindDict("result");
  if (!result) {
    return false;
  }
  const base::DictValue* model = result->FindDict("model");
  if (!model) {
    return false;
  }
  const base::ListValue* content = model->FindList("content");
  if (!content || content->size() < 8) {
    return false;
  }
  // 4개 꼭짓점 좌표의 평균으로 중심점 계산
  double sum_x = 0.0, sum_y = 0.0;
  for (size_t i = 0; i < 8; i += 2) {
    sum_x += (*content)[i].GetIfDouble().value_or(0.0);
    sum_y += (*content)[i + 1].GetIfDouble().value_or(0.0);
  }
  *out_x = sum_x / 4.0;
  *out_y = sum_y / 4.0;
  return true;
}

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
         "startSelector/endSelector로 요소를 지정하거나, "
         "startX/startY, endX/endY로 좌표를 직접 지정할 수 있습니다.";
}

base::DictValue DragTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // startSelector: 드래그 시작 요소의 CSS 셀렉터
  base::DictValue start_selector_prop;
  start_selector_prop.Set("type", "string");
  start_selector_prop.Set("description",
                           "드래그를 시작할 요소의 CSS 셀렉터. "
                           "지정 시 요소의 중심 좌표에서 드래그가 시작됩니다.");
  properties.Set("startSelector", std::move(start_selector_prop));

  // endSelector: 드롭 대상 요소의 CSS 셀렉터
  base::DictValue end_selector_prop;
  end_selector_prop.Set("type", "string");
  end_selector_prop.Set("description",
                         "드래그를 끝낼(드롭할) 요소의 CSS 셀렉터. "
                         "지정 시 요소의 중심 좌표에서 마우스가 릴리즈됩니다.");
  properties.Set("endSelector", std::move(end_selector_prop));

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

  const std::string* start_selector = arguments.FindString("startSelector");
  const std::string* end_selector = arguments.FindString("endSelector");

  if (start_selector && !start_selector->empty() &&
      end_selector && !end_selector->empty()) {
    // selector 모드: DOM 경로로 시작/끝 좌표 계산
    LOG(INFO) << "[DragTool] selector 모드: start=" << *start_selector
              << " end=" << *end_selector << " steps=" << steps;
    GetDocumentRootForStart(*start_selector, *end_selector, steps, session,
                            std::move(callback));
    return;
  }

  // 좌표 직접 지정 모드
  std::optional<double> start_x_opt = arguments.FindDouble("startX");
  std::optional<double> start_y_opt = arguments.FindDouble("startY");
  std::optional<double> end_x_opt = arguments.FindDouble("endX");
  std::optional<double> end_y_opt = arguments.FindDouble("endY");

  if (!start_x_opt.has_value() || !start_y_opt.has_value() ||
      !end_x_opt.has_value() || !end_y_opt.has_value()) {
    LOG(WARNING) << "[DragTool] startSelector/endSelector 또는 "
                    "startX/startY/endX/endY 파라미터가 필요합니다.";
    std::move(callback).Run(
        MakeErrorResult("startSelector/endSelector 또는 "
                        "startX/startY/endX/endY 파라미터가 필요합니다."));
    return;
  }

  LOG(INFO) << "[DragTool] 좌표 모드: start=(" << *start_x_opt << ","
            << *start_y_opt << ") end=(" << *end_x_opt << "," << *end_y_opt
            << ") steps=" << steps;

  StartDragSequence(*start_x_opt, *start_y_opt, *end_x_opt, *end_y_opt,
                    steps, session, std::move(callback));
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
  // buttons=1: 왼쪽 마우스 버튼이 눌린 상태
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
  if (HandleCdpError(response, "Input.dispatchMouseEvent(mousePressed)", callback)) {
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
    // 모든 중간 단계 완료 → mouseReleased 발송
    LOG(INFO) << "[DragTool] mouseMoved 시퀀스 완료, mouseReleased 발송";
    DispatchMouseReleased(end_x, end_y, session, std::move(callback));
    return;
  }

  // 선형 보간으로 현재 단계의 좌표 계산
  // t: 0.0 → 1.0 (첫 step은 살짝 이동한 위치부터 시작)
  double t = static_cast<double>(current_step + 1) / static_cast<double>(steps);
  double move_x = Lerp(start_x, end_x, t);
  double move_y = Lerp(start_y, end_y, t);

  // buttons=1: 왼쪽 버튼이 계속 눌린 상태에서 이동
  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseMoved", move_x, move_y, "left", 1),
      base::BindOnce(&DragTool::OnMouseMoved,
                     weak_factory_.GetWeakPtr(),
                     start_x, start_y, end_x, end_y, steps, current_step + 1,
                     session, std::move(callback)));
}

// mouseMoved 완료 콜백 — 다음 step으로 진행
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
  if (HandleCdpError(response, "Input.dispatchMouseEvent(mouseMoved)", callback)) {
    return;
  }

  // 다음 step 발송
  DispatchNextMove(start_x, start_y, end_x, end_y, steps, current_step,
                   session, std::move(callback));
}

// mouseReleased 발송
void DragTool::DispatchMouseReleased(
    double end_x,
    double end_y,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // buttons=0: 마우스 버튼이 릴리즈된 상태
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
  if (HandleCdpError(response, "Input.dispatchMouseEvent(mouseReleased)", callback)) {
    return;
  }
  LOG(INFO) << "[DragTool] 드래그 앤 드롭 완료";
  std::move(callback).Run(
      MakeSuccessResult("드래그 앤 드롭이 성공적으로 완료되었습니다."));
}

// ============================================================
// selector 모드: DOM 경로로 시작/끝 좌표 계산
// ============================================================

// Step A: DOM.getDocument 호출
void DragTool::GetDocumentRootForStart(
    const std::string& start_selector,
    const std::string& end_selector,
    int steps,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("depth", 0);

  session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&DragTool::OnGetDocumentRootForStart,
                     weak_factory_.GetWeakPtr(),
                     start_selector, end_selector, steps,
                     session, std::move(callback)));
}

// Step B: getDocument 응답 후 startSelector로 DOM.querySelector 호출
void DragTool::OnGetDocumentRootForStart(
    const std::string& start_selector,
    const std::string& end_selector,
    int steps,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getDocument", callback)) {
    return;
  }

  int root_node_id = ExtractRootNodeId(response);
  if (root_node_id <= 0) {
    LOG(ERROR) << "[DragTool] DOM.getDocument 응답에서 rootNodeId를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("DOM 루트 노드 ID를 획득할 수 없습니다."));
    return;
  }

  base::DictValue params;
  params.Set("nodeId", root_node_id);
  params.Set("selector", start_selector);

  session->SendCdpCommand(
      "DOM.querySelector", std::move(params),
      base::BindOnce(&DragTool::OnQuerySelectorStart,
                     weak_factory_.GetWeakPtr(),
                     end_selector, steps, session, std::move(callback)));
}

// Step C: startSelector querySelector 응답 후 start BoxModel 호출
void DragTool::OnQuerySelectorStart(
    const std::string& end_selector,
    int steps,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.querySelector(start)", callback)) {
    return;
  }

  int node_id = ExtractNodeId(response);
  if (node_id <= 0) {
    LOG(WARNING) << "[DragTool] 시작 셀렉터에 일치하는 요소를 찾을 수 없음";
    std::move(callback).Run(
        MakeErrorResult("시작 요소(startSelector)를 찾을 수 없습니다."));
    return;
  }

  base::DictValue params;
  params.Set("nodeId", node_id);

  session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(&DragTool::OnGetBoxModelStart,
                     weak_factory_.GetWeakPtr(),
                     end_selector, steps, session, std::move(callback)));
}

// Step D: start BoxModel 응답 후 endSelector로 DOM.querySelector 호출
void DragTool::OnGetBoxModelStart(
    const std::string& end_selector,
    int steps,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getBoxModel(start)", callback)) {
    return;
  }

  double start_x = 0.0, start_y = 0.0;
  if (!ExtractBoxModelCenter(response, &start_x, &start_y)) {
    LOG(ERROR) << "[DragTool] 시작 요소 BoxModel에서 좌표를 추출할 수 없음";
    std::move(callback).Run(
        MakeErrorResult("시작 요소의 BoxModel 좌표를 계산할 수 없습니다."));
    return;
  }

  LOG(INFO) << "[DragTool] 시작 좌표: (" << start_x << ", " << start_y << ")";

  // endSelector는 같은 rootNodeId를 재활용 (이미 DOM 트리 확인됨)
  // 여기서는 간단히 DOM.getDocument를 다시 호출하지 않고
  // rootNodeId=1 (document root의 일반적인 값)로 querySelector를 시도
  // 실제 구현에서는 root_node_id를 멤버 변수로 캐싱하는 것이 바람직하나
  // 여기서는 depth=0으로 다시 getDocument를 호출하는 방식 사용
  base::DictValue doc_params;
  doc_params.Set("depth", 0);

  // start_x, start_y를 OnQuerySelectorEnd에 전달하기 위해 BindOnce 활용
  session->SendCdpCommand(
      "DOM.getDocument", std::move(doc_params),
      base::BindOnce(
          [](DragTool* self, const std::string& end_sel, double sx, double sy,
             int st, McpSession* sess,
             base::OnceCallback<void(base::Value)> cb,
             base::Value doc_response) {
            if (self->HandleCdpError(doc_response, "DOM.getDocument(end)", cb)) {
              return;
            }
            int root_node_id = ExtractRootNodeId(doc_response);
            if (root_node_id <= 0) {
              std::move(cb).Run(
                  MakeErrorResult("DOM 루트 노드 ID를 획득할 수 없습니다."));
              return;
            }
            base::DictValue qs_params;
            qs_params.Set("nodeId", root_node_id);
            qs_params.Set("selector", end_sel);
            sess->SendCdpCommand(
                "DOM.querySelector", std::move(qs_params),
                base::BindOnce(&DragTool::OnQuerySelectorEnd,
                               self->weak_factory_.GetWeakPtr(),
                               sx, sy, st, sess, std::move(cb)));
          },
          base::Unretained(this),
          end_selector, start_x, start_y, steps, session,
          std::move(callback)));
}

// Step E: endSelector querySelector 응답 후 end BoxModel 호출
void DragTool::OnQuerySelectorEnd(
    double start_x,
    double start_y,
    int steps,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.querySelector(end)", callback)) {
    return;
  }

  int node_id = ExtractNodeId(response);
  if (node_id <= 0) {
    LOG(WARNING) << "[DragTool] 끝 셀렉터에 일치하는 요소를 찾을 수 없음";
    std::move(callback).Run(
        MakeErrorResult("끝 요소(endSelector)를 찾을 수 없습니다."));
    return;
  }

  base::DictValue params;
  params.Set("nodeId", node_id);

  session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(&DragTool::OnGetBoxModelEnd,
                     weak_factory_.GetWeakPtr(),
                     start_x, start_y, steps, session, std::move(callback)));
}

// Step F: end BoxModel 응답 후 드래그 시퀀스 시작
void DragTool::OnGetBoxModelEnd(
    double start_x,
    double start_y,
    int steps,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getBoxModel(end)", callback)) {
    return;
  }

  double end_x = 0.0, end_y = 0.0;
  if (!ExtractBoxModelCenter(response, &end_x, &end_y)) {
    LOG(ERROR) << "[DragTool] 끝 요소 BoxModel에서 좌표를 추출할 수 없음";
    std::move(callback).Run(
        MakeErrorResult("끝 요소의 BoxModel 좌표를 계산할 수 없습니다."));
    return;
  }

  LOG(INFO) << "[DragTool] 끝 좌표: (" << end_x << ", " << end_y << ")";
  StartDragSequence(start_x, start_y, end_x, end_y, steps, session,
                    std::move(callback));
}

// 정적 헬퍼: CDP 에러 처리
// NOLINTNEXTLINE(runtime/references)
bool DragTool::HandleCdpError(
    const base::Value& response,
    const std::string& step_name,
    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[DragTool] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

}  // namespace mcp
