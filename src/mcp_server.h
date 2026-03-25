// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_MCP_SERVER_H_
#define CHROME_BROWSER_MCP_MCP_SERVER_H_

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "content/public/browser/web_contents_observer.h"

namespace content {
class BrowserContext;
class DevToolsAgentHost;
class WebContents;
}  // namespace content

namespace mcp {

class McpSession;
class McpToolRegistry;
class McpTransport;

// MCP 도구 핸들러 타입 정의.
// tools/call 요청 시 라우팅되어 호출되는 콜백.
// params: 도구 인자 (JSON 객체), callback: 결과 반환 콜백
using McpToolHandler =
    base::RepeatingCallback<void(const base::DictValue& params,
                                 base::OnceCallback<void(base::Value)> callback)>;

// MCP 도구 명세 구조체. tools/list 응답에 사용됨.
struct McpToolDefinition {
  McpToolDefinition();
  ~McpToolDefinition();
  McpToolDefinition(McpToolDefinition&&);
  McpToolDefinition& operator=(McpToolDefinition&&);
  std::string name;         // 도구 이름 (예: "navigate", "screenshot")
  std::string description;  // 도구 설명 (AI가 이해하는 자연어)
  base::DictValue input_schema;  // JSON Schema 형식의 입력 파라미터 정의
  McpToolHandler handler;   // 실제 실행 핸들러
};

// MCP 서버 전송 방식
enum class McpTransportMode {
  kStdio,   // stdin/stdout으로 JSON-RPC 통신
  kSocket,  // Unix domain socket으로 통신
};

// McpServer: Chromium 브라우저 프로세스 내장 MCP 서버 코어.
//
// 역할:
//   1. 브라우저 시작 시 --mcp-stdio / --mcp-socket 플래그 감지 후 초기화
//   2. McpTransport로부터 JSON-RPC 2.0 메시지 수신 및 라우팅
//   3. MCP 프로토콜 핸드셰이크 처리 (initialize → initialized)
//   4. tools/list, tools/call 요청 처리
//   5. 활성 탭(WebContents)에 대한 McpSession 생성/관리
//   6. DevToolsAgentHost를 통해 CDP 명령을 내부 IPC로 실행
//
// 싱글톤 패턴: 브라우저 프로세스당 하나의 인스턴스만 존재.
// BrowserThread::UI에서 동작.
class McpServer {
 public:
  // MCP 서버 초기화 설정
  struct Config {
    Config();
    ~Config();
    McpTransportMode transport_mode = McpTransportMode::kStdio;
    std::string socket_path;         // kSocket 모드에서 사용할 경로
    std::string auth_token;          // 빈 문자열이면 인증 비활성
    std::vector<std::string> allowed_tools;  // 비어있으면 전체 허용
    bool auto_attach_active_tab = true;  // 새 탭 활성화 시 자동 세션 연결
  };

  // 싱글톤 접근자
  static McpServer* GetInstance();

  McpServer();
  McpServer(const McpServer&) = delete;
  McpServer& operator=(const McpServer&) = delete;
  ~McpServer();

  // 편의 메서드: stdio 모드로 서버 시작
  void StartWithStdio();

  // 편의 메서드: Unix 소켓 모드로 서버 시작
  void StartWithSocket(const std::string& socket_path);

  // 2단계 초기화: config만 저장 (PostEarlyInitialization에서 호출)
  void PrepareConfig(std::unique_ptr<Config> config);

  // 2단계 초기화: transport 시작 (PostBrowserStart에서 호출)
  // PrepareConfig()로 설정이 저장된 경우에만 동작.
  void StartIfConfigured();

  // config가 준비되었는지 확인
  bool HasPendingConfig() const { return pending_config_ != nullptr; }

  // -----------------------------------------------------------------------
  // 초기화 / 종료
  // -----------------------------------------------------------------------

  // 커맨드라인 플래그 또는 환경변수를 파싱하여 MCP 활성화 여부 확인.
  // --mcp-stdio, --mcp-socket=PATH, CHROMIUM_MCP 환경변수를 검사.
  // 반환값: MCP 서버를 시작해야 하면 true
  static bool ShouldStart();

  // 설정을 기반으로 서버 초기화. BrowserThread::UI에서 호출 필요.
  // content::BrowserContext는 DevToolsAgentHost 생성에 사용됨.
  bool Initialize(std::unique_ptr<Config> config,
                  content::BrowserContext* browser_context);

  // 서버 종료. 모든 세션 닫고, transport 종료.
  void Shutdown();

  // 클라이언트 연결 해제 시 해당 클라이언트의 핸드셰이크만 리셋 (서버는 유지)
  void OnClientDisconnected(int client_id);

  // -----------------------------------------------------------------------
  // 탭(WebContents) 연결 관리
  // -----------------------------------------------------------------------

  // 지정한 WebContents에 MCP 세션을 연결.
  // DevToolsAgentHost를 통해 CDP 세션을 내부적으로 생성.
  // 반환값: 생성된 세션의 포인터 (소유권은 McpServer가 가짐)
  McpSession* AttachToWebContents(content::WebContents* web_contents);

  // WebContents에 연결된 세션 분리. 탭 닫힐 때 호출.
  void DetachFromWebContents(content::WebContents* web_contents);

  // 현재 활성 탭(포커스된 탭)에 연결된 세션 반환.
  // 연결된 세션이 없으면 nullptr 반환.
  McpSession* GetActiveSession();

  // 특정 클라이언트에 배정된 탭의 세션을 반환.
  // 배정된 탭이 없거나 세션이 없으면 자동 attach 시도.
  McpSession* GetSessionForClient(int client_id);

  // 특정 클라이언트에 탭을 배정한다.
  void AssignTabToClient(int client_id, content::WebContents* web_contents);

  // -----------------------------------------------------------------------
  // MCP 메시지 처리 (McpTransport 콜백)
  // -----------------------------------------------------------------------

  // transport로부터 raw JSON 문자열 수신 시 호출.
  // client_id: 메시지를 보낸 클라이언트 식별자.
  // JSON 파싱 후 HandleMessage()로 라우팅.
  void OnMessageReceived(int client_id, const std::string& json_message);

  // -----------------------------------------------------------------------
  // 도구 등록
  // -----------------------------------------------------------------------

  // MCP 도구를 서버에 등록. Initialize() 이후 도구 추가 가능.
  void RegisterTool(McpToolDefinition tool_def);

 private:
  // -----------------------------------------------------------------------
  // JSON-RPC 메시지 처리 내부 로직
  // -----------------------------------------------------------------------

  // 파싱된 JSON-RPC 메시지를 method에 따라 적절한 핸들러로 라우팅.
  void HandleMessage(int client_id, base::DictValue message);

  // MCP initialize 핸드셰이크 처리.
  // 클라이언트 정보를 저장하고 serverInfo + capabilities 응답 반환.
  void HandleInitialize(int client_id, const base::Value* id,
                        const base::DictValue* params);

  // notifications/initialized 수신 처리.
  // 이후 도구 호출을 받을 준비 완료 상태로 전환.
  void HandleInitialized(int client_id);

  // tools/list 요청 처리.
  // 등록된 모든 도구의 명세를 배열로 응답.
  void HandleToolsList(int client_id, const base::Value* id);

  // tools/call 요청 처리.
  // 요청된 도구 이름으로 핸들러를 찾아 실행.
  void HandleToolsCall(int client_id, const base::Value* id,
                       const base::DictValue* params);

  // 알 수 없는 method 수신 시 처리. 오류 응답 전송.
  void HandleUnknownMethod(int client_id, const base::Value* id,
                           const std::string& method);

  // -----------------------------------------------------------------------
  // JSON-RPC 응답 전송 헬퍼
  // -----------------------------------------------------------------------

  // 성공 응답 전송: {"jsonrpc":"2.0","id":...,"result":...}
  void SendResult(int client_id, const base::Value* id, base::Value result);

  // 오류 응답 전송: {"jsonrpc":"2.0","id":...,"error":{"code":...,"message":...}}
  void SendError(int client_id, const base::Value* id,
                 int code,
                 const std::string& message);

  // base::Value 딕셔너리를 JSON 직렬화하여 특정 클라이언트에게 전송.
  void SendMessage(int client_id, base::DictValue message);

  // -----------------------------------------------------------------------
  // 도구 핸들러 등록 (초기화 시 자동 등록)
  // -----------------------------------------------------------------------

  // 기본 제공 MCP 도구들을 모두 등록.
  // navigate, screenshot, page_content, evaluate, network_* 는 tool_registry_ 클래스 기반.
  // click, fill, browser_info 는 레거시 인라인 유지 (Task 5,6,10에서 처리 예정).
  void RegisterBuiltinTools();

  // 레거시 인라인 도구 등록 메서드 (click/fill/browser_info만 유지)
  void RegisterClickTool();
  void RegisterFillTool();
  void RegisterBrowserInfoTool();

  // -----------------------------------------------------------------------
  // 도구 실행 핸들러 (내부 CDP 호출) — 레거시 유지분
  // -----------------------------------------------------------------------

  // click 도구: CSS 선택자로 요소를 찾아 Input.dispatchMouseEvent 실행
  void ExecuteClick(const base::DictValue& params,
                    base::OnceCallback<void(base::Value)> callback);

  // fill 도구: 입력 필드에 값을 입력 (Runtime.evaluate 활용)
  void ExecuteFill(const base::DictValue& params,
                   base::OnceCallback<void(base::Value)> callback);

  // browser_info 도구: 브라우저 버전, 활성 탭 정보 반환
  void ExecuteBrowserInfo(const base::DictValue& params,
                          base::OnceCallback<void(base::Value)> callback);

  // -----------------------------------------------------------------------
  // MCP 응답 포맷 헬퍼
  // -----------------------------------------------------------------------

  // tools/call 성공 응답 포맷:
  // {"content": [{"type": "text", "text": "..."}]}
  static base::Value MakeTextResult(const std::string& text);

  // tools/call 오류 응답 포맷:
  // {"isError": true, "content": [{"type": "text", "text": "오류 메시지"}]}
  static base::Value MakeErrorResult(const std::string& error_message);

  // -----------------------------------------------------------------------
  // 상태 변수
  // -----------------------------------------------------------------------

  // MCP 프로토콜 핸드셰이크 상태
  enum class HandshakeState {
    kNotStarted,    // initialize 수신 전
    kInitializing,  // initialize 수신, initialized 대기 중
    kReady,         // 핸드셰이크 완료, 도구 호출 수신 가능
  };

  // 클라이언트별 연결 상태 정보
  struct ClientState {
    ClientState();
    ~ClientState();
    HandshakeState handshake_state = HandshakeState::kNotStarted;
    std::string client_name;
    std::string client_version;
    std::string protocol_version;
    // 이 클라이언트에 배정된 탭. CDP 명령이 이 탭으로 라우팅됨.
    raw_ptr<content::WebContents> assigned_tab = nullptr;
  };

  // client_id → ClientState 매핑
  std::map<int, ClientState> client_states_;

  // 초기화 설정
  std::unique_ptr<Config> config_;

  // 2단계 초기화용: PrepareConfig()에서 저장, StartIfConfigured()에서 소비
  std::unique_ptr<Config> pending_config_;

  // BrowserContext: DevToolsAgentHost 생성에 필요
  raw_ptr<content::BrowserContext> browser_context_ = nullptr;

  // 전송 계층 (stdio 또는 socket)
  std::unique_ptr<McpTransport> transport_;

  // WebContents → McpSession 매핑.
  // 각 탭에 연결된 CDP 세션을 관리.
  std::unordered_map<content::WebContents*, std::unique_ptr<McpSession>>
      sessions_;

  // 인라인 도구 실행 중 현재 클라이언트 ID (-1이면 미설정)
  // GetActiveSession()에서 이 값으로 GetSessionForClient() 호출
  int current_inline_client_id_ = -1;

  // 등록된 MCP 도구 목록 (name → definition) — 레거시 인라인 핸들러용
  std::unordered_map<std::string, McpToolDefinition> tools_;

  // McpTool 인터페이스 기반 도구 레지스트리 — 도구 클래스 기반 등록용
  std::unique_ptr<McpToolRegistry> tool_registry_;

  // UI 스레드 시퀀스 검사 (BrowserThread::UI 전용)
  SEQUENCE_CHECKER(sequence_checker_);

  // 약한 참조 팩토리 (비동기 콜백에서 this 댕글링 방지)
  base::WeakPtrFactory<McpServer> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_MCP_SERVER_H_
