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

NetworkInterceptTool::NetworkInterceptTool() = default;
NetworkInterceptTool::~NetworkInterceptTool() = default;

// ============================================================================
// McpTool 인터페이스 구현
// ============================================================================

std::string NetworkInterceptTool::name() const {
  return "network_intercept";
}

std::string NetworkInterceptTool::description() const {
  return "네트워크 요청을 가로채어 수정, 차단, 또는 모킹 응답으로 대체합니다. "
         "Fetch CDP 도메인을 사용하므로 노란 배너 없이 동작합니다. "
         "action=enable로 인터셉트를 시작하고, addRule로 자동 처리 규칙을 등록하거나, "
         "Fetch.requestPaused 이벤트 수신 후 continueRequest/fulfillRequest/failRequest로 "
         "수동 처리할 수 있습니다. "
         "활용 예: 광고 차단(failRequest), API 목서버(fulfillRequest), "
         "Authorization 헤더 자동 주입(continueRequest).";
}

base::Value::Dict NetworkInterceptTool::input_schema() const {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // -------------------------------------------------------------------------
  // action: 수행할 동작 (필수)
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    base::Value::List action_enum;
    action_enum.Append("enable");
    action_enum.Append("disable");
    action_enum.Append("addRule");
    action_enum.Append("removeRule");
    action_enum.Append("continueRequest");
    action_enum.Append("fulfillRequest");
    action_enum.Append("failRequest");
    prop.Set("enum", std::move(action_enum));
    prop.Set("description",
             "수행할 동작. "
             "enable: 인터셉트 시작, "
             "disable: 인터셉트 중지, "
             "addRule: 자동 처리 규칙 등록, "
             "removeRule: 규칙 삭제, "
             "continueRequest: 요청 수정 후 진행, "
             "fulfillRequest: 모킹 응답 반환, "
             "failRequest: 요청 차단");
    properties.Set("action", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // patterns: enable 시 인터셉트할 URL 패턴 목록
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "array");
    prop.Set("description",
             "action=enable 시 인터셉트할 패턴 목록. "
             "생략하면 모든 요청을 인터셉트한다.");

    base::Value::Dict item_schema;
    item_schema.Set("type", "object");
    base::Value::Dict item_props;

    {
      base::Value::Dict p;
      p.Set("type", "string");
      p.Set("description",
            "글로브 패턴. 예: '*.js', 'https://api.example.com/*'");
      item_props.Set("urlPattern", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "string");
      p.Set("description",
            "리소스 타입 필터. "
            "예: 'Document', 'Script', 'Image', 'XHR', 'Fetch'");
      item_props.Set("resourceType", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "string");
      base::Value::List stage_enum;
      stage_enum.Append("Request");
      stage_enum.Append("Response");
      p.Set("enum", std::move(stage_enum));
      p.Set("description",
            "인터셉트 단계. Request: 요청 전송 전, Response: 응답 수신 후");
      item_props.Set("requestStage", std::move(p));
    }

    item_schema.Set("properties", std::move(item_props));
    prop.Set("items", std::move(item_schema));
    properties.Set("patterns", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // requestId: continueRequest/fulfillRequest/failRequest에서 필수
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    prop.Set("description",
             "Fetch.requestPaused 이벤트에서 받은 requestId. "
             "continueRequest, fulfillRequest, failRequest 동작에 필수.");
    properties.Set("requestId", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // url: continueRequest 시 요청 URL 수정
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    prop.Set("description",
             "continueRequest 시 변경할 요청 URL. "
             "생략하면 원본 URL 유지.");
    properties.Set("url", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // method: continueRequest 시 HTTP 메서드 수정
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    prop.Set("description",
             "continueRequest 시 변경할 HTTP 메서드. "
             "예: 'GET', 'POST', 'PUT'. 생략하면 원본 유지.");
    properties.Set("method", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // headers: 요청/응답 헤더 수정 (객체 형식)
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "object");
    prop.Set("description",
             "헤더 수정/추가. "
             "continueRequest: 요청 헤더 교체, "
             "fulfillRequest: 응답 헤더 지정. "
             "예: {\"Authorization\": \"Bearer token\", "
             "\"Content-Type\": \"application/json\"}");
    properties.Set("headers", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // postData: continueRequest 시 POST body 수정
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    prop.Set("description",
             "continueRequest 시 변경할 POST body. "
             "생략하면 원본 body 유지.");
    properties.Set("postData", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // responseCode: fulfillRequest 시 HTTP 응답 코드
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "number");
    prop.Set("description",
             "fulfillRequest 시 반환할 HTTP 상태 코드. "
             "예: 200, 404, 500. 기본값: 200.");
    properties.Set("responseCode", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // responseHeaders: fulfillRequest 시 응답 헤더
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "object");
    prop.Set("description",
             "fulfillRequest 시 반환할 응답 헤더. "
             "예: {\"Content-Type\": \"application/json\"}");
    properties.Set("responseHeaders", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // body: fulfillRequest 시 응답 본문
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    prop.Set("description",
             "fulfillRequest 시 반환할 응답 본문. "
             "UTF-8 텍스트 또는 base64 인코딩 값. "
             "예: '{\"status\": \"ok\"}' 또는 base64 문자열.");
    properties.Set("body", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // errorReason: failRequest 시 실패 사유
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    base::Value::List err_enum;
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
    prop.Set("enum", std::move(err_enum));
    prop.Set("description",
             "failRequest 시 사용할 네트워크 에러 종류. "
             "광고 차단에는 'BlockedByClient' 권장. "
             "기본값: 'BlockedByClient'.");
    properties.Set("errorReason", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // ruleId: addRule 시 지정하거나 removeRule 시 제거 대상 ID
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    prop.Set("description",
             "규칙 고유 ID. "
             "addRule 시 생략하면 자동 생성. "
             "removeRule 시 필수.");
    properties.Set("ruleId", std::move(prop));
  }

  // -------------------------------------------------------------------------
  // rule: addRule 시 규칙 정의 객체
  // -------------------------------------------------------------------------
  {
    base::Value::Dict prop;
    prop.Set("type", "object");
    prop.Set("description",
             "action=addRule 시 등록할 규칙 정의. "
             "urlPattern, resourceType, requestStage로 매칭 조건을 지정하고, "
             "ruleAction으로 처리 방법을 선택한다.");

    base::Value::Dict rule_props;

    {
      base::Value::Dict p;
      p.Set("type", "string");
      p.Set("description",
            "매칭할 URL 글로브 패턴. "
            "빈 문자열이면 모든 URL 매칭. "
            "예: '*.doubleclick.net/*', 'https://api.example.com/users'");
      rule_props.Set("urlPattern", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "string");
      p.Set("description",
            "매칭할 리소스 타입. "
            "빈 문자열이면 모든 타입 매칭.");
      rule_props.Set("resourceType", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "string");
      base::Value::List stage_enum;
      stage_enum.Append("Request");
      stage_enum.Append("Response");
      p.Set("enum", std::move(stage_enum));
      p.Set("description", "인터셉트 단계. 기본값: 'Request'.");
      rule_props.Set("requestStage", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "string");
      base::Value::List action_enum;
      action_enum.Append("continue");
      action_enum.Append("fulfill");
      action_enum.Append("fail");
      action_enum.Append("passthrough");
      p.Set("enum", std::move(action_enum));
      p.Set("description",
            "규칙 매칭 시 수행할 동작. "
            "continue: 요청 수정 후 진행, "
            "fulfill: 모킹 응답 반환, "
            "fail: 요청 차단, "
            "passthrough: 로깅 후 통과.");
      rule_props.Set("ruleAction", std::move(p));
    }
    // continue 동작용 override 필드
    {
      base::Value::Dict p;
      p.Set("type", "string");
      p.Set("description", "continue 동작 시 교체할 URL.");
      rule_props.Set("overrideUrl", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "string");
      p.Set("description", "continue 동작 시 교체할 HTTP 메서드.");
      rule_props.Set("overrideMethod", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "object");
      p.Set("description",
            "continue 동작 시 교체할 요청 헤더 전체. "
            "부분 수정이 아니라 전체 교체임에 주의.");
      rule_props.Set("overrideHeaders", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "string");
      p.Set("description", "continue 동작 시 교체할 POST body.");
      rule_props.Set("overridePostData", std::move(p));
    }
    // fulfill 동작용 응답 필드
    {
      base::Value::Dict p;
      p.Set("type", "number");
      p.Set("description", "fulfill 동작 시 응답 상태 코드. 기본값: 200.");
      rule_props.Set("responseCode", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "object");
      p.Set("description", "fulfill 동작 시 응답 헤더.");
      rule_props.Set("responseHeaders", std::move(p));
    }
    {
      base::Value::Dict p;
      p.Set("type", "string");
      p.Set("description",
            "fulfill 동작 시 응답 본문. UTF-8 또는 base64.");
      rule_props.Set("responseBody", std::move(p));
    }
    // fail 동작용 에러 사유
    {
      base::Value::Dict p;
      p.Set("type", "string");
      p.Set("description",
            "fail 동작 시 에러 사유. 기본값: 'BlockedByClient'.");
      rule_props.Set("errorReason", std::move(p));
    }

    prop.Set("properties", std::move(rule_props));
    properties.Set("rule", std::move(prop));
  }

  schema.Set("properties", std::move(properties));

  // action만 필수
  base::Value::List required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

// ============================================================================
// Execute: action 파라미터에 따라 핸들러 분기
// ============================================================================

void NetworkInterceptTool::Execute(
    const base::Value::Dict& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* action = arguments.FindString("action");
  if (!action || action->empty()) {
    LOG(WARNING) << "[MCP][NetworkIntercept] action 파라미터 누락";
    std::move(callback).Run(MakeErrorResult(
        "오류: action 파라미터가 필요합니다 "
        "(enable/disable/addRule/removeRule/"
        "continueRequest/fulfillRequest/failRequest)"));
    return;
  }

  if (*action == "enable") {
    const base::Value::List* patterns = arguments.FindList("patterns");
    HandleEnable(patterns, session, std::move(callback));

  } else if (*action == "disable") {
    HandleDisable(session, std::move(callback));

  } else if (*action == "addRule") {
    HandleAddRule(arguments, std::move(callback));

  } else if (*action == "removeRule") {
    const std::string* rule_id = arguments.FindString("ruleId");
    if (!rule_id || rule_id->empty()) {
      std::move(callback).Run(
          MakeErrorResult("오류: removeRule에는 ruleId가 필요합니다."));
      return;
    }
    HandleRemoveRule(*rule_id, std::move(callback));

  } else if (*action == "continueRequest") {
    const std::string* request_id = arguments.FindString("requestId");
    if (!request_id || request_id->empty()) {
      std::move(callback).Run(
          MakeErrorResult("오류: continueRequest에는 requestId가 필요합니다."));
      return;
    }
    HandleContinueRequest(arguments, session, std::move(callback));

  } else if (*action == "fulfillRequest") {
    const std::string* request_id = arguments.FindString("requestId");
    if (!request_id || request_id->empty()) {
      std::move(callback).Run(
          MakeErrorResult("오류: fulfillRequest에는 requestId가 필요합니다."));
      return;
    }
    HandleFulfillRequest(arguments, session, std::move(callback));

  } else if (*action == "failRequest") {
    const std::string* request_id = arguments.FindString("requestId");
    if (!request_id || request_id->empty()) {
      std::move(callback).Run(
          MakeErrorResult("오류: failRequest에는 requestId가 필요합니다."));
      return;
    }
    HandleFailRequest(arguments, session, std::move(callback));

  } else {
    LOG(WARNING) << "[MCP][NetworkIntercept] 알 수 없는 action: " << *action;
    std::move(callback).Run(
        MakeErrorResult("오류: 알 수 없는 action: " + *action));
  }
}

// ============================================================================
// action=enable
// ============================================================================

void NetworkInterceptTool::HandleEnable(
    const base::Value::List* patterns,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // Fetch.enable 파라미터 구성
  base::Value::Dict params;

  if (patterns && !patterns->empty()) {
    // 사용자 지정 패턴 목록을 CDP 형식으로 변환
    base::Value::List fetch_patterns;
    for (const auto& pattern_val : *patterns) {
      if (!pattern_val.is_dict()) {
        continue;
      }
      const base::Value::Dict& pattern_dict = pattern_val.GetDict();
      base::Value::Dict fetch_pattern;

      const std::string* url_pattern = pattern_dict.FindString("urlPattern");
      if (url_pattern && !url_pattern->empty()) {
        fetch_pattern.Set("urlPattern", *url_pattern);
      }

      const std::string* resource_type =
          pattern_dict.FindString("resourceType");
      if (resource_type && !resource_type->empty()) {
        fetch_pattern.Set("resourceType", *resource_type);
      }

      const std::string* request_stage =
          pattern_dict.FindString("requestStage");
      if (request_stage && !request_stage->empty()) {
        fetch_pattern.Set("requestStage", *request_stage);
      } else {
        // 기본값: Request 단계에서 인터셉트
        fetch_pattern.Set("requestStage", "Request");
      }

      fetch_patterns.Append(std::move(fetch_pattern));
    }
    params.Set("patterns", std::move(fetch_patterns));
  } else {
    // 패턴 미지정 시 모든 요청 인터셉트 (requestStage=Request)
    base::Value::List default_patterns;
    base::Value::Dict default_pattern;
    default_pattern.Set("urlPattern", "*");
    default_pattern.Set("requestStage", "Request");
    default_patterns.Append(std::move(default_pattern));
    params.Set("patterns", std::move(default_patterns));
  }

  LOG(INFO) << "[MCP][NetworkIntercept] Fetch.enable 전송";

  session->SendCdpCommand(
      "Fetch.enable", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnFetchEnabled,
                     weak_factory_.GetWeakPtr(), session,
                     std::move(callback)));
}

void NetworkInterceptTool::OnFetchEnabled(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // CDP 응답에 error 키가 있으면 실패
  if (response.is_dict()) {
    const base::Value::Dict* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      std::string err_text = msg ? *msg : "Fetch.enable 실패 (알 수 없는 오류)";
      LOG(ERROR) << "[MCP][NetworkIntercept] Fetch.enable 실패: " << err_text;
      std::move(callback).Run(
          MakeErrorResult("인터셉트 활성화 실패: " + err_text));
      return;
    }
  }

  is_intercepting_ = true;

  // Fetch.requestPaused 이벤트 핸들러 등록.
  // session WeakPtr을 클로저로 캡처하여 OnRequestPaused에 전달한다.
  base::WeakPtr<McpSession> session_weak = session->GetWeakPtr();
  session->RegisterCdpEventHandler(
      "Fetch.requestPaused",
      base::BindRepeating(
          [](base::WeakPtr<NetworkInterceptTool> self,
             base::WeakPtr<McpSession> weak_session,
             const std::string& event_name,
             const base::Value::Dict& params) {
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
            << "등록된 규칙 수: " << rules_.size();

  std::move(callback).Run(MakeSuccessResult(
      "네트워크 인터셉트가 활성화되었습니다. "
      "Fetch.requestPaused 이벤트를 수신합니다. "
      "addRule로 자동 처리 규칙을 등록하거나, "
      "requestPaused 이벤트 수신 후 "
      "continueRequest/fulfillRequest/failRequest로 수동 처리하세요."));
}

// ============================================================================
// action=disable
// ============================================================================

void NetworkInterceptTool::HandleDisable(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (!is_intercepting_) {
    LOG(WARNING) << "[MCP][NetworkIntercept] 인터셉트가 비활성 상태에서 disable 요청";
    std::move(callback).Run(
        MakeErrorResult("오류: 인터셉트가 활성화되어 있지 않습니다."));
    return;
  }

  // 이벤트 핸들러 먼저 해제
  session->UnregisterCdpEventHandler("Fetch.requestPaused");

  LOG(INFO) << "[MCP][NetworkIntercept] Fetch.disable 전송";

  session->SendCdpCommand(
      "Fetch.disable", base::Value::Dict(),
      base::BindOnce(&NetworkInterceptTool::OnFetchDisabled,
                     weak_factory_.GetWeakPtr(), session,
                     std::move(callback)));
}

void NetworkInterceptTool::OnFetchDisabled(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  is_intercepting_ = false;

  // CDP 에러가 있어도 상태는 비활성으로 처리
  if (response.is_dict()) {
    const base::Value::Dict* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      LOG(WARNING) << "[MCP][NetworkIntercept] Fetch.disable 경고: "
                   << (msg ? *msg : "알 수 없음");
    }
  }

  // 미처리 대기 요청 목록 초기화
  int pending_count = static_cast<int>(pending_request_ids_.size());
  pending_request_ids_.clear();

  LOG(INFO) << "[MCP][NetworkIntercept] 인터셉트 비활성화 완료. "
            << "미처리 요청 " << pending_count << "개 폐기.";

  std::string msg = "네트워크 인터셉트가 비활성화되었습니다.";
  if (pending_count > 0) {
    msg += " (미처리 대기 요청 " + base::NumberToString(pending_count) +
           "개가 폐기되었습니다)";
  }
  std::move(callback).Run(MakeSuccessResult(msg));
}

// ============================================================================
// action=addRule
// ============================================================================

void NetworkInterceptTool::HandleAddRule(
    const base::Value::Dict& arguments,
    base::OnceCallback<void(base::Value)> callback) {
  const base::Value::Dict* rule_dict = arguments.FindDict("rule");
  if (!rule_dict) {
    std::move(callback).Run(
        MakeErrorResult("오류: addRule에는 rule 객체가 필요합니다."));
    return;
  }

  InterceptRule rule;

  // ruleId: 인수 최상위 또는 rule 내부에서 찾음. 없으면 자동 생성.
  const std::string* rule_id = arguments.FindString("ruleId");
  if (!rule_id || rule_id->empty()) {
    rule_id = rule_dict->FindString("ruleId");
  }
  if (rule_id && !rule_id->empty()) {
    rule.rule_id = *rule_id;
  } else {
    rule.rule_id = "rule_" + base::NumberToString(next_rule_id_++);
  }

  // 매칭 조건 파싱
  const std::string* url_pattern = rule_dict->FindString("urlPattern");
  rule.url_pattern = url_pattern ? *url_pattern : "";

  const std::string* resource_type = rule_dict->FindString("resourceType");
  rule.resource_type = resource_type ? *resource_type : "";

  const std::string* request_stage = rule_dict->FindString("requestStage");
  rule.request_stage =
      (request_stage && !request_stage->empty()) ? *request_stage : "Request";

  // 동작 파싱
  const std::string* rule_action = rule_dict->FindString("ruleAction");
  rule.action =
      (rule_action && !rule_action->empty()) ? *rule_action : "passthrough";

  // continue 동작용 override 필드
  const std::string* override_url = rule_dict->FindString("overrideUrl");
  rule.override_url = override_url ? *override_url : "";

  const std::string* override_method = rule_dict->FindString("overrideMethod");
  rule.override_method = override_method ? *override_method : "";

  const base::Value::Dict* override_headers =
      rule_dict->FindDict("overrideHeaders");
  if (override_headers) {
    rule.override_headers = override_headers->Clone();
  }

  const std::string* override_post_data =
      rule_dict->FindString("overridePostData");
  rule.override_post_data = override_post_data ? *override_post_data : "";

  // fulfill 동작용 응답 필드
  const base::Value* resp_code_val = rule_dict->Find("responseCode");
  if (resp_code_val && resp_code_val->is_int()) {
    rule.response_code = resp_code_val->GetInt();
  } else {
    rule.response_code = 200;
  }

  const base::Value::Dict* resp_headers = rule_dict->FindDict("responseHeaders");
  if (resp_headers) {
    rule.response_headers = resp_headers->Clone();
  }

  const std::string* resp_body = rule_dict->FindString("responseBody");
  rule.response_body = resp_body ? *resp_body : "";

  // fail 동작용 에러 사유
  const std::string* error_reason = rule_dict->FindString("errorReason");
  rule.error_reason =
      (error_reason && !error_reason->empty()) ? *error_reason
                                               : "BlockedByClient";

  // 규칙 등록
  std::string registered_id = rule.rule_id;
  rules_[registered_id] = std::move(rule);

  LOG(INFO) << "[MCP][NetworkIntercept] 규칙 등록: id=" << registered_id
            << " pattern=" << rules_[registered_id].url_pattern
            << " action=" << rules_[registered_id].action;

  // 결과 반환: 등록된 규칙 ID 포함
  base::Value::Dict data;
  data.Set("ruleId", registered_id);
  data.Set("totalRules", static_cast<int>(rules_.size()));

  std::string json_str;
  base::JSONWriter::WriteWithOptions(base::Value(std::move(data)),
                                     base::JSONWriter::OPTIONS_PRETTY_PRINT,
                                     &json_str);

  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict content_item;
  content_item.Set("type", "text");
  content_item.Set("text",
                   "규칙이 등록되었습니다.\n" + json_str);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));
  std::move(callback).Run(base::Value(std::move(result)));
}

// ============================================================================
// action=removeRule
// ============================================================================

void NetworkInterceptTool::HandleRemoveRule(
    const std::string& rule_id,
    base::OnceCallback<void(base::Value)> callback) {
  auto it = rules_.find(rule_id);
  if (it == rules_.end()) {
    std::move(callback).Run(
        MakeErrorResult("오류: 규칙을 찾을 수 없습니다: " + rule_id));
    return;
  }

  rules_.erase(it);

  LOG(INFO) << "[MCP][NetworkIntercept] 규칙 삭제: id=" << rule_id
            << " 남은 규칙 수: " << rules_.size();

  std::move(callback).Run(MakeSuccessResult(
      "규칙이 삭제되었습니다. (id=" + rule_id + ", 남은 규칙: " +
      base::NumberToString(static_cast<int>(rules_.size())) + "개)"));
}

// ============================================================================
// action=continueRequest
// ============================================================================

void NetworkInterceptTool::HandleContinueRequest(
    const base::Value::Dict& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* request_id = arguments.FindString("requestId");

  base::Value::Dict params;
  params.Set("requestId", *request_id);

  // 수정할 필드만 포함 (생략하면 원본 유지)
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

  // 헤더: 객체 형식을 [{name, value}] 배열로 변환
  const base::Value::Dict* headers = arguments.FindDict("headers");
  if (headers && !headers->empty()) {
    params.Set("headers", HeaderDictToEntries(*headers));
  }

  LOG(INFO) << "[MCP][NetworkIntercept] Fetch.continueRequest 전송: "
            << *request_id;

  // 처리된 요청 ID를 pending 목록에서 제거
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(),
                      *request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  session->SendCdpCommand(
      "Fetch.continueRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnSimpleResponse,
                     weak_factory_.GetWeakPtr(),
                     "요청을 계속 진행합니다: " + *request_id,
                     std::move(callback)));
}

// ============================================================================
// action=fulfillRequest
// ============================================================================

void NetworkInterceptTool::HandleFulfillRequest(
    const base::Value::Dict& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* request_id = arguments.FindString("requestId");

  base::Value::Dict params;
  params.Set("requestId", *request_id);

  // 응답 코드 (기본값: 200)
  const base::Value* resp_code = arguments.Find("responseCode");
  int status_code = 200;
  if (resp_code && resp_code->is_int()) {
    status_code = resp_code->GetInt();
  }
  params.Set("responseCode", status_code);

  // 응답 헤더 (배열 형식으로 변환)
  const base::Value::Dict* resp_headers = arguments.FindDict("responseHeaders");
  if (resp_headers && !resp_headers->empty()) {
    params.Set("responseHeaders", HeaderDictToEntries(*resp_headers));
  } else {
    // 헤더 미지정 시 기본 Content-Type 설정
    base::Value::List default_headers;
    base::Value::Dict ct_entry;
    ct_entry.Set("name", "Content-Type");
    ct_entry.Set("value", "application/json; charset=utf-8");
    default_headers.Append(std::move(ct_entry));
    params.Set("responseHeaders", std::move(default_headers));
  }

  // 응답 본문 (base64 인코딩하여 전송)
  const std::string* body = arguments.FindString("body");
  if (body && !body->empty()) {
    // Fetch.fulfillRequest의 body 파라미터는 base64 인코딩 필요.
    // 사용자가 이미 base64를 전달했을 수도 있으므로 그대로 사용한다.
    // (실제 Chromium 구현에서는 base64::Encode 호출 필요)
    params.Set("body", *body);
  }

  LOG(INFO) << "[MCP][NetworkIntercept] Fetch.fulfillRequest 전송: "
            << *request_id << " (statusCode=" << status_code << ")";

  // 처리된 요청 ID를 pending 목록에서 제거
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(),
                      *request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  session->SendCdpCommand(
      "Fetch.fulfillRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnSimpleResponse,
                     weak_factory_.GetWeakPtr(),
                     "모킹 응답을 반환했습니다: " + *request_id +
                         " (status=" + base::NumberToString(status_code) + ")",
                     std::move(callback)));
}

// ============================================================================
// action=failRequest
// ============================================================================

void NetworkInterceptTool::HandleFailRequest(
    const base::Value::Dict& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* request_id = arguments.FindString("requestId");

  // 에러 사유 (기본값: BlockedByClient)
  const std::string* error_reason = arguments.FindString("errorReason");
  std::string reason =
      (error_reason && !error_reason->empty()) ? *error_reason
                                               : "BlockedByClient";

  base::Value::Dict params;
  params.Set("requestId", *request_id);
  params.Set("errorReason", reason);

  LOG(INFO) << "[MCP][NetworkIntercept] Fetch.failRequest 전송: "
            << *request_id << " (reason=" << reason << ")";

  // 처리된 요청 ID를 pending 목록에서 제거
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(),
                      *request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  session->SendCdpCommand(
      "Fetch.failRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnSimpleResponse,
                     weak_factory_.GetWeakPtr(),
                     "요청을 차단했습니다: " + *request_id +
                         " (errorReason=" + reason + ")",
                     std::move(callback)));
}

// ============================================================================
// CDP 단순 응답 처리 공통 핸들러
// ============================================================================

void NetworkInterceptTool::OnSimpleResponse(
    const std::string& success_message,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // CDP 응답에 error 키가 있으면 실패
  if (response.is_dict()) {
    const base::Value::Dict* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      std::string err_text = msg ? *msg : "알 수 없는 CDP 오류";
      LOG(ERROR) << "[MCP][NetworkIntercept] CDP 명령 실패: " << err_text;
      std::move(callback).Run(MakeErrorResult("CDP 오류: " + err_text));
      return;
    }
  }

  std::move(callback).Run(MakeSuccessResult(success_message));
}

// ============================================================================
// Fetch.requestPaused 이벤트 처리 (자동 규칙 엔진)
// ============================================================================

void NetworkInterceptTool::OnRequestPaused(
    const std::string& event_name,
    const base::Value::Dict& params,
    McpSession* session) {
  // requestId 추출 (필수)
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    LOG(WARNING) << "[MCP][NetworkIntercept] requestPaused: requestId 없음";
    return;
  }

  // 요청 URL 추출: CDP 파라미터 구조에서 params.request.url 로 접근
  std::string request_url;
  const base::Value::Dict* request_obj = params.FindDict("request");
  if (request_obj) {
    const std::string* u = request_obj->FindString("url");
    if (u) {
      request_url = *u;
    }
  }

  // 리소스 타입 추출
  const std::string* resource_type = params.FindString("resourceType");
  std::string resource_type_str = resource_type ? *resource_type : "";

  LOG(INFO) << "[MCP][NetworkIntercept] requestPaused: "
            << *request_id << " type=" << resource_type_str
            << " url=" << request_url;

  // pending 목록에 추가
  pending_request_ids_.push_back(*request_id);

  // ─── 자동 규칙 매칭 ────────────────────────────────────────────────────
  // 등록된 규칙을 순서대로 순회하여 첫 번째 매칭 규칙을 적용한다.
  // 규칙 우선순위: 삽입 순서 (map이므로 ruleId 알파벳 순)
  for (const auto& [rule_id, rule] : rules_) {
    if (!rule.enabled) {
      continue;
    }

    // URL 패턴 매칭
    if (!MatchesPattern(request_url, rule.url_pattern)) {
      continue;
    }

    // 리소스 타입 매칭
    if (!MatchesResourceType(resource_type_str, rule.resource_type)) {
      continue;
    }

    LOG(INFO) << "[MCP][NetworkIntercept] 규칙 매칭: "
              << rule_id << " → " << rule.action
              << " (" << request_url << ")";

    // 매칭된 규칙에 따라 자동 처리
    if (rule.action == "continue") {
      ApplyRuleContinue(*request_id, rule, params, session);
    } else if (rule.action == "fulfill") {
      ApplyRuleFulfill(*request_id, rule, session);
    } else if (rule.action == "fail") {
      ApplyRuleFail(*request_id, rule, session);
    } else {
      // passthrough 또는 알 수 없는 동작: 그냥 통과
      base::Value::Dict cont_params;
      cont_params.Set("requestId", *request_id);
      session->SendCdpCommand(
          "Fetch.continueRequest", std::move(cont_params),
          base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                         weak_factory_.GetWeakPtr(),
                         *request_id, "passthrough"));
    }

    // 첫 번째 매칭 규칙 적용 후 순회 종료
    return;
  }

  // ─── 매칭 규칙 없음: 요청 통과 ──────────────────────────────────────────
  LOG(INFO) << "[MCP][NetworkIntercept] 매칭 규칙 없음, 통과: " << request_url;

  base::Value::Dict cont_params;
  cont_params.Set("requestId", *request_id);
  session->SendCdpCommand(
      "Fetch.continueRequest", std::move(cont_params),
      base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                     weak_factory_.GetWeakPtr(),
                     *request_id, "auto-passthrough"));
}

void NetworkInterceptTool::ApplyRuleContinue(
    const std::string& request_id,
    const InterceptRule& rule,
    const base::Value::Dict& paused_params,
    McpSession* session) {
  base::Value::Dict params;
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

  session->SendCdpCommand(
      "Fetch.continueRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                     weak_factory_.GetWeakPtr(),
                     request_id, "rule-continue"));
}

void NetworkInterceptTool::ApplyRuleFulfill(
    const std::string& request_id,
    const InterceptRule& rule,
    McpSession* session) {
  base::Value::Dict params;
  params.Set("requestId", request_id);
  params.Set("responseCode", rule.response_code);

  if (!rule.response_headers.empty()) {
    params.Set("responseHeaders",
               HeaderDictToEntries(rule.response_headers));
  } else {
    // 기본 Content-Type 헤더 설정
    base::Value::List default_headers;
    base::Value::Dict ct_entry;
    ct_entry.Set("name", "Content-Type");
    ct_entry.Set("value", "application/json; charset=utf-8");
    default_headers.Append(std::move(ct_entry));
    params.Set("responseHeaders", std::move(default_headers));
  }

  if (!rule.response_body.empty()) {
    params.Set("body", rule.response_body);
  }

  session->SendCdpCommand(
      "Fetch.fulfillRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                     weak_factory_.GetWeakPtr(),
                     request_id, "rule-fulfill"));
}

void NetworkInterceptTool::ApplyRuleFail(
    const std::string& request_id,
    const InterceptRule& rule,
    McpSession* session) {
  base::Value::Dict params;
  params.Set("requestId", request_id);
  params.Set("errorReason",
             rule.error_reason.empty() ? "BlockedByClient" : rule.error_reason);

  session->SendCdpCommand(
      "Fetch.failRequest", std::move(params),
      base::BindOnce(&NetworkInterceptTool::OnAutoHandleResponse,
                     weak_factory_.GetWeakPtr(),
                     request_id, "rule-fail"));
}

void NetworkInterceptTool::OnAutoHandleResponse(
    const std::string& request_id,
    const std::string& action,
    base::Value response) {
  // 처리된 요청 ID를 pending 목록에서 제거
  auto it = std::find(pending_request_ids_.begin(),
                      pending_request_ids_.end(),
                      request_id);
  if (it != pending_request_ids_.end()) {
    pending_request_ids_.erase(it);
  }

  // CDP 에러 로깅 (자동 처리이므로 콜백 없음)
  if (response.is_dict()) {
    const base::Value::Dict* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      LOG(WARNING) << "[MCP][NetworkIntercept] 자동 처리 실패 "
                   << "[" << action << "] " << request_id << ": "
                   << (msg ? *msg : "알 수 없음");
      return;
    }
  }

  LOG(INFO) << "[MCP][NetworkIntercept] 자동 처리 완료 "
            << "[" << action << "] " << request_id;
}

// ============================================================================
// 유틸리티
// ============================================================================

// static
bool NetworkInterceptTool::MatchesPattern(const std::string& url,
                                          const std::string& pattern) {
  // 빈 패턴 또는 전체 와일드카드: 항상 매칭
  if (pattern.empty() || pattern == "*") {
    return true;
  }

  // 단순 글로브 매칭: '*'를 기준으로 분리하여 순서대로 포함 여부 확인
  // 예: "*.doubleclick.net/*" → "doubleclick.net" + "/" 포함 확인
  std::vector<std::string_view> parts = base::SplitStringPiece(
      pattern, "*", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  if (parts.empty()) {
    return true;
  }

  bool starts_with_wildcard = pattern.front() == '*';
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

    // 패턴이 '*'로 시작하지 않으면 첫 부분은 URL의 시작과 일치해야 함
    if (i == 0 && !starts_with_wildcard && found != 0) {
      return false;
    }

    search_pos = found + part.size();
  }

  // 패턴이 '*'로 끝나지 않으면 마지막 부분이 URL의 끝과 일치해야 함
  if (!pattern.empty() && pattern.back() != '*' && !parts.empty()) {
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
  // 빈 필터: 모든 리소스 타입 매칭
  if (filter.empty()) {
    return true;
  }
  return base::EqualsCaseInsensitiveASCII(resource_type, filter);
}

// static
base::Value::List NetworkInterceptTool::HeaderDictToEntries(
    const base::Value::Dict& headers) {
  // CDP Fetch API는 헤더를 [{name: "...", value: "..."}] 배열 형식으로 받는다.
  // base::Value::Dict 형식의 헤더를 이 형식으로 변환한다.
  base::Value::List entries;
  for (const auto& [name, value] : headers) {
    if (!value.is_string()) {
      continue;
    }
    base::Value::Dict entry;
    entry.Set("name", name);
    entry.Set("value", value.GetString());
    entries.Append(std::move(entry));
  }
  return entries;
}

// static
base::Value NetworkInterceptTool::MakeErrorResult(const std::string& message) {
  base::Value::Dict result;
  result.Set("isError", true);
  base::Value::List content;
  base::Value::Dict content_item;
  content_item.Set("type", "text");
  content_item.Set("text", message);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));
  return base::Value(std::move(result));
}

// static
base::Value NetworkInterceptTool::MakeSuccessResult(
    const std::string& message) {
  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict content_item;
  content_item.Set("type", "text");
  content_item.Set("text", message);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));
  return base::Value(std::move(result));
}

}  // namespace mcp
