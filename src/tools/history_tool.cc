// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/history_tool.h"

#include <utility>

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/history/history_service_factory.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"
#include "url/gurl.h"

namespace mcp {

HistoryTool::HistoryTool() = default;
HistoryTool::~HistoryTool() = default;

// -----------------------------------------------------------------------
// McpTool 인터페이스 구현
// -----------------------------------------------------------------------

std::string HistoryTool::name() const {
  return "history";
}

std::string HistoryTool::description() const {
  return "브라우저 방문 기록 조회 및 삭제";
}

base::DictValue HistoryTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue props;

  // action: 수행할 작업 (필수)
  {
    base::DictValue p;
    p.Set("type", "string");
    base::ListValue e;
    e.Append("search");
    e.Append("delete");
    e.Append("deleteRange");
    e.Append("deleteAll");
    p.Set("enum", std::move(e));
    p.Set("description",
          "수행할 작업:\n"
          "  search      : 키워드/시간 범위로 방문 기록 검색\n"
          "  delete      : 특정 URL 방문 기록 삭제\n"
          "  deleteRange : 시간 범위 내 방문 기록 일괄 삭제\n"
          "  deleteAll   : 전체 방문 기록 삭제");
    props.Set("action", std::move(p));
  }

  // query: 검색어 (action=search 시 사용)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description",
          "검색 키워드 (action=search 에서 사용; 빈 문자열이면 최신 항목 반환)");
    props.Set("query", std::move(p));
  }

  // maxResults: 최대 결과 수 (기본 50)
  {
    base::DictValue p;
    p.Set("type", "number");
    p.Set("description", "반환할 최대 결과 수 (기본값: 50)");
    p.Set("default", 50);
    props.Set("maxResults", std::move(p));
  }

  // startTime: 검색/삭제 시작 시간 (ISO 8601)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description",
          "검색/삭제 시작 시간 (ISO 8601 형식, 예: 2024-01-01T00:00:00Z)");
    props.Set("startTime", std::move(p));
  }

  // endTime: 검색/삭제 종료 시간 (ISO 8601)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description",
          "검색/삭제 종료 시간 (ISO 8601 형식, 예: 2024-12-31T23:59:59Z)");
    props.Set("endTime", std::move(p));
  }

  // url: 삭제할 특정 URL (action=delete 시 필수)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "삭제할 URL (action=delete 시 필수)");
    props.Set("url", std::move(p));
  }

  schema.Set("properties", std::move(props));

  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

// -----------------------------------------------------------------------
// Execute
// -----------------------------------------------------------------------
void HistoryTool::Execute(const base::DictValue& arguments,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback) {
  const std::string* action_ptr = arguments.FindString("action");
  if (!action_ptr) {
    std::move(callback).Run(MakeErrorResult("action 파라미터가 필요합니다"));
    return;
  }

  const std::string& action = *action_ptr;
  LOG(INFO) << "[HistoryTool] Execute action=" << action;

  if (action == "search") {
    const std::string* q = arguments.FindString("query");
    std::string query = q ? *q : "";
    int max_results = arguments.FindInt("maxResults").value_or(50);
    const std::string* st = arguments.FindString("startTime");
    const std::string* et = arguments.FindString("endTime");
    ExecuteSearch(query, max_results,
                  st ? *st : "",
                  et ? *et : "",
                  session, std::move(callback));

  } else if (action == "delete") {
    const std::string* url = arguments.FindString("url");
    if (!url || url->empty()) {
      std::move(callback).Run(MakeErrorResult("delete 액션에는 url 파라미터가 필요합니다"));
      return;
    }
    ExecuteDelete(*url, session, std::move(callback));

  } else if (action == "deleteRange") {
    const std::string* st = arguments.FindString("startTime");
    const std::string* et = arguments.FindString("endTime");
    if (!st || !et) {
      std::move(callback).Run(MakeErrorResult(
          "deleteRange 액션에는 startTime, endTime 파라미터가 필요합니다"));
      return;
    }
    ExecuteDeleteRange(*st, *et, session, std::move(callback));

  } else if (action == "deleteAll") {
    ExecuteDeleteAll(session, std::move(callback));

  } else {
    std::move(callback).Run(MakeErrorResult("알 수 없는 action: " + action));
  }
}

// -----------------------------------------------------------------------
// action="search"
// -----------------------------------------------------------------------
void HistoryTool::ExecuteSearch(
    const std::string& query,
    int max_results,
    const std::string& start_time,
    const std::string& end_time,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {

  history::HistoryService* hs = GetHistoryService(session);
  if (!hs) {
    std::move(callback).Run(MakeErrorResult(
        "HistoryService를 가져올 수 없습니다 (프로파일 오류)"));
    return;
  }

  // 검색 옵션 구성
  history::QueryOptions options;
  options.max_count = max_results;

  // 시간 범위 설정
  if (!start_time.empty()) {
    base::Time t = ParseIsoTime(start_time);
    if (!t.is_null()) options.begin_time = t;
  }
  if (!end_time.empty()) {
    base::Time t = ParseIsoTime(end_time);
    if (!t.is_null()) options.end_time = t;
  }

  LOG(INFO) << "[HistoryTool] QueryHistory query='" << query
            << "' max=" << max_results;

  // HistoryService::QueryHistory 비동기 호출
  // 결과는 UI 스레드에서 OnQueryHistoryResult 콜백으로 수신된다.
  hs->QueryHistory(
      base::UTF8ToUTF16(query), options,
      base::BindOnce(&HistoryTool::OnQueryHistoryResult,
                     weak_factory_.GetWeakPtr(), std::move(callback)),
      &task_tracker_);
}

// -----------------------------------------------------------------------
// action="delete"
// -----------------------------------------------------------------------
void HistoryTool::ExecuteDelete(const std::string& url,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  history::HistoryService* hs = GetHistoryService(session);
  if (!hs) {
    std::move(callback).Run(MakeErrorResult("HistoryService를 가져올 수 없습니다"));
    return;
  }

  GURL gurl(url);
  if (!gurl.is_valid()) {
    std::move(callback).Run(MakeErrorResult("유효하지 않은 URL: " + url));
    return;
  }

  LOG(INFO) << "[HistoryTool] DeleteURL url=" << url;

  // HistoryService::DeleteURL 은 동기적으로 삭제를 스케줄링한다.
  // 완료 통지가 필요하다면 HistoryObserver 를 사용해야 하지만,
  // 여기서는 호출 후 즉시 성공 응답을 반환한다.
  hs->DeleteURLs({gurl});

  base::DictValue result;
  result.Set("success", true);
  result.Set("url", url);
  result.Set("message", "URL 방문 기록이 삭제 대기열에 추가되었습니다");
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

// -----------------------------------------------------------------------
// action="deleteRange"
// -----------------------------------------------------------------------
void HistoryTool::ExecuteDeleteRange(
    const std::string& start_time,
    const std::string& end_time,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {

  history::HistoryService* hs = GetHistoryService(session);
  if (!hs) {
    std::move(callback).Run(MakeErrorResult("HistoryService를 가져올 수 없습니다"));
    return;
  }

  base::Time t_start = ParseIsoTime(start_time);
  base::Time t_end   = ParseIsoTime(end_time);

  if (t_start.is_null()) {
    std::move(callback).Run(MakeErrorResult("startTime 파싱 실패: " + start_time));
    return;
  }
  if (t_end.is_null()) {
    std::move(callback).Run(MakeErrorResult("endTime 파싱 실패: " + end_time));
    return;
  }

  LOG(INFO) << "[HistoryTool] ExpireHistoryBetween start=" << start_time
            << " end=" << end_time;

  // ExpireHistoryBetween: 시작~종료 시간 범위의 모든 방문 기록 삭제
  // 빈 restrict_urls 집합 → 모든 URL 삭제
  hs->ExpireHistoryBetween(
      /*restrict_urls=*/{},
      /*restrict_app_id=*/std::nullopt,
      t_start, t_end,
      /*user_initiated=*/true,
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb,
             const std::string& st, const std::string& et) {
            base::DictValue result;
            result.Set("success",   true);
            result.Set("startTime", st);
            result.Set("endTime",   et);
            result.Set("message",   "시간 범위 방문 기록이 삭제되었습니다");
            std::move(cb).Run(MakeJsonResult(std::move(result)));
          },
          std::move(callback), start_time, end_time),
      &task_tracker_);
}

// -----------------------------------------------------------------------
// action="deleteAll"
// -----------------------------------------------------------------------
void HistoryTool::ExecuteDeleteAll(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {

  history::HistoryService* hs = GetHistoryService(session);
  if (!hs) {
    std::move(callback).Run(MakeErrorResult("HistoryService를 가져올 수 없습니다"));
    return;
  }

  LOG(INFO) << "[HistoryTool] ExpireHistoryBetween (전체 삭제)";

  // base::Time() ~ base::Time::Max() 범위를 삭제하면 전체 히스토리 삭제됨
  hs->ExpireHistoryBetween(
      /*restrict_urls=*/{},
      /*restrict_app_id=*/std::nullopt,
      base::Time(), base::Time::Max(),
      /*user_initiated=*/true,
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb) {
            base::DictValue result;
            result.Set("success", true);
            result.Set("message", "전체 방문 기록이 삭제되었습니다");
            std::move(cb).Run(MakeJsonResult(std::move(result)));
          },
          std::move(callback)),
      &task_tracker_);
}

// -----------------------------------------------------------------------
// QueryHistory 완료 콜백
// -----------------------------------------------------------------------
void HistoryTool::OnQueryHistoryResult(
    base::OnceCallback<void(base::Value)> callback,
    history::QueryResults results) {

  LOG(INFO) << "[HistoryTool] QueryHistory 결과 수=" << results.size();

  base::DictValue out;
  out.Set("success", true);
  out.Set("total",   static_cast<int>(results.size()));
  out.Set("items",   SerializeResults(results));
  std::move(callback).Run(MakeJsonResult(std::move(out)));
}

// -----------------------------------------------------------------------
// 정적 헬퍼: HistoryService 획득
// -----------------------------------------------------------------------
// static
history::HistoryService* HistoryTool::GetHistoryService(McpSession* session) {
  if (!session) return nullptr;

  // 다른 도구들과 동일한 패턴으로 활성 브라우저에서 Profile 획득.
  Browser* browser = chrome::FindLastActive();
  if (!browser) {
    LOG(ERROR) << "[HistoryTool] 활성 Browser 를 찾을 수 없음";
    return nullptr;
  }
  Profile* profile = browser->profile();
  if (!profile) {
    LOG(ERROR) << "[HistoryTool] Profile 을 가져올 수 없음";
    return nullptr;
  }

  // HistoryServiceFactory 를 통해 HistoryService 포인터 획득
  // EXPLICIT_ACCESS: 사용자가 명시적으로 접근하는 경우에 사용
  history::HistoryService* hs = HistoryServiceFactory::GetForProfile(
      profile, ServiceAccessType::EXPLICIT_ACCESS);

  if (!hs) {
    LOG(ERROR) << "[HistoryTool] HistoryService 를 가져올 수 없음";
  }
  return hs;
}

// -----------------------------------------------------------------------
// 정적 헬퍼: ISO 8601 → base::Time 변환
// -----------------------------------------------------------------------
// static
base::Time HistoryTool::ParseIsoTime(const std::string& iso) {
  if (iso.empty()) return base::Time();

  base::Time result;
  // base::Time::FromString 은 여러 형식을 지원한다.
  // ISO 8601 형식 (e.g. "2024-01-15T12:30:00Z")을 처리할 수 있다.
  if (base::Time::FromString(iso.c_str(), &result)) {
    return result;
  }

  LOG(WARNING) << "[HistoryTool] 시간 파싱 실패: " << iso;
  return base::Time();
}

// -----------------------------------------------------------------------
// 정적 헬퍼: QueryResults → base::ListValue 직렬화
// -----------------------------------------------------------------------
// static
base::ListValue HistoryTool::SerializeResults(
    const history::QueryResults& results) {
  base::ListValue list;

  for (const history::URLResult& item : results) {
    base::DictValue entry;
    entry.Set("url",         item.url().spec());
    entry.Set("title",       base::UTF16ToUTF8(item.title()));
    entry.Set("visitCount",  static_cast<int>(item.visit_count()));
    entry.Set("typedCount",  static_cast<int>(item.typed_count()));
    entry.Set("lastVisited",
              static_cast<double>(
                  item.last_visit().InMillisecondsSinceUnixEpoch()));
    entry.Set("hidden",      item.hidden());
    list.Append(std::move(entry));
  }

  return list;
}

}  // namespace mcp
