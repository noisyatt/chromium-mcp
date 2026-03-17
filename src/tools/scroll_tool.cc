// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/scroll_tool.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

namespace {

// 한 틱당 픽셀 수 (마우스 휠 한 눈금 = 120px)
constexpr double kScrollPixelsPerTick = 120.0;

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

// DOM.getBoxModel 응답에서 content quad의 중심 좌표를 계산한다.
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

// direction 문자열과 amount 틱 수를 deltaX/deltaY로 변환한다.
// CDP mouseWheel: deltaY 양수=위, 음수=아래 (브라우저 표준과 반대)
void DirectionToDeltas(const std::string& direction,
                       double amount,
                       double* out_delta_x,
                       double* out_delta_y) {
  double pixels = amount * kScrollPixelsPerTick;
  *out_delta_x = 0.0;
  *out_delta_y = 0.0;
  if (direction == "down") {
    *out_delta_y = -pixels;  // 아래로 스크롤
  } else if (direction == "up") {
    *out_delta_y = pixels;   // 위로 스크롤
  } else if (direction == "right") {
    *out_delta_x = -pixels;  // 오른쪽으로 스크롤
  } else if (direction == "left") {
    *out_delta_x = pixels;   // 왼쪽으로 스크롤
  }
}

}  // namespace

// ============================================================
// ScrollTool 구현
// ============================================================

ScrollTool::ScrollTool() = default;
ScrollTool::~ScrollTool() = default;

std::string ScrollTool::name() const {
  return "scroll";
}

std::string ScrollTool::description() const {
  return "페이지 또는 특정 요소에서 스크롤을 발생시킵니다. "
         "direction(up/down/left/right)과 amount(틱 수)로 간편하게 스크롤하거나, "
         "selector로 특정 요소 내부를 스크롤할 수 있습니다. "
         "toTop/toBottom으로 페이지 맨 위/아래로 즉시 이동할 수 있습니다.";
}

base::DictValue ScrollTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // direction: 스크롤 방향
  base::DictValue direction_prop;
  direction_prop.Set("type", "string");
  base::ListValue direction_enum;
  direction_enum.Append("up");
  direction_enum.Append("down");
  direction_enum.Append("left");
  direction_enum.Append("right");
  direction_prop.Set("enum", std::move(direction_enum));
  direction_prop.Set("description",
                     "스크롤 방향. up/down/left/right. "
                     "direction 지정 시 amount 파라미터와 함께 사용.");
  properties.Set("direction", std::move(direction_prop));

  // amount: 스크롤 틱 수
  base::DictValue amount_prop;
  amount_prop.Set("type", "number");
  amount_prop.Set("default", 3);
  amount_prop.Set("description",
                  "스크롤 틱 수. 1틱 = 120px. 기본값: 3 (360px). "
                  "direction 파라미터와 함께 사용.");
  properties.Set("amount", std::move(amount_prop));

  // x: 스크롤 이벤트를 발생시킬 X 좌표
  base::DictValue x_prop;
  x_prop.Set("type", "number");
  x_prop.Set("description",
             "스크롤 이벤트를 발생시킬 X 좌표 (픽셀). 생략 시 뷰포트 중앙.");
  properties.Set("x", std::move(x_prop));

  // y: 스크롤 이벤트를 발생시킬 Y 좌표
  base::DictValue y_prop;
  y_prop.Set("type", "number");
  y_prop.Set("description",
             "스크롤 이벤트를 발생시킬 Y 좌표 (픽셀). 생략 시 뷰포트 중앙.");
  properties.Set("y", std::move(y_prop));

  // deltaX: 수평 스크롤량 (픽셀 직접 지정)
  base::DictValue delta_x_prop;
  delta_x_prop.Set("type", "number");
  delta_x_prop.Set("default", 0);
  delta_x_prop.Set("description",
                   "수평 스크롤량 (픽셀). 양수: 왼쪽, 음수: 오른쪽. "
                   "direction 파라미터가 없을 때 직접 지정.");
  properties.Set("deltaX", std::move(delta_x_prop));

  // deltaY: 수직 스크롤량 (픽셀 직접 지정)
  base::DictValue delta_y_prop;
  delta_y_prop.Set("type", "number");
  delta_y_prop.Set("default", -300);
  delta_y_prop.Set("description",
                   "수직 스크롤량 (픽셀). 양수: 위쪽, 음수: 아래쪽. "
                   "direction 파라미터가 없을 때 직접 지정. 기본값: -300.");
  properties.Set("deltaY", std::move(delta_y_prop));

  // selector: 스크롤을 발생시킬 요소의 CSS 셀렉터
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description",
                    "스크롤을 발생시킬 요소의 CSS 셀렉터. "
                    "지정 시 해당 요소의 중심 좌표에서 스크롤 이벤트가 발생합니다.");
  properties.Set("selector", std::move(selector_prop));

  // toTop: 페이지 맨 위로 스크롤
  base::DictValue to_top_prop;
  to_top_prop.Set("type", "boolean");
  to_top_prop.Set("description",
                  "true이면 window.scrollTo(0, 0) 실행. "
                  "다른 스크롤 파라미터보다 우선 적용됩니다.");
  properties.Set("toTop", std::move(to_top_prop));

  // toBottom: 페이지 맨 아래로 스크롤
  base::DictValue to_bottom_prop;
  to_bottom_prop.Set("type", "boolean");
  to_bottom_prop.Set("description",
                     "true이면 window.scrollTo(0, document.body.scrollHeight) 실행. "
                     "toTop보다 낮은 우선순위. 다른 파라미터보다 우선 적용됩니다.");
  properties.Set("toBottom", std::move(to_bottom_prop));

  schema.Set("properties", std::move(properties));

  // 모든 파라미터가 선택적
  base::ListValue required;
  schema.Set("required", std::move(required));

  return schema;
}

void ScrollTool::Execute(const base::DictValue& arguments,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback) {
  // toTop/toBottom 처리 (최우선순위)
  std::optional<bool> to_top = arguments.FindBool("toTop");
  std::optional<bool> to_bottom = arguments.FindBool("toBottom");

  if (to_top.value_or(false)) {
    LOG(INFO) << "[ScrollTool] toTop 모드: window.scrollTo(0, 0)";
    EvaluateScroll("window.scrollTo(0, 0)", session, std::move(callback));
    return;
  }

  if (to_bottom.value_or(false)) {
    LOG(INFO) << "[ScrollTool] toBottom 모드: window.scrollTo(0, scrollHeight)";
    EvaluateScroll(
        "window.scrollTo(0, document.documentElement.scrollHeight || "
        "document.body.scrollHeight)",
        session, std::move(callback));
    return;
  }

  // direction 모드: 방향과 틱 수로 deltaX/deltaY 계산
  const std::string* direction = arguments.FindString("direction");
  double delta_x = 0.0;
  double delta_y = -300.0;  // 기본값: 아래로 300px

  if (direction && !direction->empty()) {
    std::optional<double> amount_opt = arguments.FindDouble("amount");
    double amount = amount_opt.value_or(3.0);
    if (amount < 0) amount = 3.0;
    DirectionToDeltas(*direction, amount, &delta_x, &delta_y);
    LOG(INFO) << "[ScrollTool] direction 모드: direction=" << *direction
              << " amount=" << amount
              << " deltaX=" << delta_x << " deltaY=" << delta_y;
  } else {
    // deltaX/deltaY 직접 지정
    std::optional<double> dx_opt = arguments.FindDouble("deltaX");
    std::optional<double> dy_opt = arguments.FindDouble("deltaY");
    delta_x = dx_opt.value_or(0.0);
    delta_y = dy_opt.value_or(-300.0);
  }

  // selector 지정 시: DOM 경로로 좌표 계산
  const std::string* selector = arguments.FindString("selector");
  if (selector && !selector->empty()) {
    LOG(INFO) << "[ScrollTool] selector 모드: selector=" << *selector
              << " deltaX=" << delta_x << " deltaY=" << delta_y;
    GetDocumentRoot(*selector, delta_x, delta_y, session, std::move(callback));
    return;
  }

  // 좌표 직접 지정 (없으면 0, 0)
  std::optional<double> x_opt = arguments.FindDouble("x");
  std::optional<double> y_opt = arguments.FindDouble("y");
  double x = x_opt.value_or(0.0);
  double y = y_opt.value_or(0.0);

  LOG(INFO) << "[ScrollTool] 좌표 모드: x=" << x << " y=" << y
            << " deltaX=" << delta_x << " deltaY=" << delta_y;
  DispatchScroll(x, y, delta_x, delta_y, session, std::move(callback));
}

// toTop/toBottom: Runtime.evaluate로 window.scrollTo 실행
void ScrollTool::EvaluateScroll(const std::string& expression,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("expression", expression);
  params.Set("returnByValue", false);
  params.Set("awaitPromise", false);

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&ScrollTool::OnEvaluateScroll,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// Runtime.evaluate 완료 콜백
void ScrollTool::OnEvaluateScroll(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Runtime.evaluate(scroll)", callback)) {
    return;
  }
  LOG(INFO) << "[ScrollTool] 페이지 스크롤 이동 완료";
  std::move(callback).Run(MakeSuccessResult("스크롤이 성공적으로 완료되었습니다."));
}

// 좌표로 직접 mouseWheel 이벤트 발송
void ScrollTool::DispatchScroll(double x,
                                double y,
                                double delta_x,
                                double delta_y,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("type", "mouseWheel");
  params.Set("x", x);
  params.Set("y", y);
  params.Set("deltaX", delta_x);
  params.Set("deltaY", delta_y);
  params.Set("modifiers", 0);

  session->SendCdpCommand(
      "Input.dispatchMouseEvent", std::move(params),
      base::BindOnce(&ScrollTool::OnScrollDispatched,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// selector 모드 Step 1: DOM.getDocument 호출
void ScrollTool::GetDocumentRoot(
    const std::string& selector,
    double delta_x,
    double delta_y,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("depth", 0);

  session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&ScrollTool::OnGetDocumentRoot,
                     weak_factory_.GetWeakPtr(),
                     selector, delta_x, delta_y, session,
                     std::move(callback)));
}

// selector 모드 Step 2: getDocument 응답 후 DOM.querySelector 호출
void ScrollTool::OnGetDocumentRoot(
    const std::string& selector,
    double delta_x,
    double delta_y,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getDocument", callback)) {
    return;
  }

  int root_node_id = ExtractRootNodeId(response);
  if (root_node_id <= 0) {
    LOG(ERROR) << "[ScrollTool] DOM.getDocument 응답에서 rootNodeId를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("DOM 루트 노드 ID를 획득할 수 없습니다."));
    return;
  }

  base::DictValue params;
  params.Set("nodeId", root_node_id);
  params.Set("selector", selector);

  session->SendCdpCommand(
      "DOM.querySelector", std::move(params),
      base::BindOnce(&ScrollTool::OnQuerySelector,
                     weak_factory_.GetWeakPtr(),
                     delta_x, delta_y, session, std::move(callback)));
}

// selector 모드 Step 3: querySelector 응답 후 DOM.getBoxModel 호출
void ScrollTool::OnQuerySelector(
    double delta_x,
    double delta_y,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.querySelector", callback)) {
    return;
  }

  int node_id = ExtractNodeId(response);
  if (node_id <= 0) {
    LOG(WARNING) << "[ScrollTool] 셀렉터에 일치하는 요소를 찾을 수 없음";
    std::move(callback).Run(
        MakeErrorResult("지정한 셀렉터에 일치하는 요소를 찾을 수 없습니다."));
    return;
  }

  base::DictValue params;
  params.Set("nodeId", node_id);

  session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(&ScrollTool::OnGetBoxModel,
                     weak_factory_.GetWeakPtr(),
                     delta_x, delta_y, session, std::move(callback)));
}

// selector 모드 Step 4: BoxModel에서 중심좌표 계산 후 스크롤 발송
void ScrollTool::OnGetBoxModel(
    double delta_x,
    double delta_y,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getBoxModel", callback)) {
    return;
  }

  double x = 0.0, y = 0.0;
  if (!ExtractBoxModelCenter(response, &x, &y)) {
    LOG(ERROR) << "[ScrollTool] BoxModel에서 중심 좌표를 추출할 수 없음";
    std::move(callback).Run(
        MakeErrorResult("요소의 BoxModel 좌표를 계산할 수 없습니다."));
    return;
  }

  LOG(INFO) << "[ScrollTool] 스크롤 좌표: (" << x << ", " << y << ")";
  DispatchScroll(x, y, delta_x, delta_y, session, std::move(callback));
}

// mouseWheel 완료 콜백
void ScrollTool::OnScrollDispatched(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.dispatchMouseEvent(mouseWheel)", callback)) {
    return;
  }
  LOG(INFO) << "[ScrollTool] 스크롤 완료";
  std::move(callback).Run(MakeSuccessResult("스크롤이 성공적으로 완료되었습니다."));
}

// 정적 헬퍼: CDP 에러 처리
// NOLINTNEXTLINE(runtime/references)
bool ScrollTool::HandleCdpError(
    const base::Value& response,
    const std::string& step_name,
    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[ScrollTool] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

}  // namespace mcp
