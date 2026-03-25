// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/element_locator.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

ElementLocator::ElementLocator() = default;
ElementLocator::~ElementLocator() = default;

// ============================================================
// Locate: 파라미터에서 로케이터 타입 판별 및 디스패치
// ============================================================

void ElementLocator::Locate(McpSession* session,
                            const base::Value::Dict& params,
                            Callback callback) {
  // exact 파라미터 (role/name, text 매칭 시 사용, 기본값 false = 부분 일치)
  bool exact = params.FindBool("exact").value_or(false);

  // 우선순위 1: role/name (AX Tree)
  const std::string* role = params.FindString("role");
  const std::string* name = params.FindString("name");
  if (role && !role->empty()) {
    LOG(INFO) << "[ElementLocator] role/name 로케이터: role=" << *role
              << " name=" << (name ? *name : "(없음)");
    LocateByRole(session, *role, name ? *name : "", exact,
                 std::move(callback));
    return;
  }

  // 우선순위 2: text (AX Tree)
  const std::string* text = params.FindString("text");
  if (text && !text->empty()) {
    LOG(INFO) << "[ElementLocator] text 로케이터: text=" << *text
              << " exact=" << exact;
    LocateByText(session, *text, exact, std::move(callback));
    return;
  }

  // 우선순위 3: selector (CSS)
  const std::string* selector = params.FindString("selector");
  if (selector && !selector->empty()) {
    LOG(INFO) << "[ElementLocator] selector 로케이터: " << *selector;
    LocateBySelector(session, *selector, std::move(callback));
    return;
  }

  // 우선순위 4: xpath
  const std::string* xpath = params.FindString("xpath");
  if (xpath && !xpath->empty()) {
    LOG(INFO) << "[ElementLocator] xpath 로케이터: " << *xpath;
    LocateByXPath(session, *xpath, std::move(callback));
    return;
  }

  // 우선순위 5: ref (backendNodeId)
  std::optional<int> ref = params.FindInt("ref");
  if (ref.has_value() && *ref > 0) {
    LOG(INFO) << "[ElementLocator] ref 로케이터: backendNodeId=" << *ref;
    LocateByRef(session, *ref, std::move(callback));
    return;
  }

  // 파라미터가 하나도 없음
  LOG(WARNING) << "[ElementLocator] 로케이터 파라미터가 없습니다.";
  std::move(callback).Run(
      std::nullopt,
      "로케이터 파라미터가 필요합니다 (role, text, selector, xpath, ref 중 하나)");
}

// ============================================================
// LocateByRole: Accessibility.queryAXTree
// ============================================================

void ElementLocator::LocateByRole(McpSession* session,
                                  const std::string& role,
                                  const std::string& name,
                                  bool exact,
                                  Callback callback) {
  if (exact || name.empty()) {
    // exact 모드 또는 name 미지정: queryAXTree로 정확 매칭
    base::DictValue params;
    params.Set("role", role);
    if (!name.empty()) {
      params.Set("accessibleName", name);
    }
    session->SendCdpCommand(
        "Accessibility.queryAXTree", std::move(params),
        base::BindOnce(&ElementLocator::OnQueryAXTreeResponse,
                       weak_factory_.GetWeakPtr(),
                       session, std::move(callback)));
  } else {
    // exact:false + name 있음: getFullAXTree 후 role + name contains 필터
    session->SendCdpCommand(
        "Accessibility.getFullAXTree", base::DictValue(),
        base::BindOnce(&ElementLocator::OnFullAXTreeForRole,
                       weak_factory_.GetWeakPtr(),
                       session, role, name, std::move(callback)));
  }
}

// ============================================================
// OnFullAXTreeForRole: exact:false 시 role + name contains 필터
// ============================================================

void ElementLocator::OnFullAXTreeForRole(McpSession* session,
                                         const std::string& role,
                                         const std::string& name,
                                         Callback callback,
                                         base::Value response) {
  if (HasCdpError(response)) {
    std::string error_msg = ExtractCdpErrorMessage(response);
    std::move(callback).Run(
        std::nullopt, "Accessibility.getFullAXTree 실패: " + error_msg);
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(callback).Run(std::nullopt, "AX 전체 트리 응답 형식 오류");
    return;
  }

  const base::ListValue* nodes = nullptr;
  if (const base::DictValue* result = dict->FindDict("result")) {
    nodes = result->FindList("nodes");
  }
  if (!nodes) {
    nodes = dict->FindList("nodes");
  }

  if (!nodes || nodes->empty()) {
    std::move(callback).Run(std::nullopt, "AX 전체 트리가 비어 있습니다");
    return;
  }

  std::string role_lower = role;
  for (char& c : role_lower) c = base::ToLowerASCII(c);
  std::string name_lower = name;
  for (char& c : name_lower) c = base::ToLowerASCII(c);

  for (const auto& node_val : *nodes) {
    const base::DictValue* node = node_val.GetIfDict();
    if (!node) continue;

    // role 매칭 (정확 일치)
    const base::DictValue* role_obj = node->FindDict("role");
    if (!role_obj) continue;
    const std::string* role_val = role_obj->FindString("value");
    if (!role_val) continue;
    std::string node_role_lower = *role_val;
    for (char& c : node_role_lower) c = base::ToLowerASCII(c);
    if (node_role_lower != role_lower) continue;

    // name contains 매칭
    const base::DictValue* name_obj = node->FindDict("name");
    if (!name_obj) continue;
    const std::string* name_val = name_obj->FindString("value");
    if (!name_val || name_val->empty()) continue;
    std::string node_name_lower = *name_val;
    for (char& c : node_name_lower) c = base::ToLowerASCII(c);
    if (node_name_lower.find(name_lower) == std::string::npos) continue;

    // backendDOMNodeId
    std::optional<int> backend_node_id = node->FindInt("backendDOMNodeId");
    if (!backend_node_id.has_value() || *backend_node_id <= 0) continue;

    LOG(INFO) << "[ElementLocator] role+name contains 매칭: backendNodeId="
              << *backend_node_id << " role=" << *role_val
              << " name=" << *name_val;

    ResolveToCoordinates(session, *backend_node_id, *role_val, *name_val,
                         std::move(callback));
    return;
  }

  std::move(callback).Run(
      std::nullopt,
      "role='" + role + "', name contains '" + name + "' 매칭 요소 없음");
}

// ============================================================
// LocateByText: AX Tree 텍스트 매칭
// ============================================================

void ElementLocator::LocateByText(McpSession* session,
                                  const std::string& text,
                                  bool exact,
                                  Callback callback) {
  if (exact) {
    // exact 모드: Accessibility.queryAXTree에 accessibleName으로 검색
    base::DictValue params;
    params.Set("accessibleName", text);

    session->SendCdpCommand(
        "Accessibility.queryAXTree", std::move(params),
        base::BindOnce(&ElementLocator::OnQueryAXTreeResponse,
                       weak_factory_.GetWeakPtr(),
                       session, std::move(callback)));
  } else {
    // contains 모드: 전체 AX 트리를 가져와서 클라이언트 사이드 필터링
    base::DictValue params;

    session->SendCdpCommand(
        "Accessibility.getFullAXTree", std::move(params),
        base::BindOnce(&ElementLocator::OnFullAXTreeResponse,
                       weak_factory_.GetWeakPtr(),
                       session, text, std::move(callback)));
  }
}

// ============================================================
// LocateBySelector: DOM.getDocument → DOM.querySelector → DOM.describeNode
// ============================================================

void ElementLocator::LocateBySelector(McpSession* session,
                                      const std::string& selector,
                                      Callback callback) {
  base::DictValue params;
  params.Set("depth", 0);

  session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&ElementLocator::OnGetDocumentForSelector,
                     weak_factory_.GetWeakPtr(),
                     session, selector, std::move(callback)));
}

// ============================================================
// LocateByXPath: DOM.performSearch → DOM.getSearchResults → DOM.describeNode
// ============================================================

void ElementLocator::LocateByXPath(McpSession* session,
                                   const std::string& xpath,
                                   Callback callback) {
  base::DictValue params;
  params.Set("query", xpath);
  params.Set("includeUserAgentShadowDOM", false);

  session->SendCdpCommand(
      "DOM.performSearch", std::move(params),
      base::BindOnce(&ElementLocator::OnPerformSearch,
                     weak_factory_.GetWeakPtr(),
                     session, std::move(callback)));
}

// ============================================================
// LocateByRef: 직접 ResolveToCoordinates
// ============================================================

void ElementLocator::LocateByRef(McpSession* session,
                                 int backend_node_id,
                                 Callback callback) {
  ResolveToCoordinates(session, backend_node_id, "", "", std::move(callback));
}

// ============================================================
// OnQueryAXTreeResponse: AX 트리 조회 결과에서 첫 노드 선택
// ============================================================

void ElementLocator::OnQueryAXTreeResponse(McpSession* session,
                                           Callback callback,
                                           base::Value response) {
  if (HasCdpError(response)) {
    std::string error_msg = ExtractCdpErrorMessage(response);
    LOG(ERROR) << "[ElementLocator] Accessibility.queryAXTree 실패: "
               << error_msg;
    std::move(callback).Run(std::nullopt,
                            "Accessibility.queryAXTree 실패: " + error_msg);
    return;
  }

  // 응답 구조: {result: {nodes: [...]}} 또는 {nodes: [...]}
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(callback).Run(std::nullopt, "AX 트리 응답 형식 오류");
    return;
  }

  const base::ListValue* nodes = nullptr;
  if (const base::DictValue* result = dict->FindDict("result")) {
    nodes = result->FindList("nodes");
  }
  if (!nodes) {
    nodes = dict->FindList("nodes");
  }

  if (!nodes || nodes->empty()) {
    LOG(WARNING) << "[ElementLocator] AX 트리에서 일치하는 노드 없음";
    std::move(callback).Run(std::nullopt,
                            "접근성 트리에서 일치하는 요소를 찾을 수 없습니다");
    return;
  }

  // 첫 번째 노드에서 backendDOMNodeId, role, name 추출
  const base::Value& first_node_val = (*nodes)[0];
  const base::DictValue* first_node = first_node_val.GetIfDict();
  if (!first_node) {
    std::move(callback).Run(std::nullopt, "AX 노드 형식 오류");
    return;
  }

  std::optional<int> backend_node_id =
      first_node->FindInt("backendDOMNodeId");
  if (!backend_node_id.has_value() || *backend_node_id <= 0) {
    std::move(callback).Run(
        std::nullopt, "AX 노드에 backendDOMNodeId가 없습니다");
    return;
  }

  // role 추출: {type, value} 구조
  std::string result_role;
  const base::DictValue* role_obj = first_node->FindDict("role");
  if (role_obj) {
    const std::string* role_val = role_obj->FindString("value");
    if (role_val) {
      result_role = *role_val;
    }
  }

  // name 추출: {type, value} 구조
  std::string result_name;
  const base::DictValue* name_obj = first_node->FindDict("name");
  if (name_obj) {
    const std::string* name_val = name_obj->FindString("value");
    if (name_val) {
      result_name = *name_val;
    }
  }

  LOG(INFO) << "[ElementLocator] AX 노드 선택: backendNodeId="
            << *backend_node_id << " role=" << result_role
            << " name=" << result_name;

  ResolveToCoordinates(session, *backend_node_id, result_role, result_name,
                       std::move(callback));
}

// ============================================================
// OnFullAXTreeResponse: 전체 AX 트리에서 contains 필터링
// ============================================================

void ElementLocator::OnFullAXTreeResponse(McpSession* session,
                                          const std::string& text,
                                          Callback callback,
                                          base::Value response) {
  if (HasCdpError(response)) {
    std::string error_msg = ExtractCdpErrorMessage(response);
    LOG(ERROR) << "[ElementLocator] Accessibility.getFullAXTree 실패: "
               << error_msg;
    std::move(callback).Run(
        std::nullopt, "Accessibility.getFullAXTree 실패: " + error_msg);
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(callback).Run(std::nullopt, "AX 전체 트리 응답 형식 오류");
    return;
  }

  const base::ListValue* nodes = nullptr;
  if (const base::DictValue* result = dict->FindDict("result")) {
    nodes = result->FindList("nodes");
  }
  if (!nodes) {
    nodes = dict->FindList("nodes");
  }

  if (!nodes || nodes->empty()) {
    std::move(callback).Run(std::nullopt,
                            "AX 전체 트리가 비어 있습니다");
    return;
  }

  // 소문자 변환하여 contains 비교
  std::string text_lower = text;
  for (char& c : text_lower) {
    c = base::ToLowerASCII(c);
  }

  // 모든 노드를 순회하며 name.value에 text가 포함된 첫 노드 찾기
  for (const auto& node_val : *nodes) {
    const base::DictValue* node = node_val.GetIfDict();
    if (!node) {
      continue;
    }

    // name.value 추출
    const base::DictValue* name_obj = node->FindDict("name");
    if (!name_obj) {
      continue;
    }
    const std::string* name_val = name_obj->FindString("value");
    if (!name_val || name_val->empty()) {
      continue;
    }

    // contains 비교 (대소문자 무시)
    std::string name_lower = *name_val;
    for (char& c : name_lower) {
      c = base::ToLowerASCII(c);
    }

    if (name_lower.find(text_lower) == std::string::npos) {
      continue;
    }

    // backendDOMNodeId 확인
    std::optional<int> backend_node_id =
        node->FindInt("backendDOMNodeId");
    if (!backend_node_id.has_value() || *backend_node_id <= 0) {
      continue;
    }

    // role 추출
    std::string result_role;
    const base::DictValue* role_obj = node->FindDict("role");
    if (role_obj) {
      const std::string* role_val = role_obj->FindString("value");
      if (role_val) {
        result_role = *role_val;
      }
    }

    LOG(INFO) << "[ElementLocator] text contains 매칭: backendNodeId="
              << *backend_node_id << " name=" << *name_val;

    ResolveToCoordinates(session, *backend_node_id, result_role, *name_val,
                         std::move(callback));
    return;
  }

  // 매칭 노드 없음
  std::move(callback).Run(
      std::nullopt,
      "접근성 트리에서 '" + text + "'를 포함하는 요소를 찾을 수 없습니다");
}

// ============================================================
// OnGetDocumentForSelector: DOM.getDocument 응답 → DOM.querySelector
// ============================================================

void ElementLocator::OnGetDocumentForSelector(McpSession* session,
                                              const std::string& selector,
                                              Callback callback,
                                              base::Value response) {
  if (HasCdpError(response)) {
    std::string error_msg = ExtractCdpErrorMessage(response);
    LOG(ERROR) << "[ElementLocator] DOM.getDocument 실패: " << error_msg;
    std::move(callback).Run(std::nullopt,
                            "DOM.getDocument 실패: " + error_msg);
    return;
  }

  int root_node_id = ExtractRootNodeId(response);
  if (root_node_id <= 0) {
    std::move(callback).Run(std::nullopt,
                            "DOM 루트 노드 ID를 획득할 수 없습니다");
    return;
  }

  base::DictValue params;
  params.Set("nodeId", root_node_id);
  params.Set("selector", selector);

  session->SendCdpCommand(
      "DOM.querySelector", std::move(params),
      base::BindOnce(&ElementLocator::OnQuerySelector,
                     weak_factory_.GetWeakPtr(),
                     session, std::move(callback)));
}

// ============================================================
// OnQuerySelector: DOM.querySelector 응답 → DOM.describeNode
// ============================================================

void ElementLocator::OnQuerySelector(McpSession* session,
                                     Callback callback,
                                     base::Value response) {
  if (HasCdpError(response)) {
    std::string error_msg = ExtractCdpErrorMessage(response);
    LOG(ERROR) << "[ElementLocator] DOM.querySelector 실패: " << error_msg;
    std::move(callback).Run(std::nullopt,
                            "DOM.querySelector 실패: " + error_msg);
    return;
  }

  int node_id = ExtractNodeId(response);
  if (node_id <= 0) {
    std::move(callback).Run(
        std::nullopt,
        "지정한 셀렉터에 일치하는 요소를 찾을 수 없습니다");
    return;
  }

  base::DictValue params;
  params.Set("nodeId", node_id);

  session->SendCdpCommand(
      "DOM.describeNode", std::move(params),
      base::BindOnce(&ElementLocator::OnDescribeNode,
                     weak_factory_.GetWeakPtr(),
                     session, std::move(callback)));
}

// ============================================================
// OnDescribeNode: DOM.describeNode 응답 → backendNodeId 추출
// ============================================================

void ElementLocator::OnDescribeNode(McpSession* session,
                                    Callback callback,
                                    base::Value response) {
  if (HasCdpError(response)) {
    std::string error_msg = ExtractCdpErrorMessage(response);
    LOG(ERROR) << "[ElementLocator] DOM.describeNode 실패: " << error_msg;
    std::move(callback).Run(std::nullopt,
                            "DOM.describeNode 실패: " + error_msg);
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(callback).Run(std::nullopt, "DOM.describeNode 응답 형식 오류");
    return;
  }

  // 응답 구조: {result: {node: {backendNodeId: N, ...}}} 또는 {node: {...}}
  const base::DictValue* node = nullptr;
  if (const base::DictValue* result = dict->FindDict("result")) {
    node = result->FindDict("node");
  }
  if (!node) {
    node = dict->FindDict("node");
  }

  if (!node) {
    std::move(callback).Run(std::nullopt,
                            "DOM.describeNode 응답에 node 정보 없음");
    return;
  }

  std::optional<int> backend_node_id = node->FindInt("backendNodeId");
  if (!backend_node_id.has_value() || *backend_node_id <= 0) {
    std::move(callback).Run(std::nullopt,
                            "DOM.describeNode에서 backendNodeId를 추출할 수 없습니다");
    return;
  }

  LOG(INFO) << "[ElementLocator] describeNode → backendNodeId="
            << *backend_node_id;

  ResolveToCoordinates(session, *backend_node_id, "", "",
                       std::move(callback));
}

// ============================================================
// OnPerformSearch: DOM.performSearch 응답 → DOM.getSearchResults
// ============================================================

void ElementLocator::OnPerformSearch(McpSession* session,
                                     Callback callback,
                                     base::Value response) {
  if (HasCdpError(response)) {
    std::string error_msg = ExtractCdpErrorMessage(response);
    LOG(ERROR) << "[ElementLocator] DOM.performSearch 실패: " << error_msg;
    std::move(callback).Run(std::nullopt,
                            "DOM.performSearch 실패: " + error_msg);
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(callback).Run(std::nullopt, "DOM.performSearch 응답 형식 오류");
    return;
  }

  // searchId, resultCount 추출
  const base::DictValue* data = dict;
  if (const base::DictValue* result = dict->FindDict("result")) {
    data = result;
  }

  const std::string* search_id = data->FindString("searchId");
  std::optional<int> result_count = data->FindInt("resultCount");

  if (!search_id || !result_count) {
    std::move(callback).Run(
        std::nullopt, "DOM.performSearch: searchId 또는 resultCount 없음");
    return;
  }

  if (*result_count == 0) {
    // 결과 없어도 searchId 세션 정리 필요
    base::DictValue discard_params;
    discard_params.Set("searchId", *search_id);
    session->SendCdpCommand("DOM.discardSearchResults",
                            std::move(discard_params),
                            base::BindOnce([](base::Value) {}));
    std::move(callback).Run(
        std::nullopt,
        "XPath에 일치하는 요소를 찾을 수 없습니다");
    return;
  }

  LOG(INFO) << "[ElementLocator] performSearch: searchId=" << *search_id
            << " resultCount=" << *result_count;

  // 첫 번째 결과만 가져온다
  base::DictValue params;
  params.Set("searchId", *search_id);
  params.Set("fromIndex", 0);
  params.Set("toIndex", 1);

  session->SendCdpCommand(
      "DOM.getSearchResults", std::move(params),
      base::BindOnce(&ElementLocator::OnGetSearchResults,
                     weak_factory_.GetWeakPtr(),
                     session, *search_id, std::move(callback)));
}

// ============================================================
// OnGetSearchResults: DOM.getSearchResults 응답 → discardSearchResults + describeNode
// ============================================================

void ElementLocator::OnGetSearchResults(McpSession* session,
                                        const std::string& search_id,
                                        Callback callback,
                                        base::Value response) {
  // 검색 결과 누수 방지: discardSearchResults 호출 (응답 무시)
  {
    base::DictValue discard_params;
    discard_params.Set("searchId", search_id);
    session->SendCdpCommand(
        "DOM.discardSearchResults", std::move(discard_params),
        base::BindOnce([](base::Value) {}));
  }

  if (HasCdpError(response)) {
    std::string error_msg = ExtractCdpErrorMessage(response);
    LOG(ERROR) << "[ElementLocator] DOM.getSearchResults 실패: " << error_msg;
    std::move(callback).Run(std::nullopt,
                            "DOM.getSearchResults 실패: " + error_msg);
    return;
  }

  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(callback).Run(std::nullopt,
                            "DOM.getSearchResults 응답 형식 오류");
    return;
  }

  // nodeIds 배열 추출
  const base::ListValue* node_ids = nullptr;
  if (const base::DictValue* result = dict->FindDict("result")) {
    node_ids = result->FindList("nodeIds");
  }
  if (!node_ids) {
    node_ids = dict->FindList("nodeIds");
  }

  if (!node_ids || node_ids->empty()) {
    std::move(callback).Run(
        std::nullopt, "XPath 검색 결과가 비어 있습니다");
    return;
  }

  // 첫 번째 nodeId로 describeNode 호출
  const base::Value& first_id = (*node_ids)[0];
  if (!first_id.is_int()) {
    std::move(callback).Run(std::nullopt, "검색 결과 nodeId 형식 오류");
    return;
  }

  int node_id = first_id.GetInt();
  LOG(INFO) << "[ElementLocator] getSearchResults → nodeId=" << node_id;

  base::DictValue params;
  params.Set("nodeId", node_id);

  session->SendCdpCommand(
      "DOM.describeNode", std::move(params),
      base::BindOnce(&ElementLocator::OnDescribeNode,
                     weak_factory_.GetWeakPtr(),
                     session, std::move(callback)));
}

// ============================================================
// ResolveToCoordinates: backendNodeId → DOM.getBoxModel → 좌표
// ============================================================

void ElementLocator::ResolveToCoordinates(McpSession* session,
                                          int backend_node_id,
                                          const std::string& role,
                                          const std::string& name,
                                          Callback callback) {
  base::DictValue params;
  params.Set("backendNodeId", backend_node_id);

  session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(&ElementLocator::OnGetBoxModel,
                     weak_factory_.GetWeakPtr(),
                     backend_node_id, role, name, std::move(callback)));
}

// ============================================================
// OnGetBoxModel: BoxModel에서 중심 좌표 추출 → Result 생성
// ============================================================

void ElementLocator::OnGetBoxModel(int backend_node_id,
                                   const std::string& role,
                                   const std::string& name,
                                   Callback callback,
                                   base::Value response) {
  if (HasCdpError(response)) {
    std::string error_msg = ExtractCdpErrorMessage(response);
    LOG(ERROR) << "[ElementLocator] DOM.getBoxModel 실패: " << error_msg;
    std::move(callback).Run(std::nullopt,
                            "DOM.getBoxModel 실패: " + error_msg);
    return;
  }

  double x = 0.0, y = 0.0;
  if (!ExtractBoxModelCenter(response, &x, &y)) {
    LOG(ERROR) << "[ElementLocator] BoxModel에서 중심 좌표를 추출할 수 없음";
    std::move(callback).Run(
        std::nullopt, "요소의 BoxModel 좌표를 계산할 수 없습니다");
    return;
  }

  Result result;
  result.backend_node_id = backend_node_id;
  result.x = x;
  result.y = y;
  result.role = role;
  result.name = name;

  LOG(INFO) << "[ElementLocator] 요소 탐색 완료: backendNodeId="
            << backend_node_id << " (" << x << ", " << y << ")"
            << " role=" << role << " name=" << name;

  std::move(callback).Run(std::move(result), "");
}

}  // namespace mcp
