// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/bookmark_tool.h"

#include <string>
#include <utility>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/bookmarks/bookmark_model_factory.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "components/bookmarks/browser/bookmark_model.h"
#include "components/bookmarks/browser/bookmark_node.h"
#include "components/bookmarks/browser/bookmark_utils.h"
#include "components/query_parser/query_parser.h"
#include "components/bookmarks/browser/titled_url_match.h"
#include "url/gurl.h"

namespace mcp {

BookmarkTool::BookmarkTool() = default;
BookmarkTool::~BookmarkTool() = default;

// -----------------------------------------------------------------------
// McpTool 인터페이스 구현
// -----------------------------------------------------------------------

std::string BookmarkTool::name() const {
  return "bookmarks";
}

std::string BookmarkTool::description() const {
  return "북마크 조회, 추가, 삭제, 검색";
}

base::DictValue BookmarkTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue props;

  // action: 수행할 작업 (필수)
  {
    base::DictValue p;
    p.Set("type", "string");
    base::ListValue e;
    e.Append("list");
    e.Append("add");
    e.Append("remove");
    e.Append("search");
    e.Append("move");
    p.Set("enum", std::move(e));
    p.Set("description",
          "수행할 작업:\n"
          "  list   : 폴더의 북마크 목록 조회\n"
          "  add    : 새 북마크 추가 (url 필수)\n"
          "  remove : 북마크 삭제 (bookmarkId 필수)\n"
          "  search : 키워드로 북마크 검색 (query 필수)\n"
          "  move   : 북마크를 다른 폴더로 이동 (bookmarkId, destinationFolderId 필수)");
    props.Set("action", std::move(p));
  }

  // url: 북마크할 URL (add 시 필수)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "북마크할 URL (action=add 시 필수)");
    props.Set("url", std::move(p));
  }

  // title: 북마크 제목 (add 시 사용)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "북마크 제목 (action=add 시 사용; 생략 시 URL로 대체)");
    props.Set("title", std::move(p));
  }

  // folderId: 부모 폴더 ID (list/add 시 사용)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description",
          "부모 폴더 ID (action=list/add 시 사용; 생략 시 북마크 바 루트)");
    props.Set("folderId", std::move(p));
  }

  // query: 검색어 (search 시 필수)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "검색 키워드 (action=search 시 필수; 제목/URL에서 부분 일치)");
    props.Set("query", std::move(p));
  }

  // bookmarkId: 북마크 ID (remove/move 시 필수)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "북마크 ID (action=remove/move 시 필수)");
    props.Set("bookmarkId", std::move(p));
  }

  // destinationFolderId: 이동 목적지 폴더 ID (move 시 필수)
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "이동 목적지 폴더 ID (action=move 시 필수)");
    props.Set("destinationFolderId", std::move(p));
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
void BookmarkTool::Execute(const base::DictValue& arguments,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback) {
  const std::string* action_ptr = arguments.FindString("action");
  if (!action_ptr) {
    std::move(callback).Run(MakeErrorResult("action 파라미터가 필요합니다"));
    return;
  }

  const std::string& action = *action_ptr;
  LOG(INFO) << "[BookmarkTool] Execute action=" << action;

  bookmarks::BookmarkModel* model = GetBookmarkModel(session);
  if (!model) {
    std::move(callback).Run(MakeErrorResult("BookmarkModel을 가져올 수 없습니다 (프로파일 오류)"));
    return;
  }

  // 모델이 아직 로드되지 않은 경우 대기 없이 오류 반환
  if (!model->loaded()) {
    std::move(callback).Run(MakeErrorResult("BookmarkModel이 아직 로드되지 않았습니다. 잠시 후 다시 시도하세요."));
    return;
  }

  base::Value result;

  if (action == "list") {
    // folderId 또는 parentId (하위 호환) 중 첫 번째로 존재하는 값 사용
    const std::string* fid = arguments.FindString("folderId");
    if (!fid) fid = arguments.FindString("parentId");
    result = ExecuteList(fid ? *fid : "", model);

  } else if (action == "add") {
    const std::string* url = arguments.FindString("url");
    if (!url || url->empty()) {
      std::move(callback).Run(MakeErrorResult("add 액션에는 url 파라미터가 필요합니다"));
      return;
    }
    const std::string* title = arguments.FindString("title");
    const std::string* fid   = arguments.FindString("folderId");
    if (!fid) fid = arguments.FindString("parentId");
    result = ExecuteAdd(fid ? *fid : "",
                        *url,
                        title ? *title : *url,
                        model);

  } else if (action == "remove") {
    const std::string* bid = arguments.FindString("bookmarkId");
    if (!bid || bid->empty()) {
      std::move(callback).Run(MakeErrorResult("remove 액션에는 bookmarkId 파라미터가 필요합니다"));
      return;
    }
    result = ExecuteRemove(*bid, model);

  } else if (action == "search") {
    const std::string* q = arguments.FindString("query");
    if (!q || q->empty()) {
      std::move(callback).Run(MakeErrorResult("search 액션에는 query 파라미터가 필요합니다"));
      return;
    }
    result = ExecuteSearch(*q, model);

  } else if (action == "move") {
    const std::string* bid  = arguments.FindString("bookmarkId");
    const std::string* dest = arguments.FindString("destinationFolderId");
    if (!bid || bid->empty() || !dest || dest->empty()) {
      std::move(callback).Run(MakeErrorResult(
              "move 액션에는 bookmarkId, destinationFolderId 파라미터가 필요합니다"));
      return;
    }
    result = ExecuteMove(*bid, *dest, model);

  } else {
    std::move(callback).Run(MakeErrorResult("알 수 없는 action: " + action));
    return;
  }

  std::move(callback).Run(std::move(result));
}
// -----------------------------------------------------------------------
// action="list"
// -----------------------------------------------------------------------
base::Value BookmarkTool::ExecuteList(const std::string& folder_id,
                                       bookmarks::BookmarkModel* model) {
  const bookmarks::BookmarkNode* parent = nullptr;

  if (folder_id.empty()) {
    // 부모 지정 없으면 북마크 바 루트 반환
    parent = model->bookmark_bar_node();
  } else {
    int64_t id = ParseNodeId(folder_id);
    if (id < 0) {
      return MakeErrorResult("유효하지 않은 folderId: " + folder_id);
    }
    parent = bookmarks::GetBookmarkNodeByID(model, id);
    if (!parent) {
      return MakeErrorResult("해당 ID 의 북마크 폴더를 찾을 수 없음: " + folder_id);
    }
  }

  LOG(INFO) << "[BookmarkTool] list 폴더 id=" << (folder_id.empty() ? "bar" : folder_id)
            << " 자식 수=" << parent->children().size();

  // 자식 노드 직렬화 (1 depth)
  base::ListValue children;
  for (const auto& child : parent->children()) {
    children.Append(SerializeNode(child.get(), /*recursive=*/false));
  }

  base::DictValue result;
  result.Set("success",  true);
  result.Set("folderId", folder_id.empty() ? "bar" : folder_id);
  result.Set("count",    static_cast<int>(children.size()));
  result.Set("items",    std::move(children));
  return MakeJsonResult(std::move(result));
}

// -----------------------------------------------------------------------
// action="add"
// -----------------------------------------------------------------------
base::Value BookmarkTool::ExecuteAdd(const std::string& parent_id,
                                      const std::string& url,
                                      const std::string& title,
                                      bookmarks::BookmarkModel* model) {
  GURL gurl(url);
  if (!gurl.is_valid()) {
    return MakeErrorResult("유효하지 않은 URL: " + url);
  }

  // 부모 노드 결정
  const bookmarks::BookmarkNode* parent = nullptr;
  if (parent_id.empty()) {
    parent = model->bookmark_bar_node();
  } else {
    int64_t pid = ParseNodeId(parent_id);
    if (pid < 0) {
      return MakeErrorResult("유효하지 않은 folderId: " + parent_id);
    }
    parent = bookmarks::GetBookmarkNodeByID(model, pid);
    if (!parent || !parent->is_folder()) {
      return MakeErrorResult("해당 ID 의 폴더를 찾을 수 없음: " + parent_id);
    }
  }

  // BookmarkModel::AddURL 로 북마크 추가
  // position: 폴더의 마지막에 추가
  const bookmarks::BookmarkNode* new_node = model->AddURL(
      parent,
      parent->children().size(),
      base::UTF8ToUTF16(title),
      gurl);

  if (!new_node) {
    return MakeErrorResult("북마크 추가 실패");
  }

  LOG(INFO) << "[BookmarkTool] 북마크 추가 완료 id="
            << new_node->id() << " url=" << url;

  base::DictValue result;
  result.Set("success",  true);
  result.Set("bookmark", SerializeNode(new_node, /*recursive=*/false));
  return MakeJsonResult(std::move(result));
}

// -----------------------------------------------------------------------
// action="remove"
// -----------------------------------------------------------------------
base::Value BookmarkTool::ExecuteRemove(const std::string& bookmark_id,
                                         bookmarks::BookmarkModel* model) {
  int64_t id = ParseNodeId(bookmark_id);
  if (id < 0) {
    return MakeErrorResult("유효하지 않은 bookmarkId: " + bookmark_id);
  }

  const bookmarks::BookmarkNode* node =
      bookmarks::GetBookmarkNodeByID(model, id);
  if (!node) {
    return MakeErrorResult("해당 ID 의 북마크를 찾을 수 없음: " + bookmark_id);
  }

  // 루트 노드는 삭제할 수 없다.
  if (model->is_permanent_node(node)) {
    return MakeErrorResult("루트 폴더(영구 노드)는 삭제할 수 없습니다");
  }

  LOG(INFO) << "[BookmarkTool] 북마크 삭제 id=" << id;

  // BookmarkModel::Remove 로 노드 삭제 (폴더면 재귀적으로 삭제)
  model->Remove(node, bookmarks::metrics::BookmarkEditSource::kUser,
                FROM_HERE);

  base::DictValue result;
  result.Set("success",    true);
  result.Set("bookmarkId", bookmark_id);
  result.Set("message",    "북마크가 삭제되었습니다");
  return MakeJsonResult(std::move(result));
}

// -----------------------------------------------------------------------
// action="search"
// -----------------------------------------------------------------------
base::Value BookmarkTool::ExecuteSearch(const std::string& query,
                                         bookmarks::BookmarkModel* model) {
  // BookmarkModel::GetBookmarksMatching 으로 제목/URL 대상 검색
  std::vector<bookmarks::TitledUrlMatch> matches;
  bookmarks::QueryFields fields;
  fields.title = std::make_unique<std::u16string>(base::UTF8ToUTF16(query));
  // URL 도 함께 검색
  fields.url = std::make_unique<std::u16string>(base::UTF8ToUTF16(query));

  // GetBookmarksMatching: 기본 50개 최대
  matches = model->GetBookmarksMatching(
      base::UTF8ToUTF16(query), /*max_count=*/50,
      query_parser::MatchingAlgorithm::DEFAULT);

  LOG(INFO) << "[BookmarkTool] 검색 완료 query='" << query
            << "' 결과=" << matches.size();

  base::ListValue items;
  for (const auto& match : matches) {
    if (match.node) {
      items.Append(SerializeNode(
          static_cast<const bookmarks::BookmarkNode*>(match.node.get()),
          /*recursive=*/false));
    }
  }

  base::DictValue result;
  result.Set("success", true);
  result.Set("query",   query);
  result.Set("count",   static_cast<int>(items.size()));
  result.Set("items",   std::move(items));
  return MakeJsonResult(std::move(result));
}

// -----------------------------------------------------------------------
// action="move"
// -----------------------------------------------------------------------
base::Value BookmarkTool::ExecuteMove(const std::string& bookmark_id,
                                       const std::string& destination_folder_id,
                                       bookmarks::BookmarkModel* model) {
  int64_t bid = ParseNodeId(bookmark_id);
  int64_t did = ParseNodeId(destination_folder_id);

  if (bid < 0) {
    return MakeErrorResult("유효하지 않은 bookmarkId: " + bookmark_id);
  }
  if (did < 0) {
    return MakeErrorResult("유효하지 않은 destinationFolderId: " + destination_folder_id);
  }

  const bookmarks::BookmarkNode* node =
      bookmarks::GetBookmarkNodeByID(model, bid);
  if (!node) {
    return MakeErrorResult("이동할 북마크를 찾을 수 없음: " + bookmark_id);
  }

  const bookmarks::BookmarkNode* dest =
      bookmarks::GetBookmarkNodeByID(model, did);
  if (!dest || !dest->is_folder()) {
    return MakeErrorResult("목적지 폴더를 찾을 수 없음: " + destination_folder_id);
  }

  // 영구 노드(루트)는 이동 불가
  if (model->is_permanent_node(node)) {
    return MakeErrorResult("루트 폴더(영구 노드)는 이동할 수 없습니다");
  }

  // 자기 자신의 폴더로 이동 방지
  if (node->parent() == dest) {
    return MakeErrorResult("이미 해당 폴더에 있습니다");
  }
  // 자손으로 이동 방지 (순환 참조)
  if (dest->HasAncestor(node)) {
    return MakeErrorResult("하위 폴더로 이동할 수 없습니다 (순환 참조)");
  }

  LOG(INFO) << "[BookmarkTool] 북마크 이동 id=" << bid
            << " → folder=" << did;

  // BookmarkModel::Move: 목적지 폴더의 마지막 위치로 이동
  model->Move(node, dest, dest->children().size());

  base::DictValue result;
  result.Set("success",             true);
  result.Set("bookmarkId",          bookmark_id);
  result.Set("destinationFolderId", destination_folder_id);
  result.Set("bookmark",            SerializeNode(node, /*recursive=*/false));
  return MakeJsonResult(std::move(result));
}

// -----------------------------------------------------------------------
// 정적 헬퍼: BookmarkModel 획득
// -----------------------------------------------------------------------
// static
bookmarks::BookmarkModel* BookmarkTool::GetBookmarkModel(McpSession* session) {
  if (!session) return nullptr;

  Browser* browser = chrome::FindLastActive();
  if (!browser) {
    LOG(ERROR) << "[BookmarkTool] 활성 Browser 를 찾을 수 없음";
    return nullptr;
  }
  Profile* profile = browser->profile();
  if (!profile) {
    LOG(ERROR) << "[BookmarkTool] Profile 을 가져올 수 없음";
    return nullptr;
  }

  // BookmarkModelFactory::GetForBrowserContext 는 Profile 을 인자로 받는다.
  bookmarks::BookmarkModel* model =
      BookmarkModelFactory::GetForBrowserContext(profile);
  if (!model) {
    LOG(ERROR) << "[BookmarkTool] BookmarkModel 을 가져올 수 없음";
  }
  return model;
}

// -----------------------------------------------------------------------
// 정적 헬퍼: string ID → int64_t
// -----------------------------------------------------------------------
// static
int64_t BookmarkTool::ParseNodeId(const std::string& id_str) {
  if (id_str.empty()) return -1;
  int64_t id = -1;
  if (!base::StringToInt64(id_str, &id)) return -1;
  return id;
}

// -----------------------------------------------------------------------
// 정적 헬퍼: BookmarkNode → base::DictValue
// -----------------------------------------------------------------------
// static
base::DictValue BookmarkTool::SerializeNode(
    const bookmarks::BookmarkNode* node,
    bool recursive) {
  base::DictValue d;
  if (!node) return d;

  d.Set("id",       base::NumberToString(node->id()));
  d.Set("title",    base::UTF16ToUTF8(node->GetTitle()));
  d.Set("isFolder", node->is_folder());
  d.Set("dateAdded",
        static_cast<double>(node->date_added().InMillisecondsSinceUnixEpoch()));

  if (!node->is_folder()) {
    d.Set("url", node->url().spec());
  }

  if (node->parent()) {
    d.Set("parentId", base::NumberToString(node->parent()->id()));
  }

  if (recursive && node->is_folder()) {
    base::ListValue children;
    for (const auto& child : node->children()) {
      children.Append(SerializeNode(child.get(), /*recursive=*/true));
    }
    d.Set("children", std::move(children));
    d.Set("childCount", static_cast<int>(node->children().size()));
  } else if (node->is_folder()) {
    d.Set("childCount", static_cast<int>(node->children().size()));
  }

  return d;
}

// -----------------------------------------------------------------------
// 정적 헬퍼: 노드 포인터 목록 → base::ListValue
// -----------------------------------------------------------------------
// static
base::ListValue BookmarkTool::SerializeNodeList(
    const std::vector<const bookmarks::BookmarkNode*>& nodes) {
  base::ListValue list;
  for (const auto* node : nodes) {
    list.Append(SerializeNode(node, /*recursive=*/false));
  }
  return list;
}

}  // namespace mcp
