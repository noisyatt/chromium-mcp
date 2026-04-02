// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_JAVASCRIPT_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_JAVASCRIPT_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// JavaScriptTool (evaluate): 페이지 컨텍스트에서 JavaScript를 실행하는 도구.
//
// ★ 은닉성 핵심 설계 원칙 ★
//   Runtime.enable을 절대 호출하지 않는다.
//   Runtime.enable을 호출하면 브라우저의 DevTools 감지 로직이 활성화되며,
//   일부 보안 사이트는 window.CDP_ENABLED 플래그 변화, executionContext 이벤트
//   발생 여부 등으로 자동화를 탐지한다.
//   대신 Runtime.evaluate를 이벤트 활성화 없이 직접 호출한다.
//
// 실행 모드:
//   A. 메인 월드(기본):
//      Runtime.evaluate { expression, returnByValue, awaitPromise }
//      → 페이지 JS와 동일한 글로벌 컨텍스트에서 실행.
//        window 객체를 공유하므로 DOM 조작 등 대부분의 스크립트에 적합.
//
//   B. Isolated World (isolatedWorld=true):
//      1. Page.createIsolatedWorld → executionContextId 획득
//      2. Runtime.evaluate { ..., contextId: <id> }
//      → 페이지 글로벌에서 분리된 컨텍스트에서 실행.
//        content script와 동일한 격리 수준으로,
//        페이지 JS가 실행 흔적을 관측할 수 없다.
class JavaScriptTool : public McpTool {
 public:
  JavaScriptTool();
  ~JavaScriptTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 모드 A: 메인 월드에서 Runtime.evaluate 직접 호출.
  void EvaluateInMainWorld(const std::string& expression,
                           bool await_promise,
                           int timeout_ms,
                           McpSession* session,
                           base::OnceCallback<void(base::Value)> callback);

  void GetFrameTreeForIsolatedWorld(
      const std::string& expression,
      bool await_promise,
      int timeout_ms,
      McpSession* session,
      base::OnceCallback<void(base::Value)> callback);

  void OnGetFrameTree(const std::string& expression,
                      bool await_promise,
                      int timeout_ms,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback,
                      base::Value response);

  void OnIsolatedWorldCreated(const std::string& expression,
                              bool await_promise,
                              int timeout_ms,
                              McpSession* session,
                              base::OnceCallback<void(base::Value)> callback,
                              base::Value response);

  // Runtime.evaluate 응답을 MCP 결과로 변환하여 callback 호출.
  // 성공: result.value 또는 result.description을 텍스트로 반환.
  // 예외: exceptionDetails가 있으면 에러 결과로 반환.
  void OnEvaluateResponse(base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  base::WeakPtrFactory<JavaScriptTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_JAVASCRIPT_TOOL_H_
