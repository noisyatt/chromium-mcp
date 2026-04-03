// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/tab_tool.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/lifetime/browser_shutdown.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_navigator.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_commands.h"

namespace mcp {

TabsTool::TabsTool() = default;
TabsTool::~TabsTool() = default;

bool TabsTool::requires_session() const {
  return false;
}

std::string TabsTool::name() const {
  return "tabs";
}

std::string TabsTool::description() const {
  return "브라우저 탭을 관리합니다. "
         "탭 목록 조회(list), 새 탭 생성(new), 탭 닫기(close), "
         "탭 전환(select) 작업을 지원합니다. "
         "CDP를 경유하지 않고 Chromium 내부 TabStripModel API를 직접 호출합니다.";
}

base::DictValue TabsTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: 필수 파라미터
  {
    base::DictValue action_prop;
    action_prop.Set("type", "string");
    base::ListValue action_enum;
    action_enum.Append("list");
    action_enum.Append("new");
    action_enum.Append("close");
    action_enum.Append("select");
    action_prop.Set("enum", std::move(action_enum));
    action_prop.Set("description",
                    "list: 탭 목록 조회, "
                    "new: 새 탭 생성, "
                    "close: 탭 닫기, "
                    "select: 탭 활성화");
    properties.Set("action", std::move(action_prop));
  }

  // tabId: close/select에 필요한 탭 ID
  {
    base::DictValue tab_id_prop;
    tab_id_prop.Set("type", "number");
    tab_id_prop.Set("description",
                    "대상 탭의 ID. action=close 또는 action=select 시 필수. "
                    "list 응답의 id 필드 값을 사용하세요.");
    properties.Set("tabId", std::move(tab_id_prop));
  }

  // url: new 탭 생성 시 이동할 URL
  {
    base::DictValue url_prop;
    url_prop.Set("type", "string");
    url_prop.Set("description",
                 "action=new 시 열 URL. "
                 "생략하면 chrome://newtab 으로 열립니다.");
    properties.Set("url", std::move(url_prop));
  }

  schema.Set("properties", std::move(properties));

  // action만 필수
  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void TabsTool::Execute(const base::DictValue& arguments,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback) {
  // action 파라미터 추출
  const std::string* action = arguments.FindString("action");
  if (!action || action->empty()) {
    LOG(WARNING) << "[MCP][Tabs] action 파라미터 누락";
    std::move(callback).Run(
        MakeError("action 파라미터가 필요합니다 (list/new/close/select)"));
    return;
  }

  if (*action == "list") {
    HandleList(std::move(callback));
  } else if (*action == "new") {
    const std::string* url = arguments.FindString("url");
    HandleNew(url ? *url : "", std::move(callback));
  } else if (*action == "close") {
    // tabId 파라미터 추출
    const base::Value* tab_id_val = arguments.Find("tabId");
    if (!tab_id_val || !tab_id_val->is_int()) {
      std::move(callback).Run(
          MakeError("action=close 에는 tabId(number) 파라미터가 필요합니다"));
      return;
    }
    HandleClose(tab_id_val->GetInt(), std::move(callback));
  } else if (*action == "select") {
    const base::Value* tab_id_val = arguments.Find("tabId");
    if (!tab_id_val || !tab_id_val->is_int()) {
      std::move(callback).Run(
          MakeError("action=select 에는 tabId(number) 파라미터가 필요합니다"));
      return;
    }
    HandleSelect(tab_id_val->GetInt(), std::move(callback));
  } else {
    LOG(WARNING) << "[MCP][Tabs] 알 수 없는 action: " << *action;
    std::move(callback).Run(
        MakeError("알 수 없는 action: '" + *action +
                  "'. list/new/close/select 중 하나를 사용하세요."));
  }
}

void TabsTool::HandleList(base::OnceCallback<void(base::Value)> callback) {
  // BrowserList를 순회하며 모든 탭 정보를 수집한다.
  // BrowserList는 현재 열려있는 모든 Browser 창의 목록이다.
  base::ListValue tabs_list;

  auto all_windows = GetAllBrowserWindowInterfaces();
  for (BrowserWindowInterface* bwi : all_windows) {
    Browser* browser = bwi->GetBrowserForMigrationOnly();
    if (!browser) continue;

    TabStripModel* tab_strip = browser->tab_strip_model();
    if (!tab_strip) {
      continue;
    }

    int active_index = tab_strip->active_index();
    int tab_count = tab_strip->count();

    for (int i = 0; i < tab_count; ++i) {
      content::WebContents* web_contents = tab_strip->GetWebContentsAt(i);
      if (!web_contents) {
        continue;
      }

      bool is_active = (i == active_index);
      base::DictValue tab_dict =
          SerializeTab(web_contents, i, is_active);

      // 어느 브라우저 창에 속하는지 식별 정보 추가
      tab_dict.Set("windowId",
                   static_cast<int>(reinterpret_cast<uintptr_t>(browser) &
                                    0x7FFFFFFF));

      tabs_list.Append(std::move(tab_dict));
    }
  }

  LOG(INFO) << "[MCP][Tabs] 탭 목록 반환: " << tabs_list.size() << "개";

  // 결과 직렬화
  base::DictValue data;
  data.Set("tabCount", static_cast<int>(tabs_list.size()));
  data.Set("tabs", std::move(tabs_list));

  std::string json_str;
  base::JSONWriter::WriteWithOptions(
      base::Value(std::move(data)),
      base::JSONWriter::OPTIONS_PRETTY_PRINT,
      &json_str);

  base::DictValue result;
  base::ListValue content;
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text", json_str);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));

  std::move(callback).Run(base::Value(std::move(result)));
}

void TabsTool::HandleNew(const std::string& url,
                         base::OnceCallback<void(base::Value)> callback) {
  // 현재 활성 Browser를 찾는다 (3단계 fallback).
  auto is_usable = [](Browser* b) -> bool {
    return b && b->is_type_normal() && b->tab_strip_model() &&
           !b->is_delete_scheduled();
  };

  Browser* browser = chrome::FindLastActive();
  if (!is_usable(browser)) {
    browser = nullptr;
  }

  // fallback 1: BrowserList에서 사용 가능한 창 찾기
  if (!browser) {
    for (Browser* b : *BrowserList::GetInstance()) {
      if (is_usable(b)) {
        browser = b;
        break;
      }
    }
  }

  // fallback 2: 새 창 생성 (종료 중이 아닐 때만)
  if (!browser) {
    Profile* profile = ProfileManager::GetLastUsedProfileIfLoaded();
    if (!profile || browser_shutdown::HasShutdownStarted()) {
      std::move(callback).Run(MakeError("브라우저 창을 생성할 수 없습니다"));
      return;
    }
    Browser::CreateParams params(profile, true);
    browser = Browser::Create(params);
    if (browser && browser->window()) {
      browser->window()->Show();
    }
    LOG(INFO) << "[MCP][Tabs] 새 브라우저 창 생성";
  }

  if (!is_usable(browser)) {
    std::move(callback).Run(MakeError("사용 가능한 브라우저 창이 없습니다"));
    return;
  }

  // 목적지 URL 결정: 비어있으면 새 탭 페이지
  GURL target_url(url.empty() ? "chrome://newtab" : url);
  if (!url.empty() && !target_url.is_valid()) {
    LOG(WARNING) << "[MCP][Tabs] 유효하지 않은 URL: " << url;
    std::move(callback).Run(MakeError("유효하지 않은 URL: " + url));
    return;
  }

  // about:blank 탭이 있으면 재사용 (새 탭 생성 대신 navigate)
  TabStripModel* tab_strip = browser->tab_strip_model();
  if (tab_strip && !url.empty()) {
    for (int i = 0; i < tab_strip->count(); ++i) {
      content::WebContents* wc = tab_strip->GetWebContentsAt(i);
      GURL visible_url = wc->GetVisibleURL();
      if (wc && (visible_url == GURL("about:blank") ||
                 visible_url == GURL("chrome://newtab/"))) {
        LOG(INFO) << "[MCP][Tabs] about:blank 탭 재사용 (index=" << i << ")";
        tab_strip->ActivateTabAt(i);
        content::NavigationController::LoadURLParams load_params(target_url);
        load_params.transition_type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
        wc->GetController().LoadURLWithParams(load_params);
        browser->window()->Activate();

        base::DictValue tab_info = SerializeTab(wc, i, true);
        std::string json_str;
        base::JSONWriter::WriteWithOptions(
            base::Value(std::move(tab_info)),
            base::JSONWriter::OPTIONS_PRETTY_PRINT, &json_str);
        base::DictValue result;
        base::DictValue content_item;
        content_item.Set("type", "text");
        content_item.Set("text", json_str);
        base::ListValue content;
        content.Append(std::move(content_item));
        result.Set("content", std::move(content));
        std::move(callback).Run(base::Value(std::move(result)));
        return;
      }
    }
  }

  // NavigateParams를 구성하여 새 탭으로 이동
  // NEW_FOREGROUND_TAB: 새 탭을 열고 즉시 포커스
  NavigateParams params(browser, target_url,
                        ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;

  LOG(INFO) << "[MCP][Tabs] 새 탭 생성: " << target_url.spec();

  // Navigate() 는 동기적으로 탭을 생성하고 네비게이션을 시작한다.
  // 네비게이션 완료(loadEventFired)는 기다리지 않는다.
  Navigate(&params);

  // 새로 생성된 WebContents 정보 반환
  content::WebContents* new_contents = params.navigated_or_inserted_contents;
  if (!new_contents) {
    // Navigate()가 탭을 생성했지만 포인터를 반환하지 않은 경우
    LOG(WARNING) << "[MCP][Tabs] 새 탭 WebContents 포인터를 얻지 못함";
    std::move(callback).Run(
        MakeSuccess("새 탭이 열렸습니다: " + target_url.spec()));
    return;
  }

  tab_strip = browser->tab_strip_model();
  int new_index = tab_strip->GetIndexOfWebContents(new_contents);
  base::DictValue tab_info =
      SerializeTab(new_contents, new_index, true);

  std::string json_str;
  base::JSONWriter::WriteWithOptions(
      base::Value(std::move(tab_info)),
      base::JSONWriter::OPTIONS_PRETTY_PRINT,
      &json_str);

  std::move(callback).Run(MakeSuccess("새 탭이 열렸습니다:\n" + json_str));
}

void TabsTool::HandleClose(int tab_id,
                           base::OnceCallback<void(base::Value)> callback) {
  // BrowserList를 순회하며 tabId가 일치하는 WebContents를 찾는다.
  auto all_windows = GetAllBrowserWindowInterfaces();
  for (BrowserWindowInterface* bwi : all_windows) {
    Browser* browser = bwi->GetBrowserForMigrationOnly();
    if (!browser) continue;

    TabStripModel* tab_strip = browser->tab_strip_model();
    if (!tab_strip) {
      continue;
    }

    int tab_count = tab_strip->count();
    for (int i = 0; i < tab_count; ++i) {
      content::WebContents* web_contents = tab_strip->GetWebContentsAt(i);
      if (!web_contents) {
        continue;
      }

      if (GetTabId(web_contents) == tab_id) {
        LOG(INFO) << "[MCP][Tabs] 탭 닫기: id=" << tab_id
                  << " url=" << web_contents->GetLastCommittedURL().spec();

        // CloseWebContentsAt(): 탭을 즉시 닫고 TabStrip에서 제거
        // CLOSE_USER_GESTURE: 사용자가 직접 닫는 것과 동일한 처리
        tab_strip->CloseWebContentsAt(i,
                                      TabCloseTypes::CLOSE_USER_GESTURE);

        std::move(callback).Run(
            MakeSuccess("탭을 닫았습니다 (id: " +
                        std::to_string(tab_id) + ")"));
        return;
      }
    }
  }

  // 해당 tabId를 찾지 못한 경우
  LOG(WARNING) << "[MCP][Tabs] tabId=" << tab_id << " 탭을 찾을 수 없음";
  std::move(callback).Run(
      MakeError("tabId=" + std::to_string(tab_id) +
                " 에 해당하는 탭을 찾을 수 없습니다. "
                "action=list로 현재 탭 목록을 확인하세요."));
}

void TabsTool::HandleSelect(int tab_id,
                            base::OnceCallback<void(base::Value)> callback) {
  // BrowserList를 순회하며 tabId가 일치하는 WebContents를 찾아 활성화한다.
  auto all_windows = GetAllBrowserWindowInterfaces();
  for (BrowserWindowInterface* bwi : all_windows) {
    Browser* browser = bwi->GetBrowserForMigrationOnly();
    if (!browser) continue;

    TabStripModel* tab_strip = browser->tab_strip_model();
    if (!tab_strip) {
      continue;
    }

    int tab_count = tab_strip->count();
    for (int i = 0; i < tab_count; ++i) {
      content::WebContents* web_contents = tab_strip->GetWebContentsAt(i);
      if (!web_contents) {
        continue;
      }

      if (GetTabId(web_contents) == tab_id) {
        LOG(INFO) << "[MCP][Tabs] 탭 활성화: id=" << tab_id
                  << " url=" << web_contents->GetLastCommittedURL().spec();

        // ActivateTabAt(): 지정한 인덱스의 탭을 활성 탭으로 전환
        // USER_GESTURE: 사용자 클릭과 동일한 처리로 포커스 이동 허용
        tab_strip->ActivateTabAt(i,
                                 TabStripUserGestureDetails(
                                     TabStripUserGestureDetails::GestureType::
                                         kOther));

        // Browser 창 자체도 foreground로 올린다.
        browser->window()->Activate();

        base::DictValue tab_info =
            SerializeTab(web_contents, i, true);
        std::string json_str;
        base::JSONWriter::WriteWithOptions(
            base::Value(std::move(tab_info)),
            base::JSONWriter::OPTIONS_PRETTY_PRINT,
            &json_str);

        std::move(callback).Run(
            MakeSuccess("탭을 활성화했습니다:\n" + json_str));
        return;
      }
    }
  }

  // 해당 tabId를 찾지 못한 경우
  LOG(WARNING) << "[MCP][Tabs] tabId=" << tab_id << " 탭을 찾을 수 없음";
  std::move(callback).Run(
      MakeError("tabId=" + std::to_string(tab_id) +
                " 에 해당하는 탭을 찾을 수 없습니다. "
                "action=list로 현재 탭 목록을 확인하세요."));
}

// static
int TabsTool::GetTabId(content::WebContents* web_contents) {
  // Chromium에서 탭 ID로 DevTools의 탭 ID와 동일하게
  // WebContents의 세션 스토리지 네임스페이스 ID를 활용한다.
  // 실제 Chromium 구현에서는 sessions::SessionTabHelper::IdForTab()을 사용.
  //
  // 여기서는 RenderFrameHost의 FrameTreeNodeId를 정수 ID로 사용한다.
  // 이는 프로세스 내에서 고유하며 재사용되지 않는다.
  if (!web_contents) {
    return -1;
  }

  content::RenderFrameHost* rfh = web_contents->GetPrimaryMainFrame();
  if (!rfh) {
    // RenderFrameHost가 없으면 포인터 주소의 하위 31비트를 ID로 사용
    return static_cast<int>(reinterpret_cast<uintptr_t>(web_contents) &
                            0x7FFFFFFF);
  }

  // FrameTreeNodeId: 브라우저 생명주기 동안 고유한 프레임 식별자
  // FrameTreeNodeId는 base::IdType<..., int32_t> 타입이므로
  // GetUnsafeValue()로 내부 int32_t 값을 꺼낸다.
  return static_cast<int>(rfh->GetFrameTreeNodeId().GetUnsafeValue());
}

// static
base::DictValue TabsTool::SerializeTab(
    content::WebContents* web_contents,
    int tab_index,
    bool is_active) {
  base::DictValue dict;

  dict.Set("id", GetTabId(web_contents));
  dict.Set("index", tab_index);
  dict.Set("active", is_active);

  // 현재 커밋된 URL (네비게이션 중이면 마지막으로 커밋된 URL)
  const GURL& url = web_contents->GetLastCommittedURL();
  dict.Set("url", url.is_valid() ? url.spec() : "");

  // 페이지 제목
  dict.Set("title", base::UTF16ToUTF8(web_contents->GetTitle()));

  // 로딩 상태
  dict.Set("isLoading", web_contents->IsLoading());

  // 음소거 상태 (오디오 관련)
  dict.Set("audible", web_contents->IsCurrentlyAudible());

  return dict;
}

// static
base::Value TabsTool::MakeError(const std::string& message) {
  base::DictValue result;
  result.Set("isError", true);
  base::ListValue content;
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text", "오류: " + message);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));
  return base::Value(std::move(result));
}

// static
base::Value TabsTool::MakeSuccess(const std::string& text) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue content_item;
  content_item.Set("type", "text");
  content_item.Set("text", text);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));
  return base::Value(std::move(result));
}

}  // namespace mcp
