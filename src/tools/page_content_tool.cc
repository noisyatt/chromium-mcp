// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/page_content_tool.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

PageContentTool::PageContentTool() = default;
PageContentTool::~PageContentTool() = default;

std::string PageContentTool::name() const {
  return "page_content";
}

std::string PageContentTool::description() const {
  return "페이지의 접근성 트리 또는 HTML 반환";
}

base::Value::Dict PageContentTool::input_schema() const {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // mode: 콘텐츠 취득 방식 선택 (기본값 "accessibility")
  base::Value::Dict mode_prop;
  mode_prop.Set("type", "string");
  base::Value::List mode_enum;
  mode_enum.Append("accessibility");
  mode_enum.Append("html");
  mode_enum.Append("text");
  mode_prop.Set("enum", std::move(mode_enum));
  mode_prop.Set(
      "description",
      "콘텐츠 형식: accessibility(AX 트리), html(외부 HTML), text(순수 텍스트)");
  mode_prop.Set("default", "accessibility");
  properties.Set("mode", std::move(mode_prop));

  // selector: 특정 요소 범위로 제한 (html/text 모드에서 사용)
  base::Value::Dict selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description",
                    "콘텐츠를 가져올 요소의 CSS 선택자 (html/text 모드 전용, "
                    "생략 시 전체 문서)");
  properties.Set("selector", std::move(selector_prop));

  schema.Set("properties", std::move(properties));
  return schema;
}

void PageContentTool::Execute(const base::Value::Dict& arguments,
                              McpSession* session,
                              base::OnceCallback<void(base::Value)> callback) {
  // mode 파라미터 추출 (기본값 "accessibility")
  const std::string* mode_ptr = arguments.FindString("mode");
  std::string mode = mode_ptr ? *mode_ptr : "accessibility";

  // selector 파라미터 추출 (없으면 빈 문자열)
  const std::string* selector_ptr = arguments.FindString("selector");
  std::string selector = selector_ptr ? *selector_ptr : "";

  LOG(INFO) << "[PageContentTool] Execute, mode=" << mode
            << " selector=" << selector;

  if (mode == "accessibility") {
    // 접근성 트리: selector 무시 (전체 트리 반환)
    if (!selector.empty()) {
      LOG(WARNING) << "[PageContentTool] accessibility 모드는 selector 무시";
    }
    FetchAccessibilityTree(session, std::move(callback));

  } else if (mode == "html") {
    FetchHtmlContent(selector, session, std::move(callback));

  } else if (mode == "text") {
    FetchTextContent(selector, session, std::move(callback));

  } else {
    LOG(WARNING) << "[PageContentTool] 알 수 없는 mode: " << mode;
    base::Value::Dict err;
    err.Set("error", "지원하지 않는 mode: " + mode);
    std::move(callback).Run(base::Value(std::move(err)));
  }
}

// ──────────────────────────────────────────────────────────────
// accessibility 모드
// ──────────────────────────────────────────────────────────────

void PageContentTool::FetchAccessibilityTree(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // Accessibility.getFullAXTree: 페이지 전체의 접근성 트리를 반환한다.
  // 파라미터 없이 호출하면 현재 활성 프레임의 전체 트리를 가져온다.
  base::Value::Dict params;
  // depth를 지정하지 않으면 전체 트리 반환 (depth=-1 또는 생략)

  LOG(INFO) << "[PageContentTool] Accessibility.getFullAXTree 호출";
  session->SendCdpCommand(
      "Accessibility.getFullAXTree", std::move(params),
      base::BindOnce(&PageContentTool::OnAccessibilityTreeResponse,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void PageContentTool::OnAccessibilityTreeResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    LOG(ERROR) << "[PageContentTool] Accessibility.getFullAXTree 응답 오류";
    base::Value::Dict err;
    err.Set("error", "Accessibility.getFullAXTree 실패");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::Value::Dict& dict = response.GetDict();

  // CDP 오류 응답 확인
  const base::Value::Dict* error_dict = dict.FindDict("error");
  if (error_dict) {
    const std::string* msg = error_dict->FindString("message");
    std::string error_msg = msg ? *msg : "알 수 없는 CDP 오류";
    LOG(ERROR) << "[PageContentTool] AX tree CDP 오류: " << error_msg;
    base::Value::Dict err;
    err.Set("error", error_msg);
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  // 응답에서 "nodes" 배열 추출
  const base::Value::List* nodes = dict.FindList("nodes");
  if (!nodes) {
    LOG(WARNING) << "[PageContentTool] AX 트리 nodes 없음";
    base::Value::Dict result;
    result.Set("mode", "accessibility");
    result.Set("nodes", base::Value::List());
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[PageContentTool] AX 트리 노드 수: " << nodes->size();
  base::Value::Dict result;
  result.Set("mode", "accessibility");
  result.Set("nodes", nodes->Clone());
  std::move(callback).Run(base::Value(std::move(result)));
}

// ──────────────────────────────────────────────────────────────
// html 모드
// ──────────────────────────────────────────────────────────────

void PageContentTool::FetchHtmlContent(
    const std::string& selector,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // DOM.getDocument로 루트 nodeId를 가져온 뒤 DOM.getOuterHTML 호출
  base::Value::Dict params;
  params.Set("depth", 0);   // 루트 노드 정보만 필요
  params.Set("pierce", false);

  session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&PageContentTool::OnGetDocumentResponse,
                     weak_factory_.GetWeakPtr(), selector, session,
                     std::move(callback)));
}

void PageContentTool::OnGetDocumentResponse(
    const std::string& selector,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    LOG(ERROR) << "[PageContentTool] DOM.getDocument 응답 오류";
    base::Value::Dict err;
    err.Set("error", "DOM.getDocument 실패");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::Value::Dict* root = response.GetDict().FindDict("root");
  if (!root) {
    LOG(ERROR) << "[PageContentTool] DOM root 없음";
    base::Value::Dict err;
    err.Set("error", "DOM 루트 노드 없음");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  std::optional<int> root_node_id = root->FindInt("nodeId");
  if (!root_node_id) {
    base::Value::Dict err;
    err.Set("error", "루트 nodeId 추출 실패");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  if (selector.empty()) {
    // selector 없음: 루트 노드 전체 HTML 반환
    base::Value::Dict outer_html_params;
    outer_html_params.Set("nodeId", *root_node_id);

    session->SendCdpCommand(
        "DOM.getOuterHTML", std::move(outer_html_params),
        base::BindOnce(&PageContentTool::OnGetOuterHtmlResponse,
                       weak_factory_.GetWeakPtr(), std::move(callback)));
  } else {
    // selector 지정: 먼저 DOM.querySelector로 특정 노드 ID 탐색
    base::Value::Dict qs_params;
    qs_params.Set("nodeId", *root_node_id);
    qs_params.Set("selector", selector);

    session->SendCdpCommand(
        "DOM.querySelector", std::move(qs_params),
        base::BindOnce(
            [](McpSession* sess,
               base::OnceCallback<void(base::Value)> cb,
               base::Value qs_response) {
              if (!qs_response.is_dict()) {
                base::Value::Dict err;
                err.Set("error", "DOM.querySelector 실패");
                std::move(cb).Run(base::Value(std::move(err)));
                return;
              }

              std::optional<int> node_id =
                  qs_response.GetDict().FindInt("nodeId");
              if (!node_id || *node_id == 0) {
                LOG(WARNING) << "[PageContentTool] selector 요소 없음";
                base::Value::Dict err;
                err.Set("error", "해당 selector의 요소를 찾을 수 없음");
                std::move(cb).Run(base::Value(std::move(err)));
                return;
              }

              // 찾은 노드의 outerHTML 취득
              base::Value::Dict outer_params;
              outer_params.Set("nodeId", *node_id);
              sess->SendCdpCommand(
                  "DOM.getOuterHTML", std::move(outer_params),
                  base::BindOnce(
                      [](base::OnceCallback<void(base::Value)> done,
                         base::Value outer_response) {
                        if (!outer_response.is_dict()) {
                          base::Value::Dict err;
                          err.Set("error", "DOM.getOuterHTML 실패");
                          std::move(done).Run(base::Value(std::move(err)));
                          return;
                        }
                        const std::string* html =
                            outer_response.GetDict().FindString("outerHTML");
                        base::Value::Dict result;
                        result.Set("mode", "html");
                        result.Set("html", html ? *html : "");
                        std::move(done).Run(base::Value(std::move(result)));
                      },
                      std::move(cb)));
            },
            session, std::move(callback)));
  }
}

void PageContentTool::OnGetOuterHtmlResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    LOG(ERROR) << "[PageContentTool] DOM.getOuterHTML 응답 오류";
    base::Value::Dict err;
    err.Set("error", "DOM.getOuterHTML 실패");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::Value::Dict& dict = response.GetDict();

  // CDP 오류 확인
  const base::Value::Dict* error_dict = dict.FindDict("error");
  if (error_dict) {
    const std::string* msg = error_dict->FindString("message");
    std::string error_msg = msg ? *msg : "알 수 없는 CDP 오류";
    LOG(ERROR) << "[PageContentTool] getOuterHTML CDP 오류: " << error_msg;
    base::Value::Dict err;
    err.Set("error", error_msg);
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const std::string* html = dict.FindString("outerHTML");
  LOG(INFO) << "[PageContentTool] HTML 취득 성공, 길이="
            << (html ? html->size() : 0);

  base::Value::Dict result;
  result.Set("mode", "html");
  result.Set("html", html ? *html : "");
  std::move(callback).Run(base::Value(std::move(result)));
}

// ──────────────────────────────────────────────────────────────
// text 모드
// ──────────────────────────────────────────────────────────────

void PageContentTool::FetchTextContent(
    const std::string& selector,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // Runtime.evaluate를 사용해 JS로 텍스트를 직접 추출한다.
  // Runtime.enable을 호출하지 않아 DevTools 이벤트 스트림이 활성화되지 않으므로
  // 브라우저 자동화 탐지를 피할 수 있다.

  std::string expression;
  if (selector.empty()) {
    // 전체 페이지 텍스트: document.body.innerText
    expression = "document.body.innerText";
  } else {
    // 특정 요소 텍스트: querySelector 결과의 innerText
    // JS 인젝션 방지를 위해 selector의 작은따옴표를 이스케이프
    std::string escaped_selector = selector;
    base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "'", "\\'");
    expression =
        "(() => { const el = document.querySelector('" + escaped_selector +
        "'); return el ? el.innerText : null; })()";
  }

  LOG(INFO) << "[PageContentTool] Runtime.evaluate 호출 (Runtime.enable 미사용)";

  base::Value::Dict params;
  params.Set("expression", expression);
  // returnByValue=true: 결과를 직렬화된 값으로 반환
  params.Set("returnByValue", true);
  // awaitPromise=false: 동기 식은 Promise를 기다리지 않음
  params.Set("awaitPromise", false);

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&PageContentTool::OnEvaluateResponse,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void PageContentTool::OnEvaluateResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    LOG(ERROR) << "[PageContentTool] Runtime.evaluate 응답 오류";
    base::Value::Dict err;
    err.Set("error", "Runtime.evaluate 실패");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::Value::Dict& dict = response.GetDict();

  // CDP 레벨 오류 확인
  const base::Value::Dict* error_dict = dict.FindDict("error");
  if (error_dict) {
    const std::string* msg = error_dict->FindString("message");
    std::string error_msg = msg ? *msg : "알 수 없는 CDP 오류";
    LOG(ERROR) << "[PageContentTool] Runtime.evaluate CDP 오류: " << error_msg;
    base::Value::Dict err;
    err.Set("error", error_msg);
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  // JS 런타임 예외 확인: exceptionDetails 필드가 있으면 JS 오류 발생
  const base::Value::Dict* exception = dict.FindDict("exceptionDetails");
  if (exception) {
    const std::string* ex_text = exception->FindString("text");
    std::string ex_msg = ex_text ? *ex_text : "JS 실행 오류";
    LOG(WARNING) << "[PageContentTool] JS 예외: " << ex_msg;
    base::Value::Dict err;
    err.Set("error", "JS 실행 오류: " + ex_msg);
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  // 정상 결과: result.value에 문자열이 담겨 있음
  const base::Value::Dict* result_obj = dict.FindDict("result");
  if (!result_obj) {
    LOG(WARNING) << "[PageContentTool] Runtime.evaluate 결과 없음";
    base::Value::Dict result;
    result.Set("mode", "text");
    result.Set("text", "");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // result.type == "string"인 경우 result.value에서 텍스트 추출
  const std::string* type = result_obj->FindString("type");
  if (type && *type == "null") {
    // selector에 해당하는 요소가 없어 null 반환된 경우
    LOG(WARNING) << "[PageContentTool] selector 요소 없음, null 반환됨";
    base::Value::Dict result;
    result.Set("mode", "text");
    result.Set("text", "");
    result.Set("warning", "해당 selector의 요소를 찾을 수 없음");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const std::string* text_value = result_obj->FindString("value");
  std::string text = text_value ? *text_value : "";

  LOG(INFO) << "[PageContentTool] 텍스트 취득 성공, 길이=" << text.size();

  base::Value::Dict result;
  result.Set("mode", "text");
  result.Set("text", text);
  std::move(callback).Run(base::Value(std::move(result)));
}

}  // namespace mcp
