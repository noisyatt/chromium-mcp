// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/clipboard_tool.h"

#include <string>
#include <utility>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"

namespace mcp {

namespace {

// MCP 성공 응답 Value 생성.
base::Value MakeSuccessResult(const std::string& text) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", text);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", false);
  return base::Value(std::move(result));
}

// MCP 에러 응답 Value 생성.
base::Value MakeErrorResult(const std::string& text) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", text);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", true);
  return base::Value(std::move(result));
}

}  // namespace

ClipboardTool::ClipboardTool() = default;
ClipboardTool::~ClipboardTool() = default;

bool ClipboardTool::requires_session() const {
  return false;
}

std::string ClipboardTool::name() const {
  return "clipboard";
}

std::string ClipboardTool::description() const {
  return "시스템 클립보드 읽기/쓰기를 수행합니다. "
         "action=read로 현재 클립보드의 텍스트를 읽고, "
         "action=write로 지정한 텍스트를 클립보드에 씁니다. "
         "ui::Clipboard API를 직접 사용하므로 페이지 컨텍스트와 무관하게 동작합니다.";
}

base::DictValue ClipboardTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: 필수 파라미터
  {
    base::DictValue action_prop;
    action_prop.Set("type", "string");
    base::ListValue action_enum;
    action_enum.Append("read");
    action_enum.Append("write");
    action_prop.Set("enum", std::move(action_enum));
    action_prop.Set("description",
                    "수행할 작업: read(클립보드 읽기), write(클립보드 쓰기)");
    properties.Set("action", std::move(action_prop));
  }

  // text: 클립보드에 쓸 텍스트 (write 시 필수)
  {
    base::DictValue text_prop;
    text_prop.Set("type", "string");
    text_prop.Set("description",
                  "클립보드에 쓸 텍스트. action=write 시 필수.");
    properties.Set("text", std::move(text_prop));
  }

  schema.Set("properties", std::move(properties));

  // action만 필수
  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void ClipboardTool::Execute(
    const base::DictValue& arguments,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  const std::string* action = arguments.FindString("action");
  if (!action || action->empty()) {
    LOG(WARNING) << "[ClipboardTool] action 파라미터 누락";
    std::move(callback).Run(
        MakeErrorResult("action 파라미터가 필요합니다 (read 또는 write)"));
    return;
  }

  LOG(INFO) << "[ClipboardTool] action=" << *action;

  if (*action == "read") {
    // ReadText()가 callback 기반이므로 HandleRead에 callback을 전달
    HandleRead(std::move(callback));
  } else if (*action == "write") {
    const std::string* text = arguments.FindString("text");
    if (!text) {
      std::move(callback).Run(
          MakeErrorResult("write 작업에는 text 파라미터가 필요합니다."));
      return;
    }
    std::move(callback).Run(HandleWrite(*text));
  } else {
    LOG(WARNING) << "[ClipboardTool] 알 수 없는 action: " << *action;
    std::move(callback).Run(
        MakeErrorResult("action은 'read' 또는 'write'여야 합니다."));
  }
}

// action=read: ui::Clipboard::GetForCurrentThread()->ReadText() 호출.
//
// ★ kCopyPaste 버퍼 ★
//   ui::ClipboardBuffer::kCopyPaste는 표준 Ctrl+C/Ctrl+V 클립보드이다.
//   X11의 PRIMARY 선택(마우스 드래그 선택)은 kSelection 버퍼를 사용한다.
//
// ★ DataTransferEndpoint ★
//   std::nullopt을 전달하면 엔드포인트 제한 없이 읽는다.
//   Chromium 내부 도구이므로 보안 정책 검사가 불필요하다.
//
// ★ ui::Clipboard는 std::u16string(UTF-16)을 사용한다 ★
//   ReadTextCallback 내에서 base::UTF16ToUTF8()로 변환하여 MCP 응답에 포함한다.
//
// ★ ReadText()는 callback 기반 비동기 API이다 ★
//   결과는 ReadTextCallback(std::u16string)으로 전달된다.
void ClipboardTool::HandleRead(base::OnceCallback<void(base::Value)> callback) {
  ui::Clipboard* clipboard = ui::Clipboard::GetForCurrentThread();
  if (!clipboard) {
    LOG(ERROR) << "[ClipboardTool] ui::Clipboard 인스턴스를 가져올 수 없음";
    std::move(callback).Run(
        MakeErrorResult("클립보드 인스턴스를 가져올 수 없습니다."));
    return;
  }

  // ReadText: kCopyPaste 버퍼에서 일반 텍스트를 읽는다.
  // data_dst=std::nullopt: 엔드포인트 제한 없음.
  // callback 내에서 UTF-16 → UTF-8 변환 후 MCP 응답을 반환한다.
  clipboard->ReadText(
      ui::ClipboardBuffer::kCopyPaste,
      /*data_dst=*/nullptr,
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb,
             std::u16string text_utf16) {
            // UTF-16 → UTF-8 변환 (MCP 응답은 UTF-8 문자열 사용)
            std::string text_utf8 = base::UTF16ToUTF8(text_utf16);
            LOG(INFO) << "[ClipboardTool] 클립보드 읽기 완료 "
                      << "(길이: " << text_utf8.size() << " bytes)";
            std::move(cb).Run(MakeSuccessResult(text_utf8));
          },
          std::move(callback)));
}

// action=write: ui::ScopedClipboardWriter로 텍스트를 클립보드에 씀.
//
// ★ ui::ScopedClipboardWriter ★
//   RAII 방식으로 클립보드 쓰기 세션을 관리한다.
//   소멸자에서 실제 클립보드 데이터가 커밋된다.
//   WriteText()를 호출한 후 객체가 소멸되면 클립보드에 반영된다.
//
// ★ UTF-8 → UTF-16 변환 ★
//   MCP 파라미터는 UTF-8 문자열이다.
//   WriteText()는 std::u16string을 요구하므로 base::UTF8ToUTF16()으로 변환한다.
base::Value ClipboardTool::HandleWrite(const std::string& text) {
  // UTF-8 → UTF-16 변환
  std::u16string text_utf16 = base::UTF8ToUTF16(text);

  {
    // ScopedClipboardWriter: 소멸 시 클립보드에 커밋됨
    ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
    writer.WriteText(text_utf16);
    // 블록 종료 시 writer 소멸 → 클립보드에 텍스트 커밋
  }

  LOG(INFO) << "[ClipboardTool] 클립보드 쓰기 완료 "
            << "(길이: " << text.size() << " bytes)";

  return MakeSuccessResult("클립보드에 텍스트를 성공적으로 복사했습니다.");
}

}  // namespace mcp
