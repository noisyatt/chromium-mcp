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
#include "chrome/browser/mcp/tools/dom_tool.h"
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
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/web_contents.h"

// MCP 커맨드라인 플래그 정의
// 실제 배포 시 난독화된 이름으로 교체 가능
constexpr char kMcpStdioSwitch[] = "mcp-stdio";
constexpr char kMcpSocketSwitch[] = "mcp-socket";
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

McpServer::Config::Config() = default;
McpServer::Config::~Config() = default;

McpToolDefinition::McpToolDefinition() = default;
McpToolDefinition::~McpToolDefinition() = default;
McpToolDefinition::McpToolDefinition(McpToolDefinition&&) = default;
McpToolDefinition& McpToolDefinition::operator=(McpToolDefinition&&) = default;

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

// static
bool McpServer::ShouldStart() {
  // 1. 커맨드라인 플래그 확인
  const base::CommandLine* cmd = base::CommandLine::ForCurrentProcess();
  if (cmd->HasSwitch(kMcpStdioSwitch) || cmd->HasSwitch(kMcpSocketSwitch)) {
    LOG(INFO) << "[MCP] 커맨드라인 플래그로 MCP 서버 활성화";
    return true;
  }

  // 2. 환경변수 확인 (CHROMIUM_MCP=1)
  // 플래그보다 은닉성이 높은 활성화 방법
  std::unique_ptr<base::Environment> env = base::Environment::Create();
  auto env_value = env->GetVar(kMcpEnvVar);
  if (env_value.has_value() && *env_value == "1") {
    LOG(INFO) << "[MCP] 환경변수로 MCP 서버 활성화";
    return true;
  }

  return false;
}

bool McpServer::Initialize(std::unique_ptr<Config> config,
                           content::BrowserContext* browser_context) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(browser_context);

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
  transport_->Start(
      base::BindRepeating(&McpServer::OnMessageReceived,
                          weak_factory_.GetWeakPtr()),
      base::BindRepeating(&McpServer::Shutdown,
                          weak_factory_.GetWeakPtr()));

  // 기본 내장 도구 등록 (navigate, screenshot 등)
  RegisterBuiltinTools();

  LOG(INFO) << "[MCP] 서버 초기화 완료. " << tools_.size() << "개 도구 등록됨";
  return true;
}

void McpServer::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  LOG(INFO) << "[MCP] 서버 종료 시작";

  // 모든 세션 종료 (DevToolsAgentHost 연결 해제)
  sessions_.clear();
  active_web_contents_ = nullptr;

  // 전송 계층 종료
  if (transport_) {
    // transport_->Stop();
    transport_.reset();
  }

  LOG(INFO) << "[MCP] 서버 종료 완료";
}

// -----------------------------------------------------------------------
// 탭(WebContents) 연결 관리
// -----------------------------------------------------------------------

McpSession* McpServer::AttachToWebContents(
    content::WebContents* web_contents) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(web_contents);

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
  auto session = std::make_unique<McpSession>(
      agent_host,
      base::BindRepeating(&McpServer::SendMessage,
                          weak_factory_.GetWeakPtr()));

  // 세션을 통해 CDP 연결 활성화
  if (!session->Attach()) {
    LOG(ERROR) << "[MCP] CDP 세션 연결 실패";
    return nullptr;
  }

  McpSession* session_ptr = session.get();
  sessions_[web_contents] = std::move(session);
  active_web_contents_ = web_contents;

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

  if (active_web_contents_ == web_contents) {
    active_web_contents_ = nullptr;
  }

  LOG(INFO) << "[MCP] WebContents에서 세션 분리 완료";
}

McpSession* McpServer::GetActiveSession() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  if (!active_web_contents_) {
    return nullptr;
  }

  auto it = sessions_.find(active_web_contents_);
  if (it == sessions_.end()) {
    return nullptr;
  }

  return it->second.get();
}

// -----------------------------------------------------------------------
// JSON-RPC 메시지 수신 및 라우팅
// -----------------------------------------------------------------------

void McpServer::OnMessageReceived(const std::string& json_message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // JSON 파싱
  auto parsed = base::JSONReader::ReadAndReturnValueWithError(json_message, base::JSON_PARSE_RFC);
  if (!parsed.has_value()) {
    LOG(WARNING) << "[MCP] JSON 파싱 실패: " << parsed.error().message;
    // id를 알 수 없으므로 null로 오류 응답
    SendError(nullptr, kJsonRpcParseError, "Parse error: " + parsed.error().message);
    return;
  }

  if (!parsed->is_dict()) {
    LOG(WARNING) << "[MCP] JSON-RPC 메시지가 객체 형식이 아님";
    SendError(nullptr, kJsonRpcInvalidRequest, "Request must be a JSON object");
    return;
  }

  HandleMessage(std::move(parsed->GetDict()));
}

void McpServer::HandleMessage(base::DictValue message) {
  // jsonrpc 버전 검증
  const std::string* jsonrpc = message.FindString("jsonrpc");
  if (!jsonrpc || *jsonrpc != "2.0") {
    LOG(WARNING) << "[MCP] jsonrpc 버전이 2.0이 아님";
    const base::Value* id = message.Find("id");
    SendError(id, kJsonRpcInvalidRequest, "jsonrpc must be \"2.0\"");
    return;
  }

  // method 추출
  const std::string* method = message.FindString("method");
  if (!method) {
    LOG(WARNING) << "[MCP] method 필드 없음";
    const base::Value* id = message.Find("id");
    SendError(id, kJsonRpcInvalidRequest, "Missing method field");
    return;
  }

  // id 추출 (알림 메시지는 id 없음)
  const base::Value* id = message.Find("id");
  const base::DictValue* params = message.FindDict("params");

  LOG(INFO) << "[MCP] 수신된 method: " << *method;

  // method에 따라 핸들러 라우팅
  if (*method == "initialize") {
    HandleInitialize(id, params);
  } else if (*method == "notifications/initialized") {
    HandleInitialized();
  } else if (*method == "tools/list") {
    HandleToolsList(id);
  } else if (*method == "tools/call") {
    HandleToolsCall(id, params);
  } else {
    // 알 수 없는 method: 표준 JSON-RPC 오류 응답
    HandleUnknownMethod(id, *method);
  }
}

// -----------------------------------------------------------------------
// MCP 프로토콜 핸드셰이크
// -----------------------------------------------------------------------

void McpServer::HandleInitialize(const base::Value* id,
                                  const base::DictValue* params) {
  // 이미 초기화된 경우 재초기화 무시
  if (handshake_state_ != HandshakeState::kNotStarted) {
    LOG(WARNING) << "[MCP] 이미 초기화됨. initialize 요청 무시.";
    SendError(id, kJsonRpcInvalidRequest, "Already initialized");
    return;
  }

  // 클라이언트 정보 저장
  if (params) {
    if (const std::string* ver = params->FindString("protocolVersion")) {
      protocol_version_ = *ver;
    }
    if (const base::DictValue* client_info = params->FindDict("clientInfo")) {
      if (const std::string* name = client_info->FindString("name")) {
        client_name_ = *name;
      }
      if (const std::string* version = client_info->FindString("version")) {
        client_version_ = *version;
      }
    }
  }

  LOG(INFO) << "[MCP] 클라이언트 연결: " << client_name_
            << " v" << client_version_
            << " (프로토콜: " << protocol_version_ << ")";

  handshake_state_ = HandshakeState::kInitializing;

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

  SendResult(id, base::Value(std::move(result)));
}

void McpServer::HandleInitialized() {
  // notifications/initialized는 응답이 없는 알림(notification)
  if (handshake_state_ != HandshakeState::kInitializing) {
    LOG(WARNING) << "[MCP] 잘못된 상태에서 initialized 수신";
    return;
  }

  handshake_state_ = HandshakeState::kReady;
  LOG(INFO) << "[MCP] 핸드셰이크 완료. 도구 호출 대기 중.";

  // 활성 탭이 있으면 자동으로 세션 연결
  if (config_ && config_->auto_attach_active_tab && active_web_contents_) {
    AttachToWebContents(active_web_contents_);
  }
}

// -----------------------------------------------------------------------
// tools/list 처리
// -----------------------------------------------------------------------

void McpServer::HandleToolsList(const base::Value* id) {
  if (handshake_state_ != HandshakeState::kReady) {
    SendError(id, kJsonRpcInvalidRequest, "Server not ready. Call initialize first.");
    return;
  }

  // 등록된 모든 도구의 명세를 배열로 직렬화
  base::ListValue tools_array;

  // 1. 인라인 등록 도구
  for (const auto& [name, tool_def] : tools_) {
    base::DictValue tool_entry;
    tool_entry.Set("name", tool_def.name);
    tool_entry.Set("description", tool_def.description);
    tool_entry.Set("inputSchema", tool_def.input_schema.Clone());
    tools_array.Append(std::move(tool_entry));
  }

  // 2. McpToolRegistry 기반 도구
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

  size_t total = tools_.size() + (tool_registry_ ? tool_registry_->GetToolCount() : 0);
  LOG(INFO) << "[MCP] tools/list 응답: " << total << "개 도구";
  SendResult(id, base::Value(std::move(result)));
}

// -----------------------------------------------------------------------
// tools/call 처리
// -----------------------------------------------------------------------

void McpServer::HandleToolsCall(const base::Value* id,
                                 const base::DictValue* params) {
  if (handshake_state_ != HandshakeState::kReady) {
    SendError(id, kJsonRpcInvalidRequest, "Server not ready. Call initialize first.");
    return;
  }

  if (!params) {
    SendError(id, kJsonRpcInvalidParams, "Missing params");
    return;
  }

  // 도구 이름 추출
  const std::string* tool_name = params->FindString("name");
  if (!tool_name) {
    SendError(id, kJsonRpcInvalidParams, "Missing tool name");
    return;
  }

  // 도구 인자 추출 (없으면 빈 딕셔너리 사용)
  const base::DictValue* arguments = params->FindDict("arguments");
  base::DictValue empty_args;
  const base::DictValue& tool_args = arguments ? *arguments : empty_args;

  LOG(INFO) << "[MCP] 도구 실행: " << *tool_name;

  // id 복사본 생성 (비동기 콜백에서 사용하기 위해)
  std::optional<base::Value> id_copy;
  if (id) {
    id_copy = id->Clone();
  }

  auto result_callback = base::BindOnce(
      [](base::WeakPtr<McpServer> server,
         std::optional<base::Value> id_copy,
         base::Value result) {
        if (!server) return;
        const base::Value* id_ptr =
            id_copy.has_value() ? &id_copy.value() : nullptr;
        server->SendResult(id_ptr, std::move(result));
      },
      weak_factory_.GetWeakPtr(), std::move(id_copy));

  // 1. 인라인 등록 도구에서 검색
  auto it = tools_.find(*tool_name);
  if (it != tools_.end()) {
    it->second.handler.Run(tool_args, std::move(result_callback));
    return;
  }

  // 2. McpToolRegistry 기반 도구에서 검색
  if (tool_registry_) {
    tool_registry_->DispatchToolCall(
        *tool_name, tool_args, GetActiveSession(), std::move(result_callback));
    return;
  }

  LOG(WARNING) << "[MCP] 알 수 없는 도구: " << *tool_name;
  SendError(id, kJsonRpcMethodNotFound,
            "Tool not found: " + *tool_name);
}

void McpServer::HandleUnknownMethod(const base::Value* id,
                                     const std::string& method) {
  LOG(WARNING) << "[MCP] 알 수 없는 method: " << method;
  // notifications/* 형식은 알림이므로 응답 불필요
  if (base::StartsWith(method, "notifications/",
                        base::CompareCase::SENSITIVE)) {
    return;
  }
  SendError(id, kJsonRpcMethodNotFound, "Method not found: " + method);
}

// -----------------------------------------------------------------------
// JSON-RPC 응답 전송 헬퍼
// -----------------------------------------------------------------------

void McpServer::SendResult(const base::Value* id, base::Value result) {
  base::DictValue response;
  response.Set("jsonrpc", "2.0");
  if (id) {
    response.Set("id", id->Clone());
  } else {
    response.Set("id", base::Value());  // null
  }
  response.Set("result", std::move(result));
  SendMessage(std::move(response));
}

void McpServer::SendError(const base::Value* id,
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
  SendMessage(std::move(response));
}

void McpServer::SendMessage(base::DictValue message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // base::Value를 JSON 문자열로 직렬화
  std::string json_output;
  if (!base::JSONWriter::Write(base::Value(std::move(message)), &json_output)) {
    LOG(ERROR) << "[MCP] JSON 직렬화 실패";
    return;
  }

  // transport가 없으면 (테스트 환경 등) 로그만 출력
  if (!transport_) {
    LOG(INFO) << "[MCP] 전송: " << json_output;
    return;
  }

  transport_->Send(json_output);
}

// -----------------------------------------------------------------------
// 도구 등록
// -----------------------------------------------------------------------

void McpServer::RegisterTool(McpToolDefinition tool_def) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const std::string name = tool_def.name;

  if (tools_.count(name)) {
    LOG(WARNING) << "[MCP] 도구 이름 중복 등록 시도: " << name;
    return;
  }

  tools_[name] = std::move(tool_def);
  LOG(INFO) << "[MCP] 도구 등록: " << name;
}

void McpServer::RegisterBuiltinTools() {
  // ===== 기존 인라인 도구 (Phase 1 호환) =====
  RegisterNavigateTool();
  RegisterScreenshotTool();
  RegisterPageContentTool();
  RegisterClickTool();
  RegisterFillTool();
  RegisterEvaluateTool();
  RegisterNetworkCaptureTool();
  RegisterNetworkRequestsTool();
  RegisterTabsTool();
  RegisterBrowserInfoTool();

  // ===== 신규 McpTool 인터페이스 기반 도구 =====
  // McpToolRegistry에 등록하고, tools_ 맵에도 브릿지 등록하여
  // HandleToolsCall()에서 통합 디스패치
  if (!tool_registry_) {
    tool_registry_ = std::make_unique<McpToolRegistry>();
  }

  // 입력 도구
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

void McpServer::RegisterNavigateTool() {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;
  {
    base::DictValue url_prop;
    url_prop.Set("type", "string");
    url_prop.Set("description", "이동할 URL (예: https://example.com)");
    properties.Set("url", std::move(url_prop));

    base::DictValue action_prop;
    action_prop.Set("type", "string");
    base::ListValue action_enum;
    action_enum.Append("url");
    action_enum.Append("back");
    action_enum.Append("forward");
    action_enum.Append("reload");
    action_prop.Set("enum", std::move(action_enum));
    action_prop.Set("description", "탐색 동작 (기본: url)");
    properties.Set("action", std::move(action_prop));
  }
  schema.Set("properties", std::move(properties));

  McpToolDefinition def;
  def.name = "navigate";
  def.description = "URL로 이동하거나 뒤로/앞으로 탐색, 또는 페이지 새로고침";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecuteNavigate,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

void McpServer::RegisterScreenshotTool() {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;
  {
    base::DictValue full_page_prop;
    full_page_prop.Set("type", "boolean");
    full_page_prop.Set("description", "전체 페이지 캡처 여부 (기본: false)");
    properties.Set("fullPage", std::move(full_page_prop));

    base::DictValue format_prop;
    format_prop.Set("type", "string");
    base::ListValue fmt_enum;
    fmt_enum.Append("png");
    fmt_enum.Append("jpeg");
    format_prop.Set("enum", std::move(fmt_enum));
    format_prop.Set("description", "이미지 형식 (기본: png)");
    properties.Set("format", std::move(format_prop));
  }
  schema.Set("properties", std::move(properties));

  McpToolDefinition def;
  def.name = "screenshot";
  def.description = "현재 페이지의 스크린샷을 base64 인코딩된 이미지로 반환";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecuteScreenshot,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

void McpServer::RegisterPageContentTool() {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;
  {
    base::DictValue mode_prop;
    mode_prop.Set("type", "string");
    base::ListValue mode_enum;
    mode_enum.Append("accessibility");
    mode_enum.Append("html");
    mode_enum.Append("text");
    mode_prop.Set("enum", std::move(mode_enum));
    mode_prop.Set("description", "반환 형식 (기본: accessibility)");
    properties.Set("mode", std::move(mode_prop));

    base::DictValue selector_prop;
    selector_prop.Set("type", "string");
    selector_prop.Set("description", "특정 요소만 가져올 CSS 선택자 (선택적)");
    properties.Set("selector", std::move(selector_prop));
  }
  schema.Set("properties", std::move(properties));

  McpToolDefinition def;
  def.name = "page_content";
  def.description = "현재 페이지의 접근성 트리, HTML 또는 텍스트 내용 반환";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecutePageContent,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

void McpServer::RegisterClickTool() {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;
  {
    base::DictValue sel_prop;
    sel_prop.Set("type", "string");
    sel_prop.Set("description", "클릭할 요소의 CSS 선택자");
    properties.Set("selector", std::move(sel_prop));

    base::DictValue btn_prop;
    btn_prop.Set("type", "string");
    base::ListValue btn_enum;
    btn_enum.Append("left");
    btn_enum.Append("right");
    btn_enum.Append("middle");
    btn_prop.Set("enum", std::move(btn_enum));
    btn_prop.Set("description", "마우스 버튼 (기본: left)");
    properties.Set("button", std::move(btn_prop));
  }
  schema.Set("properties", std::move(properties));

  base::ListValue required;
  required.Append("selector");
  schema.Set("required", std::move(required));

  McpToolDefinition def;
  def.name = "click";
  def.description = "CSS 선택자로 지정한 요소를 클릭";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecuteClick,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

void McpServer::RegisterFillTool() {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;
  {
    base::DictValue sel_prop;
    sel_prop.Set("type", "string");
    sel_prop.Set("description", "입력 필드의 CSS 선택자");
    properties.Set("selector", std::move(sel_prop));

    base::DictValue val_prop;
    val_prop.Set("type", "string");
    val_prop.Set("description", "입력할 값");
    properties.Set("value", std::move(val_prop));
  }
  schema.Set("properties", std::move(properties));

  base::ListValue required;
  required.Append("selector");
  required.Append("value");
  schema.Set("required", std::move(required));

  McpToolDefinition def;
  def.name = "fill";
  def.description = "CSS 선택자로 지정한 입력 필드에 값 입력";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecuteFill,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

void McpServer::RegisterEvaluateTool() {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;
  {
    base::DictValue expr_prop;
    expr_prop.Set("type", "string");
    expr_prop.Set("description", "실행할 JavaScript 코드");
    properties.Set("expression", std::move(expr_prop));

    base::DictValue await_prop;
    await_prop.Set("type", "boolean");
    await_prop.Set("description", "Promise 대기 여부 (기본: true)");
    properties.Set("awaitPromise", std::move(await_prop));
  }
  schema.Set("properties", std::move(properties));

  base::ListValue required;
  required.Append("expression");
  schema.Set("required", std::move(required));

  McpToolDefinition def;
  def.name = "evaluate";
  def.description = "현재 페이지에서 JavaScript 코드 실행 후 결과 반환";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecuteEvaluate,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

void McpServer::RegisterNetworkCaptureTool() {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;
  {
    base::DictValue action_prop;
    action_prop.Set("type", "string");
    base::ListValue action_enum;
    action_enum.Append("start");
    action_enum.Append("stop");
    action_prop.Set("enum", std::move(action_enum));
    action_prop.Set("description", "캡처 시작 또는 중지");
    properties.Set("action", std::move(action_prop));

    base::DictValue body_prop;
    body_prop.Set("type", "boolean");
    body_prop.Set("description", "응답 본문 포함 여부 (기본: false)");
    properties.Set("includeResponseBody", std::move(body_prop));
  }
  schema.Set("properties", std::move(properties));

  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  McpToolDefinition def;
  def.name = "network_capture";
  def.description = "네트워크 요청 캡처 시작/중지 (디버그 배너 없음)";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecuteNetworkCapture,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

void McpServer::RegisterNetworkRequestsTool() {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;
  {
    base::DictValue static_prop;
    static_prop.Set("type", "boolean");
    static_prop.Set("description", "정적 리소스 포함 여부 (기본: false)");
    properties.Set("includeStatic", std::move(static_prop));
  }
  schema.Set("properties", std::move(properties));

  McpToolDefinition def;
  def.name = "network_requests";
  def.description = "현재까지 캡처된 네트워크 요청 목록 반환";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecuteNetworkRequests,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

void McpServer::RegisterTabsTool() {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;
  {
    base::DictValue action_prop;
    action_prop.Set("type", "string");
    base::ListValue action_enum;
    action_enum.Append("list");
    action_enum.Append("new");
    action_enum.Append("close");
    action_enum.Append("select");
    action_prop.Set("enum", std::move(action_enum));
    properties.Set("action", std::move(action_prop));

    base::DictValue tab_id_prop;
    tab_id_prop.Set("type", "number");
    tab_id_prop.Set("description", "탭 ID (close/select에서 사용)");
    properties.Set("tabId", std::move(tab_id_prop));

    base::DictValue url_prop;
    url_prop.Set("type", "string");
    url_prop.Set("description", "새 탭 URL (new 동작에서 사용)");
    properties.Set("url", std::move(url_prop));
  }
  schema.Set("properties", std::move(properties));

  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  McpToolDefinition def;
  def.name = "tabs";
  def.description = "탭 목록 조회, 새 탭 생성, 탭 닫기, 탭 전환";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecuteTabs,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

void McpServer::RegisterBrowserInfoTool() {
  base::DictValue schema;
  schema.Set("type", "object");
  schema.Set("properties", base::DictValue());

  McpToolDefinition def;
  def.name = "browser_info";
  def.description = "브라우저 버전, 활성 탭 URL, 탭 개수 등 브라우저 정보 반환";
  def.input_schema = std::move(schema);
  def.handler = base::BindRepeating(&McpServer::ExecuteBrowserInfo,
                                     weak_factory_.GetWeakPtr());
  RegisterTool(std::move(def));
}

// -----------------------------------------------------------------------
// 도구 실행 핸들러 구현
// -----------------------------------------------------------------------

void McpServer::ExecuteNavigate(const base::DictValue& params,
                                 base::OnceCallback<void(base::Value)> callback) {
  McpSession* session = GetActiveSession();
  if (!session) {
    std::move(callback).Run(MakeErrorResult("활성 탭 세션이 없습니다. 탭을 열어주세요."));
    return;
  }

  // action 파라미터 확인 (기본값: "url")
  const std::string* action = params.FindString("action");
  std::string action_str = action ? *action : "url";

  if (action_str == "url") {
    const std::string* url = params.FindString("url");
    if (!url || url->empty()) {
      std::move(callback).Run(MakeErrorResult("URL이 필요합니다."));
      return;
    }

    // CDP Page.navigate 명령 파라미터 구성
    base::DictValue cdp_params;
    cdp_params.Set("url", *url);

    // McpSession을 통해 CDP 명령 비동기 실행.
    // 콜백에서 CDP 응답을 MCP 결과로 변환.
    session->SendCdpCommand(
        "Page.navigate",
        std::move(cdp_params),
        base::BindOnce(
            [](base::OnceCallback<void(base::Value)> callback,
               std::optional<base::DictValue> cdp_result,
               const std::string& error) {
              if (!error.empty()) {
                std::move(callback).Run(MakeErrorResult("탐색 실패: " + error));
                return;
              }
              std::string result_text = "페이지 로드 시작됨";
              if (cdp_result) {
                if (const std::string* frame_id = cdp_result->FindString("frameId")) {
                  result_text += " (frameId: " + *frame_id + ")";
                }
              }
              std::move(callback).Run(MakeTextResult(result_text));
            },
            std::move(callback)));

  } else if (action_str == "back") {
    // 히스토리 뒤로 이동: Runtime.evaluate로 history.back() 실행
    base::DictValue cdp_params;
    cdp_params.Set("expression", "history.back()");
    cdp_params.Set("returnByValue", false);
    session->SendCdpCommand("Runtime.evaluate", std::move(cdp_params),
        base::BindOnce([](base::OnceCallback<void(base::Value)> cb,
                          std::optional<base::DictValue>,
                          const std::string& err) {
          if (!err.empty()) {
            std::move(cb).Run(MakeErrorResult("뒤로 이동 실패: " + err));
          } else {
            std::move(cb).Run(MakeTextResult("뒤로 이동 완료"));
          }
        }, std::move(callback)));

  } else if (action_str == "forward") {
    base::DictValue cdp_params;
    cdp_params.Set("expression", "history.forward()");
    cdp_params.Set("returnByValue", false);
    session->SendCdpCommand("Runtime.evaluate", std::move(cdp_params),
        base::BindOnce([](base::OnceCallback<void(base::Value)> cb,
                          std::optional<base::DictValue>,
                          const std::string& err) {
          if (!err.empty()) {
            std::move(cb).Run(MakeErrorResult("앞으로 이동 실패: " + err));
          } else {
            std::move(cb).Run(MakeTextResult("앞으로 이동 완료"));
          }
        }, std::move(callback)));

  } else if (action_str == "reload") {
    // 페이지 새로고침: Page.reload CDP 명령
    base::DictValue cdp_params;
    cdp_params.Set("ignoreCache", false);
    session->SendCdpCommand("Page.reload", std::move(cdp_params),
        base::BindOnce([](base::OnceCallback<void(base::Value)> cb,
                          std::optional<base::DictValue>,
                          const std::string& err) {
          if (!err.empty()) {
            std::move(cb).Run(MakeErrorResult("새로고침 실패: " + err));
          } else {
            std::move(cb).Run(MakeTextResult("페이지 새로고침 완료"));
          }
        }, std::move(callback)));
  } else {
    std::move(callback).Run(
        MakeErrorResult("알 수 없는 action: " + action_str));
  }
}

void McpServer::ExecuteScreenshot(
    const base::DictValue& params,
    base::OnceCallback<void(base::Value)> callback) {
  McpSession* session = GetActiveSession();
  if (!session) {
    std::move(callback).Run(MakeErrorResult("활성 탭 세션이 없습니다."));
    return;
  }

  // CDP Page.captureScreenshot 파라미터 구성
  base::DictValue cdp_params;

  // 이미지 형식 설정 (기본: png)
  const std::string* format = params.FindString("format");
  cdp_params.Set("format", format ? *format : "png");
  cdp_params.Set("quality", 90);

  // captureBeyondViewport: 전체 페이지 캡처 여부
  std::optional<bool> full_page = params.FindBool("fullPage");
  cdp_params.Set("captureBeyondViewport",
                  full_page.value_or(false));

  session->SendCdpCommand(
      "Page.captureScreenshot",
      std::move(cdp_params),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> callback,
             std::optional<base::DictValue> cdp_result,
             const std::string& error) {
            if (!error.empty()) {
              std::move(callback).Run(
                  MakeErrorResult("스크린샷 실패: " + error));
              return;
            }
            if (!cdp_result) {
              std::move(callback).Run(MakeErrorResult("스크린샷 응답 없음"));
              return;
            }

            // CDP 응답에서 base64 인코딩된 이미지 데이터 추출
            const std::string* data = cdp_result->FindString("data");
            if (!data) {
              std::move(callback).Run(MakeErrorResult("이미지 데이터 없음"));
              return;
            }

            // MCP 이미지 응답 포맷:
            // {"content": [{"type": "image", "data": "...", "mimeType": "..."}]}
            base::DictValue image_content;
            image_content.Set("type", "image");
            image_content.Set("data", *data);
            image_content.Set("mimeType", "image/png");

            base::ListValue content_list;
            content_list.Append(std::move(image_content));

            base::DictValue result;
            result.Set("content", std::move(content_list));
            std::move(callback).Run(base::Value(std::move(result)));
          },
          std::move(callback)));
}

void McpServer::ExecutePageContent(
    const base::DictValue& params,
    base::OnceCallback<void(base::Value)> callback) {
  McpSession* session = GetActiveSession();
  if (!session) {
    std::move(callback).Run(MakeErrorResult("활성 탭 세션이 없습니다."));
    return;
  }

  const std::string* mode = params.FindString("mode");
  std::string mode_str = mode ? *mode : "accessibility";

  if (mode_str == "html") {
    // DOM.getOuterHTML: 전체 페이지 HTML 반환
    base::DictValue cdp_params;
    // nodeId 없이 호출하면 document 루트 반환
    session->SendCdpCommand(
        "DOM.getDocument",
        base::DictValue(),
        base::BindOnce(
            [](McpSession* session,
               base::OnceCallback<void(base::Value)> callback,
               std::optional<base::DictValue> result,
               const std::string& error) {
              if (!error.empty() || !result) {
                std::move(callback).Run(
                    MakeErrorResult("DOM 가져오기 실패: " + error));
                return;
              }
              // root.nodeId 추출 후 getOuterHTML 호출
              const base::DictValue* root = result->FindDict("root");
              if (!root) {
                std::move(callback).Run(MakeErrorResult("루트 노드 없음"));
                return;
              }
              std::optional<int> node_id = root->FindInt("nodeId");
              if (!node_id) {
                std::move(callback).Run(MakeErrorResult("nodeId 없음"));
                return;
              }
              base::DictValue html_params;
              html_params.Set("nodeId", *node_id);
              session->SendCdpCommand(
                  "DOM.getOuterHTML",
                  std::move(html_params),
                  base::BindOnce(
                      [](base::OnceCallback<void(base::Value)> cb,
                         std::optional<base::DictValue> html_result,
                         const std::string& html_error) {
                        if (!html_error.empty() || !html_result) {
                          std::move(cb).Run(
                              MakeErrorResult("HTML 추출 실패: " + html_error));
                          return;
                        }
                        const std::string* html = html_result->FindString("outerHTML");
                        std::move(cb).Run(
                            MakeTextResult(html ? *html : ""));
                      },
                      std::move(callback)));
            },
            session, std::move(callback)));

  } else if (mode_str == "text") {
    // Runtime.evaluate로 document.body.innerText 실행
    base::DictValue cdp_params;
    cdp_params.Set("expression", "document.body ? document.body.innerText : ''");
    cdp_params.Set("returnByValue", true);
    session->SendCdpCommand(
        "Runtime.evaluate",
        std::move(cdp_params),
        base::BindOnce(
            [](base::OnceCallback<void(base::Value)> cb,
               std::optional<base::DictValue> result,
               const std::string& error) {
              if (!error.empty() || !result) {
                std::move(cb).Run(MakeErrorResult("텍스트 추출 실패: " + error));
                return;
              }
              // 결과 구조: {"result": {"type": "string", "value": "..."}}
              const base::DictValue* inner = result->FindDict("result");
              std::string text;
              if (inner) {
                if (const std::string* val = inner->FindString("value")) {
                  text = *val;
                }
              }
              std::move(cb).Run(MakeTextResult(text));
            },
            std::move(callback)));

  } else {
    // accessibility 모드: Accessibility.getFullAXTree 실행
    session->SendCdpCommand(
        "Accessibility.getFullAXTree",
        base::DictValue(),
        base::BindOnce(
            [](base::OnceCallback<void(base::Value)> cb,
               std::optional<base::DictValue> result,
               const std::string& error) {
              if (!error.empty() || !result) {
                std::move(cb).Run(
                    MakeErrorResult("접근성 트리 추출 실패: " + error));
                return;
              }
              // 접근성 트리를 JSON 문자열로 변환
              std::string json_output;
              base::JSONWriter::Write(base::Value(result->Clone()), &json_output);
              std::move(cb).Run(MakeTextResult(json_output));
            },
            std::move(callback)));
  }
}

void McpServer::ExecuteClick(const base::DictValue& params,
                              base::OnceCallback<void(base::Value)> callback) {
  McpSession* session = GetActiveSession();
  if (!session) {
    std::move(callback).Run(MakeErrorResult("활성 탭 세션이 없습니다."));
    return;
  }

  const std::string* selector = params.FindString("selector");
  if (!selector || selector->empty()) {
    std::move(callback).Run(MakeErrorResult("selector가 필요합니다."));
    return;
  }

  // JavaScript로 요소를 찾아 getBoundingClientRect() 실행.
  // 좌표를 구한 후 Input.dispatchMouseEvent로 클릭 시뮬레이션.
  // Runtime.enable 없이 Runtime.evaluate만 사용하여 탐지 최소화.
  const std::string& sel = *selector;
  std::string js = "(() => {"
      "  const el = document.querySelector('" + sel + "');"
      "  if (!el) return null;"
      "  const r = el.getBoundingClientRect();"
      "  return {x: r.left + r.width/2, y: r.top + r.height/2};"
      "})()";

  base::DictValue cdp_params;
  cdp_params.Set("expression", js);
  cdp_params.Set("returnByValue", true);

  session->SendCdpCommand(
      "Runtime.evaluate",
      std::move(cdp_params),
      base::BindOnce(
          [](McpSession* session,
             std::string button_str,
             base::OnceCallback<void(base::Value)> callback,
             std::optional<base::DictValue> result,
             const std::string& error) {
            if (!error.empty() || !result) {
              std::move(callback).Run(
                  MakeErrorResult("요소 위치 조회 실패: " + error));
              return;
            }
            // 좌표 추출
            const base::DictValue* inner = result->FindDict("result");
            if (!inner) {
              std::move(callback).Run(MakeErrorResult("결과 없음"));
              return;
            }
            const base::DictValue* coords = inner->FindDict("value");
            if (!coords) {
              std::move(callback).Run(MakeErrorResult("요소를 찾을 수 없습니다."));
              return;
            }
            std::optional<double> x = coords->FindDouble("x");
            std::optional<double> y = coords->FindDouble("y");
            if (!x || !y) {
              std::move(callback).Run(MakeErrorResult("좌표 추출 실패"));
              return;
            }

            // Input.dispatchMouseEvent로 mousedown → mouseup → click 시퀀스 전송
            auto send_mouse_event = [&](const std::string& type) {
              base::DictValue mouse_params;
              mouse_params.Set("type", type);
              mouse_params.Set("x", *x);
              mouse_params.Set("y", *y);
              mouse_params.Set("button", button_str);
              mouse_params.Set("clickCount", 1);
              session->SendCdpCommand(
                  "Input.dispatchMouseEvent",
                  std::move(mouse_params),
                  base::BindOnce([](base::Value) {}));
            };

            send_mouse_event("mousePressed");
            send_mouse_event("mouseReleased");

            std::move(callback).Run(MakeTextResult("클릭 완료"));
          },
          session,
          params.FindString("button") ? *params.FindString("button") : "left",
          std::move(callback)));
}

void McpServer::ExecuteFill(const base::DictValue& params,
                             base::OnceCallback<void(base::Value)> callback) {
  McpSession* session = GetActiveSession();
  if (!session) {
    std::move(callback).Run(MakeErrorResult("활성 탭 세션이 없습니다."));
    return;
  }

  const std::string* selector = params.FindString("selector");
  const std::string* value = params.FindString("value");
  if (!selector || !value) {
    std::move(callback).Run(MakeErrorResult("selector와 value가 필요합니다."));
    return;
  }

  // JavaScript로 입력 필드 값 설정.
  // React/Vue 등 프레임워크 호환을 위해 input 이벤트도 발생시킴.
  const std::string& sel = *selector;
  const std::string& val = *value;
  std::string js =
      "(() => {"
      "  const el = document.querySelector('" + sel + "');"
      "  if (!el) return 'not_found';"
      "  const nativeInputValueSetter = "
      "    Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value')?.set;"
      "  if (nativeInputValueSetter) {"
      "    nativeInputValueSetter.call(el, '" + val + "');"
      "  } else {"
      "    el.value = '" + val + "';"
      "  }"
      "  el.dispatchEvent(new Event('input', {bubbles: true}));"
      "  el.dispatchEvent(new Event('change', {bubbles: true}));"
      "  return 'ok';"
      "})()";

  base::DictValue cdp_params;
  cdp_params.Set("expression", js);
  cdp_params.Set("returnByValue", true);

  session->SendCdpCommand(
      "Runtime.evaluate",
      std::move(cdp_params),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb,
             std::optional<base::DictValue> result,
             const std::string& error) {
            if (!error.empty()) {
              std::move(cb).Run(MakeErrorResult("입력 실패: " + error));
              return;
            }
            std::move(cb).Run(MakeTextResult("값 입력 완료"));
          },
          std::move(callback)));
}

void McpServer::ExecuteEvaluate(const base::DictValue& params,
                                 base::OnceCallback<void(base::Value)> callback) {
  McpSession* session = GetActiveSession();
  if (!session) {
    std::move(callback).Run(MakeErrorResult("활성 탭 세션이 없습니다."));
    return;
  }

  const std::string* expression = params.FindString("expression");
  if (!expression) {
    std::move(callback).Run(MakeErrorResult("expression이 필요합니다."));
    return;
  }

  std::optional<bool> await_promise = params.FindBool("awaitPromise");

  // Runtime.evaluate CDP 명령 실행.
  // 핵심: Runtime.enable을 보내지 않고 evaluate만 직접 호출.
  // 외부 CDP에서는 불가능하지만 내부 세션에서는 가능.
  // 이를 통해 "Runtime.enable" 탐지 신호 회피.
  base::DictValue cdp_params;
  cdp_params.Set("expression", *expression);
  cdp_params.Set("returnByValue", true);
  cdp_params.Set("awaitPromise", await_promise.value_or(true));

  session->SendCdpCommand(
      "Runtime.evaluate",
      std::move(cdp_params),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb,
             std::optional<base::DictValue> result,
             const std::string& error) {
            if (!error.empty()) {
              std::move(cb).Run(MakeErrorResult("실행 실패: " + error));
              return;
            }
            if (!result) {
              std::move(cb).Run(MakeTextResult("undefined"));
              return;
            }
            // CDP 결과를 JSON 문자열로 반환
            std::string json_output;
            base::JSONWriter::Write(base::Value(result->Clone()), &json_output);
            std::move(cb).Run(MakeTextResult(json_output));
          },
          std::move(callback)));
}

void McpServer::ExecuteNetworkCapture(
    const base::DictValue& params,
    base::OnceCallback<void(base::Value)> callback) {
  McpSession* session = GetActiveSession();
  if (!session) {
    std::move(callback).Run(MakeErrorResult("활성 탭 세션이 없습니다."));
    return;
  }

  const std::string* action = params.FindString("action");
  if (!action) {
    std::move(callback).Run(MakeErrorResult("action이 필요합니다."));
    return;
  }

  if (*action == "start") {
    // Network.enable: 네트워크 이벤트 수신 시작.
    // 내부 CDP 세션이므로 노란 디버그 배너가 표시되지 않음.
    base::DictValue cdp_params;
    // maxTotalBufferSize: 네트워크 버퍼 크기 (10MB)
    cdp_params.Set("maxTotalBufferSize", 10 * 1024 * 1024);
    cdp_params.Set("maxResourceBufferSize", 5 * 1024 * 1024);

    session->SendCdpCommand(
        "Network.enable",
        std::move(cdp_params),
        base::BindOnce(
            [](base::OnceCallback<void(base::Value)> cb,
               std::optional<base::DictValue>,
               const std::string& error) {
              if (!error.empty()) {
                std::move(cb).Run(
                    MakeErrorResult("네트워크 캡처 시작 실패: " + error));
              } else {
                std::move(cb).Run(MakeTextResult("네트워크 캡처 시작됨"));
              }
            },
            std::move(callback)));
  } else if (*action == "stop") {
    // Network.disable: 네트워크 이벤트 수신 중지
    session->SendCdpCommand(
        "Network.disable",
        base::DictValue(),
        base::BindOnce(
            [](base::OnceCallback<void(base::Value)> cb,
               std::optional<base::DictValue>,
               const std::string& error) {
              if (!error.empty()) {
                std::move(cb).Run(
                    MakeErrorResult("네트워크 캡처 중지 실패: " + error));
              } else {
                std::move(cb).Run(MakeTextResult("네트워크 캡처 중지됨"));
              }
            },
            std::move(callback)));
  } else {
    std::move(callback).Run(
        MakeErrorResult("알 수 없는 action: " + *action));
  }
}

void McpServer::ExecuteNetworkRequests(
    const base::DictValue& params,
    base::OnceCallback<void(base::Value)> callback) {
  McpSession* session = GetActiveSession();
  if (!session) {
    std::move(callback).Run(MakeErrorResult("활성 탭 세션이 없습니다."));
    return;
  }

  // McpSession에서 버퍼링된 네트워크 요청 목록 반환
  bool include_static = params.FindBool("includeStatic").value_or(false);
  base::Value requests_value = session->GetCapturedNetworkRequests(include_static);

  std::string json_output;
  base::JSONWriter::Write(requests_value, &json_output);
  std::move(callback).Run(MakeTextResult(json_output));
}

void McpServer::ExecuteTabs(const base::DictValue& params,
                             base::OnceCallback<void(base::Value)> callback) {
  // 탭 관리는 TabStripModel API를 직접 사용하므로 CDP 불필요.
  // 실제 구현에서는 TabStripModel* 참조가 필요함.
  const std::string* action = params.FindString("action");
  if (!action) {
    std::move(callback).Run(MakeErrorResult("action이 필요합니다."));
    return;
  }

  if (*action == "list") {
    // 현재 열린 모든 WebContents 목록 반환
    base::ListValue tabs_list;

    // DevToolsAgentHost::GetAll()로 모든 열린 탭의 에이전트 호스트 조회
    content::DevToolsAgentHost::List all_hosts =
        content::DevToolsAgentHost::GetAll();

    int tab_id = 0;
    for (const auto& host : all_hosts) {
      if (host->GetType() != content::DevToolsAgentHost::kTypePage) {
        continue;
      }
      base::DictValue tab_info;
      tab_info.Set("id", tab_id++);
      tab_info.Set("url", host->GetURL().spec());
      tab_info.Set("title", host->GetTitle());
      tabs_list.Append(std::move(tab_info));
    }

    base::DictValue result;
    result.Set("tabs", std::move(tabs_list));
    std::move(callback).Run(base::Value(std::move(result)));

  } else if (*action == "new") {
    // 새 탭 생성 (실제 구현: TabStripModel::AddWebContents)
    // TODO: BrowserList 또는 TabStripModel에 접근하여 새 탭 생성
    std::move(callback).Run(MakeTextResult("새 탭 생성됨 (구현 예정)"));

  } else if (*action == "close") {
    // 탭 닫기 (실제 구현: WebContents::Close)
    std::move(callback).Run(MakeTextResult("탭 닫음 (구현 예정)"));

  } else if (*action == "select") {
    // 탭 선택 (실제 구현: Browser::ActivateTabAt)
    std::move(callback).Run(MakeTextResult("탭 선택됨 (구현 예정)"));

  } else {
    std::move(callback).Run(
        MakeErrorResult("알 수 없는 action: " + *action));
  }
}

void McpServer::ExecuteBrowserInfo(
    const base::DictValue& params,
    base::OnceCallback<void(base::Value)> callback) {
  base::DictValue info;

  // 브라우저 버전 정보
  // 실제 구현: version_info::GetVersionNumber()
  info.Set("browserName", "Chromium-MCP");
  info.Set("browserVersion", "130.0.0");  // 빌드 시 실제 버전으로 교체

  // 활성 탭 정보
  if (active_web_contents_) {
    auto it = sessions_.find(active_web_contents_);
    if (it != sessions_.end()) {
      // 실제 구현: web_contents->GetVisibleURL().spec()
      info.Set("activeTabUrl", "https://example.com");
      info.Set("activeTabTitle", "Active Tab");
    }
  }

  // 열린 탭 수: DevToolsAgentHost::GetAll()로 조회
  content::DevToolsAgentHost::List all_hosts =
      content::DevToolsAgentHost::GetAll();
  int page_count = 0;
  for (const auto& host : all_hosts) {
    if (host->GetType() == content::DevToolsAgentHost::kTypePage) {
      ++page_count;
    }
  }
  info.Set("tabCount", page_count);
  info.Set("mcpServerName", kMcpServerName);
  info.Set("mcpProtocolVersion", kMcpProtocolVersion);

  std::string json_output;
  base::JSONWriter::Write(base::Value(info.Clone()), &json_output);
  std::move(callback).Run(MakeTextResult(json_output));
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
