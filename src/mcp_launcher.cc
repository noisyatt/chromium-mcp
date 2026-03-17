// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/mcp_launcher.h"

#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/posix/eintr_wrapper.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/values.h"

namespace mcp {

namespace {

// lock 파일이 위치하는 디렉토리 이름
constexpr char kLockDirName[] = ".chromium-mcp";

// lock 파일 이름
constexpr char kLockFileName[] = "instance.lock";

// lock 파일 JSON 필드 이름
constexpr char kFieldPid[] = "pid";
constexpr char kFieldSocket[] = "socket";
constexpr char kFieldStartedAt[] = "started_at";

// 소켓 연결 테스트 타임아웃 (마이크로초)
constexpr int kSocketConnectTimeoutUs = 500'000;  // 0.5초

}  // namespace

// ---------------------------------------------------------------------------
// 생성자 / 소멸자
// ---------------------------------------------------------------------------

McpLauncher::McpLauncher() = default;

McpLauncher::~McpLauncher() {
  // 소멸자에서도 안전하게 정리: Initialize() 이후 Shutdown() 을 빠트린 경우 대비
  if (initialized_) {
    Shutdown();
  }
}

// ---------------------------------------------------------------------------
// 초기화 / 종료
// ---------------------------------------------------------------------------

bool McpLauncher::Initialize(const std::string& socket_path) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  socket_path_ = socket_path;
  lock_file_path_ = GetDefaultLockFilePath();

  // lock 파일 디렉토리가 없으면 생성
  base::FilePath lock_dir = lock_file_path_.DirName();
  if (!EnsureLockDirectory(lock_dir)) {
    LOG(ERROR) << "[MCP 런처] lock 파일 디렉토리 생성 실패: " << lock_dir;
    return false;
  }

  // 기존 lock 파일이 있으면 확인 후 덮어씀
  // (이전 크래시 등으로 남은 오래된 lock 파일 처리)
  if (base::PathExists(lock_file_path_)) {
    int old_pid = 0;
    std::string old_socket;
    if (ReadLockFile(lock_file_path_, &old_pid, &old_socket)) {
      if (IsProcessAlive(old_pid) && IsSocketConnectable(old_socket)) {
        // 다른 인스턴스가 실제로 살아있음 — 중복 실행 방지
        LOG(WARNING) << "[MCP 런처] 이미 실행 중인 인스턴스 감지 (PID "
                     << old_pid << ", 소켓: " << old_socket << ")";
        // 중복 실행을 허용하지 않는 정책이라면 여기서 false 를 반환.
        // 현재 구현은 경고만 출력하고 lock 파일을 덮어씀 (다중 세션 허용).
      }
    }
    // 기존 lock 파일 삭제 후 새로 작성
    base::DeleteFile(lock_file_path_);
  }

  // lock 파일 작성
  if (!WriteLockFile()) {
    LOG(ERROR) << "[MCP 런처] lock 파일 작성 실패: " << lock_file_path_;
    return false;
  }

  initialized_ = true;
  VLOG(1) << "[MCP 런처] 초기화 완료. lock 파일: " << lock_file_path_
          << ", 소켓: " << socket_path_;
  return true;
}

void McpLauncher::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!initialized_) {
    return;
  }

  // lock 파일 삭제: 브라우저 종료를 다른 프로세스에 알림
  if (base::PathExists(lock_file_path_)) {
    // 현재 lock 파일의 PID 가 자신인지 재확인 후 삭제 (다른 인스턴스 파일 보호)
    int locked_pid = 0;
    std::string locked_socket;
    if (ReadLockFile(lock_file_path_, &locked_pid, &locked_socket)) {
      if (locked_pid == static_cast<int>(base::GetCurrentProcId())) {
        base::DeleteFile(lock_file_path_);
        VLOG(1) << "[MCP 런처] lock 파일 삭제 완료: " << lock_file_path_;
      } else {
        VLOG(1) << "[MCP 런처] lock 파일 PID 불일치, 삭제 건너뜀 (파일 PID: "
                << locked_pid << ", 현재 PID: " << base::GetCurrentProcId()
                << ")";
      }
    }
  }

  initialized_ = false;
}

// ---------------------------------------------------------------------------
// 기존 인스턴스 감지 (정적 메서드)
// ---------------------------------------------------------------------------

// static
bool McpLauncher::IsInstanceRunning() {
  base::FilePath lock_path = GetDefaultLockFilePath();

  // 1단계: lock 파일 존재 확인
  if (!base::PathExists(lock_path)) {
    return false;
  }

  // 2단계: lock 파일 파싱
  int pid = 0;
  std::string socket_path;
  if (!ReadLockFile(lock_path, &pid, &socket_path)) {
    VLOG(1) << "[MCP 런처] lock 파일 파싱 실패: " << lock_path;
    return false;
  }

  // 3단계: PID 살아있는지 확인
  if (!IsProcessAlive(pid)) {
    VLOG(1) << "[MCP 런처] 기존 인스턴스 PID " << pid << " 이미 종료됨";
    // 죽은 프로세스의 lock 파일 정리
    base::DeleteFile(lock_path);
    return false;
  }

  // 4단계: 소켓 연결 가능한지 확인
  if (!IsSocketConnectable(socket_path)) {
    VLOG(1) << "[MCP 런처] 소켓 연결 불가: " << socket_path;
    // 소켓이 없으면 브라우저가 아직 MCP 서버를 시작하지 않았거나 오류 상태
    return false;
  }

  VLOG(1) << "[MCP 런처] 실행 중인 인스턴스 확인 (PID " << pid
          << ", 소켓: " << socket_path << ")";
  return true;
}

// static
std::string McpLauncher::GetRunningInstanceSocketPath() {
  base::FilePath lock_path = GetDefaultLockFilePath();

  if (!base::PathExists(lock_path)) {
    return std::string();
  }

  int pid = 0;
  std::string socket_path;
  if (!ReadLockFile(lock_path, &pid, &socket_path)) {
    return std::string();
  }

  // PID 와 소켓 유효성 재확인
  if (!IsProcessAlive(pid) || !IsSocketConnectable(socket_path)) {
    return std::string();
  }

  return socket_path;
}

// ---------------------------------------------------------------------------
// lock 파일 경로
// ---------------------------------------------------------------------------

// static
base::FilePath McpLauncher::GetDefaultLockFilePath() {
  // HOME 디렉토리 조회
  base::FilePath home_dir;
  if (!base::PathService::Get(base::DIR_HOME, &home_dir)) {
    // HOME 을 찾지 못한 경우 /tmp 를 fallback 으로 사용
    home_dir = base::FilePath("/tmp");
    LOG(WARNING) << "[MCP 런처] HOME 디렉토리 조회 실패, /tmp 를 사용";
  }

  return home_dir.AppendASCII(kLockDirName).AppendASCII(kLockFileName);
}

// ---------------------------------------------------------------------------
// 내부 구현 (정적 메서드)
// ---------------------------------------------------------------------------

// static
bool McpLauncher::EnsureLockDirectory(const base::FilePath& lock_dir) {
  if (base::DirectoryExists(lock_dir)) {
    return true;
  }

  // 디렉토리 생성 (중간 경로도 함께 생성)
  if (!base::CreateDirectory(lock_dir)) {
    PLOG(ERROR) << "[MCP 런처] 디렉토리 생성 실패: " << lock_dir;
    return false;
  }

  // 디렉토리 권한을 0700 으로 설정 (소유자만 접근 가능)
  if (chmod(lock_dir.value().c_str(), 0700) != 0) {
    PLOG(WARNING) << "[MCP 런처] 디렉토리 권한 설정 실패: " << lock_dir;
    // 권한 설정 실패는 치명적이지 않으므로 계속 진행
  }

  return true;
}

// static
bool McpLauncher::ReadLockFile(const base::FilePath& lock_path,
                               int* out_pid,
                               std::string* out_socket_path) {
  DCHECK(out_pid);
  DCHECK(out_socket_path);

  // 파일 전체 내용 읽기
  std::string contents;
  if (!base::ReadFileToString(lock_path, &contents)) {
    VLOG(1) << "[MCP 런처] lock 파일 읽기 실패: " << lock_path;
    return false;
  }

  // JSON 파싱
  auto parsed = base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  if (!parsed) {
    VLOG(1) << "[MCP 런처] lock 파일 JSON 파싱 실패: " << lock_path;
    return false;
  }

  // PID 필드 추출
  const base::Value* pid_val = parsed->Find(kFieldPid);
  if (!pid_val || !pid_val->is_int()) {
    VLOG(1) << "[MCP 런처] lock 파일에 pid 필드 없음";
    return false;
  }
  *out_pid = pid_val->GetInt();

  // socket 필드 추출
  const std::string* socket_val = parsed->FindString(kFieldSocket);
  if (!socket_val || socket_val->empty()) {
    VLOG(1) << "[MCP 런처] lock 파일에 socket 필드 없음";
    return false;
  }
  *out_socket_path = *socket_val;

  return true;
}

// static
bool McpLauncher::IsProcessAlive(int pid) {
  if (pid <= 0) {
    return false;
  }

  // kill(pid, 0): 시그널을 보내지 않고 프로세스 존재 여부만 확인
  // 반환값 0: 프로세스 존재, -1: 없음 또는 권한 없음
  int result = kill(static_cast<pid_t>(pid), 0);
  if (result == 0) {
    return true;
  }

  // EPERM 은 프로세스가 존재하지만 시그널을 보낼 권한이 없음을 의미
  // (이 경우 프로세스는 살아있음)
  if (errno == EPERM) {
    return true;
  }

  // ESRCH: 프로세스가 없음
  return false;
}

// static
bool McpLauncher::IsSocketConnectable(const std::string& socket_path) {
  if (socket_path.empty()) {
    return false;
  }

  // Unix domain socket 생성
  int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    PLOG(WARNING) << "[MCP 런처] 테스트 소켓 생성 실패";
    return false;
  }

  // non-blocking 모드로 전환하여 타임아웃 구현
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = kSocketConnectTimeoutUs;
  setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  // 소켓 주소 설정
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;

  // 경로 길이 검사 (sun_path 크기 제한)
  if (socket_path.size() >= sizeof(addr.sun_path)) {
    LOG(WARNING) << "[MCP 런처] 소켓 경로가 너무 길다: " << socket_path;
    close(sock_fd);
    return false;
  }
  base::strlcpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path));

  // 연결 시도
  int ret = HANDLE_EINTR(connect(sock_fd,
                                 reinterpret_cast<struct sockaddr*>(&addr),
                                 sizeof(addr)));
  close(sock_fd);

  return ret == 0;
}

// ---------------------------------------------------------------------------
// 내부 구현 (인스턴스 메서드)
// ---------------------------------------------------------------------------

bool McpLauncher::WriteLockFile() {
  // 현재 프로세스 정보를 JSON 딕셔너리로 구성
  base::DictValue lock_data;
  lock_data.Set(kFieldPid, static_cast<int>(base::GetCurrentProcId()));
  lock_data.Set(kFieldSocket, socket_path_);

  // 시작 시각 (ISO 8601 형식 근사치: epoch 초 단위)
  // base::Time 은 RFC 3339 직렬화를 지원하지 않으므로 간단한 형식 사용
  lock_data.Set(kFieldStartedAt,
                base::NumberToString(
                    base::Time::Now().InMillisecondsSinceUnixEpoch() / 1000));

  // JSON 직렬화
  std::string json_content;
  if (!base::JSONWriter::WriteWithOptions(
          base::Value(std::move(lock_data)),
          base::JSONWriter::OPTIONS_PRETTY_PRINT,
          &json_content)) {
    LOG(ERROR) << "[MCP 런처] lock 파일 JSON 직렬화 실패";
    return false;
  }

  // 파일 쓰기
  if (!base::WriteFile(lock_file_path_, json_content)) {
    PLOG(ERROR) << "[MCP 런처] lock 파일 쓰기 실패: " << lock_file_path_;
    return false;
  }

  // 파일 권한을 0600 으로 설정 (소유자만 읽기/쓰기)
  if (chmod(lock_file_path_.value().c_str(), 0600) != 0) {
    PLOG(WARNING) << "[MCP 런처] lock 파일 권한 설정 실패";
    // 치명적이지 않으므로 계속 진행
  }

  return true;
}

}  // namespace mcp
