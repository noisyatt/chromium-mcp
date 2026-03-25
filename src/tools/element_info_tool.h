// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_ELEMENT_INFO_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_ELEMENT_INFO_TOOL_H_

#include <set>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// 통합 로케이터로 DOM 요소의 상세 정보를 조회하는 도구.
//
// 조회 가능한 항목 (properties 파라미터로 선택):
//   - "attributes":    모든 HTML 속성 (DOM.getAttributes)
//   - "computedStyle": 계산된 CSS 스타일 (CSS.getComputedStyleForNode)
//   - "boundingBox":   위치/크기 정보 (DOM.getBoxModel)
//   - "text":          innerText (Runtime.evaluate)
//   - "html":          outerHTML (Runtime.evaluate)
//   - "value":         input/textarea의 현재 값 (Runtime.evaluate)
//   - "checked":       checkbox/radio의 체크 상태 (Runtime.evaluate)
//   - "visible":       가시성 여부 (Runtime.evaluate)
//
// 구현 흐름:
//   1. ElementLocator::Locate() → nodeId 획득
//   2. 요청된 각 property에 대해 CDP 명령 실행
//   3. 모든 결과를 취합하여 반환
class ElementInfoTool : public McpTool {
 public:
  ElementInfoTool();
  ~ElementInfoTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 public:
  // 조회 실행 컨텍스트.
  struct QueryContext {
    QueryContext();
    ~QueryContext();
    std::string selector;          // 로깅용
    std::set<std::string> properties;  // 조회할 property 목록
    int node_id = 0;               // ElementLocator로 획득한 nodeId
    base::DictValue result;        // 최종 결과 누적
    McpSession* session = nullptr;
    base::OnceCallback<void(base::Value)> callback;
  };

 private:
  // ElementLocator 콜백: nodeId 획득 후 property 조회를 시작한다.
  void OnLocated(std::shared_ptr<QueryContext> ctx,
                 std::optional<ElementLocator::Result> result,
                 std::string error);

  // 요청된 각 property에 대한 조회를 실행한다.
  void FetchProperties(std::shared_ptr<QueryContext> ctx);

  // DOM.getAttributes 응답 처리 (attributes property)
  void OnGetAttributesResponse(std::shared_ptr<QueryContext> ctx,
                                 base::Value response);

  // CSS.getComputedStyleForNode 응답 처리 (computedStyle property)
  void OnGetComputedStyleResponse(std::shared_ptr<QueryContext> ctx,
                                   base::Value response);

  // DOM.getBoxModel 응답 처리 (boundingBox property)
  void OnGetBoxModelResponse(std::shared_ptr<QueryContext> ctx,
                               base::Value response);

  // Runtime.evaluate 응답 처리 (text/html/value/checked/visible property)
  void OnRuntimeEvaluateResponse(std::shared_ptr<QueryContext> ctx,
                                   const std::string& property_name,
                                   base::Value response);

  // 모든 property 조회가 완료되었을 때 최종 결과를 반환한다.
  void FinalizeResult(std::shared_ptr<QueryContext> ctx);

  ElementLocator locator_;

  base::WeakPtrFactory<ElementInfoTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_ELEMENT_INFO_TOOL_H_
