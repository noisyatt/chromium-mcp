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

// MCP 에러 응답 Value를 생성한다.
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

// CDP 응답 Dict에 "error" 키가 있는지 확인한다.
bool HasCdpError(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return true;
  }
  return dict->Find("error") != nullptr;
}

// CDP 응답에서 에러 메시지 문자열을 추출한다.
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

// CDP 응답 result.nodeId 값을 추출한다. 없으면 -1 반환.
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

}  // namespace

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

base::DictValue FillTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description", "입력 필드의 CSS 셀렉터 (예: \"input[name='q']\")");
  properties.Set("selector", std::move(selector_prop));

  base::DictValue ref_prop;
  ref_prop.Set("type", "string");
  ref_prop.Set("description", "접근성 스냅샷에서 얻은 요소 ref");
  properties.Set("ref", std::move(ref_prop));

  base::DictValue value_prop;
  value_prop.Set("type", "string");
  value_prop.Set("description", "입력할 텍스트 값");
  properties.Set("value", std::move(value_prop));

  schema.Set("properties", std::move(properties));

  // value는 필수
  base::ListValue required;
  required.Append("value");
  schema.Set("required", std::move(required));

  return schema;
}

void FillTool::Execute(const base::DictValue& arguments,
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
  base::DictValue params;
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

  const base::DictValue* dict = response.GetIfDict();
  const base::DictValue* result = dict ? dict->FindDict("result") : nullptr;
  const base::DictValue* root = result ? result->FindDict("root") : nullptr;
  std::optional<int> root_node_id = root ? root->FindInt("nodeId") : std::nullopt;

  if (!root_node_id.has_value() || *root_node_id <= 0) {
    LOG(ERROR) << "[FillTool] DOM.getDocument 응답에서 rootNodeId를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("DOM 루트 노드 ID를 획득할 수 없습니다."));
    return;
  }

  base::DictValue params;
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

  base::DictValue params;
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
  base::DictValue params;
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

  base::DictValue params;
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
  base::DictValue params;
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
  base::DictValue params;
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
