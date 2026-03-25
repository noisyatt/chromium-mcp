// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_ELEMENT_LOCATOR_H_
#define CHROME_BROWSER_MCP_TOOLS_ELEMENT_LOCATOR_H_

#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"

namespace mcp {

class McpSession;

// ElementLocator: AX Tree 기반 통합 요소 탐색 클래스.
//
// 모든 액션 도구(click, fill, hover 등)가 공유하는 요소 탐색 로직을 제공한다.
// CSS 셀렉터 전용이던 탐색을 접근성 트리(AX Tree) 기반으로 확장하여,
// role/name, text, selector, xpath, ref(backendNodeId) 총 5가지 로케이터를 지원한다.
//
// 우선순위: role/name > text > selector > xpath > ref
//
// 사용 예시:
//   ElementLocator locator;
//   locator.Locate(session, params, std::move(callback));
class ElementLocator {
 public:
  // 요소 탐색 결과.
  struct Result {
    Result();
    ~Result();
    Result(const Result&);
    Result& operator=(const Result&);

    int backend_node_id = 0;  // DOM.getBoxModel에 사용된 backendNodeId
    int node_id = 0;          // DOM nodeId (0이면 미사용)
    double x = 0;             // 요소 중심 X 좌표
    double y = 0;             // 요소 중심 Y 좌표
    std::string role;         // AX 역할 (예: "button", "link")
    std::string name;         // AX 접근성 이름
  };

  // 탐색 완료 콜백.
  // 성공: {Result, ""} — error 빈 문자열
  // 실패: {std::nullopt, "에러 메시지"}
  using Callback =
      base::OnceCallback<void(std::optional<Result>, std::string error)>;

  ElementLocator();
  ~ElementLocator();

  // 파라미터에서 로케이터 타입을 판별하고 적절한 경로로 디스패치.
  // 우선순위: role/name > text > selector > xpath > ref
  // 파라미터가 하나도 없으면 에러 콜백 호출.
  void Locate(McpSession* session,
              const base::DictValue& params,
              Callback callback);

 private:
  // -----------------------------------------------------------------------
  // AX Tree 경로: role/name 매칭
  // -----------------------------------------------------------------------

  // Accessibility.queryAXTree({role, accessibleName}) → OnQueryAXTreeResponse
  void LocateByRole(McpSession* session,
                    const std::string& role,
                    const std::string& name,
                    bool exact,
                    Callback callback);

  // exact:false 시 getFullAXTree 후 role + name contains 필터
  void OnFullAXTreeForRole(McpSession* session,
                           const std::string& role,
                           const std::string& name,
                           Callback callback,
                           base::Value response);

  // -----------------------------------------------------------------------
  // AX Tree 경로: text 매칭
  // -----------------------------------------------------------------------

  // exact:true → Accessibility.queryAXTree({accessibleName})
  // exact:false → Accessibility.getFullAXTree() → 클라이언트 사이드 contains 필터
  void LocateByText(McpSession* session,
                    const std::string& text,
                    bool exact,
                    Callback callback);

  // -----------------------------------------------------------------------
  // DOM 경로: CSS 셀렉터
  // -----------------------------------------------------------------------

  // DOM.getDocument → DOM.querySelector → DOM.describeNode → ResolveToCoordinates
  void LocateBySelector(McpSession* session,
                        const std::string& selector,
                        Callback callback);

  // -----------------------------------------------------------------------
  // DOM 경로: XPath
  // -----------------------------------------------------------------------

  // DOM.performSearch → DOM.getSearchResults → DOM.describeNode → ResolveToCoordinates
  void LocateByXPath(McpSession* session,
                     const std::string& xpath,
                     Callback callback);

  // -----------------------------------------------------------------------
  // 직접 참조: backendNodeId
  // -----------------------------------------------------------------------

  // 직접 ResolveToCoordinates(backendNodeId)
  void LocateByRef(McpSession* session,
                   int backend_node_id,
                   Callback callback);

  // -----------------------------------------------------------------------
  // AX Tree 콜백
  // -----------------------------------------------------------------------

  // Accessibility.queryAXTree 응답에서 visible 우선으로 노드 선택
  void OnQueryAXTreeResponse(McpSession* session,
                             Callback callback,
                             base::Value response);

  // nodes 배열을 index부터 순회하며 getBoxModel 성공(=visible) 노드 선택.
  // 모두 실패 시 fallback_backend_id로 ResolveToCoordinates 호출.
  void TryNextVisibleNode(McpSession* session,
                          base::Value nodes_storage,
                          size_t index,
                          int fallback_backend_id,
                          const std::string& fallback_role,
                          const std::string& fallback_name,
                          Callback callback);

  // exact:false 시 Accessibility.getFullAXTree 응답에서 contains 필터링
  void OnFullAXTreeResponse(McpSession* session,
                            const std::string& text,
                            Callback callback,
                            base::Value response);

  // -----------------------------------------------------------------------
  // Selector 콜백 체인
  // -----------------------------------------------------------------------

  // DOM.getDocument 응답 → DOM.querySelector 호출
  void OnGetDocumentForSelector(McpSession* session,
                                const std::string& selector,
                                Callback callback,
                                base::Value response);

  // DOM.querySelector 응답 → DOM.describeNode 호출
  void OnQuerySelector(McpSession* session,
                       Callback callback,
                       base::Value response);

  // DOM.describeNode 응답 → backendNodeId 추출 → ResolveToCoordinates
  void OnDescribeNode(McpSession* session,
                      Callback callback,
                      base::Value response);

  // -----------------------------------------------------------------------
  // XPath 콜백 체인
  // -----------------------------------------------------------------------

  // DOM.performSearch 응답 → DOM.getSearchResults 호출
  void OnPerformSearch(McpSession* session,
                       Callback callback,
                       base::Value response);

  // DOM.getSearchResults 응답 → DOM.discardSearchResults + DOM.describeNode
  void OnGetSearchResults(McpSession* session,
                          const std::string& search_id,
                          Callback callback,
                          base::Value response);

  // -----------------------------------------------------------------------
  // 공통: 좌표 해상도
  // -----------------------------------------------------------------------

  // backendNodeId → DOM.getBoxModel → Result
  void ResolveToCoordinates(McpSession* session,
                            int backend_node_id,
                            const std::string& role,
                            const std::string& name,
                            Callback callback);

  // DOM.getBoxModel 응답 → ExtractBoxModelCenter → Result 생성
  void OnGetBoxModel(int backend_node_id,
                     const std::string& role,
                     const std::string& name,
                     Callback callback,
                     base::Value response);

  // weak_factory_는 반드시 클래스 마지막 멤버
  base::WeakPtrFactory<ElementLocator> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_ELEMENT_LOCATOR_H_
