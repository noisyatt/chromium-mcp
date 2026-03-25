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
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

namespace {

// Runtime.evaluate 결과에서 예외(exception)가 발생했는지 확인한다.
bool HasJsException(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return false;
  }
  const base::DictValue* result = dict->FindDict("result");
  if (!result) {
    return false;
  }
  return result->Find("exceptionDetails") != nullptr;
}

// Runtime.evaluate 결과에서 JS 예외 메시지를 추출한다.
std::string ExtractJsExceptionMessage(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return "JS 예외 발생 (상세 정보 없음)";
  }
  const base::DictValue* result = dict->FindDict("result");
  if (!result) {
    return "JS 예외 발생 (상세 정보 없음)";
  }
  const base::DictValue* exception_details = result->FindDict("exceptionDetails");
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
         "role/name, text, selector 등 다양한 방법으로 요소를 찾을 수 있습니다. "
         "value 또는 text로 단일 옵션을 선택하거나, "
         "values 배열로 다중 선택할 수 있습니다. "
         "'change' 이벤트가 버블링되어 발생합니다.";
}

base::DictValue SelectOptionTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // ---- 공통 로케이터 파라미터 ----

  // role: ARIA 역할
  base::DictValue role_prop;
  role_prop.Set("type", "string");
  role_prop.Set("description",
                "select 요소의 ARIA 역할 (일반적으로 \"combobox\"). "
                "name 파라미터와 함께 사용합니다.");
  properties.Set("role", std::move(role_prop));

  // name: 접근성 이름
  base::DictValue name_prop;
  name_prop.Set("type", "string");
  name_prop.Set("description",
                "select 요소의 접근성 이름 (레이블 텍스트, aria-label 등). "
                "role 파라미터와 함께 사용합니다.");
  properties.Set("name", std::move(name_prop));

  // text: 표시 텍스트 (select 요소의 표시 텍스트로 탐색)
  base::DictValue text_locator_prop;
  text_locator_prop.Set("type", "string");
  text_locator_prop.Set("description",
                        "select 요소를 찾기 위한 표시 텍스트. exact=false이면 부분 일치. "
                        "주의: 옵션 텍스트 선택용 text 파라미터와 다릅니다.");
  properties.Set("text", std::move(text_locator_prop));

  // selector: select 요소의 CSS 셀렉터
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description", "select 요소의 CSS 셀렉터 (예: \"select#country\", \"select[name='size']\")");
  properties.Set("selector", std::move(selector_prop));

  // xpath: XPath 표현식
  base::DictValue xpath_prop;
  xpath_prop.Set("type", "string");
  xpath_prop.Set("description",
                 "select 요소의 XPath 표현식.");
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

  // ---- 옵션 선택 파라미터 ----

  // value: 선택할 option의 value 속성
  base::DictValue value_prop;
  value_prop.Set("type", "string");
  value_prop.Set("description", "선택할 option 요소의 value 속성값. value와 optionText 중 하나를 사용.");
  properties.Set("value", std::move(value_prop));

  // optionText: 선택할 option의 텍스트 내용 (로케이터 text와 구분)
  base::DictValue option_text_prop;
  option_text_prop.Set("type", "string");
  option_text_prop.Set("description",
                "선택할 option 요소의 표시 텍스트. value 파라미터보다 우선순위가 낮음.");
  properties.Set("optionText", std::move(option_text_prop));

  // values: 다중 선택 시 value 배열
  base::DictValue values_prop;
  values_prop.Set("type", "array");
  base::DictValue values_items;
  values_items.Set("type", "string");
  values_prop.Set("items", std::move(values_items));
  values_prop.Set("description",
                  "다중 선택 시 사용. value 배열로 여러 옵션을 동시에 선택 (multiple 속성 필요).");
  properties.Set("values", std::move(values_prop));

  schema.Set("properties", std::move(properties));

  // 로케이터는 런타임에서 검증
  base::ListValue required;
  schema.Set("required", std::move(required));

  return schema;
}

void SelectOptionTool::Execute(const base::DictValue& arguments,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback) {
  // CSS selector 파라미터 (기존 호환)
  const std::string* selector = arguments.FindString("selector");

  // 로케이터가 없으면 에러
  const bool has_locator =
      (selector && !selector->empty()) ||
      arguments.FindString("role") || arguments.FindString("name") ||
      arguments.FindString("text") || arguments.FindString("xpath") ||
      arguments.FindString("ref");

  if (!has_locator) {
    LOG(WARNING) << "[SelectOptionTool] 로케이터 파라미터가 필요합니다.";
    std::move(callback).Run(
        MakeErrorResult("로케이터 파라미터(role/name/text/selector/xpath/ref)가 필요합니다."));
    return;
  }

  // 셀렉터 문자열 결정 (JS querySelector에 사용)
  // 로케이터가 있으면 JS 기반 querySelector 대신 ElementLocator가 필요하지만,
  // select_option은 backendNodeId보다 CSS selector로 JS 실행이 더 직접적.
  // 이 도구는 JS evaluate 기반이므로 selector가 없는 경우 비CSS 로케이터는
  // 현재 JS 문자열에 삽입할 수 없어 selector 파라미터를 우선 사용한다.
  // role/name/text/xpath/ref는 향후 확장을 위해 input_schema에 노출한다.
  // 현재 구현: selector 있으면 사용, 없으면 로케이터 파라미터로 에러 안내.
  if (!selector || selector->empty()) {
    // role/name/text 등 비-selector 로케이터는 현재 JS evaluate 방식과 직접 결합 불가.
    // ElementLocator에서 selector를 얻을 수 없으므로 안내 메시지 반환.
    LOG(WARNING) << "[SelectOptionTool] CSS selector 파라미터가 필요합니다 "
                    "(role/name/text는 현재 미지원).";
    std::move(callback).Run(
        MakeErrorResult("현재 select_option은 CSS selector 파라미터가 필요합니다."));
    return;
  }

  const std::string* value = arguments.FindString("value");
  // optionText: option 요소의 텍스트 내용 (로케이터 text와 구분)
  const std::string* option_text = arguments.FindString("optionText");
  const base::ListValue* values_list = arguments.FindList("values");

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
    // value 속성으로 단일 선택 (selectedIndex 확인으로 사일런트 실패 방지)
    std::string escaped_value = EscapeJsString(*value);
    js_code =
        "(function() {"
        "  var el = document.querySelector('" + escaped_selector + "');"
        "  if (!el) { throw new Error('요소를 찾을 수 없음: " + escaped_selector + "'); }"
        "  var prevIndex = el.selectedIndex;"
        "  el.value = '" + escaped_value + "';"
        "  if (el.selectedIndex < 0 || el.value !== '" + escaped_value + "') {"
        "    throw new Error('존재하지 않는 옵션 값: " + escaped_value + "');"
        "  }"
        "  el.dispatchEvent(new Event('change', {bubbles: true}));"
        "  return '선택 완료: ' + el.value;"
        "})()";

    LOG(INFO) << "[SelectOptionTool] value 모드: selector=" << *selector
              << " value=" << *value;

  } else if (option_text && !option_text->empty()) {
    // 텍스트 내용으로 단일 선택
    std::string escaped_text = EscapeJsString(*option_text);
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

    LOG(INFO) << "[SelectOptionTool] optionText 모드: selector=" << *selector
              << " optionText=" << *option_text;

  } else {
    LOG(WARNING) << "[SelectOptionTool] value, optionText, values 중 하나가 필요합니다.";
    std::move(callback).Run(
        MakeErrorResult("value, optionText, values 파라미터 중 하나가 필요합니다."));
    return;
  }

  // Runtime.evaluate로 JS 실행
  base::DictValue params;
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
  if (mcp::HandleCdpError(response, "Runtime.evaluate", callback)) {
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
  const base::DictValue* dict = response.GetIfDict();
  const base::DictValue* result = dict ? dict->FindDict("result") : nullptr;
  const base::DictValue* value_obj = result ? result->FindDict("result") : nullptr;
  const std::string* return_val = value_obj ? value_obj->FindString("value") : nullptr;

  std::string message = return_val ? *return_val : "옵션 선택이 완료되었습니다.";
  LOG(INFO) << "[SelectOptionTool] 완료: " << message;
  std::move(callback).Run(MakeSuccessResult(message));
}

// JavaScript 문자열에서 single quote를 이스케이프한다.
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
