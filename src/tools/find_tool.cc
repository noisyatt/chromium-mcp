// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/find_tool.h"

#include <memory>
#include <string>
#include <utility>

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

// ============================================================
// 생성자 / 소멸자
// ============================================================

FindTool::FindTool() = default;
FindTool::~FindTool() = default;

FindTool::SearchContext::SearchContext() = default;
FindTool::SearchContext::~SearchContext() = default;

// ============================================================
// McpTool 인터페이스
// ============================================================

std::string FindTool::name() const {
  return "find";
}

std::string FindTool::description() const {
  return "페이지에서 AX Tree 또는 DOM 기반으로 요소를 검색하여 목록 반환. "
         "role/name → AX 트리, text → AX 트리, selector → CSS, xpath → XPath.";
}

base::DictValue FindTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue props;

  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "ARIA role 필터 (예: 'button', 'link', 'heading')");
    props.Set("role", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "Accessible name 필터. role과 함께 사용하면 AX 트리에서 role+name으로 검색");
    props.Set("name", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "텍스트 내용으로 검색 (AX name.value 기준). role 없이 단독 사용 가능");
    props.Set("text", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "CSS 셀렉터 (예: 'button.primary', 'input[type=email]')");
    props.Set("selector", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "XPath 표현식 (예: '//h2[@class=\"title\"]')");
    props.Set("xpath", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "boolean");
    p.Set("description", "정확 일치 여부. false이면 contains 매칭 (기본값: false)");
    p.Set("default", false);
    props.Set("exact", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "boolean");
    p.Set("description", "true: 화면에 보이는 요소만 반환, false: 숨겨진 요소도 포함");
    props.Set("visible", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "boolean");
    p.Set("description", "true: 활성화된(disabled 아닌) 요소만 반환");
    props.Set("enabled", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "number");
    p.Set("description", "최대 반환 수 (기본값: 10)");
    p.Set("default", 10);
    props.Set("limit", std::move(p));
  }

  schema.Set("properties", std::move(props));
  // 필수 파라미터 없음 — 하나 이상 제공해야 하지만 스키마 레벨에서 강제하지 않음
  return schema;
}

// ============================================================
// Execute
// ============================================================

void FindTool::Execute(const base::DictValue& arguments,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback) {
  auto ctx = std::make_shared<SearchContext>();
  ctx->session  = session;
  ctx->callback = std::move(callback);

  // 파라미터 추출
  if (const std::string* v = arguments.FindString("role"))
    ctx->role = *v;
  if (const std::string* v = arguments.FindString("name"))
    ctx->name = *v;
  if (const std::string* v = arguments.FindString("text"))
    ctx->text = *v;
  if (const std::string* v = arguments.FindString("selector"))
    ctx->selector = *v;
  if (const std::string* v = arguments.FindString("xpath"))
    ctx->xpath = *v;

  ctx->exact = arguments.FindBool("exact").value_or(false);

  if (auto v = arguments.FindBool("visible")) {
    ctx->filter_visible = true;
    ctx->visible_value  = *v;
  }
  if (auto v = arguments.FindBool("enabled")) {
    ctx->filter_enabled = true;
    ctx->enabled_value  = *v;
  }
  if (auto v = arguments.FindInt("limit")) {
    ctx->limit = (*v > 0) ? *v : 10;
  }

  // 경로 분기 (우선순위: role > text > selector > xpath)
  if (!ctx->role.empty()) {
    LOG(INFO) << "[FindTool] role 검색: role=" << ctx->role
              << " name=" << ctx->name;
    DoRoleSearch(ctx);
  } else if (!ctx->text.empty()) {
    LOG(INFO) << "[FindTool] text 검색: text=" << ctx->text;
    DoTextSearch(ctx);
  } else if (!ctx->selector.empty()) {
    LOG(INFO) << "[FindTool] selector 검색: " << ctx->selector;
    DoSelectorSearch(ctx);
  } else if (!ctx->xpath.empty()) {
    LOG(INFO) << "[FindTool] xpath 검색: " << ctx->xpath;
    DoXPathSearch(ctx);
  } else {
    std::move(ctx->callback).Run(MakeErrorResult(
        "검색 파라미터가 필요합니다 (role, text, selector, xpath 중 하나)"));
  }
}

// ============================================================
// DoRoleSearch: Accessibility.queryAXTree 또는 getFullAXTree
// ============================================================

void FindTool::DoRoleSearch(std::shared_ptr<SearchContext> ctx) {
  if (ctx->exact || ctx->name.empty()) {
    // exact 또는 name 없음 → queryAXTree (정확 매칭)
    base::DictValue params;
    params.Set("role", ctx->role);
    if (!ctx->name.empty())
      params.Set("accessibleName", ctx->name);

    ctx->session->SendCdpCommand(
        "Accessibility.queryAXTree", std::move(params),
        base::BindOnce(&FindTool::OnQueryAXTree,
                       weak_factory_.GetWeakPtr(), ctx));
  } else {
    // name이 있고 exact:false → getFullAXTree + contains 필터
    ctx->session->SendCdpCommand(
        "Accessibility.getFullAXTree", base::DictValue(),
        base::BindOnce(&FindTool::OnFullAXTree,
                       weak_factory_.GetWeakPtr(), ctx, /*is_role_search=*/true));
  }
}

// ============================================================
// DoTextSearch: Accessibility.queryAXTree(accessibleName) 또는 getFullAXTree
// ============================================================

void FindTool::DoTextSearch(std::shared_ptr<SearchContext> ctx) {
  if (ctx->exact) {
    base::DictValue params;
    params.Set("accessibleName", ctx->text);

    ctx->session->SendCdpCommand(
        "Accessibility.queryAXTree", std::move(params),
        base::BindOnce(&FindTool::OnQueryAXTree,
                       weak_factory_.GetWeakPtr(), ctx));
  } else {
    ctx->session->SendCdpCommand(
        "Accessibility.getFullAXTree", base::DictValue(),
        base::BindOnce(&FindTool::OnFullAXTree,
                       weak_factory_.GetWeakPtr(), ctx, /*is_role_search=*/false));
  }
}

// ============================================================
// OnQueryAXTree: queryAXTree 응답 → ax_entries 수집
// ============================================================

void FindTool::OnQueryAXTree(std::shared_ptr<SearchContext> ctx,
                             base::Value response) {
  if (HasCdpError(response)) {
    std::move(ctx->callback).Run(MakeErrorResult(
        "Accessibility.queryAXTree 실패: " + ExtractCdpErrorMessage(response)));
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(ctx->callback).Run(MakeErrorResult("queryAXTree 응답 형식 오류"));
    return;
  }

  const base::ListValue* nodes = nullptr;
  if (const base::DictValue* result = dict->FindDict("result"))
    nodes = result->FindList("nodes");
  if (!nodes)
    nodes = dict->FindList("nodes");

  if (!nodes || nodes->empty()) {
    // 결과 없음
    base::DictValue out;
    out.Set("total", 0);
    out.Set("items", base::ListValue());
    std::move(ctx->callback).Run(MakeJsonResult(std::move(out)));
    return;
  }

  // limit 제한을 적용하여 ax_entries 수집
  int count = 0;
  for (const auto& node_val : *nodes) {
    if (count >= ctx->limit) break;
    const base::DictValue* node = node_val.GetIfDict();
    if (!node) continue;

    std::optional<int> bnode_id = node->FindInt("backendDOMNodeId");
    if (!bnode_id.has_value() || *bnode_id <= 0) continue;

    SearchContext::AXEntry entry;
    entry.backend_node_id = *bnode_id;

    if (const base::DictValue* role_obj = node->FindDict("role")) {
      if (const std::string* v = role_obj->FindString("value"))
        entry.role = *v;
    }
    if (const base::DictValue* name_obj = node->FindDict("name")) {
      if (const std::string* v = name_obj->FindString("value"))
        entry.name = *v;
    }
    if (const base::DictValue* desc_obj = node->FindDict("description")) {
      if (const std::string* v = desc_obj->FindString("value"))
        entry.description = *v;
    }

    // disabled 속성 확인
    entry.enabled = true;
    if (const base::ListValue* props = node->FindList("properties")) {
      for (const auto& prop_val : *props) {
        const base::DictValue* prop = prop_val.GetIfDict();
        if (!prop) continue;
        const std::string* pname = prop->FindString("name");
        if (!pname || *pname != "disabled") continue;
        const base::DictValue* pval = prop->FindDict("value");
        if (!pval) continue;
        std::optional<bool> bval = pval->FindBool("value");
        if (bval.has_value() && *bval)
          entry.enabled = false;
        break;
      }
    }

    ctx->ax_entries.push_back(std::move(entry));
    ++count;
  }

  if (ctx->ax_entries.empty()) {
    base::DictValue out;
    out.Set("total", 0);
    out.Set("items", base::ListValue());
    std::move(ctx->callback).Run(MakeJsonResult(std::move(out)));
    return;
  }

  ResolveAXEntries(ctx);
}

// ============================================================
// OnFullAXTree: getFullAXTree → contains 필터
// ============================================================

void FindTool::OnFullAXTree(std::shared_ptr<SearchContext> ctx,
                            bool is_role_search,
                            base::Value response) {
  if (HasCdpError(response)) {
    std::move(ctx->callback).Run(MakeErrorResult(
        "Accessibility.getFullAXTree 실패: " +
        ExtractCdpErrorMessage(response)));
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(ctx->callback).Run(MakeErrorResult("getFullAXTree 응답 형식 오류"));
    return;
  }

  const base::ListValue* nodes = nullptr;
  if (const base::DictValue* result = dict->FindDict("result"))
    nodes = result->FindList("nodes");
  if (!nodes)
    nodes = dict->FindList("nodes");

  if (!nodes || nodes->empty()) {
    base::DictValue out;
    out.Set("total", 0);
    out.Set("items", base::ListValue());
    std::move(ctx->callback).Run(MakeJsonResult(std::move(out)));
    return;
  }

  // 소문자 변환 (contains 비교용)
  std::string role_lower = base::ToLowerASCII(ctx->role);
  std::string match_lower = base::ToLowerASCII(
      is_role_search ? ctx->name : ctx->text);

  int count = 0;
  for (const auto& node_val : *nodes) {
    if (count >= ctx->limit) break;
    const base::DictValue* node = node_val.GetIfDict();
    if (!node) continue;

    // role 매칭 (is_role_search 시 정확 일치)
    std::string node_role;
    if (const base::DictValue* role_obj = node->FindDict("role")) {
      if (const std::string* v = role_obj->FindString("value"))
        node_role = *v;
    }
    if (is_role_search) {
      std::string node_role_lower = base::ToLowerASCII(node_role);
      if (node_role_lower != role_lower) continue;
    }

    // name contains 매칭
    std::string node_name;
    if (const base::DictValue* name_obj = node->FindDict("name")) {
      if (const std::string* v = name_obj->FindString("value"))
        node_name = *v;
    }
    if (!match_lower.empty()) {
      std::string node_name_lower = base::ToLowerASCII(node_name);
      if (node_name_lower.find(match_lower) == std::string::npos) continue;
    }

    std::optional<int> bnode_id = node->FindInt("backendDOMNodeId");
    if (!bnode_id.has_value() || *bnode_id <= 0) continue;

    SearchContext::AXEntry entry;
    entry.backend_node_id = *bnode_id;
    entry.role = node_role;
    entry.name = node_name;

    if (const base::DictValue* desc_obj = node->FindDict("description")) {
      if (const std::string* v = desc_obj->FindString("value"))
        entry.description = *v;
    }

    entry.enabled = true;
    if (const base::ListValue* props = node->FindList("properties")) {
      for (const auto& prop_val : *props) {
        const base::DictValue* prop = prop_val.GetIfDict();
        if (!prop) continue;
        const std::string* pname = prop->FindString("name");
        if (!pname || *pname != "disabled") continue;
        const base::DictValue* pval = prop->FindDict("value");
        if (!pval) continue;
        std::optional<bool> bval = pval->FindBool("value");
        if (bval.has_value() && *bval)
          entry.enabled = false;
        break;
      }
    }

    ctx->ax_entries.push_back(std::move(entry));
    ++count;
  }

  if (ctx->ax_entries.empty()) {
    base::DictValue out;
    out.Set("total", 0);
    out.Set("items", base::ListValue());
    std::move(ctx->callback).Run(MakeJsonResult(std::move(out)));
    return;
  }

  ResolveAXEntries(ctx);
}

// ============================================================
// ResolveAXEntries: 각 AXEntry에 대해 DOM.describeNode 요청 (tag/attributes 획득)
// ============================================================

void FindTool::ResolveAXEntries(std::shared_ptr<SearchContext> ctx) {
  size_t n = ctx->ax_entries.size();

  // items 슬롯 예약
  ctx->items.reserve(n);
  for (size_t i = 0; i < n; ++i)
    ctx->items.Append(base::DictValue());

  ctx->pending = static_cast<int>(n);

  for (size_t i = 0; i < n; ++i) {
    base::DictValue params;
    params.Set("backendNodeId", ctx->ax_entries[i].backend_node_id);
    params.Set("depth", 0);

    ctx->session->SendCdpCommand(
        "DOM.describeNode", std::move(params),
        base::BindOnce(&FindTool::OnAXDescribeNode,
                       weak_factory_.GetWeakPtr(), ctx, i));
  }
}

// ============================================================
// OnAXDescribeNode: AX 경로 DOM.describeNode 응답 → tag/attributes 추출 → getBoxModel
// ============================================================

void FindTool::OnAXDescribeNode(std::shared_ptr<SearchContext> ctx,
                                size_t index,
                                base::Value response) {
  std::string tag;
  base::DictValue attributes;

  if (!HasCdpError(response)) {
    const base::DictValue* dict = response.GetIfDict();
    const base::DictValue* node = nullptr;
    if (dict) {
      if (const base::DictValue* r = dict->FindDict("result"))
        node = r->FindDict("node");
      if (!node)
        node = dict->FindDict("node");
    }

    if (node) {
      if (const std::string* v = node->FindString("localName"))
        tag = *v;
      else if (const std::string* v2 = node->FindString("nodeName"))
        tag = base::ToLowerASCII(*v2);

      // HTML 속성 파싱
      if (const base::ListValue* attrs = node->FindList("attributes")) {
        for (size_t ai = 0; ai + 1 < attrs->size(); ai += 2) {
          const auto& k = (*attrs)[ai];
          const auto& v = (*attrs)[ai + 1];
          if (k.is_string() && v.is_string())
            attributes.Set(k.GetString(), v.GetString());
        }
      }
    }
  }

  // DOM.getBoxModel로 bounding box 획득
  base::DictValue params;
  params.Set("backendNodeId", ctx->ax_entries[index].backend_node_id);

  ctx->session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(&FindTool::OnAXBoxModel,
                     weak_factory_.GetWeakPtr(), ctx, index,
                     std::move(tag), std::move(attributes)));
}

// ============================================================
// OnAXBoxModel: AX 경로 getBoxModel 응답 → 항목 빌드
// ============================================================

void FindTool::OnAXBoxModel(std::shared_ptr<SearchContext> ctx,
                            size_t index,
                            std::string tag,
                            base::DictValue attributes,
                            base::Value response) {
  const SearchContext::AXEntry& entry = ctx->ax_entries[index];

  // enabled 필터
  if (ctx->filter_enabled && entry.enabled != ctx->enabled_value) {
    --ctx->pending;
    MaybeFinalize(ctx);
    return;
  }

  base::DictValue item;
  item.Set("index", static_cast<int>(index));
  item.Set("backendNodeId", entry.backend_node_id);
  item.Set("role", entry.role);
  item.Set("name", entry.name);
  item.Set("description", entry.description);
  item.Set("enabled", entry.enabled);

  bool visible = false;
  base::DictValue bbox;

  if (!HasCdpError(response)) {
    if (ExtractBoundingBox(response, &bbox)) {
      visible = true;
    }
  }

  // visible 필터
  if (ctx->filter_visible && visible != ctx->visible_value) {
    --ctx->pending;
    MaybeFinalize(ctx);
    return;
  }

  item.Set("visible", visible);
  if (visible) {
    item.Set("boundingBox", std::move(bbox));
  } else {
    item.Set("boundingBox", base::DictValue());
  }

  item.Set("tag", std::move(tag));
  item.Set("attributes", std::move(attributes));

  if (index < ctx->items.size())
    ctx->items[index] = base::Value(std::move(item));

  --ctx->pending;
  MaybeFinalize(ctx);
}

// ============================================================
// DoSelectorSearch: DOM.getDocument → DOM.querySelectorAll
// ============================================================

void FindTool::DoSelectorSearch(std::shared_ptr<SearchContext> ctx) {
  base::DictValue params;
  params.Set("depth", 0);
  params.Set("pierce", false);

  ctx->session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&FindTool::OnGetDocumentForSelector,
                     weak_factory_.GetWeakPtr(), ctx));
}

void FindTool::OnGetDocumentForSelector(std::shared_ptr<SearchContext> ctx,
                                        base::Value response) {
  if (HasCdpError(response)) {
    std::move(ctx->callback).Run(MakeErrorResult(
        "DOM.getDocument 실패: " + ExtractCdpErrorMessage(response)));
    return;
  }

  int root_id = ExtractRootNodeId(response);
  if (root_id <= 0) {
    std::move(ctx->callback).Run(
        MakeErrorResult("DOM.getDocument: 루트 nodeId 추출 실패"));
    return;
  }

  base::DictValue params;
  params.Set("nodeId",   root_id);
  params.Set("selector", ctx->selector);

  ctx->session->SendCdpCommand(
      "DOM.querySelectorAll", std::move(params),
      base::BindOnce(&FindTool::OnQuerySelectorAll,
                     weak_factory_.GetWeakPtr(), ctx));
}

void FindTool::OnQuerySelectorAll(std::shared_ptr<SearchContext> ctx,
                                  base::Value response) {
  if (HasCdpError(response)) {
    std::move(ctx->callback).Run(MakeErrorResult(
        "DOM.querySelectorAll 실패: " + ExtractCdpErrorMessage(response)));
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(ctx->callback).Run(
        MakeErrorResult("DOM.querySelectorAll 응답 형식 오류"));
    return;
  }

  const base::ListValue* ids = nullptr;
  if (const base::DictValue* r = dict->FindDict("result"))
    ids = r->FindList("nodeIds");
  if (!ids)
    ids = dict->FindList("nodeIds");

  if (!ids || ids->empty()) {
    base::DictValue out;
    out.Set("total", 0);
    out.Set("items", base::ListValue());
    std::move(ctx->callback).Run(MakeJsonResult(std::move(out)));
    return;
  }

  ctx->result_count = static_cast<int>(ids->size());
  int added = 0;
  for (const auto& id_val : *ids) {
    if (!id_val.is_int() || added >= ctx->limit) break;
    ctx->node_ids.push_back(id_val.GetInt());
    ++added;
  }

  DescribeDomNodes(ctx);
}

// ============================================================
// DoXPathSearch: DOM.performSearch → getSearchResults → discardSearchResults
// ============================================================

void FindTool::DoXPathSearch(std::shared_ptr<SearchContext> ctx) {
  base::DictValue params;
  params.Set("query", ctx->xpath);
  params.Set("includeUserAgentShadowDOM", false);

  ctx->session->SendCdpCommand(
      "DOM.performSearch", std::move(params),
      base::BindOnce(&FindTool::OnPerformSearch,
                     weak_factory_.GetWeakPtr(), ctx));
}

void FindTool::OnPerformSearch(std::shared_ptr<SearchContext> ctx,
                               base::Value response) {
  if (HasCdpError(response)) {
    std::move(ctx->callback).Run(MakeErrorResult(
        "DOM.performSearch 실패: " + ExtractCdpErrorMessage(response)));
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(ctx->callback).Run(
        MakeErrorResult("DOM.performSearch 응답 형식 오류"));
    return;
  }

  const base::DictValue* data = dict;
  if (const base::DictValue* r = dict->FindDict("result"))
    data = r;

  const std::string* sid  = data->FindString("searchId");
  std::optional<int>  cnt = data->FindInt("resultCount");

  if (!sid || !cnt) {
    std::move(ctx->callback).Run(
        MakeErrorResult("DOM.performSearch: searchId 또는 resultCount 없음"));
    return;
  }

  ctx->search_id    = *sid;
  ctx->result_count = *cnt;

  if (*cnt == 0) {
    // 결과 없음 — 세션 정리
    base::DictValue discard_params;
    discard_params.Set("searchId", ctx->search_id);
    ctx->session->SendCdpCommand("DOM.discardSearchResults",
                                 std::move(discard_params),
                                 base::BindOnce([](base::Value) {}));
    base::DictValue out;
    out.Set("total", 0);
    out.Set("items", base::ListValue());
    std::move(ctx->callback).Run(MakeJsonResult(std::move(out)));
    return;
  }

  int fetch_count = std::min(ctx->result_count, ctx->limit);

  base::DictValue params;
  params.Set("searchId",  ctx->search_id);
  params.Set("fromIndex", 0);
  params.Set("toIndex",   fetch_count);

  ctx->session->SendCdpCommand(
      "DOM.getSearchResults", std::move(params),
      base::BindOnce(&FindTool::OnGetSearchResults,
                     weak_factory_.GetWeakPtr(), ctx));
}

void FindTool::OnGetSearchResults(std::shared_ptr<SearchContext> ctx,
                                  base::Value response) {
  // 검색 세션 반드시 정리 (누수 방지)
  {
    base::DictValue discard_params;
    discard_params.Set("searchId", ctx->search_id);
    ctx->session->SendCdpCommand("DOM.discardSearchResults",
                                 std::move(discard_params),
                                 base::BindOnce([](base::Value) {}));
  }

  if (HasCdpError(response)) {
    std::move(ctx->callback).Run(MakeErrorResult(
        "DOM.getSearchResults 실패: " + ExtractCdpErrorMessage(response)));
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(ctx->callback).Run(
        MakeErrorResult("DOM.getSearchResults 응답 형식 오류"));
    return;
  }

  const base::ListValue* ids = nullptr;
  if (const base::DictValue* r = dict->FindDict("result"))
    ids = r->FindList("nodeIds");
  if (!ids)
    ids = dict->FindList("nodeIds");

  if (!ids || ids->empty()) {
    base::DictValue out;
    out.Set("total", ctx->result_count);
    out.Set("items", base::ListValue());
    std::move(ctx->callback).Run(MakeJsonResult(std::move(out)));
    return;
  }

  for (const auto& id_val : *ids) {
    if (id_val.is_int())
      ctx->node_ids.push_back(id_val.GetInt());
  }

  DescribeDomNodes(ctx);
}

// ============================================================
// DescribeDomNodes: 각 nodeId에 DOM.describeNode 요청
// ============================================================

void FindTool::DescribeDomNodes(std::shared_ptr<SearchContext> ctx) {
  if (ctx->node_ids.empty()) {
    base::DictValue out;
    out.Set("total", ctx->result_count);
    out.Set("items", base::ListValue());
    std::move(ctx->callback).Run(MakeJsonResult(std::move(out)));
    return;
  }

  size_t n = ctx->node_ids.size();

  // items 슬롯 예약
  ctx->items.reserve(n);
  for (size_t i = 0; i < n; ++i)
    ctx->items.Append(base::DictValue());

  ctx->pending = static_cast<int>(n);

  for (size_t i = 0; i < n; ++i) {
    base::DictValue params;
    params.Set("nodeId", ctx->node_ids[i]);
    params.Set("depth",  0);

    ctx->session->SendCdpCommand(
        "DOM.describeNode", std::move(params),
        base::BindOnce(&FindTool::OnDescribeDomNode,
                       weak_factory_.GetWeakPtr(), ctx, i));
  }
}

// ============================================================
// OnDescribeDomNode: describeNode 응답 → tag/attributes 추출 → getBoxModel
// ============================================================

void FindTool::OnDescribeDomNode(std::shared_ptr<SearchContext> ctx,
                                 size_t index,
                                 base::Value response) {
  std::string tag;
  base::DictValue attributes;
  int backend_node_id = 0;

  if (!HasCdpError(response)) {
    const base::DictValue* dict = response.GetIfDict();
    const base::DictValue* node = nullptr;
    if (dict) {
      if (const base::DictValue* r = dict->FindDict("result"))
        node = r->FindDict("node");
      if (!node)
        node = dict->FindDict("node");
    }

    if (node) {
      if (const std::string* v = node->FindString("localName"))
        tag = *v;
      else if (const std::string* v2 = node->FindString("nodeName"))
        tag = base::ToLowerASCII(*v2);

      if (auto bni = node->FindInt("backendNodeId"))
        backend_node_id = *bni;

      // HTML 속성 파싱
      if (const base::ListValue* attrs = node->FindList("attributes")) {
        for (size_t ai = 0; ai + 1 < attrs->size(); ai += 2) {
          const auto& k = (*attrs)[ai];
          const auto& v = (*attrs)[ai + 1];
          if (k.is_string() && v.is_string())
            attributes.Set(k.GetString(), v.GetString());
        }
      }
    }
  }

  if (backend_node_id <= 0) {
    // describeNode 실패 — 빈 항목으로 처리
    --ctx->pending;
    MaybeFinalize(ctx);
    return;
  }

  base::DictValue params;
  params.Set("backendNodeId", backend_node_id);

  ctx->session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(&FindTool::OnDomBoxModel,
                     weak_factory_.GetWeakPtr(), ctx, index,
                     backend_node_id, std::move(tag), std::move(attributes)));
}

// ============================================================
// OnDomBoxModel: DOM 경로 getBoxModel 응답 → 항목 빌드
// ============================================================

void FindTool::OnDomBoxModel(std::shared_ptr<SearchContext> ctx,
                             size_t index,
                             int backend_node_id,
                             std::string tag,
                             base::DictValue attributes,
                             base::Value response) {
  bool visible = false;
  base::DictValue bbox;

  if (!HasCdpError(response)) {
    if (ExtractBoundingBox(response, &bbox))
      visible = true;
  }

  // visible 필터
  if (ctx->filter_visible && visible != ctx->visible_value) {
    --ctx->pending;
    MaybeFinalize(ctx);
    return;
  }

  // enabled 필터 — DOM 경로에서는 별도 조회 없이 항상 true로 간주
  // (selector/xpath 검색 특성상 enabled 정보 없음)
  bool enabled = true;
  if (ctx->filter_enabled && enabled != ctx->enabled_value) {
    --ctx->pending;
    MaybeFinalize(ctx);
    return;
  }

  base::DictValue item;
  item.Set("index",         static_cast<int>(index));
  item.Set("backendNodeId", backend_node_id);
  item.Set("tag",           tag);
  item.Set("role",          "");
  item.Set("name",          "");
  item.Set("description",   "");
  item.Set("visible",       visible);
  item.Set("enabled",       enabled);
  if (visible) {
    item.Set("boundingBox", std::move(bbox));
  } else {
    item.Set("boundingBox", base::DictValue());
  }
  item.Set("attributes", std::move(attributes));

  if (index < ctx->items.size())
    ctx->items[index] = base::Value(std::move(item));

  --ctx->pending;
  MaybeFinalize(ctx);
}

// ============================================================
// MaybeFinalize: 모든 비동기 작업 완료 시 콜백 호출
// ============================================================

void FindTool::MaybeFinalize(std::shared_ptr<SearchContext> ctx) {
  if (ctx->pending > 0 || ctx->finalized) return;
  ctx->finalized = true;

  // 빈 슬롯(필터로 제외된 항목) 제거 및 index 재부여
  base::ListValue result_items;
  int out_index = 0;
  for (auto& item_val : ctx->items) {
    if (!item_val.is_dict()) continue;
    base::DictValue& d = item_val.GetDict();
    // 빈 DictValue (필터로 제외된 슬롯)인지 확인:
    // 실제 항목이면 "backendNodeId" int 키가 있음.
    if (!d.FindInt("backendNodeId")) continue;
    d.Set("index", out_index++);
    result_items.Append(std::move(item_val));
  }

  int total = static_cast<int>(result_items.size());

  LOG(INFO) << "[FindTool] 검색 완료: 결과=" << total;

  base::DictValue out;
  out.Set("total", total);
  out.Set("items", std::move(result_items));

  std::move(ctx->callback).Run(MakeJsonResult(std::move(out)));
}

}  // namespace mcp
