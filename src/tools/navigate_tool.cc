// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/navigate_tool.h"

#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

NavigateTool::NavigateTool() = default;
NavigateTool::~NavigateTool() = default;

std::string NavigateTool::name() const {
  return "navigate";
}

std::string NavigateTool::description() const {
  return "URL로 이동하거나 뒤로/앞으로 탐색";
}

base::Value::Dict NavigateTool::input_schema() const {
  // JSON Schema 형식으로 입력 파라미터를 정의한다.
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // url: action이 "url"일 때 필수 파라미터
  base::Value::Dict url_prop;
  url_prop.Set("type", "string");
  url_prop.Set("description", "이동할 대상 URL (action이 'url'일 때 필수)");
  properties.Set("url", std::move(url_prop));

  // action: 탐색 종류 (기본값 "url")
  base::Value::Dict action_prop;
  action_prop.Set("type", "string");
  base::Value::List action_enum;
  action_enum.Append("url");
  action_enum.Append("back");
  action_enum.Append("forward");
  action_enum.Append("reload");
  action_prop.Set("enum", std::move(action_enum));
  action_prop.Set("description",
                  "탐색 동작: url(URL 이동), back(뒤로), "
                  "forward(앞으로), reload(새로고침)");
  action_prop.Set("default", "url");
  properties.Set("action", std::move(action_prop));

  schema.Set("properties", std::move(properties));

  // action이 "url"인 경우 url 필드가 필요하지만,
  // JSON Schema의 conditionally required는 복잡하므로 description으로 안내
  return schema;
}

void NavigateTool::Execute(const base::Value::Dict& arguments,
                           McpSession* session,
                           base::OnceCallback<void(base::Value)> callback) {
  // action 파라미터 추출 (없으면 기본값 "url" 사용)
  const std::string* action_ptr = arguments.FindString("action");
  std::string action = action_ptr ? *action_ptr : "url";

  LOG(INFO) << "[NavigateTool] Execute called, action=" << action;

  if (action == "back") {
    // 뒤로 이동: Page.goBack
    SendNavigationCommand("Page.goBack", base::Value::Dict(), session,
                          std::move(callback));

  } else if (action == "forward") {
    // 앞으로 이동: Page.goForward
    SendNavigationCommand("Page.goForward", base::Value::Dict(), session,
                          std::move(callback));

  } else if (action == "reload") {
    // 새로고침: Page.reload (캐시 무시 안함)
    base::Value::Dict params;
    params.Set("ignoreCache", false);
    SendNavigationCommand("Page.reload", std::move(params), session,
                          std::move(callback));

  } else {
    // 기본 동작: URL로 이동 (action == "url" 또는 기타 값)
    const std::string* url_ptr = arguments.FindString("url");
    if (!url_ptr || url_ptr->empty()) {
      LOG(WARNING) << "[NavigateTool] action='url'인데 url 파라미터가 없음";
      base::Value::Dict error;
      error.Set("error", "url 파라미터가 필요합니다");
      std::move(callback).Run(base::Value(std::move(error)));
      return;
    }

    LOG(INFO) << "[NavigateTool] Page.navigate url=" << *url_ptr;
    base::Value::Dict params;
    params.Set("url", *url_ptr);
    SendNavigationCommand("Page.navigate", std::move(params), session,
                          std::move(callback));
  }
}

void NavigateTool::SendNavigationCommand(
    const std::string& method,
    base::Value::Dict params,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // CDP 명령 전송 후 응답을 OnNavigationCommandResponse에서 처리한다.
  // Page.navigate/goBack/goForward/reload 모두 네비게이션이 시작되면 응답을
  // 반환하며, 페이지 로드 완료는 별도의 loadEventFired 이벤트로 확인해야
  // 하지만 현재 McpSession API에서는 이벤트 구독 없이 CDP 명령 응답만
  // 사용한다. 실제 Chromium 구현에서는 WebContentsObserver를 통해
  // DidFinishNavigation을 감시하는 것이 더 적합하다.
  session->SendCdpCommand(
      method, std::move(params),
      base::BindOnce(&NavigateTool::OnNavigationCommandResponse,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void NavigateTool::OnNavigationCommandResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // CDP 응답에 에러가 있는지 확인한다.
  // CDP 에러 응답 형식: {"error": {"code": -32601, "message": "..."}}
  if (response.is_dict()) {
    const base::Value::Dict& dict = response.GetDict();
    const base::Value::Dict* error_dict = dict.FindDict("error");
    if (error_dict) {
      const std::string* msg = error_dict->FindString("message");
      std::string error_msg = msg ? *msg : "알 수 없는 CDP 오류";
      LOG(ERROR) << "[NavigateTool] CDP 오류: " << error_msg;

      base::Value::Dict result;
      result.Set("success", false);
      result.Set("error", error_msg);
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }

    // 성공: frameId, loaderId 등을 포함한 응답 반환
    LOG(INFO) << "[NavigateTool] 네비게이션 명령 성공";
    base::Value::Dict result;
    result.Set("success", true);

    // Page.navigate 응답에서 frameId와 loaderId를 추출해 결과에 포함
    const std::string* frame_id = dict.FindString("frameId");
    if (frame_id) {
      result.Set("frameId", *frame_id);
    }
    const std::string* loader_id = dict.FindString("loaderId");
    if (loader_id) {
      result.Set("loaderId", *loader_id);
    }

    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // 예상치 못한 응답 형식
  LOG(WARNING) << "[NavigateTool] 예상치 못한 CDP 응답 형식";
  base::Value::Dict result;
  result.Set("success", true);  // 오류가 없으면 성공으로 간주
  std::move(callback).Run(base::Value(std::move(result)));
}

}  // namespace mcp
