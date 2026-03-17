// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/browser_info_tool.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/version_info/version_info.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace mcp {

BrowserInfoTool::BrowserInfoTool() = default;
BrowserInfoTool::~BrowserInfoTool() = default;

std::string BrowserInfoTool::name() const {
  return "browser_info";
}

std::string BrowserInfoTool::description() const {
  return "브라우저의 기본 정보를 반환합니다. "
         "버전, User-Agent, 현재 활성 탭(URL/제목), "
         "총 탭 수, 창 수, 플랫폼 정보를 포함합니다.";
}

base::Value::Dict BrowserInfoTool::input_schema() const {
  // 입력 파라미터 없음: 빈 스키마 반환
  base::Value::Dict schema;
  schema.Set("type", "object");
  schema.Set("properties", base::Value::Dict());
  return schema;
}

void BrowserInfoTool::Execute(
    const base::Value::Dict& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  LOG(INFO) << "[MCP][BrowserInfo] 브라우저 정보 수집 시작";

  base::Value::Dict info;

  // ── 버전 정보 ──────────────────────────────────────────────────────────────
  // version_info::GetVersionNumber(): "120.0.6099.130" 형태의 문자열 반환
  info.Set("version",
           std::string(version_info::GetVersionNumber()));

  // 전체 제품명 및 버전 (예: "Chromium/120.0.6099.130")
  info.Set("productName",
           std::string(version_info::GetProductNameAndVersionForUserAgent()));

  // 빌드 채널 (예: "stable", "beta", "dev", "canary")
  // version_info::GetChannelString(): 채널 이름 반환
  info.Set("channel",
           std::string(version_info::GetChannelString()));

  // ── User-Agent ─────────────────────────────────────────────────────────────
  info.Set("userAgent", GetUserAgentString());

  // ── 플랫폼 ────────────────────────────────────────────────────────────────
  info.Set("platform", GetPlatformString());

  // ── 창 및 탭 통계 ──────────────────────────────────────────────────────────
  const BrowserList* browser_list = BrowserList::GetInstance();
  int window_count = 0;
  if (browser_list) {
    // BrowserList::size()는 현재 열린 Browser 창 수
    window_count = static_cast<int>(browser_list->size());
  }
  info.Set("windowCount", window_count);
  info.Set("tabCount", GetTotalTabCount());

  // ── 활성 탭 정보 ──────────────────────────────────────────────────────────
  base::Value::Dict active_tab = GetActiveTabInfo();
  if (!active_tab.empty()) {
    info.Set("activeTab", std::move(active_tab));
  } else {
    // 활성 탭이 없으면 null 설정
    info.Set("activeTab", base::Value());
  }

  // ── 결과 직렬화 ────────────────────────────────────────────────────────────
  std::string json_str;
  base::JSONWriter::WriteWithOptions(
      base::Value(std::move(info)),
      base::JSONWriter::OPTIONS_PRETTY_PRINT,
      &json_str);

  LOG(INFO) << "[MCP][BrowserInfo] 브라우저 정보 수집 완료";

  base::Value::Dict result;
  base::Value::List content;
  base::Value::Dict content_item;
  content_item.Set("type", "text");
  content_item.Set("text", json_str);
  content.Append(std::move(content_item));
  result.Set("content", std::move(content));

  std::move(callback).Run(base::Value(std::move(result)));
}

// static
base::Value::Dict BrowserInfoTool::GetActiveTabInfo() {
  // BrowserList에서 가장 최근에 활성화된 Browser를 찾는다.
  // GetLastActive()는 포커스된 창을 반환한다.
  const BrowserList* browser_list = BrowserList::GetInstance();
  if (!browser_list) {
    return base::Value::Dict();
  }

  Browser* active_browser = browser_list->GetLastActive();
  if (!active_browser) {
    return base::Value::Dict();
  }

  TabStripModel* tab_strip = active_browser->tab_strip_model();
  if (!tab_strip) {
    return base::Value::Dict();
  }

  // 현재 활성 탭의 WebContents
  content::WebContents* active_contents = tab_strip->GetActiveWebContents();
  if (!active_contents) {
    return base::Value::Dict();
  }

  base::Value::Dict tab_info;

  // 탭 ID: RenderFrameHost의 FrameTreeNodeId를 사용
  content::RenderFrameHost* rfh = active_contents->GetPrimaryMainFrame();
  if (rfh) {
    tab_info.Set("id", rfh->GetFrameTreeNodeId());
  }

  // URL: 현재 커밋된 URL
  const GURL& url = active_contents->GetLastCommittedURL();
  tab_info.Set("url", url.is_valid() ? url.spec() : "");

  // 제목: UTF-16 → UTF-8 변환
  tab_info.Set("title",
               base::UTF16ToUTF8(active_contents->GetTitle()));

  // 로딩 상태
  tab_info.Set("isLoading", active_contents->IsLoading());

  // TabStrip 내 인덱스
  int active_index = tab_strip->active_index();
  tab_info.Set("index", active_index);

  return tab_info;
}

// static
int BrowserInfoTool::GetTotalTabCount() {
  const BrowserList* browser_list = BrowserList::GetInstance();
  if (!browser_list) {
    return 0;
  }

  int total = 0;
  for (Browser* browser : *browser_list) {
    if (!browser) {
      continue;
    }
    TabStripModel* tab_strip = browser->tab_strip_model();
    if (tab_strip) {
      // TabStripModel::count()는 현재 열린 탭 수를 반환
      total += tab_strip->count();
    }
  }
  return total;
}

// static
std::string BrowserInfoTool::GetPlatformString() {
  // build/build_config.h 의 BUILDFLAG를 사용하여 컴파일 타임에 플랫폼 결정
#if BUILDFLAG(IS_WIN)
  return "Windows";
#elif BUILDFLAG(IS_MAC)
  return "macOS";
#elif BUILDFLAG(IS_LINUX)
  return "Linux";
#elif BUILDFLAG(IS_CHROMEOS)
  return "ChromeOS";
#elif BUILDFLAG(IS_ANDROID)
  return "Android";
#elif BUILDFLAG(IS_IOS)
  return "iOS";
#else
  return "Unknown";
#endif
}

// static
std::string BrowserInfoTool::GetUserAgentString() {
  // g_browser_process는 브라우저 프로세스 전역 싱글톤이다.
  // GetUserAgent()가 없는 경우 content::BuildUserAgentFromProduct()를
  // 대신 사용할 수 있다.
  //
  // 여기서는 version_info를 활용하여 표준 형식의 UA를 수동 구성한다.
  // 실제 Chromium 빌드에서는 embedder_support::GetUserAgent()를 사용한다.
  if (g_browser_process) {
    // BrowserProcess::GetUserAgent()가 있으면 그것을 사용
    // (Chromium 빌드 설정에 따라 없을 수 있으므로 주석 처리)
    // return g_browser_process->GetUserAgent();
  }

  // version_info에서 버전 번호와 제품명을 조합하여 UA 문자열 생성
  // 표준 형식: "Mozilla/5.0 (...) AppleWebKit/... Chrome/VERSION Safari/..."
  std::string version = std::string(version_info::GetVersionNumber());

  std::string platform_ua;
#if BUILDFLAG(IS_WIN)
  platform_ua = "Windows NT 10.0; Win64; x64";
#elif BUILDFLAG(IS_MAC)
  platform_ua = "Macintosh; Intel Mac OS X 10_15_7";
#elif BUILDFLAG(IS_LINUX)
  platform_ua = "X11; Linux x86_64";
#elif BUILDFLAG(IS_CHROMEOS)
  platform_ua = "X11; CrOS x86_64";
#else
  platform_ua = "Unknown";
#endif

  return "Mozilla/5.0 (" + platform_ua +
         ") AppleWebKit/537.36 (KHTML, like Gecko) Chrome/" + version +
         " Safari/537.36";
}

}  // namespace mcp
