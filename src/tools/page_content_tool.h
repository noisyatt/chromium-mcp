// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_PAGE_CONTENT_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_PAGE_CONTENT_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 페이지의 접근성 트리, HTML, 또는 순수 텍스트 콘텐츠를 반환하는 도구.
//
// mode별 CDP 사용:
//   - "accessibility": Accessibility.getFullAXTree
//   - "html":          DOM.getDocument + DOM.getOuterHTML
//   - "text":          Runtime.evaluate("document.body.innerText")
//                      (Runtime.enable 호출 없이 직접 평가 — 은닉성 유지)
//
// selector 파라미터가 주어지면 해당 요소 범위 내의 콘텐츠만 반환한다.
// (html/text 모드에서만 지원; accessibility 모드는 전체 트리를 항상 반환)
class PageContentTool : public McpTool {
 public:
  PageContentTool();
  ~PageContentTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // Accessibility.getFullAXTree 호출 및 응답 처리
  void FetchAccessibilityTree(McpSession* session,
                              base::OnceCallback<void(base::Value)> callback);
  void OnAccessibilityTreeResponse(
      base::OnceCallback<void(base::Value)> callback,
      base::Value response);

  // DOM.getDocument → DOM.getOuterHTML 체인으로 HTML 취득
  // |selector|가 비어있지 않으면 해당 요소의 HTML만 반환
  void FetchHtmlContent(const std::string& selector,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);
  void OnGetDocumentResponse(const std::string& selector,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback,
                             base::Value response);
  void OnGetOuterHtmlResponse(base::OnceCallback<void(base::Value)> callback,
                              base::Value response);

  // Runtime.evaluate로 innerText 추출.
  // Runtime.enable은 은닉성을 위해 절대 호출하지 않는다.
  void FetchTextContent(const std::string& selector,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);
  void OnEvaluateResponse(base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  base::WeakPtrFactory<PageContentTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_PAGE_CONTENT_TOOL_H_
