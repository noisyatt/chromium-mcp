// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_BROWSER_INFO_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_BROWSER_INFO_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// BrowserInfoTool: 브라우저의 기본 정보를 반환하는 MCP 도구.
//
// 입력 파라미터 없이 호출하면 다음 정보를 반환한다:
//   - version       : Chromium 버전 문자열 (예: "120.0.6099.130")
//   - userAgent     : 현재 브라우저의 User-Agent 헤더 문자열
//   - activeTab     : 현재 활성 탭의 id, url, title 정보
//   - tabCount      : 현재 열려있는 총 탭 수 (모든 창 합산)
//   - windowCount   : 현재 열려있는 Browser 창 수
//   - platform      : 운영체제 플랫폼 문자열
//
// 사용하는 Chromium 헤더:
//   - components/version_info/version_info.h : 버전 정보
//   - chrome/browser/browser_process.h       : g_browser_process 전역 접근
//   - chrome/browser/ui/browser_list.h       : 활성 브라우저/탭 접근
//   - content/public/browser/web_contents.h  : 탭 URL/제목 접근
class BrowserInfoTool : public McpTool {
 public:
  BrowserInfoTool();
  ~BrowserInfoTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  bool requires_session() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 활성 탭 정보를 수집하여 base::DictValue으로 반환.
  // 활성 브라우저가 없거나 탭이 없으면 빈 딕셔너리 반환.
  static base::DictValue GetActiveTabInfo();

  // 모든 Browser 창의 총 탭 수를 반환.
  static int GetTotalTabCount();

  // 현재 플랫폼 문자열 반환 (예: "Windows", "Mac", "Linux").
  static std::string GetPlatformString();

  // User-Agent 문자열 반환.
  // content::GetUserAgent() 또는 g_browser_process->GetUserAgent() 활용.
  static std::string GetUserAgentString();

  // 약한 참조 팩토리 (비동기 콜백에서 this 댕글링 방지)
  base::WeakPtrFactory<BrowserInfoTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_BROWSER_INFO_TOOL_H_
