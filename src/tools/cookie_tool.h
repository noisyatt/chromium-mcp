// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_COOKIE_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_COOKIE_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// CookieTool: 쿠키 조회, 설정, 삭제, 전체 삭제를 수행하는 도구.
//
// action 파라미터에 따라 아래 CDP 명령을 호출한다:
//   get    → Network.getCookies(urls)
//   set    → Network.setCookie(name, value, domain, path, ...)
//   delete → Network.deleteCookies(name, url/domain)
//   clear  → Network.clearBrowserCookies
//
// ★ 내부 CDP 세션을 사용하므로 노란 배너가 표시되지 않는다.
// ★ Network.enable을 호출하지 않으므로 안티봇 탐지를 유발하지 않는다.
class CookieTool : public McpTool {
 public:
  CookieTool();
  ~CookieTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // action=get 처리: Network.getCookies(urls) 호출.
  // |url|이 있으면 해당 URL에 적용되는 쿠키만 반환.
  // 없으면 모든 쿠키 반환.
  void HandleGet(const std::string& url,
                 McpSession* session,
                 base::OnceCallback<void(base::Value)> callback);

  // action=set 처리: Network.setCookie 호출.
  // name, value는 필수. domain/path/secure/httpOnly/sameSite/expirationDate는 선택.
  void HandleSet(const base::Value::Dict& arguments,
                 McpSession* session,
                 base::OnceCallback<void(base::Value)> callback);

  // action=delete 처리: Network.deleteCookies 호출.
  // name은 필수. url 또는 domain으로 범위 지정.
  void HandleDelete(const base::Value::Dict& arguments,
                    McpSession* session,
                    base::OnceCallback<void(base::Value)> callback);

  // action=clear 처리: Network.clearBrowserCookies 호출.
  // 현재 브라우저 세션의 모든 쿠키를 삭제한다.
  void HandleClear(McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // Network.getCookies CDP 응답 처리.
  // cookies 배열을 JSON으로 직렬화하여 반환.
  void OnGetCookiesResponse(base::OnceCallback<void(base::Value)> callback,
                            base::Value response);

  // Network.setCookie CDP 응답 처리.
  // success 필드를 확인하여 결과 반환.
  void OnSetCookieResponse(base::OnceCallback<void(base::Value)> callback,
                           base::Value response);

  // Network.deleteCookies / Network.clearBrowserCookies CDP 응답 처리.
  // 에러가 없으면 성공 메시지 반환.
  void OnSimpleCommandResponse(const std::string& success_message,
                               base::OnceCallback<void(base::Value)> callback,
                               base::Value response);

  // 약한 참조 팩토리 (비동기 CDP 콜백에서 this 댕글링 방지)
  base::WeakPtrFactory<CookieTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_COOKIE_TOOL_H_
