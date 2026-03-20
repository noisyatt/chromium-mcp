// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_CLIPBOARD_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_CLIPBOARD_TOOL_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// ClipboardTool: 시스템 클립보드 읽기/쓰기를 수행하는 도구.
//
// ★ 구현 방식 ★
//   Chromium 내부 ui::Clipboard API를 직접 사용한다.
//   CDP(Runtime.evaluate + navigator.clipboard)를 사용하지 않으므로
//   페이지 컨텍스트와 독립적이다. HTTPS 여부와 무관하게 동작한다.
//
// ★ 스레드 주의사항 ★
//   ui::Clipboard::GetForCurrentThread()는 UI 스레드에서만 호출해야 한다.
//   McpTool::Execute()는 UI 스레드에서 호출되므로 안전하다.
//
// 지원 동작:
//   read  → ui::Clipboard::GetForCurrentThread()->ReadText(kCopyPaste, ...)
//   write → ui::ScopedClipboardWriter로 텍스트를 클립보드에 씀
class ClipboardTool : public McpTool {
 public:
  ClipboardTool();
  ~ClipboardTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  bool requires_session() const override;

  // Execute는 read 액션의 경우 ReadText() callback을 통해 비동기적으로 완료된다.
  // write 액션은 동기적으로 즉시 완료된다.
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // action=read 처리.
  // ui::Clipboard::GetForCurrentThread()->ReadText() callback을 통해
  // 클립보드 텍스트를 읽고 callback을 호출한다.
  void HandleRead(base::OnceCallback<void(base::Value)> callback);

  // action=write 처리.
  // ui::ScopedClipboardWriter를 사용하여 텍스트를 클립보드에 쓴다.
  base::Value HandleWrite(const std::string& text);
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_CLIPBOARD_TOOL_H_
