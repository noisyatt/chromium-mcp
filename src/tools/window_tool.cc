// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/window_tool.h"

#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

WindowTool::WindowTool() = default;
WindowTool::~WindowTool() = default;

std::string WindowTool::name() const {
  return "window";
}

std::string WindowTool::description() const {
  return "브라우저 윈도우 크기 조절, 위치 이동, 최소화/최대화";
}

base::DictValue WindowTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: 수행할 윈도우 동작
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue enums;
    enums.Append("resize");
    enums.Append("move");
    enums.Append("minimize");
    enums.Append("maximize");
    enums.Append("fullscreen");
    enums.Append("restore");
    enums.Append("getBounds");
    enums.Append("list");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "resize=크기 변경, move=위치 이동, minimize=최소화, "
             "maximize=최대화, fullscreen=전체화면, restore=일반 크기 복원, "
             "getBounds=현재 위치·크기 조회, list=윈도우 목록 조회");
    properties.Set("action", std::move(prop));
  }

  // width: 윈도우 너비 (action=resize 시 사용)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description", "윈도우 너비 (픽셀). action=resize 시 사용");
    properties.Set("width", std::move(prop));
  }

  // height: 윈도우 높이 (action=resize 시 사용)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description", "윈도우 높이 (픽셀). action=resize 시 사용");
    properties.Set("height", std::move(prop));
  }

  // x: 윈도우 X 좌표 (action=move 시 사용)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description", "윈도우 왼쪽 모서리 X 좌표 (픽셀). action=move 시 사용");
    properties.Set("x", std::move(prop));
  }

  // y: 윈도우 Y 좌표 (action=move 시 사용)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description", "윈도우 위쪽 모서리 Y 좌표 (픽셀). action=move 시 사용");
    properties.Set("y", std::move(prop));
  }

  // windowId: 대상 윈도우 ID (생략 시 현재 세션 탭의 윈도우를 자동 조회)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description",
             "대상 윈도우 ID. 생략하면 Browser.getWindowForTarget으로 "
             "현재 탭의 윈도우를 자동 조회한다");
    properties.Set("windowId", std::move(prop));
  }

  schema.Set("properties", std::move(properties));

  // action은 필수 파라미터
  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void WindowTool::Execute(const base::DictValue& arguments,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback) {
  const std::string* action_ptr = arguments.FindString("action");
  if (!action_ptr || action_ptr->empty()) {
    base::DictValue err;
    err.Set("error",
            "action 파라미터가 필요합니다: "
            "resize/move/minimize/maximize/fullscreen/restore/getBounds/list");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }
  const std::string& action = *action_ptr;

  // action=list는 windowId 없이 처리한다.
  if (action == "list") {
    ExecuteList(session, std::move(callback));
    return;
  }

  // windowId 파라미터 확인
  std::optional<double> window_id_opt = arguments.FindDouble("windowId");
  if (window_id_opt.has_value()) {
    // windowId가 제공된 경우 바로 action 실행
    int window_id = static_cast<int>(*window_id_opt);
    ExecuteWithWindowId(window_id, action, arguments, session,
                        std::move(callback));
    return;
  }

  // windowId가 없으면 Browser.getWindowForTarget으로 현재 탭 윈도우 조회
  LOG(INFO) << "[WindowTool] windowId 미제공: getWindowForTarget 호출";
  session->SendCdpCommand(
      "Browser.getWindowForTarget", base::DictValue(),
      base::BindOnce(&WindowTool::OnGetWindowForTarget,
                     weak_factory_.GetWeakPtr(),
                     action,
                     arguments.Clone(),  // arguments를 값 복사하여 캡처
                     session,
                     std::move(callback)));
}

void WindowTool::OnGetWindowForTarget(
    const std::string& action,
    base::DictValue arguments_copy,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  int window_id = ExtractWindowId(response);
  if (window_id < 0) {
    LOG(ERROR) << "[WindowTool] getWindowForTarget 실패 또는 windowId 없음";
    base::DictValue err;
    err.Set("error",
            "현재 탭의 윈도우 ID를 가져오지 못했습니다. "
            "windowId 파라미터를 직접 지정해 주세요");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }
  LOG(INFO) << "[WindowTool] 자동 조회된 windowId: " << window_id;
  ExecuteWithWindowId(window_id, action, arguments_copy, session,
                      std::move(callback));
}

void WindowTool::ExecuteWithWindowId(
    int window_id,
    const std::string& action,
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (action == "resize") {
    std::optional<double> width  = arguments.FindDouble("width");
    std::optional<double> height = arguments.FindDouble("height");
    if (!width.has_value() || !height.has_value()) {
      base::DictValue err;
      err.Set("error", "action=resize에는 width와 height가 필요합니다");
      std::move(callback).Run(base::Value(std::move(err)));
      return;
    }
    ExecuteResize(window_id,
                  static_cast<int>(*width),
                  static_cast<int>(*height),
                  session, std::move(callback));
    return;
  }

  if (action == "move") {
    std::optional<double> x = arguments.FindDouble("x");
    std::optional<double> y = arguments.FindDouble("y");
    if (!x.has_value() || !y.has_value()) {
      base::DictValue err;
      err.Set("error", "action=move에는 x와 y가 필요합니다");
      std::move(callback).Run(base::Value(std::move(err)));
      return;
    }
    ExecuteMove(window_id,
                static_cast<int>(*x),
                static_cast<int>(*y),
                session, std::move(callback));
    return;
  }

  if (action == "minimize") {
    ExecuteSetState(window_id, "minimized", session, std::move(callback));
    return;
  }
  if (action == "maximize") {
    ExecuteSetState(window_id, "maximized", session, std::move(callback));
    return;
  }
  if (action == "fullscreen") {
    ExecuteSetState(window_id, "fullscreen", session, std::move(callback));
    return;
  }
  if (action == "restore") {
    ExecuteSetState(window_id, "normal", session, std::move(callback));
    return;
  }
  if (action == "getBounds") {
    ExecuteGetBounds(window_id, session, std::move(callback));
    return;
  }

  // 알 수 없는 action
  base::DictValue err;
  err.Set("error",
          "유효하지 않은 action: " + action +
          ". resize/move/minimize/maximize/fullscreen/restore/"
          "getBounds/list 중 하나를 사용하세요");
  std::move(callback).Run(base::Value(std::move(err)));
}

void WindowTool::ExecuteResize(int window_id,
                               int width,
                               int height,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback) {
  // Browser.setWindowBounds: windowState를 "normal"로 유지하고
  // 크기만 변경한다 (최소화/최대화 상태에서는 "normal"로 먼저 복원).
  base::DictValue bounds;
  bounds.Set("windowState", "normal");
  bounds.Set("width",  width);
  bounds.Set("height", height);

  base::DictValue params;
  params.Set("windowId", window_id);
  params.Set("bounds",   std::move(bounds));

  LOG(INFO) << "[WindowTool] resize: windowId=" << window_id
            << " " << width << "x" << height;

  session->SendCdpCommand(
      "Browser.setWindowBounds", std::move(params),
      base::BindOnce(&WindowTool::OnSetWindowBoundsResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback), "resize"));
}

void WindowTool::ExecuteMove(int window_id,
                             int x,
                             int y,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback) {
  // Browser.setWindowBounds: windowState를 "normal"로 유지하고
  // 위치(left, top)만 변경한다.
  base::DictValue bounds;
  bounds.Set("windowState", "normal");
  bounds.Set("left", x);
  bounds.Set("top",  y);

  base::DictValue params;
  params.Set("windowId", window_id);
  params.Set("bounds",   std::move(bounds));

  LOG(INFO) << "[WindowTool] move: windowId=" << window_id
            << " (" << x << ", " << y << ")";

  session->SendCdpCommand(
      "Browser.setWindowBounds", std::move(params),
      base::BindOnce(&WindowTool::OnSetWindowBoundsResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback), "move"));
}

void WindowTool::ExecuteSetState(int window_id,
                                 const std::string& window_state,
                                 McpSession* session,
                                 base::OnceCallback<void(base::Value)> callback) {
  // Browser.setWindowBounds: windowState만 변경한다.
  // "minimized" | "maximized" | "fullscreen" | "normal"
  base::DictValue bounds;
  bounds.Set("windowState", window_state);

  base::DictValue params;
  params.Set("windowId", window_id);
  params.Set("bounds",   std::move(bounds));

  LOG(INFO) << "[WindowTool] setState: windowId=" << window_id
            << " state=" << window_state;

  session->SendCdpCommand(
      "Browser.setWindowBounds", std::move(params),
      base::BindOnce(&WindowTool::OnSetWindowBoundsResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback), window_state));
}

void WindowTool::ExecuteGetBounds(int window_id,
                                  McpSession* session,
                                  base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("windowId", window_id);

  LOG(INFO) << "[WindowTool] getBounds: windowId=" << window_id;

  session->SendCdpCommand(
      "Browser.getWindowBounds", std::move(params),
      base::BindOnce(&WindowTool::OnGetWindowBoundsResponse,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

void WindowTool::ExecuteList(McpSession* session,
                             base::OnceCallback<void(base::Value)> callback) {
  // CDP Browser 도메인에는 윈도우 전체 목록을 반환하는 API가 없다.
  // 현재 탭이 속한 윈도우 정보만 반환하고, 필요 시 getWindowBounds도 조회한다.
  LOG(INFO) << "[WindowTool] list: 현재 탭 윈도우 조회";

  session->SendCdpCommand(
      "Browser.getWindowForTarget", base::DictValue(),
      base::BindOnce(
          [](base::WeakPtr<WindowTool> weak_self,
             McpSession* session,
             base::OnceCallback<void(base::Value)> callback,
             base::Value response) {
            int window_id = ExtractWindowId(response);
            if (window_id < 0) {
              base::DictValue err;
              err.Set("error", "현재 탭의 윈도우 ID를 가져오지 못했습니다");
              std::move(callback).Run(base::Value(std::move(err)));
              return;
            }
            if (!weak_self) {
              return;
            }
            // 윈도우 목록 형식으로 bounds를 조회하여 반환한다.
            base::DictValue params;
            params.Set("windowId", window_id);
            session->SendCdpCommand(
                "Browser.getWindowBounds", std::move(params),
                base::BindOnce(
                    [](int wid,
                       base::OnceCallback<void(base::Value)> cb,
                       base::Value bounds_response) {
                      base::DictValue result;
                      result.Set("success", true);
                      base::ListValue windows;

                      base::DictValue win_info;
                      win_info.Set("windowId", wid);

                      // bounds 정보 추가
                      if (bounds_response.is_dict()) {
                        const base::DictValue* bounds_dict =
                            bounds_response.GetDict().FindDict("bounds");
                        if (!bounds_dict) {
                          // SendCdpCommand 간편 오버로드는 result 필드로 감싸지 않는다.
                          // 응답 구조: {left, top, width, height, windowState}
                          win_info.Set("left",   bounds_response.GetDict()
                                                     .FindInt("left").value_or(0));
                          win_info.Set("top",    bounds_response.GetDict()
                                                     .FindInt("top").value_or(0));
                          win_info.Set("width",  bounds_response.GetDict()
                                                     .FindInt("width").value_or(0));
                          win_info.Set("height", bounds_response.GetDict()
                                                     .FindInt("height").value_or(0));
                          const std::string* state =
                              bounds_response.GetDict().FindString("windowState");
                          win_info.Set("windowState",
                                       state ? *state : "normal");
                        } else {
                          win_info.Set("left",   bounds_dict->FindInt("left").value_or(0));
                          win_info.Set("top",    bounds_dict->FindInt("top").value_or(0));
                          win_info.Set("width",  bounds_dict->FindInt("width").value_or(0));
                          win_info.Set("height", bounds_dict->FindInt("height").value_or(0));
                          const std::string* state =
                              bounds_dict->FindString("windowState");
                          win_info.Set("windowState",
                                       state ? *state : "normal");
                        }
                      }

                      windows.Append(std::move(win_info));
                      result.Set("windows", std::move(windows));
                      result.Set("count", 1);
                      result.Set("note",
                                 "CDP는 전체 윈도우 목록 API를 제공하지 않아 "
                                 "현재 탭의 윈도우 정보만 반환합니다");
                      std::move(cb).Run(base::Value(std::move(result)));
                    },
                    window_id, std::move(callback)));
          },
          weak_factory_.GetWeakPtr(), session, std::move(callback)));
}

void WindowTool::OnSetWindowBoundsResponse(
    base::OnceCallback<void(base::Value)> callback,
    const std::string& action,
    base::Value response) {
  if (!response.is_dict()) {
    base::DictValue err;
    err.Set("error", "Browser.setWindowBounds 응답 형식 오류");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  // CDP 오류 응답 확인
  const base::DictValue* err_dict = response.GetDict().FindDict("error");
  if (err_dict) {
    const std::string* msg = err_dict->FindString("message");
    std::string err_msg = msg ? *msg : "윈도우 조작 실패";
    LOG(ERROR) << "[WindowTool] " << action << " 실패: " << err_msg;
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", err_msg);
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[WindowTool] " << action << " 성공";
  base::DictValue result;
  result.Set("success", true);
  result.Set("action",  action);
  result.Set("message", action + " 완료");
  std::move(callback).Run(base::Value(std::move(result)));
}

void WindowTool::OnGetWindowBoundsResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    base::DictValue err;
    err.Set("error", "Browser.getWindowBounds 응답 형식 오류");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::DictValue& resp_dict = response.GetDict();

  // CDP 오류 확인
  const base::DictValue* err_dict = resp_dict.FindDict("error");
  if (err_dict) {
    const std::string* msg = err_dict->FindString("message");
    std::string err_msg = msg ? *msg : "윈도우 bounds 조회 실패";
    LOG(ERROR) << "[WindowTool] getBounds 실패: " << err_msg;
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", err_msg);
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // Browser.getWindowBounds 응답 구조:
  //   { bounds: { left, top, width, height, windowState } }
  // 또는 SendCdpCommand 간편 오버로드를 통해 result 없이 직접 전달되는 경우:
  //   { left, top, width, height, windowState }
  const base::DictValue* bounds_source = resp_dict.FindDict("bounds");
  if (!bounds_source) {
    bounds_source = &resp_dict;
  }

  base::DictValue result;
  result.Set("success", true);
  result.Set("left",    bounds_source->FindInt("left").value_or(0));
  result.Set("top",     bounds_source->FindInt("top").value_or(0));
  result.Set("width",   bounds_source->FindInt("width").value_or(0));
  result.Set("height",  bounds_source->FindInt("height").value_or(0));

  const std::string* state = bounds_source->FindString("windowState");
  result.Set("windowState", state ? *state : "normal");

  LOG(INFO) << "[WindowTool] getBounds 성공: "
            << result.FindInt("width").value_or(0) << "x"
            << result.FindInt("height").value_or(0)
            << " state=" << (state ? *state : "normal");

  std::move(callback).Run(base::Value(std::move(result)));
}

// static
int WindowTool::ExtractWindowId(const base::Value& response) {
  if (!response.is_dict()) return -1;
  const base::DictValue& dict = response.GetDict();

  // Browser.getWindowForTarget 응답 구조:
  //   { windowId: <int>, bounds: {...} }
  std::optional<int> id = dict.FindInt("windowId");
  if (id.has_value()) return *id;

  // SendCdpCommand 간편 오버로드에서 result 필드로 감싸는 경우 대비
  const base::DictValue* result = dict.FindDict("result");
  if (result) {
    id = result->FindInt("windowId");
    if (id.has_value()) return *id;
  }

  return -1;
}

}  // namespace mcp
