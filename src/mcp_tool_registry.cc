// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/mcp_tool_registry.h"

#include "base/logging.h"
#include "base/values.h"

namespace mcp {

McpToolRegistry::McpToolRegistry() = default;
McpToolRegistry::~McpToolRegistry() = default;

void McpToolRegistry::RegisterTool(std::unique_ptr<McpTool> tool) {
  if (!tool) {
    LOG(WARNING) << "[MCP] RegisterTool: null 도구 등록 시도 무시";
    return;
  }

  const std::string tool_name = tool->name();
  if (tool_name.empty()) {
    LOG(WARNING) << "[MCP] RegisterTool: 이름 없는 도구 등록 시도 무시";
    return;
  }

  if (tools_.count(tool_name)) {
    LOG(INFO) << "[MCP] RegisterTool: '" << tool_name
              << "' 도구를 덮어씀 (기존 등록 있음)";
  } else {
    LOG(INFO) << "[MCP] RegisterTool: '" << tool_name << "' 도구 등록 완료";
  }

  tools_[tool_name] = std::move(tool);
}

McpTool* McpToolRegistry::GetTool(const std::string& name) const {
  auto it = tools_.find(name);
  if (it == tools_.end()) {
    return nullptr;
  }
  return it->second.get();
}

std::vector<McpTool*> McpToolRegistry::GetAllTools() const {
  std::vector<McpTool*> result;
  result.reserve(tools_.size());
  for (const auto& [name, tool] : tools_) {
    result.push_back(tool.get());
  }
  return result;
}

size_t McpToolRegistry::GetToolCount() const {
  return tools_.size();
}

base::Value McpToolRegistry::BuildToolListResponse() const {
  // 도구 목록을 List 형태로 직렬화하여 반환한다.
  base::ListValue tools_list;

  for (const auto& [name, tool] : tools_) {
    base::DictValue tool_entry;
    tool_entry.Set("name", tool->name());
    tool_entry.Set("description", tool->description());
    tool_entry.Set("inputSchema", tool->input_schema());
    tools_list.Append(std::move(tool_entry));
  }

  return base::Value(std::move(tools_list));
}

void McpToolRegistry::DispatchToolCall(
    const std::string& tool_name,
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) const {
  McpTool* tool = GetTool(tool_name);

  if (!tool) {
    // 도구를 찾지 못한 경우 MCP 에러 응답을 즉시 반환한다.
    LOG(WARNING) << "[MCP] DispatchToolCall: 알 수 없는 도구 '" << tool_name
                 << "'";

    base::DictValue error;
    error.Set("code", -32601);  // Method not found (JSON-RPC 표준 에러 코드)
    error.Set("message",
              "Unknown tool: " + tool_name);

    base::DictValue response;
    response.Set("error", std::move(error));
    std::move(callback).Run(base::Value(std::move(response)));
    return;
  }

  LOG(INFO) << "[MCP] DispatchToolCall: '" << tool_name << "' 도구 실행";
  tool->Execute(arguments, session, std::move(callback));
}

}  // namespace mcp
