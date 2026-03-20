// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_TAB_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_TAB_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace content {
class WebContents;
}  // namespace content

namespace mcp {

// TabsTool: 브라우저 탭을 관리하는 MCP 도구.
//
// CDP를 경유하지 않고 Chromium 내부 API를 직접 호출하여 탭을 제어한다.
//
// 지원하는 action:
//   - list  : 열려있는 모든 탭의 id, url, title, active 정보를 반환
//   - new   : 새 탭을 열고 url로 이동 (url 파라미터 선택적)
//   - close : tabId에 해당하는 탭을 닫음
//   - select: tabId에 해당하는 탭을 활성화(포커스)
//
// 내부 구현:
//   - list  : BrowserList 순회 → TabStripModel::count() / GetWebContentsAt()
//   - new   : NavigateParams + Navigate() 또는 chrome::AddSelectedTabWithURL()
//   - close : TabStripModel::CloseWebContentsAt()
//   - select: TabStripModel::ActivateTabAt()
//
// 모든 작업은 BrowserThread::UI에서 실행되어야 한다.
// WebContents의 ID로는 content::WebContents 포인터의 세션 스토리지에서
// 파생된 정수 ID를 사용한다 (DevTools 탭 ID와 동일).
class TabsTool : public McpTool {
 public:
  TabsTool();
  ~TabsTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  bool requires_session() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // action=list: 모든 브라우저의 모든 탭 목록을 직렬화하여 반환.
  // BrowserList를 순회하며 각 Browser의 TabStripModel에서 탭 정보를 수집.
  void HandleList(base::OnceCallback<void(base::Value)> callback);

  // action=new: 새 탭을 생성하고 지정한 URL로 이동.
  // |url|이 비어있으면 chrome://newtab 으로 이동.
  // NavigateParams를 구성하여 chrome::Navigate()를 호출한다.
  void HandleNew(const std::string& url,
                 base::OnceCallback<void(base::Value)> callback);

  // action=close: tabId에 해당하는 탭을 닫음.
  // BrowserList를 순회하며 WebContents ID가 일치하는 탭을 찾아
  // TabStripModel::CloseWebContentsAt() 호출.
  void HandleClose(int tab_id,
                   base::OnceCallback<void(base::Value)> callback);

  // action=select: tabId에 해당하는 탭을 활성화.
  // BrowserList를 순회하며 TabStripModel::ActivateTabAt() 호출.
  // 해당 Browser 창도 foreground로 올린다.
  void HandleSelect(int tab_id,
                    base::OnceCallback<void(base::Value)> callback);

  // WebContents 포인터로부터 MCP가 사용하는 정수 탭 ID를 반환.
  // content::WebContents::GetPrimaryMainFrame()의 process/routing ID 조합.
  static int GetTabId(content::WebContents* web_contents);

  // 단일 탭 정보를 base::DictValue으로 직렬화.
  // |tab_index|: TabStripModel 내 인덱스
  // |is_active|: 현재 활성 탭 여부
  static base::DictValue SerializeTab(content::WebContents* web_contents,
                                        int tab_index,
                                        bool is_active);

  // MCP 오류 응답 생성 헬퍼.
  static base::Value MakeError(const std::string& message);

  // MCP 성공 응답 생성 헬퍼 (텍스트 내용).
  static base::Value MakeSuccess(const std::string& text);

  // 약한 참조 팩토리 (비동기 콜백에서 this 댕글링 방지)
  base::WeakPtrFactory<TabsTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_TAB_TOOL_H_
