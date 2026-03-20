// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_MCP_TRANSPORT_H_
#define CHROME_BROWSER_MCP_MCP_TRANSPORT_H_

#include <string>

#include "base/functional/callback.h"

namespace mcp {

// MCP 전송 계층 추상 인터페이스.
//
// 구체적인 전송 방식(stdio, Unix socket 등)에 무관하게
// 상위 계층(McpServer)이 동일한 방식으로 메시지를 주고받을 수 있도록 한다.
//
// 메시지 포맷: Content-Length 기반 프레이밍 (LSP/MCP 표준)
//   Content-Length: <byte_length>\r\n
//   \r\n
//   <json_body>
class McpTransport {
 public:
  // 메시지 수신 콜백 타입.
  // client_id: 메시지를 보낸 클라이언트 식별자.
  // 완전한 JSON 메시지 본문(바디만, 헤더 제외)이 전달된다.
  using MessageCallback =
      base::RepeatingCallback<void(int client_id, const std::string&)>;

  // 연결 종료 콜백 타입.
  // client_id: 연결이 종료된 클라이언트 식별자.
  // 전송 계층이 닫히거나 에러가 발생할 때 호출된다.
  using DisconnectCallback = base::RepeatingCallback<void(int client_id)>;

  virtual ~McpTransport() = default;

  // 전송 계층을 시작한다.
  // message_cb: 완전한 JSON 메시지가 수신될 때마다 호출 (UI 스레드)
  // disconnect_cb: 연결이 끊어질 때 호출 (UI 스레드)
  virtual void Start(MessageCallback message_cb,
                     DisconnectCallback disconnect_cb) = 0;

  // 전송 계층을 종료한다.
  // Start() 이후 언제든 호출 가능. 멱등(idempotent)해야 한다.
  virtual void Stop() = 0;

  // JSON 메시지를 특정 클라이언트에게 Content-Length 프레이밍으로 송신한다.
  // client_id: 메시지를 보낼 대상 클라이언트. -1이면 전체 브로드캐스트.
  // json_message: 전송할 JSON 문자열 (헤더 없이 바디만)
  // 내부에서 Content-Length 헤더를 자동으로 붙여 전송한다.
  virtual void Send(int client_id, const std::string& json_message) = 0;

  // 전송 계층이 현재 연결(활성) 상태인지 반환한다.
  virtual bool IsConnected() const = 0;
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_MCP_TRANSPORT_H_
