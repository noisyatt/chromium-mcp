// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/file_upload_tool.h"

#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

std::string FileUploadTool::name() const {
  return "file_upload";
}

std::string FileUploadTool::description() const {
  return "파일 입력 요소에 파일 업로드";
}

base::Value::Dict FileUploadTool::input_schema() const {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // selector: file input 요소를 찾는 CSS 선택자 (필수)
  // 예: "input[type=file]", "#file-upload", ".upload-btn"
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    prop.Set("description",
             "파일 입력 요소의 CSS 선택자 "
             "(예: 'input[type=file]', '#file-input', '.upload-field')");
    properties.Set("selector", std::move(prop));
  }

  // filePaths: 업로드할 파일의 절대 경로 배열 (필수)
  // 각 경로는 Chromium 프로세스가 접근 가능한 로컬 절대 경로여야 한다.
  {
    base::Value::Dict prop;
    prop.Set("type", "array");
    base::Value::Dict items;
    items.Set("type", "string");
    prop.Set("items", std::move(items));
    prop.Set("description",
             "업로드할 파일의 절대 경로 배열 "
             "(예: [\"/home/user/photo.jpg\", \"/home/user/report.pdf\"])");
    prop.Set("minItems", 1);
    properties.Set("filePaths", std::move(prop));
  }

  schema.Set("properties", std::move(properties));

  // selector와 filePaths 모두 필수 파라미터
  base::Value::List required;
  required.Append("selector");
  required.Append("filePaths");
  schema.Set("required", std::move(required));

  return schema;
}

void FileUploadTool::Execute(const base::Value::Dict& arguments,
                              McpSession* session,
                              base::OnceCallback<void(base::Value)> callback) {
  // -------------------------------------------------------------------------
  // 파라미터 추출 및 검증
  // -------------------------------------------------------------------------
  const std::string* selector_ptr = arguments.FindString("selector");
  if (!selector_ptr || selector_ptr->empty()) {
    LOG(WARNING) << "[FileUploadTool] selector 파라미터 없음";
    base::Value::Dict err;
    err.Set("error", "selector 파라미터가 필요합니다");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  // filePaths 배열 추출
  const base::Value::List* paths_list = arguments.FindList("filePaths");
  if (!paths_list || paths_list->empty()) {
    LOG(WARNING) << "[FileUploadTool] filePaths 파라미터 없거나 비어있음";
    base::Value::Dict err;
    err.Set("error",
            "filePaths 파라미터가 필요합니다 (비어있지 않은 문자열 배열)");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  // 배열에서 유효한 문자열 경로만 수집
  std::vector<std::string> file_paths;
  for (const base::Value& v : *paths_list) {
    if (v.is_string() && !v.GetString().empty()) {
      file_paths.push_back(v.GetString());
    }
  }

  if (file_paths.empty()) {
    base::Value::Dict err;
    err.Set("error", "filePaths 배열에 유효한 파일 경로가 없습니다");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  LOG(INFO) << "[FileUploadTool] Execute, selector=" << *selector_ptr
            << ", filePaths count=" << file_paths.size();

  // 1단계: DOM.getDocument로 처리 시작
  FetchDocument(*selector_ptr, std::move(file_paths), session,
                std::move(callback));
}

// -----------------------------------------------------------------------------
// 1단계: DOM.getDocument — 문서 루트 nodeId 획득
// -----------------------------------------------------------------------------

void FileUploadTool::FetchDocument(
    const std::string& selector,
    std::vector<std::string> file_paths,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // depth=0: 루트 노드 정보만 필요 (자식 노드 트리 불필요)
  // pierce=false: Shadow DOM 내부 탐색 제외
  base::Value::Dict params;
  params.Set("depth", 0);
  params.Set("pierce", false);

  LOG(INFO) << "[FileUploadTool] DOM.getDocument 호출";

  session->SendCdpCommand(
      "DOM.getDocument", std::move(params),
      base::BindOnce(&FileUploadTool::OnDocumentReceived,
                     weak_factory_.GetWeakPtr(), selector,
                     std::move(file_paths), session, std::move(callback)));
}

void FileUploadTool::OnDocumentReceived(
    const std::string& selector,
    std::vector<std::string> file_paths,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // 응답 형식: {"result": {"root": {"nodeId": 1, ...}}}
  if (!response.is_dict()) {
    LOG(ERROR) << "[FileUploadTool] DOM.getDocument 응답 형식 오류";
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", "DOM.getDocument 응답 형식 오류");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::Value::Dict& dict = response.GetDict();

  // CDP 오류 확인
  const base::Value::Dict* error_dict = dict.FindDict("error");
  if (error_dict) {
    const std::string* msg = error_dict->FindString("message");
    std::string err_msg = msg ? *msg : "DOM.getDocument CDP 오류";
    LOG(ERROR) << "[FileUploadTool] DOM.getDocument 오류: " << err_msg;
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", err_msg);
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // result.root.nodeId 추출
  const base::Value::Dict* result_dict = dict.FindDict("result");
  if (!result_dict) {
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", "DOM.getDocument 응답에 result 필드가 없습니다");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::Value::Dict* root_dict = result_dict->FindDict("root");
  if (!root_dict) {
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", "DOM.getDocument 응답에 root 필드가 없습니다");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  std::optional<int> node_id_opt = root_dict->FindInt("nodeId");
  if (!node_id_opt.has_value()) {
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", "문서 루트 nodeId를 찾을 수 없습니다");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  int root_node_id = *node_id_opt;
  LOG(INFO) << "[FileUploadTool] 문서 루트 nodeId=" << root_node_id;

  // 2단계: CSS 선택자로 file input 요소 검색
  QuerySelector(root_node_id, selector, std::move(file_paths), session,
                std::move(callback));
}

// -----------------------------------------------------------------------------
// 2단계: DOM.querySelector — 선택자로 file input nodeId 획득
// -----------------------------------------------------------------------------

void FileUploadTool::QuerySelector(
    int root_node_id,
    const std::string& selector,
    std::vector<std::string> file_paths,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::Value::Dict params;
  params.Set("nodeId", root_node_id);
  params.Set("selector", selector);

  LOG(INFO) << "[FileUploadTool] DOM.querySelector 호출, selector=" << selector;

  session->SendCdpCommand(
      "DOM.querySelector", std::move(params),
      base::BindOnce(&FileUploadTool::OnQuerySelectorResult,
                     weak_factory_.GetWeakPtr(), selector,
                     std::move(file_paths), session, std::move(callback)));
}

void FileUploadTool::OnQuerySelectorResult(
    const std::string& selector,
    std::vector<std::string> file_paths,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // 응답 형식: {"result": {"nodeId": 42}}
  // nodeId=0 이면 요소를 찾지 못한 것
  if (!response.is_dict()) {
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", "DOM.querySelector 응답 형식 오류");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::Value::Dict& dict = response.GetDict();

  const base::Value::Dict* error_dict = dict.FindDict("error");
  if (error_dict) {
    const std::string* msg = error_dict->FindString("message");
    std::string err_msg = msg ? *msg : "DOM.querySelector CDP 오류";
    LOG(ERROR) << "[FileUploadTool] DOM.querySelector 오류: " << err_msg;
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", err_msg);
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::Value::Dict* result_dict = dict.FindDict("result");
  if (!result_dict) {
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", "DOM.querySelector 응답에 result 필드가 없습니다");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  std::optional<int> node_id_opt = result_dict->FindInt("nodeId");
  if (!node_id_opt.has_value() || *node_id_opt == 0) {
    // nodeId=0: 요소를 찾지 못함
    LOG(WARNING) << "[FileUploadTool] 선택자에 해당하는 요소 없음: "
                 << selector;
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error",
               "선택자 '" + selector + "'에 해당하는 요소를 찾을 수 없습니다");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  int node_id = *node_id_opt;
  LOG(INFO) << "[FileUploadTool] file input 요소 발견, nodeId=" << node_id;

  // 3단계: 파일 경로 배열을 file input에 설정
  SetFileInputFiles(node_id, selector, std::move(file_paths), session,
                    std::move(callback));
}

// -----------------------------------------------------------------------------
// 3단계: DOM.setFileInputFiles — file input에 파일 경로 배열 설정
// -----------------------------------------------------------------------------

void FileUploadTool::SetFileInputFiles(
    int node_id,
    const std::string& selector,
    std::vector<std::string> file_paths,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // DOM.setFileInputFiles 파라미터:
  //   files  : 설정할 파일 경로 배열 (절대 경로)
  //   nodeId : 대상 file input 요소의 nodeId
  base::Value::Dict params;

  base::Value::List file_list;
  for (const std::string& path : file_paths) {
    file_list.Append(path);
    LOG(INFO) << "[FileUploadTool] 파일 경로 추가: " << path;
  }
  params.Set("files", std::move(file_list));
  params.Set("nodeId", node_id);

  LOG(INFO) << "[FileUploadTool] DOM.setFileInputFiles 호출, nodeId="
            << node_id << ", 파일 수=" << file_paths.size();

  session->SendCdpCommand(
      "DOM.setFileInputFiles", std::move(params),
      base::BindOnce(&FileUploadTool::OnSetFileInputFilesResult,
                     weak_factory_.GetWeakPtr(), selector,
                     std::move(file_paths), std::move(callback)));
}

void FileUploadTool::OnSetFileInputFilesResult(
    const std::string& selector,
    std::vector<std::string> file_paths,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // DOM.setFileInputFiles 성공 시 빈 result 객체가 반환된다.
  if (!response.is_dict()) {
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", "DOM.setFileInputFiles 응답 형식 오류");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::Value::Dict& dict = response.GetDict();

  const base::Value::Dict* error_dict = dict.FindDict("error");
  if (error_dict) {
    const std::string* msg = error_dict->FindString("message");
    std::string err_msg = msg ? *msg : "DOM.setFileInputFiles CDP 오류";
    LOG(ERROR) << "[FileUploadTool] 파일 설정 실패: " << err_msg;
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", err_msg);
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // 성공: 설정된 파일 정보를 응답에 포함
  LOG(INFO) << "[FileUploadTool] 파일 설정 완료, selector=" << selector
            << ", 파일 수=" << file_paths.size();

  base::Value::Dict result;
  result.Set("success", true);
  result.Set("selector", selector);

  // 설정된 파일 경로 목록
  base::Value::List uploaded;
  for (const std::string& path : file_paths) {
    uploaded.Append(path);
  }
  result.Set("filePaths", std::move(uploaded));
  result.Set("fileCount", static_cast<int>(file_paths.size()));
  result.Set("message",
             std::to_string(file_paths.size()) +
             "개 파일이 성공적으로 설정되었습니다");

  std::move(callback).Run(base::Value(std::move(result)));
}

}  // namespace mcp
