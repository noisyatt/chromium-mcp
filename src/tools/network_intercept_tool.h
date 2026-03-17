// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_NETWORK_INTERCEPT_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_NETWORK_INTERCEPT_TOOL_H_

#include <map>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// ============================================================================
// InterceptRule: 자동 처리 규칙 하나를 나타내는 데이터 구조체.
//
// addRule action으로 등록되며, Fetch.requestPaused 이벤트 수신 시
// URL 패턴과 리소스 타입이 매칭되는 요청에 자동으로 동작을 적용한다.
// ============================================================================
struct InterceptRule {
  // 규칙 고유 ID (removeRule 시 사용)
  std::string rule_id;

  // 매칭 조건 ---------------------------------------------------------------

  // 글로브 패턴 (빈 문자열이면 모든 URL 매칭)
  // 예: "*.js", "https://api.example.com/*"
  std::string url_pattern;

  // 리소스 타입 필터 (빈 문자열이면 모든 타입 매칭)
  // 예: "Document", "Script", "Image", "XHR", "Fetch"
  std::string resource_type;

  // 인터셉트 단계: "Request" 또는 "Response"
  std::string request_stage;  // 기본값: "Request"

  // 자동 적용할 동작 --------------------------------------------------------

  // 동작 종류: "continue" | "fulfill" | "fail" | "passthrough"
  // passthrough: 규칙 매칭 시에도 그냥 통과 (로깅 목적)
  std::string action;  // 기본값: "passthrough"

  // continue 동작 시 수정할 필드 (빈 문자열이면 원본 유지)
  std::string override_url;
  std::string override_method;
  base::Value::Dict override_headers;  // 빈 dict이면 헤더 수정 없음
  std::string override_post_data;

  // fulfill 동작 시 모킹 응답
  int response_code = 200;
  base::Value::Dict response_headers;
  std::string response_body;  // base64 또는 UTF-8 텍스트

  // fail 동작 시 에러 사유
  // 가능한 값: "Failed", "Aborted", "TimedOut", "AccessDenied",
  //           "ConnectionClosed", "ConnectionReset", "ConnectionRefused",
  //           "ConnectionAborted", "ConnectionFailed", "NameNotResolved",
  //           "InternetDisconnected", "AddressUnreachable", "BlockedByClient",
  //           "BlockedByResponse"
  std::string error_reason;  // 기본값: "BlockedByClient"

  // 규칙 활성화 여부
  bool enabled = true;
};

// ============================================================================
// NetworkInterceptTool: 네트워크 요청을 가로채고 수정하거나 차단하는 도구.
//
// Fetch 도메인의 CDP 명령을 사용하여 브라우저 레벨에서 요청을 인터셉트한다.
//
// [주요 기능]
//   1. 인터셉트 활성화/비활성화 (enable/disable)
//   2. 자동 처리 규칙 등록 및 삭제 (addRule/removeRule)
//   3. 일시 정지된 요청 수동 처리:
//      - continueRequest: 수정된 요청으로 계속 진행
//      - fulfillRequest:  커스텀 응답으로 모킹 (API Mock 서버)
//      - failRequest:     요청 차단 또는 실패 처리
//
// [자동 규칙 엔진]
//   Fetch.requestPaused 이벤트 수신 시 등록된 rules_ 맵을 순회하여
//   URL 패턴·리소스 타입이 매칭되는 첫 번째 규칙을 적용한다.
//   매칭 규칙이 없으면 Fetch.continueRequest로 요청을 그대로 통과시킨다.
//
// [활용 예시]
//   - 광고/트래커 차단: "*.doubleclick.net/*" → failRequest(BlockedByClient)
//   - API Mock:       "/api/users" → fulfillRequest(200, JSON 응답)
//   - 헤더 주입:       "/api/*" → continueRequest(Authorization 헤더 추가)
//   - 응답 수정:       requestStage=Response 에서 본문 교체
//
// [주의사항]
//   - Fetch.enable은 Network.enable과 독립적이므로 함께 사용 가능하다.
//   - 인터셉트된 모든 요청은 반드시 continue/fulfill/fail 중 하나로
//     응답해야 한다. 그렇지 않으면 해당 요청은 영원히 대기 상태가 된다.
//   - 내부 CDP 세션을 사용하므로 노란 배너가 표시되지 않는다.
// ============================================================================
class NetworkInterceptTool : public McpTool {
 public:
  NetworkInterceptTool();
  ~NetworkInterceptTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // =========================================================================
  // action 핸들러
  // =========================================================================

  // action=enable: Fetch.enable 호출하여 인터셉트 시작.
  // |patterns|: Fetch.enable에 전달할 패턴 목록 (없으면 전체 인터셉트)
  void HandleEnable(const base::Value::List* patterns,
                    McpSession* session,
                    base::OnceCallback<void(base::Value)> callback);

  // action=disable: Fetch.disable 호출하여 인터셉트 중지.
  // 등록된 규칙은 유지되므로 재활성화 시 그대로 사용 가능하다.
  void HandleDisable(McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // action=addRule: 자동 처리 규칙을 rules_ 맵에 등록.
  // ruleId가 없으면 자동 생성한다.
  void HandleAddRule(const base::Value::Dict& arguments,
                     base::OnceCallback<void(base::Value)> callback);

  // action=removeRule: ruleId에 해당하는 규칙을 rules_ 맵에서 제거.
  void HandleRemoveRule(const std::string& rule_id,
                        base::OnceCallback<void(base::Value)> callback);

  // action=continueRequest: 일시 정지된 요청을 수정하여 계속 진행.
  // Fetch.continueRequest CDP 명령 호출.
  void HandleContinueRequest(const base::Value::Dict& arguments,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback);

  // action=fulfillRequest: 일시 정지된 요청에 커스텀 응답 반환.
  // Fetch.fulfillRequest CDP 명령 호출.
  void HandleFulfillRequest(const base::Value::Dict& arguments,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback);

  // action=failRequest: 일시 정지된 요청을 에러로 종료.
  // Fetch.failRequest CDP 명령 호출.
  void HandleFailRequest(const base::Value::Dict& arguments,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  // =========================================================================
  // CDP 응답 핸들러
  // =========================================================================

  // Fetch.enable CDP 응답 처리.
  // 성공하면 is_intercepting_=true로 설정하고 이벤트 핸들러를 등록한다.
  void OnFetchEnabled(McpSession* session,
                      base::OnceCallback<void(base::Value)> callback,
                      base::Value response);

  // Fetch.disable CDP 응답 처리.
  // is_intercepting_=false로 설정하고 이벤트 핸들러를 해제한다.
  void OnFetchDisabled(McpSession* session,
                       base::OnceCallback<void(base::Value)> callback,
                       base::Value response);

  // Fetch.continueRequest / Fetch.fulfillRequest / Fetch.failRequest
  // 단순 성공/실패 응답 공통 처리기.
  // |success_message|: 성공 시 반환할 텍스트
  void OnSimpleResponse(const std::string& success_message,
                        base::OnceCallback<void(base::Value)> callback,
                        base::Value response);

  // =========================================================================
  // Fetch.requestPaused 이벤트 처리
  // =========================================================================

  // Fetch.requestPaused 이벤트 수신 시 호출되는 핸들러.
  // 등록된 규칙 매칭 → 자동 처리 / 매칭 없으면 continueRequest 통과.
  void OnRequestPaused(const std::string& event_name,
                       const base::Value::Dict& params,
                       McpSession* session);

  // 인터셉트된 요청에 규칙의 continue 동작 적용.
  // override 필드가 있으면 수정하여, 없으면 원본 그대로 통과.
  void ApplyRuleContinue(const std::string& request_id,
                         const InterceptRule& rule,
                         const base::Value::Dict& paused_params,
                         McpSession* session);

  // 인터셉트된 요청에 규칙의 fulfill 동작 적용.
  void ApplyRuleFulfill(const std::string& request_id,
                        const InterceptRule& rule,
                        McpSession* session);

  // 인터셉트된 요청에 규칙의 fail 동작 적용.
  void ApplyRuleFail(const std::string& request_id,
                     const InterceptRule& rule,
                     McpSession* session);

  // 자동 처리 결과의 CDP 응답을 조용히 처리 (로그만 기록).
  void OnAutoHandleResponse(const std::string& request_id,
                            const std::string& action,
                            base::Value response);

  // =========================================================================
  // 유틸리티
  // =========================================================================

  // URL이 규칙의 패턴과 매칭되는지 확인.
  // 글로브 패턴(*) 지원. 빈 패턴이면 항상 true 반환.
  static bool MatchesPattern(const std::string& url,
                             const std::string& pattern);

  // 리소스 타입이 규칙의 타입 필터와 매칭되는지 확인.
  // 빈 필터이면 항상 true 반환.
  static bool MatchesResourceType(const std::string& resource_type,
                                  const std::string& filter);

  // base::Value::Dict 헤더를 Fetch.continueRequest/fulfillRequest 형식의
  // headerEntries 배열로 변환.
  // Fetch API는 헤더를 [{name:"...", value:"..."}] 배열 형식으로 받는다.
  static base::Value::List HeaderDictToEntries(
      const base::Value::Dict& headers);

  // 오류 응답 구조체 생성 헬퍼.
  static base::Value MakeErrorResult(const std::string& message);

  // 성공 응답 구조체 생성 헬퍼.
  static base::Value MakeSuccessResult(const std::string& message);

  // =========================================================================
  // 상태 변수
  // =========================================================================

  // 현재 Fetch.enable 상태인지 여부
  bool is_intercepting_ = false;

  // 자동 처리 규칙 맵: ruleId → InterceptRule
  // addRule/removeRule로 관리된다.
  std::map<std::string, InterceptRule> rules_;

  // 규칙 ID 자동 생성용 카운터
  int next_rule_id_ = 1;

  // 인터셉트된 요청 중 아직 처리되지 않은 requestId 목록.
  // continueRequest/fulfillRequest/failRequest로 처리 완료 시 제거.
  std::vector<std::string> pending_request_ids_;

  // 약한 참조 팩토리 (비동기 CDP 콜백에서 this 댕글링 방지)
  base::WeakPtrFactory<NetworkInterceptTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_NETWORK_INTERCEPT_TOOL_H_
