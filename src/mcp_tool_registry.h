// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_MCP_TOOL_REGISTRY_H_
#define CHROME_BROWSER_MCP_MCP_TOOL_REGISTRY_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/values.h"

namespace mcp {

class McpSession;

// MCP 도구의 기본 인터페이스.
// 각 도구는 이 클래스를 상속하여 Execute()를 구현한다.
class McpTool {
 public:
  virtual ~McpTool() = default;

  // 도구 이름 (예: "navigate", "screenshot")
  virtual std::string name() const = 0;

  // 도구 설명 (tools/list 응답에 포함)
  virtual std::string description() const = 0;

  // JSON Schema 형식의 입력 파라미터 스키마
  virtual base::Value::Dict input_schema() const = 0;

  // 도구 실행.
  // arguments: tools/call 요청의 arguments 필드
  // session: 현재 MCP 세션 (탭/브라우저 컨텍스트 접근용)
  // callback: 실행 완료 시 결과(base::Value)를 전달하는 콜백
  virtual void Execute(const base::Value::Dict& arguments,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback) = 0;
};

// MCP 도구 레지스트리.
// 도구를 등록하고, tools/list 및 tools/call 요청을 처리한다.
class McpToolRegistry {
 public:
  McpToolRegistry();
  ~McpToolRegistry();

  // 복사/이동 불가 (싱글톤 용도)
  McpToolRegistry(const McpToolRegistry&) = delete;
  McpToolRegistry& operator=(const McpToolRegistry&) = delete;

  // 도구를 레지스트리에 등록한다.
  // 같은 이름의 도구가 이미 있으면 덮어쓴다.
  void RegisterTool(std::unique_ptr<McpTool> tool);

  // 이름으로 도구를 조회한다. 없으면 nullptr 반환.
  McpTool* GetTool(const std::string& name) const;

  // 등록된 모든 도구 목록을 반환한다.
  std::vector<McpTool*> GetAllTools() const;

  // 등록된 도구 수 반환.
  size_t GetToolCount() const;

  // 등록된 모든 도구의 명세를 List 형태로 반환한다.
  // [{ "name": "...", "description": "...", "inputSchema": {...} }, ...]
  base::Value BuildToolListResponse() const;

  // MCP tools/call 요청을 처리한다.
  // tool_name: 호출할 도구 이름
  // arguments: 도구에 전달할 인자
  // session: 현재 세션
  // callback: 실행 완료 시 MCP 응답 Value를 전달
  //
  // 도구를 찾지 못하면 MCP 에러 응답을 콜백으로 즉시 전달한다.
  void DispatchToolCall(
      const std::string& tool_name,
      const base::Value::Dict& arguments,
      McpSession* session,
      base::OnceCallback<void(base::Value)> callback) const;

 private:
  // 도구 이름 → 도구 인스턴스 맵
  std::map<std::string, std::unique_ptr<McpTool>> tools_;
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_MCP_TOOL_REGISTRY_H_
