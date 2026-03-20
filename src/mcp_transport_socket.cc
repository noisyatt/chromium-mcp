// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/mcp_transport_socket.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "base/containers/span.h"
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

McpTransportSocket::ClientInfo::ClientInfo() = default;
McpTransportSocket::ClientInfo::~ClientInfo() = default;

McpTransportSocket::McpTransportSocket(const std::string& socket_path)
    : socket_path_(socket_path) {}

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
  LOG(INFO) << "[MCP] Unix socket 전송 계층 시작됨 (멀티 클라이언트): "
            << socket_path_;
}

void McpTransportSocket::Stop() {
  if (!running_) {
    return;
  }

  running_ = false;

  // FileDescriptorWatcher 해제
  accept_watcher_.reset();

  // 모든 클라이언트 소켓을 닫아 블로킹 read()를 깨운다.
  // 1) 먼저 클라이언트 맵을 스왑으로 추출 (락 최소화)
  // 2) 각 ClientInfo의 fd를 닫아 ReadLoop가 에러로 종료되게 한다.
  // 3) IO 스레드 Stop()을 호출하여 스레드 완전 종료를 대기한다.
  //    (Thread::Stop()은 락을 잡은 채로 호출하면 데드락 위험이 있으므로
  //     락 외부에서 호출한다.)
  std::map<int, std::unique_ptr<ClientInfo>> clients_to_stop;
  {
    base::AutoLock lock(write_lock_);
    clients_to_stop.swap(clients_);
  }

  // fd 닫기 → ReadLoop 종료 → Thread::Stop() 순서로 정리
  for (auto& [id, info] : clients_to_stop) {
    // fd를 먼저 닫아 블로킹 read()를 깨운다.
    info->fd.reset();
    // IO 스레드 종료 대기 (ReadLoop가 이미 return하여 안전)
    if (info->io_thread && info->io_thread->IsRunning()) {
      info->io_thread->Stop();
    }
  }

  // 리스닝 소켓 닫기
  listen_fd_.reset();

  // 소켓 파일 삭제 (다음 시작 시 바인딩 실패 방지)
  if (!socket_path_.empty()) {
    unlink(socket_path_.c_str());
  }

  LOG(INFO) << "[MCP] Unix socket 전송 계층 중지됨";
}

void McpTransportSocket::Send(int target_client_id,
                               const std::string& json_message) {
  // Content-Length 기반 메시지 프레이밍
  // 형식: "Content-Length: N\r\n\r\n{json}"
  std::string framed =
      std::string(kContentLengthPrefix) +
      base::NumberToString(json_message.size()) +
      kCRLF + kCRLF +
      json_message;

  // 쓰기 실패한 클라이언트의 ClientInfo는 락 해제 후 소멸시켜야 한다.
  // (Thread::Stop()이 데드락 없이 실행되려면 락 외부여야 함)
  std::vector<std::unique_ptr<ClientInfo>> clients_to_remove;

  {
    base::AutoLock lock(write_lock_);

    if (clients_.empty()) {
      LOG(WARNING) << "[MCP] Send(): 연결된 클라이언트 없음";
      return;
    }

    // 쓰기 실패한 클라이언트 ID를 수집하여 나중에 제거한다.
    std::vector<int> failed_ids;

    // 특정 클라이언트 또는 전체 브로드캐스트 대상 결정
    for (auto& [cid, info] : clients_) {
      // target_client_id >= 0이면 해당 클라이언트만, -1이면 전체
      if (target_client_id >= 0 && cid != target_client_id) {
        continue;
      }

      int fd = info->fd.get();
      if (fd < 0) {
        LOG(WARNING) << "[MCP] Send(): 유효하지 않은 fd (client_id="
                     << cid << ")";
        failed_ids.push_back(cid);
        continue;
      }

      // macOS: SO_NOSIGPIPE로 write 시 SIGPIPE 방지
#if defined(__APPLE__)
      int nosigpipe = 1;
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

      // 소켓을 일시적으로 non-blocking으로 전환하여 UI 스레드 블로킹 방지.
      int flags = fcntl(fd, F_GETFL, 0);
      bool was_blocking = (flags != -1) && !(flags & O_NONBLOCK);
      if (was_blocking) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      }

      base::span<const char> remaining(framed);
      bool write_ok = true;
      while (!remaining.empty()) {
        ssize_t result =
            HANDLE_EINTR(write(fd, remaining.data(), remaining.size()));
        if (result < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
          }
          LOG(ERROR) << "[MCP] 소켓 쓰기 실패 (client_id=" << cid
                     << "): errno=" << errno;
          write_ok = false;
          break;
        }
        remaining = remaining.subspan(static_cast<size_t>(result));
      }

      // blocking 모드로 복구 (IO 스레드의 ReadLoop를 위해)
      if (was_blocking) {
        fcntl(fd, F_SETFL, flags);
      }

      if (!write_ok) {
        failed_ids.push_back(cid);
      }
    }

    // 쓰기 실패한 클라이언트를 맵에서 꺼낸다 (소유권 이전).
    for (int id : failed_ids) {
      auto it = clients_.find(id);
      if (it != clients_.end()) {
        clients_to_remove.push_back(std::move(it->second));
        clients_.erase(it);
      }
    }
  }  // write_lock_ 해제

  // 락 해제 후 ClientInfo 소멸 (Thread::Stop() 포함).
}

bool McpTransportSocket::IsConnected() const {
  // 하나 이상의 클라이언트가 연결되어 있으면 true
  base::AutoLock lock(write_lock_);
  return !clients_.empty();
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
  base::strlcpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path));

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

  // macOS: 클라이언트 소켓에 SO_NOSIGPIPE 설정
#if defined(__APPLE__)
  int nosigpipe = 1;
  setsockopt(client_fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe,
             sizeof(nosigpipe));
#endif

  // 새 클라이언트 ID 부여
  int client_id = next_client_id_++;

  LOG(INFO) << "[MCP] 클라이언트 연결 수락됨 (client_id=" << client_id
            << ", fd=" << client_fd << ")";

  // ClientInfo 생성
  auto info = std::make_unique<ClientInfo>();
  info->id = client_id;
  info->fd.reset(client_fd);

  // 클라이언트 전용 IO 스레드 생성 및 시작
  std::string thread_name = "MCP-reader-" + base::NumberToString(client_id);
  info->io_thread = std::make_unique<base::Thread>(thread_name);

  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  if (!info->io_thread->StartWithOptions(std::move(options))) {
    LOG(ERROR) << "[MCP] IO 스레드 시작 실패 (client_id=" << client_id << ")";
    return;
  }

  // IO 스레드에서 ReadLoop 시작.
  // fd 원시 값을 전달한다 (소유권은 ClientInfo가 유지).
  info->io_thread->task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&McpTransportSocket::ReadLoop,
                     base::Unretained(this),
                     client_id,
                     client_fd));

  // clients_ 맵에 등록 (write_lock_ 보호)
  {
    base::AutoLock lock(write_lock_);
    clients_[client_id] = std::move(info);
  }

  LOG(INFO) << "[MCP] 현재 연결된 클라이언트 수: " << clients_.size();
}

void McpTransportSocket::ReadLoop(int client_id, int fd) {
  // 클라이언트 전용 IO 스레드에서 실행된다.
  // 지정된 fd에서 Content-Length 프레이밍된 MCP 메시지를 읽는다.
  LOG(INFO) << "[MCP] 소켓 읽기 루프 시작 (client_id=" << client_id
            << ", fd=" << fd << ")";

  while (running_) {
    // --- 1단계: Content-Length 헤더 파싱 ---
    size_t content_length = 0;
    bool found_content_length = false;

    while (true) {
      std::string header_line;
      if (!ReadLine(fd, &header_line)) {
        LOG(INFO) << "[MCP] 소켓 헤더 읽기 실패 또는 EOF (client_id="
                  << client_id << ")";
        HandleClientDisconnect(client_id);
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
      LOG(WARNING) << "[MCP] Content-Length 헤더 없음, 건너뜀 (client_id="
                   << client_id << ")";
      continue;
    }

    if (content_length == 0 || content_length > kMaxMessageSize) {
      LOG(WARNING) << "[MCP] 비정상 Content-Length: " << content_length
                   << " (client_id=" << client_id << ")";
      HandleClientDisconnect(client_id);
      return;
    }

    // --- 2단계: JSON 바디 읽기 ---
    std::string body;
    if (!ReadExactBytes(fd, content_length, &body)) {
      LOG(ERROR) << "[MCP] 소켓 바디 읽기 실패 (client_id=" << client_id
                 << ")";
      HandleClientDisconnect(client_id);
      return;
    }

    // --- 3단계: UI 스레드로 메시지 포스팅 (client_id 태깅) ---
    PostMessageToUIThread(client_id, std::move(body));
  }

  LOG(INFO) << "[MCP] 소켓 읽기 루프 종료 (client_id=" << client_id << ")";
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
  base::span<char> remaining(base::as_writable_chars(base::span(*out)));

  while (!remaining.empty()) {
    ssize_t result =
        HANDLE_EINTR(read(fd, remaining.data(), remaining.size()));
    if (result == 0) {
      return false;  // EOF
    }
    if (result < 0) {
      LOG(ERROR) << "[MCP] 소켓 read() 실패: errno=" << errno;
      return false;
    }
    remaining = remaining.subspan(static_cast<size_t>(result));
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

void McpTransportSocket::PostMessageToUIThread(int client_id,
                                                std::string message) {
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](MessageCallback cb, int client_id, std::string msg) {
            if (cb) cb.Run(client_id, msg);
          },
          message_cb_, client_id, std::move(message)));
}

void McpTransportSocket::HandleClientDisconnect(int client_id) {
  // IO 스레드에서 호출. UI 스레드에 특정 클라이언트 연결 종료를 알린다.
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<McpTransportSocket> self,
             int client_id,
             DisconnectCallback disconnect_cb) {
            if (!self) {
              return;
            }

            // clients_ 맵에서 해당 클라이언트를 꺼낸다.
            // ClientInfo 소멸 시 Thread::Stop()이 호출되므로,
            // 락을 잡은 채로 소멸시키면 데드락이 발생할 수 있다.
            // (HandleClientDisconnect는 IO 스레드→UI 스레드 포스트이므로
            //  UI 스레드에서 실행되지만, Thread::Stop()은 내부적으로
            //  join을 수행하므로 이미 종료된 ReadLoop와 순서가 맞아야 함)
            // ReadLoop는 이미 return했으므로 Thread::Stop()은 빠르게 완료된다.
            std::unique_ptr<ClientInfo> removed_client;
            {
              base::AutoLock lock(self->write_lock_);
              auto it = self->clients_.find(client_id);
              if (it != self->clients_.end()) {
                removed_client = std::move(it->second);
                self->clients_.erase(it);
              }
            }
            // 락 해제 후 ClientInfo 소멸 (Thread::Stop() 포함)

            LOG(INFO) << "[MCP] 클라이언트 연결 종료됨 (client_id="
                      << client_id << "), 남은 클라이언트: "
                      << self->clients_.size();

            // 연결 종료 콜백 호출 (클라이언트 ID 전달)
            if (disconnect_cb) {
              disconnect_cb.Run(client_id);
            }
          },
          weak_factory_.GetWeakPtr(), client_id, disconnect_cb_));
}

}  // namespace mcp
