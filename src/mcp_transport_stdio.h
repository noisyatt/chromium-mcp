// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_MCP_TRANSPORT_STDIO_H_
#define CHROME_BROWSER_MCP_MCP_TRANSPORT_STDIO_H_

#include <atomic>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/synchronization/lock.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "chrome/browser/mcp/mcp_transport.h"

namespace mcp {

// stdio 기반 MCP 전송 계층.
//
// MCP 클라이언트가 프로세스를 직접 실행하여 stdin/stdout으로 통신하는 경우
// 사용한다. Claude Desktop 등 표준 MCP 호스트가 이 방식을 사용한다.
//
// 동작 방식:
//   - 전용 스레드(io_thread_)에서 stdin을 블로킹으로 읽는다.
//   - Content-Length 헤더를 파싱하여 메시지 경계를 확인한다.
//   - 완전한 메시지가 수신되면 UI 스레드로 콜백을 포스팅한다.
//   - Send()는 stdout에 Content-Length 프레이밍을 붙여 쓴다.
//   - stdout 쓰기는 락으로 직렬화한다 (여러 스레드에서 호출 가능).
class McpTransportStdio : public McpTransport {
 public:
  McpTransportStdio();
  ~McpTransportStdio() override;

  // McpTransport 구현
  void Start(MessageCallback message_cb,
             DisconnectCallback disconnect_cb) override;
  void Stop() override;
  void Send(int client_id, const std::string& json_message) override;
  bool IsConnected() const override;

 private:
  // stdin 읽기 루프. io_thread_ 에서 실행된다.
  // Content-Length 헤더를 읽고, 그 길이만큼 JSON 바디를 읽는다.
  void ReadLoop();

  // Content-Length 헤더 행 파싱.
  // "Content-Length: N\r\n" 형식에서 N을 추출한다.
  // 성공 시 true, 실패 시 false 반환.
  bool ParseContentLengthHeader(const std::string& header_line,
                                size_t* out_length) const;

  // stdin에서 정확히 n바이트를 읽어 out에 저장한다.
  // EOF 또는 에러 시 false 반환.
  bool ReadExactBytes(size_t n, std::string* out) const;

  // stdin에서 \n이 나올 때까지 한 줄을 읽어 out에 저장한다.
  // \r\n도 처리하며, out에는 줄바꿈 문자를 포함하지 않는다.
  // EOF 또는 에러 시 false 반환.
  bool ReadLine(std::string* out) const;

  // UI 스레드에서 message_cb_를 호출하도록 포스팅한다.
  void PostMessageToUIThread(std::string message);

  // stdin 읽기 전용 스레드
  base::Thread io_thread_;

  // UI 스레드(또는 Start() 호출 스레드) 태스크 러너
  scoped_refptr<base::SequencedTaskRunner> ui_task_runner_;

  // 수신 메시지 콜백 (UI 스레드에서 호출)
  MessageCallback message_cb_;

  // 연결 종료 콜백 (UI 스레드에서 호출)
  DisconnectCallback disconnect_cb_;

  // stdout 동시 쓰기 방지 락
  base::Lock write_lock_;

  // 활성 상태 플래그 (스레드 간 공유)
  std::atomic<bool> running_{false};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_MCP_TRANSPORT_STDIO_H_
