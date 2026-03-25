// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/file_upload_tool.h"

#include <optional>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

FileUploadTool::FileUploadTool() = default;
FileUploadTool::~FileUploadTool() = default;

std::string FileUploadTool::name() const {
  return "file_upload";
}

std::string FileUploadTool::description() const {
  return "파일 입력 요소에 파일을 업로드합니다. "
         "role/name, text, selector, xpath, ref 등 다양한 방법으로 "
         "file input 요소를 찾아 파일 경로를 설정합니다.";
}

base::DictValue FileUploadTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // ---- 공통 로케이터 파라미터 ----

  // role: ARIA 역할
  base::DictValue role_prop;
  role_prop.Set("type", "string");
  role_prop.Set("description",
                "file input 요소의 ARIA 역할 (일반적으로 \"button\" 또는 없음). "
                "name 파라미터와 함께 사용합니다.");
  properties.Set("role", std::move(role_prop));

  // name: 접근성 이름
  base::DictValue name_prop;
  name_prop.Set("type", "string");
  name_prop.Set("description",
                "file input 요소의 접근성 이름. role 파라미터와 함께 사용합니다.");
  properties.Set("name", std::move(name_prop));

  // text: 표시 텍스트
  base::DictValue text_prop;
  text_prop.Set("type", "string");
  text_prop.Set("description",
                "요소의 표시 텍스트로 탐색. exact=false이면 부분 일치 허용.");
  properties.Set("text", std::move(text_prop));

  // selector: file input 요소를 찾는 CSS 선택자
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "파일 입력 요소의 CSS 선택자 "
             "(예: 'input[type=file]', '#file-input', '.upload-field')");
    properties.Set("selector", std::move(prop));
  }

  // xpath: XPath 표현식
  base::DictValue xpath_prop;
  xpath_prop.Set("type", "string");
  xpath_prop.Set("description",
                 "file input 요소의 XPath 표현식.");
  properties.Set("xpath", std::move(xpath_prop));

  // ref: backendNodeId 참조
  base::DictValue ref_prop;
  ref_prop.Set("type", "string");
  ref_prop.Set("description",
               "접근성 스냅샷 또는 element 도구에서 얻은 요소 ref (backendNodeId).");
  properties.Set("ref", std::move(ref_prop));

  // exact: 텍스트/이름 정확히 일치 여부
  base::DictValue exact_prop;
  exact_prop.Set("type", "boolean");
  exact_prop.Set("default", false);
  exact_prop.Set("description",
                 "true이면 name/text 파라미터를 정확히 일치, "
                 "false이면 부분 문자열 일치로 탐색 (기본: false).");
  properties.Set("exact", std::move(exact_prop));

  // filePaths: 업로드할 파일의 절대 경로 배열 (필수)
  {
    base::DictValue prop;
    prop.Set("type", "array");
    base::DictValue items;
    items.Set("type", "string");
    prop.Set("items", std::move(items));
    prop.Set("description",
             "업로드할 파일의 절대 경로 배열 "
             "(예: [\"/home/user/photo.jpg\", \"/home/user/report.pdf\"])");
    prop.Set("minItems", 1);
    properties.Set("filePaths", std::move(prop));
  }

  schema.Set("properties", std::move(properties));

  // filePaths는 필수
  base::ListValue required;
  required.Append("filePaths");
  schema.Set("required", std::move(required));

  return schema;
}

void FileUploadTool::Execute(const base::DictValue& arguments,
                              McpSession* session,
                              base::OnceCallback<void(base::Value)> callback) {
  // 로케이터가 있는지 확인
  const bool has_locator =
      arguments.FindString("role") || arguments.FindString("name") ||
      arguments.FindString("text") || arguments.FindString("selector") ||
      arguments.FindString("xpath") || arguments.FindString("ref");

  if (!has_locator) {
    LOG(WARNING) << "[FileUploadTool] 로케이터 파라미터가 필요합니다.";
    std::move(callback).Run(
        MakeErrorResult("로케이터 파라미터(role/name/text/selector/xpath/ref)가 필요합니다."));
    return;
  }

  // filePaths 배열 추출
  const base::ListValue* paths_list = arguments.FindList("filePaths");
  if (!paths_list || paths_list->empty()) {
    LOG(WARNING) << "[FileUploadTool] filePaths 파라미터 없거나 비어있음";
    std::move(callback).Run(
        MakeErrorResult("filePaths 파라미터가 필요합니다 (비어있지 않은 문자열 배열)"));
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
    std::move(callback).Run(
        MakeErrorResult("filePaths 배열에 유효한 파일 경로가 없습니다"));
    return;
  }

  LOG(INFO) << "[FileUploadTool] Execute, filePaths count=" << file_paths.size();

  // ElementLocator로 요소 탐색 → backendNodeId 획득
  locator_.Locate(
      session, arguments,
      base::BindOnce(&FileUploadTool::OnLocated, weak_factory_.GetWeakPtr(),
                     std::move(file_paths), session, std::move(callback)));
}

// ElementLocator 콜백: backendNodeId 획득 후 파일 설정
void FileUploadTool::OnLocated(
    std::vector<std::string> file_paths,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    std::optional<ElementLocator::Result> result,
    std::string error) {
  if (!error.empty()) {
    LOG(WARNING) << "[FileUploadTool] ElementLocator 실패: " << error;
    std::move(callback).Run(MakeErrorResult(error));
    return;
  }

  LOG(INFO) << "[FileUploadTool] 요소 발견, backendNodeId="
            << result->backend_node_id;

  SetFileInputFiles(result->backend_node_id, std::move(file_paths), session,
                    std::move(callback));
}

// DOM.setFileInputFiles로 파일 경로 배열을 file input 요소에 설정
void FileUploadTool::SetFileInputFiles(
    int backend_node_id,
    std::vector<std::string> file_paths,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;

  base::ListValue file_list;
  for (const std::string& path : file_paths) {
    file_list.Append(path);
    LOG(INFO) << "[FileUploadTool] 파일 경로 추가: " << path;
  }
  params.Set("files", std::move(file_list));
  params.Set("backendNodeId", backend_node_id);

  LOG(INFO) << "[FileUploadTool] DOM.setFileInputFiles 호출, backendNodeId="
            << backend_node_id << ", 파일 수=" << file_paths.size();

  session->SendCdpCommand(
      "DOM.setFileInputFiles", std::move(params),
      base::BindOnce(&FileUploadTool::OnSetFileInputFilesResult,
                     weak_factory_.GetWeakPtr(),
                     std::move(file_paths), std::move(callback)));
}

void FileUploadTool::OnSetFileInputFilesResult(
    std::vector<std::string> file_paths,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (mcp::HandleCdpError(response, "DOM.setFileInputFiles", callback)) {
    return;
  }

  LOG(INFO) << "[FileUploadTool] 파일 설정 완료, 파일 수=" << file_paths.size();

  std::string message = std::to_string(file_paths.size()) +
                        "개 파일이 성공적으로 설정되었습니다.";
  std::move(callback).Run(MakeSuccessResult(message));
}

}  // namespace mcp
