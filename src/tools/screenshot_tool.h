// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_SCREENSHOT_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_SCREENSHOT_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 현재 페이지 또는 특정 CSS 선택자에 해당하는 요소의 스크린샷을 캡처하는 도구.
//
// selector 없을 때: Page.captureScreenshot 직접 호출
// selector 있을 때: DOM.querySelector → DOM.getBoxModel → clip 계산 →
//                  Page.captureScreenshot(clip=...) 순서로 비동기 체인 실행
//
// 결과는 base64 인코딩된 이미지 데이터를 포함하는 base::Value::Dict 이다.
class ScreenshotTool : public McpTool {
 public:
  ScreenshotTool();
  ~ScreenshotTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 전체 페이지 또는 뷰포트 스크린샷을 캡처한다.
  // |format|: "png" 또는 "jpeg"
  // |full_page|: true이면 스크롤 포함 전체 페이지 캡처
  void CaptureFullPageScreenshot(const std::string& format,
                                 bool full_page,
                                 McpSession* session,
                                 base::OnceCallback<void(base::Value)> callback);

  // DOM.querySelector를 호출해 selector에 해당하는 노드 ID를 가져온다.
  void QuerySelectorForScreenshot(
      const std::string& selector,
      const std::string& format,
      McpSession* session,
      base::OnceCallback<void(base::Value)> callback);

  // querySelector 응답을 받아 DOM.getBoxModel을 호출한다.
  void OnQuerySelectorResponse(
      const std::string& format,
      McpSession* session,
      base::OnceCallback<void(base::Value)> callback,
      base::Value response);

  // getBoxModel 응답을 받아 clip 영역을 계산하고 captureScreenshot을 호출한다.
  void OnGetBoxModelResponse(const std::string& format,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback,
                             base::Value response);

  // captureScreenshot 응답을 받아 최종 결과를 구성한다.
  void OnCaptureScreenshotResponse(
      base::OnceCallback<void(base::Value)> callback,
      base::Value response);

  base::WeakPtrFactory<ScreenshotTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_SCREENSHOT_TOOL_H_
