// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_KEYBOARD_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_KEYBOARD_TOOL_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// KeyboardTool: 키보드 입력을 시뮬레이션하는 도구.
//
// 세 가지 동작 모드:
//   - action="type"     : Input.insertText 로 텍스트를 한 번에 삽입
//   - action="press"    : Input.dispatchKeyEvent(keyDown + keyUp) 로 키/조합키 입력
//   - action="shortcut" : modifier 키와 함께 keyDown/keyUp 시퀀스 (예: Ctrl+A)
//
// modifiers 배열에서 비트 플래그를 계산하는 방식:
//   Alt=1, Ctrl=2, Meta=4, Shift=8  (CDP 표준)
//
// delay 파라미터: type 모드에서 각 문자 사이 딜레이 (ms).
//   0이면 Input.insertText로 한 번에 삽입, 양수이면 각 문자마다 dispatchKeyEvent.
class KeyboardTool : public McpTool {
 public:
  KeyboardTool();
  ~KeyboardTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // type 모드: Input.insertText 발송 (delay=0일 때)
  void ExecuteType(const std::string& text,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // type 모드: 각 문자마다 dispatchKeyEvent 발송 (delay>0일 때)
  void ExecuteTypeWithDelay(const std::string& text,
                            int delay_ms,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback);

  // press/shortcut 모드: keyDown 발송
  void ExecutePress(const std::string& key,
                    int modifiers,
                    McpSession* session,
                    base::OnceCallback<void(base::Value)> callback);

  // type 완료 콜백 (insertText 방식)
  void OnInsertText(base::OnceCallback<void(base::Value)> callback,
                    base::Value response);

  // 지연 타이핑: 다음 문자 발송 (딜레이 타이머 콜백)
  void DispatchNextChar(std::string remaining_text,
                        int delay_ms,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  // 단일 문자 keyDown 완료 콜백 (지연 타이핑용)
  void OnCharKeyDown(std::string remaining_text,
                     int delay_ms,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback,
                     base::Value response);

  // 단일 문자 keyUp 완료 콜백 (지연 타이핑용)
  void OnCharKeyUp(std::string remaining_text,
                   int delay_ms,
                   McpSession* session,
                   base::OnceCallback<void(base::Value)> callback,
                   base::Value response);

  // keyDown 완료 후 keyUp 발송
  void OnKeyDown(const std::string& key,
                 int modifiers,
                 McpSession* session,
                 base::OnceCallback<void(base::Value)> callback,
                 base::Value response);

  // keyUp 완료 콜백
  void OnKeyUp(base::OnceCallback<void(base::Value)> callback,
               base::Value response);

  // modifiers 문자열 배열을 CDP 비트 플래그 정수로 변환한다.
  // Alt=1, Ctrl=2, Meta=4, Shift=8
  static int ComputeModifiers(const base::ListValue* modifiers_list);

  // CDP 에러 응답 처리 헬퍼
  static bool HandleCdpError(const base::Value& response,
                              const std::string& step_name,
                              base::OnceCallback<void(base::Value)>& callback);

  // 지연 타이핑용 타이머
  base::OneShotTimer char_delay_timer_;

  base::WeakPtrFactory<KeyboardTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_KEYBOARD_TOOL_H_
