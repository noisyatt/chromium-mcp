// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/dom_tool.h"

#include <cmath>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"  // McpSession::SendCdpCommand

namespace mcp {

namespace {

// MCP 성공 응답 Value를 생성한다.
// content 배열에 type=text 항목 하나를 포함하는 표준 MCP 형식.
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

// MCP 에러 응답 Value를 생성한다.
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

// CDP 응답 Dict에 "error" 키가 있는지 확인한다.
bool HasCdpError(const base::Value& response) {
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict) {
    return true;
  }
  return dict->Find("error") != nullptr;
}

// CDP 응답에서 에러 메시지 문자열을 추출한다.
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

// CDP 응답 result.nodeId 값을 추출한다. 없으면 -1 반환.
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

// DOM.getBoxModel 응답에서 content quad를 파싱하여 중심 좌표를 계산한다.
// content quad는 [x1,y1, x2,y2, x3,y3, x4,y4] 형태의 8개 double 배열.
// 반환값: 성공 시 true, 좌표는 out_x / out_y 에 기록.
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
  // "content" 키: 요소의 content-box 꼭짓점 좌표 배열
  const base::Value::List* content = model->FindList("content");
  if (!content || content->size() < 8) {
    return false;
  }

  // 4개 꼭짓점의 평균으로 중심점 계산
  double sum_x = 0.0, sum_y = 0.0;
  for (size_t i = 0; i < 8; i += 2) {
    sum_x += (*content)[i].GetIfDouble().value_or(0.0);
    sum_y += (*content)[i + 1].GetIfDouble().value_or(0.0);
  }
  *out_x = sum_x / 4.0;
  *out_y = sum_y / 4.0;
  return true;
}

// Input.dispatchMouseEvent 파라미터 Dict를 생성한다.
// type: "mousePressed" 또는 "mouseReleased"
// button: "left", "right", "middle"
base::Value::Dict MakeMouseEventParams(const std::string& type,
                                       double x,
                                       double y,
                                       const std::string& button) {
  // CDP 버튼 인덱스: left=0, middle=1, right=2
  int button_index = 0;
  if (button == "middle") {
    button_index = 1;
  } else if (button == "right") {
    button_index = 2;
  }

  base::Value::Dict params;
  params.Set("type", type);
  params.Set("x", x);
  params.Set("y", y);
  params.Set("button", button);
  params.Set("buttons", button_index == 0 ? 1 : (button_index == 1 ? 4 : 2));
  params.Set("clickCount", 1);
  params.Set("modifiers", 0);
  return params;
}

}  // namespace

// ============================================================
// ClickTool 구현
// ============================================================

ClickTool::ClickTool() = default;
ClickTool::~ClickTool() = default;

std::string ClickTool::name() const {
  return "click";
}

std::string ClickTool::description() const {
  return "CSS 셀렉터 또는 ref로 지정한 DOM 요소를 클릭합니다. "
         "button으로 마우스 버튼을 선택하고, "
         "waitForNavigation으로 페이지 이동 완료를 기다릴 수 있습니다.";
}

base::Value::Dict ClickTool::input_schema() const {
  // JSON Schema (draft-07 형식)
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // selector: CSS 셀렉터 문자열
  base::Value::Dict selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description", "클릭할 요소의 CSS 셀렉터 (예: \"#submit\", \".btn\")");
  properties.Set("selector", std::move(selector_prop));

  // ref: 접근성 트리 ref (selector와 둘 중 하나 사용)
  base::Value::Dict ref_prop;
  ref_prop.Set("type", "string");
  ref_prop.Set("description", "접근성 스냅샷에서 얻은 요소 ref");
  properties.Set("ref", std::move(ref_prop));

  // button: 마우스 버튼 종류
  base::Value::Dict button_prop;
  button_prop.Set("type", "string");
  base::Value::List button_enum;
  button_enum.Append("left");
  button_enum.Append("right");
  button_enum.Append("middle");
  button_prop.Set("enum", std::move(button_enum));
  button_prop.Set("default", "left");
  button_prop.Set("description", "클릭에 사용할 마우스 버튼");
  properties.Set("button", std::move(button_prop));

  // waitForNavigation: 클릭 후 페이지 이동 대기 여부
  base::Value::Dict wait_prop;
  wait_prop.Set("type", "boolean");
  wait_prop.Set("default", false);
  wait_prop.Set("description", "true이면 Page.loadEventFired 이벤트까지 대기");
  properties.Set("waitForNavigation", std::move(wait_prop));

  schema.Set("properties", std::move(properties));

  // selector와 ref 중 하나는 필수
  // (런타임에서 검증하므로 required 배열은 빈 채로 둠)
  base::Value::List required;
  schema.Set("required", std::move(required));

  return schema;
}

void ClickTool::Execute(const base::Value::Dict& arguments,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback) {
  // selector / ref 중 하나 필수
  const std::string* selector = arguments.FindString("selector");
  const std::string* ref = arguments.FindString("ref");

  std::string target_selector;
  if (selector && !selector->empty()) {
    target_selector = *selector;
  } else if (ref && !ref->empty()) {
    // ref는 접근성 트리 ID — 여기서는 data-ref 속성 기반 CSS 셀렉터로 변환
    target_selector = "[data-ref=\"" + *ref + "\"]";
  } else {
    LOG(WARNING) << "[ClickTool] selector 또는 ref 파라미터가 필요합니다.";
    std::move(callback).Run(MakeErrorResult("selector 또는 ref 파라미터가 필요합니다."));
    return;
  }

  // button 파라미터 (기본값: "left")
  const std::string* button_param = arguments.FindString("button");
  std::string button = (button_param && !button_param->empty()) ? *button_param : "left";
  if (button != "left" && button != "right" && button != "middle") {
    LOG(WARNING) << "[ClickTool] 알 수 없는 버튼 값: " << button << " → left로 대체";
    button = "left";
  }

  // waitForNavigation 파라미터 (기본값: false)
  std::optional<bool> wait_opt = arguments.FindBool("waitForNavigation");
  bool wait_for_navigation = wait_opt.value_or(false);

  LOG(INFO) << "[ClickTool] 실행: selector=" << target_selector
            << " button=" << button
            << " waitForNavigation=" << wait_for_navigation;

  GetDocumentRoot(target_selector, button, wait_for_navigation, session,
                  std::move(callback));
}

// Step 1: DOM.getDocument 호출 → 루트 nodeId 획득
void ClickTool::GetDocumentRoot(
    const std::string& selector,
    const std::string& button,
    bool wait_for_navigation,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::Value::Dict params;
  params.Set("depth", 0);  // 루트만 필요

  session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&ClickTool::OnGetDocumentRoot,
                     weak_factory_.GetWeakPtr(),
                     selector, button, wait_for_navigation,
                     session, std::move(callback)));
}

// Step 2: getDocument 응답으로 rootNodeId 추출 후 DOM.querySelector 호출
void ClickTool::OnGetDocumentRoot(
    const std::string& selector,
    const std::string& button,
    bool wait_for_navigation,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getDocument", callback)) {
    return;
  }

  // result.root.nodeId 추출
  const base::Value::Dict* dict = response.GetIfDict();
  const base::Value::Dict* result = dict ? dict->FindDict("result") : nullptr;
  const base::Value::Dict* root = result ? result->FindDict("root") : nullptr;
  std::optional<int> root_node_id = root ? root->FindInt("nodeId") : std::nullopt;

  if (!root_node_id.has_value() || *root_node_id <= 0) {
    LOG(ERROR) << "[ClickTool] DOM.getDocument 응답에서 rootNodeId를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("DOM 루트 노드 ID를 획득할 수 없습니다."));
    return;
  }

  base::Value::Dict params;
  params.Set("nodeId", *root_node_id);
  params.Set("selector", selector);

  session->SendCdpCommand(
      "DOM.querySelector", std::move(params),
      base::BindOnce(&ClickTool::OnQuerySelector,
                     weak_factory_.GetWeakPtr(),
                     button, wait_for_navigation,
                     session, std::move(callback)));
}

// Step 3: querySelector 응답으로 nodeId 획득 후 DOM.getBoxModel 호출
void ClickTool::OnQuerySelector(
    const std::string& button,
    bool wait_for_navigation,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.querySelector", callback)) {
    return;
  }

  int node_id = ExtractNodeId(response);
  if (node_id <= 0) {
    LOG(WARNING) << "[ClickTool] 셀렉터에 일치하는 요소를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("지정한 셀렉터에 일치하는 요소를 찾을 수 없습니다."));
    return;
  }

  base::Value::Dict params;
  params.Set("nodeId", node_id);

  session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(&ClickTool::OnGetBoxModel,
                     weak_factory_.GetWeakPtr(),
                     button, wait_for_navigation,
                     session, std::move(callback)));
}

// Step 4: BoxModel에서 중심 좌표 계산 후 mousePressed 발송
void ClickTool::OnGetBoxModel(
    const std::string& button,
    bool wait_for_navigation,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getBoxModel", callback)) {
    return;
  }

  double x = 0.0, y = 0.0;
  if (!ExtractBoxModelCenter(response, &x, &y)) {
    LOG(ERROR) << "[ClickTool] BoxModel에서 좌표를 추출할 수 없음";
    std::move(callback).Run(MakeErrorResult("요소의 BoxModel 좌표를 계산할 수 없습니다."));
    return;
  }

  LOG(INFO) << "[ClickTool] 클릭 좌표: (" << x << ", " << y << ")";

  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mousePressed", x, y, button),
      base::BindOnce(&ClickTool::OnMousePressed,
                     weak_factory_.GetWeakPtr(),
                     x, y, button, wait_for_navigation,
                     session, std::move(callback)));
}

// Step 5: mousePressed 완료 후 mouseReleased 발송
void ClickTool::OnMousePressed(
    double x,
    double y,
    const std::string& button,
    bool wait_for_navigation,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.dispatchMouseEvent(mousePressed)", callback)) {
    return;
  }

  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseReleased", x, y, button),
      base::BindOnce(&ClickTool::OnMouseReleased,
                     weak_factory_.GetWeakPtr(),
                     wait_for_navigation,
                     session, std::move(callback)));
}

// Step 6: mouseReleased 완료
void ClickTool::OnMouseReleased(
    bool wait_for_navigation,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.dispatchMouseEvent(mouseReleased)", callback)) {
    return;
  }

  if (!wait_for_navigation) {
    // 네비게이션 대기 불필요 → 즉시 성공 반환
    LOG(INFO) << "[ClickTool] 클릭 완료";
    std::move(callback).Run(MakeSuccessResult("클릭이 성공적으로 완료되었습니다."));
    return;
  }

  // Page.loadEventFired 이벤트 구독 후 대기.
  // McpSession이 단발성 CDP 이벤트 리스너를 지원한다고 가정한다.
  LOG(INFO) << "[ClickTool] 페이지 로드 이벤트 대기 중...";
  session->SendCdpCommand(
      "Page.loadEventFired",
      base::Value::Dict(),
      base::BindOnce(&ClickTool::OnLoadEventFired,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// Step 7 (optional): loadEventFired 수신 후 완료 처리
void ClickTool::OnLoadEventFired(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  LOG(INFO) << "[ClickTool] 페이지 로드 완료";
  std::move(callback).Run(MakeSuccessResult("클릭 후 페이지 로드가 완료되었습니다."));
}

// 정적 헬퍼: CDP 에러 응답 처리
// NOLINTNEXTLINE(runtime/references)
bool ClickTool::HandleCdpError(
    const base::Value& response,
    const std::string& step_name,
    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[ClickTool] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

// ============================================================
// FillTool 구현
// ============================================================

FillTool::FillTool() = default;
FillTool::~FillTool() = default;

std::string FillTool::name() const {
  return "fill";
}

std::string FillTool::description() const {
  return "CSS 셀렉터 또는 ref로 지정한 입력 필드(input, textarea 등)에 "
         "텍스트 값을 입력합니다. 기존 내용은 자동으로 삭제됩니다.";
}

base::Value::Dict FillTool::input_schema() const {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  base::Value::Dict selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description", "입력 필드의 CSS 셀렉터 (예: \"input[name='q']\")");
  properties.Set("selector", std::move(selector_prop));

  base::Value::Dict ref_prop;
  ref_prop.Set("type", "string");
  ref_prop.Set("description", "접근성 스냅샷에서 얻은 요소 ref");
  properties.Set("ref", std::move(ref_prop));

  base::Value::Dict value_prop;
  value_prop.Set("type", "string");
  value_prop.Set("description", "입력할 텍스트 값");
  properties.Set("value", std::move(value_prop));

  schema.Set("properties", std::move(properties));

  // value는 필수
  base::Value::List required;
  required.Append("value");
  schema.Set("required", std::move(required));

  return schema;
}

void FillTool::Execute(const base::Value::Dict& arguments,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback) {
  const std::string* selector = arguments.FindString("selector");
  const std::string* ref = arguments.FindString("ref");
  const std::string* value = arguments.FindString("value");

  if (!value) {
    LOG(WARNING) << "[FillTool] value 파라미터가 누락되었습니다.";
    std::move(callback).Run(MakeErrorResult("value 파라미터가 필요합니다."));
    return;
  }

  std::string target_selector;
  if (selector && !selector->empty()) {
    target_selector = *selector;
  } else if (ref && !ref->empty()) {
    target_selector = "[data-ref=\"" + *ref + "\"]";
  } else {
    LOG(WARNING) << "[FillTool] selector 또는 ref 파라미터가 필요합니다.";
    std::move(callback).Run(MakeErrorResult("selector 또는 ref 파라미터가 필요합니다."));
    return;
  }

  LOG(INFO) << "[FillTool] 실행: selector=" << target_selector
            << " value=" << *value;

  GetDocumentRoot(target_selector, *value, session, std::move(callback));
}

// Step 1: DOM.getDocument 호출
void FillTool::GetDocumentRoot(
    const std::string& selector,
    const std::string& value,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::Value::Dict params;
  params.Set("depth", 0);

  session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&FillTool::OnGetDocumentRoot,
                     weak_factory_.GetWeakPtr(),
                     selector, value, session, std::move(callback)));
}

// Step 2: getDocument 응답 후 DOM.querySelector 호출
void FillTool::OnGetDocumentRoot(
    const std::string& selector,
    const std::string& value,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.getDocument", callback)) {
    return;
  }

  const base::Value::Dict* dict = response.GetIfDict();
  const base::Value::Dict* result = dict ? dict->FindDict("result") : nullptr;
  const base::Value::Dict* root = result ? result->FindDict("root") : nullptr;
  std::optional<int> root_node_id = root ? root->FindInt("nodeId") : std::nullopt;

  if (!root_node_id.has_value() || *root_node_id <= 0) {
    LOG(ERROR) << "[FillTool] DOM.getDocument 응답에서 rootNodeId를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("DOM 루트 노드 ID를 획득할 수 없습니다."));
    return;
  }

  base::Value::Dict params;
  params.Set("nodeId", *root_node_id);
  params.Set("selector", selector);

  session->SendCdpCommand(
      "DOM.querySelector", std::move(params),
      base::BindOnce(&FillTool::OnQuerySelector,
                     weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

// Step 3: querySelector 응답 후 DOM.focus 호출
void FillTool::OnQuerySelector(
    const std::string& value,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.querySelector", callback)) {
    return;
  }

  int node_id = ExtractNodeId(response);
  if (node_id <= 0) {
    LOG(WARNING) << "[FillTool] 셀렉터에 일치하는 요소를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("지정한 셀렉터에 일치하는 요소를 찾을 수 없습니다."));
    return;
  }

  base::Value::Dict params;
  params.Set("nodeId", node_id);

  session->SendCdpCommand(
      "DOM.focus", std::move(params),
      base::BindOnce(&FillTool::OnFocused,
                     weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

// Step 4: 포커스 완료 후 Ctrl+A (전체 선택) keyDown 발송
void FillTool::OnFocused(
    const std::string& value,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "DOM.focus", callback)) {
    return;
  }

  // Ctrl+A: 기존 텍스트 전체 선택
  // modifiers: 2 = Control (CDP 규격)
  base::Value::Dict params;
  params.Set("type", "keyDown");
  params.Set("key", "a");
  params.Set("code", "KeyA");
  params.Set("modifiers", 2);  // Control
  params.Set("windowsVirtualKeyCode", 65);  // 'A'

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&FillTool::OnSelectAllKeyDown,
                     weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

// Step 5: Ctrl+A keyUp 발송
void FillTool::OnSelectAllKeyDown(
    const std::string& value,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(Ctrl+A down)", callback)) {
    return;
  }

  base::Value::Dict params;
  params.Set("type", "keyUp");
  params.Set("key", "a");
  params.Set("code", "KeyA");
  params.Set("modifiers", 2);  // Control
  params.Set("windowsVirtualKeyCode", 65);

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&FillTool::OnSelectAllKeyUp,
                     weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

// Step 6: Delete 키로 선택 영역 삭제
void FillTool::OnSelectAllKeyUp(
    const std::string& value,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(Ctrl+A up)", callback)) {
    return;
  }

  // Delete 키 → 선택된 텍스트 삭제
  base::Value::Dict params;
  params.Set("type", "keyDown");
  params.Set("key", "Delete");
  params.Set("code", "Delete");
  params.Set("modifiers", 0);
  params.Set("windowsVirtualKeyCode", 46);  // VK_DELETE

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&FillTool::OnDeleteKey,
                     weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

// Step 7: Delete 완료 후 Input.insertText로 새 값 삽입
void FillTool::OnDeleteKey(
    const std::string& value,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(Delete)", callback)) {
    return;
  }

  // Input.insertText: 현재 포커스된 입력창에 텍스트를 한 번에 삽입.
  // dispatchKeyEvent를 문자마다 반복하는 방식보다 효율적이며,
  // 한글·이모지 등 멀티바이트 문자도 안전하게 처리된다.
  base::Value::Dict params;
  params.Set("text", value);

  session->SendCdpCommand(
      "Input.insertText", std::move(params),
      base::BindOnce(&FillTool::OnInsertText,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// Step 8: 삽입 완료 — 성공 결과 반환
void FillTool::OnInsertText(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.insertText", callback)) {
    return;
  }

  LOG(INFO) << "[FillTool] 텍스트 입력 완료";
  std::move(callback).Run(MakeSuccessResult("텍스트 입력이 성공적으로 완료되었습니다."));
}

// 정적 헬퍼: CDP 에러 처리
// NOLINTNEXTLINE(runtime/references)
bool FillTool::HandleCdpError(
    const base::Value& response,
    const std::string& step_name,
    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[FillTool] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

}  // namespace mcp
