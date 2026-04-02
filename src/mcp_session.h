// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_MCP_SESSION_H_
#define CHROME_BROWSER_MCP_MCP_SESSION_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"

namespace mcp {

// CDP 명령 완료 시 호출되는 콜백 타입.
// result: CDP 응답의 "result" 필드 (성공 시), error: 오류 메시지 (실패 시)
using CdpResponseCallback =
    base::OnceCallback<void(std::optional<base::DictValue> result,
                            const std::string& error)>;

// 캡처된 네트워크 요청 정보 구조체
struct NetworkRequest {
  NetworkRequest();
  ~NetworkRequest();
  NetworkRequest(NetworkRequest&&);
  NetworkRequest& operator=(NetworkRequest&&);

  std::string request_id;    // CDP Network.requestId
  std::string url;           // 요청 URL
  std::string method;        // HTTP 메서드 (GET, POST 등)
  std::string resource_type; // 리소스 타입 (Document, Script, Image 등)
  int status_code = 0;       // HTTP 응답 코드
  double timestamp = 0.0;    // 요청 시작 시각 (Unix timestamp)
  std::string response_body; // 응답 본문 (includeResponseBody=true 시)
  bool is_static = false;    // 정적 리소스 여부 (js/css/img 등)
};

// McpSession: 하나의 탭(WebContents)에 대한 MCP-CDP 브릿지 세션.
//
// 역할:
//   1. content::DevToolsAgentHostClient 인터페이스 구현
//      - DispatchProtocolMessage(): CDP 이벤트/응답 수신
//      - AgentHostClosed(): CDP 세션 종료 감지
//   2. CDP 명령을 비동기로 전송하고 응답을 콜백으로 반환
//   3. CDP 이벤트(Network.*, Page.* 등)를 수신하여 내부 상태 업데이트
//   4. 네트워크 요청 캡처 및 버퍼링
//   5. CDP 응답 ID 매핑으로 요청/응답 비동기 상관관계 관리
//
// 생명주기:
//   McpServer::AttachToWebContents() → Attach() → 명령 처리 → Detach() → 소멸
//
// 스레드 안전성:
//   BrowserThread::UI에서만 접근해야 함 (sequence_checker_로 검사)
class McpSession : public content::DevToolsAgentHostClient {
 public:
  // send_message_callback: CDP 이벤트 발생 시 McpServer로 알림을 전달하는 콜백.
  // 현재는 사용되지 않지만, 서버-사이드 이벤트 스트리밍 구현 시 활용 예정.
  McpSession(
      scoped_refptr<content::DevToolsAgentHost> agent_host,
      base::RepeatingCallback<void(base::DictValue)> send_message_callback);

  McpSession(const McpSession&) = delete;
  McpSession& operator=(const McpSession&) = delete;
  ~McpSession() override;

  // -----------------------------------------------------------------------
  // 세션 생명주기
  // -----------------------------------------------------------------------

  // DevToolsAgentHost에 CDP 내부 세션 연결.
  // restricted=false: 전체 CDP 도메인 접근 허용 (개발자 도구 수준)
  // 반환값: 연결 성공 시 true
  bool Attach();

  // CDP 세션 해제. 소멸자에서도 자동 호출됨.
  void Detach();

  // 현재 세션이 활성 상태인지 확인
  bool IsAttached() const { return is_attached_; }

  // -----------------------------------------------------------------------
  // CDP 명령 전송
  // -----------------------------------------------------------------------

  // CDP 명령을 비동기로 전송하고 응답을 콜백으로 수신.
  //
  // 내부 동작:
  //   1. CDP 명령 ID 생성 (자동 증가)
  //   2. JSON-RPC 형식으로 직렬화: {"id":N,"method":"...","params":{...}}
  //   3. DevToolsAgentHost::DispatchProtocolMessage() 호출 (내부 IPC)
  //   4. DispatchProtocolMessage() 콜백에서 ID 매핑으로 응답 라우팅
  //
  // method: CDP 도메인 + 명령 (예: "Page.navigate", "Runtime.evaluate")
  // params: CDP 명령 파라미터 (JSON 객체)
  // callback: 응답 수신 시 호출 (BrowserThread::UI에서 실행)
  void SendCdpCommand(const std::string& method,
                      base::DictValue params,
                      CdpResponseCallback callback);

  // 도구 구현용 간편 오버로드.
  // CdpResponseCallback 대신 base::Value 단일 인자 콜백을 받는다.
  // 내부에서 result/error를 base::DictValue로 변환하여 전달:
  //   - 성공 시: {"result": {CDP 응답 데이터}}
  //   - 실패 시: {"error": {"message": "오류 메시지"}}
  void SendCdpCommand(const std::string& method,
                      base::DictValue params,
                      base::OnceCallback<void(base::Value)> callback);

  // -----------------------------------------------------------------------
  // CDP 이벤트 핸들러 등록
  // -----------------------------------------------------------------------

  // CDP 이벤트 핸들러 타입.
  // event_name: CDP 이벤트 이름 (예: "Network.requestWillBeSent")
  // params: 이벤트 파라미터
  using CdpEventHandler =
      base::RepeatingCallback<void(const std::string& event_name,
                                   const base::DictValue& params)>;

  // 특정 CDP 이벤트에 대한 핸들러를 등록한다.
  // 같은 이벤트 이름에 대해 중복 등록하면 기존 핸들러를 덮어쓴다.
  void RegisterCdpEventHandler(const std::string& event_name,
                                CdpEventHandler handler);

  // 등록된 CDP 이벤트 핸들러를 해제한다.
  void UnregisterCdpEventHandler(const std::string& event_name);

  // 약한 참조 포인터를 반환한다.
  base::WeakPtr<McpSession> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  // -----------------------------------------------------------------------
  // 네트워크 캡처
  // -----------------------------------------------------------------------

  // 버퍼링된 네트워크 요청 목록 반환.
  // include_static=false: js/css/image 등 정적 리소스 제외
  base::Value GetCapturedNetworkRequests(bool include_static = false) const;

  // 캡처된 네트워크 요청 버퍼 초기화
  void ClearNetworkRequests();

  // -----------------------------------------------------------------------
  // content::DevToolsAgentHostClient 인터페이스 구현
  // -----------------------------------------------------------------------

  // CDP 메시지 수신 콜백. DevToolsAgentHost가 응답/이벤트 전달 시 호출.
  // message: JSON 직렬화된 CDP 응답 또는 이벤트
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                                base::span<const uint8_t> message) override;

  // CDP 세션 비정상 종료 시 호출 (탭 닫힘, 크래시 등)
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;

 private:
  // -----------------------------------------------------------------------
  // CDP 응답 처리
  // -----------------------------------------------------------------------

  // 수신된 CDP 메시지를 파싱하여 응답 또는 이벤트로 분류
  void HandleCdpMessage(const base::DictValue& message);

  // CDP 명령 응답 처리: id를 통해 대기 중인 콜백 찾아 실행
  void HandleCdpResponse(int id, const base::DictValue& message);

  // CDP 이벤트 처리 (응답이 아닌 서버 푸시 메시지)
  void HandleCdpEvent(const std::string& method,
                       const base::DictValue* params);

  // -----------------------------------------------------------------------
  // CDP 이벤트 핸들러 (네트워크 모니터링)
  // -----------------------------------------------------------------------

  // Network.requestWillBeSent: 새 네트워크 요청 시작 시 호출
  void OnNetworkRequestWillBeSent(const base::DictValue& params);

  // Network.responseReceived: 응답 헤더 수신 시 호출
  void OnNetworkResponseReceived(const base::DictValue& params);

  // Network.loadingFinished: 응답 본문 로드 완료 시 호출
  void OnNetworkLoadingFinished(const base::DictValue& params);

  // Network.loadingFailed: 요청 실패 시 호출
  void OnNetworkLoadingFailed(const base::DictValue& params);

  // -----------------------------------------------------------------------
  // 유틸리티
  // -----------------------------------------------------------------------

  // 리소스 타입이 정적 리소스인지 판별
  // (Image, Stylesheet, Script, Font, Media 등)
  static bool IsStaticResource(const std::string& resource_type);

  // -----------------------------------------------------------------------
  // 상태 변수
  // -----------------------------------------------------------------------

  // 연결된 DevToolsAgentHost (탭/프레임을 나타냄)
  scoped_refptr<content::DevToolsAgentHost> agent_host_;

  // McpServer로 알림을 전달하는 콜백
  base::RepeatingCallback<void(base::DictValue)> send_message_callback_;

  // CDP 세션 연결 상태
  bool is_attached_ = false;

  // CDP 명령 ID 카운터. 각 명령마다 고유한 ID 부여.
  int next_cdp_id_ = 1;

  // 대기 중인 CDP 명령 응답 콜백 맵.
  // 키: CDP 명령 ID, 값: 응답 수신 시 호출할 콜백
  std::map<int, CdpResponseCallback> pending_callbacks_;

  // CDP 명령 타임아웃 (기본 60초). 응답이 없으면 에러로 콜백 호출.
  static constexpr base::TimeDelta kCdpCommandTimeout = base::Seconds(60);
  void OnCdpCommandTimeout(int cmd_id, const std::string& method);

  // 캡처된 네트워크 요청 버퍼.
  // requestId를 키로 사용하여 요청-응답 쌍을 매핑.
  std::map<std::string, NetworkRequest> captured_requests_;

  // CDP 이벤트 핸들러 맵.
  // 이벤트 이름 → 핸들러 콜백.
  std::map<std::string, CdpEventHandler> event_handlers_;

  // UI 스레드 시퀀스 검사
  SEQUENCE_CHECKER(sequence_checker_);

  // 약한 참조 팩토리 (CDP 비동기 응답에서 this 댕글링 방지)
  base::WeakPtrFactory<McpSession> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_MCP_SESSION_H_
