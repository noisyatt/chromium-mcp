// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/network_intercept_tool.h"

#include <algorithm>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// ============================================================================
// 생성자 / 소멸자
// ============================================================================

InterceptRule::InterceptRule() = default;
InterceptRule::~InterceptRule() = default;
InterceptRule::InterceptRule(InterceptRule&&) = default;
InterceptRule& InterceptRule::operator=(InterceptRule&&) = default;

NetworkInterceptTool::NetworkInterceptTool() = default;
NetworkInterceptTool::~NetworkInterceptTool() = default;

// ============================================================================
// McpTool 인터페이스 구현
// ============================================================================

std::string NetworkInterceptTool::name() const {
  return "network_intercept";
}

std::string NetworkInterceptTool::description() const {
  return "Fetch 도메인 CDP를 사용하여 네트워크 요청을 인터셉트하고 수정하거나 "
         "차단합니다. "
         "enable/disable로 인터셉트를 켜고 끄며, addRule/removeRule로 자동 처리 "
         "규칙을 관리합니다. "
         "일시 정지된 요청은 continueRequest/fulfillRequest/failRequest로 수동 "
         "처리할 수 있습니다. "
         "내부 CDP 세션을 사용하므로 노란 배너가 표시되지 않습니다.";
}

base::DictValue NetworkInterceptTool::input_schema() const {
  // JSON Schema: action 및 각 action에 필요한 파라미터 정의
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // -------------------------------------------------------------------------
  // action: 필수 파라미터
  // -------------------------------------------------------------------------
  {
    base::DictValue action_prop;
    action_prop.Set("type", "string");
    base::ListValue action_enum;
    action_enum.Append("enable");
    action_enum.Append("disable");
    action_enum.Append("addRule");
    action_enum.Append("removeRule");
    action_enum.Append("continueRequest");
    action_enum.Append("fulfillRequest");
    action_enum.Append("failRequest");
    action_prop.Set("enum", std::move(action_enum));
    action_prop.Set(
        "description",
        "수행할 동작. "
        "enable: 인터셉트 시작, disable: 중지, "
        "addRule: 자동 처리 규칙 등록, removeRule: 규칙 삭제, "
        "continueRequest: 일시 정지된 요청 계속 진행, "
        "fulfillRequest: 커스텀 응답 반환, failRequest: 요청 차단");
    properties.Set("action", std::move(action_prop));
  }

  // -------------------------------------------------------------------------
  // patterns: Fetch.enable에 전달할 URL 패턴 목록 (enable 시 선택)
  // -------------------------------------------------------------------------
  {
    base::DictValue patterns_prop;
    patterns_prop.Set("type", "array");

    base::DictValue patterns_item_props;

    base::DictValue url_pattern_p;
    url_pattern_p.Set("type", "string");
    url_pattern_p.Set("description",
                      "인터셉트할 URL 패턴. 비어있으면 전체 URL.");
    patterns_item_props.Set("urlPattern", std::move(url_pattern_p));

    base::DictValue resource_type_item_p;
    resource_type_item_p.Set("type", "string");
    resource_type_item_p.Set(
        "description",
        "인터셉트할 리소스 타입 (예: Document, Script, XHR)");
    patterns_item_props.Set("resourceType", std::move(resource_type_item_p));

    base::DictValue request_stage_p;
    request_stage_p.Set("type", "string");
    base::ListValue stage_enum;
    stage_enum.Append("Request");
    stage_enum.Append("Response");
    request_stage_p.Set("enum", std::move(stage_enum));
    request_stage_p.Set("description",
                        "인터셉트 단계: 요청(Request) 또는 응답(Response)");
    patterns_item_props.Set("requestStage", std::move(request_stage_p));

    base::DictValue patterns_items;
    patterns_items.Set("type", "object");
    patterns_items.Set("properties", std::move(patterns_item_props));
    patterns_prop.Set("items", std::move(patterns_items));
    patterns_prop.Set(
        "description",
        "Fetch.enable에 전달할 패턴 목록. "
        "비어있으면 모든 요청을 인터셉트합니다.");
    properties.Set("patterns", std::move(patterns_prop));
  }

  // -------------------------------------------------------------------------
  // requestId: continueRequest / fulfillRequest / failRequest에 필수
  // -------------------------------------------------------------------------
  {
    base::DictValue req_id_prop;
    req_id_prop.Set("type", "string");
    req_id_prop.Set(
        "description",
        "Fetch.requestPaused 이벤트로 받은 requestId. "
        "continueRequest/fulfillRequest/failRequest 시 필수.");
    properties.Set("requestId", std::move(req_id_prop));
  }

  // -------------------------------------------------------------------------
  // url: continueRequest 시 요청 URL 오버라이드 (선택)
  // -------------------------------------------------------------------------
  {
    base::DictValue url_prop;
    url_prop.Set("type", "string");
    url_prop.Set("description",
                 "continueRequest 시 요청 URL 변경. 생략하면 원본 URL 유지.");
    properties.Set("url", std::move(url_prop));
  }

  // -------------------------------------------------------------------------
  // method: continueRequest 시 HTTP 메서드 오버라이드 (선택)
  // -------------------------------------------------------------------------
  {
    base::DictValue method_prop;
    method_prop.Set("type", "string");
    method_prop.Set(
        "description",
        "continueRequest 시 HTTP 메서드 변경 (예: GET, POST). "
        "생략하면 원본 메서드 유지.");
    properties.Set("method", std::move(method_prop));
  }

  // -------------------------------------------------------------------------
  // headers: continueRequest / fulfillRequest 시 헤더 딕셔너리 (선택)
  // -------------------------------------------------------------------------
  {
    base::DictValue headers_prop;
    headers_prop.Set("type", "object");
    headers_prop.Set(
        "description",
        "헤더 딕셔너리 {\"이름\": \"값\"}. "
        "continueRequest 시 요청 헤더, fulfillRequest 시 응답 헤더로 사용.");
    properties.Set("headers", std::move(headers_prop));
  }

  // -------------------------------------------------------------------------
  // postData: continueRequest 시 요청 바디 오버라이드 (선택)
  // -------------------------------------------------------------------------
  {
    base::DictValue post_data_prop;
    post_data_prop.Set("type", "string");
    post_data_prop.Set(
        "description",
        "continueRequest 시 POST 바디 변경. "
        "생략하면 원본 바디 유지.");
    properties.Set("postData", std::move(post_data_prop));
  }

  // -------------------------------------------------------------------------
  // responseCode: fulfillRequest 시 응답 상태 코드 (선택, 기본값 200)
  // -------------------------------------------------------------------------
  {
    base::DictValue resp_code_prop;
    resp_code_prop.Set("type", "integer");
    resp_code_prop.Set(
        "description",
        "fulfillRequest 시 HTTP 응답 상태 코드. 기본값 200.");
    properties.Set("responseCode", std::move(resp_code_prop));
  }

  // -------------------------------------------------------------------------
  // responseHeaders: fulfillRequest 시 응답 헤더 딕셔너리 (선택)
  // -------------------------------------------------------------------------
  {
    base::DictValue resp_headers_prop;
    resp_headers_prop.Set("type", "object");
    resp_headers_prop.Set(
        "description",
        "fulfillRequest 시 응답 헤더 딕셔너리 {\"이름\": \"값\"}.");
    properties.Set("responseHeaders", std::move(resp_headers_prop));
  }

  // -------------------------------------------------------------------------
  // body: fulfillRequest 시 응답 바디 (선택)
  // -------------------------------------------------------------------------
  {
    base::DictValue body_prop;
    body_prop.Set("type", "string");
    body_prop.Set(
        "description",
        "fulfillRequest 시 응답 바디. "
        "UTF-8 텍스트 또는 base64 인코딩 데이터.");
    properties.Set("body", std::move(body_prop));
  }

  // -------------------------------------------------------------------------
  // errorReason: failRequest 시 에러 사유 (선택, 기본값 BlockedByClient)
  // -------------------------------------------------------------------------
  {
    base::DictValue error_prop;
    error_prop.Set("type", "string");
    base::ListValue err_enum;
    err_enum.Append("Failed");
    err_enum.Append("Aborted");
    err_enum.Append("TimedOut");
    err_enum.Append("AccessDenied");
    err_enum.Append("ConnectionClosed");
    err_enum.Append("ConnectionReset");
    err_enum.Append("ConnectionRefused");
    err_enum.Append("ConnectionAborted");
    err_enum.Append("ConnectionFailed");
    err_enum.Append("NameNotResolved");
    err_enum.Append("InternetDisconnected");
    err_enum.Append("AddressUnreachable");
    err_enum.Append("BlockedByClient");
    err_enum.Append("BlockedByResponse");
    error_prop.Set("enum", std::move(err_enum));
    error_prop.Set(
        "description",
        "failRequest 시 에러 사유. 기본값 BlockedByClient.");
    properties.Set("errorReason", std::move(error_prop));
  }

  // -------------------------------------------------------------------------
  // ruleId: addRule/removeRule 시 사용하는 규칙 고유 ID
  // -------------------------------------------------------------------------
  {
    base::DictValue rule_id_prop;
    rule_id_prop.Set("type", "string");
    rule_id_prop.Set(
        "description",
        "addRule 시 규칙 ID (생략하면 자동 생성). "
        "removeRule 시 삭제할 규칙 ID (필수).");
    properties.Set("ruleId", std::move(rule_id_prop));
  }

  // -------------------------------------------------------------------------
  // urlPattern: addRule 시 매칭할 URL 글로브 패턴 (선택)
  // -------------------------------------------------------------------------
  {
    base::DictValue url_pat_prop;
    url_pat_prop.Set("type", "string");
    url_pat_prop.Set(
        "description",
        "addRule 시 매칭할 URL 글로브 패턴. * 와일드카드 지원. "
        "비어있으면 모든 URL 매칭. "
        "예: \"*.doubleclick.net/*\", \"/api/users\"");
    properties.Set("urlPattern", std::move(url_pat_prop));
  }

  // -------------------------------------------------------------------------
  // resourceType: addRule 시 매칭할 리소스 타입 필터 (선택)
  // -------------------------------------------------------------------------
  {
    base::DictValue res_type_prop;
    res_type_prop.Set("type", "string");
    res_type_prop.Set(
        "description",
        "addRule 시 매칭할 리소스 타입. 비어있으면 모든 타입 매칭. "
        "예: Document, Script, Image, XHR, Fetch");
    properties.Set("resourceType", std::move(res_type_prop));
  }

  // -------------------------------------------------------------------------
  // ruleAction: addRule 시 매칭 후 적용할 자동 동작 (선택, 기본값 passthrough)
  // -------------------------------------------------------------------------
  {
    base::DictValue rule_action_prop;
    rule_action_prop.Set("type", "string");
    base::ListValue ra_enum;
    ra_enum.Append("continue");
    ra_enum.Append("fulfill");
    ra_enum.Append("fail");
    ra_enum.Append("passthrough");
    rule_action_prop.Set("enum", std::move(ra_enum));
    rule_action_prop.Set(
        "description",
        "addRule 시 매칭 시 수행할 동작. "
        "passthrough: 그냥 통과 (로깅 목적), "
        "continue: 수정된 요청으로 통과, "
        "fulfill: 커스텀 응답 반환, "
        "fail: 요청 차단. 기본값 passthrough.");
    properties.Set("ruleAction", std::move(rule_action_prop));
  }

  schema.Set("properties", std::move(properties));

  // action만 필수
  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

// ============================================================================
// Execute: action 파라미터에 따라 적절한 Handle* 메서드로 디스패치
// ============================================================================

void NetworkInterceptTool::Execute(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // action 파라미터 추출 (필수)
  const std::string* action = arguments.FindString("action");
  if (!action || action->empty()) {
    LOG(WARNING) << "[MCP][NetworkIntercept] action 파라미터 누락";
    std::move(callback).Run(
        MakeErrorResult("오류: action 파라미터가 필요합니다"));
    return;
  }

  LOG(INFO) << "[MCP][NetworkIntercept] action=" << *action;

  if (*action == "enable") {
    // patterns 파라미터 (선택)
    const base::ListValue* patterns = arguments.FindList("patterns");
    HandleEnable(patterns, session, std::move(callback));

  } else if (*action == "disable") {
    HandleDisable(session, std::move(callback));

  } else if (*action == "addRule") {
    HandleAddRule(arguments, std::move(callback));

  } else if (*action == "removeRule") {
    // ruleId 파라미터 (필수)
    const std::string* rule_id = arguments.FindString("ruleId");
    if (!rule_id || rule_id->empty()) {
      LOG(WARNING) << "[MCP][NetworkIntercept] removeRule: ruleId 누락";
      std::move(callback).Run(
          MakeErrorResult("오류: removeRule은 ruleId가 필요합니다"));
      return;
    }
    HandleRemoveRule(*rule_id, std::move(callback));

  } else if (*action == "continueRequest") {
    HandleContinueRequest(arguments, session, std::move(callback));

  } else if (*action == "fulfillRequest") {
    HandleFulfillRequest(arguments, session, std::move(callback));

  } else if (*action == "failRequest") {
    HandleFailRequest(arguments, session, std::move(callback));

  } else {
    LOG(WARNING) << "[MCP][NetworkIntercept] 알 수 없는 action: " << *action;
    std::move(callback).Run(
        MakeErrorResult("오류: 알 수 없는 action입니다: " + *action));
  }
}

// ============================================================================
// action 핸들러 구현
// ============================================================================

void NetworkInterceptTool::HandleEnable(
    const base::ListValue* patterns,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // Fetch.enable CDP 파라미터 구성
  base::DictValue params;

  // patterns가 제공된 경우 Fetch.enable의 patterns 배열에 전달
  // 없으면 빈 배열 → 모든 요청을 인터셉트
  if (patterns && !patterns->empty()) {
    params.Set("patterns", patterns->Clone());
    LOG(INFO) << "[MCP][NetworkIntercept] Fetch.enable: "
              << patterns->size() << "개 패턴으로 인터셉트 시작";
  } else {
    // 빈 patterns 배열: URL 패턴 없음 → 전체 인터셉트
    base::ListValue empty_patterns;
    params.Set("patterns", std::move(empty_patterns));
    LOG(INFO) << "[MCP][NetworkIntercept] Fetch.enable: 전체 인터셉트 시작";
  }

  // Fetch.enable CDP 명령 전송
  session->SendCdpCommand(
      "Fetch.enable", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnFetchEnabled,
                     weak_factory_.GetWeakPtr(), session,
                     std::move(callback)));
}

void NetworkInterceptTool::HandleDisable(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (!is_intercepting_) {
    LOG(WARNING) << "[MCP][NetworkIntercept] disable: 인터셉트가 활성화되지 "
                    "않은 상태";
    // 이미 비활성화된 상태여도 오류가 아니라 성공으로 처리
    std::move(callback).Run(
        MakeSuccessResult("인터셉트가 이미 비활성화 상태입니다."));
    return;
  }

  LOG(INFO) << "[MCP][NetworkIntercept] Fetch.disable 전송";

  // Fetch.requestPaused 이벤트 핸들러 먼저 해제 (메모리 누수 방지)
  session->UnregisterCdpEventHandler("Fetch.requestPaused");

  // Fetch.disable CDP 명령 전송
  session->SendCdpCommand(
      "Fetch.disable", base::DictValue(),
      base::BindOnce(&NetworkInterceptTool::OnFetchDisabled,
                     weak_factory_.GetWeakPtr(), session,
                     std::move(callback)));
}

void NetworkInterceptTool::HandleAddRule(
    const base::DictValue& arguments,
    base::OnceCallback<void(base::Value)> callback) {
  // 새 규칙 구성
  InterceptRule rule;

  // ruleId: 제공되면 사용, 없으면 자동 생성
  const std::string* rule_id = arguments.FindString("ruleId");
  if (rule_id && !rule_id->empty()) {
    rule.rule_id = *rule_id;
  } else {
    rule.rule_id = "rule_" + base::NumberToString(next_rule_id_++);
  }

  // URL 패턴 (선택)
  const std::string* url_pattern = arguments.FindString("urlPattern");
  if (url_pattern) {
    rule.url_pattern = *url_pattern;
  }

  // 리소스 타입 필터 (선택)
  const std::string* resource_type = arguments.FindString("resourceType");
  if (resource_type) {
    rule.resource_type = *resource_type;
  }

  // 요청 단계 (선택, 기본값 "Request")
  const std::string* request_stage = arguments.FindString("requestStage");
  if (request_stage && !request_stage->empty()) {
    rule.request_stage = *request_stage;
  } else {
    rule.request_stage = "Request";
  }

  // 자동 처리 동작 (선택, 기본값 "passthrough")
  const std::string* rule_action = arguments.FindString("ruleAction");
  if (rule_action && !rule_action->empty()) {
    rule.action = *rule_action;
  } else {
    rule.action = "passthrough";
  }

  // continue 동작 시 오버라이드 필드
  const std::string* override_url = arguments.FindString("url");
  if (override_url) {
    rule.override_url = *override_url;
  }

  const std::string* override_method = arguments.FindString("method");
  if (override_method) {
    rule.override_method = *override_method;
  }

  const base::DictValue* override_headers = arguments.FindDict("headers");
  if (override_headers) {
    rule.override_headers = override_headers->Clone();
  }

  const std::string* override_post_data = arguments.FindString("postData");
  if (override_post_data) {
    rule.override_post_data = *override_post_data;
  }

  // fulfill 동작 시 모킹 응답 필드
  const base::Value* resp_code_val = arguments.Find("responseCode");
  if (resp_code_val && resp_code_val->is_int()) {
    rule.response_code = resp_code_val->GetInt();
  }

  const base::DictValue* resp_headers = arguments.FindDict("responseHeaders");
  if (resp_headers) {
    rule.response_headers = resp_headers->Clone();
  }

  const std::string* body = arguments.FindString("body");
  if (body) {
    rule.response_body = *body;
  }

  // fail 동작 시 에러 사유 (기본값 "BlockedByClient")
  const std::string* error_reason = arguments.FindString("errorReason");
  if (error_reason && !error_reason->empty()) {
    rule.error_reason = *error_reason;
  } else {
    rule.error_reason = "BlockedByClient";
  }

  // 규칙 등록
  std::string assigned_rule_id = rule.rule_id;
  rules_[assigned_rule_id] = std::move(rule);

  LOG(INFO) << "[MCP][NetworkIntercept] 규칙 등록: id=" << assigned_rule_id
            << " pattern=" << rules_[assigned_rule_id].url_pattern
            << " action=" << rules_[assigned_rule_id].action;

  // 성공 응답: 등록된 ruleId를 포함하여 반환
  base::DictValue result;
  base::ListValue content;
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set(
      "text", "규칙이 등록되었습니다. ruleId: " + assigned_rule_id);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));
  // 편의를 위해 ruleId를 최상위에도 포함
  result.Set("ruleId", assigned_rule_id);

  std::move(callback).Run(base::Value(std::move(result)));
}

void NetworkInterceptTool::HandleRemoveRule(
    const std::string& rule_id,
    base::OnceCallback<void(base::Value)> callback) {
  auto it = rules_.find(rule_id);
  if (it == rules_.end()) {
    LOG(WARNING) << "[MCP][NetworkIntercept] removeRule: 규칙 없음: "
                 << rule_id;
    std::move(callback).Run(MakeErrorResult(
        "오류: ruleId '" + rule_id + "'에 해당하는 규칙이 없습니다"));
    return;
  }

  rules_.erase(it);
  LOG(INFO) << "[MCP][NetworkIntercept] 규칙 삭제 완료: " << rule_id;

  std::move(callback).Run(
      MakeSuccessResult("규칙이 삭제되었습니다. ruleId: " + rule_id));
}

void NetworkInterceptTool::HandleContinueRequest(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // requestId 필수
  const std::string* request_id = arguments.FindString("requestId");
  if (!request_id || request_id->empty()) {
    LOG(WARNING) << "[MCP][NetworkIntercept] continueRequest: requestId 누락";
    std::move(callback).Run(
        MakeErrorResult("오류: continueRequest는 requestId가 필요합니다"));
    return;
  }

  // Fetch.continueRequest CDP 파라미터 구성
  base::DictValue params;
  params.Set("requestId", *request_id);

  // 선택적 오버라이드 필드 추가
  const std::string* url = arguments.FindString("url");
  if (url && !url->empty()) {
    params.Set("url", *url);
  }

  const std::string* method = arguments.FindString("method");
  if (method && !method->empty()) {
    params.Set("method", *method);
  }

  const std::string* post_data = arguments.FindString("postData");
  if (post_data && !post_data->empty()) {
    params.Set("postData", *post_data);
  }

  // 헤더: {"name":"value"} → [{name:"name", value:"value"}] 변환
  const base::DictValue* headers = arguments.FindDict("headers");
  if (headers && !headers->empty()) {
    params.Set("headers", HeaderDictToEntries(*headers));
  }

  LOG(INFO) << "[MCP][NetworkIntercept] Fetch.continueRequest: "
            << *request_id;

  // pending 목록에서 제거 (사용자가 수동으로 처리함)
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(), *request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  session->SendCdpCommand(
      "Fetch.continueRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnSimpleResponse,
                     weak_factory_.GetWeakPtr(),
                     "요청이 계속 진행되었습니다.",
                     std::move(callback)));
}

void NetworkInterceptTool::HandleFulfillRequest(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // requestId 필수
  const std::string* request_id = arguments.FindString("requestId");
  if (!request_id || request_id->empty()) {
    LOG(WARNING) << "[MCP][NetworkIntercept] fulfillRequest: requestId 누락";
    std::move(callback).Run(
        MakeErrorResult("오류: fulfillRequest는 requestId가 필요합니다"));
    return;
  }

  // Fetch.fulfillRequest CDP 파라미터 구성
  base::DictValue params;
  params.Set("requestId", *request_id);

  // 응답 코드 (기본값 200)
  const base::Value* resp_code_val = arguments.Find("responseCode");
  int response_code = 200;
  if (resp_code_val && resp_code_val->is_int()) {
    response_code = resp_code_val->GetInt();
  }
  params.Set("responseCode", response_code);

  // 응답 헤더: {"name":"value"} → [{name:"name", value:"value"}] 변환
  const base::DictValue* resp_headers = arguments.FindDict("responseHeaders");
  if (resp_headers && !resp_headers->empty()) {
    params.Set("responseHeaders", HeaderDictToEntries(*resp_headers));
  }

  // 응답 바디
  const std::string* body = arguments.FindString("body");
  if (body && !body->empty()) {
    params.Set("body", *body);
  }

  LOG(INFO) << "[MCP][NetworkIntercept] Fetch.fulfillRequest: "
            << *request_id << " (code=" << response_code << ")";

  // pending 목록에서 제거 (사용자가 수동으로 처리함)
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(), *request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  session->SendCdpCommand(
      "Fetch.fulfillRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnSimpleResponse,
                     weak_factory_.GetWeakPtr(),
                     "커스텀 응답이 반환되었습니다.",
                     std::move(callback)));
}

void NetworkInterceptTool::HandleFailRequest(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // requestId 필수
  const std::string* request_id = arguments.FindString("requestId");
  if (!request_id || request_id->empty()) {
    LOG(WARNING) << "[MCP][NetworkIntercept] failRequest: requestId 누락";
    std::move(callback).Run(
        MakeErrorResult("오류: failRequest는 requestId가 필요합니다"));
    return;
  }

  // Fetch.failRequest CDP 파라미터 구성
  base::DictValue params;
  params.Set("requestId", *request_id);

  // 에러 사유 (기본값 "BlockedByClient")
  const std::string* error_reason = arguments.FindString("errorReason");
  std::string reason = (error_reason && !error_reason->empty())
                           ? *error_reason
                           : "BlockedByClient";
  params.Set("errorReason", reason);

  LOG(INFO) << "[MCP][NetworkIntercept] Fetch.failRequest: "
            << *request_id << " reason=" << reason;

  // pending 목록에서 제거 (사용자가 수동으로 처리함)
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(), *request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  session->SendCdpCommand(
      "Fetch.failRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnSimpleResponse,
                     weak_factory_.GetWeakPtr(),
                     "요청이 차단되었습니다.",
                     std::move(callback)));
}

// ============================================================================
// CDP 응답 핸들러 구현
// ============================================================================

void NetworkInterceptTool::OnFetchEnabled(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // CDP 응답에 error 키가 있으면 실패
  if (response.is_dict()) {
    const base::DictValue* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      std::string err_text =
          msg ? *msg : "Fetch.enable 실패 (알 수 없는 오류)";
      LOG(ERROR) << "[MCP][NetworkIntercept] Fetch.enable 실패: " << err_text;
      std::move(callback).Run(
          MakeErrorResult("인터셉트 활성화 실패: " + err_text));
      return;
    }
  }

  is_intercepting_ = true;

  // Fetch.requestPaused 이벤트 핸들러 등록.
  // 세션 WeakPtr을 클로저로 캡처하여 McpSession 소멸 후 댕글링 포인터 방지.
  base::WeakPtr<McpSession> session_weak = session->GetWeakPtr();
  session->RegisterCdpEventHandler(
      "Fetch.requestPaused",
      base::BindRepeating(
          [](base::WeakPtr<NetworkInterceptTool> self,
             base::WeakPtr<McpSession> weak_session,
             const std::string& event_name,
             const base::DictValue& params) {
            if (!self) {
              return;
            }
            McpSession* s = weak_session.get();
            if (!s) {
              return;
            }
            self->OnRequestPaused(event_name, params, s);
          },
          weak_factory_.GetWeakPtr(), session_weak));

  LOG(INFO) << "[MCP][NetworkIntercept] 인터셉트 활성화 완료. "
               "Fetch.requestPaused 이벤트 대기 중.";

  std::move(callback).Run(
      MakeSuccessResult("네트워크 인터셉트가 활성화되었습니다. "
                        "Fetch.requestPaused 이벤트를 수신합니다."));
}

void NetworkInterceptTool::OnFetchDisabled(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // 에러가 있어도 상태를 비활성화로 업데이트 (일관성 유지)
  is_intercepting_ = false;

  if (response.is_dict()) {
    const base::DictValue* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      LOG(WARNING) << "[MCP][NetworkIntercept] Fetch.disable 경고: "
                   << (msg ? *msg : "알 수 없음");
    }
  }

  // 처리되지 않은 pending 요청 수 로그 기록
  if (!pending_request_ids_.empty()) {
    LOG(WARNING) << "[MCP][NetworkIntercept] 처리되지 않은 요청 "
                 << pending_request_ids_.size()
                 << "개. 인터셉트 비활성화로 자동 해제됩니다.";
    pending_request_ids_.clear();
  }

  LOG(INFO) << "[MCP][NetworkIntercept] 인터셉트 비활성화 완료.";

  std::move(callback).Run(
      MakeSuccessResult("네트워크 인터셉트가 비활성화되었습니다."));
}

void NetworkInterceptTool::OnSimpleResponse(
    const std::string& success_message,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // CDP 응답에 error 키가 있으면 실패
  if (response.is_dict()) {
    const base::DictValue* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      std::string err_text = msg ? *msg : "CDP 명령 실패 (알 수 없는 오류)";
      LOG(ERROR) << "[MCP][NetworkIntercept] CDP 명령 실패: " << err_text;
      std::move(callback).Run(MakeErrorResult("오류: " + err_text));
      return;
    }
  }

  std::move(callback).Run(MakeSuccessResult(success_message));
}

// ============================================================================
// Fetch.requestPaused 이벤트 처리 구현
// ============================================================================

void NetworkInterceptTool::OnRequestPaused(
    const std::string& event_name,
    const base::DictValue& params,
    McpSession* session) {
  if (!is_intercepting_) {
    return;
  }

  // requestId 추출 (필수)
  const std::string* request_id = params.FindString("requestId");
  if (!request_id || request_id->empty()) {
    LOG(WARNING) << "[MCP][NetworkIntercept] requestPaused: requestId 없음";
    return;
  }

  // pending 목록에 추가 (수동 처리 도구가 사용할 수 있도록)
  pending_request_ids_.push_back(*request_id);

  // 인터셉트된 요청 URL 추출
  const base::DictValue* request_obj = params.FindDict("request");
  std::string url;
  if (request_obj) {
    const std::string* url_ptr = request_obj->FindString("url");
    if (url_ptr) {
      url = *url_ptr;
    }
  }

  // 리소스 타입 추출
  const std::string* resource_type_ptr = params.FindString("resourceType");
  std::string resource_type =
      resource_type_ptr ? *resource_type_ptr : "";

  LOG(INFO) << "[MCP][NetworkIntercept] requestPaused: id=" << *request_id
            << " url=" << url << " type=" << resource_type;

  // -------------------------------------------------------------------------
  // 등록된 규칙을 순회하여 첫 번째 매칭 규칙 적용
  // -------------------------------------------------------------------------
  for (const auto& [rule_id, rule] : rules_) {
    if (!rule.enabled) {
      continue;
    }

    // URL 패턴 매칭 확인
    if (!MatchesPattern(url, rule.url_pattern)) {
      continue;
    }

    // 리소스 타입 필터 매칭 확인
    if (!MatchesResourceType(resource_type, rule.resource_type)) {
      continue;
    }

    LOG(INFO) << "[MCP][NetworkIntercept] 규칙 매칭: ruleId=" << rule_id
              << " action=" << rule.action;

    // 규칙 동작에 따라 분기 처리
    if (rule.action == "continue") {
      ApplyRuleContinue(*request_id, rule, params, session);
    } else if (rule.action == "fulfill") {
      ApplyRuleFulfill(*request_id, rule, session);
    } else if (rule.action == "fail") {
      ApplyRuleFail(*request_id, rule, session);
    } else {
      // passthrough: Fetch.continueRequest로 그냥 통과
      LOG(INFO) << "[MCP][NetworkIntercept] passthrough: " << *request_id;
      base::DictValue pass_params;
      pass_params.Set("requestId", *request_id);
      session->SendCdpCommand(
          "Fetch.continueRequest", std::move(pass_params),
          base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                         weak_factory_.GetWeakPtr(), *request_id,
                         "passthrough"));
      // pending 목록에서 제거 (자동 처리됨)
      auto pit = std::find(pending_request_ids_.begin(),
                           pending_request_ids_.end(), *request_id);
      if (pit != pending_request_ids_.end()) {
        pending_request_ids_.erase(pit);
      }
    }
    // 첫 번째 매칭 규칙만 적용 후 종료
    return;
  }

  // -------------------------------------------------------------------------
  // 매칭되는 규칙이 없으면 Fetch.continueRequest로 그냥 통과
  // -------------------------------------------------------------------------
  LOG(INFO) << "[MCP][NetworkIntercept] 매칭 규칙 없음. 통과: "
            << *request_id;

  base::DictValue pass_params;
  pass_params.Set("requestId", *request_id);
  session->SendCdpCommand(
      "Fetch.continueRequest", std::move(pass_params),
      base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                     weak_factory_.GetWeakPtr(), *request_id, "passthrough"));

  // pending 목록에서 제거 (자동으로 처리됨)
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(), *request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }
}

void NetworkInterceptTool::ApplyRuleContinue(
    const std::string& request_id,
    const InterceptRule& rule,
    const base::DictValue& paused_params,
    McpSession* session) {
  // Fetch.continueRequest 파라미터 구성
  // override 필드가 있으면 수정, 없으면 원본 그대로 통과
  base::DictValue params;
  params.Set("requestId", request_id);

  if (!rule.override_url.empty()) {
    params.Set("url", rule.override_url);
  }

  if (!rule.override_method.empty()) {
    params.Set("method", rule.override_method);
  }

  if (!rule.override_post_data.empty()) {
    params.Set("postData", rule.override_post_data);
  }

  if (!rule.override_headers.empty()) {
    params.Set("headers", HeaderDictToEntries(rule.override_headers));
  }

  LOG(INFO) << "[MCP][NetworkIntercept] ApplyRuleContinue: " << request_id;

  // pending 목록에서 제거 (자동 처리됨)
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(), request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  session->SendCdpCommand(
      "Fetch.continueRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                     weak_factory_.GetWeakPtr(), request_id, "continue"));
}

void NetworkInterceptTool::ApplyRuleFulfill(
    const std::string& request_id,
    const InterceptRule& rule,
    McpSession* session) {
  // Fetch.fulfillRequest 파라미터 구성
  base::DictValue params;
  params.Set("requestId", request_id);
  params.Set("responseCode", rule.response_code);

  if (!rule.response_headers.empty()) {
    params.Set("responseHeaders",
               HeaderDictToEntries(rule.response_headers));
  }

  if (!rule.response_body.empty()) {
    params.Set("body", rule.response_body);
  }

  LOG(INFO) << "[MCP][NetworkIntercept] ApplyRuleFulfill: " << request_id
            << " code=" << rule.response_code;

  // pending 목록에서 제거 (자동 처리됨)
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(), request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  session->SendCdpCommand(
      "Fetch.fulfillRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                     weak_factory_.GetWeakPtr(), request_id, "fulfill"));
}

void NetworkInterceptTool::ApplyRuleFail(
    const std::string& request_id,
    const InterceptRule& rule,
    McpSession* session) {
  // Fetch.failRequest 파라미터 구성
  base::DictValue params;
  params.Set("requestId", request_id);

  std::string reason =
      rule.error_reason.empty() ? "BlockedByClient" : rule.error_reason;
  params.Set("errorReason", reason);

  LOG(INFO) << "[MCP][NetworkIntercept] ApplyRuleFail: " << request_id
            << " reason=" << reason;

  // pending 목록에서 제거 (자동 처리됨)
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(), request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  session->SendCdpCommand(
      "Fetch.failRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                     weak_factory_.GetWeakPtr(), request_id, "fail"));
}

void NetworkInterceptTool::OnAutoHandleResponse(
    const std::string& request_id,
    const std::string& action,
    base::Value response) {
  // 자동 처리 결과를 조용히 로그에만 기록 (MCP 클라이언트에 응답 불필요)
  if (response.is_dict()) {
    const base::DictValue* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      LOG(WARNING) << "[MCP][NetworkIntercept] 자동 처리 실패: "
                   << "requestId=" << request_id << " action=" << action
                   << " error=" << (msg ? *msg : "알 수 없음");
      return;
    }
  }

  LOG(INFO) << "[MCP][NetworkIntercept] 자동 처리 완료: "
            << "requestId=" << request_id << " action=" << action;
}

// ============================================================================
// 유틸리티 메서드 구현
// ============================================================================

// static
bool NetworkInterceptTool::MatchesPattern(const std::string& url,
                                          const std::string& pattern) {
  // 빈 패턴이면 모든 URL 매칭
  if (pattern.empty() || pattern == "*") {
    return true;
  }

  // 단순 글로브 매칭: '*' 기준으로 분리하여 순서대로 포함 여부 확인
  // 예: "*.js" → url이 ".js"로 끝나면 매칭
  // 예: "https://api.example.com/*" → 접두사 매칭
  // 예: "*.doubleclick.net/*" → 중간 포함 매칭
  std::vector<std::string_view> parts = base::SplitStringPiece(
      pattern, "*", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  if (parts.empty()) {
    // 패턴이 '*'만으로 구성된 경우 → 전체 매칭
    return true;
  }

  bool starts_with_wildcard = (pattern.front() == '*');
  bool ends_with_wildcard = (pattern.back() == '*');

  size_t search_pos = 0;

  for (size_t i = 0; i < parts.size(); ++i) {
    const auto& part = parts[i];
    if (part.empty()) {
      continue;
    }

    size_t found = url.find(part, search_pos);
    if (found == std::string::npos) {
      return false;
    }

    // 패턴이 '*'로 시작하지 않으면 첫 파트는 URL의 시작과 일치해야 함
    if (i == 0 && !starts_with_wildcard && found != 0) {
      return false;
    }

    search_pos = found + part.size();
  }

  // 패턴이 '*'로 끝나지 않으면 마지막 파트가 URL의 끝과 일치해야 함
  if (!ends_with_wildcard && !parts.empty()) {
    const auto& last_part = parts.back();
    if (!base::EndsWith(url, last_part, base::CompareCase::SENSITIVE)) {
      return false;
    }
  }

  return true;
}

// static
bool NetworkInterceptTool::MatchesResourceType(
    const std::string& resource_type,
    const std::string& filter) {
  // 빈 필터이면 모든 타입 매칭
  if (filter.empty()) {
    return true;
  }

  // 대소문자 무시 비교 (CDP는 PascalCase를 사용하므로 비교 시 정규화)
  return base::EqualsCaseInsensitiveASCII(resource_type, filter);
}

// static
base::ListValue NetworkInterceptTool::HeaderDictToEntries(
    const base::DictValue& headers) {
  // {"Content-Type": "application/json", "Authorization": "Bearer token"}
  // →
  // [{"name": "Content-Type", "value": "application/json"},
  //  {"name": "Authorization", "value": "Bearer token"}]
  //
  // Fetch.continueRequest / Fetch.fulfillRequest의 headerEntries 형식 요구사항
  base::ListValue entries;

  for (const auto [name, value_node] : headers) {
    // 값이 문자열인 경우만 변환 (비문자열 값은 무시)
    if (!value_node.is_string()) {
      continue;
    }

    base::DictValue entry;
    entry.Set("name", name);
    entry.Set("value", value_node.GetString());
    entries.Append(std::move(entry));
  }

  return entries;
}

// static
base::Value NetworkInterceptTool::MakeErrorResult(
    const std::string& message) {
  // MCP tools/call 에러 응답 형식:
  // {"isError": true, "content": [{"type": "text", "text": "오류 메시지"}]}
  base::DictValue result;
  result.Set("isError", true);
  base::ListValue content;
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text", message);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));
  return base::Value(std::move(result));
}

// static
base::Value NetworkInterceptTool::MakeSuccessResult(
    const std::string& message) {
  // MCP tools/call 성공 응답 형식:
  // {"content": [{"type": "text", "text": "성공 메시지"}]}
  base::DictValue result;
  base::ListValue content;
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text", message);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));
  return base::Value(std::move(result));
}

}  // namespace mcp
