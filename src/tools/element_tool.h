// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_ELEMENT_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_ELEMENT_TOOL_H_

#include <memory>
#include <set>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// CSS 선택자로 DOM 요소의 상세 정보를 조회하는 도구.
//
// 조회 흐름:
//   1. DOM.getDocument → DOM.querySelector → nodeId 획득
//   2. DOM.getAttributes(nodeId)         — HTML 속성 목록
//   3. CSS.getComputedStyleForNode(nodeId) — 계산된 CSS 스타일 (includeStyles)
//   4. DOM.getBoxModel(nodeId)             — margin/border/padding/content 영역 (includeBox)
//   5. DOM.describeNode(nodeId)            — tagName, className, nodeType 등
//
// 결과 필드:
//   tagName, id, className, attributes, computedStyles, boxModel
//
// 사용 예:
//   { "selector": "#my-div", "includeStyles": true, "includeBox": true }
class ElementTool : public McpTool {
 public:
  ElementTool();
  ~ElementTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 한 번의 Execute 호출에 필요한 모든 상태를 담는 컨텍스트.
  // 비동기 CDP 호출 간에 shared_ptr로 공유된다.
  struct QueryContext {
    QueryContext();
    ~QueryContext();
    // 입력 파라미터
    std::string selector;
    std::set<std::string> requested_properties;  // 빈 집합 = 전체 조회
    bool include_styles = false;
    bool include_box = false;

    // 중간 결과
    int node_id = 0;           // DOM.querySelector 로 획득한 nodeId

    // 완료 추적 (비동기 요청 수 관리)
    int pending = 0;           // 아직 완료되지 않은 CDP 요청 수
    bool finalized = false;    // 최종 콜백이 이미 호출되었는지 여부

    // 누적 결과
    base::DictValue result;

    // 소유권 없는 포인터 (McpSession의 수명이 더 길다고 가정)
    McpSession* session = nullptr;
    base::OnceCallback<void(base::Value)> callback;
  };

  // -----------------------------------------------------------------------
  // CDP 단계별 핸들러
  // -----------------------------------------------------------------------

  // DOM.getDocument 응답 → DOM.querySelector 호출
  void OnGetDocument(std::shared_ptr<QueryContext> ctx, base::Value response);

  // DOM.querySelector 응답 → 속성/스타일/박스 조회 시작
  void OnQuerySelector(std::shared_ptr<QueryContext> ctx, base::Value response);

  // DOM.getAttributes 응답 처리
  void OnGetAttributes(std::shared_ptr<QueryContext> ctx, base::Value response);

  // CSS.getComputedStyleForNode 응답 처리
  void OnGetComputedStyle(std::shared_ptr<QueryContext> ctx,
                          base::Value response);

  // DOM.getBoxModel 응답 처리
  void OnGetBoxModel(std::shared_ptr<QueryContext> ctx, base::Value response);

  // DOM.describeNode 응답 처리 (tagName, className 등)
  void OnDescribeNode(std::shared_ptr<QueryContext> ctx, base::Value response);

  // -----------------------------------------------------------------------
  // 내부 헬퍼
  // -----------------------------------------------------------------------

  // nodeId 획득 후 요청된 CDP 명령들을 병렬로 실행한다.
  void DispatchRequests(std::shared_ptr<QueryContext> ctx);

  // 비동기 요청 하나가 완료될 때마다 호출.
  // pending 카운터가 0이 되면 최종 결과를 반환한다.
  void OnOneRequestDone(std::shared_ptr<QueryContext> ctx);

  // CDP 응답에서 오류 메시지를 추출한다. 오류가 없으면 빈 문자열 반환.
  static std::string ExtractCdpError(const base::DictValue& response_dict);

  base::WeakPtrFactory<ElementTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_ELEMENT_TOOL_H_
