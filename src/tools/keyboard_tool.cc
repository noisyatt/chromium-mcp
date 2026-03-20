// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/keyboard_tool.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

namespace {

// MCP 성공 응답 Value 생성
base::Value MakeSuccessResult(const std::string& message) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", message);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", false);
  return base::Value(std::move(result));
}

// MCP 에러 응답 Value 생성
base::Value MakeErrorResult(const std::string& message) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", message);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", true);
  return base::Value(std::move(result));
}

// CDP 응답 Dict에 "error" 키가 있는지 확인한다.
bool HasCdpError(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return true;
  }
  return dict->Find("error") != nullptr;
}

// CDP 응답에서 에러 메시지를 추출한다.
std::string ExtractCdpErrorMessage(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return "CDP 응답이 Dict 형식이 아님";
  }
  const base::DictValue* error = dict->FindDict("error");
  if (!error) {
    return "알 수 없는 CDP 에러";
  }
  const std::string* msg = error->FindString("message");
  if (!msg) {
    return "에러 메시지 없음";
  }
  return *msg;
}

// 키 이름으로부터 Windows Virtual Key Code를 반환한다.
// CDP Input.dispatchKeyEvent에서 windowsVirtualKeyCode 필드가 필요하다.
int KeyNameToVirtualKeyCode(const std::string& key) {
  // 내비게이션 키
  if (key == "Enter") return 13;
  if (key == "Tab") return 9;
  if (key == "Escape" || key == "Esc") return 27;
  if (key == "Backspace") return 8;
  if (key == "Delete") return 46;
  if (key == "Insert") return 45;
  // 방향 키
  if (key == "ArrowLeft") return 37;
  if (key == "ArrowUp") return 38;
  if (key == "ArrowRight") return 39;
  if (key == "ArrowDown") return 40;
  // 페이지 이동 키
  if (key == "Home") return 36;
  if (key == "End") return 35;
  if (key == "PageUp") return 33;
  if (key == "PageDown") return 34;
  // 기능 키 F1~F12
  if (key == "F1") return 112;
  if (key == "F2") return 113;
  if (key == "F3") return 114;
  if (key == "F4") return 115;
  if (key == "F5") return 116;
  if (key == "F6") return 117;
  if (key == "F7") return 118;
  if (key == "F8") return 119;
  if (key == "F9") return 120;
  if (key == "F10") return 121;
  if (key == "F11") return 122;
  if (key == "F12") return 123;
  // 특수 키
  if (key == "Space" || key == " ") return 32;
  if (key == "CapsLock") return 20;
  if (key == "NumLock") return 144;
  if (key == "ScrollLock") return 145;
  if (key == "PrintScreen") return 44;
  if (key == "Pause") return 19;
  // 단일 문자 키: ASCII 코드를 대문자로 변환하여 반환
  if (key.size() == 1) {
    char c = key[0];
    if (c >= 'a' && c <= 'z') {
      return static_cast<int>(c - 'a' + 'A');
    }
    return static_cast<int>(c);
  }
  return 0;
}

// 키 이름으로부터 CDP code 문자열을 반환한다.
// https://www.w3.org/TR/uievents-code/ 규격 참조
std::string KeyNameToCode(const std::string& key) {
  // 내비게이션 키
  if (key == "Enter") return "Enter";
  if (key == "Tab") return "Tab";
  if (key == "Escape" || key == "Esc") return "Escape";
  if (key == "Backspace") return "Backspace";
  if (key == "Delete") return "Delete";
  if (key == "Insert") return "Insert";
  // 방향 키
  if (key == "ArrowLeft") return "ArrowLeft";
  if (key == "ArrowUp") return "ArrowUp";
  if (key == "ArrowRight") return "ArrowRight";
  if (key == "ArrowDown") return "ArrowDown";
  // 페이지 이동 키
  if (key == "Home") return "Home";
  if (key == "End") return "End";
  if (key == "PageUp") return "PageUp";
  if (key == "PageDown") return "PageDown";
  // 특수 키
  if (key == "Space" || key == " ") return "Space";
  if (key == "CapsLock") return "CapsLock";
  // 기능 키
  if (key.size() == 2 && key[0] == 'F') return key;
  if (key.size() == 3 && key[0] == 'F') return key;
  // 알파벳 단일 문자
  if (key.size() == 1) {
    char c = key[0];
    if (c >= 'a' && c <= 'z') {
      return std::string("Key") + static_cast<char>(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
      return std::string("Key") + c;
    }
    if (c >= '0' && c <= '9') {
      return std::string("Digit") + c;
    }
  }
  return key;  // 인식되지 않는 키는 그대로 반환
}

// macOS 단축키에 대응하는 editing command를 반환한다.
// CDP Input.dispatchKeyEvent의 "commands" 필드에 설정해야
// macOS에서 Cmd+키 조합이 실제로 동작한다.
std::string GetMacCommand(const std::string& key, int modifiers) {
  // Meta(Cmd) 키가 포함된 경우만 처리
  bool has_meta = (modifiers & 4) != 0;
  bool has_shift = (modifiers & 8) != 0;
  if (!has_meta) return "";

  std::string lower_key = key;
  if (lower_key.size() == 1 && lower_key[0] >= 'A' && lower_key[0] <= 'Z') {
    lower_key[0] = lower_key[0] - 'A' + 'a';
  }

  if (lower_key == "a") return "selectAll";
  if (lower_key == "c") return "copy";
  if (lower_key == "v") return "paste";
  if (lower_key == "x") return "cut";
  if (lower_key == "z" && !has_shift) return "undo";
  if (lower_key == "z" && has_shift) return "redo";
  return "";
}

// Input.dispatchKeyEvent 파라미터 Dict 생성
base::DictValue MakeKeyEventParams(const std::string& event_type,
                                     const std::string& key,
                                     int modifiers) {
  base::DictValue params;
  params.Set("type", event_type);
  params.Set("key", key);
  params.Set("code", KeyNameToCode(key));
  params.Set("modifiers", modifiers);
  int vk_code = KeyNameToVirtualKeyCode(key);
  if (vk_code > 0) {
    params.Set("windowsVirtualKeyCode", vk_code);
    params.Set("nativeVirtualKeyCode", vk_code);
  }

  // macOS: keyDown 이벤트에 commands 필드를 추가해야 단축키가 동작한다.
  if (event_type == "keyDown") {
    std::string command = GetMacCommand(key, modifiers);
    if (!command.empty()) {
      base::ListValue commands;
      commands.Append(command);
      params.Set("commands", std::move(commands));
    }
  }

  return params;
}

}  // namespace

// ============================================================
// KeyboardTool 구현
// ============================================================

KeyboardTool::KeyboardTool() = default;
KeyboardTool::~KeyboardTool() = default;

std::string KeyboardTool::name() const {
  return "keyboard";
}

std::string KeyboardTool::description() const {
  return "키보드 입력을 시뮬레이션합니다. "
         "action='type'이면 텍스트를 삽입하고 (delay>0이면 글자별 딜레이), "
         "action='press'이면 특수키·조합키를 발송합니다. "
         "action='shortcut'이면 modifier+키 조합을 단축키로 발송합니다. "
         "modifiers 배열로 ctrl, alt, shift, meta 조합이 가능합니다.";
}

base::DictValue KeyboardTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action: "type", "press", "shortcut"
  base::DictValue action_prop;
  action_prop.Set("type", "string");
  base::ListValue action_enum;
  action_enum.Append("type");
  action_enum.Append("press");
  action_enum.Append("shortcut");
  action_prop.Set("enum", std::move(action_enum));
  action_prop.Set("description",
                  "type: 텍스트 삽입 (Input.insertText 또는 글자별 keyEvent), "
                  "press: 키/조합키 발송 (Input.dispatchKeyEvent keyDown+keyUp), "
                  "shortcut: modifier+키 단축키 시퀀스 (예: Ctrl+A, Ctrl+Shift+T)");
  properties.Set("action", std::move(action_prop));

  // text: action=type 일 때 입력할 텍스트
  base::DictValue text_prop;
  text_prop.Set("type", "string");
  text_prop.Set("description",
                "action=type 일 때 입력할 텍스트. 한글·이모지 포함 가능. "
                "delay=0이면 insertText로 한 번에 삽입, delay>0이면 글자별로 keyEvent 발송.");
  properties.Set("text", std::move(text_prop));

  // key: action=press/shortcut 일 때 키 이름
  base::DictValue key_prop;
  key_prop.Set("type", "string");
  key_prop.Set("description",
               "action=press/shortcut 일 때 키 이름. "
               "예: Enter, Tab, Escape, ArrowDown, F5, a, A");
  properties.Set("key", std::move(key_prop));

  // modifiers: 조합키 배열
  base::DictValue modifiers_prop;
  modifiers_prop.Set("type", "array");
  base::DictValue modifiers_items;
  modifiers_items.Set("type", "string");
  base::ListValue modifiers_enum;
  modifiers_enum.Append("ctrl");
  modifiers_enum.Append("alt");
  modifiers_enum.Append("shift");
  modifiers_enum.Append("meta");
  modifiers_items.Set("enum", std::move(modifiers_enum));
  modifiers_prop.Set("items", std::move(modifiers_items));
  modifiers_prop.Set("description",
                     "누를 조합키 목록. Alt=1, Ctrl=2, Meta=4, Shift=8 비트 플래그. "
                     "예: [\"ctrl\", \"shift\"] → Ctrl+Shift+키");
  properties.Set("modifiers", std::move(modifiers_prop));

  // delay: type 모드에서 각 문자 사이 딜레이 (ms)
  base::DictValue delay_prop;
  delay_prop.Set("type", "number");
  delay_prop.Set("default", 0);
  delay_prop.Set("description",
                 "action=type 일 때 각 문자 사이 딜레이 (밀리초). "
                 "0이면 insertText로 한 번에 삽입 (기본값). "
                 "양수이면 각 문자마다 keyDown/keyUp 이벤트를 발송.");
  properties.Set("delay", std::move(delay_prop));

  schema.Set("properties", std::move(properties));

  // action은 필수
  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void KeyboardTool::Execute(const base::DictValue& arguments,
                           McpSession* session,
                           base::OnceCallback<void(base::Value)> callback) {
  const std::string* action = arguments.FindString("action");
  if (!action || action->empty()) {
    LOG(WARNING) << "[KeyboardTool] action 파라미터가 필요합니다.";
    std::move(callback).Run(MakeErrorResult("action 파라미터가 필요합니다."));
    return;
  }

  if (*action == "type") {
    // 텍스트 삽입 모드
    const std::string* text = arguments.FindString("text");
    if (!text || text->empty()) {
      LOG(WARNING) << "[KeyboardTool] action=type 일 때 text 파라미터가 필요합니다.";
      std::move(callback).Run(MakeErrorResult("text 파라미터가 필요합니다."));
      return;
    }
    // delay 파라미터 추출 (기본값: 0)
    std::optional<double> delay_opt = arguments.FindDouble("delay");
    int delay_ms = static_cast<int>(delay_opt.value_or(0.0));

    LOG(INFO) << "[KeyboardTool] type 모드: text=" << *text
              << " delay=" << delay_ms << "ms";

    if (delay_ms > 0) {
      // 글자별 딜레이 모드
      ExecuteTypeWithDelay(*text, delay_ms, session, std::move(callback));
    } else {
      // insertText 한 번에 삽입
      ExecuteType(*text, session, std::move(callback));
    }

  } else if (*action == "press" || *action == "shortcut") {
    // 키 입력 / 단축키 모드 (CDP 동작 동일: modifier 플래그 차이만 있음)
    const std::string* key = arguments.FindString("key");
    if (!key || key->empty()) {
      LOG(WARNING) << "[KeyboardTool] action=" << *action
                   << " 일 때 key 파라미터가 필요합니다.";
      std::move(callback).Run(MakeErrorResult("key 파라미터가 필요합니다."));
      return;
    }

    // modifiers 비트 플래그 계산
    const base::ListValue* modifiers_list = arguments.FindList("modifiers");
    int modifiers = ComputeModifiers(modifiers_list);

    LOG(INFO) << "[KeyboardTool] " << *action << " 모드: key=" << *key
              << " modifiers=" << modifiers;
    ExecutePress(*key, modifiers, session, std::move(callback));

  } else {
    LOG(WARNING) << "[KeyboardTool] 알 수 없는 action 값: " << *action;
    std::move(callback).Run(
        MakeErrorResult("action은 'type', 'press', 'shortcut' 중 하나여야 합니다."));
  }
}

// type 모드: Input.insertText 발송 (딜레이 없이 한 번에)
void KeyboardTool::ExecuteType(const std::string& text,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("text", text);

  session->SendCdpCommand(
      "Input.insertText", std::move(params),
      base::BindOnce(&KeyboardTool::OnInsertText,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// type 모드: 각 문자마다 keyDown/keyUp 발송 (딜레이 있을 때)
void KeyboardTool::ExecuteTypeWithDelay(
    const std::string& text,
    int delay_ms,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (text.empty()) {
    // 모든 문자 입력 완료
    LOG(INFO) << "[KeyboardTool] 지연 타이핑 완료";
    std::move(callback).Run(MakeSuccessResult("텍스트 입력이 완료되었습니다."));
    return;
  }
  DispatchNextChar(text, delay_ms, session, std::move(callback));
}

// 지연 타이핑: 다음 문자 keyDown 발송
void KeyboardTool::DispatchNextChar(
    std::string remaining_text,
    int delay_ms,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (remaining_text.empty()) {
    LOG(INFO) << "[KeyboardTool] 지연 타이핑 완료";
    std::move(callback).Run(MakeSuccessResult("텍스트 입력이 완료되었습니다."));
    return;
  }

  // UTF-8 멀티바이트 문자 올바르게 추출.
  // 한글은 3바이트, 이모지는 4바이트이므로 첫 바이트의 상위 비트로 길이 판별.
  unsigned char first_byte = static_cast<unsigned char>(remaining_text[0]);
  size_t char_len = 1;
  if (first_byte >= 0xF0) {
    char_len = 4;
  } else if (first_byte >= 0xE0) {
    char_len = 3;
  } else if (first_byte >= 0xC0) {
    char_len = 2;
  }
  // 문자열 끝을 넘지 않도록 보호
  char_len = std::min(char_len, remaining_text.size());

  std::string current_char = remaining_text.substr(0, char_len);
  std::string rest = remaining_text.substr(char_len);

  // 비ASCII 문자(한글, 이모지 등) 및 특수문자는 Input.insertText로 처리.
  // keyDown/keyUp은 알파벳과 숫자, 공백에만 안전하다.
  // 특수문자(!@#$% 등)는 VK 코드가 다른 키(PageUp 등)와 충돌할 수 있다.
  bool is_simple_ascii = (first_byte >= 'a' && first_byte <= 'z') ||
                         (first_byte >= 'A' && first_byte <= 'Z') ||
                         (first_byte >= '0' && first_byte <= '9') ||
                         first_byte == ' ';
  if (!is_simple_ascii) {
    base::DictValue params;
    params.Set("text", current_char);
    session->SendCdpCommand(
        "Input.insertText", std::move(params),
        base::BindOnce(&KeyboardTool::OnCharKeyUp,
                       weak_factory_.GetWeakPtr(),
                       std::move(rest), delay_ms, session,
                       std::move(callback)));
    return;
  }

  base::DictValue params = MakeKeyEventParams("keyDown", current_char, 0);
  // 단일 문자 키에 text 필드 추가 (브라우저 입력 처리용)
  params.Set("text", current_char);

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&KeyboardTool::OnCharKeyDown,
                     weak_factory_.GetWeakPtr(),
                     std::move(rest), delay_ms, session,
                     std::move(callback)));
}

// 단일 문자 keyDown 완료 콜백 → keyUp 발송
void KeyboardTool::OnCharKeyDown(
    std::string remaining_text,
    int delay_ms,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(keyDown/char)", callback)) {
    return;
  }

  // keyDown 완료 후 현재 문자 역추적: remaining_text는 이미 다음 문자들이므로
  // keyUp을 위해 직전 문자를 알 수 없음 → 단순히 키업 발송
  // (실제로 keyUp의 key 값은 입력에 영향 없음)
  base::DictValue params;
  params.Set("type", "keyUp");
  params.Set("key", "Unidentified");
  params.Set("code", "");
  params.Set("modifiers", 0);

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&KeyboardTool::OnCharKeyUp,
                     weak_factory_.GetWeakPtr(),
                     std::move(remaining_text), delay_ms, session,
                     std::move(callback)));
}

// 단일 문자 keyUp 완료 콜백 → 딜레이 후 다음 문자
void KeyboardTool::OnCharKeyUp(
    std::string remaining_text,
    int delay_ms,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(keyUp/char)", callback)) {
    return;
  }

  if (remaining_text.empty()) {
    LOG(INFO) << "[KeyboardTool] 지연 타이핑 완료";
    std::move(callback).Run(MakeSuccessResult("텍스트 입력이 완료되었습니다."));
    return;
  }

  // 딜레이 타이머 설정 후 다음 문자 발송
  char_delay_timer_.Start(
      FROM_HERE,
      base::Milliseconds(delay_ms),
      base::BindOnce(&KeyboardTool::DispatchNextChar,
                     weak_factory_.GetWeakPtr(),
                     std::move(remaining_text), delay_ms, session,
                     std::move(callback)));
}

// type 완료 콜백 (insertText 방식)
void KeyboardTool::OnInsertText(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (HandleCdpError(response, "Input.insertText", callback)) {
    return;
  }
  LOG(INFO) << "[KeyboardTool] 텍스트 삽입 완료";
  std::move(callback).Run(MakeSuccessResult("텍스트 입력이 완료되었습니다."));
}

// press/shortcut 모드: keyDown 발송
void KeyboardTool::ExecutePress(const std::string& key,
                                int modifiers,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params = MakeKeyEventParams("keyDown", key, modifiers);

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&KeyboardTool::OnKeyDown,
                     weak_factory_.GetWeakPtr(),
                     key, modifiers, session, std::move(callback)));
}

// keyDown 완료 후 keyUp 발송
void KeyboardTool::OnKeyDown(const std::string& key,
                             int modifiers,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback,
                             base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(keyDown)", callback)) {
    return;
  }

  base::DictValue params = MakeKeyEventParams("keyUp", key, modifiers);

  session->SendCdpCommand(
      "Input.dispatchKeyEvent", std::move(params),
      base::BindOnce(&KeyboardTool::OnKeyUp,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// keyUp 완료 콜백
void KeyboardTool::OnKeyUp(base::OnceCallback<void(base::Value)> callback,
                           base::Value response) {
  if (HandleCdpError(response, "Input.dispatchKeyEvent(keyUp)", callback)) {
    return;
  }
  LOG(INFO) << "[KeyboardTool] 키 입력 완료";
  std::move(callback).Run(MakeSuccessResult("키 입력이 완료되었습니다."));
}

// modifiers 문자열 배열 → CDP 비트 플래그 정수
// CDP 규격: Alt=1, Ctrl=2, Meta=4, Shift=8
// NOLINTNEXTLINE(runtime/references)
int KeyboardTool::ComputeModifiers(const base::ListValue* modifiers_list) {
  if (!modifiers_list) {
    return 0;
  }
  int flags = 0;
  for (const base::Value& item : *modifiers_list) {
    const std::string* mod = item.GetIfString();
    if (!mod) {
      continue;
    }
    if (*mod == "alt") {
      flags |= 1;
    } else if (*mod == "ctrl") {
      flags |= 2;
    } else if (*mod == "meta") {
      flags |= 4;
    } else if (*mod == "shift") {
      flags |= 8;
    }
  }
  return flags;
}

// 정적 헬퍼: CDP 에러 처리
// NOLINTNEXTLINE(runtime/references)
bool KeyboardTool::HandleCdpError(
    const base::Value& response,
    const std::string& step_name,
    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[KeyboardTool] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

}  // namespace mcp
