// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/element_tool.h"

#include <memory>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

namespace {

// properties 파라미터에서 사용 가능한 속성 이름 상수
constexpr char kPropAttributes[]    = "attributes";
constexpr char kPropComputedStyles[] = "computedStyles";
constexpr char kPropBoxModel[]      = "boxModel";
constexpr char kPropTagInfo[]       = "tagInfo";  // describeNode 결과

}  // namespace

ElementTool::ElementTool() = default;
ElementTool::~ElementTool() = default;

std::string ElementTool::name() const {
  return "element";
}

std::string ElementTool::description() const {
  return "DOM 요소의 속성, 스타일, 위치 등 상세 정보 조회";
}

base::Value::Dict ElementTool::input_schema() const {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict props;

  // selector: CSS 선택자 (필수)
  {
    base::Value::Dict p;
    p.Set("type", "string");
    p.Set("description",
          "조회할 DOM 요소의 CSS 선택자 (예: '#main', '.btn', 'input[type=text]')");
    props.Set("selector", std::move(p));
  }

  // properties: 조회할 속성 목록 (선택적; 생략 시 전체)
  {
    base::Value::Dict p;
    p.Set("type", "array");
    base::Value::Dict items;
    items.Set("type", "string");
    base::Value::List enum_vals;
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
    base::Value::Dict p;
    p.Set("type", "boolean");
    p.Set("description",
          "true 이면 computedStyles 항목을 자동으로 포함한다 (기본값: false)");
    p.Set("default", false);
    props.Set("includeStyles", std::move(p));
  }

  // includeBox: box model 포함 여부 (bool; 기본 false)
  {
    base::Value::Dict p;
    p.Set("type", "boolean");
    p.Set("description",
          "true 이면 boxModel 항목을 자동으로 포함한다 (기본값: false)");
    p.Set("default", false);
    props.Set("includeBox", std::move(p));
  }

  schema.Set("properties", std::move(props));

  // selector 는 필수
  base::Value::List required;
  required.Append("selector");
  schema.Set("required", std::move(required));

  return schema;
}

// -----------------------------------------------------------------------
// Execute
// -----------------------------------------------------------------------
void ElementTool::Execute(const base::Value::Dict& arguments,
                           McpSession* session,
                           base::OnceCallback<void(base::Value)> callback) {
  const std::string* selector_ptr = arguments.FindString("selector");
  if (!selector_ptr || selector_ptr->empty()) {
    base::Value::Dict err;
    err.Set("error", "selector 파라미터가 필요합니다");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  auto ctx = std::make_shared<QueryContext>();
  ctx->selector = *selector_ptr;
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
  if (const base::Value::List* list = arguments.FindList("properties")) {
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

  // 1단계: DOM.getDocument (depth=0 으로 최소한의 응답만 받음)
  base::Value::Dict params;
  params.Set("depth", 0);
  params.Set("pierce", false);

  session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&ElementTool::OnGetDocument,
                     weak_factory_.GetWeakPtr(), ctx));
}

// -----------------------------------------------------------------------
// DOM.getDocument 응답 → DOM.querySelector
// -----------------------------------------------------------------------
void ElementTool::OnGetDocument(std::shared_ptr<QueryContext> ctx,
                                 base::Value response) {
  if (!response.is_dict()) {
    base::Value::Dict err;
    err.Set("error", "DOM.getDocument: 응답 형식 오류");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::Value::Dict& d = response.GetDict();
  std::string cdp_err = ExtractCdpError(d);
  if (!cdp_err.empty()) {
    base::Value::Dict err;
    err.Set("error", "DOM.getDocument 실패: " + cdp_err);
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // 루트 nodeId 추출: result.root.nodeId 또는 root.nodeId
  const base::Value::Dict* root = nullptr;
  if (const base::Value::Dict* r = d.FindDict("result")) {
    root = r->FindDict("root");
  }
  if (!root) {
    root = d.FindDict("root");
  }

  std::optional<int> root_id = root ? root->FindInt("nodeId") : std::nullopt;
  if (!root_id) {
    base::Value::Dict err;
    err.Set("error", "DOM.getDocument: 루트 nodeId 추출 실패");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // 2단계: DOM.querySelector
  base::Value::Dict params;
  params.Set("nodeId", *root_id);
  params.Set("selector", ctx->selector);

  ctx->session->SendCdpCommand(
      "DOM.querySelector", std::move(params),
      base::BindOnce(&ElementTool::OnQuerySelector,
                     weak_factory_.GetWeakPtr(), ctx));
}

// -----------------------------------------------------------------------
// DOM.querySelector 응답 → 병렬 CDP 요청 실행
// -----------------------------------------------------------------------
void ElementTool::OnQuerySelector(std::shared_ptr<QueryContext> ctx,
                                   base::Value response) {
  if (!response.is_dict()) {
    base::Value::Dict err;
    err.Set("error", "DOM.querySelector: 응답 형식 오류");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::Value::Dict& d = response.GetDict();
  std::string cdp_err = ExtractCdpError(d);
  if (!cdp_err.empty()) {
    base::Value::Dict err;
    err.Set("error", "DOM.querySelector 실패: " + cdp_err);
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // nodeId 추출: result.nodeId 또는 nodeId
  std::optional<int> node_id;
  if (const base::Value::Dict* r = d.FindDict("result")) {
    node_id = r->FindInt("nodeId");
  }
  if (!node_id) {
    node_id = d.FindInt("nodeId");
  }

  if (!node_id || *node_id == 0) {
    base::Value::Dict err;
    err.Set("error",
            "selector 에 해당하는 요소를 찾을 수 없음: " + ctx->selector);
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  ctx->node_id = *node_id;
  LOG(INFO) << "[ElementTool] nodeId=" << ctx->node_id;

  DispatchRequests(ctx);
}

// -----------------------------------------------------------------------
// 요청된 속성에 따라 CDP 명령 병렬 실행
// -----------------------------------------------------------------------
void ElementTool::DispatchRequests(std::shared_ptr<QueryContext> ctx) {
  const std::set<std::string>& req = ctx->requested_properties;

  // pending 수 계산 (요청할 CDP 명령 수)
  ctx->pending = 0;
  if (req.count(kPropAttributes))    ++ctx->pending;
  if (req.count(kPropComputedStyles)) ++ctx->pending;
  if (req.count(kPropBoxModel))      ++ctx->pending;
  if (req.count(kPropTagInfo))       ++ctx->pending;

  if (ctx->pending == 0) {
    // 조회할 항목이 없으면 바로 반환
    ctx->result.Set("success", true);
    ctx->result.Set("selector", ctx->selector);
    std::move(ctx->callback).Run(base::Value(std::move(ctx->result)));
    return;
  }

  // DOM.getAttributes
  if (req.count(kPropAttributes)) {
    base::Value::Dict p;
    p.Set("nodeId", ctx->node_id);
    ctx->session->SendCdpCommand(
        "DOM.getAttributes", std::move(p),
        base::BindOnce(&ElementTool::OnGetAttributes,
                       weak_factory_.GetWeakPtr(), ctx));
  }

  // CSS.getComputedStyleForNode
  if (req.count(kPropComputedStyles)) {
    base::Value::Dict p;
    p.Set("nodeId", ctx->node_id);
    ctx->session->SendCdpCommand(
        "CSS.getComputedStyleForNode", std::move(p),
        base::BindOnce(&ElementTool::OnGetComputedStyle,
                       weak_factory_.GetWeakPtr(), ctx));
  }

  // DOM.getBoxModel
  if (req.count(kPropBoxModel)) {
    base::Value::Dict p;
    p.Set("nodeId", ctx->node_id);
    ctx->session->SendCdpCommand(
        "DOM.getBoxModel", std::move(p),
        base::BindOnce(&ElementTool::OnGetBoxModel,
                       weak_factory_.GetWeakPtr(), ctx));
  }

  // DOM.describeNode
  if (req.count(kPropTagInfo)) {
    base::Value::Dict p;
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
    const base::Value::Dict& d = response.GetDict();

    // attributes 배열: [name, value, name, value, ...]
    const base::Value::List* attrs = nullptr;
    if (const base::Value::Dict* r = d.FindDict("result")) {
      attrs = r->FindList("attributes");
    }
    if (!attrs) {
      attrs = d.FindList("attributes");
    }

    if (attrs) {
      base::Value::Dict attrs_dict;
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
    const base::Value::Dict& d = response.GetDict();

    // computedStyle: [{name: "...", value: "..."}, ...]
    const base::Value::List* style_list = nullptr;
    if (const base::Value::Dict* r = d.FindDict("result")) {
      style_list = r->FindList("computedStyle");
    }
    if (!style_list) {
      style_list = d.FindList("computedStyle");
    }

    if (style_list) {
      base::Value::Dict style_dict;
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
    const base::Value::Dict& d = response.GetDict();

    // model: {content, padding, border, margin, width, height}
    const base::Value::Dict* model = nullptr;
    if (const base::Value::Dict* r = d.FindDict("result")) {
      model = r->FindDict("model");
    }
    if (!model) {
      model = d.FindDict("model");
    }

    if (model) {
      base::Value::Dict box;

      // 전체 width / height
      if (auto v = model->FindInt("width"))  box.Set("width",  *v);
      if (auto v = model->FindInt("height")) box.Set("height", *v);

      // 각 영역별 쿼드(8개 좌표) → {x, y, width, height} 로 변환
      auto quad_to_rect = [](const base::Value::List* quad,
                              base::Value::Dict& out,
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
    const base::Value::Dict& d = response.GetDict();

    // node 객체: {nodeType, nodeName, localName, attributes, ...}
    const base::Value::Dict* node = nullptr;
    if (const base::Value::Dict* r = d.FindDict("result")) {
      node = r->FindDict("node");
    }
    if (!node) {
      node = d.FindDict("node");
    }

    if (node) {
      base::Value::Dict tag_info;

      if (const std::string* v = node->FindString("nodeName")) {
        // nodeName은 대문자이므로 그대로 tagName 으로 노출
        tag_info.Set("tagName", *v);
        // 최상위 결과에도 tagName 설정
        ctx->result.Set("tagName", *v);
      }
      if (const std::string* v = node->FindString("localName")) {
        tag_info.Set("localName", *v);
      }
      if (const auto v = node->FindInt("nodeType")) {
        tag_info.Set("nodeType", *v);
      }

      // attributes 배열 → id, className 추출
      // DOM.describeNode 에서 반환되는 attributes 는 같은 [k,v,...] 형식
      if (const base::Value::List* attrs = node->FindList("attributes")) {
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
    const base::Value::Dict& response_dict) {
  const base::Value::Dict* err = response_dict.FindDict("error");
  if (!err) return {};
  const std::string* msg = err->FindString("message");
  return msg ? *msg : "CDP 오류 (메시지 없음)";
}

}  // namespace mcp
