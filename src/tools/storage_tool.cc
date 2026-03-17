// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/storage_tool.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

namespace {

// MCP 성공 응답 Value 생성.
base::Value MakeSuccessResult(const std::string& text) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", text);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", false);
  return base::Value(std::move(result));
}

// MCP 에러 응답 Value 생성.
base::Value MakeErrorResult(const std::string& text) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", text);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", true);
  return base::Value(std::move(result));
}

// 문자열을 JavaScript 문자열 리터럴로 안전하게 이스케이프한다.
// 작은따옴표와 역슬래시를 이스케이프하여 JS 인젝션을 방지한다.
// 결과에는 작은따옴표 구분자가 포함되지 않는다.
std::string EscapeForJsString(const std::string& input) {
  std::string result;
  result.reserve(input.size());
  for (char c : input) {
    if (c == '\'') {
      result += "\\'";
    } else if (c == '\\') {
      result += "\\\\";
    } else if (c == '\n') {
      result += "\\n";
    } else if (c == '\r') {
      result += "\\r";
    } else {
      result += c;
    }
  }
  return result;
}

// Runtime.evaluate 응답에서 예외 정보를 추출한다.
// exceptionDetails가 없으면 빈 문자열 반환.
std::string ExtractExceptionMessage(const base::DictValue& result_dict) {
  const base::DictValue* exception = result_dict.FindDict("exceptionDetails");
  if (!exception) {
    return "";
  }

  // exceptionDetails.exception.description: 상세 에러 메시지 (스택 포함)
  const base::DictValue* exc_obj = exception->FindDict("exception");
  if (exc_obj) {
    const std::string* desc = exc_obj->FindString("description");
    if (desc && !desc->empty()) {
      return *desc;
    }
  }

  // exceptionDetails.text: 간략한 설명
  const std::string* text = exception->FindString("text");
  if (text && !text->empty()) {
    return *text;
  }

  return "JavaScript 예외 발생 (상세 정보 없음)";
}

// Runtime.evaluate result.result RemoteObject에서 값을 문자열로 변환.
std::string RemoteObjectToString(const base::DictValue& remote_obj) {
  const base::Value* value = remote_obj.Find("value");
  if (value) {
    if (value->is_string()) {
      return value->GetString();
    }
    if (value->is_bool()) {
      return value->GetBool() ? "true" : "false";
    }
    if (value->is_int()) {
      return std::to_string(value->GetInt());
    }
    if (value->is_double()) {
      double d = value->GetDouble();
      if (d == static_cast<long long>(d)) {
        return std::to_string(static_cast<long long>(d));
      }
      return std::to_string(d);
    }
    if (value->is_none()) {
      const std::string* type = remote_obj.FindString("type");
      return (type && *type == "undefined") ? "undefined" : "null";
    }
    // 객체/배열은 JSON으로 직렬화
    std::string json;
    if (base::JSONWriter::Write(*value, &json)) {
      return json;
    }
  }

  // value 키가 없으면 description 사용
  const std::string* desc = remote_obj.FindString("description");
  if (desc && !desc->empty()) {
    return *desc;
  }

  const std::string* type = remote_obj.FindString("type");
  if (type) {
    return "<" + *type + ">";
  }

  return "<알 수 없는 결과>";
}

}  // namespace

StorageTool::StorageTool() = default;
StorageTool::~StorageTool() = default;

std::string StorageTool::name() const {
  return "storage";
}

std::string StorageTool::description() const {
  return "localStorage 및 sessionStorage에 접근합니다. "
         "action=get으로 특정 키의 값을 조회하고, "
         "action=set으로 키-값을 저장하며, "
         "action=remove로 키를 삭제하고, "
         "action=clear로 스토리지를 비우며, "
         "action=getAll로 모든 항목을 조회합니다. "
         "storageType으로 localStorage(기본) 또는 sessionStorage를 선택합니다.";
}

base::DictValue StorageTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: 필수 파라미터
  {
    base::DictValue action_prop;
    action_prop.Set("type", "string");
    base::ListValue action_enum;
    action_enum.Append("get");
    action_enum.Append("set");
    action_enum.Append("remove");
    action_enum.Append("clear");
    action_enum.Append("getAll");
    action_prop.Set("enum", std::move(action_enum));
    action_prop.Set("description",
                    "수행할 작업: get(값 조회), set(값 저장), remove(키 삭제), "
                    "clear(전체 삭제), getAll(전체 조회)");
    properties.Set("action", std::move(action_prop));
  }

  // storageType: localStorage 또는 sessionStorage (기본: local)
  {
    base::DictValue type_prop;
    type_prop.Set("type", "string");
    base::ListValue type_enum;
    type_enum.Append("local");
    type_enum.Append("session");
    type_prop.Set("enum", std::move(type_enum));
    type_prop.Set("default", "local");
    type_prop.Set("description",
                  "스토리지 유형: local(localStorage, 영구 저장) 또는 "
                  "session(sessionStorage, 탭 단위). 기본값: local");
    properties.Set("storageType", std::move(type_prop));
  }

  // key: 키 이름 (get/set/remove 시 필수)
  {
    base::DictValue key_prop;
    key_prop.Set("type", "string");
    key_prop.Set("description", "스토리지 키 이름. get/set/remove 시 필수.");
    properties.Set("key", std::move(key_prop));
  }

  // value: 저장할 값 (set 시 필수)
  {
    base::DictValue value_prop;
    value_prop.Set("type", "string");
    value_prop.Set("description", "저장할 문자열 값. set 시 필수.");
    properties.Set("value", std::move(value_prop));
  }

  schema.Set("properties", std::move(properties));

  // action만 필수
  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void StorageTool::Execute(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* action = arguments.FindString("action");
  if (!action || action->empty()) {
    LOG(WARNING) << "[StorageTool] action 파라미터 누락";
    std::move(callback).Run(
        MakeErrorResult("action 파라미터가 필요합니다 (get/set/remove/clear/getAll)"));
    return;
  }

  // storageType 파라미터 처리 (기본값: "local")
  const std::string* storage_type_str = arguments.FindString("storageType");
  std::string storage_type =
      (storage_type_str && !storage_type_str->empty()) ? *storage_type_str : "local";

  // storageType 유효성 검사
  if (storage_type != "local" && storage_type != "session") {
    std::move(callback).Run(
        MakeErrorResult("storageType은 'local' 또는 'session'이어야 합니다."));
    return;
  }

  // JavaScript 전역 객체 이름: localStorage 또는 sessionStorage
  std::string js_storage = StorageTypeToJsObject(storage_type);

  LOG(INFO) << "[StorageTool] action=" << *action
            << " storageType=" << storage_type;

  std::string expression;

  if (*action == "get") {
    const std::string* key = arguments.FindString("key");
    if (!key || key->empty()) {
      std::move(callback).Run(
          MakeErrorResult("get 작업에는 key 파라미터가 필요합니다."));
      return;
    }
    // localStorage.getItem(key): 키가 없으면 null 반환
    expression = js_storage + ".getItem('" + EscapeForJsString(*key) + "')";

  } else if (*action == "set") {
    const std::string* key = arguments.FindString("key");
    const std::string* value = arguments.FindString("value");
    if (!key || key->empty()) {
      std::move(callback).Run(
          MakeErrorResult("set 작업에는 key 파라미터가 필요합니다."));
      return;
    }
    if (!value) {
      std::move(callback).Run(
          MakeErrorResult("set 작업에는 value 파라미터가 필요합니다."));
      return;
    }
    // localStorage.setItem(key, value): 반환값 없음 (undefined)
    expression = js_storage + ".setItem('" + EscapeForJsString(*key) +
                 "', '" + EscapeForJsString(*value) + "')";

  } else if (*action == "remove") {
    // removeItem: 키 하나를 삭제한다.
    const std::string* key = arguments.FindString("key");
    if (!key || key->empty()) {
      std::move(callback).Run(
          MakeErrorResult("remove 작업에는 key 파라미터가 필요합니다."));
      return;
    }
    // localStorage.removeItem(key): 반환값 없음 (undefined)
    expression = js_storage + ".removeItem('" + EscapeForJsString(*key) + "')";

  } else if (*action == "clear") {
    // localStorage.clear(): 모든 항목 삭제. 반환값 없음 (undefined)
    expression = js_storage + ".clear()";

  } else if (*action == "getAll") {
    // Object.entries(localStorage): [[key, value], ...] 형태로 전체 반환.
    // JSON.stringify로 직렬화하여 MCP 응답 텍스트로 전달.
    expression = "JSON.stringify(Object.entries(" + js_storage + "))";

  } else {
    LOG(WARNING) << "[StorageTool] 알 수 없는 action: " << *action;
    std::move(callback).Run(
        MakeErrorResult("action은 get/set/remove/clear/getAll 중 하나여야 합니다."));
    return;
  }

  LOG(INFO) << "[StorageTool] JS 표현식 실행: " << expression;
  EvaluateStorageScript(expression, session, std::move(callback));
}

// Runtime.evaluate로 스토리지 접근 JS를 실행한다.
//
// ★ Runtime.enable 미호출 원칙 ★
//   Runtime.evaluate는 Runtime.enable 없이도 직접 호출 가능하다.
//   Runtime.enable을 호출하면 executionContextCreated 이벤트가 발생하여
//   일부 안티봇 스크립트가 DevTools 연결을 탐지할 수 있다.
//
// awaitPromise=false: localStorage/sessionStorage API는 동기식이므로
//   Promise 대기가 불필요하다.
void StorageTool::EvaluateStorageScript(
    const std::string& expression,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("expression", expression);

  // 결과를 JSON 직렬화 가능한 값으로 수신 (RemoteObjectId 대신)
  params.Set("returnByValue", true);

  // 스토리지 API는 동기식이므로 Promise 대기 불필요
  params.Set("awaitPromise", false);

  // 사용자 제스처 컨텍스트에서 실행 (일부 브라우저 보안 정책 우회)
  params.Set("userGesture", true);

  // ★ contextId 미지정: 메인 월드(기본 컨텍스트)에서 실행.
  // Runtime.enable 없이도 기본 컨텍스트에서 실행 가능.

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&StorageTool::OnEvaluateResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// Runtime.evaluate 응답을 MCP 결과로 변환.
//
// CDP Runtime.evaluate 응답 구조:
//   {
//     "result": {                    // 항상 존재
//       "result": { RemoteObject },  // 실행 결과
//       "exceptionDetails": { ... }  // 예외 발생 시에만 존재
//     }
//   }
void StorageTool::OnEvaluateResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    LOG(ERROR) << "[StorageTool] Runtime.evaluate 응답이 Dict 형식이 아님";
    std::move(callback).Run(MakeErrorResult("Runtime.evaluate 응답 형식 오류"));
    return;
  }

  // CDP 전송 레벨 에러 (연결 실패, 프로토콜 오류 등)
  if (dict->Find("error") != nullptr) {
    const base::DictValue* err = dict->FindDict("error");
    const std::string* msg = err ? err->FindString("message") : nullptr;
    std::string error_str = msg ? *msg : "알 수 없는 CDP 에러";
    LOG(ERROR) << "[StorageTool] CDP 에러: " << error_str;
    std::move(callback).Run(
        MakeErrorResult("스토리지 접근 실패: " + error_str));
    return;
  }

  const base::DictValue* result = dict->FindDict("result");
  if (!result) {
    LOG(ERROR) << "[StorageTool] 응답에 result 키가 없음";
    std::move(callback).Run(MakeErrorResult("Runtime.evaluate 응답에 result 없음"));
    return;
  }

  // JavaScript 예외 발생 여부 확인 (예: QuotaExceededError)
  std::string exception_msg = ExtractExceptionMessage(*result);
  if (!exception_msg.empty()) {
    LOG(WARNING) << "[StorageTool] JavaScript 예외: " << exception_msg;
    std::move(callback).Run(
        MakeErrorResult("스토리지 접근 중 오류: " + exception_msg));
    return;
  }

  // 정상 결과 처리
  const base::DictValue* remote_obj = result->FindDict("result");
  if (!remote_obj) {
    // setItem/removeItem/clear는 undefined를 반환하므로 성공으로 처리
    LOG(INFO) << "[StorageTool] 작업 완료 (반환값 없음)";
    std::move(callback).Run(MakeSuccessResult("작업이 완료되었습니다."));
    return;
  }

  std::string result_str = RemoteObjectToString(*remote_obj);

  // undefined: setItem/removeItem/clear 등 반환값 없는 작업의 정상 결과
  if (result_str == "undefined") {
    LOG(INFO) << "[StorageTool] 작업 완료";
    std::move(callback).Run(MakeSuccessResult("작업이 완료되었습니다."));
    return;
  }

  // null: getItem에서 키가 존재하지 않을 때
  if (result_str == "null") {
    LOG(INFO) << "[StorageTool] 키를 찾을 수 없음 (null)";
    std::move(callback).Run(MakeSuccessResult("null"));
    return;
  }

  LOG(INFO) << "[StorageTool] 실행 결과 길이: " << result_str.size();
  std::move(callback).Run(MakeSuccessResult(result_str));
}

// static
// 스토리지 타입 문자열을 JavaScript 전역 객체 이름으로 변환.
// "local"   → "localStorage"
// "session" → "sessionStorage"
// 기타       → "localStorage" (기본값)
std::string StorageTool::StorageTypeToJsObject(
    const std::string& storage_type) {
  if (storage_type == "session") {
    return "sessionStorage";
  }
  return "localStorage";
}

}  // namespace mcp
