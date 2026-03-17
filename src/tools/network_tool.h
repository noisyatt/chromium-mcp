// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_NETWORK_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_NETWORK_TOOL_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 개별 네트워크 요청을 나타내는 내부 데이터 구조체.
// Network.requestWillBeSent, Network.responseReceived,
// Network.loadingFinished 이벤트를 통해 채워진다.
struct CapturedRequest {
  CapturedRequest();
  ~CapturedRequest();
  CapturedRequest(CapturedRequest&&);
  CapturedRequest& operator=(CapturedRequest&&);

  // requestWillBeSent에서 채워지는 필드
  std::string request_id;
  std::string url;
  std::string method;
  std::string resource_type;  // "Document", "XHR", "Fetch", "Script" 등
  double timestamp = 0.0;     // CDP 타임스탬프 (초 단위)

  // responseReceived에서 채워지는 필드
  int status_code = 0;
  std::string status_text;
  std::string mime_type;
  base::DictValue response_headers;

  // loadingFinished에서 채워지는 필드
  double encoded_data_length = 0.0;

  // Network.getResponseBody 호출 결과 (includeResponseBody=true 일 때)
  std::string response_body;
  bool response_body_loaded = false;

  // 정적 리소스 여부 (image, font, stylesheet, script 등)
  bool is_static = false;
};

// NetworkCaptureTool: 네트워크 요청 캡처를 시작/중지하는 도구.
//
// action=start 시:
//   - Network.enable CDP 명령으로 이벤트 스트림 활성화
//   - Network.requestWillBeSent / responseReceived / loadingFinished 이벤트
//     수신 시 내부 captured_requests_ 버퍼에 저장
//   - includeResponseBody=true 이면 loadingFinished 수신 후
//     Network.getResponseBody를 호출하여 응답 바디를 버퍼에 추가
//
// action=stop 시:
//   - Network.disable CDP 명령으로 이벤트 스트림 비활성화
//   - 누적된 captured_requests_ 버퍼를 JSON 배열로 직렬화하여 반환
//   - 반환 후 버퍼 초기화
//
// 내부 CDP 세션을 사용하므로 chrome.debugger의 노란 배너가 표시되지 않는다.
class NetworkCaptureTool : public McpTool {
 public:
  NetworkCaptureTool();
  ~NetworkCaptureTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // action=start 처리: Network.enable 전송 후 이벤트 수신 대기 시작.
  // |include_response_body|: 응답 바디 자동 수집 여부
  // |filter|: URL 패턴 / 리소스 타입 필터 (현재 세션에 저장)
  void HandleStart(bool include_response_body,
                   const base::DictValue* filter,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // action=stop 처리: Network.disable 전송 후 버퍼를 직렬화하여 반환.
  void HandleStop(McpSession* session,
                  base::OnceCallback<void(base::Value)> callback);

  // Network.enable CDP 명령 응답 처리.
  // 성공하면 이벤트 핸들러 등록 상태를 기록한다.
  void OnNetworkEnabled(McpSession* session,
                        base::OnceCallback<void(base::Value)> callback,
                        base::Value response);

  // Network.disable CDP 명령 응답 처리.
  // 성공하면 captured_requests_ 버퍼를 직렬화하여 콜백 호출.
  void OnNetworkDisabled(base::OnceCallback<void(base::Value)> callback,
                         base::Value response);

  // CDP 이벤트 수신 시 호출되는 내부 핸들러.
  // McpSession이 이벤트를 전달하기 위해 사용한다.
  void OnCdpEvent(const std::string& event_name,
                  const base::DictValue& event_params);

  // Network.requestWillBeSent 이벤트 처리.
  // 새 CapturedRequest를 생성하여 버퍼에 추가한다.
  void OnRequestWillBeSent(const base::DictValue& params);

  // Network.responseReceived 이벤트 처리.
  // 버퍼에서 해당 requestId 항목을 찾아 응답 정보를 업데이트한다.
  void OnResponseReceived(const base::DictValue& params);

  // Network.loadingFinished 이벤트 처리.
  // 전송 완료된 바이트 수를 기록하고, includeResponseBody=true 이면
  // Network.getResponseBody CDP 명령을 비동기 호출한다.
  void OnLoadingFinished(const base::DictValue& params,
                         McpSession* session);

  // Network.getResponseBody CDP 응답 처리.
  // |request_id|: 어느 요청의 바디인지 식별자
  void OnResponseBodyFetched(const std::string& request_id,
                             base::Value response);

  // 리소스 타입이 정적 리소스(이미지/폰트/스크립트/스타일시트)인지 판단.
  static bool IsStaticResource(const std::string& resource_type);

  // URL 패턴 필터 매칭 (단순 와일드카드 지원: * 는 임의 문자열).
  // |pattern|이 비어 있으면 항상 true 반환.
  static bool MatchesUrlPattern(const std::string& url,
                                const std::string& pattern);

  // captured_requests_ 벡터를 MCP tools/call 응답 형식의 JSON으로 직렬화.
  base::Value SerializeRequests() const;

  // -----------------------------------------------------------------------
  // 상태 변수
  // -----------------------------------------------------------------------

  // 현재 캡처 중인지 여부
  bool is_capturing_ = false;

  // 응답 바디 자동 수집 여부 (start 시 설정)
  bool include_response_body_ = false;

  // URL 패턴 필터 (비어있으면 전체 허용)
  std::string url_filter_pattern_;

  // 허용할 리소스 타입 목록 (비어있으면 전체 허용)
  std::vector<std::string> resource_type_filter_;

  // 캡처된 요청 버퍼: requestId 순서대로 저장
  std::vector<CapturedRequest> captured_requests_;

  // NetworkRequestsTool이 버퍼를 읽기 전용으로 접근할 수 있도록 허용.
  friend class NetworkRequestsTool;

  // 약한 참조 팩토리 (비동기 CDP 콜백에서 this 댕글링 방지)
  base::WeakPtrFactory<NetworkCaptureTool> weak_factory_{this};
};

// NetworkRequestsTool: 현재까지 캡처된 네트워크 요청 목록을 반환하는 도구.
//
// NetworkCaptureTool과 동일한 captured_requests_ 버퍼를 공유한다.
// capture를 stop하지 않고도 중간 결과를 조회할 수 있다.
//
// includeStatic=false(기본값)이면 이미지, 폰트, 스크립트, 스타일시트 등
// 정적 리소스를 결과에서 제외한다.
class NetworkRequestsTool : public McpTool {
 public:
  // |capture_tool|: 같은 버퍼를 공유하는 NetworkCaptureTool 참조.
  // McpServer 또는 레지스트리가 두 도구의 생명주기를 함께 관리해야 한다.
  explicit NetworkRequestsTool(NetworkCaptureTool* capture_tool);
  ~NetworkRequestsTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 공유 캡처 도구: 버퍼 접근에 사용. 소유권은 갖지 않음.
  raw_ptr<NetworkCaptureTool> capture_tool_;
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_NETWORK_TOOL_H_
