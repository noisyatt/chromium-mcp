// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_MCP_TRANSPORT_SOCKET_H_
#define CHROME_BROWSER_MCP_MCP_TRANSPORT_SOCKET_H_

#include <map>
#include <memory>
#include <string>

#include "base/files/file_descriptor_watcher_posix.h"
#include "base/files/scoped_file.h"
#include "base/memory/weak_ptr.h"
#include "base/synchronization/lock.h"
#include "base/threading/thread.h"
#include "chrome/browser/mcp/mcp_transport.h"

namespace mcp {

// Unix domain socket 기반 MCP 전송 계층.
//
// 브라우저가 이미 실행 중일 때 외부 MCP 클라이언트가 소켓으로 연결하는 경우
// 사용한다. 로컬 LLM 클라이언트나 자동화 도구 연동에 적합하다.
//
// 보안 설계:
//   - 소켓 파일은 숨김 파일 경로 사용 (/tmp/.chromium-mcp.sock)
//   - 파일 권한을 0600으로 설정 (소유자만 읽기/쓰기 가능)
//   - 다른 사용자의 접속을 OS 수준에서 차단
//
// 동작 방식:
//   - UI 스레드에서 base::FileDescriptorWatcher로 accept() 이벤트 감시
//   - 클라이언트 연결 수락 후 각 클라이언트 전용 스레드에서 블로킹 read()
//   - Content-Length 프레이밍으로 메시지 경계 처리
//   - 멀티 클라이언트 지원: 각 클라이언트마다 별도 IO 스레드로 관리
//   - Send()는 모든 연결된 클라이언트에 브로드캐스트
class McpTransportSocket : public McpTransport {
 public:
  // socket_path: Unix 소켓 파일 경로.
  //   기본값은 /tmp/.chromium-mcp.sock (숨김 파일).
  explicit McpTransportSocket(
      const std::string& socket_path = "/tmp/.chromium-mcp.sock");
  ~McpTransportSocket() override;

  // McpTransport 구현
  void Start(MessageCallback message_cb,
             DisconnectCallback disconnect_cb) override;
  void Stop() override;
  void Send(int client_id, const std::string& json_message) override;
  bool IsConnected() const override;

  // 현재 소켓 파일 경로를 반환한다.
  const std::string& socket_path() const { return socket_path_; }

 private:
  // 개별 클라이언트 연결 정보를 담는 구조체.
  // 각 클라이언트는 고유 ID, 소켓 fd, 전용 IO 스레드를 가진다.
  struct ClientInfo {
    ClientInfo();
    ~ClientInfo();

    // 클라이언트 고유 식별자
    int id = 0;
    // 클라이언트 소켓 fd
    base::ScopedFD fd;
    // 클라이언트 전용 IO 스레드 (블로킹 read 실행용)
    std::unique_ptr<base::Thread> io_thread;
  };

  // Unix domain socket을 생성하고 바인딩한다.
  // 성공 시 true, 실패 시 false 반환.
  bool CreateAndBindSocket();

  // 소켓 파일 권한을 0600으로 설정한다.
  // 소유자만 접속할 수 있도록 보안을 강화한다.
  bool SetSocketPermissions();

  // accept() 이벤트 핸들러 (FileDescriptorWatcher 콜백).
  // UI 스레드에서 호출된다.
  void OnAcceptReady();

  // 특정 클라이언트의 소켓에서 메시지를 읽는 루프.
  // 해당 클라이언트의 io_thread에서 실행된다.
  // client_id: 읽기 대상 클라이언트 ID
  // fd: 읽기 대상 소켓 fd (raw, ClientInfo에서 소유)
  void ReadLoop(int client_id, int fd);

  // Content-Length 헤더 파싱.
  bool ParseContentLengthHeader(const std::string& header_line,
                                size_t* out_length) const;

  // 정확히 n바이트를 fd에서 읽어 out에 저장한다.
  bool ReadExactBytes(int fd, size_t n, std::string* out) const;

  // fd에서 줄 하나를 읽어 out에 저장한다 (\r\n 처리 포함).
  bool ReadLine(int fd, std::string* out) const;

  // UI 스레드로 메시지를 포스팅한다 (client_id 태깅).
  void PostMessageToUIThread(int client_id, std::string message);

  // 특정 클라이언트 연결 종료를 처리한다 (UI 스레드에서 실행).
  // client_id에 해당하는 클라이언트를 clients_ 맵에서 제거한다.
  void HandleClientDisconnect(int client_id);


  // 소켓 파일 경로 (예: /tmp/.chromium-mcp.sock)
  const std::string socket_path_;

  // 리스닝 소켓 fd
  base::ScopedFD listen_fd_;

  // listen_fd_ 읽기 이벤트 감시기
  std::unique_ptr<base::FileDescriptorWatcher::Controller> accept_watcher_;

  // Start() 호출 스레드(UI 스레드) 태스크 러너
  scoped_refptr<base::SequencedTaskRunner> ui_task_runner_;

  // 수신 메시지 콜백
  MessageCallback message_cb_;

  // 연결 종료 콜백
  DisconnectCallback disconnect_cb_;

  // 소켓 쓰기 직렬화 락 (Send 브로드캐스트 시 clients_ 순회 보호)
  // IsConnected()는 const 메서드이므로 mutable 선언 필요
  mutable base::Lock write_lock_;

  // 전송 계층 활성 상태
  bool running_ = false;

  // 다음 클라이언트에 부여할 고유 ID (단조 증가)
  int next_client_id_ = 0;

  // 연결된 클라이언트 맵: client_id -> ClientInfo
  // UI 스레드에서만 접근한다 (accept/disconnect 모두 UI 스레드).
  // Send()에서의 fd 접근은 write_lock_으로 보호한다.
  std::map<int, std::unique_ptr<ClientInfo>> clients_;

  // 약한 포인터 팩토리 (비동기 콜백 수명 관리)
  base::WeakPtrFactory<McpTransportSocket> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_MCP_TRANSPORT_SOCKET_H_
