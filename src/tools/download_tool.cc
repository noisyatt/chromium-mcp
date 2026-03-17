// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/download_tool.h"

#include <utility>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

DownloadTool::DownloadTool() = default;
DownloadTool::~DownloadTool() = default;

std::string DownloadTool::name() const {
  return "download";
}

std::string DownloadTool::description() const {
  return "파일 다운로드 관리 (시작, 모니터링, 경로 설정)";
}

base::DictValue DownloadTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: 수행할 다운로드 작업 종류
  // start    - 새 다운로드 시작
  // list     - 현재 세션 다운로드 목록 조회
  // cancel   - 진행 중인 다운로드 취소
  // setPath  - 전역 다운로드 저장 경로 변경
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue enums;
    enums.Append("start");
    enums.Append("list");
    enums.Append("cancel");
    enums.Append("setPath");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "수행할 작업: start(시작), list(목록 조회), "
             "cancel(취소), setPath(경로 설정)");
    properties.Set("action", std::move(prop));
  }

  // url: 다운로드할 파일 URL (action=start 시 필수)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description", "다운로드할 파일 URL (action=start 시 필수)");
    properties.Set("url", std::move(prop));
  }

  // savePath: 로컬 저장 경로 (action=start 또는 setPath 시 사용)
  // start  : 파일명까지 포함한 전체 경로 또는 디렉토리 경로
  // setPath: 다운로드 기본 저장 디렉토리
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "로컬 저장 경로 (action=start 시 선택, action=setPath 시 필수). "
             "start 시 파일명 포함 전체 경로 또는 디렉토리.");
    properties.Set("savePath", std::move(prop));
  }

  // downloadId: 다운로드 ID (action=cancel 시 필수)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description", "대상 다운로드 ID (action=cancel 시 필수)");
    properties.Set("downloadId", std::move(prop));
  }

  schema.Set("properties", std::move(properties));

  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void DownloadTool::Execute(const base::DictValue& arguments,
                           McpSession* session,
                           base::OnceCallback<void(base::Value)> callback) {
  const std::string* action_ptr = arguments.FindString("action");
  if (!action_ptr) {
    base::DictValue err;
    err.Set("error", "action 파라미터가 필요합니다");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const std::string& action = *action_ptr;
  LOG(INFO) << "[DownloadTool] Execute action=" << action;

  if (action == "start") {
    const std::string* url_ptr = arguments.FindString("url");
    if (!url_ptr || url_ptr->empty()) {
      base::DictValue err;
      err.Set("error", "start 액션에는 url 파라미터가 필요합니다");
      std::move(callback).Run(base::Value(std::move(err)));
      return;
    }
    const std::string* path_ptr = arguments.FindString("savePath");
    ExecuteStart(*url_ptr, path_ptr ? *path_ptr : "", session,
                 std::move(callback));

  } else if (action == "list") {
    ExecuteList(session, std::move(callback));

  } else if (action == "cancel") {
    std::optional<int> id_opt = arguments.FindInt("downloadId");
    if (!id_opt.has_value()) {
      base::DictValue err;
      err.Set("error", "cancel 액션에는 downloadId 파라미터가 필요합니다");
      std::move(callback).Run(base::Value(std::move(err)));
      return;
    }
    ExecuteCancel(*id_opt, session, std::move(callback));

  } else if (action == "setPath") {
    const std::string* path_ptr = arguments.FindString("savePath");
    if (!path_ptr || path_ptr->empty()) {
      base::DictValue err;
      err.Set("error", "setPath 액션에는 savePath 파라미터가 필요합니다");
      std::move(callback).Run(base::Value(std::move(err)));
      return;
    }
    ExecuteSetPath(*path_ptr, session, std::move(callback));

  } else {
    base::DictValue err;
    err.Set("error", "알 수 없는 action: " + action);
    std::move(callback).Run(base::Value(std::move(err)));
  }
}

// -----------------------------------------------------------------------------
// ExecuteStart
// -----------------------------------------------------------------------------

void DownloadTool::ExecuteStart(const std::string& url,
                                const std::string& save_path,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  // 1단계: Page.setDownloadBehavior로 다운로드를 허용하고 저장 경로를 설정한다.
  //
  // behavior="allow": 다운로드 파일을 자동으로 downloadPath에 저장.
  // downloadPath: 디렉토리 경로여야 하므로, save_path에 파일명이 포함되어 있으면
  //               dirname만 추출하여 사용한다.
  base::DictValue params;
  params.Set("behavior", "allow");

  if (!save_path.empty()) {
    // 경로에서 디렉토리 부분을 추출한다.
    // 예: "/home/user/docs/file.pdf" → "/home/user/docs"
    std::string dir_path = save_path;
    auto last_sep = save_path.find_last_of("/\\");
    if (last_sep != std::string::npos) {
      dir_path = save_path.substr(0, last_sep);
    }
    params.Set("downloadPath", dir_path);
    LOG(INFO) << "[DownloadTool] 다운로드 저장 디렉토리: " << dir_path;
  }

  LOG(INFO) << "[DownloadTool] Page.setDownloadBehavior 설정, url=" << url;

  session->SendCdpCommand(
      "Page.setDownloadBehavior", std::move(params),
      base::BindOnce(&DownloadTool::OnDownloadBehaviorSet,
                     weak_factory_.GetWeakPtr(), url, save_path,
                     std::move(callback), session));
}

void DownloadTool::OnDownloadBehaviorSet(
    const std::string& url,
    const std::string& save_path,
    base::OnceCallback<void(base::Value)> callback,
    McpSession* session,
    base::Value response) {
  // setDownloadBehavior 결과는 오류가 있어도 계속 진행한다.
  // 일부 환경에서는 해당 명령을 지원하지 않을 수 있으므로 무시한다.
  if (response.is_dict()) {
    const base::DictValue* err_dict = response.GetDict().FindDict("error");
    if (err_dict) {
      const std::string* msg = err_dict->FindString("message");
      LOG(WARNING) << "[DownloadTool] setDownloadBehavior 경고: "
                   << (msg ? *msg : "알 수 없는 오류") << " (계속 진행)";
    }
  }

  // 2단계: Runtime.evaluate로 a 태그 클릭 패턴을 통해 다운로드를 트리거한다.
  //
  // 사용 패턴:
  //   1. <a href=URL download=filename> 생성
  //   2. document.body에 임시 추가
  //   3. a.click() 호출로 브라우저 다운로드 시작
  //   4. 임시 요소 제거
  //
  // save_path에 파일명이 포함된 경우 download 속성에 파일명만 설정한다.
  std::string filename;
  if (!save_path.empty()) {
    auto last_sep = save_path.find_last_of("/\\");
    if (last_sep != std::string::npos && last_sep + 1 < save_path.size()) {
      filename = save_path.substr(last_sep + 1);
    } else if (last_sep == std::string::npos) {
      // 구분자가 없으면 save_path 전체를 파일명으로 사용
      filename = save_path;
    }
  }

  // JS 내 작은따옴표 충돌 방지를 위해 큰따옴표 이스케이프 처리
  auto escape_js_str = [](const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      if (c == '\'') out += "\\'";
      else if (c == '\\') out += "\\\\";
      else out += c;
    }
    return out;
  };

  std::string js_code =
      "(function() {"
      "  try {"
      "    var a = document.createElement('a');"
      "    a.href = '" + escape_js_str(url) + "';"
      "    a.download = '" + escape_js_str(filename) + "';"
      "    a.style.display = 'none';"
      "    document.body.appendChild(a);"
      "    a.click();"
      "    document.body.removeChild(a);"
      "    return JSON.stringify({"
      "      success: true,"
      "      url: '" + escape_js_str(url) + "',"
      "      message: '다운로드가 시작되었습니다'"
      "    });"
      "  } catch (e) {"
      "    return JSON.stringify({ success: false, error: e.toString() });"
      "  }"
      "})()";

  base::DictValue eval_params;
  eval_params.Set("expression", js_code);
  eval_params.Set("returnByValue", true);

  LOG(INFO) << "[DownloadTool] 다운로드 트리거 실행, url=" << url;

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(eval_params),
      base::BindOnce(&DownloadTool::OnDownloadTriggered,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void DownloadTool::OnDownloadTriggered(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "예상치 못한 CDP 응답 형식");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* error_dict = dict.FindDict("error");
  if (error_dict) {
    const std::string* msg = error_dict->FindString("message");
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", msg ? *msg : "CDP 오류");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[DownloadTool] 다운로드 트리거 완료";

  base::DictValue result;
  result.Set("success", true);
  result.Set("message", "다운로드가 시작되었습니다");

  // Runtime.evaluate 결과에서 JS 반환값 추출
  const base::DictValue* result_dict = dict.FindDict("result");
  if (result_dict) {
    const std::string* value_str = result_dict->FindString("value");
    if (value_str) {
      result.Set("detail", *value_str);
    }
  }

  std::move(callback).Run(base::Value(std::move(result)));
}

// -----------------------------------------------------------------------------
// ExecuteList
// -----------------------------------------------------------------------------

void DownloadTool::ExecuteList(McpSession* session,
                               base::OnceCallback<void(base::Value)> callback) {
  // chrome.downloads.search API로 최근 50개 다운로드 목록을 조회한다.
  // 실제 내장 Chromium에서는 DownloadManager C++ API를 직접 사용하지만,
  // CDP 경유 구현에서는 Extension API를 활용한다.
  const std::string js_code =
      "(function() {"
      "  try {"
      "    if (typeof chrome !== 'undefined' && chrome.downloads) {"
      "      return new Promise(function(resolve) {"
      "        chrome.downloads.search("
      "          { limit: 50, orderBy: ['-startTime'] },"
      "          function(items) {"
      "            resolve(JSON.stringify({ success: true, downloads: items }));"
      "          }"
      "        );"
      "      });"
      "    } else {"
      "      return JSON.stringify({ success: true, downloads: [],"
      "        note: 'chrome.downloads API를 사용할 수 없습니다' });"
      "    }"
      "  } catch (e) {"
      "    return JSON.stringify({ success: false, error: e.toString() });"
      "  }"
      "})()";

  base::DictValue params;
  params.Set("expression", js_code);
  params.Set("awaitPromise", true);
  params.Set("returnByValue", true);

  LOG(INFO) << "[DownloadTool] 다운로드 목록 조회";

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&DownloadTool::OnCdpActionResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

// -----------------------------------------------------------------------------
// ExecuteCancel
// -----------------------------------------------------------------------------

void DownloadTool::ExecuteCancel(int download_id,
                                 McpSession* session,
                                 base::OnceCallback<void(base::Value)> callback) {
  // chrome.downloads.cancel API로 특정 다운로드를 취소한다.
  std::string js_code =
      "(function() {"
      "  var id = " + base::NumberToString(download_id) + ";"
      "  try {"
      "    if (typeof chrome !== 'undefined' && chrome.downloads) {"
      "      return new Promise(function(resolve) {"
      "        chrome.downloads.cancel(id, function() {"
      "          var err = chrome.runtime.lastError;"
      "          if (err) {"
      "            resolve(JSON.stringify({ success: false,"
      "              error: err.message }));"
      "          } else {"
      "            resolve(JSON.stringify({ success: true, downloadId: id,"
      "              message: '다운로드가 취소되었습니다' }));"
      "          }"
      "        });"
      "      });"
      "    } else {"
      "      return JSON.stringify({ success: false,"
      "        error: 'chrome.downloads API를 사용할 수 없습니다' });"
      "    }"
      "  } catch (e) {"
      "    return JSON.stringify({ success: false, error: e.toString() });"
      "  }"
      "})()";

  base::DictValue params;
  params.Set("expression", js_code);
  params.Set("awaitPromise", true);
  params.Set("returnByValue", true);

  LOG(INFO) << "[DownloadTool] 다운로드 취소, downloadId=" << download_id;

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&DownloadTool::OnCdpActionResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

// -----------------------------------------------------------------------------
// ExecuteSetPath
// -----------------------------------------------------------------------------

void DownloadTool::ExecuteSetPath(const std::string& path,
                                  McpSession* session,
                                  base::OnceCallback<void(base::Value)> callback) {
  // Browser.setDownloadBehavior 로 전역 다운로드 저장 경로를 변경한다.
  // Page.setDownloadBehavior는 현재 탭에만 적용되지만,
  // Browser.setDownloadBehavior는 모든 탭에 전역 적용된다.
  base::DictValue params;
  params.Set("behavior", "allow");
  params.Set("downloadPath", path);

  LOG(INFO) << "[DownloadTool] Browser.setDownloadBehavior, path=" << path;

  session->SendCdpCommand(
      "Browser.setDownloadBehavior", std::move(params),
      base::BindOnce(&DownloadTool::OnCdpActionResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

// -----------------------------------------------------------------------------
// 범용 CDP 응답 처리
// -----------------------------------------------------------------------------

void DownloadTool::OnCdpActionResult(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "예상치 못한 CDP 응답 형식");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::DictValue& dict = response.GetDict();

  const base::DictValue* error_dict = dict.FindDict("error");
  if (error_dict) {
    const std::string* msg = error_dict->FindString("message");
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", msg ? *msg : "CDP 오류");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // Runtime.evaluate 결과값이 있으면 data 필드로 포함
  const base::DictValue* result_dict = dict.FindDict("result");
  if (result_dict) {
    const std::string* value_str = result_dict->FindString("value");
    if (value_str) {
      base::DictValue result;
      result.Set("success", true);
      result.Set("data", *value_str);
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }
  }

  base::DictValue result;
  result.Set("success", true);
  std::move(callback).Run(base::Value(std::move(result)));
}

}  // namespace mcp
