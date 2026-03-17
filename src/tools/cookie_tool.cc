// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/cookie_tool.h"

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

// CDP 응답에서 에러 메시지를 추출한다.
// 에러가 없으면 빈 문자열 반환.
std::string ExtractCdpError(const base::DictValue& dict) {
  const base::DictValue* error = dict.FindDict("error");
  if (!error) {
    return "";
  }
  const std::string* msg = error->FindString("message");
  return msg ? *msg : "알 수 없는 CDP 에러";
}

}  // namespace

CookieTool::CookieTool() = default;
CookieTool::~CookieTool() = default;

std::string CookieTool::name() const {
  return "cookies";
}

std::string CookieTool::description() const {
  return "쿠키 조회, 설정, 삭제를 수행합니다. "
         "action=get으로 특정 URL 또는 전체 쿠키를 조회하고, "
         "action=set으로 새 쿠키를 설정하며, "
         "action=delete로 특정 쿠키를 삭제하고, "
         "action=clear로 브라우저의 모든 쿠키를 삭제합니다.";
}

base::DictValue CookieTool::input_schema() const {
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
    action_enum.Append("delete");
    action_enum.Append("clear");
    action_prop.Set("enum", std::move(action_enum));
    action_prop.Set("description",
                    "수행할 작업: get(조회), set(설정), delete(삭제), "
                    "clear(전체 삭제)");
    properties.Set("action", std::move(action_prop));
  }

  // url: 쿠키 대상 URL (get/delete 시 범위 지정에 사용)
  {
    base::DictValue url_prop;
    url_prop.Set("type", "string");
    url_prop.Set("description",
                 "쿠키 대상 URL. get 시 해당 URL에 적용되는 쿠키만 반환. "
                 "delete 시 해당 URL에서 쿠키를 삭제.");
    properties.Set("url", std::move(url_prop));
  }

  // name: 쿠키 이름 (set/delete 시 필수)
  {
    base::DictValue name_prop;
    name_prop.Set("type", "string");
    name_prop.Set("description", "쿠키 이름. set/delete 시 필수.");
    properties.Set("name", std::move(name_prop));
  }

  // value: 쿠키 값 (set 시 필수)
  {
    base::DictValue value_prop;
    value_prop.Set("type", "string");
    value_prop.Set("description", "쿠키 값. set 시 필수.");
    properties.Set("value", std::move(value_prop));
  }

  // domain: 쿠키 도메인 (set 시 사용)
  {
    base::DictValue domain_prop;
    domain_prop.Set("type", "string");
    domain_prop.Set("description",
                    "쿠키 도메인. set 시 설정. 예: \".example.com\"");
    properties.Set("domain", std::move(domain_prop));
  }

  // path: 쿠키 경로 (기본값: "/")
  {
    base::DictValue path_prop;
    path_prop.Set("type", "string");
    path_prop.Set("description", "쿠키 경로. 기본값: \"/\"");
    properties.Set("path", std::move(path_prop));
  }

  // secure: Secure 플래그
  {
    base::DictValue secure_prop;
    secure_prop.Set("type", "boolean");
    secure_prop.Set("description",
                    "true이면 HTTPS 연결에서만 전송되는 Secure 쿠키로 설정.");
    properties.Set("secure", std::move(secure_prop));
  }

  // httpOnly: HttpOnly 플래그
  {
    base::DictValue http_only_prop;
    http_only_prop.Set("type", "boolean");
    http_only_prop.Set("description",
                       "true이면 JavaScript에서 접근 불가한 HttpOnly 쿠키로 설정.");
    properties.Set("httpOnly", std::move(http_only_prop));
  }

  // sameSite: SameSite 정책
  {
    base::DictValue same_site_prop;
    same_site_prop.Set("type", "string");
    base::ListValue same_site_enum;
    same_site_enum.Append("Strict");
    same_site_enum.Append("Lax");
    same_site_enum.Append("None");
    same_site_prop.Set("enum", std::move(same_site_enum));
    same_site_prop.Set("description",
                       "SameSite 정책: Strict(엄격), Lax(기본), None(없음)");
    properties.Set("sameSite", std::move(same_site_prop));
  }

  // expirationDate: 만료 시간 (Unix timestamp, 초 단위)
  // Network.setCookie의 expires 필드에 매핑된다.
  {
    base::DictValue expires_prop;
    expires_prop.Set("type", "number");
    expires_prop.Set("description",
                     "쿠키 만료 시간 (Unix timestamp, 초 단위). "
                     "설정하지 않으면 세션 쿠키로 생성.");
    properties.Set("expirationDate", std::move(expires_prop));
  }

  schema.Set("properties", std::move(properties));

  // action만 필수
  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void CookieTool::Execute(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* action = arguments.FindString("action");
  if (!action || action->empty()) {
    LOG(WARNING) << "[CookieTool] action 파라미터 누락";
    std::move(callback).Run(
        MakeErrorResult("action 파라미터가 필요합니다 (get/set/delete/clear)"));
    return;
  }

  LOG(INFO) << "[CookieTool] action=" << *action;

  if (*action == "get") {
    // url 파라미터는 선택적
    const std::string* url = arguments.FindString("url");
    HandleGet(url ? *url : "", session, std::move(callback));
  } else if (*action == "set") {
    HandleSet(arguments, session, std::move(callback));
  } else if (*action == "delete") {
    HandleDelete(arguments, session, std::move(callback));
  } else if (*action == "clear") {
    HandleClear(session, std::move(callback));
  } else {
    LOG(WARNING) << "[CookieTool] 알 수 없는 action: " << *action;
    std::move(callback).Run(
        MakeErrorResult("action은 get/set/delete/clear 중 하나여야 합니다."));
  }
}

// action=get: Network.getCookies 호출.
// url이 비어 있으면 모든 쿠키를 반환한다.
void CookieTool::HandleGet(
    const std::string& url,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;

  if (!url.empty()) {
    // urls 배열에 대상 URL을 추가하면 해당 URL에 적용되는 쿠키만 반환된다.
    base::ListValue urls;
    urls.Append(url);
    params.Set("urls", std::move(urls));
    LOG(INFO) << "[CookieTool] Network.getCookies url=" << url;
  } else {
    LOG(INFO) << "[CookieTool] Network.getCookies (전체 쿠키)";
  }

  session->SendCdpCommand(
      "Network.getCookies", std::move(params),
      base::BindOnce(&CookieTool::OnGetCookiesResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// action=set: Network.setCookie 호출.
// name과 value는 필수. 나머지는 선택적으로 설정.
void CookieTool::HandleSet(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* name = arguments.FindString("name");
  const std::string* value = arguments.FindString("value");

  if (!name || name->empty()) {
    std::move(callback).Run(MakeErrorResult("set 작업에는 name 파라미터가 필요합니다."));
    return;
  }
  if (!value) {
    std::move(callback).Run(MakeErrorResult("set 작업에는 value 파라미터가 필요합니다."));
    return;
  }

  base::DictValue params;
  params.Set("name", *name);
  params.Set("value", *value);

  // url 또는 domain 중 하나로 쿠키 범위를 지정해야 한다.
  // url이 있으면 url로, 없으면 domain으로 설정.
  const std::string* url = arguments.FindString("url");
  const std::string* domain = arguments.FindString("domain");
  if (url && !url->empty()) {
    params.Set("url", *url);
  } else if (domain && !domain->empty()) {
    params.Set("domain", *domain);
  }

  // path: 기본값 "/"
  const std::string* path = arguments.FindString("path");
  params.Set("path", (path && !path->empty()) ? *path : "/");

  // secure 플래그 (선택적)
  std::optional<bool> secure = arguments.FindBool("secure");
  if (secure.has_value()) {
    params.Set("secure", *secure);
  }

  // httpOnly 플래그 (선택적)
  std::optional<bool> http_only = arguments.FindBool("httpOnly");
  if (http_only.has_value()) {
    params.Set("httpOnly", *http_only);
  }

  // sameSite 정책 (선택적)
  const std::string* same_site = arguments.FindString("sameSite");
  if (same_site && !same_site->empty()) {
    params.Set("sameSite", *same_site);
  }

  // expirationDate: Unix timestamp (초 단위, 선택적).
  // Network.setCookie의 expires 필드에 매핑한다.
  const base::Value* expiration_date = arguments.Find("expirationDate");
  if (expiration_date) {
    if (expiration_date->is_double()) {
      params.Set("expires", expiration_date->GetDouble());
    } else if (expiration_date->is_int()) {
      params.Set("expires", static_cast<double>(expiration_date->GetInt()));
    }
  }

  LOG(INFO) << "[CookieTool] Network.setCookie name=" << *name;

  session->SendCdpCommand(
      "Network.setCookie", std::move(params),
      base::BindOnce(&CookieTool::OnSetCookieResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// action=delete: Network.deleteCookies 호출.
// name은 필수. url 또는 domain으로 범위를 지정한다.
void CookieTool::HandleDelete(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* name = arguments.FindString("name");
  if (!name || name->empty()) {
    std::move(callback).Run(
        MakeErrorResult("delete 작업에는 name 파라미터가 필요합니다."));
    return;
  }

  base::DictValue params;
  params.Set("name", *name);

  // url 또는 domain으로 범위 지정 (선택적)
  const std::string* url = arguments.FindString("url");
  if (url && !url->empty()) {
    params.Set("url", *url);
  }
  const std::string* domain = arguments.FindString("domain");
  if (domain && !domain->empty()) {
    params.Set("domain", *domain);
  }
  const std::string* path = arguments.FindString("path");
  if (path && !path->empty()) {
    params.Set("path", *path);
  }

  LOG(INFO) << "[CookieTool] Network.deleteCookies name=" << *name;

  session->SendCdpCommand(
      "Network.deleteCookies", std::move(params),
      base::BindOnce(&CookieTool::OnSimpleCommandResponse,
                     weak_factory_.GetWeakPtr(),
                     "쿠키가 삭제되었습니다: " + *name,
                     std::move(callback)));
}

// action=clear: Network.clearBrowserCookies 호출.
// 브라우저의 모든 쿠키를 삭제한다.
void CookieTool::HandleClear(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  LOG(INFO) << "[CookieTool] Network.clearBrowserCookies";

  session->SendCdpCommand(
      "Network.clearBrowserCookies", base::DictValue(),
      base::BindOnce(&CookieTool::OnSimpleCommandResponse,
                     weak_factory_.GetWeakPtr(),
                     "모든 쿠키가 삭제되었습니다.",
                     std::move(callback)));
}

// Network.getCookies 응답 처리.
// 응답 구조: { result: { cookies: [ CookieObject, ... ] } }
void CookieTool::OnGetCookiesResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(callback).Run(MakeErrorResult("Network.getCookies 응답 형식 오류"));
    return;
  }

  // CDP 레벨 에러 확인
  std::string cdp_error = ExtractCdpError(*dict);
  if (!cdp_error.empty()) {
    LOG(ERROR) << "[CookieTool] Network.getCookies 실패: " << cdp_error;
    std::move(callback).Run(MakeErrorResult("쿠키 조회 실패: " + cdp_error));
    return;
  }

  // result.cookies 배열 추출
  const base::DictValue* result = dict->FindDict("result");
  const base::ListValue* cookies =
      result ? result->FindList("cookies") : nullptr;

  if (!cookies) {
    // 쿠키가 없거나 cookies 키가 없는 경우
    std::move(callback).Run(MakeSuccessResult("[]"));
    return;
  }

  // 쿠키 배열을 JSON으로 직렬화
  std::string json_str;
  base::JSONWriter::WriteWithOptions(
      base::Value(cookies->Clone()),
      base::JSONWriter::OPTIONS_PRETTY_PRINT,
      &json_str);

  LOG(INFO) << "[CookieTool] 쿠키 " << cookies->size() << "개 반환";
  std::move(callback).Run(MakeSuccessResult(json_str));
}

// Network.setCookie 응답 처리.
// 응답 구조: { result: { success: true/false } }
void CookieTool::OnSetCookieResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    std::move(callback).Run(MakeErrorResult("Network.setCookie 응답 형식 오류"));
    return;
  }

  // CDP 레벨 에러 확인
  std::string cdp_error = ExtractCdpError(*dict);
  if (!cdp_error.empty()) {
    LOG(ERROR) << "[CookieTool] Network.setCookie 실패: " << cdp_error;
    std::move(callback).Run(MakeErrorResult("쿠키 설정 실패: " + cdp_error));
    return;
  }

  // result.success 필드 확인
  const base::DictValue* result = dict->FindDict("result");
  std::optional<bool> success =
      result ? result->FindBool("success") : std::nullopt;

  if (success.has_value() && !*success) {
    // success=false: 유효하지 않은 쿠키 파라미터 (예: SameSite=None without Secure)
    LOG(WARNING) << "[CookieTool] Network.setCookie success=false";
    std::move(callback).Run(
        MakeErrorResult("쿠키 설정 실패: 유효하지 않은 쿠키 파라미터입니다. "
                        "(SameSite=None은 Secure=true와 함께 사용해야 합니다.)"));
    return;
  }

  LOG(INFO) << "[CookieTool] 쿠키 설정 성공";
  std::move(callback).Run(MakeSuccessResult("쿠키가 성공적으로 설정되었습니다."));
}

// 단순 명령(deleteCookies, clearBrowserCookies) 응답 처리.
// 에러가 없으면 success_message를 반환한다.
void CookieTool::OnSimpleCommandResponse(
    const std::string& success_message,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  const base::DictValue* dict = response.GetIfDict();
  if (dict) {
    std::string cdp_error = ExtractCdpError(*dict);
    if (!cdp_error.empty()) {
      LOG(ERROR) << "[CookieTool] CDP 명령 실패: " << cdp_error;
      std::move(callback).Run(MakeErrorResult("작업 실패: " + cdp_error));
      return;
    }
  }

  LOG(INFO) << "[CookieTool] " << success_message;
  std::move(callback).Run(MakeSuccessResult(success_message));
}

}  // namespace mcp
