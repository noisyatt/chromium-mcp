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
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// 파일 입력(input[type=file]) 요소에 파일을 설정하는 MCP 도구.
//
// ElementLocator로 요소를 찾아 backendNodeId를 획득한 후
// DOM.setFileInputFiles로 파일을 설정한다.
//
//   1. ElementLocator::Locate() → backendNodeId 획득
//   2. DOM.setFileInputFiles: 지정된 파일 경로 배열을 file input에 설정
//
// 파라미터:
//   - 로케이터 (role/name/text/selector/xpath/ref 중 하나, 필수)
//   - filePaths (array<string>) : 업로드할 파일의 절대 경로 배열 (필수)
//
// 주의사항:
//   - filePaths의 각 경로는 Chromium 프로세스가 읽을 수 있는 로컬 경로여야 한다.
//   - 여러 파일을 동시에 설정하려면 input 요소에 multiple 속성이 있어야 한다.
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
  // ElementLocator 콜백: backendNodeId 획득 후 파일 설정
  void OnLocated(std::vector<std::string> file_paths,
                 McpSession* session,
                 base::OnceCallback<void(base::Value)> callback,
                 std::optional<ElementLocator::Result> result,
                 std::string error);

  // DOM.setFileInputFiles로 파일 경로 배열을 file input 요소에 설정
  void SetFileInputFiles(int backend_node_id,
                         std::vector<std::string> file_paths,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  // DOM.setFileInputFiles 응답: 성공/실패 결과 반환
  void OnSetFileInputFilesResult(
      std::vector<std::string> file_paths,
      base::OnceCallback<void(base::Value)> callback,
      base::Value response);

  ElementLocator locator_;

  base::WeakPtrFactory<FileUploadTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_FILE_UPLOAD_TOOL_H_
