// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/javascript_tool.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"  // McpSession::SendCdpCommand

namespace mcp {

namespace {

// MCP 성공 응답 Value 생성.
base::Value MakeSuccessResult(const std::string& text) {
  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict item;
  item.Set("type", "text");
  item.Set("text", text);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", false);
  return base::Value(std::move(result));
}

// MCP 에러 응답 Value 생성.
base::Value MakeErrorResult(const std::string& text) {
  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict item;
  item.Set("type", "text");
  item.Set("text", text);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", true);
  return base::Value(std::move(result));
}

// base::Value를 JSON 문자열로 직렬화한다.
// 직렬화 실패 시 "<직렬화 불가>" 반환.
std::string ValueToJson(const base::Value& value) {
  std::string json;
  if (!base::JSONWriter::Write(value, &json)) {
    return "<직렬화 불가>";
  }
  return json;
}

// Runtime.evaluate 결과 객체(result.result RemoteObject)를
// 사람이 읽기 좋은 문자열로 변환한다.
//
// CDP RemoteObject 필드 우선순위:
//   1. value       (returnByValue=true 일 때 실제 값)
//   2. description (객체·함수·에러 등의 문자열 표현)
//   3. type        (값이 없을 때 타입 이름만 반환)
std::string RemoteObjectToString(const base::Value::Dict& remote_obj) {
  // 1. 원시값(primitive) 처리: returnByValue=true면 value 키에 실제 값이 있다.
  const base::Value* value = remote_obj.Find("value");
  if (value) {
    if (value->is_string()) {
      return value->GetString();
    }
    if (value->is_int()) {
      return std::to_string(value->GetInt());
    }
    if (value->is_double()) {
      // 정수처럼 보이면 소수점 없이 출력
      double d = value->GetDouble();
      if (d == static_cast<long long>(d)) {
        return std::to_string(static_cast<long long>(d));
      }
      return std::to_string(d);
    }
    if (value->is_bool()) {
      return value->GetBool() ? "true" : "false";
    }
    if (value->is_none()) {
      // null 또는 undefined
      const std::string* type = remote_obj.FindString("type");
      return (type && *type == "undefined") ? "undefined" : "null";
    }
    // Dict/List → JSON 직렬화
    return ValueToJson(*value);
  }

  // 2. description 필드 (객체, 함수, Promise 등)
  const std::string* desc = remote_obj.FindString("description");
  if (desc && !desc->empty()) {
    return *desc;
  }

  // 3. 타입만 반환
  const std::string* type = remote_obj.FindString("type");
  if (type) {
    return "<" + *type + ">";
  }

  return "<알 수 없는 결과>";
}

// Runtime.evaluate 응답에서 예외 정보를 추출한다.
// exceptionDetails가 없으면 빈 문자열 반환.
std::string ExtractExceptionMessage(const base::Value::Dict& result_dict) {
  const base::Value::Dict* exception = result_dict.FindDict("exceptionDetails");
  if (!exception) {
    return "";
  }

  // exceptionDetails.text: 간략한 예외 설명
  const std::string* text = exception->FindString("text");

  // exceptionDetails.exception: RemoteObject (상세 에러 객체)
  const base::Value::Dict* exc_obj = exception->FindDict("exception");
  std::string exc_desc;
  if (exc_obj) {
    const std::string* desc = exc_obj->FindString("description");
    if (desc) {
      exc_desc = *desc;
    }
  }

  if (!exc_desc.empty()) {
    return exc_desc;  // 스택 트레이스 포함된 상세 설명 우선
  }
  if (text && !text->empty()) {
    return *text;
  }
  return "JavaScript 예외 발생 (상세 정보 없음)";
}

}  // namespace

JavaScriptTool::JavaScriptTool() = default;
JavaScriptTool::~JavaScriptTool() = default;

std::string JavaScriptTool::name() const {
  return "evaluate";
}

std::string JavaScriptTool::description() const {
  return "현재 페이지의 JavaScript 컨텍스트에서 코드를 실행하고 결과를 반환합니다. "
         "awaitPromise=true이면 Promise가 완료될 때까지 대기합니다. "
         "isolatedWorld=true이면 페이지 JS에서 감지할 수 없는 격리된 컨텍스트에서 실행합니다.";
}

base::Value::Dict JavaScriptTool::input_schema() const {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // expression: 실행할 JS 코드
  base::Value::Dict expr_prop;
  expr_prop.Set("type", "string");
  expr_prop.Set("description", "실행할 JavaScript 표현식 또는 코드 블록");
  properties.Set("expression", std::move(expr_prop));

  // awaitPromise: Promise 결과 대기 여부
  base::Value::Dict await_prop;
  await_prop.Set("type", "boolean");
  await_prop.Set("default", true);
  await_prop.Set("description",
                 "true이면 expression이 Promise를 반환할 때 이행(resolve) 값을 "
                 "기다렸다가 반환한다. false이면 Promise 객체 자체를 반환한다.");
  properties.Set("awaitPromise", std::move(await_prop));

  // isolatedWorld: 격리된 컨텍스트에서 실행 여부
  base::Value::Dict isolated_prop;
  isolated_prop.Set("type", "boolean");
  isolated_prop.Set("default", false);
  isolated_prop.Set("description",
                    "true이면 Page.createIsolatedWorld로 별도 실행 컨텍스트를 생성하여 "
                    "페이지 JavaScript에서 실행 흔적을 감지할 수 없게 한다. "
                    "false이면 페이지 메인 월드(window 공유)에서 실행한다.");
  properties.Set("isolatedWorld", std::move(isolated_prop));

  schema.Set("properties", std::move(properties));

  // expression은 필수
  base::Value::List required;
  required.Append("expression");
  schema.Set("required", std::move(required));

  return schema;
}

void JavaScriptTool::Execute(
    const base::Value::Dict& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* expression = arguments.FindString("expression");
  if (!expression || expression->empty()) {
    LOG(WARNING) << "[JavaScriptTool] expression 파라미터가 누락되었습니다.";
    std::move(callback).Run(MakeErrorResult("expression 파라미터가 필요합니다."));
    return;
  }

  // awaitPromise 기본값: true
  std::optional<bool> await_opt = arguments.FindBool("awaitPromise");
  bool await_promise = await_opt.value_or(true);

  // isolatedWorld 기본값: false
  std::optional<bool> isolated_opt = arguments.FindBool("isolatedWorld");
  bool isolated_world = isolated_opt.value_or(false);

  LOG(INFO) << "[JavaScriptTool] 실행 모드: "
            << (isolated_world ? "IsolatedWorld" : "MainWorld")
            << " awaitPromise=" << await_promise
            << " expression length=" << expression->size();

  if (isolated_world) {
    // Isolated World 모드: 먼저 Page.getFrameTree로 메인 프레임 ID 획득
    GetFrameTreeForIsolatedWorld(*expression, await_promise, session,
                                 std::move(callback));
  } else {
    EvaluateInMainWorld(*expression, await_promise, session, std::move(callback));
  }
}

// 모드 A: 메인 월드에서 Runtime.evaluate 직접 호출.
//
// ★ Runtime.enable을 호출하지 않음 ★
// Runtime.evaluate는 런타임 도메인이 활성화되지 않아도 호출 가능하다.
// Runtime.enable을 호출하면 executionContextCreated 등 이벤트가 발생하여
// 안티봇 스크립트가 DevTools 연결을 탐지할 수 있다.
void JavaScriptTool::EvaluateInMainWorld(
    const std::string& expression,
    bool await_promise,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::Value::Dict params;
  params.Set("expression", expression);

  // returnByValue: 결과를 JSON 직렬화 가능한 값으로 받는다.
  // false이면 RemoteObjectId(참조)만 반환되어 후속 호출이 필요하다.
  params.Set("returnByValue", true);

  // awaitPromise: expression이 Promise를 반환할 경우 완료 대기.
  params.Set("awaitPromise", await_promise);

  // userGesture: true로 설정하면 click() 등 사용자 제스처가 필요한 API를
  // 스크립트에서도 호출할 수 있다.
  params.Set("userGesture", true);

  // ★ contextId를 지정하지 않으면 현재 페이지의 기본 컨텍스트(메인 월드)에서 실행된다.
  // Runtime.enable 없이도 기본 컨텍스트 ID(1)는 항상 유효하다.

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&JavaScriptTool::OnEvaluateResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// 모드 B Step 1: Page.getFrameTree 로 메인 프레임 ID를 획득한다.
//
// McpSession 공개 API에는 GetMainFrameId()가 없으므로,
// Page.getFrameTree CDP 명령으로 프레임 트리를 조회하여
// 루트 프레임(메인 프레임)의 id를 얻는다.
void JavaScriptTool::GetFrameTreeForIsolatedWorld(
    const std::string& expression,
    bool await_promise,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  session->SendCdpCommand(
      "Page.getFrameTree", base::Value::Dict(),
      base::BindOnce(&JavaScriptTool::OnGetFrameTree,
                     weak_factory_.GetWeakPtr(),
                     expression, await_promise,
                     session, std::move(callback)));
}

// 모드 B Step 2: getFrameTree 응답에서 frameId 추출 후
//               Page.createIsolatedWorld 호출.
//
// createIsolatedWorld는 content script와 동일한 격리 수준의 실행 컨텍스트를 만든다.
// 이 컨텍스트에서 실행된 코드:
//   - DOM에는 접근 가능 (document, window 등)
//   - 페이지 JS의 글로벌 변수/프로토타입 오염에 영향받지 않음
//   - 페이지 JS가 이 컨텍스트의 실행을 직접 관찰 불가
void JavaScriptTool::OnGetFrameTree(
    const std::string& expression,
    bool await_promise,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict || dict->Find("error") != nullptr) {
    LOG(ERROR) << "[JavaScriptTool] Page.getFrameTree 실패";
    std::move(callback).Run(MakeErrorResult("메인 프레임 ID를 획득할 수 없습니다."));
    return;
  }

  // 응답 구조: result.frameTree.frame.id
  const base::Value::Dict* result = dict->FindDict("result");
  const base::Value::Dict* frame_tree = result ? result->FindDict("frameTree") : nullptr;
  const base::Value::Dict* frame = frame_tree ? frame_tree->FindDict("frame") : nullptr;
  const std::string* frame_id = frame ? frame->FindString("id") : nullptr;

  if (!frame_id || frame_id->empty()) {
    LOG(ERROR) << "[JavaScriptTool] frameTree에서 메인 프레임 ID를 찾을 수 없음";
    std::move(callback).Run(MakeErrorResult("메인 프레임 ID를 찾을 수 없습니다."));
    return;
  }

  LOG(INFO) << "[JavaScriptTool] 메인 프레임 ID: " << *frame_id;

  base::Value::Dict params;
  params.Set("frameId", *frame_id);

  // worldName: DevTools에서 표시되는 컨텍스트 이름. 빈 문자열로 익명 처리.
  params.Set("worldName", "");

  // grantUniversalAccess: true이면 CSP(Content Security Policy) 우회 가능.
  // 보안상 false를 기본값으로 사용한다.
  params.Set("grantUniversalAccess", false);

  session->SendCdpCommand(
      "Page.createIsolatedWorld", std::move(params),
      base::BindOnce(&JavaScriptTool::OnIsolatedWorldCreated,
                     weak_factory_.GetWeakPtr(),
                     expression, await_promise,
                     session, std::move(callback)));
}

// 모드 B Step 2: executionContextId로 Runtime.evaluate 호출.
void JavaScriptTool::OnIsolatedWorldCreated(
    const std::string& expression,
    bool await_promise,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // CDP 전송 레벨 에러 확인
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict || dict->Find("error") != nullptr) {
    std::string error_msg = "Page.createIsolatedWorld 실패";
    if (dict) {
      const base::Value::Dict* err = dict->FindDict("error");
      const std::string* msg = err ? err->FindString("message") : nullptr;
      if (msg) {
        error_msg += ": " + *msg;
      }
    }
    LOG(ERROR) << "[JavaScriptTool] " << error_msg;
    std::move(callback).Run(MakeErrorResult(error_msg));
    return;
  }

  // result.executionContextId 추출
  const base::Value::Dict* result = dict->FindDict("result");
  std::optional<int> context_id =
      result ? result->FindInt("executionContextId") : std::nullopt;

  if (!context_id.has_value() || *context_id <= 0) {
    LOG(ERROR) << "[JavaScriptTool] IsolatedWorld executionContextId를 획득할 수 없음";
    std::move(callback).Run(
        MakeErrorResult("Isolated World 실행 컨텍스트 ID를 획득할 수 없습니다."));
    return;
  }

  LOG(INFO) << "[JavaScriptTool] IsolatedWorld contextId=" << *context_id;

  // ★ Runtime.enable 없이 특정 contextId에 Runtime.evaluate 직접 호출
  base::Value::Dict params;
  params.Set("expression", expression);
  params.Set("returnByValue", true);
  params.Set("awaitPromise", await_promise);
  params.Set("userGesture", true);

  // contextId를 명시하면 해당 Isolated World 컨텍스트에서만 실행된다.
  params.Set("contextId", *context_id);

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&JavaScriptTool::OnEvaluateResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// Runtime.evaluate 응답 처리.
//
// CDP Runtime.evaluate 응답 구조:
//   {
//     "result": {               // 항상 존재 (RemoteObject)
//       "type": "string"|"number"|"boolean"|"object"|"undefined"|"function",
//       "value": <원시값>,      // returnByValue=true & 원시값일 때
//       "description": "..."   // 객체/함수/에러 등의 문자열 표현
//     },
//     "exceptionDetails": {    // JavaScript 예외 발생 시에만 존재
//       "text": "...",
//       "exception": { RemoteObject }
//     }
//   }
void JavaScriptTool::OnEvaluateResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  const base::Value::Dict* dict = response.GetIfDict();
  if (!dict) {
    LOG(ERROR) << "[JavaScriptTool] Runtime.evaluate 응답이 Dict 형식이 아님";
    std::move(callback).Run(MakeErrorResult("Runtime.evaluate 응답 형식 오류"));
    return;
  }

  // CDP 전송 레벨 에러 (프로토콜 에러, 타임아웃 등)
  if (dict->Find("error") != nullptr) {
    const base::Value::Dict* err = dict->FindDict("error");
    const std::string* msg = err ? err->FindString("message") : nullptr;
    std::string error_str = msg ? *msg : "알 수 없는 CDP 에러";
    LOG(ERROR) << "[JavaScriptTool] CDP 에러: " << error_str;
    std::move(callback).Run(MakeErrorResult("Runtime.evaluate 실패: " + error_str));
    return;
  }

  const base::Value::Dict* result = dict->FindDict("result");
  if (!result) {
    LOG(ERROR) << "[JavaScriptTool] 응답에 result 키가 없음";
    std::move(callback).Run(MakeErrorResult("Runtime.evaluate 응답에 result 없음"));
    return;
  }

  // JavaScript 예외 발생 여부 확인 (런타임 예외 ≠ CDP 에러)
  std::string exception_msg = ExtractExceptionMessage(*result);
  if (!exception_msg.empty()) {
    LOG(WARNING) << "[JavaScriptTool] JavaScript 예외: " << exception_msg;
    std::move(callback).Run(MakeErrorResult("JavaScript 예외: " + exception_msg));
    return;
  }

  // 정상 결과 처리
  const base::Value::Dict* remote_obj = result->FindDict("result");
  if (!remote_obj) {
    // result.result가 없는 경우는 매우 드물지만 방어 처리
    LOG(WARNING) << "[JavaScriptTool] result.result RemoteObject가 없음";
    std::move(callback).Run(MakeSuccessResult("undefined"));
    return;
  }

  std::string result_str = RemoteObjectToString(*remote_obj);
  LOG(INFO) << "[JavaScriptTool] 실행 결과: " << result_str;
  std::move(callback).Run(MakeSuccessResult(result_str));
}

}  // namespace mcp
