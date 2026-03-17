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

// 페이지에서 텍스트·CSS 선택자·XPath로 DOM 요소를 검색하는 도구.
//
// 검색 유형 (type 파라미터):
//   - text    : DOM.performSearch(plainText) → DOM.getSearchResults
//   - selector: DOM.getDocument → DOM.querySelectorAll
//   - xpath   : DOM.performSearch(XPath 표현식) → DOM.getSearchResults
//
// 각 nodeId에 대해 DOM.describeNode 로 tagName/attributes/innerText 수집.
//
// 사용 예:
//   { "type": "text",     "query": "로그인",              "limit": 5 }
//   { "type": "selector", "query": "button.primary",      "includeAttributes": true }
//   { "type": "xpath",    "query": "//h2[@class='title']", "includeText": true }
class FindTool : public McpTool {
 public:
  FindTool();
  ~FindTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 한 번의 Execute 호출에 필요한 모든 상태
  struct SearchContext {
    // 입력 파라미터
    std::string type;    // "text" | "selector" | "xpath"
    std::string query;
    int limit = 10;
    bool include_text = true;
    bool include_attributes = false;

    // 중간 상태
    std::string search_id;          // DOM.performSearch 로 반환된 searchId
    int result_count = 0;           // performSearch 의 전체 결과 수
    std::vector<int> node_ids;      // 검색된 nodeId 목록

    // 수집 중인 결과
    base::Value::List items;         // 최종 결과 배열

    // 비동기 완료 추적
    int pending_describe = 0;        // 아직 완료되지 않은 describeNode 요청 수
    bool finalized = false;

    McpSession* session = nullptr;
    base::OnceCallback<void(base::Value)> callback;
  };

  // -----------------------------------------------------------------------
  // 검색 유형별 진입점
  // -----------------------------------------------------------------------

  // text/xpath 검색: DOM.performSearch
  void DoPerformSearch(std::shared_ptr<SearchContext> ctx);

  // selector 검색: DOM.getDocument → DOM.querySelectorAll
  void DoSelectorSearch(std::shared_ptr<SearchContext> ctx);

  // -----------------------------------------------------------------------
  // CDP 응답 핸들러
  // -----------------------------------------------------------------------

  // DOM.performSearch 응답 → DOM.getSearchResults 호출
  void OnPerformSearch(std::shared_ptr<SearchContext> ctx,
                       base::Value response);

  // DOM.getSearchResults 응답 → nodeId 목록 수집 → describeNode 호출
  void OnGetSearchResults(std::shared_ptr<SearchContext> ctx,
                          base::Value response);

  // DOM.getDocument 응답 (selector 검색용) → DOM.querySelectorAll 호출
  void OnGetDocumentForSelector(std::shared_ptr<SearchContext> ctx,
                                base::Value response);

  // DOM.querySelectorAll 응답 → nodeId 목록 수집 → describeNode 호출
  void OnQuerySelectorAll(std::shared_ptr<SearchContext> ctx,
                          base::Value response);

  // nodeId 목록이 준비된 후 DOM.describeNode 를 각 nodeId 에 대해 요청
  void DescribeNodes(std::shared_ptr<SearchContext> ctx);

  // DOM.describeNode 응답 처리 (인덱스 i 번째 항목)
  void OnDescribeNode(std::shared_ptr<SearchContext> ctx,
                      size_t index,
                      base::Value response);

  // -----------------------------------------------------------------------
  // 헬퍼
  // -----------------------------------------------------------------------

  // CDP 오류 메시지 추출 (없으면 빈 문자열)
  static std::string ExtractCdpError(const base::Value::Dict& d);

  base::WeakPtrFactory<FindTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_FIND_TOOL_H_
