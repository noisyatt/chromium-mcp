// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_BOOKMARK_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_BOOKMARK_TOOL_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"

namespace mcp {

// 북마크 조회, 추가, 삭제, 검색, 이동 도구.
//
// Chrome 내부 API(bookmarks::BookmarkModel)를 직접 사용하여
// CDP Runtime.evaluate 없이 북마크를 관리한다.
//
// 접근 방법:
//   BookmarkModelFactory::GetForBrowserContext(session->GetProfile())
//
// 지원 액션:
//   list    : 특정 폴더 또는 전체 북마크 트리 조회
//   add     : 새 북마크 추가 (URL + 제목 + 부모 폴더)
//   remove  : 북마크 삭제 (bookmarkId 로 지정)
//   search  : 키워드로 북마크 검색 (제목/URL 대상)
//   move    : 북마크를 다른 폴더로 이동
//
// 관련 헤더:
//   chrome/browser/bookmarks/bookmark_model_factory.h
//   components/bookmarks/browser/bookmark_model.h
//   components/bookmarks/browser/bookmark_node.h
//
// 파라미터 매핑:
//   - folderId / parentId 모두 허용 (같은 의미; folderId 우선)
//   - bookmarkId 는 int64 를 string 으로 직렬화한 값
class BookmarkTool : public McpTool {
 public:
  BookmarkTool();
  ~BookmarkTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // -----------------------------------------------------------------------
  // 액션별 처리 메서드 (모두 동기 완료)
  // -----------------------------------------------------------------------

  // action="list": 폴더 내 북마크 목록 또는 전체 트리 조회.
  //   folder_id: 빈 문자열이면 북마크 바 루트부터 반환
  base::Value ExecuteList(const std::string& folder_id,
                          bookmarks::BookmarkModel* model);

  // action="add": 새 북마크 추가.
  //   parent_id: 부모 폴더 ID (빈 문자열이면 북마크 바)
  //   url      : 북마크할 URL
  //   title    : 북마크 제목
  base::Value ExecuteAdd(const std::string& parent_id,
                         const std::string& url,
                         const std::string& title,
                         bookmarks::BookmarkModel* model);

  // action="remove": 북마크 삭제.
  //   bookmark_id: 삭제할 북마크의 int64 ID (string 형식)
  base::Value ExecuteRemove(const std::string& bookmark_id,
                            bookmarks::BookmarkModel* model);

  // action="search": 키워드로 북마크 검색.
  //   query: 검색어 (제목 또는 URL에 부분 일치)
  base::Value ExecuteSearch(const std::string& query,
                            bookmarks::BookmarkModel* model);

  // action="move": 북마크를 다른 폴더로 이동.
  //   bookmark_id          : 이동할 북마크 ID
  //   destination_folder_id: 목적지 폴더 ID
  base::Value ExecuteMove(const std::string& bookmark_id,
                          const std::string& destination_folder_id,
                          bookmarks::BookmarkModel* model);

  // -----------------------------------------------------------------------
  // 내부 헬퍼
  // -----------------------------------------------------------------------

  // 프로파일에서 BookmarkModel 포인터를 획득한다.
  static bookmarks::BookmarkModel* GetBookmarkModel(McpSession* session);

  // string 형식의 ID를 int64_t 로 변환한다. 실패 시 -1 반환.
  static int64_t ParseNodeId(const std::string& id_str);

  // BookmarkNode 를 base::DictValue 으로 직렬화한다.
  //   recursive: true 이면 children 도 재귀적으로 포함
  static base::DictValue SerializeNode(
      const bookmarks::BookmarkNode* node,
      bool recursive = false);

  // 노드 목록을 base::ListValue 로 직렬화한다.
  static base::ListValue SerializeNodeList(
      const std::vector<const bookmarks::BookmarkNode*>& nodes);

  base::WeakPtrFactory<BookmarkTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_BOOKMARK_TOOL_H_
