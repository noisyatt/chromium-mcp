// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/mcp_session.h"

#include <utility>

#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"

namespace mcp {

// NetworkRequest out-of-line 구현 (chromium-style 요구사항)
NetworkRequest::NetworkRequest() = default;
NetworkRequest::~NetworkRequest() = default;
NetworkRequest::NetworkRequest(NetworkRequest&&) = default;
NetworkRequest& NetworkRequest::operator=(NetworkRequest&&) = default;

// 정적 리소스로 분류되는 CDP 리소스 타입 목록
constexpr const char* kStaticResourceTypes[] = {
    "Image", "Stylesheet", "Script", "Font", "Media",
    "Manifest", "SignedExchange", "Ping", "Preflight",
};

McpSession::McpSession(
    scoped_refptr<content::DevToolsAgentHost> agent_host,
    base::RepeatingCallback<void(base::DictValue)> send_message_callback)
    : agent_host_(std::move(agent_host)),
      send_message_callback_(std::move(send_message_callback)) {
  DCHECK(agent_host_);
}

McpSession::~McpSession() {
  // 소멸 시 CDP 세션 자동 해제
  Detach();
}

// -----------------------------------------------------------------------
// 세션 생명주기
// -----------------------------------------------------------------------

bool McpSession::Attach() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (is_attached_) {
    LOG(WARNING) << "[McpSession] 이미 연결된 상태. Attach() 중복 호출 무시.";
    return true;
  }

  if (!agent_host_) {
    LOG(ERROR) << "[McpSession] DevToolsAgentHost가 null. 연결 불가.";
    return false;
  }

  // DevToolsAgentHost에 이 세션(client)을 연결.
  //
  // 핵심: AttachClient()는 내부 IPC만 사용하므로:
  //   - 외부 네트워크 포트를 열지 않음
  //   - chrome.debugger API의 노란 배너가 표시되지 않음
  //   - navigator.webdriver 플래그가 변경되지 않음
  //
  // restricted=false: 전체 CDP 도메인 접근 허용
  //   (true이면 일부 민감한 도메인 제한)
  agent_host_->AttachClient(this);
  is_attached_ = true;

  LOG(INFO) << "[McpSession] CDP 세션 연결 완료. URL: "
            << agent_host_->GetURL().spec();

  // Accessibility 도메인 활성화 (ElementLocator의 AX Tree 조회에 필요)
  SendCdpCommand("Accessibility.enable", base::DictValue(),
                 base::BindOnce([](base::Value) {}));

  // Page 도메인 활성화 (Page.javascriptDialogOpening 이벤트 수신에 필요)
  SendCdpCommand("Page.enable", base::DictValue(),
                 base::BindOnce([](base::Value) {}));

  return true;
}

void McpSession::Detach() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!is_attached_) {
    return;
  }

  // 대기 중인 모든 CDP 콜백을 오류로 종료
  for (auto& [id, callback] : pending_callbacks_) {
    std::move(callback).Run(std::nullopt, "Session detached");
  }
  pending_callbacks_.clear();

  // DevToolsAgentHost 연결 해제
  if (agent_host_) {
    agent_host_->DetachClient(this);
  }

  is_attached_ = false;
  LOG(INFO) << "[McpSession] CDP 세션 분리 완료";
}

// -----------------------------------------------------------------------
// CDP 명령 전송
// -----------------------------------------------------------------------

void McpSession::SendCdpCommand(const std::string& method,
                                 base::DictValue params,
                                 CdpResponseCallback callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!is_attached_) {
    LOG(ERROR) << "[McpSession] 세션이 연결되지 않음. 명령 전송 실패: " << method;
    std::move(callback).Run(std::nullopt, "Session not attached");
    return;
  }

  // CDP 명령에 고유 ID 부여 (요청-응답 매핑에 사용)
  int cmd_id = next_cdp_id_++;

  // CDP JSON-RPC 요청 구성:
  // {"id": N, "method": "Domain.command", "params": {...}}
  base::DictValue request;
  request.Set("id", cmd_id);
  request.Set("method", method);
  request.Set("params", std::move(params));

  // JSON 직렬화
  std::string json_message;
  if (!base::JSONWriter::Write(base::Value(std::move(request)), &json_message)) {
    LOG(ERROR) << "[McpSession] CDP 명령 직렬화 실패: " << method;
    std::move(callback).Run(std::nullopt, "JSON serialization failed");
    return;
  }

  // 콜백을 대기 맵에 등록 (응답 수신 시 cmd_id로 찾아 실행)
  pending_callbacks_[cmd_id] = std::move(callback);

  LOG(INFO) << "[McpSession] CDP 명령 전송 (id=" << cmd_id << "): " << method;

  // DevToolsAgentHost를 통해 CDP 명령을 브라우저 내부 IPC로 전달.
  // 이 호출은 네트워크 없이 직접 렌더러 프로세스와 통신.
  agent_host_->DispatchProtocolMessage(
      this, base::as_byte_span(json_message));
}

void McpSession::SendCdpCommand(
    const std::string& method,
    base::DictValue params,
    base::OnceCallback<void(base::Value)> callback) {
  // 도구 구현용 간편 오버로드.
  // CdpResponseCallback을 래핑하여 base::Value 단일 콜백으로 변환한다.
  SendCdpCommand(
      method, std::move(params),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb,
             std::optional<base::DictValue> result,
             const std::string& error) {
            if (!error.empty()) {
              // 오류 응답: {"error": {"message": "..."}}
              base::DictValue error_dict;
              base::DictValue error_detail;
              error_detail.Set("message", error);
              error_dict.Set("error", std::move(error_detail));
              std::move(cb).Run(base::Value(std::move(error_dict)));
            } else if (result.has_value()) {
              // 성공 응답: CDP result Dict 그대로 전달
              std::move(cb).Run(base::Value(std::move(*result)));
            } else {
              // 빈 성공 응답
              std::move(cb).Run(base::Value(base::DictValue()));
            }
          },
          std::move(callback)));
}

// -----------------------------------------------------------------------
// CDP 이벤트 핸들러 등록/해제
// -----------------------------------------------------------------------

void McpSession::RegisterCdpEventHandler(const std::string& event_name,
                                          CdpEventHandler handler) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  event_handlers_[event_name] = std::move(handler);
  LOG(INFO) << "[McpSession] CDP 이벤트 핸들러 등록: " << event_name;
}

void McpSession::UnregisterCdpEventHandler(const std::string& event_name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  event_handlers_.erase(event_name);
  LOG(INFO) << "[McpSession] CDP 이벤트 핸들러 해제: " << event_name;
}

// -----------------------------------------------------------------------
// content::DevToolsAgentHostClient 인터페이스 구현
// -----------------------------------------------------------------------

void McpSession::DispatchProtocolMessage(
    content::DevToolsAgentHost* agent_host,
    base::span<const uint8_t> message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // base::span<const uint8_t> → std::string_view로 변환
  std::string_view message_str(reinterpret_cast<const char*>(message.data()),
                                message.size());

  // CDP 응답 또는 이벤트를 JSON 파싱
  auto parsed = base::JSONReader::ReadAndReturnValueWithError(
      message_str, base::JSON_PARSE_RFC);
  if (!parsed.has_value() || !parsed->is_dict()) {
    LOG(WARNING) << "[McpSession] CDP 메시지 파싱 실패";
    return;
  }

  HandleCdpMessage(parsed->GetDict());
}

void McpSession::AgentHostClosed(content::DevToolsAgentHost* agent_host) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  LOG(INFO) << "[McpSession] DevToolsAgentHost 종료 감지 (탭 닫힘 또는 크래시)";

  is_attached_ = false;

  // 대기 중인 모든 콜백을 오류로 종료
  for (auto& [id, callback] : pending_callbacks_) {
    std::move(callback).Run(std::nullopt, "Agent host closed");
  }
  pending_callbacks_.clear();
}

// -----------------------------------------------------------------------
// CDP 메시지 처리
// -----------------------------------------------------------------------

void McpSession::HandleCdpMessage(const base::DictValue& message) {
  // CDP 메시지 분류:
  //   - 응답: "id" 필드 존재 (이전에 보낸 명령에 대한 응답)
  //   - 이벤트: "method" 필드만 존재 (서버가 비동기로 푸시)

  const base::Value* id_value = message.Find("id");

  if (id_value && id_value->is_int()) {
    // CDP 명령 응답 처리
    HandleCdpResponse(id_value->GetInt(), message);
  } else {
    // CDP 이벤트 처리 (Network.*, Page.*, Runtime.* 등)
    const std::string* method = message.FindString("method");
    if (method) {
      const base::DictValue* params = message.FindDict("params");
      HandleCdpEvent(*method, params);
    }
  }
}

void McpSession::HandleCdpResponse(int id, const base::DictValue& message) {
  // 대기 중인 콜백 찾기
  auto it = pending_callbacks_.find(id);
  if (it == pending_callbacks_.end()) {
    // 알 수 없는 응답 ID (이미 처리되었거나 취소된 명령)
    LOG(WARNING) << "[McpSession] 알 수 없는 CDP 응답 ID: " << id;
    return;
  }

  CdpResponseCallback callback = std::move(it->second);
  pending_callbacks_.erase(it);

  // CDP 오류 응답 확인: {"id":N,"error":{"code":-32601,"message":"..."}}
  if (const base::DictValue* error = message.FindDict("error")) {
    std::string error_message = "CDP error";
    if (const std::string* msg = error->FindString("message")) {
      error_message = *msg;
    }
    LOG(WARNING) << "[McpSession] CDP 명령 오류 (id=" << id
                 << "): " << error_message;
    std::move(callback).Run(std::nullopt, error_message);
    return;
  }

  // CDP 성공 응답: {"id":N,"result":{...}}
  // result 필드를 추출하여 콜백에 전달
  const base::DictValue* result = message.FindDict("result");
  if (result) {
    LOG(INFO) << "[McpSession] CDP 응답 수신 (id=" << id << ")";
    std::move(callback).Run(result->Clone(), "");
  } else {
    // result가 없는 응답 (예: 파라미터 없는 명령의 빈 응답)
    std::move(callback).Run(base::DictValue(), "");
  }
}

void McpSession::HandleCdpEvent(const std::string& method,
                                  const base::DictValue* params) {
  // CDP 이벤트 라우팅.
  // Network.* 이벤트는 내부 네트워크 캡처 버퍼에 저장.
  // 필요 시 Page.*, Runtime.* 이벤트도 여기서 처리 가능.

  if (method == "Network.requestWillBeSent") {
    if (params) {
      OnNetworkRequestWillBeSent(*params);
    }
  } else if (method == "Network.responseReceived") {
    if (params) {
      OnNetworkResponseReceived(*params);
    }
  } else if (method == "Network.loadingFinished") {
    if (params) {
      OnNetworkLoadingFinished(*params);
    }
  } else if (method == "Network.loadingFailed") {
    if (params) {
      OnNetworkLoadingFailed(*params);
    }
  } else {
    // 처리하지 않는 내장 이벤트는 무시 (로그도 생략하여 노이즈 최소화)
  }

  // 등록된 외부 이벤트 핸들러에도 이벤트를 전달한다.
  auto handler_it = event_handlers_.find(method);
  if (handler_it != event_handlers_.end() && params) {
    handler_it->second.Run(method, *params);
  }
}

// -----------------------------------------------------------------------
// 네트워크 이벤트 핸들러
// -----------------------------------------------------------------------

void McpSession::OnNetworkRequestWillBeSent(const base::DictValue& params) {
  // Network.requestWillBeSent 이벤트 처리.
  // 새로운 네트워크 요청이 시작될 때 호출됨.

  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  NetworkRequest req;
  req.request_id = *request_id;

  // 요청 URL 추출
  if (const base::DictValue* request = params.FindDict("request")) {
    if (const std::string* url = request->FindString("url")) {
      req.url = *url;
    }
    if (const std::string* method = request->FindString("method")) {
      req.method = *method;
    }
  }

  // 리소스 타입 추출 (Document, XHR, Fetch, Image 등)
  if (const std::string* type = params.FindString("type")) {
    req.resource_type = *type;
    req.is_static = IsStaticResource(*type);
  }

  // 요청 시각 기록
  std::optional<double> timestamp = params.FindDouble("timestamp");
  if (timestamp) {
    req.timestamp = *timestamp;
  }

  // 요청 정보를 버퍼에 저장
  captured_requests_[req.request_id] = std::move(req);
}

void McpSession::OnNetworkResponseReceived(const base::DictValue& params) {
  // Network.responseReceived 이벤트 처리.
  // 응답 헤더가 수신되었을 때 호출됨.

  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  auto it = captured_requests_.find(*request_id);
  if (it == captured_requests_.end()) {
    // requestWillBeSent 없이 responseReceived가 도착하는 경우 (예: 캐시 응답)
    return;
  }

  // HTTP 상태 코드 추출
  if (const base::DictValue* response = params.FindDict("response")) {
    std::optional<int> status = response->FindInt("status");
    if (status) {
      it->second.status_code = *status;
    }
  }
}

void McpSession::OnNetworkLoadingFinished(const base::DictValue& params) {
  // Network.loadingFinished 이벤트 처리.
  // 응답 본문 로드가 완료되었을 때 호출됨.
  // 이 시점에서 Network.getResponseBody를 호출하면 응답 본문을 가져올 수 있음.

  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  // 현재는 로드 완료만 기록. 응답 본문은 필요 시 GetResponseBody로 요청.
  // (자동으로 모든 응답 본문을 가져오면 메모리 사용량이 과다해질 수 있음)
  auto it = captured_requests_.find(*request_id);
  if (it == captured_requests_.end()) {
    return;
  }

  LOG(INFO) << "[McpSession] 네트워크 요청 완료: " << it->second.url;
}

void McpSession::OnNetworkLoadingFailed(const base::DictValue& params) {
  // Network.loadingFailed 이벤트 처리. 요청 실패 시 호출됨.

  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  auto it = captured_requests_.find(*request_id);
  if (it == captured_requests_.end()) {
    return;
  }

  // 실패한 요청의 상태 코드를 0으로 표시 (또는 특수 값 사용)
  it->second.status_code = 0;

  if (const std::string* error = params.FindString("errorText")) {
    LOG(INFO) << "[McpSession] 네트워크 요청 실패: "
              << it->second.url << " - " << *error;
  }
}

// -----------------------------------------------------------------------
// 네트워크 요청 조회
// -----------------------------------------------------------------------

base::Value McpSession::GetCapturedNetworkRequests(
    bool include_static) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::ListValue requests_list;

  for (const auto& [request_id, req] : captured_requests_) {
    // 정적 리소스 필터링
    if (!include_static && req.is_static) {
      continue;
    }

    base::DictValue req_dict;
    req_dict.Set("requestId", req.request_id);
    req_dict.Set("url", req.url);
    req_dict.Set("method", req.method);
    req_dict.Set("resourceType", req.resource_type);
    req_dict.Set("statusCode", req.status_code);
    req_dict.Set("timestamp", req.timestamp);

    if (!req.response_body.empty()) {
      req_dict.Set("responseBody", req.response_body);
    }

    requests_list.Append(std::move(req_dict));
  }

  return base::Value(std::move(requests_list));
}

void McpSession::ClearNetworkRequests() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  captured_requests_.clear();
  LOG(INFO) << "[McpSession] 네트워크 요청 버퍼 초기화";
}

// -----------------------------------------------------------------------
// 유틸리티
// -----------------------------------------------------------------------

// static
bool McpSession::IsStaticResource(const std::string& resource_type) {
  for (const char* static_type : kStaticResourceTypes) {
    if (resource_type == static_type) {
      return true;
    }
  }
  return false;
}

}  // namespace mcp
