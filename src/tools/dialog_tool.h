// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_DIALOG_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_DIALOG_TOOL_H_

#include <optional>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// JavaScript 다이얼로그(alert/confirm/prompt/beforeunload)를 처리하는 도구.
//
// 동작 방식:
//   1. 세션 생성 시 Page.javascriptDialogOpening / Closed 이벤트 핸들러를
//      자동으로 등록한다 (EnsureEventHandlerRegistered).
//   2. action=getInfo : 현재 열린 다이얼로그 정보를 반환한다.
//   3. action=accept  : Page.handleJavaScriptDialog(accept=true) 호출.
//                       promptText 파라미터가 있으면 prompt 입력값으로 전달.
//   4. action=dismiss : Page.handleJavaScriptDialog(accept=false) 호출.
//
// autoHandle / autoAction 파라미터:
//   autoHandle=true  로 설정하면 다이얼로그가 열릴 때마다 자동으로
//   autoAction(accept 또는 dismiss)에 따라 처리한다.
//   autoHandle=false 로 설정하면 자동 처리를 해제한다.
class DialogTool : public McpTool {
 public:
  DialogTool();
  ~DialogTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 이벤트 핸들러를 아직 등록하지 않았거나 세션이 교체된 경우에만 등록한다.
  void EnsureEventHandlerRegistered(McpSession* session);

  // Page.javascriptDialogOpening 이벤트 핸들러.
  // 열린 다이얼로그의 type/message/url/defaultPrompt를
  // current_dialog_에 저장한다. autoHandle 모드가 활성화된 경우
  // 즉시 autoAction에 따라 Page.handleJavaScriptDialog를 호출한다.
  void OnJavaScriptDialogOpening(const std::string& event_name,
                                 const base::DictValue& params);

  // Page.javascriptDialogClosed 이벤트 핸들러.
  // 다이얼로그가 닫히면 current_dialog_를 초기화한다.
  void OnJavaScriptDialogClosed(const std::string& event_name,
                                const base::DictValue& params);

  // Page.handleJavaScriptDialog CDP 응답 처리.
  void OnHandleDialogResponse(base::OnceCallback<void(base::Value)> callback,
                              base::Value response);

  // 현재 열린 다이얼로그 정보 구조체
  struct DialogInfo {
    DialogInfo();
    ~DialogInfo();
    DialogInfo(DialogInfo&&);
    DialogInfo& operator=(DialogInfo&&);
    std::string type;            // "alert", "confirm", "prompt", "beforeunload"
    std::string message;         // 다이얼로그 메시지 본문
    std::string url;             // 다이얼로그를 트리거한 페이지 URL
    std::string default_prompt;  // prompt 다이얼로그의 기본 입력값
  };

  // 현재 열린 다이얼로그 정보 (다이얼로그가 없으면 nullopt)
  std::optional<DialogInfo> current_dialog_;

  // autoHandle 모드 활성화 여부
  bool auto_handle_ = false;

  // autoHandle 모드에서 사용할 동작: "accept"(기본) 또는 "dismiss"
  std::string auto_action_ = "accept";

  // Page 이벤트 핸들러 등록 여부 (중복 등록 방지)
  bool event_handler_registered_ = false;

  // 이벤트 핸들러가 등록된 세션 포인터 (교체 감지용 raw pointer)
  McpSession* registered_session_ = nullptr;

  base::WeakPtrFactory<DialogTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_DIALOG_TOOL_H_
