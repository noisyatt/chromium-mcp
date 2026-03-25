// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/element_tool.h"

#include <memory>
#include <optional>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

namespace {

// properties 파라미터에서 사용 가능한 속성 이름 상수
constexpr char kPropAttributes[]    = "attributes";
constexpr char kPropComputedStyles[] = "computedStyles";
constexpr char kPropBoxModel[]      = "boxModel";
constexpr char kPropTagInfo[]       = "tagInfo";  // describeNode 결과

}  // namespace

ElementTool::QueryContext::QueryContext() = default;
ElementTool::QueryContext::~QueryContext() = default;

ElementTool::ElementTool() = default;
ElementTool::~ElementTool() = default;

std::string ElementTool::name() const {
  return "element";
}

std::string ElementTool::description() const {
  return "DOM 요소의 속성, 스타일, 위치 등 상세 정보 조회. "
         "role/name, text, selector, xpath, ref 등 다양한 방법으로 요소를 지정합니다.";
}

base::DictValue ElementTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue props;

  // ---- 공통 로케이터 파라미터 ----

  // role: ARIA 역할
  base::DictValue role_prop;
  role_prop.Set("type", "string");
  role_prop.Set("description",
                "조회할 요소의 ARIA 역할 (예: \"button\", \"link\"). "
                "name 파라미터와 함께 사용합니다.");
  props.Set("role", std::move(role_prop));

  // name: 접근성 이름
  base::DictValue name_prop;
  name_prop.Set("type", "string");
  name_prop.Set("description",
                "요소의 접근성 이름. role 파라미터와 함께 사용합니다.");
  props.Set("name", std::move(name_prop));

  // text: 표시 텍스트
  base::DictValue text_prop;
  text_prop.Set("type", "string");
  text_prop.Set("description",
                "요소의 표시 텍스트로 탐색. exact=false이면 부분 일치 허용.");
  props.Set("text", std::move(text_prop));

  // selector: CSS 선택자
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description",
          "조회할 DOM 요소의 CSS 선택자 (예: '#main', '.btn', 'input[type=text]')");
    props.Set("selector", std::move(p));
  }

  // xpath: XPath 표현식
  base::DictValue xpath_prop;
  xpath_prop.Set("type", "string");
  xpath_prop.Set("description",
                 "조회할 요소의 XPath 표현식.");
  props.Set("xpath", std::move(xpath_prop));

  // ref: backendNodeId 참조
  base::DictValue ref_prop;
  ref_prop.Set("type", "string");
  ref_prop.Set("description",
               "접근성 스냅샷 또는 element 도구에서 얻은 요소 ref (backendNodeId).");
  props.Set("ref", std::move(ref_prop));

  // exact: 텍스트/이름 정확히 일치 여부
  base::DictValue exact_prop;
  exact_prop.Set("type", "boolean");
  exact_prop.Set("default", true);
  exact_prop.Set("description",
                 "true이면 name/text 파라미터를 정확히 일치, "
                 "false이면 부분 문자열 일치로 탐색 (기본: true).");
  props.Set("exact", std::move(exact_prop));

  // properties: 조회할 속성 목록 (선택적; 생략 시 전체)
  {
    base::DictValue p;
    p.Set("type", "array");
    base::DictValue items;
    items.Set("type", "string");
    base::ListValue enum_vals;
    enum_vals.Append(kPropAttributes);
    enum_vals.Append(kPropComputedStyles);
    enum_vals.Append(kPropBoxModel);
    enum_vals.Append(kPropTagInfo);
    items.Set("enum", std::move(enum_vals));
    p.Set("items", std::move(items));
    p.Set("description",
          "조회할 항목 목록. 생략 시 전체 조회.\n"
          "  attributes   : HTML 속성 (DOM.getAttributes)\n"
          "  computedStyles: 계산된 CSS 스타일 (CSS.getComputedStyleForNode)\n"
          "  boxModel     : 마진/보더/패딩/컨텐츠 영역 (DOM.getBoxModel)\n"
          "  tagInfo      : 태그명·클래스명 등 기본 정보 (DOM.describeNode)");
    props.Set("properties", std::move(p));
  }

  // includeStyles: computed styles 포함 여부 (bool; 기본 false)
  {
    base::DictValue p;
    p.Set("type", "boolean");
    p.Set("description",
          "true 이면 computedStyles 항목을 자동으로 포함한다 (기본값: false)");
    p.Set("default", false);
    props.Set("includeStyles", std::move(p));
  }

  // includeBox: box model 포함 여부 (bool; 기본 false)
  {
    base::DictValue p;
    p.Set("type", "boolean");
    p.Set("description",
          "true 이면 boxModel 항목을 자동으로 포함한다 (기본값: false)");
    p.Set("default", false);
    props.Set("includeBox", std::move(p));
  }

  schema.Set("properties", std::move(props));

  // 로케이터는 런타임에서 검증
  base::ListValue required;
  schema.Set("required", std::move(required));

  return schema;
}

// -----------------------------------------------------------------------
// Execute
// -----------------------------------------------------------------------
void ElementTool::Execute(const base::DictValue& arguments,
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

  auto ctx = std::make_shared<QueryContext>();
  // 로깅용 selector 또는 첫 번째 로케이터 표기
  const std::string* sel = arguments.FindString("selector");
  ctx->selector = sel ? *sel : "(locator)";
  ctx->session  = session;
  ctx->callback = std::move(callback);

  // includeStyles / includeBox 플래그 읽기
  if (const std::optional<bool> v = arguments.FindBool("includeStyles")) {
    ctx->include_styles = *v;
  }
  if (const std::optional<bool> v = arguments.FindBool("includeBox")) {
    ctx->include_box = *v;
  }

  // properties 목록 읽기
  if (const base::ListValue* list = arguments.FindList("properties")) {
    for (const auto& item : *list) {
      if (item.is_string()) {
        ctx->requested_properties.insert(item.GetString());
      }
    }
  }

  // 플래그로 추가된 항목을 requested_properties 에 반영
  if (ctx->include_styles) {
    ctx->requested_properties.insert(kPropComputedStyles);
  }
  if (ctx->include_box) {
    ctx->requested_properties.insert(kPropBoxModel);
  }

  // 아무 것도 지정하지 않았으면 전체 조회
  if (ctx->requested_properties.empty()) {
    ctx->requested_properties.insert(kPropAttributes);
    ctx->requested_properties.insert(kPropComputedStyles);
    ctx->requested_properties.insert(kPropBoxModel);
    ctx->requested_properties.insert(kPropTagInfo);
  }

  LOG(INFO) << "[ElementTool] 요소 조회 시작 selector=" << ctx->selector;

  // ElementLocator로 요소 탐색
  locator_.Locate(
      session, arguments,
      base::BindOnce(&ElementTool::OnLocated, weak_factory_.GetWeakPtr(), ctx));
}

// -----------------------------------------------------------------------
// ElementLocator 콜백 → 병렬 CDP 요청 실행
// -----------------------------------------------------------------------
void ElementTool::OnLocated(std::shared_ptr<QueryContext> ctx,
                             std::optional<ElementLocator::Result> result,
                             std::string error) {
  if (!error.empty()) {
    std::move(ctx->callback).Run(MakeErrorResult(error));
    return;
  }

  ctx->node_id = result->node_id;
  if (ctx->node_id == 0) {
    // node_id가 없으면 backend_node_id로 DOM.describeNode를 통해 nodeId 획득 필요.
    // ElementLocator는 backendNodeId 기반이므로, nodeId가 0인 경우
    // DOM.getBoxModel 등에는 backendNodeId를 사용한다.
    // 여기서는 backend_node_id를 음수로 저장하여 구분 가능하게 한다.
    // 실제로는 ElementLocator가 nodeId를 함께 반환하는지 확인 필요.
    // ElementLocator::Result.node_id는 0이면 미사용이므로
    // DOM 조회 시 backendNodeId 파라미터를 사용해야 한다.
    ctx->node_id = result->backend_node_id;  // backendNodeId로 대체
  }

  LOG(INFO) << "[ElementTool] nodeId=" << ctx->node_id
            << " backendNodeId=" << result->backend_node_id;

  DispatchRequests(ctx);
}

// -----------------------------------------------------------------------
// 요청된 속성에 따라 CDP 명령 병렬 실행
// -----------------------------------------------------------------------
void ElementTool::DispatchRequests(std::shared_ptr<QueryContext> ctx) {
  const std::set<std::string>& req = ctx->requested_properties;

  ctx->pending = 0;
  if (req.count(kPropAttributes))    ++ctx->pending;
  if (req.count(kPropComputedStyles)) ++ctx->pending;
  if (req.count(kPropBoxModel))      ++ctx->pending;
  if (req.count(kPropTagInfo))       ++ctx->pending;

  if (ctx->pending == 0) {
    ctx->result.Set("success", true);
    ctx->result.Set("selector", ctx->selector);
    std::move(ctx->callback).Run(base::Value(std::move(ctx->result)));
    return;
  }

  // DOM.getAttributes
  if (req.count(kPropAttributes)) {
    base::DictValue p;
    p.Set("nodeId", ctx->node_id);
    ctx->session->SendCdpCommand(
        "DOM.getAttributes", std::move(p),
        base::BindOnce(&ElementTool::OnGetAttributes,
                       weak_factory_.GetWeakPtr(), ctx));
  }

  // CSS.getComputedStyleForNode
  if (req.count(kPropComputedStyles)) {
    base::DictValue p;
    p.Set("nodeId", ctx->node_id);
    ctx->session->SendCdpCommand(
        "CSS.getComputedStyleForNode", std::move(p),
        base::BindOnce(&ElementTool::OnGetComputedStyle,
                       weak_factory_.GetWeakPtr(), ctx));
  }

  // DOM.getBoxModel
  if (req.count(kPropBoxModel)) {
    base::DictValue p;
    p.Set("nodeId", ctx->node_id);
    ctx->session->SendCdpCommand(
        "DOM.getBoxModel", std::move(p),
        base::BindOnce(&ElementTool::OnGetBoxModel,
                       weak_factory_.GetWeakPtr(), ctx));
  }

  // DOM.describeNode
  if (req.count(kPropTagInfo)) {
    base::DictValue p;
    p.Set("nodeId", ctx->node_id);
    p.Set("depth", 0);
    ctx->session->SendCdpCommand(
        "DOM.describeNode", std::move(p),
        base::BindOnce(&ElementTool::OnDescribeNode,
                       weak_factory_.GetWeakPtr(), ctx));
  }
}

// -----------------------------------------------------------------------
// DOM.getAttributes 응답
// -----------------------------------------------------------------------
void ElementTool::OnGetAttributes(std::shared_ptr<QueryContext> ctx,
                                   base::Value response) {
  if (response.is_dict()) {
    const base::DictValue& d = response.GetDict();

    const base::ListValue* attrs = nullptr;
    if (const base::DictValue* r = d.FindDict("result")) {
      attrs = r->FindList("attributes");
    }
    if (!attrs) {
      attrs = d.FindList("attributes");
    }

    if (attrs) {
      base::DictValue attrs_dict;
      for (size_t i = 0; i + 1 < attrs->size(); i += 2) {
        const auto& k = (*attrs)[i];
        const auto& v = (*attrs)[i + 1];
        if (k.is_string() && v.is_string()) {
          attrs_dict.Set(k.GetString(), v.GetString());
        }
      }
      ctx->result.Set(kPropAttributes, std::move(attrs_dict));
      LOG(INFO) << "[ElementTool] attributes 완료";
    }
  }
  OnOneRequestDone(ctx);
}

// -----------------------------------------------------------------------
// CSS.getComputedStyleForNode 응답
// -----------------------------------------------------------------------
void ElementTool::OnGetComputedStyle(std::shared_ptr<QueryContext> ctx,
                                      base::Value response) {
  if (response.is_dict()) {
    const base::DictValue& d = response.GetDict();

    const base::ListValue* style_list = nullptr;
    if (const base::DictValue* r = d.FindDict("result")) {
      style_list = r->FindList("computedStyle");
    }
    if (!style_list) {
      style_list = d.FindList("computedStyle");
    }

    if (style_list) {
      base::DictValue style_dict;
      for (const auto& item : *style_list) {
        if (!item.is_dict()) continue;
        const auto& item_d = item.GetDict();
        const std::string* n = item_d.FindString("name");
        const std::string* v = item_d.FindString("value");
        if (n && v) {
          style_dict.Set(*n, *v);
        }
      }
      ctx->result.Set(kPropComputedStyles, std::move(style_dict));
      LOG(INFO) << "[ElementTool] computedStyles 완료";
    }
  }
  OnOneRequestDone(ctx);
}

// -----------------------------------------------------------------------
// DOM.getBoxModel 응답
// -----------------------------------------------------------------------
void ElementTool::OnGetBoxModel(std::shared_ptr<QueryContext> ctx,
                                 base::Value response) {
  if (response.is_dict()) {
    const base::DictValue& d = response.GetDict();

    const base::DictValue* model = nullptr;
    if (const base::DictValue* r = d.FindDict("result")) {
      model = r->FindDict("model");
    }
    if (!model) {
      model = d.FindDict("model");
    }

    if (model) {
      base::DictValue box;

      if (auto v = model->FindInt("width"))  box.Set("width",  *v);
      if (auto v = model->FindInt("height")) box.Set("height", *v);

      auto quad_to_rect = [](const base::ListValue* quad,
                              base::DictValue& out,
                              const std::string& prefix) {
        if (!quad || quad->size() < 8) return;
        double x1 = (*quad)[0].GetDouble();
        double y1 = (*quad)[1].GetDouble();
        double x3 = (*quad)[4].GetDouble();
        double y3 = (*quad)[5].GetDouble();
        out.Set(prefix + "X",      x1);
        out.Set(prefix + "Y",      y1);
        out.Set(prefix + "Width",  x3 - x1);
        out.Set(prefix + "Height", y3 - y1);
      };

      quad_to_rect(model->FindList("content"), box, "content");
      quad_to_rect(model->FindList("padding"), box, "padding");
      quad_to_rect(model->FindList("border"),  box, "border");
      quad_to_rect(model->FindList("margin"),  box, "margin");

      ctx->result.Set(kPropBoxModel, std::move(box));
      LOG(INFO) << "[ElementTool] boxModel 완료";
    }
  }
  OnOneRequestDone(ctx);
}

// -----------------------------------------------------------------------
// DOM.describeNode 응답
// -----------------------------------------------------------------------
void ElementTool::OnDescribeNode(std::shared_ptr<QueryContext> ctx,
                                  base::Value response) {
  if (response.is_dict()) {
    const base::DictValue& d = response.GetDict();

    const base::DictValue* node = nullptr;
    if (const base::DictValue* r = d.FindDict("result")) {
      node = r->FindDict("node");
    }
    if (!node) {
      node = d.FindDict("node");
    }

    if (node) {
      base::DictValue tag_info;

      if (const std::string* v = node->FindString("nodeName")) {
        tag_info.Set("tagName", *v);
        ctx->result.Set("tagName", *v);
      }
      if (const std::string* v = node->FindString("localName")) {
        tag_info.Set("localName", *v);
      }
      if (const auto v = node->FindInt("nodeType")) {
        tag_info.Set("nodeType", *v);
      }

      if (const base::ListValue* attrs = node->FindList("attributes")) {
        for (size_t i = 0; i + 1 < attrs->size(); i += 2) {
          const auto& k = (*attrs)[i];
          const auto& v = (*attrs)[i + 1];
          if (!k.is_string() || !v.is_string()) continue;
          const std::string& key = k.GetString();
          if (key == "id") {
            tag_info.Set("id", v.GetString());
            ctx->result.Set("id", v.GetString());
          } else if (key == "class") {
            tag_info.Set("className", v.GetString());
            ctx->result.Set("className", v.GetString());
          }
        }
      }

      ctx->result.Set(kPropTagInfo, std::move(tag_info));
      LOG(INFO) << "[ElementTool] tagInfo 완료";
    }
  }
  OnOneRequestDone(ctx);
}

// -----------------------------------------------------------------------
// 완료 카운터 처리 및 최종 결과 반환
// -----------------------------------------------------------------------
void ElementTool::OnOneRequestDone(std::shared_ptr<QueryContext> ctx) {
  --ctx->pending;
  if (ctx->pending > 0 || ctx->finalized) return;

  ctx->finalized = true;
  ctx->result.Set("success",  true);
  ctx->result.Set("selector", ctx->selector);
  ctx->result.Set("nodeId",   ctx->node_id);

  LOG(INFO) << "[ElementTool] 조회 완료 selector=" << ctx->selector;
  std::move(ctx->callback).Run(base::Value(std::move(ctx->result)));
}

// -----------------------------------------------------------------------
// 정적 헬퍼: CDP 오류 메시지 추출
// -----------------------------------------------------------------------
// static
std::string ElementTool::ExtractCdpError(
    const base::DictValue& response_dict) {
  const base::DictValue* err = response_dict.FindDict("error");
  if (!err) return {};
  const std::string* msg = err->FindString("message");
  return msg ? *msg : "CDP 오류 (메시지 없음)";
}

}  // namespace mcp
