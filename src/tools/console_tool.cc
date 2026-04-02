// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/console_tool.h"

#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

// -----------------------------------------------------------------------
// CDP 이벤트 이름 상수
// -----------------------------------------------------------------------

// Log 도메인 이벤트 — 안전 모드에서 사용 (Runtime.enable 불필요)
constexpr char kLogEntryAddedEvent[] = "Log.entryAdded";

// Runtime 도메인 이벤트 — 전체 모드에서 사용 (Runtime.enable 필요)
// 주의: Runtime.enable은 DevTools 감지 위험이 있으므로 명시적 요청 시에만 사용.
constexpr char kConsoleApiCalledEvent[] = "Runtime.consoleAPICalled";

// -----------------------------------------------------------------------
// 생성자 / 소멸자
// -----------------------------------------------------------------------

ConsoleTool::ConsoleTool() = default;
ConsoleTool::~ConsoleTool() = default;

// -----------------------------------------------------------------------
// McpTool 인터페이스
// -----------------------------------------------------------------------

std::string ConsoleTool::name() const {
  return "console";
}

std::string ConsoleTool::description() const {
  return "브라우저 콘솔 메시지 캡처 및 조회. "
         "안전 모드(Log.enable)와 전체 모드(Runtime.consoleAPICalled) 지원.";
}

base::DictValue ConsoleTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: 수행할 동작
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue enums;
    enums.Append("start");
    enums.Append("stop");
    enums.Append("get");
    enums.Append("clear");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "수행할 동작: start=캡처 시작, stop=캡처 중지, "
             "get=메시지 조회, clear=버퍼 초기화");
    properties.Set("action", std::move(prop));
  }

  // level: 메시지 레벨 필터 (get 동작 시 사용, 기본값 all)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue enums;
    enums.Append("all");
    enums.Append("log");
    enums.Append("info");
    enums.Append("warn");
    enums.Append("error");
    enums.Append("debug");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "필터링할 메시지 레벨 (기본값: all). get 동작에서만 사용됨");
    prop.Set("default", "all");
    properties.Set("level", std::move(prop));
  }

  // pattern: 메시지 텍스트 필터링 정규식
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "메시지 텍스트를 필터링할 정규식 (get 동작에서만 사용됨)");
    properties.Set("pattern", std::move(prop));
  }

  // limit: 최대 반환 메시지 수
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description",
             "최대 반환 메시지 수 (get 동작에서만 사용됨, 기본값: 제한 없음)");
    properties.Set("limit", std::move(prop));
  }

  // safeMode: 안전 모드 여부 (start 동작 시 사용, 기본값 true)
  {
    base::DictValue prop;
    prop.Set("type", "boolean");
    prop.Set("description",
             "안전 모드 사용 여부 (기본값: true). "
             "true=Log.enable만 사용(은닉 안전), "
             "false=Runtime.enable+Runtime.consoleAPICalled(은닉 해제 감수)");
    prop.Set("default", true);
    properties.Set("safeMode", std::move(prop));
  }

  schema.Set("properties", std::move(properties));

  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void ConsoleTool::Execute(const base::DictValue& arguments,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback) {
  const std::string* action_ptr = arguments.FindString("action");
  if (!action_ptr || action_ptr->empty()) {
    std::move(callback).Run(MakeErrorResult("action 파라미터가 필요합니다 (start/stop/get/clear)"));
    return;
  }
  const std::string& action = *action_ptr;

  if (action == "start") {
    // safeMode 파라미터 읽기 (기본값 true — Log.enable 사용)
    std::optional<bool> safe_mode_opt = arguments.FindBool("safeMode");
    bool use_safe_mode = safe_mode_opt.value_or(true);

    if (use_safe_mode) {
      StartSafeMode(session, std::move(callback));
    } else {
      StartFullMode(session, std::move(callback));
    }

  } else if (action == "stop") {
    StopCapture(session, std::move(callback));

  } else if (action == "get") {
    const std::string* level_ptr = arguments.FindString("level");
    std::string level_filter = level_ptr ? *level_ptr : "all";

    const std::string* pattern_ptr = arguments.FindString("pattern");
    std::string pattern_filter = pattern_ptr ? *pattern_ptr : "";

    std::optional<int> limit_opt = arguments.FindInt("limit");
    int limit = limit_opt.value_or(0);  // 0 = 제한 없음

    GetMessages(level_filter, pattern_filter, limit, std::move(callback));

  } else if (action == "clear") {
    ClearMessages(std::move(callback));

  } else {
    std::move(callback).Run(MakeErrorResult("유효하지 않은 action: " + action));
  }
}

// -----------------------------------------------------------------------
// 캡처 시작 — 안전 모드 (Log.enable + Log.entryAdded)
// -----------------------------------------------------------------------

void ConsoleTool::StartSafeMode(McpSession* session,
                                 base::OnceCallback<void(base::Value)> callback) {
  if (is_capturing_) {
    base::DictValue result;
    result.Set("success", true);
    result.Set("message", "이미 콘솔 캡처가 진행 중입니다");
    result.Set("mode", safe_mode_ ? "safe" : "full");
    std::move(callback).Run(MakeJsonResult(std::move(result)));
    return;
  }

  safe_mode_ = true;
  LOG(INFO) << "[ConsoleTool] 안전 모드 시작: Log.enable 호출";

  // Log.enable: Runtime.enable 없이 브라우저 콘솔 메시지를 수신할 수 있다.
  // 이 도메인은 자동화 탐지에 영향을 주지 않는다.
  session->SendCdpCommand(
      "Log.enable", base::DictValue(),
      base::BindOnce(&ConsoleTool::OnLogEnabled,
                     weak_factory_.GetWeakPtr(), session,
                     std::move(callback)));
}

void ConsoleTool::OnLogEnabled(McpSession* session,
                                base::OnceCallback<void(base::Value)> callback,
                                base::Value response) {
  // CDP 오류 확인
  if (response.is_dict()) {
    const base::DictValue* err = response.GetDict().FindDict("error");
    if (err) {
      const std::string* msg = err->FindString("message");
      std::string err_msg = msg ? *msg : "Log.enable 실패";
      LOG(ERROR) << "[ConsoleTool] Log.enable 오류: " << err_msg;
      std::move(callback).Run(MakeErrorResult(err_msg));
      return;
    }
  }

  // Log.entryAdded 이벤트 핸들러 등록
  // 이 이벤트는 console.* 및 브라우저 내부 경고 메시지를 모두 전달한다.
  session->RegisterCdpEventHandler(
      kLogEntryAddedEvent,
      base::BindRepeating(&ConsoleTool::OnLogEntryAdded,
                          weak_factory_.GetWeakPtr()));

  is_capturing_ = true;
  LOG(INFO) << "[ConsoleTool] 안전 모드 캡처 시작됨 (Log.entryAdded 수신 중)";

  base::DictValue result;
  result.Set("success", true);
  result.Set("message", "콘솔 캡처를 시작했습니다 (안전 모드: Log.entryAdded)");
  result.Set("mode", "safe");
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

void ConsoleTool::OnLogEntryAdded(const std::string& event_name,
                                   const base::DictValue& params) {
  // Log.entryAdded 이벤트 파라미터:
  //   entry: {
  //     source: "javascript" | "network" | "other" | ...
  //     level: "verbose" | "info" | "warning" | "error"
  //     text: 메시지 텍스트
  //     timestamp: Unix 타임스탬프 (초)
  //     url: 소스 URL (선택적)
  //     lineNumber: 줄 번호 (선택적)
  //     stackTrace: 호출 스택 (선택적)
  //   }

  const base::DictValue* entry = params.FindDict("entry");
  if (!entry) return;

  const std::string* level_ptr = entry->FindString("level");
  const std::string* text_ptr  = entry->FindString("text");
  const std::string* url_ptr   = entry->FindString("url");

  std::string level = level_ptr ? *level_ptr : "info";
  std::string text  = text_ptr  ? *text_ptr  : "";

  // Log 도메인의 레벨은 "verbose"/"info"/"warning"/"error".
  // 내부 버퍼에서는 "log"/"info"/"warn"/"error"/"debug"로 정규화한다.
  if (level == "verbose") level = "debug";
  if (level == "warning")  level = "warn";

  std::optional<double> timestamp = entry->FindDouble("timestamp");
  std::optional<int>    line_num  = entry->FindInt("lineNumber");

  base::DictValue msg_entry;
  msg_entry.Set("level", level);
  msg_entry.Set("text", text);
  msg_entry.Set("source", "log_domain");
  if (timestamp.has_value()) msg_entry.Set("timestamp", *timestamp);
  if (url_ptr)               msg_entry.Set("url", *url_ptr);
  if (line_num.has_value())  msg_entry.Set("lineNumber", *line_num);

  messages_.push_back(std::move(msg_entry));
  VLOG(1) << "[ConsoleTool] Log.entryAdded 수신: level=" << level
          << " text=" << text;
}

// -----------------------------------------------------------------------
// 캡처 시작 — 전체 모드 (Runtime.enable + Runtime.consoleAPICalled)
// -----------------------------------------------------------------------

void ConsoleTool::StartFullMode(McpSession* session,
                                 base::OnceCallback<void(base::Value)> callback) {
  if (is_capturing_) {
    base::DictValue result;
    result.Set("success", true);
    result.Set("message", "이미 콘솔 캡처가 진행 중입니다");
    result.Set("mode", safe_mode_ ? "safe" : "full");
    std::move(callback).Run(MakeJsonResult(std::move(result)));
    return;
  }

  safe_mode_ = false;
  LOG(INFO) << "[ConsoleTool] 전체 모드 시작: Runtime.enable 호출"
            << " (주의: DevTools 감지 위험이 있습니다)";

  // Runtime.enable: 실행 컨텍스트 이벤트 및 콘솔 API 이벤트를 활성화한다.
  // executionContextCreated 이벤트가 발생하여 일부 봇 탐지 스크립트에 감지될 수 있다.
  // 은닉이 필요한 경우 StartSafeMode()를 사용할 것.
  session->SendCdpCommand(
      "Runtime.enable", base::DictValue(),
      base::BindOnce(&ConsoleTool::OnRuntimeEnabled,
                     weak_factory_.GetWeakPtr(), session,
                     std::move(callback)));
}

void ConsoleTool::OnRuntimeEnabled(McpSession* session,
                                    base::OnceCallback<void(base::Value)> callback,
                                    base::Value response) {
  if (response.is_dict()) {
    const base::DictValue* err = response.GetDict().FindDict("error");
    if (err) {
      const std::string* msg = err->FindString("message");
      std::string err_msg = msg ? *msg : "Runtime.enable 실패";
      LOG(ERROR) << "[ConsoleTool] Runtime.enable 오류: " << err_msg;
      std::move(callback).Run(MakeErrorResult(err_msg));
      return;
    }
  }

  // Runtime.consoleAPICalled 이벤트 핸들러 등록
  session->RegisterCdpEventHandler(
      kConsoleApiCalledEvent,
      base::BindRepeating(&ConsoleTool::OnConsoleApiCalled,
                          weak_factory_.GetWeakPtr()));

  is_capturing_ = true;
  LOG(INFO) << "[ConsoleTool] 전체 모드 캡처 시작됨 (Runtime.consoleAPICalled 수신 중)";

  base::DictValue result;
  result.Set("success", true);
  result.Set("message",
             "콘솔 캡처를 시작했습니다 (전체 모드: Runtime.consoleAPICalled). "
             "주의: DevTools 감지 위험이 있습니다.");
  result.Set("mode", "full");
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

void ConsoleTool::OnConsoleApiCalled(const std::string& event_name,
                                      const base::DictValue& params) {
  // Runtime.consoleAPICalled 이벤트 파라미터:
  //   type: "log" | "debug" | "info" | "warning" | "error" | "dir" | ...
  //   args: RemoteObject[] — 각 인수의 상세 정보
  //   timestamp: 타임스탬프
  //   stackTrace: 호출 스택 (선택적)

  const std::string* type_ptr = params.FindString("type");
  std::string level = type_ptr ? *type_ptr : "log";

  // Runtime 도메인의 "warning"을 "warn"으로 정규화
  if (level == "warning") level = "warn";

  // args 배열에서 메시지 텍스트 조합
  std::string text;
  const base::ListValue* args = params.FindList("args");
  if (args) {
    text = ExtractTextFromArgs(*args);
  }

  std::optional<double> timestamp = params.FindDouble("timestamp");

  // stackTrace에서 첫 번째 호출 위치 추출
  std::string call_url;
  std::optional<int> line_num;
  const base::DictValue* stack = params.FindDict("stackTrace");
  if (stack) {
    const base::ListValue* frames = stack->FindList("callFrames");
    if (frames && !frames->empty()) {
      const auto& first = (*frames)[0];
      if (first.is_dict()) {
        const base::DictValue& f = first.GetDict();
        const std::string* url = f.FindString("url");
        if (url) call_url = *url;
        line_num = f.FindInt("lineNumber");
      }
    }
  }

  base::DictValue msg_entry;
  msg_entry.Set("level", level);
  msg_entry.Set("text", text);
  msg_entry.Set("source", "runtime_domain");
  if (timestamp.has_value()) msg_entry.Set("timestamp", *timestamp);
  if (!call_url.empty())     msg_entry.Set("url", call_url);
  if (line_num.has_value())  msg_entry.Set("lineNumber", *line_num);

  messages_.push_back(std::move(msg_entry));
  VLOG(1) << "[ConsoleTool] Runtime.consoleAPICalled 수신: level=" << level
          << " text=" << text;
}

// -----------------------------------------------------------------------
// 캡처 중지
// -----------------------------------------------------------------------

void ConsoleTool::StopCapture(McpSession* session,
                               base::OnceCallback<void(base::Value)> callback) {
  if (!is_capturing_) {
    base::DictValue result;
    result.Set("success", true);
    result.Set("message", "캡처가 이미 중지되어 있습니다");
    std::move(callback).Run(MakeJsonResult(std::move(result)));
    return;
  }

  // 등록된 이벤트 핸들러 해제
  if (safe_mode_) {
    session->UnregisterCdpEventHandler(kLogEntryAddedEvent);
  } else {
    session->UnregisterCdpEventHandler(kConsoleApiCalledEvent);
  }

  is_capturing_ = false;
  LOG(INFO) << "[ConsoleTool] 캡처 중지됨, 저장된 메시지 수: "
            << messages_.size();

  base::DictValue result;
  result.Set("success", true);
  result.Set("message", "콘솔 캡처를 중지했습니다");
  result.Set("messageCount", static_cast<int>(messages_.size()));
  result.Set("mode", safe_mode_ ? "safe" : "full");
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

// -----------------------------------------------------------------------
// 메시지 조회
// -----------------------------------------------------------------------

void ConsoleTool::GetMessages(const std::string& level_filter,
                               const std::string& pattern_filter,
                               int limit,
                               base::OnceCallback<void(base::Value)> callback) {
  // 패턴 필터 확인 (단순 부분 문자열 매칭)
  const bool has_filter = !pattern_filter.empty();

  base::ListValue filtered;

  for (const auto& msg : messages_) {
    // 레벨 필터
    const std::string* lv = msg.FindString("level");
    if (!MatchesLevel(lv ? *lv : "", level_filter)) continue;

    // 패턴 필터 (부분 문자열 매칭)
    if (has_filter) {
      const std::string* text = msg.FindString("text");
      std::string msg_text = text ? *text : "";
      if (msg_text.find(pattern_filter) == std::string::npos) continue;
    }

    filtered.Append(msg.Clone());

    // limit 적용 (0이면 제한 없음)
    if (limit > 0 && static_cast<int>(filtered.size()) >= limit) break;
  }

  LOG(INFO) << "[ConsoleTool] 메시지 조회: 전체=" << messages_.size()
            << " 필터 후=" << filtered.size()
            << " level=" << level_filter
            << " pattern=" << pattern_filter
            << " limit=" << limit;

  base::DictValue result;
  result.Set("success", true);
  result.Set("messages", std::move(filtered));
  result.Set("totalCount", static_cast<int>(messages_.size()));
  result.Set("isCapturing", is_capturing_);
  result.Set("mode", safe_mode_ ? "safe" : "full");
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

// -----------------------------------------------------------------------
// 버퍼 초기화
// -----------------------------------------------------------------------

void ConsoleTool::ClearMessages(
    base::OnceCallback<void(base::Value)> callback) {
  int cleared = static_cast<int>(messages_.size());
  messages_.clear();

  LOG(INFO) << "[ConsoleTool] 메시지 버퍼 초기화: " << cleared << "개 삭제";

  base::DictValue result;
  result.Set("success", true);
  result.Set("message", "콘솔 메시지 버퍼를 초기화했습니다");
  result.Set("clearedCount", cleared);
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

// -----------------------------------------------------------------------
// 정적 헬퍼
// -----------------------------------------------------------------------

// static
std::string ConsoleTool::ExtractTextFromArgs(const base::ListValue& args) {
  std::string result;
  for (const auto& arg : args) {
    if (!arg.is_dict()) continue;
    const base::DictValue& d = arg.GetDict();

    if (!result.empty()) result += " ";

    // value 필드 우선 (primitive 값)
    const base::Value* val = d.Find("value");
    if (val) {
      if (val->is_string()) {
        result += val->GetString();
      } else if (val->is_int()) {
        result += std::to_string(val->GetInt());
      } else if (val->is_double()) {
        result += std::to_string(val->GetDouble());
      } else if (val->is_bool()) {
        result += val->GetBool() ? "true" : "false";
      }
      continue;
    }

    // description 필드 차선 (객체, 배열 등의 문자열 표현)
    const std::string* desc = d.FindString("description");
    if (desc) {
      result += *desc;
    }
  }
  return result;
}

// static
bool ConsoleTool::MatchesLevel(const std::string& msg_level,
                                const std::string& level_filter) {
  if (level_filter == "all" || level_filter.empty()) return true;

  // 정규화된 레벨끼리 비교
  if (msg_level == level_filter) return true;

  // "log" 타입은 "info" 필터에도 포함시킨다.
  if (level_filter == "info" && msg_level == "log") return true;

  return false;
}

}  // namespace mcp
