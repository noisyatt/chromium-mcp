// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/mcp_transport_socket.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"

namespace mcp {

namespace {

// 단일 메시지 최대 크기: 64MB
constexpr size_t kMaxMessageSize = 64 * 1024 * 1024;
// Content-Length 헤더 접두사
constexpr char kContentLengthPrefix[] = "Content-Length: ";
// CRLF
constexpr char kCRLF[] = "\r\n";
// 소켓 연결 대기 큐 크기 (동시 연결 수락 대기 수)
constexpr int kListenBacklog = 5;

}  // namespace

McpTransportSocket::McpTransportSocket(const std::string& socket_path)
    : socket_path_(socket_path),
      io_thread_("MCP-socket-reader") {}

McpTransportSocket::~McpTransportSocket() {
  Stop();
}

void McpTransportSocket::Start(MessageCallback message_cb,
                               DisconnectCallback disconnect_cb) {
  DCHECK(!running_) << "Start()를 중복 호출함";

  message_cb_ = std::move(message_cb);
  disconnect_cb_ = std::move(disconnect_cb);
  ui_task_runner_ = base::SequencedTaskRunner::GetCurrentDefault();

  // Unix domain socket 생성 및 바인딩
  if (!CreateAndBindSocket()) {
    LOG(ERROR) << "[MCP] 소켓 생성/바인딩 실패: " << socket_path_;
    return;
  }

  // 소켓 파일 권한 0600 설정 (소유자만 접근 가능)
  if (!SetSocketPermissions()) {
    LOG(ERROR) << "[MCP] 소켓 파일 권한 설정 실패: " << socket_path_;
    return;
  }

  // accept() 이벤트 감시 시작.
  // FileDescriptorWatcher는 UI 스레드에서 실행되어야 한다.
  accept_watcher_ = base::FileDescriptorWatcher::WatchReadable(
      listen_fd_.get(),
      base::BindRepeating(&McpTransportSocket::OnAcceptReady,
                          weak_factory_.GetWeakPtr()));

  running_ = true;
  LOG(INFO) << "[MCP] Unix socket 전송 계층 시작됨: " << socket_path_;
}

void McpTransportSocket::Stop() {
  if (!running_) {
    return;
  }

  running_ = false;
  client_connected_ = false;

  // FileDescriptorWatcher 해제
  accept_watcher_.reset();

  // 클라이언트 소켓 닫기
  client_fd_.reset();

  // 리스닝 소켓 닫기
  listen_fd_.reset();

  // 소켓 파일 삭제 (다음 시작 시 바인딩 실패 방지)
  if (!socket_path_.empty()) {
    unlink(socket_path_.c_str());
  }

  // IO 스레드 중지
  io_thread_.Stop();

  LOG(INFO) << "[MCP] Unix socket 전송 계층 중지됨";
}

void McpTransportSocket::Send(const std::string& json_message) {
  if (!client_connected_) {
    LOG(WARNING) << "[MCP] Send(): 클라이언트가 연결되지 않음";
    return;
  }

  // Content-Length 기반 메시지 프레이밍
  // 형식: "Content-Length: N\r\n\r\n{json}"
  std::string framed =
      std::string(kContentLengthPrefix) +
      base::NumberToString(json_message.size()) +
      kCRLF + kCRLF +
      json_message;

  // 동시 쓰기 직렬화
  base::AutoLock guard(write_lock_);

  int fd = client_fd_.get();
  if (fd < 0) {
    LOG(WARNING) << "[MCP] Send(): 유효하지 않은 클라이언트 fd";
    return;
  }

  const char* data = framed.data();
  const size_t total = framed.size();
  size_t written = 0;

  // 부분 쓰기 처리를 위해 반복 쓰기
  while (written < total) {
    ssize_t result = HANDLE_EINTR(write(fd, data + written, total - written));
    if (result < 0) {
      LOG(ERROR) << "[MCP] 소켓 쓰기 실패: errno=" << errno;
      return;
    }
    written += static_cast<size_t>(result);
  }
}

bool McpTransportSocket::IsConnected() const {
  return client_connected_;
}

bool McpTransportSocket::CreateAndBindSocket() {
  // 기존 소켓 파일이 있으면 먼저 삭제한다.
  // 이전 실행에서 정상 종료되지 않아 파일이 남아있을 수 있다.
  unlink(socket_path_.c_str());

  // AF_UNIX 도메인 소켓 생성
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    LOG(ERROR) << "[MCP] socket() 실패: errno=" << errno;
    return false;
  }
  listen_fd_.reset(fd);

  // 소켓 주소 설정
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;

  if (socket_path_.size() >= sizeof(addr.sun_path)) {
    LOG(ERROR) << "[MCP] 소켓 경로가 너무 김: " << socket_path_;
    return false;
  }
  strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

  // 소켓 파일에 바인딩
  if (bind(fd, reinterpret_cast<const struct sockaddr*>(&addr),
           sizeof(addr)) < 0) {
    LOG(ERROR) << "[MCP] bind() 실패: errno=" << errno
               << " path=" << socket_path_;
    return false;
  }

  // 연결 대기 큐 설정
  if (listen(fd, kListenBacklog) < 0) {
    LOG(ERROR) << "[MCP] listen() 실패: errno=" << errno;
    return false;
  }

  return true;
}

bool McpTransportSocket::SetSocketPermissions() {
  // 소켓 파일 권한을 0600으로 설정한다.
  // 이렇게 하면 소유자(현재 사용자)만 읽기/쓰기 가능하고,
  // 그룹 및 다른 사용자는 접근 불가하다.
  if (chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR) < 0) {
    LOG(ERROR) << "[MCP] chmod(0600) 실패: errno=" << errno
               << " path=" << socket_path_;
    return false;
  }

  LOG(INFO) << "[MCP] 소켓 파일 권한 0600 설정: " << socket_path_;
  return true;
}

void McpTransportSocket::OnAcceptReady() {
  // UI 스레드에서 호출된다. (FileDescriptorWatcher 콜백)

  // 이미 클라이언트가 연결되어 있으면 새 연결을 거절한다.
  // 현재 구현은 단일 클라이언트만 지원한다.
  if (client_connected_) {
    struct sockaddr_un addr;
    socklen_t addr_len = sizeof(addr);
    int new_fd = HANDLE_EINTR(
        accept(listen_fd_.get(),
               reinterpret_cast<struct sockaddr*>(&addr), &addr_len));
    if (new_fd >= 0) {
      LOG(WARNING) << "[MCP] 이미 클라이언트 연결 중 - 새 연결 거절";
      close(new_fd);
    }
    return;
  }

  // 새 클라이언트 연결 수락
  struct sockaddr_un client_addr;
  socklen_t client_addr_len = sizeof(client_addr);
  int client_fd = HANDLE_EINTR(
      accept(listen_fd_.get(),
             reinterpret_cast<struct sockaddr*>(&client_addr),
             &client_addr_len));

  if (client_fd < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      LOG(ERROR) << "[MCP] accept() 실패: errno=" << errno;
    }
    return;
  }

  client_fd_.reset(client_fd);
  client_connected_ = true;

  LOG(INFO) << "[MCP] 클라이언트 연결 수락됨 (fd=" << client_fd << ")";

  // 클라이언트 소켓 읽기를 전용 IO 스레드에서 시작한다.
  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  if (!io_thread_.IsRunning()) {
    io_thread_.StartWithOptions(std::move(options));
  }

  io_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&McpTransportSocket::ReadLoop, base::Unretained(this)));
}

void McpTransportSocket::ReadLoop() {
  // IO 스레드에서 실행된다.
  // client_fd_에서 Content-Length 프레이밍된 MCP 메시지를 읽는다.
  LOG(INFO) << "[MCP] 소켓 읽기 루프 시작";

  const int fd = client_fd_.get();

  while (running_ && client_connected_) {
    // --- 1단계: Content-Length 헤더 파싱 ---
    size_t content_length = 0;
    bool found_content_length = false;

    while (true) {
      std::string header_line;
      if (!ReadLine(fd, &header_line)) {
        LOG(INFO) << "[MCP] 소켓 헤더 읽기 실패 또는 EOF";
        HandleClientDisconnect();
        return;
      }

      if (header_line.empty()) {
        // 빈 줄: 헤더 종료
        break;
      }

      size_t length = 0;
      if (ParseContentLengthHeader(header_line, &length)) {
        content_length = length;
        found_content_length = true;
      }
    }

    if (!found_content_length) {
      LOG(WARNING) << "[MCP] Content-Length 헤더 없음, 건너뜀";
      continue;
    }

    if (content_length == 0 || content_length > kMaxMessageSize) {
      LOG(WARNING) << "[MCP] 비정상 Content-Length: " << content_length;
      HandleClientDisconnect();
      return;
    }

    // --- 2단계: JSON 바디 읽기 ---
    std::string body;
    if (!ReadExactBytes(fd, content_length, &body)) {
      LOG(ERROR) << "[MCP] 소켓 바디 읽기 실패";
      HandleClientDisconnect();
      return;
    }

    // --- 3단계: UI 스레드로 메시지 포스팅 ---
    PostMessageToUIThread(std::move(body));
  }

  LOG(INFO) << "[MCP] 소켓 읽기 루프 종료";
}

bool McpTransportSocket::ParseContentLengthHeader(
    const std::string& header_line,
    size_t* out_length) const {
  if (!base::StartsWith(header_line, kContentLengthPrefix,
                        base::CompareCase::INSENSITIVE_ASCII)) {
    return false;
  }

  std::string value_str =
      header_line.substr(std::string(kContentLengthPrefix).size());
  base::TrimWhitespaceASCII(value_str, base::TRIM_ALL, &value_str);

  size_t length = 0;
  if (!base::StringToSizeT(value_str, &length)) {
    LOG(WARNING) << "[MCP] Content-Length 파싱 실패: '" << value_str << "'";
    return false;
  }

  *out_length = length;
  return true;
}

bool McpTransportSocket::ReadExactBytes(int fd,
                                        size_t n,
                                        std::string* out) const {
  out->resize(n);
  char* buf = &(*out)[0];
  size_t total_read = 0;

  while (total_read < n) {
    ssize_t result = HANDLE_EINTR(read(fd, buf + total_read, n - total_read));
    if (result == 0) {
      return false;  // EOF
    }
    if (result < 0) {
      LOG(ERROR) << "[MCP] 소켓 read() 실패: errno=" << errno;
      return false;
    }
    total_read += static_cast<size_t>(result);
  }

  return true;
}

bool McpTransportSocket::ReadLine(int fd, std::string* out) const {
  out->clear();

  while (true) {
    char c;
    ssize_t result = HANDLE_EINTR(read(fd, &c, 1));
    if (result == 0) {
      return !out->empty();  // EOF
    }
    if (result < 0) {
      return false;
    }

    if (c == '\n') {
      // \r\n 처리: 마지막 \r 제거
      if (!out->empty() && out->back() == '\r') {
        out->pop_back();
      }
      return true;
    }

    out->push_back(c);
  }
}

void McpTransportSocket::PostMessageToUIThread(std::string message) {
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](MessageCallback cb, std::string msg) {
            if (cb) cb.Run(msg);
          },
          message_cb_, std::move(message)));
}

void McpTransportSocket::HandleClientDisconnect() {
  // IO 스레드에서 호출. UI 스레드에 클라이언트 연결 종료를 알린다.
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<McpTransportSocket> self,
             DisconnectCallback disconnect_cb) {
            if (!self) {
              return;
            }
            self->client_connected_ = false;
            self->client_fd_.reset();
            LOG(INFO) << "[MCP] 클라이언트 연결 종료됨";
            if (disconnect_cb) {
              std::move(disconnect_cb).Run();
            }
          },
          weak_factory_.GetWeakPtr(), disconnect_cb_));
}

}  // namespace mcp
