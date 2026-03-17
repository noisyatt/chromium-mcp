// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/element_info_tool.h"

#include <memory>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

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

// QueryContext에 완료 카운터를 추가하기 위한 확장 구조체
struct ExtendedQueryContext : public ElementInfoTool::QueryContext {
  int pending_count = 0;     // 아직 완료되지 않은 비동기 요청 수
  bool finalized = false;    // 이미 최종 결과를 반환했는지 여부
};

ElementInfoTool::QueryContext::QueryContext() = default;
ElementInfoTool::QueryContext::~QueryContext() = default;

ElementInfoTool::ElementInfoTool() = default;
ElementInfoTool::~ElementInfoTool() = default;

std::string ElementInfoTool::name() const {
  return "element_info";
}

std::string ElementInfoTool::description() const {
  return "요소의 상세 정보 조회 (속성, 스타일, 위치, 텍스트)";
}

base::DictValue ElementInfoTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // selector: CSS 선택자 (필수)
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description", "조회할 요소의 CSS 선택자 (필수)");
  properties.Set("selector", std::move(selector_prop));

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

  // selector는 필수 파라미터
  base::ListValue required;
  required.Append("selector");
  schema.Set("required", std::move(required));

  return schema;
}

void ElementInfoTool::Execute(const base::DictValue& arguments,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback) {
  // selector 파라미터 추출 (필수)
  const std::string* selector_ptr = arguments.FindString("selector");
  if (!selector_ptr || selector_ptr->empty()) {
    base::DictValue err;
    err.Set("error", "selector 파라미터가 필요합니다");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  // properties 파라미터 추출 (선택적)
  auto ctx = std::make_shared<QueryContext>();
  ctx->selector = *selector_ptr;
  ctx->session = session;
  ctx->callback = std::move(callback);

  const base::ListValue* props_list = arguments.FindList("properties");
  if (props_list && !props_list->empty()) {
    // 지정된 property만 조회
    for (const auto& prop : *props_list) {
      if (prop.is_string()) {
        ctx->properties.insert(prop.GetString());
      }
    }
  } else {
    // 미지정 시 전체 property 조회
    for (const char* prop : kAllProperties) {
      ctx->properties.insert(prop);
    }
  }

  LOG(INFO) << "[ElementInfoTool] 요소 정보 조회: selector=" << ctx->selector
            << " properties 수=" << ctx->properties.size();

  // 1단계: DOM.getDocument로 루트 nodeId 획득
  base::DictValue get_doc_params;
  get_doc_params.Set("depth", 0);
  get_doc_params.Set("pierce", false);

  session->SendCdpCommand(
      "DOM.getDocument", std::move(get_doc_params),
      base::BindOnce(&ElementInfoTool::OnGetDocumentResponse,
                     weak_factory_.GetWeakPtr(), ctx));
}

void ElementInfoTool::OnGetDocumentResponse(std::shared_ptr<QueryContext> ctx,
                                             base::Value response) {
  if (!response.is_dict()) {
    base::DictValue err;
    err.Set("error", "DOM.getDocument 응답 형식 오류");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // CDP 오류 확인
  const base::DictValue* err_dict = response.GetDict().FindDict("error");
  if (err_dict) {
    const std::string* msg = err_dict->FindString("message");
    base::DictValue err;
    err.Set("error", msg ? *msg : "DOM.getDocument 실패");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // result 내부의 root 노드에서 nodeId 추출
  const base::DictValue& dict = response.GetDict();
  const base::DictValue* result_obj = dict.FindDict("result");
  const base::DictValue* root =
      result_obj ? result_obj->FindDict("root") : dict.FindDict("root");

  if (!root) {
    base::DictValue err;
    err.Set("error", "DOM 루트 노드를 찾을 수 없음");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  std::optional<int> root_node_id = root->FindInt("nodeId");
  if (!root_node_id) {
    base::DictValue err;
    err.Set("error", "루트 nodeId 추출 실패");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // 2단계: DOM.querySelector로 selector에 해당하는 nodeId 획득
  base::DictValue qs_params;
  qs_params.Set("nodeId", *root_node_id);
  qs_params.Set("selector", ctx->selector);

  ctx->session->SendCdpCommand(
      "DOM.querySelector", std::move(qs_params),
      base::BindOnce(&ElementInfoTool::OnQuerySelectorResponse,
                     weak_factory_.GetWeakPtr(), ctx));
}

void ElementInfoTool::OnQuerySelectorResponse(std::shared_ptr<QueryContext> ctx,
                                               base::Value response) {
  if (!response.is_dict()) {
    base::DictValue err;
    err.Set("error", "DOM.querySelector 응답 형식 오류");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::DictValue& dict = response.GetDict();

  // CDP 오류 확인
  const base::DictValue* err_dict = dict.FindDict("error");
  if (err_dict) {
    const std::string* msg = err_dict->FindString("message");
    base::DictValue err;
    err.Set("error", msg ? *msg : "DOM.querySelector 실패");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // result 내부의 nodeId 추출 (래핑 방식에 따라 위치가 다를 수 있음)
  const base::DictValue* result_obj = dict.FindDict("result");
  const base::DictValue* node_dict = result_obj ? result_obj : &dict;
  std::optional<int> node_id = node_dict->FindInt("nodeId");
  if (!node_id || *node_id == 0) {
    LOG(WARNING) << "[ElementInfoTool] selector에 해당하는 요소 없음: "
                 << ctx->selector;
    base::DictValue err;
    err.Set("error", "selector에 해당하는 요소를 찾을 수 없음: " + ctx->selector);
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  ctx->node_id = *node_id;
  LOG(INFO) << "[ElementInfoTool] nodeId 획득: " << ctx->node_id;

  // 3단계: 요청된 각 property 조회 시작
  FetchProperties(ctx);
}

void ElementInfoTool::FetchProperties(std::shared_ptr<QueryContext> ctx) {
  // 각 property에 대해 비동기 CDP 명령을 실행한다.
  // 완료 카운터로 모든 요청이 끝났을 때 최종 결과를 반환한다.

  // pending_count를 공유 변수로 관리
  auto pending = std::make_shared<int>(static_cast<int>(ctx->properties.size()));
  auto finalized = std::make_shared<bool>(false);

  // 완료 체크 람다: pending이 0이 되면 FinalizeResult 호출
  auto check_done = [ctx, pending, finalized]() {
    (*pending)--;
    if (*pending <= 0 && !*finalized) {
      *finalized = true;
      // 최종 결과 반환
      ctx->result.Set("success", true);
      ctx->result.Set("selector", ctx->selector);
      std::move(ctx->callback).Run(base::Value(std::move(ctx->result)));
    }
  };

  if (ctx->properties.count(kPropAttributes)) {
    // DOM.getAttributes: HTML 속성 조회
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
    // CSS.getComputedStyleForNode: 계산된 스타일 조회
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
    // DOM.getBoxModel: 위치/크기 조회
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

  // Runtime.evaluate 기반 property들 (Runtime.enable 없이 사용 가능)
  // 각 property마다 JS 표현식을 구성하여 평가한다.
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
                tool->OnRuntimeEvaluateResponse(c, pname, std::move(response));
              }
              done();
            },
            weak_factory_.GetWeakPtr(), ctx, check_done, prop_name));
  };

  // selector를 안전하게 이스케이프하여 JS 문자열 내 삽입
  // 기본적인 이스케이프: 작은따옴표를 \\'로 변환
  std::string escaped_sel = ctx->selector;
  std::string::size_type pos = 0;
  while ((pos = escaped_sel.find("'", pos)) != std::string::npos) {
    escaped_sel.replace(pos, 1, "\\'");
    pos += 2;
  }
  const std::string sel_js = "document.querySelector('" + escaped_sel + "')";

  // text: innerText
  eval_property(kPropText,
                "(" + sel_js + " ? " + sel_js + ".innerText : null)");

  // html: outerHTML
  eval_property(kPropHtml,
                "(" + sel_js + " ? " + sel_js + ".outerHTML : null)");

  // value: input/textarea의 현재 값
  eval_property(kPropValue,
                "(" + sel_js + " ? " + sel_js + ".value : null)");

  // checked: checkbox/radio의 체크 상태
  eval_property(kPropChecked,
                "(" + sel_js + " ? " + sel_js + ".checked : null)");

  // visible: 가시성 여부 (offsetParent가 null이 아니고 display가 none이 아닌 경우)
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

void ElementInfoTool::OnGetAttributesResponse(std::shared_ptr<QueryContext> ctx,
                                               base::Value response) {
  // DOM.getAttributes 응답: {attributes: [name, value, name, value, ...]}
  // 속성 이름과 값이 교대로 나열된 배열 형식이다.
  if (!response.is_dict()) return;

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* result_obj = dict.FindDict("result");
  const base::DictValue* data = result_obj ? result_obj : &dict;
  const base::ListValue* attrs = data->FindList("attributes");

  if (!attrs) return;

  // 속성 배열을 {name: value} 딕셔너리로 변환
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
  // CSS.getComputedStyleForNode 응답:
  // {computedStyle: [{name: "...", value: "..."}, ...]}
  if (!response.is_dict()) return;

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* result_obj = dict.FindDict("result");
  const base::DictValue* data = result_obj ? result_obj : &dict;
  const base::ListValue* style_list = data->FindList("computedStyle");

  if (!style_list) return;

  // 스타일 배열을 {property: value} 딕셔너리로 변환
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
  // DOM.getBoxModel 응답:
  // {model: {content, padding, border, margin, width, height}}
  // 각 배열은 4개 꼭짓점의 x,y 좌표 8개 값이다.
  if (!response.is_dict()) return;

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* result_obj = dict.FindDict("result");
  const base::DictValue* data = result_obj ? result_obj : &dict;
  const base::DictValue* model = data->FindDict("model");

  if (!model) return;

  base::DictValue bbox;

  // width, height는 직접 포함되어 있다.
  std::optional<int> width  = model->FindInt("width");
  std::optional<int> height = model->FindInt("height");
  if (width.has_value())  bbox.Set("width",  *width);
  if (height.has_value()) bbox.Set("height", *height);

  // content 배열에서 좌상단 x, y 추출 (첫 번째 꼭짓점)
  const base::ListValue* content = model->FindList("content");
  if (content && content->size() >= 2) {
    bbox.Set("x", (*content)[0].GetDouble());
    bbox.Set("y", (*content)[1].GetDouble());
  }

  // border 배열도 함께 포함
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
  // Runtime.evaluate 응답에서 값을 추출한다.
  // 응답 구조: {result: {type, value, ...}} 또는 래핑된 형태
  if (!response.is_dict()) return;

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* result_obj = dict.FindDict("result");
  const base::DictValue* eval_result = result_obj;

  // 한 번 더 래핑된 경우 처리
  if (eval_result) {
    const base::DictValue* inner = eval_result->FindDict("result");
    if (inner) eval_result = inner;
  }

  if (!eval_result) return;

  const base::Value* val = eval_result->Find("value");
  if (val) {
    ctx->result.Set(property_name, val->Clone());
    LOG(INFO) << "[ElementInfoTool] " << property_name << " 조회 완료";
  } else {
    // null 또는 undefined
    ctx->result.Set(property_name, base::Value());
  }
}

void ElementInfoTool::FinalizeResult(std::shared_ptr<QueryContext> ctx) {
  // 이 메서드는 FetchProperties 내 람다에서 직접 처리하므로 사용되지 않음.
  // 향후 리팩토링을 위해 남겨둠.
  ctx->result.Set("success", true);
  ctx->result.Set("selector", ctx->selector);
  std::move(ctx->callback).Run(base::Value(std::move(ctx->result)));
}

}  // namespace mcp
