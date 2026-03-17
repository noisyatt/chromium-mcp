// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/select_option_tool.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
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

// Runtime.evaluate 결과에서 예외(exception)가 발생했는지 확인한다.
bool HasJsException(const base::Value& response) {
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict) {
    return false;
  }
  const base::Value::Dict* result = dict->FindDict("result");
  if (!result) {
    return false;
  }
  // exceptionDetails 키가 있으면 JS 예외 발생
  return result->Find("exceptionDetails") != nullptr;
}

// Runtime.evaluate 결과에서 JS 예외 메시지를 추출한다.
std::string ExtractJsExceptionMessage(const base::Value& response) {
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict) {
    return "JS 예외 발생 (상세 정보 없음)";
  }
  const base::Value::Dict* result = dict->FindDict("result");
  if (!result) {
    return "JS 예외 발생 (상세 정보 없음)";
  }
  const base::Value::Dict* exception_details = result->FindDict("exceptionDetails");
  if (!exception_details) {
    return "JS 예외 발생 (상세 정보 없음)";
  }
  const std::string* text = exception_details->FindString("text");
  if (text) {
    return *text;
  }
  return "JS 예외 발생 (메시지 없음)";
}

}  // namespace

// ============================================================
// SelectOptionTool 구현
// ============================================================

SelectOptionTool::SelectOptionTool() = default;
SelectOptionTool::~SelectOptionTool() = default;

std::string SelectOptionTool::name() const {
  return "select_option";
}

std::string SelectOptionTool::description() const {
  return "드롭다운(<select>) 요소에서 옵션을 선택합니다. "
         "value 또는 text로 단일 옵션을 선택하거나, "
         "values 배열로 다중 선택할 수 있습니다. "
         "'change' 이벤트가 버블링되어 발생합니다.";
}

base::Value::Dict SelectOptionTool::input_schema() const {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // selector: select 요소의 CSS 셀렉터
  base::Value::Dict selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description", "select 요소의 CSS 셀렉터 (예: \"select#country\", \"select[name='size']\")");
  properties.Set("selector", std::move(selector_prop));

  // value: 선택할 option의 value 속성
  base::Value::Dict value_prop;
  value_prop.Set("type", "string");
  value_prop.Set("description", "선택할 option 요소의 value 속성값. value와 text 중 하나를 사용.");
  properties.Set("value", std::move(value_prop));

  // text: 선택할 option의 텍스트 내용
  base::Value::Dict text_prop;
  text_prop.Set("type", "string");
  text_prop.Set("description",
                "선택할 option 요소의 표시 텍스트. value 파라미터보다 우선순위가 낮음.");
  properties.Set("text", std::move(text_prop));

  // values: 다중 선택 시 value 배열
  base::Value::Dict values_prop;
  values_prop.Set("type", "array");
  base::Value::Dict values_items;
  values_items.Set("type", "string");
  values_prop.Set("items", std::move(values_items));
  values_prop.Set("description",
                  "다중 선택 시 사용. value 배열로 여러 옵션을 동시에 선택 (multiple 속성 필요).");
  properties.Set("values", std::move(values_prop));

  schema.Set("properties", std::move(properties));

  // selector는 필수
  base::Value::List required;
  required.Append("selector");
  schema.Set("required", std::move(required));

  return schema;
}

void SelectOptionTool::Execute(const base::Value::Dict& arguments,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback) {
  const std::string* selector = arguments.FindString("selector");
  if (!selector || selector->empty()) {
    LOG(WARNING) << "[SelectOptionTool] selector 파라미터가 필요합니다.";
    std::move(callback).Run(MakeErrorResult("selector 파라미터가 필요합니다."));
    return;
  }

  const std::string* value = arguments.FindString("value");
  const std::string* text = arguments.FindString("text");
  const base::Value::List* values_list = arguments.FindList("values");

  // 실행할 JavaScript 코드 조합
  std::string js_code;
  std::string escaped_selector = EscapeJsString(*selector);

  if (values_list && !values_list->empty()) {
    // 다중 선택 모드: values 배열을 순회하며 option.selected 설정
    std::string values_array_str = "[";
    bool first = true;
    for (const base::Value& v : *values_list) {
      const std::string* val_str = v.GetIfString();
      if (!val_str) {
        continue;
      }
      if (!first) {
        values_array_str += ", ";
      }
      values_array_str += "'" + EscapeJsString(*val_str) + "'";
      first = false;
    }
    values_array_str += "]";

    js_code =
        "(function() {"
        "  var el = document.querySelector('" + escaped_selector + "');"
        "  if (!el) { throw new Error('요소를 찾을 수 없음: " + escaped_selector + "'); }"
        "  var targetValues = " + values_array_str + ";"
        "  for (var i = 0; i < el.options.length; i++) {"
        "    el.options[i].selected = targetValues.indexOf(el.options[i].value) !== -1;"
        "  }"
        "  el.dispatchEvent(new Event('change', {bubbles: true}));"
        "  return '다중 선택 완료: ' + targetValues.join(', ');"
        "})()";

    LOG(INFO) << "[SelectOptionTool] 다중 선택 모드: selector=" << *selector;

  } else if (value && !value->empty()) {
    // value 속성으로 단일 선택
    std::string escaped_value = EscapeJsString(*value);
    js_code =
        "(function() {"
        "  var el = document.querySelector('" + escaped_selector + "');"
        "  if (!el) { throw new Error('요소를 찾을 수 없음: " + escaped_selector + "'); }"
        "  el.value = '" + escaped_value + "';"
        "  el.dispatchEvent(new Event('change', {bubbles: true}));"
        "  return '선택 완료: ' + el.value;"
        "})()";

    LOG(INFO) << "[SelectOptionTool] value 모드: selector=" << *selector
              << " value=" << *value;

  } else if (text && !text->empty()) {
    // 텍스트 내용으로 단일 선택
    std::string escaped_text = EscapeJsString(*text);
    js_code =
        "(function() {"
        "  var el = document.querySelector('" + escaped_selector + "');"
        "  if (!el) { throw new Error('요소를 찾을 수 없음: " + escaped_selector + "'); }"
        "  var found = false;"
        "  for (var i = 0; i < el.options.length; i++) {"
        "    if (el.options[i].text.trim() === '" + escaped_text + "') {"
        "      el.selectedIndex = i;"
        "      found = true;"
        "      break;"
        "    }"
        "  }"
        "  if (!found) { throw new Error('텍스트에 일치하는 옵션 없음: " + escaped_text + "'); }"
        "  el.dispatchEvent(new Event('change', {bubbles: true}));"
        "  return '선택 완료 (텍스트): ' + el.value;"
        "})()";

    LOG(INFO) << "[SelectOptionTool] text 모드: selector=" << *selector
              << " text=" << *text;

  } else {
    LOG(WARNING) << "[SelectOptionTool] value, text, values 중 하나가 필요합니다.";
    std::move(callback).Run(
        MakeErrorResult("value, text, values 파라미터 중 하나가 필요합니다."));
    return;
  }

  // Runtime.evaluate로 JS 실행
  base::Value::Dict params;
  params.Set("expression", js_code);
  params.Set("returnByValue", true);

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&SelectOptionTool::OnEvaluateComplete,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// Runtime.evaluate 완료 콜백
void SelectOptionTool::OnEvaluateComplete(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Runtime.evaluate", callback)) {
    return;
  }

  // JS 예외 확인
  if (HasJsException(response)) {
    std::string js_err = ExtractJsExceptionMessage(response);
    LOG(ERROR) << "[SelectOptionTool] JavaScript 예외: " << js_err;
    std::move(callback).Run(MakeErrorResult("옵션 선택 중 오류: " + js_err));
    return;
  }

  // 결과에서 반환 문자열 추출
  const base::Value::Dict* dict = response.GetIfDict();
  const base::Value::Dict* result = dict ? dict->FindDict("result") : nullptr;
  const base::Value::Dict* value_obj = result ? result->FindDict("result") : nullptr;
  const std::string* return_val = value_obj ? value_obj->FindString("value") : nullptr;

  std::string message = return_val ? *return_val : "옵션 선택이 완료되었습니다.";
  LOG(INFO) << "[SelectOptionTool] 완료: " << message;
  std::move(callback).Run(MakeSuccessResult(message));
}

// 정적 헬퍼: CDP 에러 처리
// NOLINTNEXTLINE(runtime/references)
bool SelectOptionTool::HandleCdpError(
    const base::Value& response,
    const std::string& step_name,
    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[SelectOptionTool] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

// JavaScript 문자열에서 single quote를 이스케이프한다.
// SQL 인젝션 방지와 유사하게 JS 코드 삽입 시 안전하게 처리.
std::string SelectOptionTool::EscapeJsString(const std::string& str) {
  std::string result;
  result.reserve(str.size() + 4);
  for (char c : str) {
    if (c == '\'') {
      result += "\\'";
    } else if (c == '\\') {
      result += "\\\\";
    } else if (c == '\n') {
      result += "\\n";
    } else if (c == '\r') {
      result += "\\r";
    } else {
      result += c;
    }
  }
  return result;
}

}  // namespace mcp
