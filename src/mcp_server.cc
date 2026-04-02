// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/mcp_server.h"

#include <utility>

#include "base/command_line.h"
#include "base/environment.h"
#include "base/no_destructor.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "chrome/browser/mcp/mcp_transport_stdio.h"
#include "chrome/browser/mcp/mcp_transport_socket.h"
#include "chrome/browser/mcp/tools/bookmark_tool.h"
#include "chrome/browser/mcp/tools/browser_info_tool.h"
#include "chrome/browser/mcp/tools/clipboard_tool.h"
#include "chrome/browser/mcp/tools/console_tool.h"
#include "chrome/browser/mcp/tools/cookie_tool.h"
#include "chrome/browser/mcp/tools/coverage_tool.h"
#include "chrome/browser/mcp/tools/dialog_tool.h"
#include "chrome/browser/mcp/tools/click_tool.h"
#include "chrome/browser/mcp/tools/fill_tool.h"
#include "chrome/browser/mcp/tools/download_tool.h"
#include "chrome/browser/mcp/tools/drag_tool.h"
#include "chrome/browser/mcp/tools/element_tool.h"
#include "chrome/browser/mcp/tools/element_info_tool.h"
#include "chrome/browser/mcp/tools/emulation_tool.h"
#include "chrome/browser/mcp/tools/file_upload_tool.h"
#include "chrome/browser/mcp/tools/find_tool.h"
#include "chrome/browser/mcp/tools/history_tool.h"
#include "chrome/browser/mcp/tools/hover_tool.h"
#include "chrome/browser/mcp/tools/javascript_tool.h"
#include "chrome/browser/mcp/tools/keyboard_tool.h"
#include "chrome/browser/mcp/tools/mouse_tool.h"
#include "chrome/browser/mcp/tools/navigate_tool.h"
#include "chrome/browser/mcp/tools/network_intercept_tool.h"
#include "chrome/browser/mcp/tools/network_tool.h"
#include "chrome/browser/mcp/tools/page_content_tool.h"
#include "chrome/browser/mcp/tools/pdf_tool.h"
#include "chrome/browser/mcp/tools/performance_tool.h"
#include "chrome/browser/mcp/tools/screenshot_tool.h"
#include "chrome/browser/mcp/tools/scroll_tool.h"
#include "chrome/browser/mcp/tools/select_option_tool.h"
#include "chrome/browser/mcp/tools/storage_tool.h"
#include "chrome/browser/mcp/tools/tab_tool.h"
#include "chrome/browser/mcp/tools/wait_tool.h"
#include "chrome/browser/mcp/tools/window_tool.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"

#include <signal.h>

// MCP 커맨드라인 플래그 정의
// 실제 배포 시 난독화된 이름으로 교체 가능
// 커맨드라인 스위치는 chrome_main_delegate.cc에서 처리.
// ShouldStart()는 기본 활성화이므로 여기서는 사용하지 않음.
constexpr char kMcpEnvVar[] = "CHROMIUM_MCP";

// MCP 프로토콜 버전 (2024-11-05 스펙 준수)
constexpr char kMcpProtocolVersion[] = "2024-11-05";
constexpr char kMcpServerName[] = "chromium-mcp";
constexpr char kMcpServerVersion[] = "1.0.0";

// JSON-RPC 오류 코드 (MCP 스펙)
constexpr int kJsonRpcParseError = -32700;
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcMethodNotFound = -32601;
constexpr int kJsonRpcInvalidParams = -32602;


namespace mcp {

namespace {
}  // namespace

McpServer::Config::Config() = default;
McpServer::Config::~Config() = default;

McpServer::ClientState::ClientState() = default;
McpServer::ClientState::~ClientState() = default;

McpServer::McpServer() = default;
McpServer::~McpServer() = default;

// static
McpServer* McpServer::GetInstance() {
  static base::NoDestructor<McpServer> instance;
  return instance.get();
}

void McpServer::StartWithStdio() {
  auto config = std::make_unique<Config>();
  config->transport_mode = McpTransportMode::kStdio;
  Initialize(std::move(config), nullptr);
}

void McpServer::StartWithSocket(const std::string& socket_path) {
  auto config = std::make_unique<Config>();
  config->transport_mode = McpTransportMode::kSocket;
  config->socket_path = socket_path;
  Initialize(std::move(config), nullptr);
}

void McpServer::PrepareConfig(std::unique_ptr<Config> config) {
  pending_config_ = std::move(config);
  LOG(INFO) << "[MCP] config 저장 완료. PostBrowserStart에서 서버 시작 예정.";
}

void McpServer::StartIfConfigured() {
  if (!pending_config_) {
    return;
  }
  LOG(INFO) << "[MCP] 지연 초기화 시작 (PostBrowserStart)";
  Initialize(std::move(pending_config_), nullptr);
}

// static
bool McpServer::ShouldStart() {
  // CHROMIUM_MCP=0 으로 명시적 비활성화하지 않는 한 항상 활성화.
  // 기본 브라우저로 사용해도 MCP 서버가 상시 동작한다.
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  auto env_value = env->GetVar(kMcpEnvVar);
  if (env_value.has_value() && *env_value == "0") {
    LOG(INFO) << "[MCP] 환경변수로 MCP 서버 비활성화";
    return false;
  }

  return true;
}

bool McpServer::Initialize(std::unique_ptr<Config> config,
                           content::BrowserContext* browser_context) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // browser_context는 nullptr 허용 — PostEarlyInitialization 시점에는
  // 아직 Profile이 생성되지 않았으므로, 이후 AttachToWebContents()에서
  // WebContents를 통해 BrowserContext를 획득한다.

  // SIGPIPE 무시: 소켓 write 시 상대방 연결 끊김으로 인한 프로세스 종료 방지
  signal(SIGPIPE, SIG_IGN);

  config_ = std::move(config);
  browser_context_ = browser_context;

  LOG(INFO) << "[MCP] 서버 초기화 시작. 전송 모드: "
            << (config_->transport_mode == McpTransportMode::kStdio
                    ? "stdio"
                    : "socket");

  // 전송 계층 생성
  if (config_->transport_mode == McpTransportMode::kStdio) {
    transport_ = std::make_unique<McpTransportStdio>();
  } else {
    transport_ = std::make_unique<McpTransportSocket>(config_->socket_path);
  }

  // 전송 계층 시작: 메시지 수신 및 연결 해제 콜백 등록
  // 클라이언트 연결 해제 시 서버를 종료하지 않고 핸드셰이크만 리셋하여
  // 새 클라이언트 연결을 기다린다.
  transport_->Start(
      base::BindRepeating(&McpServer::OnMessageReceived,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&McpServer::OnClientDisconnected,
                          weak_factory_.GetWeakPtr()));

  // 기본 내장 도구 등록 (navigate, screenshot 등)
  RegisterBuiltinTools();

  size_t tool_count = tool_registry_ ? tool_registry_->GetToolCount() : 0;
  LOG(INFO) << "[MCP] 서버 초기화 완료. " << tool_count << "개 도구 등록됨";
  return true;
}

void McpServer::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(INFO) << "[MCP] 서버 종료 시작";

  // 모든 클라이언트 상태 정리
  client_states_.clear();

  // 모든 세션 종료 (DevToolsAgentHost 연결 해제)
  sessions_.clear();

  // 전송 계층 종료
  if (transport_) {
    // transport_->Stop();
    transport_.reset();
  }

  LOG(INFO) << "[MCP] 서버 종료 완료";
}

void McpServer::OnClientDisconnected(int client_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(INFO) << "[MCP] 클라이언트 연결 해제됨 (client_id=" << client_id
            << "). 해당 클라이언트 상태 제거.";

  // 해당 클라이언트의 상태만 제거한다.
  // 다른 클라이언트는 영향 없이 계속 동작한다.
  client_states_.erase(client_id);
}

// -----------------------------------------------------------------------
// 탭(WebContents) 연결 관리
// -----------------------------------------------------------------------

McpSession* McpServer::AttachToWebContents(
    content::WebContents* web_contents) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(web_contents);

  // 초기화 시 browser_context가 없었으면 여기서 획득
  if (!browser_context_) {
    browser_context_ = web_contents->GetBrowserContext();
  }

  // 이미 세션이 존재하면 기존 세션 반환
  auto it = sessions_.find(web_contents);
  if (it != sessions_.end()) {
    LOG(INFO) << "[MCP] WebContents에 이미 세션 존재. 기존 세션 반환.";
    return it->second.get();
  }

  // DevToolsAgentHost를 통해 CDP 내부 세션 생성.
  // GetOrCreateFor(): 탭에 대한 에이전트 호스트를 가져오거나 새로 생성.
  // 이 호출은 네트워크 포트를 열지 않으며 내부 IPC만 사용.
  scoped_refptr<content::DevToolsAgentHost> agent_host =
      content::DevToolsAgentHost::GetOrCreateFor(web_contents);

  if (!agent_host) {
    LOG(ERROR) << "[MCP] DevToolsAgentHost 생성 실패";
    return nullptr;
  }

  // McpSession 생성: DevToolsAgentHostClient 인터페이스 구현체
  // CDP 이벤트는 모든 클라이언트에게 브로드캐스트 (client_id=-1)
  auto session = std::make_unique<McpSession>(
      agent_host,
      base::BindRepeating(&McpServer::SendMessage,
                          weak_factory_.GetWeakPtr(),
                          /*client_id=*/-1));

  // 세션을 통해 CDP 연결 활성화
  if (!session->Attach()) {
    LOG(ERROR) << "[MCP] CDP 세션 연결 실패";
    return nullptr;
  }

  McpSession* session_ptr = session.get();
  sessions_[web_contents] = std::move(session);

  LOG(INFO) << "[MCP] WebContents에 세션 연결 완료";
  return session_ptr;
}

void McpServer::DetachFromWebContents(content::WebContents* web_contents) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = sessions_.find(web_contents);
  if (it == sessions_.end()) {
    return;
  }

  // 세션 정리 (소멸자에서 DevToolsAgentHost 연결 해제)
  sessions_.erase(it);

  // 이 탭을 assigned_tab으로 가진 모든 클라이언트의 배정 해제
  for (auto& [id, state] : client_states_) {
    if (state.assigned_tab == web_contents) {
      state.assigned_tab = nullptr;
      LOG(INFO) << "[MCP] 탭 닫힘으로 client_id=" << id
                << "의 assigned_tab 해제";
    }
  }

  LOG(INFO) << "[MCP] WebContents에서 세션 분리 완료";
}

McpSession* McpServer::GetActiveSession() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // 첫 번째 배정된 클라이언트의 탭 반환
  for (const auto& [id, state] : client_states_) {
    if (state.assigned_tab) {
      auto it = sessions_.find(state.assigned_tab);
      if (it != sessions_.end()) {
        return it->second.get();
      }
    }
  }
  return nullptr;
}

McpSession* McpServer::GetSessionForClient(int client_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto state_it = client_states_.find(client_id);
  if (state_it == client_states_.end()) {
    return nullptr;
  }

  content::WebContents* wc = state_it->second.assigned_tab;
  if (!wc) {
    // 배정된 탭이 없으면 현재 활성 탭에 자동 배정
    Browser* browser = chrome::FindLastActive();
    if (!browser || !browser->tab_strip_model()) {
      return nullptr;
    }
    wc = browser->tab_strip_model()->GetActiveWebContents();
    if (!wc) {
      return nullptr;
    }
    state_it->second.assigned_tab = wc;
    LOG(INFO) << "[MCP] client_id=" << client_id
              << "에 활성 탭 자동 배정: " << wc->GetVisibleURL().spec();
  }

  auto session_it = sessions_.find(wc);
  if (session_it == sessions_.end()) {
    // 세션이 없으면 자동 attach
    return AttachToWebContents(wc);
  }
  return session_it->second.get();
}

void McpServer::AssignTabToClient(int client_id,
                                   content::WebContents* web_contents) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = client_states_.find(client_id);
  if (it == client_states_.end()) {
    return;
  }

  it->second.assigned_tab = web_contents;
  LOG(INFO) << "[MCP] client_id=" << client_id << "에 탭 배정";

  // 새 탭에 세션이 없으면 자동 attach
  if (web_contents && sessions_.find(web_contents) == sessions_.end()) {
    AttachToWebContents(web_contents);
  }
}

// -----------------------------------------------------------------------
// JSON-RPC 메시지 수신 및 라우팅
// -----------------------------------------------------------------------

void McpServer::OnMessageReceived(int client_id,
                                   const std::string& json_message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // JSON 파싱
  auto parsed = base::JSONReader::ReadAndReturnValueWithError(json_message, base::JSON_PARSE_RFC);
  if (!parsed.has_value()) {
    LOG(WARNING) << "[MCP] JSON 파싱 실패 (client_id=" << client_id
                 << "): " << parsed.error().message;
    SendError(client_id, nullptr, kJsonRpcParseError,
              "Parse error: " + parsed.error().message);
    return;
  }

  if (!parsed->is_dict()) {
    LOG(WARNING) << "[MCP] JSON-RPC 메시지가 객체 형식이 아님 (client_id="
                 << client_id << ")";
    SendError(client_id, nullptr, kJsonRpcInvalidRequest,
              "Request must be a JSON object");
    return;
  }

  HandleMessage(client_id, std::move(parsed->GetDict()));
}

void McpServer::HandleMessage(int client_id, base::DictValue message) {
  // jsonrpc 버전 검증
  const std::string* jsonrpc = message.FindString("jsonrpc");
  if (!jsonrpc || *jsonrpc != "2.0") {
    LOG(WARNING) << "[MCP] jsonrpc 버전이 2.0이 아님 (client_id="
                 << client_id << ")";
    const base::Value* id = message.Find("id");
    SendError(client_id, id, kJsonRpcInvalidRequest,
              "jsonrpc must be \"2.0\"");
    return;
  }

  // method 추출
  const std::string* method = message.FindString("method");
  if (!method) {
    LOG(WARNING) << "[MCP] method 필드 없음 (client_id=" << client_id << ")";
    const base::Value* id = message.Find("id");
    SendError(client_id, id, kJsonRpcInvalidRequest, "Missing method field");
    return;
  }

  // id 추출 (알림 메시지는 id 없음)
  const base::Value* id = message.Find("id");
  const base::DictValue* params = message.FindDict("params");

  LOG(INFO) << "[MCP] 수신된 method: " << *method
            << " (client_id=" << client_id << ")";

  // method에 따라 핸들러 라우팅
  if (*method == "initialize") {
    HandleInitialize(client_id, id, params);
  } else if (*method == "notifications/initialized") {
    HandleInitialized(client_id);
  } else if (*method == "tools/list") {
    HandleToolsList(client_id, id);
  } else if (*method == "tools/call") {
    HandleToolsCall(client_id, id, params);
  } else {
    HandleUnknownMethod(client_id, id, *method);
  }
}

// -----------------------------------------------------------------------
// MCP 프로토콜 핸드셰이크
// -----------------------------------------------------------------------

void McpServer::HandleInitialize(int client_id, const base::Value* id,
                                  const base::DictValue* params) {
  // 해당 클라이언트의 상태 가져오기 (없으면 새로 생성)
  auto& state = client_states_[client_id];

  // 이미 초기화된 경우 재초기화 무시
  if (state.handshake_state != HandshakeState::kNotStarted) {
    LOG(WARNING) << "[MCP] 이미 초기화됨 (client_id=" << client_id
                 << "). initialize 요청 무시.";
    SendError(client_id, id, kJsonRpcInvalidRequest, "Already initialized");
    return;
  }

  // 클라이언트 정보 저장
  if (params) {
    if (const std::string* ver = params->FindString("protocolVersion")) {
      state.protocol_version = *ver;
    }
    if (const base::DictValue* client_info = params->FindDict("clientInfo")) {
      if (const std::string* name = client_info->FindString("name")) {
        state.client_name = *name;
      }
      if (const std::string* version = client_info->FindString("version")) {
        state.client_version = *version;
      }
    }
  }

  LOG(INFO) << "[MCP] 클라이언트 연결 (client_id=" << client_id << "): "
            << state.client_name << " v" << state.client_version
            << " (프로토콜: " << state.protocol_version << ")";

  state.handshake_state = HandshakeState::kInitializing;

  // MCP initialize 응답 구성.
  // serverInfo: 이 서버의 이름과 버전
  // capabilities.tools: 도구 호출 기능 지원 선언
  base::DictValue server_info;
  server_info.Set("name", kMcpServerName);
  server_info.Set("version", kMcpServerVersion);

  // capabilities: 서버가 지원하는 MCP 기능 목록
  // tools: 빈 객체({})는 tools/list, tools/call 지원을 의미
  base::DictValue capabilities;
  capabilities.Set("tools", base::DictValue());

  base::DictValue result;
  result.Set("protocolVersion", kMcpProtocolVersion);
  result.Set("serverInfo", std::move(server_info));
  result.Set("capabilities", std::move(capabilities));

  SendResult(client_id, id, base::Value(std::move(result)));
}

void McpServer::HandleInitialized(int client_id) {
  // notifications/initialized는 응답이 없는 알림(notification)
  auto it = client_states_.find(client_id);
  if (it == client_states_.end() ||
      it->second.handshake_state != HandshakeState::kInitializing) {
    LOG(WARNING) << "[MCP] 잘못된 상태에서 initialized 수신 (client_id="
                 << client_id << ")";
    return;
  }

  it->second.handshake_state = HandshakeState::kReady;
  LOG(INFO) << "[MCP] 핸드셰이크 완료 (client_id=" << client_id
            << "). 도구 호출 대기 중.";

  // 활성 탭이 있으면 이 클라이언트에 자동 배정
  if (config_ && config_->auto_attach_active_tab) {
    Browser* browser = chrome::FindLastActive();
    if (browser && browser->tab_strip_model()) {
      content::WebContents* wc =
          browser->tab_strip_model()->GetActiveWebContents();
      if (wc) {
        it->second.assigned_tab = wc;
        // 세션이 없으면 자동 attach
        if (sessions_.find(wc) == sessions_.end()) {
          AttachToWebContents(wc);
        }
        LOG(INFO) << "[MCP] client_id=" << client_id
                  << "에 활성 탭 배정: " << wc->GetVisibleURL().spec();
      }
    }
  }
}

// -----------------------------------------------------------------------
// tools/list 처리
// -----------------------------------------------------------------------

void McpServer::HandleToolsList(int client_id, const base::Value* id) {
  auto it = client_states_.find(client_id);
  if (it == client_states_.end() ||
      it->second.handshake_state != HandshakeState::kReady) {
    SendError(client_id, id, kJsonRpcInvalidRequest,
              "Server not ready. Call initialize first.");
    return;
  }

  // tool_registry_->BuildToolListResponse() 단일 경로
  base::ListValue tools_array;
  if (tool_registry_) {
    base::Value registry_list = tool_registry_->BuildToolListResponse();
    if (registry_list.is_list()) {
      for (auto& tool_entry : registry_list.GetList()) {
        tools_array.Append(std::move(tool_entry));
      }
    }
  }

  base::DictValue result;
  result.Set("tools", std::move(tools_array));

  size_t total = tool_registry_ ? tool_registry_->GetToolCount() : 0;
  LOG(INFO) << "[MCP] tools/list 응답: " << total << "개 도구 (client_id="
            << client_id << ")";
  SendResult(client_id, id, base::Value(std::move(result)));
}

// -----------------------------------------------------------------------
// tools/call 처리
// -----------------------------------------------------------------------

void McpServer::HandleToolsCall(int client_id, const base::Value* id,
                                 const base::DictValue* params) {
  auto state_it = client_states_.find(client_id);
  if (state_it == client_states_.end() ||
      state_it->second.handshake_state != HandshakeState::kReady) {
    SendError(client_id, id, kJsonRpcInvalidRequest,
              "Server not ready. Call initialize first.");
    return;
  }

  if (!params) {
    SendError(client_id, id, kJsonRpcInvalidParams, "Missing params");
    return;
  }

  // 도구 이름 추출
  const std::string* tool_name = params->FindString("name");
  if (!tool_name) {
    SendError(client_id, id, kJsonRpcInvalidParams, "Missing tool name");
    return;
  }

  // 도구 인자 추출 (없으면 빈 딕셔너리 사용)
  const base::DictValue* arguments = params->FindDict("arguments");
  base::DictValue empty_args;
  const base::DictValue& tool_args = arguments ? *arguments : empty_args;

  LOG(INFO) << "[MCP] 도구 실행: " << *tool_name;

  // 클라이언트에 배정된 세션 확인
  McpSession* client_session = GetSessionForClient(client_id);

  // 세션이 필요한 도구인데 세션이 없으면 자동 배정 시도
  bool tool_requires_session = true;
  if (tool_registry_) {
    McpTool* tool = tool_registry_->GetTool(*tool_name);
    if (tool) {
      tool_requires_session = tool->requires_session();
    }
  }

  if (!client_session && tool_requires_session) {
    LOG(INFO) << "[MCP] client_id=" << client_id
              << " 세션 없음. 자동 배정 시도...";
    Browser* browser = chrome::FindLastActive();
    if (!browser) {
      Profile* profile = ProfileManager::GetLastUsedProfileIfLoaded();
      if (profile) {
        chrome::NewEmptyWindow(profile);
        browser = chrome::FindLastActive();
      }
    }
    if (browser && browser->tab_strip_model()) {
      content::WebContents* wc =
          browser->tab_strip_model()->GetActiveWebContents();
      if (wc) {
        AssignTabToClient(client_id, wc);
        client_session = GetSessionForClient(client_id);
        LOG(INFO) << "[MCP] client_id=" << client_id
                  << " 자동 배정 성공: " << wc->GetVisibleURL().spec();
      }
    }
  }

  // id 복사본 생성 (비동기 콜백에서 사용하기 위해)
  std::optional<base::Value> id_copy;
  if (id) {
    id_copy = id->Clone();
  }

  auto result_callback = base::BindOnce(
      [](base::WeakPtr<McpServer> server,
         int client_id,
         std::optional<base::Value> id_copy,
         base::Value result) {
        if (!server) return;
        const base::Value* id_ptr =
            id_copy.has_value() ? &id_copy.value() : nullptr;
        server->SendResult(client_id, id_ptr, std::move(result));
      },
      weak_factory_.GetWeakPtr(), client_id, std::move(id_copy));

  // tool_registry_ 단일 경로
  if (tool_registry_) {
    // tabs new/select 완료 후 호출 클라이언트의 assigned_tab 갱신
    const std::string* action = tool_args.FindString("action");
    bool needs_tab_reassign =
        (*tool_name == "tabs") && action &&
        (*action == "new" || *action == "select");

    if (needs_tab_reassign) {
      auto wrapped_cb = base::BindOnce(
          [](base::WeakPtr<McpServer> server, int cid,
             base::OnceCallback<void(base::Value)> orig_cb,
             base::Value result) {
            if (server) {
              Browser* browser = chrome::FindLastActive();
              if (browser && browser->tab_strip_model()) {
                content::WebContents* wc =
                    browser->tab_strip_model()->GetActiveWebContents();
                if (wc) {
                  server->AssignTabToClient(cid, wc);
                }
              }
            }
            std::move(orig_cb).Run(std::move(result));
          },
          weak_factory_.GetWeakPtr(), client_id,
          std::move(result_callback));
      tool_registry_->DispatchToolCall(
          *tool_name, tool_args, client_session, std::move(wrapped_cb));
    } else {
      tool_registry_->DispatchToolCall(
          *tool_name, tool_args, client_session, std::move(result_callback));
    }
    return;
  }

  LOG(WARNING) << "[MCP] 알 수 없는 도구: " << *tool_name;
  SendError(client_id, id, kJsonRpcMethodNotFound,
            "Tool not found: " + *tool_name);
}

void McpServer::HandleUnknownMethod(int client_id, const base::Value* id,
                                     const std::string& method) {
  LOG(WARNING) << "[MCP] 알 수 없는 method: " << method
               << " (client_id=" << client_id << ")";
  // notifications/* 형식은 알림이므로 응답 불필요
  if (base::StartsWith(method, "notifications/",
                        base::CompareCase::SENSITIVE)) {
    return;
  }
  SendError(client_id, id, kJsonRpcMethodNotFound,
            "Method not found: " + method);
}

// -----------------------------------------------------------------------
// JSON-RPC 응답 전송 헬퍼
// -----------------------------------------------------------------------

void McpServer::SendResult(int client_id, const base::Value* id,
                            base::Value result) {
  // 도구별로 MakeJsonResult/MakeSuccessResult/MakeErrorResult/MakeImageResult를
  // 사용하므로 여기서 추가 정규화 불필요.
  base::DictValue response;
  response.Set("jsonrpc", "2.0");
  if (id) {
    response.Set("id", id->Clone());
  } else {
    response.Set("id", base::Value());  // null
  }
  response.Set("result", std::move(result));
  SendMessage(client_id, std::move(response));
}

void McpServer::SendError(int client_id, const base::Value* id,
                           int code,
                           const std::string& message) {
  base::DictValue error_obj;
  error_obj.Set("code", code);
  error_obj.Set("message", message);

  base::DictValue response;
  response.Set("jsonrpc", "2.0");
  if (id) {
    response.Set("id", id->Clone());
  } else {
    response.Set("id", base::Value());  // null
  }
  response.Set("error", std::move(error_obj));
  SendMessage(client_id, std::move(response));
}

void McpServer::SendMessage(int client_id, base::DictValue message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // base::Value를 JSON 문자열로 직렬화
  std::string json_output;
  if (!base::JSONWriter::Write(base::Value(std::move(message)), &json_output)) {
    LOG(ERROR) << "[MCP] JSON 직렬화 실패";
    return;
  }

  // transport가 없으면 (테스트 환경 등) 로그만 출력
  if (!transport_) {
    LOG(INFO) << "[MCP] 전송 (client_id=" << client_id << "): " << json_output;
    return;
  }

  transport_->Send(client_id, json_output);
}

// -----------------------------------------------------------------------
// 도구 등록
// -----------------------------------------------------------------------

void McpServer::RegisterBuiltinTools() {
  // 모든 도구는 tool_registry_ (McpTool 인터페이스 기반)로 등록된다.
  if (!tool_registry_) {
    tool_registry_ = std::make_unique<McpToolRegistry>();
  }

  // 브라우저 정보 (BrowserInfoTool — requires_session=false)
  tool_registry_->RegisterTool(std::make_unique<BrowserInfoTool>());

  // 페이지 제어
  tool_registry_->RegisterTool(std::make_unique<NavigateTool>());
  tool_registry_->RegisterTool(std::make_unique<ScreenshotTool>());
  tool_registry_->RegisterTool(std::make_unique<PageContentTool>());
  tool_registry_->RegisterTool(std::make_unique<JavaScriptTool>());
  {
    auto capture_tool = std::make_unique<NetworkCaptureTool>();
    NetworkCaptureTool* capture_tool_ptr = capture_tool.get();
    tool_registry_->RegisterTool(std::move(capture_tool));
    tool_registry_->RegisterTool(
        std::make_unique<NetworkRequestsTool>(capture_tool_ptr));
  }

  // 입력 도구
  tool_registry_->RegisterTool(std::make_unique<ClickTool>());
  tool_registry_->RegisterTool(std::make_unique<FillTool>());
  tool_registry_->RegisterTool(std::make_unique<KeyboardTool>());
  tool_registry_->RegisterTool(std::make_unique<MouseTool>());
  tool_registry_->RegisterTool(std::make_unique<ScrollTool>());
  tool_registry_->RegisterTool(std::make_unique<HoverTool>());
  tool_registry_->RegisterTool(std::make_unique<DragTool>());
  tool_registry_->RegisterTool(std::make_unique<SelectOptionTool>());

  // 스토리지/데이터
  tool_registry_->RegisterTool(std::make_unique<CookieTool>());
  tool_registry_->RegisterTool(std::make_unique<StorageTool>());
  tool_registry_->RegisterTool(std::make_unique<ClipboardTool>());

  // 브라우저 유틸
  tool_registry_->RegisterTool(std::make_unique<DialogTool>());
  tool_registry_->RegisterTool(std::make_unique<EmulationTool>());
  tool_registry_->RegisterTool(std::make_unique<TabsTool>());
  tool_registry_->RegisterTool(std::make_unique<WindowTool>());
  tool_registry_->RegisterTool(std::make_unique<WaitTool>());

  // 파일/미디어
  tool_registry_->RegisterTool(std::make_unique<DownloadTool>());
  tool_registry_->RegisterTool(std::make_unique<PdfTool>());
  tool_registry_->RegisterTool(std::make_unique<FileUploadTool>());

  // 요소/검색
  tool_registry_->RegisterTool(std::make_unique<ElementTool>());
  tool_registry_->RegisterTool(std::make_unique<ElementInfoTool>());
  tool_registry_->RegisterTool(std::make_unique<FindTool>());

  // 개발자/디버그
  tool_registry_->RegisterTool(std::make_unique<ConsoleTool>());
  tool_registry_->RegisterTool(std::make_unique<PerformanceTool>());
  tool_registry_->RegisterTool(std::make_unique<CoverageTool>());

  // 네트워크
  tool_registry_->RegisterTool(std::make_unique<NetworkInterceptTool>());

  // 브라우저 데이터
  tool_registry_->RegisterTool(std::make_unique<HistoryTool>());
  tool_registry_->RegisterTool(std::make_unique<BookmarkTool>());
}

// -----------------------------------------------------------------------
// MCP 응답 포맷 헬퍼 (static)
// -----------------------------------------------------------------------

// static
base::Value McpServer::MakeTextResult(const std::string& text) {
  // MCP tools/call 성공 응답 포맷:
  // {"content": [{"type": "text", "text": "..."}]}
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text", text);

  base::ListValue content_list;
  content_list.Append(std::move(content_item));

  base::DictValue result;
  result.Set("content", std::move(content_list));
  return base::Value(std::move(result));
}

// static
base::Value McpServer::MakeErrorResult(const std::string& error_message) {
  // MCP tools/call 오류 응답 포맷:
  // {"isError": true, "content": [{"type": "text", "text": "오류 메시지"}]}
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text", error_message);

  base::ListValue content_list;
  content_list.Append(std::move(content_item));

  base::DictValue result;
  result.Set("isError", true);
  result.Set("content", std::move(content_list));
  return base::Value(std::move(result));
}

}  // namespace mcp
