// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_MCP_LAUNCHER_H_
#define CHROME_BROWSER_MCP_MCP_LAUNCHER_H_

#include <string>

#include "base/files/file_path.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"

namespace mcp {

// McpLauncher: 브라우저 프로세스 내에서 인스턴스 잠금 파일을 관리한다.
//
// 역할:
//   - 브라우저가 --mcp-socket 모드로 시작될 때 lock 파일에 PID 와 소켓 경로를 기록
//   - 다른 프로세스(chromium-mcp-launcher.py 등)가 실행 중인 인스턴스를 감지할 수 있게 함
//   - 브라우저 정상/비정상 종료 시 lock 파일 삭제
//   - 기존 인스턴스 유효성 검사: lock 파일 존재 + PID 살아있음 + 소켓 연결 가능
//
// lock 파일 형식 (~/.chromium-mcp/instance.lock):
//   {
//     "pid": 12345,
//     "socket": "/tmp/.chromium-mcp.sock",
//     "started_at": "2024-01-01T00:00:00Z"
//   }
//
// 사용 예:
//   McpLauncher launcher;
//   if (launcher.Initialize("/tmp/.chromium-mcp.sock")) {
//     // 브라우저 동작 중 ...
//     launcher.Shutdown();
//   }
class McpLauncher {
 public:
  McpLauncher();
  McpLauncher(const McpLauncher&) = delete;
  McpLauncher& operator=(const McpLauncher&) = delete;
  ~McpLauncher();

  // -------------------------------------------------------------------------
  // 초기화 / 종료
  // -------------------------------------------------------------------------

  // socket_path: 현재 브라우저가 열어둔 Unix domain socket 경로.
  // lock 파일에 이 경로와 현재 PID 를 기록한다.
  // 반환값: lock 파일 생성 성공 시 true
  bool Initialize(const std::string& socket_path);

  // lock 파일을 삭제한다. 브라우저 종료 시 반드시 호출해야 한다.
  void Shutdown();

  // -------------------------------------------------------------------------
  // 기존 인스턴스 감지
  // -------------------------------------------------------------------------

  // 기존 Chromium-MCP 인스턴스가 실행 중인지 확인한다.
  // 확인 조건: lock 파일 존재 + PID 살아있음 + 소켓 연결 가능
  // 반환값: 유효한 기존 인스턴스가 있으면 true
  static bool IsInstanceRunning();

  // 실행 중인 인스턴스의 소켓 경로를 반환한다.
  // IsInstanceRunning() 이 true 일 때만 유효한 값을 반환한다.
  // 반환값: 소켓 경로 문자열, 없으면 빈 문자열
  static std::string GetRunningInstanceSocketPath();

  // -------------------------------------------------------------------------
  // lock 파일 경로
  // -------------------------------------------------------------------------

  // lock 파일의 기본 경로를 반환한다: ~/.chromium-mcp/instance.lock
  static base::FilePath GetDefaultLockFilePath();

 private:
  // -------------------------------------------------------------------------
  // 내부 구현
  // -------------------------------------------------------------------------

  // lock 파일 디렉토리가 존재하지 않으면 생성한다.
  // 반환값: 디렉토리 준비 완료 시 true
  static bool EnsureLockDirectory(const base::FilePath& lock_dir);

  // lock 파일을 읽어 PID 와 소켓 경로를 파싱한다.
  // 반환값: 파싱 성공 시 true
  static bool ReadLockFile(const base::FilePath& lock_path,
                           int* out_pid,
                           std::string* out_socket_path);

  // PID 가 현재 살아있는 프로세스인지 확인한다.
  // kill(pid, 0) 을 사용하여 시그널 없이 존재 여부만 검사한다.
  static bool IsProcessAlive(int pid);

  // 주어진 경로의 Unix domain socket 에 연결 가능한지 테스트한다.
  // 실제로 연결을 시도하고 즉시 닫는다 (연결 테스트만).
  static bool IsSocketConnectable(const std::string& socket_path);

  // lock 파일에 현재 상태를 JSON 형식으로 기록한다.
  bool WriteLockFile();

  // 현재 기록된 lock 파일 경로
  base::FilePath lock_file_path_;

  // 이 인스턴스가 관리하는 소켓 경로
  std::string socket_path_;

  // Initialize() 가 성공적으로 완료되었는지 여부
  bool initialized_ = false;

  // 시퀀스 검사 (BrowserThread::UI 전용)
  SEQUENCE_CHECKER(sequence_checker_);

  // 약한 참조 팩토리 (비동기 콜백에서 this 댕글링 방지)
  base::WeakPtrFactory<McpLauncher> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_MCP_LAUNCHER_H_
