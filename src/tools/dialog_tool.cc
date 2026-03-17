// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/dialog_tool.h"

#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

// CDP 이벤트 이름 상수
constexpr char kDialogOpeningEvent[] = "Page.javascriptDialogOpening";
constexpr char kDialogClosedEvent[]  = "Page.javascriptDialogClosed";

DialogTool::DialogInfo::DialogInfo() = default;
DialogTool::DialogInfo::~DialogInfo() = default;
DialogTool::DialogInfo::DialogInfo(DialogInfo&&) = default;
DialogTool::DialogInfo& DialogTool::DialogInfo::operator=(DialogInfo&&) = default;

DialogTool::DialogTool() = default;
DialogTool::~DialogTool() = default;

std::string DialogTool::name() const {
  return "dialog";
}

std::string DialogTool::description() const {
  return "JavaScript 다이얼로그(alert/confirm/prompt) 처리";
}

base::DictValue DialogTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: 수행할 동작 (accept / dismiss / getInfo)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue enums;
    enums.Append("accept");
    enums.Append("dismiss");
    enums.Append("getInfo");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "accept=다이얼로그 확인, dismiss=다이얼로그 취소, "
             "getInfo=현재 열린 다이얼로그 정보 조회");
    properties.Set("action", std::move(prop));
  }

  // promptText: prompt 다이얼로그에 입력할 텍스트 (action=accept 시 사용)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "prompt 다이얼로그에 입력할 텍스트 (action=accept일 때 사용)");
    properties.Set("promptText", std::move(prop));
  }

  // autoHandle: 향후 다이얼로그 자동 처리 여부
  {
    base::DictValue prop;
    prop.Set("type", "boolean");
    prop.Set("description",
             "true=다이얼로그 자동 처리 모드 활성화, "
             "false=자동 처리 해제. action 없이 단독 사용 가능");
    properties.Set("autoHandle", std::move(prop));
  }

  // autoAction: autoHandle 모드에서 사용할 동작 (accept / dismiss)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue enums;
    enums.Append("accept");
    enums.Append("dismiss");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "autoHandle 모드에서 자동으로 수행할 동작 "
             "(기본값: accept). autoHandle=true와 함께 사용");
    properties.Set("autoAction", std::move(prop));
  }

  schema.Set("properties", std::move(properties));
  return schema;
}

void DialogTool::Execute(const base::DictValue& arguments,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback) {
  // CDP 이벤트 핸들러를 세션에 등록한다 (이미 등록된 경우 재등록하지 않는다).
  EnsureEventHandlerRegistered(session);

  // ------------------------------------------------------------------
  // 1. autoHandle / autoAction 파라미터 처리 (action과 독립적으로 설정)
  // ------------------------------------------------------------------
  const std::string* auto_action_ptr = arguments.FindString("autoAction");
  if (auto_action_ptr) {
    if (*auto_action_ptr == "accept" || *auto_action_ptr == "dismiss") {
      auto_action_ = *auto_action_ptr;
      LOG(INFO) << "[DialogTool] autoAction 설정: " << auto_action_;
    } else {
      LOG(WARNING) << "[DialogTool] 유효하지 않은 autoAction: "
                   << *auto_action_ptr << " (accept/dismiss 사용)";
    }
  }

  std::optional<bool> auto_handle_opt = arguments.FindBool("autoHandle");
  if (auto_handle_opt.has_value()) {
    auto_handle_ = *auto_handle_opt;
    LOG(INFO) << "[DialogTool] autoHandle 모드 "
              << (auto_handle_ ? "활성화" : "비활성화")
              << " (autoAction=" << auto_action_ << ")";
  }

  // ------------------------------------------------------------------
  // 2. action 파라미터 처리
  // ------------------------------------------------------------------
  const std::string* action_ptr = arguments.FindString("action");
  if (!action_ptr || action_ptr->empty()) {
    // action 없이 autoHandle 설정만 했을 때 성공 응답 반환
    base::DictValue result;
    result.Set("success", true);
    result.Set("autoHandle", auto_handle_);
    result.Set("autoAction", auto_action_);
    result.Set("hasDialog", current_dialog_.has_value());
    if (current_dialog_.has_value()) {
      result.Set("dialogType", current_dialog_->type);
    }
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }
  const std::string& action = *action_ptr;

  // ------------------------------------------------------------------
  // action=getInfo: 현재 열린 다이얼로그 정보 반환
  // ------------------------------------------------------------------
  if (action == "getInfo") {
    base::DictValue result;
    result.Set("success", true);
    if (!current_dialog_.has_value()) {
      result.Set("hasDialog", false);
      result.Set("message", "현재 열린 다이얼로그가 없습니다");
    } else {
      result.Set("hasDialog", true);
      result.Set("type", current_dialog_->type);
      result.Set("message", current_dialog_->message);
      result.Set("url", current_dialog_->url);
      if (!current_dialog_->default_prompt.empty()) {
        result.Set("defaultPrompt", current_dialog_->default_prompt);
      }
    }
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // ------------------------------------------------------------------
  // action=accept / action=dismiss: 다이얼로그 처리
  // ------------------------------------------------------------------
  bool is_accept = (action == "accept");
  if (!is_accept && action != "dismiss") {
    base::DictValue err;
    err.Set("error",
            "유효하지 않은 action: " + action +
            ". accept/dismiss/getInfo 중 하나를 사용하세요");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  // 열린 다이얼로그가 없으면 오류 반환
  if (!current_dialog_.has_value()) {
    LOG(WARNING) << "[DialogTool] 처리할 다이얼로그가 없음";
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "현재 열린 다이얼로그가 없습니다");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // Page.handleJavaScriptDialog CDP 호출 파라미터 구성
  base::DictValue params;
  params.Set("accept", is_accept);

  // accept이고 prompt 다이얼로그인 경우 promptText 설정
  if (is_accept && current_dialog_->type == "prompt") {
    const std::string* text_ptr = arguments.FindString("promptText");
    if (text_ptr && !text_ptr->empty()) {
      params.Set("promptText", *text_ptr);
      LOG(INFO) << "[DialogTool] prompt 입력값 설정: " << *text_ptr;
    }
  }

  LOG(INFO) << "[DialogTool] 다이얼로그 " << (is_accept ? "accept" : "dismiss")
            << " 처리, type=" << current_dialog_->type;

  session->SendCdpCommand(
      "Page.handleJavaScriptDialog", std::move(params),
      base::BindOnce(&DialogTool::OnHandleDialogResponse,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void DialogTool::EnsureEventHandlerRegistered(McpSession* session) {
  // 이미 동일한 세션에 등록되어 있으면 재등록 불필요
  if (event_handler_registered_ && registered_session_ == session) {
    return;
  }

  // Page.javascriptDialogOpening 이벤트 핸들러 등록
  session->RegisterCdpEventHandler(
      kDialogOpeningEvent,
      base::BindRepeating(&DialogTool::OnJavaScriptDialogOpening,
                          weak_factory_.GetWeakPtr()));

  // Page.javascriptDialogClosed 이벤트 핸들러 등록
  session->RegisterCdpEventHandler(
      kDialogClosedEvent,
      base::BindRepeating(&DialogTool::OnJavaScriptDialogClosed,
                          weak_factory_.GetWeakPtr()));

  event_handler_registered_ = true;
  registered_session_ = session;
  LOG(INFO) << "[DialogTool] 다이얼로그 이벤트 핸들러 등록 완료";
}

void DialogTool::OnJavaScriptDialogOpening(
    const std::string& event_name,
    const base::DictValue& params) {
  // CDP Page.javascriptDialogOpening 이벤트 파라미터:
  //   url           : 다이얼로그를 트리거한 페이지 URL
  //   message       : 다이얼로그 메시지 본문
  //   type          : "alert" | "confirm" | "prompt" | "beforeunload"
  //   hasBrowserHandler : 브라우저 자체 핸들러 존재 여부
  //   defaultPrompt : prompt 다이얼로그의 기본 입력값 (type=prompt 시)

  DialogInfo info;
  const std::string* type = params.FindString("type");
  info.type = type ? *type : "alert";

  const std::string* message = params.FindString("message");
  info.message = message ? *message : "";

  const std::string* url = params.FindString("url");
  info.url = url ? *url : "";

  const std::string* default_prompt = params.FindString("defaultPrompt");
  info.default_prompt = default_prompt ? *default_prompt : "";

  current_dialog_ = std::move(info);

  LOG(INFO) << "[DialogTool] 다이얼로그 열림: type=" << current_dialog_->type
            << ", message=" << current_dialog_->message;

  // autoHandle 모드가 활성화된 경우 자동으로 처리한다.
  if (auto_handle_ && registered_session_) {
    bool do_accept = (auto_action_ == "accept");
    LOG(INFO) << "[DialogTool] autoHandle: 자동 "
              << (do_accept ? "accept" : "dismiss") << " 처리";

    base::DictValue handle_params;
    handle_params.Set("accept", do_accept);

    // prompt 다이얼로그는 기본값으로 수락
    if (do_accept && current_dialog_->type == "prompt" &&
        !current_dialog_->default_prompt.empty()) {
      handle_params.Set("promptText", current_dialog_->default_prompt);
    }

    registered_session_->SendCdpCommand(
        "Page.handleJavaScriptDialog", std::move(handle_params),
        base::BindOnce([](base::Value response) {
          // autoHandle 자동 처리 응답은 로그만 남기고 무시
          LOG(INFO) << "[DialogTool] autoHandle 처리 완료";
        }));
  }
}

void DialogTool::OnJavaScriptDialogClosed(
    const std::string& event_name,
    const base::DictValue& params) {
  // 다이얼로그가 닫히면 내부 상태를 초기화한다.
  LOG(INFO) << "[DialogTool] 다이얼로그 닫힘";
  current_dialog_.reset();
}

void DialogTool::OnHandleDialogResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    base::DictValue err;
    err.Set("error", "Page.handleJavaScriptDialog 응답 형식 오류");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  // CDP 오류 응답 확인
  const base::DictValue* err_dict = response.GetDict().FindDict("error");
  if (err_dict) {
    const std::string* msg = err_dict->FindString("message");
    std::string err_msg = msg ? *msg : "다이얼로그 처리 실패";
    LOG(ERROR) << "[DialogTool] 다이얼로그 처리 오류: " << err_msg;
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", err_msg);
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[DialogTool] 다이얼로그 처리 성공";
  base::DictValue result;
  result.Set("success", true);
  result.Set("message", "다이얼로그를 처리했습니다");
  std::move(callback).Run(base::Value(std::move(result)));
}

}  // namespace mcp
