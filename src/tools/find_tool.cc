// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/find_tool.h"

#include <memory>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

FindTool::FindTool() = default;
FindTool::~FindTool() = default;

FindTool::SearchContext::SearchContext() = default;
FindTool::SearchContext::~SearchContext() = default;

std::string FindTool::name() const {
  return "find";
}

std::string FindTool::description() const {
  return "페이지에서 텍스트 또는 CSS 선택자로 요소 검색";
}

base::DictValue FindTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue props;

  // type: 검색 유형 (필수)
  {
    base::DictValue p;
    p.Set("type", "string");
    base::ListValue e;
    e.Append("text");
    e.Append("selector");
    e.Append("xpath");
    p.Set("enum", std::move(e));
    p.Set("description",
          "검색 유형:\n"
          "  text    : 페이지 텍스트 내 문자열 검색 (DOM.performSearch)\n"
          "  selector: CSS 선택자로 요소 검색 (DOM.querySelectorAll)\n"
          "  xpath   : XPath 표현식으로 요소 검색 (DOM.performSearch)");
    props.Set("type", std::move(p));
  }

  // query: 검색어 (필수)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description",
          "검색어. type 에 따라 의미가 다름:\n"
          "  text     → 검색할 텍스트 문자열\n"
          "  selector → CSS 선택자 (예: 'button.primary')\n"
          "  xpath    → XPath 표현식 (예: '//h2[@class=\"title\"]')");
    props.Set("query", std::move(p));
  }

  // limit: 최대 결과 수 (기본 10)
  {
    base::DictValue p;
    p.Set("type", "number");
    p.Set("description", "반환할 최대 결과 수 (기본값: 10)");
    p.Set("default", 10);
    props.Set("limit", std::move(p));
  }

  // includeText: 요소의 innerText 포함 여부 (기본 true)
  {
    base::DictValue p;
    p.Set("type", "boolean");
    p.Set("description",
          "각 결과 항목에 innerText 를 포함할지 여부 (기본값: true)");
    p.Set("default", true);
    props.Set("includeText", std::move(p));
  }

  // includeAttributes: HTML 속성 포함 여부 (기본 false)
  {
    base::DictValue p;
    p.Set("type", "boolean");
    p.Set("description",
          "각 결과 항목에 HTML 속성 목록을 포함할지 여부 (기본값: false)");
    p.Set("default", false);
    props.Set("includeAttributes", std::move(p));
  }

  schema.Set("properties", std::move(props));

  // type, query 는 필수
  base::ListValue required;
  required.Append("type");
  required.Append("query");
  schema.Set("required", std::move(required));

  return schema;
}

// -----------------------------------------------------------------------
// Execute
// -----------------------------------------------------------------------
void FindTool::Execute(const base::DictValue& arguments,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback) {
  const std::string* type_ptr = arguments.FindString("type");
  const std::string* query_ptr = arguments.FindString("query");

  if (!type_ptr || type_ptr->empty()) {
    base::DictValue err;
    err.Set("error", "type 파라미터가 필요합니다 (text/selector/xpath)");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }
  if (!query_ptr || query_ptr->empty()) {
    base::DictValue err;
    err.Set("error", "query 파라미터가 필요합니다");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  if (*type_ptr != "text" && *type_ptr != "selector" && *type_ptr != "xpath") {
    base::DictValue err;
    err.Set("error",
            "type 은 'text', 'selector', 'xpath' 중 하나여야 합니다");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  auto ctx = std::make_shared<SearchContext>();
  ctx->type    = *type_ptr;
  ctx->query   = *query_ptr;
  ctx->session = session;
  ctx->callback = std::move(callback);

  // 선택적 파라미터
  if (auto v = arguments.FindInt("limit")) {
    ctx->limit = (*v > 0) ? *v : 10;
  }
  if (auto v = arguments.FindBool("includeText")) {
    ctx->include_text = *v;
  } else {
    ctx->include_text = true;  // 기본값
  }
  if (auto v = arguments.FindBool("includeAttributes")) {
    ctx->include_attributes = *v;
  }

  LOG(INFO) << "[FindTool] 검색 시작 type=" << ctx->type
            << " query=" << ctx->query;

  if (ctx->type == "selector") {
    DoSelectorSearch(ctx);
  } else {
    // text 또는 xpath → DOM.performSearch
    DoPerformSearch(ctx);
  }
}

// -----------------------------------------------------------------------
// text / xpath: DOM.performSearch
// -----------------------------------------------------------------------
void FindTool::DoPerformSearch(std::shared_ptr<SearchContext> ctx) {
  base::DictValue params;
  params.Set("query", ctx->query);
  // includeUserAgentShadowDOM: 기본 false
  params.Set("includeUserAgentShadowDOM", false);

  ctx->session->SendCdpCommand(
      "DOM.performSearch", std::move(params),
      base::BindOnce(&FindTool::OnPerformSearch,
                     weak_factory_.GetWeakPtr(), ctx));
}

// -----------------------------------------------------------------------
// selector: DOM.getDocument → DOM.querySelectorAll
// -----------------------------------------------------------------------
void FindTool::DoSelectorSearch(std::shared_ptr<SearchContext> ctx) {
  base::DictValue params;
  params.Set("depth", 0);
  params.Set("pierce", false);

  ctx->session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&FindTool::OnGetDocumentForSelector,
                     weak_factory_.GetWeakPtr(), ctx));
}

// -----------------------------------------------------------------------
// DOM.performSearch 응답
// -----------------------------------------------------------------------
void FindTool::OnPerformSearch(std::shared_ptr<SearchContext> ctx,
                                base::Value response) {
  if (!response.is_dict()) {
    base::DictValue err;
    err.Set("error", "DOM.performSearch: 응답 형식 오류");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::DictValue& d = response.GetDict();
  std::string cdp_err = ExtractCdpError(d);
  if (!cdp_err.empty()) {
    base::DictValue err;
    err.Set("error", "DOM.performSearch 실패: " + cdp_err);
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // searchId, resultCount 추출
  const base::DictValue* r = d.FindDict("result");
  const base::DictValue* data = r ? r : &d;

  const std::string* sid = data->FindString("searchId");
  std::optional<int> cnt = data->FindInt("resultCount");

  if (!sid || !cnt) {
    base::DictValue err;
    err.Set("error", "DOM.performSearch: searchId 또는 resultCount 없음");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  ctx->search_id    = *sid;
  ctx->result_count = *cnt;

  LOG(INFO) << "[FindTool] performSearch 완료 searchId=" << ctx->search_id
            << " resultCount=" << ctx->result_count;

  if (ctx->result_count == 0) {
    // 결과 없음
    base::DictValue out;
    out.Set("success", true);
    out.Set("query",   ctx->query);
    out.Set("type",    ctx->type);
    out.Set("total",   0);
    out.Set("items",   base::ListValue());
    std::move(ctx->callback).Run(base::Value(std::move(out)));
    return;
  }

  // 실제 가져올 결과 수는 limit 와 resultCount 중 작은 값
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

// -----------------------------------------------------------------------
// DOM.getSearchResults 응답
// -----------------------------------------------------------------------
void FindTool::OnGetSearchResults(std::shared_ptr<SearchContext> ctx,
                                   base::Value response) {
  if (!response.is_dict()) {
    base::DictValue err;
    err.Set("error", "DOM.getSearchResults: 응답 형식 오류");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::DictValue& d = response.GetDict();
  std::string cdp_err = ExtractCdpError(d);
  if (!cdp_err.empty()) {
    base::DictValue err;
    err.Set("error", "DOM.getSearchResults 실패: " + cdp_err);
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // nodeIds 배열 추출
  const base::ListValue* ids = nullptr;
  if (const base::DictValue* r = d.FindDict("result")) {
    ids = r->FindList("nodeIds");
  }
  if (!ids) {
    ids = d.FindList("nodeIds");
  }

  if (!ids || ids->empty()) {
    base::DictValue out;
    out.Set("success", true);
    out.Set("query",   ctx->query);
    out.Set("type",    ctx->type);
    out.Set("total",   ctx->result_count);
    out.Set("items",   base::ListValue());
    std::move(ctx->callback).Run(base::Value(std::move(out)));
    return;
  }

  for (const auto& id_val : *ids) {
    if (id_val.is_int()) {
      ctx->node_ids.push_back(id_val.GetInt());
    }
  }

  DescribeNodes(ctx);
}

// -----------------------------------------------------------------------
// DOM.getDocument 응답 (selector 검색용)
// -----------------------------------------------------------------------
void FindTool::OnGetDocumentForSelector(std::shared_ptr<SearchContext> ctx,
                                         base::Value response) {
  if (!response.is_dict()) {
    base::DictValue err;
    err.Set("error", "DOM.getDocument: 응답 형식 오류");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::DictValue& d = response.GetDict();
  std::string cdp_err = ExtractCdpError(d);
  if (!cdp_err.empty()) {
    base::DictValue err;
    err.Set("error", "DOM.getDocument 실패: " + cdp_err);
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // 루트 nodeId 추출
  const base::DictValue* root = nullptr;
  if (const base::DictValue* r = d.FindDict("result")) {
    root = r->FindDict("root");
  }
  if (!root) {
    root = d.FindDict("root");
  }

  std::optional<int> root_id = root ? root->FindInt("nodeId") : std::nullopt;
  if (!root_id) {
    base::DictValue err;
    err.Set("error", "DOM.getDocument: 루트 nodeId 추출 실패");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  base::DictValue params;
  params.Set("nodeId",   *root_id);
  params.Set("selector", ctx->query);

  ctx->session->SendCdpCommand(
      "DOM.querySelectorAll", std::move(params),
      base::BindOnce(&FindTool::OnQuerySelectorAll,
                     weak_factory_.GetWeakPtr(), ctx));
}

// -----------------------------------------------------------------------
// DOM.querySelectorAll 응답
// -----------------------------------------------------------------------
void FindTool::OnQuerySelectorAll(std::shared_ptr<SearchContext> ctx,
                                   base::Value response) {
  if (!response.is_dict()) {
    base::DictValue err;
    err.Set("error", "DOM.querySelectorAll: 응답 형식 오류");
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::DictValue& d = response.GetDict();
  std::string cdp_err = ExtractCdpError(d);
  if (!cdp_err.empty()) {
    base::DictValue err;
    err.Set("error", "DOM.querySelectorAll 실패: " + cdp_err);
    std::move(ctx->callback).Run(base::Value(std::move(err)));
    return;
  }

  // nodeIds 배열 추출
  const base::ListValue* ids = nullptr;
  if (const base::DictValue* r = d.FindDict("result")) {
    ids = r->FindList("nodeIds");
  }
  if (!ids) {
    ids = d.FindList("nodeIds");
  }

  if (!ids || ids->empty()) {
    base::DictValue out;
    out.Set("success", true);
    out.Set("query",   ctx->query);
    out.Set("type",    ctx->type);
    out.Set("total",   0);
    out.Set("items",   base::ListValue());
    std::move(ctx->callback).Run(base::Value(std::move(out)));
    return;
  }

  int added = 0;
  for (const auto& id_val : *ids) {
    if (id_val.is_int() && added < ctx->limit) {
      ctx->node_ids.push_back(id_val.GetInt());
      ++added;
    }
  }

  ctx->result_count = static_cast<int>(ids->size());
  DescribeNodes(ctx);
}

// -----------------------------------------------------------------------
// 각 nodeId 에 대해 DOM.describeNode 요청
// -----------------------------------------------------------------------
void FindTool::DescribeNodes(std::shared_ptr<SearchContext> ctx) {
  if (ctx->node_ids.empty()) {
    base::DictValue out;
    out.Set("success", true);
    out.Set("query",   ctx->query);
    out.Set("type",    ctx->type);
    out.Set("total",   ctx->result_count);
    out.Set("items",   base::ListValue());
    std::move(ctx->callback).Run(base::Value(std::move(out)));
    return;
  }

  // 결과 배열을 미리 nodeId 수만큼 예약 (순서 보장을 위해 인덱스 사용)
  ctx->items.reserve(ctx->node_ids.size());
  for (size_t i = 0; i < ctx->node_ids.size(); ++i) {
    ctx->items.Append(base::DictValue());  // 빈 슬롯
  }

  ctx->pending_describe = static_cast<int>(ctx->node_ids.size());

  for (size_t i = 0; i < ctx->node_ids.size(); ++i) {
    base::DictValue params;
    params.Set("nodeId", ctx->node_ids[i]);
    params.Set("depth",  0);

    ctx->session->SendCdpCommand(
        "DOM.describeNode", std::move(params),
        base::BindOnce(&FindTool::OnDescribeNode,
                       weak_factory_.GetWeakPtr(), ctx, i));
  }
}

// -----------------------------------------------------------------------
// DOM.describeNode 응답 (인덱스 i)
// -----------------------------------------------------------------------
void FindTool::OnDescribeNode(std::shared_ptr<SearchContext> ctx,
                               size_t index,
                               base::Value response) {
  base::DictValue item;
  item.Set("nodeId", ctx->node_ids[index]);

  if (response.is_dict()) {
    const base::DictValue& d = response.GetDict();

    // node 객체 추출
    const base::DictValue* node = nullptr;
    if (const base::DictValue* r = d.FindDict("result")) {
      node = r->FindDict("node");
    }
    if (!node) {
      node = d.FindDict("node");
    }

    if (node) {
      // tagName (nodeName 을 소문자로)
      if (const std::string* v = node->FindString("nodeName")) {
        item.Set("tagName", *v);
      }
      if (const std::string* v = node->FindString("localName")) {
        item.Set("localName", *v);
      }

      // HTML 속성 파싱 (includeAttributes 또는 id/className 추출 목적)
      if (const base::ListValue* attrs = node->FindList("attributes")) {
        // id, className 은 항상 추출 (식별 목적)
        std::string id_val, class_val;
        base::DictValue attrs_dict;

        for (size_t i = 0; i + 1 < attrs->size(); i += 2) {
          const auto& k = (*attrs)[i];
          const auto& v = (*attrs)[i + 1];
          if (!k.is_string() || !v.is_string()) continue;
          const std::string& key = k.GetString();
          const std::string& val = v.GetString();
          if (key == "id")    id_val    = val;
          if (key == "class") class_val = val;
          if (ctx->include_attributes) {
            attrs_dict.Set(key, val);
          }
        }

        if (!id_val.empty())    item.Set("id",        id_val);
        if (!class_val.empty()) item.Set("className", class_val);
        if (ctx->include_attributes) {
          item.Set("attributes", std::move(attrs_dict));
        }
      }

      // innerText: Runtime.evaluate 를 추가 호출하지 않고
      // describeNode 에 포함되지 않으므로 nodeValue 로 대체 시도
      // (실제 텍스트 노드가 아닌 한 비어 있음; 텍스트는 별도 수집 불필요)
      // includeText=true 이더라도 describeNode 에서는 직접 얻을 수 없으므로
      // nodeValue 가 있으면 포함한다.
      if (ctx->include_text) {
        if (const std::string* v = node->FindString("nodeValue")) {
          if (!v->empty()) {
            item.Set("text", *v);
          }
        }
        // textContent 는 DOM 트리를 탐색해야 하므로 여기서는 생략.
        // 필요하다면 Runtime.evaluate 로 추가 조회 가능하다.
      }
    }
  }

  // 결과 슬롯에 저장
  if (index < ctx->items.size()) {
    ctx->items[index] = base::Value(std::move(item));
  }

  // 완료 카운터 감소
  --ctx->pending_describe;
  if (ctx->pending_describe > 0 || ctx->finalized) return;

  ctx->finalized = true;

  base::DictValue out;
  out.Set("success", true);
  out.Set("query",   ctx->query);
  out.Set("type",    ctx->type);
  out.Set("total",   ctx->result_count);
  out.Set("count",   static_cast<int>(ctx->node_ids.size()));
  out.Set("items",   std::move(ctx->items));

  LOG(INFO) << "[FindTool] 검색 완료 type=" << ctx->type
            << " 결과=" << ctx->node_ids.size();

  std::move(ctx->callback).Run(base::Value(std::move(out)));
}

// -----------------------------------------------------------------------
// 정적 헬퍼
// -----------------------------------------------------------------------
// static
std::string FindTool::ExtractCdpError(const base::DictValue& d) {
  const base::DictValue* err = d.FindDict("error");
  if (!err) return {};
  const std::string* msg = err->FindString("message");
  return msg ? *msg : "CDP 오류 (메시지 없음)";
}

}  // namespace mcp
