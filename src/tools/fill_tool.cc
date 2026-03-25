// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/fill_tool.h"

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

// CDP 응답에 "error" 키가 있는지 확인
bool HasCdpError(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return true;
  }
  return dict->Find("error") != nullptr;
}

// CDP 응답에서 에러 메시지 추출
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
  return "role/name, text, selector, xpath, ref 등 다양한 방법으로 입력 필드를 찾아 "
         "텍스트를 입력합니다. ActionabilityChecker로 요소 상태(가시성, 활성화, "
         "편집 가능 여부)를 검증한 후 CDP Input 도메인으로 신뢰 이벤트(isTrusted)를 발생시킵니다. "
         "기존 내용은 Cmd+A → Delete로 자동 삭제됩니다.";
}

base::DictValue FillTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // ---- 공통 로케이터 파라미터 ----

  // role: ARIA 역할
  base::DictValue role_prop;
  role_prop.Set("type", "string");
  role_prop.Set("description",
                "입력할 요소의 ARIA 역할 (예: \"textbox\", \"searchbox\"). "
                "name 파라미터와 함께 사용하면 정확도가 높아집니다.");
  properties.Set("role", std::move(role_prop));

  // name: 접근성 이름
  base::DictValue name_prop;
  name_prop.Set("type", "string");
  name_prop.Set("description",
                "요소의 접근성 이름 (레이블 텍스트, aria-label 등). "
                "role 파라미터와 함께 사용합니다.");
  properties.Set("name", std::move(name_prop));

  // text: 표시 텍스트
  base::DictValue text_prop;
  text_prop.Set("type", "string");
  text_prop.Set("description",
                "요소의 표시 텍스트로 탐색. exact=false 이면 부분 일치 허용.");
  properties.Set("text", std::move(text_prop));

  // selector: CSS 셀렉터
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description",
                    "입력 필드의 CSS 셀렉터 (예: \"input[name='q']\", \"#email\").");
  properties.Set("selector", std::move(selector_prop));

  // xpath: XPath 표현식
  base::DictValue xpath_prop;
  xpath_prop.Set("type", "string");
  xpath_prop.Set("description",
                 "입력 필드의 XPath 표현식 (예: \"//input[@placeholder='검색']\").");
  properties.Set("xpath", std::move(xpath_prop));

  // ref: backendNodeId 참조
  base::DictValue ref_prop;
  ref_prop.Set("type", "string");
  ref_prop.Set("description",
               "접근성 스냅샷 또는 element 도구에서 얻은 요소 ref (backendNodeId).");
  properties.Set("ref", std::move(ref_prop));

  // exact: 텍스트/이름 정확히 일치 여부
  base::DictValue exact_prop;
  exact_prop.Set("type", "boolean");
  exact_prop.Set("default", true);
  exact_prop.Set("description",
                 "true이면 name/text 파라미터를 정확히 일치, "
                 "false이면 부분 문자열 일치로 탐색 (기본: true).");
  properties.Set("exact", std::move(exact_prop));

  // ---- auto-wait 파라미터 ----

  // timeout: 최대 대기 시간 (ms)
  base::DictValue timeout_prop;
  timeout_prop.Set("type", "number");
  timeout_prop.Set("default", 5000);
  timeout_prop.Set("description",
                   "요소가 actionable 상태가 될 때까지 최대 대기 시간 (ms, 기본: 5000).");
  properties.Set("timeout", std::move(timeout_prop));

  // force: actionability 체크 건너뜀
  base::DictValue force_prop;
  force_prop.Set("type", "boolean");
  force_prop.Set("default", false);
  force_prop.Set("description",
                 "true이면 actionability 체크를 건너뛰고 강제로 입력 (기본: false).");
  properties.Set("force", std::move(force_prop));

  // ---- fill 전용 파라미터 ----

  // value: 입력할 텍스트 (필수)
  base::DictValue value_prop;
  value_prop.Set("type", "string");
  value_prop.Set("description",
                 "입력 필드에 채울 텍스트 값. 기존 내용은 자동으로 삭제됩니다.");
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
  const std::string* value = arguments.FindString("value");
  if (!value) {
    LOG(WARNING) << "[FillTool] value 파라미터 누락";
    std::move(callback).Run(MakeErrorResult("value 파라미터가 필요합니다."));
    return;
  }

  // timeout / force 파라미터
  ActionabilityChecker::Options options;
  std::optional<double> timeout_opt = arguments.FindDouble("timeout");
  if (timeout_opt.has_value()) {
    options.timeout_ms = static_cast<int>(*timeout_opt);
  }
  options.force = arguments.FindBool("force").value_or(false);

  LOG(INFO) << "[FillTool] 실행: value=" << *value
            << " timeout=" << options.timeout_ms
            << " force=" << options.force;

  actionability_checker_.VerifyAndLocate(
      session, arguments, ActionabilityChecker::ActionType::kFill, options,
      base::BindOnce(&FillTool::OnActionable, weak_factory_.GetWeakPtr(),
                     *value, session, std::move(callback)));
}

void FillTool::OnActionable(const std::string& value,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback,
                            ElementLocator::Result result,
                            std::string error) {
  if (!error.empty()) {
    LOG(WARNING) << "[FillTool] ActionabilityChecker 실패: " << error;
    std::move(callback).Run(MakeErrorResult(error));
    return;
  }

  LOG(INFO) << "[FillTool] 요소 확인 완료: backendNodeId="
            << result.backend_node_id
            << " role=" << result.role
            << " name=" << result.name;

  // DOM.focus: backendNodeId로 요소에 포커스
  base::DictValue params;
  params.Set("backendNodeId", result.backend_node_id);

  session->SendCdpCommand(
      "DOM.focus", std::move(params),
      base::BindOnce(&FillTool::OnFocused, weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

void FillTool::OnFocused(const std::string& value,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback,
                         base::Value response) {
  if (HandleCdpError(response, "DOM.focus", callback)) {
    return;
  }

  // macOS Cmd+A: 전체 선택
  // modifier=4 (Meta/Cmd), commands=["selectAll"] 포함
  base::DictValue params;
  params.Set("type", "keyDown");
  params.Set("key", "a");
  params.Set("code", "KeyA");
  params.Set("modifiers", 4);  // Meta (macOS Cmd)
  params.Set("windowsVirtualKeyCode", 65);  // 'A'
  base::ListValue commands;
  commands.Append("selectAll");
  params.Set("commands", std::move(commands));

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&FillTool::OnSelectAllKeyDown, weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

void FillTool::OnSelectAllKeyDown(const std::string& value,
                                  McpSession* session,
                                  base::OnceCallback<void(base::Value)> callback,
                                  base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(Cmd+A down)", callback)) {
    return;
  }

  // Cmd+A keyUp
  base::DictValue params;
  params.Set("type", "keyUp");
  params.Set("key", "a");
  params.Set("code", "KeyA");
  params.Set("modifiers", 4);  // Meta (macOS Cmd)
  params.Set("windowsVirtualKeyCode", 65);

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&FillTool::OnSelectAllKeyUp, weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

void FillTool::OnSelectAllKeyUp(const std::string& value,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback,
                                base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(Cmd+A up)", callback)) {
    return;
  }

  // Delete keyDown: 선택된 텍스트 삭제
  base::DictValue params;
  params.Set("type", "keyDown");
  params.Set("key", "Delete");
  params.Set("code", "Delete");
  params.Set("modifiers", 0);
  params.Set("windowsVirtualKeyCode", 46);  // VK_DELETE

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&FillTool::OnDeleteKeyDown, weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

void FillTool::OnDeleteKeyDown(const std::string& value,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback,
                               base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(Delete down)", callback)) {
    return;
  }

  // Delete keyUp — 기존 dom_tool.cc에서 누락된 부분 수정
  base::DictValue params;
  params.Set("type", "keyUp");
  params.Set("key", "Delete");
  params.Set("code", "Delete");
  params.Set("modifiers", 0);
  params.Set("windowsVirtualKeyCode", 46);  // VK_DELETE

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&FillTool::OnDeleteKeyUp, weak_factory_.GetWeakPtr(),
                     value, session, std::move(callback)));
}

void FillTool::OnDeleteKeyUp(const std::string& value,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback,
                             base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(Delete up)", callback)) {
    return;
  }

  // Input.insertText: 한글·이모지 등 멀티바이트 문자도 안전하게 삽입
  base::DictValue params;
  params.Set("text", value);

  session->SendCdpCommand(
      "Input.insertText", std::move(params),
      base::BindOnce(&FillTool::OnInsertText, weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

void FillTool::OnInsertText(base::OnceCallback<void(base::Value)> callback,
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
