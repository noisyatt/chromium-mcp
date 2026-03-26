// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_FIND_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_FIND_TOOL_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 페이지에서 AX Tree 또는 DOM 기반으로 요소를 검색하고 목록을 반환하는 도구.
//
// 검색 경로 (파라미터 우선순위):
//   1. role/name  → Accessibility.queryAXTree 또는 getFullAXTree
//   2. text       → Accessibility.queryAXTree / getFullAXTree
//   3. selector   → DOM.querySelectorAll
//   4. xpath      → DOM.performSearch + DOM.discardSearchResults
//
// 각 결과 항목: index, backendNodeId, tag, role, name, description,
//               visible, enabled, boundingBox, attributes
class FindTool : public McpTool {
 public:
  FindTool();
  ~FindTool() override;

  // McpTool 인터페이스
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // -----------------------------------------------------------------------
  // 한 번의 Execute 호출에 필요한 모든 상태
  // -----------------------------------------------------------------------
  struct SearchContext {
    SearchContext();
    ~SearchContext();

    // 입력 파라미터
    std::string role;
    std::string name;
    std::string text;
    std::string selector;
    std::string xpath;
    bool exact = false;
    bool filter_visible = false;   // visible 필터 사용 여부
    bool visible_value = true;     // filter_visible=true 시 원하는 값
    bool filter_enabled = false;   // enabled 필터 사용 여부
    bool enabled_value = true;     // filter_enabled=true 시 원하는 값
    int limit = 10;

    // 중간 상태 — selector/xpath 경로
    std::string search_id;         // DOM.performSearch searchId (xpath용)
    int result_count = 0;
    std::vector<int> node_ids;     // DOM nodeId 목록 (selector/xpath)

    // 중간 상태 — role/name/text 경로
    // AX 노드 목록: 각 항목은 {backendDOMNodeId, role, name, description, enabled}를 담음
    struct AXEntry {
      AXEntry();
      ~AXEntry();
      AXEntry(const AXEntry&);
      AXEntry& operator=(const AXEntry&);
      AXEntry(AXEntry&&);
      AXEntry& operator=(AXEntry&&);
      int backend_node_id = 0;
      std::string role;
      std::string name;
      std::string description;
      bool enabled = true;
    };
    std::vector<AXEntry> ax_entries;

    // 수집 중인 최종 결과 (순서 보장을 위해 인덱스 기반 예약)
    base::ListValue items;

    // 비동기 완료 추적
    int pending = 0;
    bool finalized = false;

    McpSession* session = nullptr;
    base::OnceCallback<void(base::Value)> callback;
  };

  // -----------------------------------------------------------------------
  // 검색 경로 진입점
  // -----------------------------------------------------------------------

  // role/name: Accessibility.queryAXTree 또는 getFullAXTree+필터
  void DoRoleSearch(std::shared_ptr<SearchContext> ctx);

  // text: Accessibility.queryAXTree(accessibleName) 또는 getFullAXTree+필터
  void DoTextSearch(std::shared_ptr<SearchContext> ctx);

  // selector: DOM.getDocument → DOM.querySelectorAll
  void DoSelectorSearch(std::shared_ptr<SearchContext> ctx);

  // xpath: DOM.performSearch → DOM.getSearchResults → DOM.discardSearchResults
  void DoXPathSearch(std::shared_ptr<SearchContext> ctx);

  // -----------------------------------------------------------------------
  // AX Tree 경로 콜백
  // -----------------------------------------------------------------------

  // Accessibility.queryAXTree 응답 → ax_entries 수집 → ResolveAXEntries
  void OnQueryAXTree(std::shared_ptr<SearchContext> ctx, base::Value response);

  // Accessibility.getFullAXTree 응답 (role+name contains 또는 text contains)
  // is_role_search=true이면 role 매칭도 수행
  void OnFullAXTree(std::shared_ptr<SearchContext> ctx,
                    bool is_role_search,
                    base::Value response);

  // ax_entries 목록이 준비된 후 각 backendNodeId에 대해 describeNode 요청
  void ResolveAXEntries(std::shared_ptr<SearchContext> ctx);

  // DOM.describeNode 응답 (AX 경로, 인덱스 i) → tag/attributes 추출 → getBoxModel
  void OnAXDescribeNode(std::shared_ptr<SearchContext> ctx,
                        size_t index,
                        base::Value response);

  // DOM.getBoxModel 응답 (AX 경로, 인덱스 i)
  void OnAXBoxModel(std::shared_ptr<SearchContext> ctx,
                    size_t index,
                    std::string tag,
                    base::DictValue attributes,
                    base::Value response);

  // -----------------------------------------------------------------------
  // Selector 경로 콜백
  // -----------------------------------------------------------------------

  // DOM.getDocument 응답 → DOM.querySelectorAll
  void OnGetDocumentForSelector(std::shared_ptr<SearchContext> ctx,
                                base::Value response);

  // DOM.querySelectorAll 응답 → node_ids 수집 → DescribeDomNodes
  void OnQuerySelectorAll(std::shared_ptr<SearchContext> ctx,
                          base::Value response);

  // -----------------------------------------------------------------------
  // XPath 경로 콜백
  // -----------------------------------------------------------------------

  // DOM.performSearch 응답 → DOM.getSearchResults
  void OnPerformSearch(std::shared_ptr<SearchContext> ctx, base::Value response);

  // DOM.getSearchResults 응답 → discardSearchResults + DescribeDomNodes
  void OnGetSearchResults(std::shared_ptr<SearchContext> ctx,
                          base::Value response);

  // -----------------------------------------------------------------------
  // DOM 경로 공통: describeNode + getBoxModel 체인
  // -----------------------------------------------------------------------

  // node_ids가 준비된 후 각 nodeId에 대해 DOM.describeNode 요청
  void DescribeDomNodes(std::shared_ptr<SearchContext> ctx);

  // DOM.describeNode 응답 (인덱스 i) → DOM.getBoxModel
  void OnDescribeDomNode(std::shared_ptr<SearchContext> ctx,
                         size_t index,
                         base::Value response);

  // DOM.getBoxModel 응답 (DOM 경로, 인덱스 i)
  void OnDomBoxModel(std::shared_ptr<SearchContext> ctx,
                     size_t index,
                     int backend_node_id,
                     std::string tag,
                     base::DictValue attributes,
                     base::Value response);

  // -----------------------------------------------------------------------
  // 완료 처리
  // -----------------------------------------------------------------------

  // pending 카운터가 0이 되면 items를 정렬하여 콜백 호출
  void MaybeFinalize(std::shared_ptr<SearchContext> ctx);

  base::WeakPtrFactory<FindTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_FIND_TOOL_H_
