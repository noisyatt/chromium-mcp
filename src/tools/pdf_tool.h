// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_PDF_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_PDF_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 현재 페이지를 PDF로 변환하는 MCP 도구.
//
// CDP Page.printToPDF 명령을 호출하여 현재 페이지를 PDF로 변환한다.
// savePath가 지정된 경우 base::WriteFile로 로컬에 저장하고,
// 지정되지 않은 경우 base64 인코딩 데이터를 응답으로 반환한다.
//
// 지원 파라미터:
//   - savePath            : 저장 경로 (없으면 base64 반환)
//   - landscape           : 가로 방향 출력 여부 (기본값: false)
//   - printBackground     : 배경색/이미지 포함 여부 (기본값: true)
//   - scale               : 출력 스케일 0.1~2.0 (기본값: 1.0)
//   - paperWidth          : 용지 너비 인치 (기본값: 8.5 = Letter)
//   - paperHeight         : 용지 높이 인치 (기본값: 11.0 = Letter)
//   - marginTop/Right/Bottom/Left : 여백 인치 (기본값: 0.4)
//   - pageRanges          : 인쇄 범위 "1-5, 8" 형식
//   - headerTemplate      : 머리글 HTML 템플릿
//   - footerTemplate      : 바닥글 HTML 템플릿
//   - displayHeaderFooter : 머리글/바닥글 표시 여부
//   - preferCSSPageSize   : CSS @page 크기 우선 적용 여부
//
// CDP 명령:
//   - Page.printToPDF : PDF 데이터를 base64로 반환
class PdfTool : public McpTool {
 public:
  PdfTool();
  ~PdfTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // Page.printToPDF CDP 응답을 처리한다.
  // savePath가 있으면 파일로 저장하고, 없으면 base64 데이터를 반환한다.
  void OnPrintToPdfResponse(const std::string& save_path,
                             base::OnceCallback<void(base::Value)> callback,
                             base::Value response);

  base::WeakPtrFactory<PdfTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_PDF_TOOL_H_
