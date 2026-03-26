// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/element_info_tool.h"

#include <memory>
#include <optional>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

// 지원하는 property 목록
constexpr char kPropAttributes[]    = "attributes";
constexpr char kPropComputedStyle[] = "computedStyle";
constexpr char kPropBoundingBox[]   = "boundingBox";
constexpr char kPropText[]          = "text";
constexpr char kPropHtml[]          = "html";
constexpr char kPropValue[]         = "value";
constexpr char kPropChecked[]       = "checked";
constexpr char kPropVisible[]       = "visible";

// 모든 property 목록 (미지정 시 전체 조회)
constexpr const char* kAllProperties[] = {
    kPropAttributes,
    kPropComputedStyle,
    kPropBoundingBox,
    kPropText,
    kPropHtml,
    kPropValue,
    kPropChecked,
    kPropVisible,
};

ElementInfoTool::QueryContext::QueryContext() = default;
ElementInfoTool::QueryContext::~QueryContext() = default;

ElementInfoTool::ElementInfoTool() = default;
ElementInfoTool::~ElementInfoTool() = default;

std::string ElementInfoTool::name() const {
  return "element_info";
}

std::string ElementInfoTool::description() const {
  return "요소의 상세 정보 조회 (속성, 스타일, 위치, 텍스트). "
         "role/name, text, selector, xpath, ref 등 다양한 방법으로 요소를 지정합니다.";
}

base::DictValue ElementInfoTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // ---- 공통 로케이터 파라미터 ----

  // role: ARIA 역할
  base::DictValue role_prop;
  role_prop.Set("type", "string");
  role_prop.Set("description",
                "조회할 요소의 ARIA 역할. name 파라미터와 함께 사용합니다.");
  properties.Set("role", std::move(role_prop));

  // name: 접근성 이름
  base::DictValue name_prop;
  name_prop.Set("type", "string");
  name_prop.Set("description",
                "요소의 접근성 이름. role 파라미터와 함께 사용합니다.");
  properties.Set("name", std::move(name_prop));

  // text: 표시 텍스트
  base::DictValue text_prop;
  text_prop.Set("type", "string");
  text_prop.Set("description",
                "요소의 표시 텍스트로 탐색. exact=false이면 부분 일치 허용.");
  properties.Set("text", std::move(text_prop));

  // selector: CSS 선택자
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description", "조회할 요소의 CSS 선택자 (필수 아님, 다른 로케이터 사용 가능)");
  properties.Set("selector", std::move(selector_prop));

  // xpath: XPath 표현식
  base::DictValue xpath_prop;
  xpath_prop.Set("type", "string");
  xpath_prop.Set("description",
                 "조회할 요소의 XPath 표현식.");
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
  exact_prop.Set("default", false);
  exact_prop.Set("description",
                 "true이면 name/text 파라미터를 정확히 일치, "
                 "false이면 부분 문자열 일치로 탐색 (기본: false).");
  properties.Set("exact", std::move(exact_prop));

  // ---- 조회 항목 선택 ----

  // properties: 조회할 항목 목록 (선택적, 미지정 시 전체)
  base::DictValue props_prop;
  props_prop.Set("type", "array");
  base::DictValue items_schema;
  items_schema.Set("type", "string");
  base::ListValue items_enum;
  items_enum.Append(kPropAttributes);
  items_enum.Append(kPropComputedStyle);
  items_enum.Append(kPropBoundingBox);
  items_enum.Append(kPropText);
  items_enum.Append(kPropHtml);
  items_enum.Append(kPropValue);
  items_enum.Append(kPropChecked);
  items_enum.Append(kPropVisible);
  items_schema.Set("enum", std::move(items_enum));
  props_prop.Set("items", std::move(items_schema));
  props_prop.Set("description",
                 "조회할 항목 목록. 생략 시 모든 항목 반환. "
                 "가능한 값: attributes, computedStyle, boundingBox, "
                 "text, html, value, checked, visible");
  properties.Set("properties", std::move(props_prop));

  schema.Set("properties", std::move(properties));

  // 로케이터는 런타임에서 검증
  base::ListValue required;
  schema.Set("required", std::move(required));

  return schema;
}

void ElementInfoTool::Execute(const base::DictValue& arguments,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback) {
  // 로케이터가 있는지 확인
  const bool has_locator =
      arguments.FindString("role") || arguments.FindString("name") ||
      arguments.FindString("text") || arguments.FindString("selector") ||
      arguments.FindString("xpath") || arguments.FindString("ref");

  if (!has_locator) {
    std::move(callback).Run(
        MakeErrorResult("로케이터 파라미터(role/name/text/selector/xpath/ref)가 필요합니다."));
    return;
  }

  // properties 파라미터 추출 (선택적)
  auto ctx = std::make_shared<QueryContext>();
  const std::string* sel = arguments.FindString("selector");
  ctx->selector = sel ? *sel : "(locator)";
  ctx->session = session;
  ctx->callback = std::move(callback);

  const base::ListValue* props_list = arguments.FindList("properties");
  if (props_list && !props_list->empty()) {
    for (const auto& prop : *props_list) {
      if (prop.is_string()) {
        ctx->properties.insert(prop.GetString());
      }
    }
  } else {
    for (const char* prop : kAllProperties) {
      ctx->properties.insert(prop);
    }
  }

  LOG(INFO) << "[ElementInfoTool] 요소 정보 조회: selector=" << ctx->selector
            << " properties 수=" << ctx->properties.size();

  // ElementLocator로 요소 탐색
  locator_.Locate(
      session, arguments,
      base::BindOnce(&ElementInfoTool::OnLocated, weak_factory_.GetWeakPtr(),
                     ctx));
}

void ElementInfoTool::OnLocated(std::shared_ptr<QueryContext> ctx,
                                 std::optional<ElementLocator::Result> result,
                                 std::string error) {
  if (!error.empty()) {
    std::move(ctx->callback).Run(MakeErrorResult(error));
    return;
  }

  ctx->node_id = result->node_id;
  if (ctx->node_id == 0) {
    ctx->node_id = result->backend_node_id;
  }

  LOG(INFO) << "[ElementInfoTool] nodeId 획득: " << ctx->node_id;

  // property 조회 시작
  FetchProperties(ctx);
}

void ElementInfoTool::FetchProperties(std::shared_ptr<QueryContext> ctx) {
  auto pending = std::make_shared<int>(static_cast<int>(ctx->properties.size()));
  auto finalized = std::make_shared<bool>(false);

  // 완료 체크 람다: pending이 0이 되면 최종 결과 반환
  auto check_done = [ctx, pending, finalized]() {
    (*pending)--;
    if (*pending <= 0 && !*finalized) {
      *finalized = true;
      ctx->result.Set("success", true);
      ctx->result.Set("selector", ctx->selector);
      std::move(ctx->callback).Run(base::Value(std::move(ctx->result)));
    }
  };

  if (ctx->properties.count(kPropAttributes)) {
    base::DictValue params;
    params.Set("nodeId", ctx->node_id);
    ctx->session->SendCdpCommand(
        "DOM.getAttributes", std::move(params),
        base::BindOnce(
            [](base::WeakPtr<ElementInfoTool> tool,
               std::shared_ptr<QueryContext> c,
               std::function<void()> done,
               base::Value response) {
              if (tool) tool->OnGetAttributesResponse(c, std::move(response));
              done();
            },
            weak_factory_.GetWeakPtr(), ctx, check_done));
  }

  if (ctx->properties.count(kPropComputedStyle)) {
    base::DictValue params;
    params.Set("nodeId", ctx->node_id);
    ctx->session->SendCdpCommand(
        "CSS.getComputedStyleForNode", std::move(params),
        base::BindOnce(
            [](base::WeakPtr<ElementInfoTool> tool,
               std::shared_ptr<QueryContext> c,
               std::function<void()> done,
               base::Value response) {
              if (tool) tool->OnGetComputedStyleResponse(c, std::move(response));
              done();
            },
            weak_factory_.GetWeakPtr(), ctx, check_done));
  }

  if (ctx->properties.count(kPropBoundingBox)) {
    base::DictValue params;
    params.Set("nodeId", ctx->node_id);
    ctx->session->SendCdpCommand(
        "DOM.getBoxModel", std::move(params),
        base::BindOnce(
            [](base::WeakPtr<ElementInfoTool> tool,
               std::shared_ptr<QueryContext> c,
               std::function<void()> done,
               base::Value response) {
              if (tool) tool->OnGetBoxModelResponse(c, std::move(response));
              done();
            },
            weak_factory_.GetWeakPtr(), ctx, check_done));
  }

  // JS property (text, html, value, checked, visible) 처리
  // AX Tree 경로(selector == "(locator)")에서는 CSS 선택자가 없으므로
  // Runtime.evaluate 기반 조회를 스킵하고 경고를 결과에 포함한다.
  const bool is_locator_path = (ctx->selector == "(locator)");

  static constexpr const char* kJsProperties[] = {
      kPropText, kPropHtml, kPropValue, kPropChecked, kPropVisible,
  };

  if (is_locator_path) {
    // JS property가 요청된 경우 경고 메시지 포함 후 pending 감소
    bool has_js_prop = false;
    for (const char* prop : kJsProperties) {
      if (ctx->properties.count(prop)) {
        has_js_prop = true;
        break;
      }
    }
    if (has_js_prop) {
      ctx->result.Set(
          "_warning",
          "AX Tree 로케이터 경로에서는 text/html/value/checked/visible 조회를 "
          "지원하지 않습니다. CSS 선택자(selector) 또는 ref 로케이터를 사용하세요.");
    }
    // JS property에 해당하는 pending 수 감소
    for (const char* prop : kJsProperties) {
      if (ctx->properties.count(prop)) {
        check_done();
      }
    }
  } else {
    // selector 경로: Runtime.evaluate 기반 property 조회
    auto eval_property = [&](const std::string& prop_name,
                              const std::string& js_expr) {
      if (!ctx->properties.count(prop_name)) return;

      base::DictValue params;
      params.Set("expression", js_expr);
      params.Set("returnByValue", true);
      params.Set("awaitPromise", false);

      ctx->session->SendCdpCommand(
          "Runtime.evaluate", std::move(params),
          base::BindOnce(
              [](base::WeakPtr<ElementInfoTool> tool,
                 std::shared_ptr<QueryContext> c,
                 std::function<void()> done,
                 const std::string& pname,
                 base::Value response) {
                if (tool) {
                  tool->OnRuntimeEvaluateResponse(c, pname,
                                                   std::move(response));
                }
                done();
              },
              weak_factory_.GetWeakPtr(), ctx, check_done, prop_name));
    };

    // selector를 안전하게 이스케이프
    std::string escaped_sel = ctx->selector;
    std::string::size_type pos = 0;
    while ((pos = escaped_sel.find("'", pos)) != std::string::npos) {
      escaped_sel.replace(pos, 1, "\\'");
      pos += 2;
    }
    const std::string sel_js =
        "document.querySelector('" + escaped_sel + "')";

    eval_property(kPropText,
                  "(" + sel_js + " ? " + sel_js + ".innerText : null)");
    eval_property(kPropHtml,
                  "(" + sel_js + " ? " + sel_js + ".outerHTML : null)");
    eval_property(kPropValue,
                  "(" + sel_js + " ? " + sel_js + ".value : null)");
    eval_property(kPropChecked,
                  "(" + sel_js + " ? " + sel_js + ".checked : null)");
    eval_property(
        kPropVisible,
        "(() => {"
        "  const el = " + sel_js + ";"
        "  if (!el) return false;"
        "  const style = window.getComputedStyle(el);"
        "  return style.display !== 'none' && "
        "         style.visibility !== 'hidden' && "
        "         style.opacity !== '0' && "
        "         el.offsetWidth > 0 && el.offsetHeight > 0;"
        "})()");
  }
}

void ElementInfoTool::OnGetAttributesResponse(std::shared_ptr<QueryContext> ctx,
                                               base::Value response) {
  if (!response.is_dict()) return;

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* result_obj = dict.FindDict("result");
  const base::DictValue* data = result_obj ? result_obj : &dict;
  const base::ListValue* attrs = data->FindList("attributes");

  if (!attrs) return;

  base::DictValue attrs_dict;
  for (size_t i = 0; i + 1 < attrs->size(); i += 2) {
    const auto& name = (*attrs)[i];
    const auto& value = (*attrs)[i + 1];
    if (name.is_string() && value.is_string()) {
      attrs_dict.Set(name.GetString(), value.GetString());
    }
  }

  ctx->result.Set(kPropAttributes, std::move(attrs_dict));
  LOG(INFO) << "[ElementInfoTool] attributes 조회 완료";
}

void ElementInfoTool::OnGetComputedStyleResponse(
    std::shared_ptr<QueryContext> ctx,
    base::Value response) {
  if (!response.is_dict()) return;

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* result_obj = dict.FindDict("result");
  const base::DictValue* data = result_obj ? result_obj : &dict;
  const base::ListValue* style_list = data->FindList("computedStyle");

  if (!style_list) return;

  base::DictValue style_dict;
  for (const auto& item : *style_list) {
    if (!item.is_dict()) continue;
    const base::DictValue& item_dict = item.GetDict();
    const std::string* name  = item_dict.FindString("name");
    const std::string* value = item_dict.FindString("value");
    if (name && value) {
      style_dict.Set(*name, *value);
    }
  }

  ctx->result.Set(kPropComputedStyle, std::move(style_dict));
  LOG(INFO) << "[ElementInfoTool] computedStyle 조회 완료";
}

void ElementInfoTool::OnGetBoxModelResponse(std::shared_ptr<QueryContext> ctx,
                                             base::Value response) {
  if (!response.is_dict()) return;

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* result_obj = dict.FindDict("result");
  const base::DictValue* data = result_obj ? result_obj : &dict;
  const base::DictValue* model = data->FindDict("model");

  if (!model) return;

  base::DictValue bbox;

  std::optional<int> width  = model->FindInt("width");
  std::optional<int> height = model->FindInt("height");
  if (width.has_value())  bbox.Set("width",  *width);
  if (height.has_value()) bbox.Set("height", *height);

  const base::ListValue* content = model->FindList("content");
  if (content && content->size() >= 2) {
    bbox.Set("x", (*content)[0].GetDouble());
    bbox.Set("y", (*content)[1].GetDouble());
  }

  const base::ListValue* border = model->FindList("border");
  if (border && border->size() >= 8) {
    double x1 = (*border)[0].GetDouble();
    double y1 = (*border)[1].GetDouble();
    double x3 = (*border)[4].GetDouble();
    double y3 = (*border)[5].GetDouble();
    bbox.Set("borderX", x1);
    bbox.Set("borderY", y1);
    bbox.Set("borderWidth",  x3 - x1);
    bbox.Set("borderHeight", y3 - y1);
  }

  ctx->result.Set(kPropBoundingBox, std::move(bbox));
  LOG(INFO) << "[ElementInfoTool] boundingBox 조회 완료";
}

void ElementInfoTool::OnRuntimeEvaluateResponse(
    std::shared_ptr<QueryContext> ctx,
    const std::string& property_name,
    base::Value response) {
  if (!response.is_dict()) return;

  const base::DictValue& dict = response.GetDict();
  // 편의 오버로드: dict = {"result": RemoteObject, ...}
  // result_obj는 이미 RemoteObject
  const base::DictValue* result_obj = dict.FindDict("result");
  if (!result_obj) return;

  const base::Value* val = result_obj->Find("value");
  if (val) {
    ctx->result.Set(property_name, val->Clone());
    LOG(INFO) << "[ElementInfoTool] " << property_name << " 조회 완료";
  } else {
    ctx->result.Set(property_name, base::Value());
  }
}

void ElementInfoTool::FinalizeResult(std::shared_ptr<QueryContext> ctx) {
  ctx->result.Set("success", true);
  ctx->result.Set("selector", ctx->selector);
  std::move(ctx->callback).Run(base::Value(std::move(ctx->result)));
}

}  // namespace mcp
