// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_FILE_UPLOAD_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_FILE_UPLOAD_TOOL_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 파일 입력(input[type=file]) 요소에 파일을 설정하는 MCP 도구.
//
// CDP DOM 도메인을 통해 3단계로 처리한다:
//   1. DOM.getDocument  : 문서 루트 nodeId 획득
//   2. DOM.querySelector: CSS 선택자로 file input 요소의 nodeId 획득
//   3. DOM.setFileInputFiles: 지정된 파일 경로 배열을 file input에 설정
//
// 파라미터:
//   - selector  (string)        : input[type=file] CSS 선택자 (필수)
//   - filePaths (array<string>) : 업로드할 파일의 절대 경로 배열 (필수)
//
// 주의사항:
//   - filePaths의 각 경로는 Chromium 프로세스가 읽을 수 있는 로컬 경로여야 한다.
//   - 여러 파일을 동시에 설정하려면 input 요소에 multiple 속성이 있어야 한다.
//   - Shadow DOM 내부 요소는 기본적으로 탐색되지 않는다 (pierce=false).
class FileUploadTool : public McpTool {
 public:
  FileUploadTool();
  ~FileUploadTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // -----------------------------------------------------------------------
  // 처리 단계별 메서드
  // -----------------------------------------------------------------------

  // 1단계: DOM.getDocument로 문서 루트 nodeId를 획득한다.
  void FetchDocument(const std::string& selector,
                     std::vector<std::string> file_paths,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // 2단계: DOM.querySelector로 CSS 선택자에 해당하는 요소의 nodeId를 획득한다.
  // |root_node_id|: DOM.getDocument에서 반환된 루트 nodeId
  void QuerySelector(int root_node_id,
                     const std::string& selector,
                     std::vector<std::string> file_paths,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // 3단계: DOM.setFileInputFiles로 파일 경로 배열을 file input 요소에 설정한다.
  // |node_id|: DOM.querySelector에서 반환된 file input 요소의 nodeId
  void SetFileInputFiles(int node_id,
                         const std::string& selector,
                         std::vector<std::string> file_paths,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  // -----------------------------------------------------------------------
  // CDP 응답 처리 콜백
  // -----------------------------------------------------------------------

  // DOM.getDocument 응답: 루트 nodeId 추출 후 QuerySelector 호출
  void OnDocumentReceived(const std::string& selector,
                          std::vector<std::string> file_paths,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // DOM.querySelector 응답: 요소 nodeId 추출 후 SetFileInputFiles 호출
  void OnQuerySelectorResult(const std::string& selector,
                             std::vector<std::string> file_paths,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback,
                             base::Value response);

  // DOM.setFileInputFiles 응답: 성공/실패 결과 반환
  void OnSetFileInputFilesResult(
      const std::string& selector,
      std::vector<std::string> file_paths,
      base::OnceCallback<void(base::Value)> callback,
      base::Value response);

  base::WeakPtrFactory<FileUploadTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_FILE_UPLOAD_TOOL_H_
