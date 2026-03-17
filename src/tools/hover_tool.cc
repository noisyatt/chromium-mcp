// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/hover_tool.h"

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
  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict item;
  item.Set("type", "text");
  item.Set("text", message);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", false);
  return base::Value(std::move(result));
}

// MCP 에러 응답 Value 생성
base::Value MakeErrorResult(const std::string& message) {
  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict item;
  item.Set("type", "text");
  item.Set("text", message);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", true);
  return base::Value(std::move(result));
}

// CDP 응답에 "error" 키가 있는지 확인한다.
bool HasCdpError(const base::Value& response) {
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict) {
    return true;
  }
  return dict->Find("error") != nullptr;
}

// CDP 응답에서 에러 메시지를 추출한다.
std::string ExtractCdpErrorMessage(const base::Value& response) {
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict) {
    return "CDP 응답이 Dict 형식이 아님";
  }
  const base::Value::Dict* error = dict->FindDict("error");
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
  const base::Value::Dict* dict = response.GetIfDict();
  const base::Value::Dict* result = dict ? dict->FindDict("result") : nullptr;
  const base::Value::Dict* root = result ? result->FindDict("root") : nullptr;
  if (!root) {
    return -1;
  }
  std::optional<int> node_id = root->FindInt("nodeId");
  return node_id.value_or(-1);
}

// DOM.querySelector 응답에서 nodeId를 추출한다.
int ExtractNodeId(const base::Value& response) {
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict) {
    return -1;
  }
  const base::Value::Dict* result = dict->FindDict("result");
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
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict) {
    return false;
  }
  const base::Value::Dict* result = dict->FindDict("result");
  if (!result) {
    return false;
  }
  const base::Value::Dict* model = result->FindDict("model");
  if (!model) {
    return false;
  }
  const base::Value::List* content = model->FindList("content");
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

}  // namespace

// ============================================================
// HoverTool 구현
// ============================================================

HoverTool::HoverTool() = default;
HoverTool::~HoverTool() = default;

std::string HoverTool::name() const {
  return "hover";
}

std::string HoverTool::description() const {
  return "요소 위에 마우스 커서를 위치시켜 호버 이벤트를 발생시킵니다. "
         "selector로 CSS 셀렉터를 지정하거나, x/y 좌표를 직접 지정할 수 있습니다. "
         "tooltip 표시, 드롭다운 메뉴 열기 등의 호버 인터랙션에 사용합니다.";
}

base::Value::Dict HoverTool::input_schema() const {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // selector: 호버할 요소의 CSS 셀렉터
  base::Value::Dict selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description",
                    "호버할 요소의 CSS 셀렉터. "
                    "지정 시 해당 요소의 중심 좌표에서 mouseMoved 이벤트가 발생합니다.");
  properties.Set("selector", std::move(selector_prop));

  // x: 직접 좌표 지정 시 X 좌표
  base::Value::Dict x_prop;
  x_prop.Set("type", "number");
  x_prop.Set("description", "호버 이벤트를 발생시킬 X 좌표 (픽셀). selector가 없을 때 사용.");
  properties.Set("x", std::move(x_prop));

  // y: 직접 좌표 지정 시 Y 좌표
  base::Value::Dict y_prop;
  y_prop.Set("type", "number");
  y_prop.Set("description", "호버 이벤트를 발생시킬 Y 좌표 (픽셀). selector가 없을 때 사용.");
  properties.Set("y", std::move(y_prop));

  schema.Set("properties", std::move(properties));

  // selector 또는 x/y 중 하나 필요 (런타임 검증)
  base::Value::List required;
  schema.Set("required", std::move(required));

  return schema;
}

void HoverTool::Execute(const base::Value::Dict& arguments,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback) {
  const std::string* selector = arguments.FindString("selector");
  if (selector && !selector->empty()) {
    // selector 모드: DOM 경로로 좌표 계산
    LOG(INFO) << "[HoverTool] selector 모드: selector=" << *selector;
    GetDocumentRoot(*selector, session, std::move(callback));
    return;
  }

  // 좌표 직접 지정 모드
  std::optional<double> x_opt = arguments.FindDouble("x");
  std::optional<double> y_opt = arguments.FindDouble("y");

  if (!x_opt.has_value() || !y_opt.has_value()) {
    LOG(WARNING) << "[HoverTool] selector 또는 x/y 좌표 파라미터가 필요합니다.";
    std::move(callback).Run(
        MakeErrorResult("selector 또는 x/y 좌표 파라미터가 필요합니다."));
    return;
  }

  LOG(INFO) << "[HoverTool] 좌표 모드: x=" << *x_opt << " y=" << *y_opt;
  DispatchHover(*x_opt, *y_opt, session, std::move(callback));
}

// 좌표로 직접 mouseMoved 이벤트 발송
void HoverTool::DispatchHover(double x,
                              double y,
                              McpSession* session,
                              base::OnceCallback<void(base::Value)> callback) {
  base::Value::Dict params;
  params.Set("type", "mouseMoved");
  params.Set("x", x);
  params.Set("y", y);
  params.Set("button", "none");
  params.Set("buttons", 0);
  params.Set("modifiers", 0);

  session->SendCdpCommand(
      "Input.dispatchMouseEvent", std::move(params),
      base::BindOnce(&HoverTool::OnHoverDispatched,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// selector 모드 Step 1: DOM.getDocument 호출
void HoverTool::GetDocumentRoot(
    const std::string& selector,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::Value::Dict params;
  params.Set("depth", 0);

  session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&HoverTool::OnGetDocumentRoot,
                     weak_factory_.GetWeakPtr(),
                     selector, session, std::move(callback)));
}

// selector 모드 Step 2: getDocument 응답 후 DOM.querySelector 호출
void HoverTool::OnGetDocumentRoot(
    const std::string& selector,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getDocument", callback)) {
    return;
  }

  int root_node_id = ExtractRootNodeId(response);
  if (root_node_id <= 0) {
    LOG(ERROR) << "[HoverTool] DOM.getDocument 응답에서 rootNodeId를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("DOM 루트 노드 ID를 획득할 수 없습니다."));
    return;
  }

  base::Value::Dict params;
  params.Set("nodeId", root_node_id);
  params.Set("selector", selector);

  session->SendCdpCommand(
      "DOM.querySelector", std::move(params),
      base::BindOnce(&HoverTool::OnQuerySelector,
                     weak_factory_.GetWeakPtr(),
                     session, std::move(callback)));
}

// selector 모드 Step 3: querySelector 응답 후 DOM.getBoxModel 호출
void HoverTool::OnQuerySelector(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.querySelector", callback)) {
    return;
  }

  int node_id = ExtractNodeId(response);
  if (node_id <= 0) {
    LOG(WARNING) << "[HoverTool] 셀렉터에 일치하는 요소를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("지정한 셀렉터에 일치하는 요소를 찾을 수 없습니다."));
    return;
  }

  base::Value::Dict params;
  params.Set("nodeId", node_id);

  session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(&HoverTool::OnGetBoxModel,
                     weak_factory_.GetWeakPtr(),
                     session, std::move(callback)));
}

// selector 모드 Step 4: BoxModel에서 중심좌표 계산 후 호버 발송
void HoverTool::OnGetBoxModel(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getBoxModel", callback)) {
    return;
  }

  double x = 0.0, y = 0.0;
  if (!ExtractBoxModelCenter(response, &x, &y)) {
    LOG(ERROR) << "[HoverTool] BoxModel에서 중심 좌표를 추출할 수 없음";
    std::move(callback).Run(MakeErrorResult("요소의 BoxModel 좌표를 계산할 수 없습니다."));
    return;
  }

  LOG(INFO) << "[HoverTool] 호버 좌표: (" << x << ", " << y << ")";
  DispatchHover(x, y, session, std::move(callback));
}

// mouseMoved 완료 콜백
void HoverTool::OnHoverDispatched(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.dispatchMouseEvent(mouseMoved)", callback)) {
    return;
  }
  LOG(INFO) << "[HoverTool] 호버 완료";
  std::move(callback).Run(MakeSuccessResult("호버가 성공적으로 완료되었습니다."));
}

// 정적 헬퍼: CDP 에러 처리
// NOLINTNEXTLINE(runtime/references)
bool HoverTool::HandleCdpError(
    const base::Value& response,
    const std::string& step_name,
    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[HoverTool] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

}  // namespace mcp
