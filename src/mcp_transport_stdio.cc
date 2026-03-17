// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/mcp_transport_stdio.h"

#include <stdio.h>

#include <string>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"

namespace mcp {

namespace {

// MCP Content-Length 헤더 접두사
constexpr char kContentLengthPrefix[] = "Content-Length: ";
// CRLF 빈 줄 (헤더와 바디 구분자)
constexpr char kCRLF[] = "\r\n";
// 단일 메시지 최대 크기: 64MB (이상이면 비정상 메시지로 간주)
constexpr size_t kMaxMessageSize = 64 * 1024 * 1024;

}  // namespace

McpTransportStdio::McpTransportStdio()
    : io_thread_("MCP-stdio-reader") {}

McpTransportStdio::~McpTransportStdio() {
  Stop();
}

void McpTransportStdio::Start(MessageCallback message_cb,
                              DisconnectCallback disconnect_cb) {
  DCHECK(!running_.load()) << "Start()를 중복 호출함";

  message_cb_ = std::move(message_cb);
  disconnect_cb_ = std::move(disconnect_cb);

  // Start()를 호출한 스레드(보통 UI 스레드)의 태스크 러너를 저장한다.
  // 수신 메시지를 이 태스크 러너로 포스팅하게 된다.
  ui_task_runner_ = base::SequencedTaskRunner::GetCurrentDefault();

  running_.store(true);

  // stdin 읽기 전용 스레드를 시작한다.
  // base::Thread는 내부적으로 MessageLoop를 가지므로
  // PostTask로 ReadLoop를 실행시킬 수 있다.
  base::Thread::Options options;
  options.message_pump_type = base::MessagePumpType::IO;
  io_thread_.StartWithOptions(std::move(options));

  io_thread_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&McpTransportStdio::ReadLoop, base::Unretained(this)));

  LOG(INFO) << "[MCP] stdio 전송 계층 시작됨";
}

void McpTransportStdio::Stop() {
  bool was_running = running_.exchange(false);
  if (!was_running) {
    return;
  }

  // io_thread_ 중지. stdin에서 블로킹 중인 ReadLoop는
  // running_ 플래그가 false가 되면 다음 루프에서 자연스럽게 종료된다.
  // 단, 현재 read() 시스템 콜이 블로킹 중이면 스레드 중지가 지연될 수 있다.
  io_thread_.Stop();

  LOG(INFO) << "[MCP] stdio 전송 계층 중지됨";
}

void McpTransportStdio::Send(const std::string& json_message) {
  if (!running_.load()) {
    LOG(WARNING) << "[MCP] Send(): 전송 계층이 이미 중지됨";
    return;
  }

  // Content-Length 기반 메시지 프레이밍.
  // 형식: "Content-Length: N\r\n\r\n{json}"
  std::string framed =
      std::string(kContentLengthPrefix) +
      base::NumberToString(json_message.size()) +
      kCRLF + kCRLF +
      json_message;

  // stdout 동시 쓰기는 락으로 직렬화한다.
  // (여러 도구의 비동기 응답이 동시에 Send()를 호출할 수 있음)
  base::AutoLock guard(write_lock_);

  size_t written = 0;
  const char* data = framed.data();
  const size_t total = framed.size();

  // 부분 쓰기(partial write)를 처리하기 위해 반복 쓰기
  while (written < total) {
    ssize_t result = write(STDOUT_FILENO, data + written, total - written);
    if (result < 0) {
      if (errno == EINTR) {
        continue;  // 시그널에 의한 중단 → 재시도
      }
      LOG(ERROR) << "[MCP] stdout 쓰기 실패: errno=" << errno;
      return;
    }
    written += static_cast<size_t>(result);
  }
}

bool McpTransportStdio::IsConnected() const {
  return running_.load();
}

void McpTransportStdio::ReadLoop() {
  // 이 함수는 io_thread_ 에서 실행된다.
  LOG(INFO) << "[MCP] stdin 읽기 루프 시작";

  while (running_.load()) {
    // --- 1단계: Content-Length 헤더 파싱 ---
    //
    // MCP 메시지 헤더 형식:
    //   Content-Length: N\r\n
    //   \r\n          (빈 줄: 헤더 종료 표시)
    //
    // 여러 헤더가 올 수 있으나 현재 MCP 스펙은 Content-Length만 사용한다.
    size_t content_length = 0;
    bool found_content_length = false;

    while (true) {
      std::string header_line;
      if (!ReadLine(&header_line)) {
        // EOF 또는 읽기 에러 → 연결 종료
        LOG(INFO) << "[MCP] stdin EOF 또는 에러, 읽기 루프 종료";
        running_.store(false);
        // UI 스레드에 연결 종료를 알린다.
        ui_task_runner_->PostTask(
            FROM_HERE,
            base::BindOnce(
                [](DisconnectCallback cb) {
                  if (cb) std::move(cb).Run();
                },
                disconnect_cb_));
        return;
      }

      if (header_line.empty()) {
        // 빈 줄: 헤더 섹션 종료
        break;
      }

      size_t length = 0;
      if (ParseContentLengthHeader(header_line, &length)) {
        content_length = length;
        found_content_length = true;
      }
      // 알 수 없는 헤더는 무시한다 (향후 확장 고려)
    }

    if (!found_content_length) {
      LOG(WARNING) << "[MCP] Content-Length 헤더 없음, 메시지 건너뜀";
      continue;
    }

    if (content_length == 0) {
      LOG(WARNING) << "[MCP] Content-Length: 0, 빈 메시지 건너뜀";
      continue;
    }

    if (content_length > kMaxMessageSize) {
      LOG(ERROR) << "[MCP] Content-Length " << content_length
                 << " 초과 (최대 " << kMaxMessageSize << "), 연결 종료";
      running_.store(false);
      return;
    }

    // --- 2단계: JSON 바디 읽기 ---
    std::string body;
    if (!ReadExactBytes(content_length, &body)) {
      LOG(ERROR) << "[MCP] JSON 바디 읽기 실패 (요청 길이=" << content_length
                 << ")";
      running_.store(false);
      ui_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](DisconnectCallback cb) {
                if (cb) std::move(cb).Run();
              },
              disconnect_cb_));
      return;
    }

    // --- 3단계: UI 스레드로 메시지 포스팅 ---
    PostMessageToUIThread(std::move(body));
  }

  LOG(INFO) << "[MCP] stdin 읽기 루프 종료";
}

bool McpTransportStdio::ParseContentLengthHeader(
    const std::string& header_line,
    size_t* out_length) const {
  // "Content-Length: N" 형식 파싱
  if (!base::StartsWith(header_line, kContentLengthPrefix,
                        base::CompareCase::INSENSITIVE_ASCII)) {
    return false;
  }

  std::string value_str =
      header_line.substr(std::string(kContentLengthPrefix).size());

  // 앞뒤 공백 제거
  base::TrimWhitespaceASCII(value_str, base::TRIM_ALL, &value_str);

  size_t length = 0;
  if (!base::StringToSizeT(value_str, &length)) {
    LOG(WARNING) << "[MCP] Content-Length 값 파싱 실패: '" << value_str << "'";
    return false;
  }

  *out_length = length;
  return true;
}

bool McpTransportStdio::ReadExactBytes(size_t n, std::string* out) const {
  out->resize(n);
  size_t total_read = 0;
  char* buf = &(*out)[0];

  while (total_read < n) {
    ssize_t result = read(STDIN_FILENO, buf + total_read, n - total_read);
    if (result == 0) {
      // EOF
      return false;
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;  // 시그널 중단 → 재시도
      }
      LOG(ERROR) << "[MCP] stdin 읽기 실패: errno=" << errno;
      return false;
    }
    total_read += static_cast<size_t>(result);
  }

  return true;
}

bool McpTransportStdio::ReadLine(std::string* out) const {
  out->clear();

  // 한 바이트씩 읽어 줄바꿈 문자를 찾는다.
  // 성능보다 정확성을 우선하는 구현 (메시지 헤더는 짧으므로 충분)
  while (true) {
    char c;
    ssize_t result = read(STDIN_FILENO, &c, 1);
    if (result == 0) {
      // EOF: 읽은 내용이 있으면 true, 없으면 false
      return !out->empty();
    }
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }

    if (c == '\n') {
      // \r\n 처리: 마지막에 붙은 \r 제거
      if (!out->empty() && out->back() == '\r') {
        out->pop_back();
      }
      return true;
    }

    out->push_back(c);
  }
}

void McpTransportStdio::PostMessageToUIThread(std::string message) {
  // io_thread_에서 호출되어 UI 스레드로 메시지를 전달한다.
  ui_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](MessageCallback cb, std::string msg) {
            if (cb) cb.Run(msg);
          },
          message_cb_, std::move(message)));
}

}  // namespace mcp
