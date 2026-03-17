// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_CONSOLE_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_CONSOLE_TOOL_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 브라우저 콘솔 메시지를 캡처하고 조회하는 도구.
//
// ★ 은닉 모드(safe_mode=true, 기본값) vs 전체 모드(safe_mode=false) ★
//
//  [안전 모드 — Log.enable만 사용]
//   CDP Log 도메인(Log.enable + Log.entryAdded)을 활용한다.
//   Runtime.enable을 호출하지 않으므로 DevTools 자동화 탐지 위험이 없다.
//   페이지의 console.* 호출이 브라우저 내부 Log 도메인으로 라우팅되므로
//   일반적인 콘솔 메시지는 모두 수신할 수 있다.
//   단, args 배열(RemoteObject)이 아닌 텍스트 형태로만 메시지를 얻는다.
//
//  [전체 모드 — Runtime.enable + Runtime.consoleAPICalled]
//   Runtime.enable을 호출하여 Runtime.consoleAPICalled 이벤트를 수신한다.
//   args 배열로부터 각 인수의 상세 타입·값을 확인할 수 있다.
//   단, Runtime.enable은 브라우저의 executionContextCreated 이벤트를 발생시켜
//   일부 봇 탐지 사이트에서 자동화로 인식될 수 있다.
//   → 은닉이 필요한 경우 safe_mode=true(기본)를 사용할 것.
//
// CDP 흐름:
//   start(safe_mode=true) :
//     Log.enable → Log.entryAdded 이벤트 수신
//   start(safe_mode=false) :
//     Runtime.enable → Runtime.consoleAPICalled 이벤트 수신
//   stop   : 등록된 이벤트 핸들러 해제
//   get    : 내부 버퍼 반환 (level/pattern 필터 적용)
//   clear  : 내부 버퍼 초기화
class ConsoleTool : public McpTool {
 public:
  ConsoleTool();
  ~ConsoleTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // -----------------------------------------------------------------------
  // 캡처 시작 — 두 가지 모드
  // -----------------------------------------------------------------------

  // safe_mode=true: Log.enable 호출 후 Log.entryAdded 핸들러 등록.
  void StartSafeMode(McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // Log.enable CDP 응답 처리 후 Log.entryAdded 핸들러를 등록한다.
  void OnLogEnabled(McpSession* session,
                    base::OnceCallback<void(base::Value)> callback,
                    base::Value response);

  // Log.entryAdded 이벤트 핸들러.
  // 이벤트 파라미터에서 entry(level, text, timestamp, url, lineNumber)를 추출한다.
  void OnLogEntryAdded(const std::string& event_name,
                       const base::DictValue& params);

  // safe_mode=false: Runtime.enable 호출 후 Runtime.consoleAPICalled 핸들러 등록.
  // 주의: Runtime.enable은 DevTools 감지 위험이 있다. 필요할 때만 사용할 것.
  void StartFullMode(McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // Runtime.enable CDP 응답 처리 후 Runtime.consoleAPICalled 핸들러를 등록한다.
  void OnRuntimeEnabled(McpSession* session,
                        base::OnceCallback<void(base::Value)> callback,
                        base::Value response);

  // Runtime.consoleAPICalled 이벤트 핸들러.
  // args 배열(RemoteObject)에서 메시지 텍스트를 조합한다.
  void OnConsoleApiCalled(const std::string& event_name,
                          const base::DictValue& params);

  // -----------------------------------------------------------------------
  // 캡처 중지 / 조회 / 클리어
  // -----------------------------------------------------------------------

  // 등록된 CDP 이벤트 핸들러를 해제하고 캡처를 중지한다.
  void StopCapture(McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // 버퍼에서 메시지를 조회한다.
  // |level_filter|: "all" 또는 특정 레벨 문자열
  // |pattern_filter|: 정규식 패턴 (빈 문자열이면 모두 통과)
  // |limit|: 최대 반환 개수 (0이면 제한 없음)
  void GetMessages(const std::string& level_filter,
                   const std::string& pattern_filter,
                   int limit,
                   base::OnceCallback<void(base::Value)> callback);

  // 메시지 버퍼를 비운다.
  void ClearMessages(base::OnceCallback<void(base::Value)> callback);

  // -----------------------------------------------------------------------
  // 헬퍼
  // -----------------------------------------------------------------------

  // CDP RuntimeAPI args 배열(RemoteObject[])에서 텍스트를 조합한다.
  static std::string ExtractTextFromArgs(const base::ListValue& args);

  // |msg_level|이 |level_filter| 조건에 부합하는지 검사한다.
  // level_filter == "all"이면 항상 true.
  static bool MatchesLevel(const std::string& msg_level,
                            const std::string& level_filter);

  // -----------------------------------------------------------------------
  // 상태
  // -----------------------------------------------------------------------

  // 캡처된 콘솔 메시지 버퍼.
  // 각 항목: {level, text, timestamp, url, lineNumber}
  std::vector<base::DictValue> messages_;

  // 현재 캡처 중인지 여부
  bool is_capturing_ = false;

  // 안전 모드(Log.enable) 여부. start 시점에 결정된다.
  bool safe_mode_ = true;

  base::WeakPtrFactory<ConsoleTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_CONSOLE_TOOL_H_
