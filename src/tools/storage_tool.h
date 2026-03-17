// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_STORAGE_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_STORAGE_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// StorageTool: localStorage 및 sessionStorage에 접근하는 도구.
//
// ★ 은닉성 설계 원칙 ★
//   Runtime.enable을 호출하지 않는다.
//   대신 Runtime.evaluate로 JavaScript를 직접 실행하여
//   localStorage/sessionStorage API에 접근한다.
//   이 방식은 DevTools 이벤트를 발생시키지 않아 탐지 위험이 낮다.
//
// 지원 동작 (action 파라미터):
//   get    → storageType.getItem(key)
//   set    → storageType.setItem(key, value)
//   remove → storageType.removeItem(key)
//   clear  → storageType.clear()
//   getAll → JSON.stringify(Object.entries(storageType))
//
// storageType 파라미터:
//   "local"   → localStorage (기본값, 영구 저장)
//   "session" → sessionStorage (탭 단위, 탭 닫으면 삭제)
class StorageTool : public McpTool {
 public:
  StorageTool();
  ~StorageTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // Runtime.evaluate로 JavaScript 표현식을 실행한다.
  // awaitPromise=false: 동기 JS 표현식이므로 Promise 대기 불필요.
  // returnByValue=true: 결과를 JSON 직렬화 가능한 값으로 수신.
  // userGesture=true: 일부 스토리지 API가 사용자 제스처를 요구할 수 있음.
  void EvaluateStorageScript(
      const std::string& expression,
      McpSession* session,
      base::OnceCallback<void(base::Value)> callback);

  // Runtime.evaluate 응답을 MCP 결과로 변환.
  // 예외 발생 시 에러 결과 반환.
  void OnEvaluateResponse(base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // 스토리지 타입 문자열("local" 또는 "session")을
  // JavaScript 전역 객체 이름으로 변환한다.
  // "local" → "localStorage", "session" → "sessionStorage"
  static std::string StorageTypeToJsObject(const std::string& storage_type);

  // 약한 참조 팩토리 (비동기 CDP 콜백에서 this 댕글링 방지)
  base::WeakPtrFactory<StorageTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_STORAGE_TOOL_H_
