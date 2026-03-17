// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/network_tool.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// CapturedRequest out-of-line 구현 (chromium-style 요구사항)
CapturedRequest::CapturedRequest() = default;
CapturedRequest::~CapturedRequest() = default;
CapturedRequest::CapturedRequest(CapturedRequest&&) = default;
CapturedRequest& CapturedRequest::operator=(CapturedRequest&&) = default;

// ============================================================================
// NetworkCaptureTool 구현
// ============================================================================

NetworkCaptureTool::NetworkCaptureTool() = default;
NetworkCaptureTool::~NetworkCaptureTool() = default;

std::string NetworkCaptureTool::name() const {
  return "network_capture";
}

std::string NetworkCaptureTool::description() const {
  return "네트워크 요청 캡처를 시작하거나 중지합니다. "
         "action=start 시 Network.enable을 통해 CDP 이벤트를 수신하여 "
         "내부 버퍼에 요청을 누적합니다. "
         "action=stop 시 Network.disable 후 누적된 결과를 반환합니다. "
         "내부 CDP 세션을 사용하므로 노란 배너가 표시되지 않습니다.";
}

base::DictValue NetworkCaptureTool::input_schema() const {
  // JSON Schema: action, includeResponseBody, filter 파라미터 정의
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: 필수 파라미터 (start 또는 stop)
  {
    base::DictValue action_prop;
    action_prop.Set("type", "string");
    base::ListValue action_enum;
    action_enum.Append("start");
    action_enum.Append("stop");
    action_prop.Set("enum", std::move(action_enum));
    action_prop.Set("description", "캡처 시작(start) 또는 중지(stop)");
    properties.Set("action", std::move(action_prop));
  }

  // includeResponseBody: 응답 바디 자동 수집 여부 (기본값 false)
  {
    base::DictValue body_prop;
    body_prop.Set("type", "boolean");
    body_prop.Set("description",
                  "true이면 각 요청 완료 후 Network.getResponseBody를 자동 "
                  "호출하여 응답 바디를 수집합니다.");
    properties.Set("includeResponseBody", std::move(body_prop));
  }

  // filter: URL 패턴 및 리소스 타입 필터
  {
    base::DictValue filter_prop;
    filter_prop.Set("type", "object");

    base::DictValue filter_props;

    base::DictValue url_pattern_prop;
    url_pattern_prop.Set("type", "string");
    url_pattern_prop.Set("description",
                         "캡처할 URL 패턴. * 와일드카드 지원. "
                         "비어있으면 전체 URL 캡처.");
    filter_props.Set("urlPattern", std::move(url_pattern_prop));

    base::DictValue resource_types_prop;
    resource_types_prop.Set("type", "array");
    base::DictValue rt_items;
    rt_items.Set("type", "string");
    resource_types_prop.Set("items", std::move(rt_items));
    resource_types_prop.Set("description",
                            "캡처할 리소스 타입 목록. "
                            "예: [\"XHR\", \"Fetch\", \"Document\"]. "
                            "비어있으면 전체 타입 캡처.");
    filter_props.Set("resourceTypes", std::move(resource_types_prop));

    filter_prop.Set("properties", std::move(filter_props));
    properties.Set("filter", std::move(filter_prop));
  }

  schema.Set("properties", std::move(properties));

  // action만 필수
  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void NetworkCaptureTool::Execute(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // action 파라미터 추출
  const std::string* action = arguments.FindString("action");
  if (!action || action->empty()) {
    LOG(WARNING) << "[MCP][NetworkCapture] action 파라미터 누락";
    base::DictValue error_result;
    error_result.Set("isError", true);
    base::ListValue content;
    base::DictValue content_item;
    content_item.Set("type", "text");
    content_item.Set("text", "오류: action 파라미터가 필요합니다 (start 또는 stop)");
    content.Append(std::move(content_item));
    error_result.Set("content", std::move(content));
    std::move(callback).Run(base::Value(std::move(error_result)));
    return;
  }

  if (*action == "start") {
    // includeResponseBody 파라미터 (기본값 false)
    bool include_body = false;
    const base::Value* include_body_val =
        arguments.Find("includeResponseBody");
    if (include_body_val && include_body_val->is_bool()) {
      include_body = include_body_val->GetBool();
    }

    // filter 파라미터 (선택적)
    const base::DictValue* filter = arguments.FindDict("filter");

    HandleStart(include_body, filter, session, std::move(callback));
  } else if (*action == "stop") {
    HandleStop(session, std::move(callback));
  } else {
    LOG(WARNING) << "[MCP][NetworkCapture] 알 수 없는 action: " << *action;
    base::DictValue error_result;
    error_result.Set("isError", true);
    base::ListValue content;
    base::DictValue content_item;
    content_item.Set("type", "text");
    content_item.Set("text", "오류: action은 'start' 또는 'stop'이어야 합니다");
    content.Append(std::move(content_item));
    error_result.Set("content", std::move(content));
    std::move(callback).Run(base::Value(std::move(error_result)));
  }
}

void NetworkCaptureTool::HandleStart(
    bool include_response_body,
    const base::DictValue* filter,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (is_capturing_) {
    // 이미 캡처 중이면 기존 버퍼를 초기화하고 재시작
    LOG(INFO) << "[MCP][NetworkCapture] 기존 캡처 세션을 초기화하고 재시작";
    captured_requests_.clear();
  }

  // 캡처 설정 저장
  include_response_body_ = include_response_body;

  // 필터 설정 적용
  url_filter_pattern_.clear();
  resource_type_filter_.clear();
  if (filter) {
    const std::string* url_pattern = filter->FindString("urlPattern");
    if (url_pattern) {
      url_filter_pattern_ = *url_pattern;
      LOG(INFO) << "[MCP][NetworkCapture] URL 필터 패턴: "
                << url_filter_pattern_;
    }
    const base::ListValue* resource_types =
        filter->FindList("resourceTypes");
    if (resource_types) {
      for (const auto& rt_val : *resource_types) {
        if (rt_val.is_string()) {
          resource_type_filter_.push_back(rt_val.GetString());
        }
      }
      LOG(INFO) << "[MCP][NetworkCapture] 리소스 타입 필터: "
                << resource_type_filter_.size() << "개";
    }
  }

  LOG(INFO) << "[MCP][NetworkCapture] Network.enable 전송 시작 "
            << "(responseBody=" << include_response_body_ << ")";

  // Network.enable CDP 명령으로 네트워크 이벤트 스트림 활성화
  // maxTotalBufferSize, maxResourceBufferSize는 0으로 설정해 제한 없음
  base::DictValue params;
  params.Set("maxTotalBufferSize", 0);
  params.Set("maxResourceBufferSize", 0);

  session->SendCdpCommand(
      "Network.enable", std::move(params),
      base::BindOnce(&NetworkCaptureTool::OnNetworkEnabled,
                     weak_factory_.GetWeakPtr(), session,
                     std::move(callback)));
}

void NetworkCaptureTool::OnNetworkEnabled(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // CDP 응답에 error 키가 있으면 실패
  if (response.is_dict()) {
    const base::DictValue* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      std::string err_text =
          msg ? *msg : "Network.enable 실패 (알 수 없는 오류)";
      LOG(ERROR) << "[MCP][NetworkCapture] Network.enable 실패: " << err_text;

      base::DictValue error_result;
      error_result.Set("isError", true);
      base::ListValue content;
      base::DictValue content_item;
      content_item.Set("type", "text");
      content_item.Set("text", "네트워크 캡처 시작 실패: " + err_text);
      content.Append(std::move(content_item));
      error_result.Set("content", std::move(content));
      std::move(callback).Run(base::Value(std::move(error_result)));
      return;
    }
  }

  is_capturing_ = true;
  captured_requests_.clear();

  // McpSession에 CDP 이벤트 수신 핸들러 등록.
  // Network.* 이벤트를 OnCdpEvent 로 라우팅한다.
  // RegisterCdpEventHandler 의 시그니처:
  //   void(const std::string& event_name, const base::DictValue& params)
  session->RegisterCdpEventHandler(
      "Network.requestWillBeSent",
      base::BindRepeating(&NetworkCaptureTool::OnCdpEvent,
                          weak_factory_.GetWeakPtr()));
  session->RegisterCdpEventHandler(
      "Network.responseReceived",
      base::BindRepeating(&NetworkCaptureTool::OnCdpEvent,
                          weak_factory_.GetWeakPtr()));

  // loadingFinished 핸들러: session WeakPtr을 클로저로 캡처하여
  // OnLoadingFinished에 session을 전달한다.
  // McpSession은 UI 스레드 전용이므로 raw_ptr 캡처는 안전하다.
  // (McpSession 소멸 시 이벤트 핸들러가 먼저 해제되기 때문)
  base::WeakPtr<McpSession> session_weak = session->GetWeakPtr();
  session->RegisterCdpEventHandler(
      "Network.loadingFinished",
      base::BindRepeating(
          [](base::WeakPtr<NetworkCaptureTool> self,
             base::WeakPtr<McpSession> weak_session,
             const std::string& /*event_name*/,
             const base::DictValue& params) {
            if (!self) {
              return;
            }
            McpSession* s = weak_session.get();
            self->OnLoadingFinished(params, s);
          },
          weak_factory_.GetWeakPtr(), session_weak));

  LOG(INFO) << "[MCP][NetworkCapture] 캡처 시작 완료. 이벤트 대기 중.";

  // 성공 응답 반환
  base::DictValue result;
  base::ListValue content;
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text",
                   "네트워크 캡처가 시작되었습니다. "
                   "network_requests 도구로 중간 결과를 조회하거나, "
                   "action=stop으로 캡처를 중지하고 전체 결과를 받을 수 있습니다.");
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));
  std::move(callback).Run(base::Value(std::move(result)));
}

void NetworkCaptureTool::HandleStop(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (!is_capturing_) {
    LOG(WARNING) << "[MCP][NetworkCapture] 캡처가 시작되지 않았는데 stop 요청";
    base::DictValue error_result;
    error_result.Set("isError", true);
    base::ListValue content;
    base::DictValue content_item;
    content_item.Set("type", "text");
    content_item.Set("text",
                     "오류: 캡처가 진행 중이지 않습니다. "
                     "먼저 action=start로 캡처를 시작하세요.");
    content.Append(std::move(content_item));
    error_result.Set("content", std::move(content));
    std::move(callback).Run(base::Value(std::move(error_result)));
    return;
  }

  LOG(INFO) << "[MCP][NetworkCapture] Network.disable 전송. "
            << "캡처된 요청 수: " << captured_requests_.size();

  // 이벤트 핸들러 해제
  session->UnregisterCdpEventHandler("Network.requestWillBeSent");
  session->UnregisterCdpEventHandler("Network.responseReceived");
  session->UnregisterCdpEventHandler("Network.loadingFinished");

  // Network.disable CDP 명령으로 이벤트 스트림 비활성화
  session->SendCdpCommand(
      "Network.disable", base::DictValue(),
      base::BindOnce(&NetworkCaptureTool::OnNetworkDisabled,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void NetworkCaptureTool::OnNetworkDisabled(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  is_capturing_ = false;

  // CDP 응답에 error 키가 있어도 버퍼는 반환한다 (이미 수집된 데이터 손실 방지)
  if (response.is_dict()) {
    const base::DictValue* error = response.GetDict().FindDict("error");
    if (error) {
      const std::string* msg = error->FindString("message");
      LOG(WARNING) << "[MCP][NetworkCapture] Network.disable 경고: "
                   << (msg ? *msg : "알 수 없음");
    }
  }

  LOG(INFO) << "[MCP][NetworkCapture] 캡처 중지 완료. "
            << "총 " << captured_requests_.size() << "개 요청 반환.";

  // 버퍼를 직렬화하여 결과 반환
  base::Value result = SerializeRequests();

  // 버퍼 초기화
  captured_requests_.clear();
  include_response_body_ = false;
  url_filter_pattern_.clear();
  resource_type_filter_.clear();

  std::move(callback).Run(std::move(result));
}

void NetworkCaptureTool::OnCdpEvent(const std::string& event_name,
                                    const base::DictValue& event_params) {
  if (!is_capturing_) {
    return;
  }

  // 이벤트 종류에 따라 처리 분기
  // requestWillBeSent, responseReceived 는 이 핸들러로 라우팅된다.
  // loadingFinished 는 session WeakPtr을 캡처하는 별도 람다로 등록되어
  // OnLoadingFinished를 직접 호출하므로 여기서는 처리하지 않는다.
  if (event_name == "Network.requestWillBeSent") {
    OnRequestWillBeSent(event_params);
  } else if (event_name == "Network.responseReceived") {
    OnResponseReceived(event_params);
  }
}

void NetworkCaptureTool::OnRequestWillBeSent(
    const base::DictValue& params) {
  // requestId 추출 (필수)
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  // request 객체에서 URL, method 추출
  const base::DictValue* request = params.FindDict("request");
  if (!request) {
    return;
  }

  const std::string* url = request->FindString("url");
  if (!url) {
    return;
  }

  // URL 패턴 필터 적용
  if (!url_filter_pattern_.empty() &&
      !MatchesUrlPattern(*url, url_filter_pattern_)) {
    return;
  }

  // 리소스 타입 추출
  const std::string* resource_type = params.FindString("type");
  std::string resource_type_str =
      resource_type ? *resource_type : "Other";

  // 리소스 타입 필터 적용
  if (!resource_type_filter_.empty()) {
    bool matched = false;
    for (const auto& allowed_type : resource_type_filter_) {
      if (base::EqualsCaseInsensitiveASCII(resource_type_str, allowed_type)) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return;
    }
  }

  // 이미 동일한 requestId가 있는지 확인 (redirect 등)
  for (auto& existing : captured_requests_) {
    if (existing.request_id == *request_id) {
      // 리다이렉트: URL 업데이트
      existing.url = *url;
      return;
    }
  }

  // 새 요청 생성
  CapturedRequest req;
  req.request_id = *request_id;
  req.url = *url;

  const std::string* method = request->FindString("method");
  req.method = method ? *method : "GET";
  req.resource_type = resource_type_str;

  // 타임스탬프 (CDP 단위: 초)
  const base::Value* ts = params.Find("timestamp");
  if (ts && ts->is_double()) {
    req.timestamp = ts->GetDouble();
  }

  // 정적 리소스 여부 판단
  req.is_static = IsStaticResource(resource_type_str);

  captured_requests_.push_back(std::move(req));
}

void NetworkCaptureTool::OnResponseReceived(
    const base::DictValue& params) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  // 버퍼에서 requestId에 해당하는 항목 검색
  CapturedRequest* target = nullptr;
  for (auto& req : captured_requests_) {
    if (req.request_id == *request_id) {
      target = &req;
      break;
    }
  }
  if (!target) {
    return;
  }

  // response 객체에서 상태코드, mimeType, headers 추출
  const base::DictValue* response = params.FindDict("response");
  if (!response) {
    return;
  }

  const base::Value* status = response->Find("status");
  if (status && status->is_int()) {
    target->status_code = status->GetInt();
  }

  const std::string* status_text = response->FindString("statusText");
  if (status_text) {
    target->status_text = *status_text;
  }

  const std::string* mime_type = response->FindString("mimeType");
  if (mime_type) {
    target->mime_type = *mime_type;
  }

  // 응답 헤더 복사
  const base::DictValue* headers = response->FindDict("headers");
  if (headers) {
    target->response_headers = headers->Clone();
  }
}

void NetworkCaptureTool::OnLoadingFinished(
    const base::DictValue& params,
    McpSession* session) {
  const std::string* request_id = params.FindString("requestId");
  if (!request_id) {
    return;
  }

  // 버퍼에서 항목 검색
  CapturedRequest* target = nullptr;
  for (auto& req : captured_requests_) {
    if (req.request_id == *request_id) {
      target = &req;
      break;
    }
  }
  if (!target) {
    return;
  }

  // 전송 완료 바이트 수 기록
  const base::Value* encoded_len = params.Find("encodedDataLength");
  if (encoded_len && encoded_len->is_double()) {
    target->encoded_data_length = encoded_len->GetDouble();
  }

  // includeResponseBody=true 이고 session이 있으면 응답 바디 요청
  if (include_response_body_ && session) {
    base::DictValue body_params;
    body_params.Set("requestId", *request_id);

    LOG(INFO) << "[MCP][NetworkCapture] 응답 바디 요청: " << *request_id;

    session->SendCdpCommand(
        "Network.getResponseBody", std::move(body_params),
        base::BindOnce(&NetworkCaptureTool::OnResponseBodyFetched,
                       weak_factory_.GetWeakPtr(), *request_id));
  }
}

void NetworkCaptureTool::OnResponseBodyFetched(
    const std::string& request_id,
    base::Value response) {
  // 버퍼에서 항목 검색
  CapturedRequest* target = nullptr;
  for (auto& req : captured_requests_) {
    if (req.request_id == request_id) {
      target = &req;
      break;
    }
  }
  if (!target) {
    return;
  }

  if (!response.is_dict()) {
    return;
  }

  const base::DictValue& result = response.GetDict();

  // error 키가 있으면 바디 수집 실패 (캐시 미스, 리소스 취소 등)
  if (result.FindDict("error")) {
    LOG(WARNING) << "[MCP][NetworkCapture] 응답 바디 수집 실패: "
                 << request_id;
    return;
  }

  // body 또는 base64Encoded 키에서 바디 추출
  const std::string* body = result.FindString("body");
  if (body) {
    target->response_body = *body;
    target->response_body_loaded = true;
    LOG(INFO) << "[MCP][NetworkCapture] 응답 바디 수집 완료: "
              << request_id << " (" << body->size() << " bytes)";
  }
}

// static
bool NetworkCaptureTool::IsStaticResource(
    const std::string& resource_type) {
  // Chromium DevTools 리소스 타입 분류 기준
  static constexpr const char* kStaticTypes[] = {
      "Image", "Font", "Stylesheet", "Media",
  };
  for (const char* static_type : kStaticTypes) {
    if (base::EqualsCaseInsensitiveASCII(resource_type, static_type)) {
      return true;
    }
  }
  return false;
}

// static
bool NetworkCaptureTool::MatchesUrlPattern(const std::string& url,
                                           const std::string& pattern) {
  if (pattern.empty() || pattern == "*") {
    return true;
  }

  // 단순 와일드카드 매칭: '*' 를 기준으로 분리하여 순서대로 포함 여부 확인
  // 예: "*.example.com/api/*" → "example.com" 포함 + "/api/" 포함
  std::vector<std::string_view> parts = base::SplitStringPiece(
      pattern, "*", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY);

  if (parts.empty()) {
    return true;
  }

  size_t search_pos = 0;
  bool starts_with_wildcard = pattern.front() == '*';

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

base::Value NetworkCaptureTool::SerializeRequests() const {
  // captured_requests_ 를 MCP tools/call 응답 형식으로 직렬화
  base::ListValue requests_list;

  for (const auto& req : captured_requests_) {
    base::DictValue req_dict;
    req_dict.Set("requestId", req.request_id);
    req_dict.Set("url", req.url);
    req_dict.Set("method", req.method);
    req_dict.Set("resourceType", req.resource_type);
    req_dict.Set("timestamp", req.timestamp);
    req_dict.Set("statusCode", req.status_code);
    req_dict.Set("statusText", req.status_text);
    req_dict.Set("mimeType", req.mime_type);
    req_dict.Set("encodedDataLength", req.encoded_data_length);
    req_dict.Set("isStatic", req.is_static);
    req_dict.Set("responseHeaders", req.response_headers.Clone());

    if (req.response_body_loaded) {
      req_dict.Set("responseBody", req.response_body);
    }

    requests_list.Append(std::move(req_dict));
  }

  // 결과를 JSON 문자열로 직렬화하여 content[].text 에 담아 반환
  base::DictValue data;
  data.Set("capturedCount",
           static_cast<int>(captured_requests_.size()));
  data.Set("requests", std::move(requests_list));

  std::string json_str;
  base::JSONWriter::WriteWithOptions(
      base::Value(std::move(data)),
      base::JSONWriter::OPTIONS_PRETTY_PRINT,
      &json_str);

  base::DictValue result;
  base::ListValue content;
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text", json_str);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));

  return base::Value(std::move(result));
}

// ============================================================================
// NetworkRequestsTool 구현
// ============================================================================

NetworkRequestsTool::NetworkRequestsTool(NetworkCaptureTool* capture_tool)
    : capture_tool_(capture_tool) {
  DCHECK(capture_tool_);
}

NetworkRequestsTool::~NetworkRequestsTool() = default;

std::string NetworkRequestsTool::name() const {
  return "network_requests";
}

std::string NetworkRequestsTool::description() const {
  return "현재까지 캡처된 네트워크 요청 목록을 반환합니다. "
         "network_capture(action=stop)과 달리 캡처를 중지하지 않고 "
         "중간 결과를 조회할 수 있습니다. "
         "includeStatic=false(기본값)이면 이미지, 폰트 등 "
         "정적 리소스를 결과에서 제외합니다.";
}

base::DictValue NetworkRequestsTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // includeStatic: 정적 리소스 포함 여부 (기본값 false)
  base::DictValue include_static_prop;
  include_static_prop.Set("type", "boolean");
  include_static_prop.Set("description",
                          "true이면 이미지, 폰트, 스타일시트, 미디어 등 "
                          "정적 리소스도 결과에 포함합니다. "
                          "기본값은 false (정적 리소스 제외).");
  properties.Set("includeStatic", std::move(include_static_prop));

  schema.Set("properties", std::move(properties));
  return schema;
}

void NetworkRequestsTool::Execute(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // includeStatic 파라미터 (기본값 false)
  bool include_static = false;
  const base::Value* include_static_val = arguments.Find("includeStatic");
  if (include_static_val && include_static_val->is_bool()) {
    include_static = include_static_val->GetBool();
  }

  // capture_tool_ 의 captured_requests_ 에서 필터링하여 반환
  const std::vector<CapturedRequest>& all_requests =
      capture_tool_->captured_requests_;

  base::ListValue requests_list;
  int filtered_count = 0;

  for (const auto& req : all_requests) {
    // includeStatic=false 이면 정적 리소스 제외
    if (!include_static && req.is_static) {
      ++filtered_count;
      continue;
    }

    base::DictValue req_dict;
    req_dict.Set("requestId", req.request_id);
    req_dict.Set("url", req.url);
    req_dict.Set("method", req.method);
    req_dict.Set("resourceType", req.resource_type);
    req_dict.Set("timestamp", req.timestamp);
    req_dict.Set("statusCode", req.status_code);
    req_dict.Set("statusText", req.status_text);
    req_dict.Set("mimeType", req.mime_type);
    req_dict.Set("encodedDataLength", req.encoded_data_length);
    req_dict.Set("isStatic", req.is_static);

    if (req.response_body_loaded) {
      req_dict.Set("responseBody", req.response_body);
    }

    requests_list.Append(std::move(req_dict));
  }

  LOG(INFO) << "[MCP][NetworkRequests] 요청 목록 반환: "
            << requests_list.size() << "개 (정적 리소스 제외: "
            << filtered_count << "개)";

  // 결과 직렬화
  base::DictValue data;
  data.Set("total", static_cast<int>(all_requests.size()));
  data.Set("returned", static_cast<int>(requests_list.size()));
  data.Set("filteredStatic", filtered_count);
  data.Set("requests", std::move(requests_list));

  std::string json_str;
  base::JSONWriter::WriteWithOptions(
      base::Value(std::move(data)),
      base::JSONWriter::OPTIONS_PRETTY_PRINT,
      &json_str);

  base::DictValue result;
  base::ListValue content;
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text", json_str);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));

  std::move(callback).Run(base::Value(std::move(result)));
}

}  // namespace mcp
