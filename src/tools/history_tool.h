// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_HISTORY_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_HISTORY_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/task/cancelable_task_tracker.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "components/history/core/browser/history_service.h"
#include "components/history/core/browser/history_types.h"

namespace mcp {

// 브라우저 방문 기록 조회 및 삭제 도구.
//
// Chrome 내부 API(history::HistoryService)를 직접 사용하여
// CDP Runtime.evaluate 없이 히스토리를 조작한다.
//
// 접근 방법:
//   Browser::profile() → HistoryServiceFactory::GetForProfile(profile,
//                          ServiceAccessType::EXPLICIT_ACCESS)
//
// 지원 액션:
//   search      : 키워드/시간 범위로 방문 기록 검색
//   delete      : 특정 URL 방문 기록 삭제
//   deleteRange : ISO 시간 범위 내 방문 기록 일괄 삭제
//   deleteAll   : 전체 방문 기록 삭제
//
// 관련 헤더:
//   chrome/browser/history/history_service_factory.h
//   components/history/core/browser/history_service.h
//   components/history/core/browser/history_types.h
class HistoryTool : public McpTool {
 public:
  HistoryTool() = default;
  ~HistoryTool() override = default;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // -----------------------------------------------------------------------
  // 액션별 처리 메서드
  // -----------------------------------------------------------------------

  // action="search": HistoryService::QueryHistory() 로 방문 기록 검색.
  //   query       : 검색 키워드 (빈 문자열이면 최신 순 반환)
  //   max_results : 최대 결과 수 (기본 50)
  //   start_time  : 검색 시작 시간 (빈 문자열 가능)
  //   end_time    : 검색 종료 시간 (빈 문자열 가능)
  void ExecuteSearch(const std::string& query,
                     int max_results,
                     const std::string& start_time,
                     const std::string& end_time,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // action="delete": HistoryService::DeleteURL() 로 특정 URL 삭제.
  //   url: 삭제할 URL (필수)
  void ExecuteDelete(const std::string& url,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // action="deleteRange": HistoryService::ExpireHistoryBetween() 으로
  //   시간 범위 내 방문 기록 삭제.
  //   start_time, end_time: ISO 8601 형식 시간 문자열
  void ExecuteDeleteRange(const std::string& start_time,
                          const std::string& end_time,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback);

  // action="deleteAll": HistoryService::ExpireHistoryBetween() 으로
  //   base::Time() ~ base::Time::Max() 범위를 삭제하여 전체 히스토리 삭제.
  void ExecuteDeleteAll(McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  // -----------------------------------------------------------------------
  // 내부 헬퍼
  // -----------------------------------------------------------------------

  // 프로파일에서 HistoryService 포인터를 획득한다.
  // 실패하면 nullptr 반환.
  //   session: 현재 McpSession (Browser/Profile 접근 경로)
  static history::HistoryService* GetHistoryService(McpSession* session);

  // ISO 8601 문자열을 base::Time 으로 변환한다.
  // 변환 실패 시 base::Time() (null time) 반환.
  static base::Time ParseIsoTime(const std::string& iso);

  // QueryResults 를 base::Value::List 로 직렬화한다.
  static base::Value::List SerializeResults(
      const history::QueryResults& results);

  // -----------------------------------------------------------------------
  // 비동기 콜백
  // -----------------------------------------------------------------------

  // HistoryService::QueryHistory 완료 콜백
  void OnQueryHistoryResult(base::OnceCallback<void(base::Value)> callback,
                            history::QueryResults results);

  // 비동기 취소 추적기 (QueryHistory 에 필요)
  base::CancelableTaskTracker task_tracker_;

  base::WeakPtrFactory<HistoryTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_HISTORY_TOOL_H_
