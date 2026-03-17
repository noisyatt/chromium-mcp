// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_EMULATION_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_EMULATION_TOOL_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 디바이스/환경 에뮬레이션 도구.
//
// 하나의 Execute 호출에서 여러 에뮬레이션 설정을 동시에 적용할 수 있다.
// 각 설정은 해당 CDP 명령을 순차 호출하며, 모든 명령 완료 후 최종 결과를
// 반환한다.
//
// 지원 에뮬레이션 항목:
//   - viewport         : Emulation.setDeviceMetricsOverride
//                        (width, height, deviceScaleFactor, isMobile,
//                         hasTouch, isLandscape)
//   - geolocation      : Emulation.setGeolocationOverride
//                        (latitude, longitude, accuracy)
//   - colorScheme      : Emulation.setEmulatedMedia
//                        (prefers-color-scheme: light/dark/auto)
//   - timezone         : Emulation.setTimezoneOverride (예: "Asia/Seoul")
//   - locale           : Emulation.setLocaleOverride   (예: "ko-KR")
//   - userAgent        : Emulation.setUserAgentOverride
//   - cpuThrottling    : Emulation.setCPUThrottlingRate (배율; 1=비활성)
//   - networkConditions: Network.emulateNetworkConditions
//                        (offline/slow3g/fast3g/slow4g/fast4g 또는 custom)
//   - reset            : 각 에뮬레이션 설정을 기본값으로 복원
class EmulationTool : public McpTool {
 public:
  EmulationTool();
  ~EmulationTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // 여러 CDP 명령을 순차 실행하기 위한 컨텍스트.
  // commands 목록의 명령을 하나씩 실행하고, 결과를 applied/errors에 수집한다.
  struct ExecuteContext {
    // 실행 대기 중인 CDP 명령 목록 (method, params 쌍)
    std::vector<std::pair<std::string, base::Value::Dict>> commands;
    // 현재 실행 인덱스
    size_t current_index = 0;
    // 성공한 설정 이름 목록
    std::vector<std::string> applied;
    // 실패 오류 메시지 목록
    std::vector<std::string> errors;
    // 모든 명령 완료 후 호출할 최종 콜백
    base::OnceCallback<void(base::Value)> callback;
  };

  // 네트워크 조건 사전 정의 프리셋 (networkConditions enum 값에 대응)
  struct NetworkPreset {
    bool offline;
    double download_throughput;  // bytes/sec (-1 = 제한 없음)
    double upload_throughput;    // bytes/sec (-1 = 제한 없음)
    double latency;              // ms
  };

  // 다음 CDP 명령 실행. 모든 명령이 끝나면 최종 결과를 반환한다.
  void ExecuteNextCommand(std::unique_ptr<ExecuteContext> ctx,
                          McpSession* session);

  // CDP 명령 응답을 처리하고 다음 명령을 실행한다.
  void OnCommandResponse(std::unique_ptr<ExecuteContext> ctx,
                         McpSession* session,
                         const std::string& setting_name,
                         base::Value response);

  // reset=true 시 모든 에뮬레이션 해제 명령을 commands에 추가한다.
  static void AppendResetCommands(
      std::vector<std::pair<std::string, base::Value::Dict>>& commands);

  // networkConditions 프리셋 이름을 NetworkPreset으로 변환한다.
  // 알 수 없는 프리셋이면 nullopt를 반환한다.
  static std::optional<NetworkPreset> GetNetworkPreset(
      const std::string& preset_name);

  base::WeakPtrFactory<EmulationTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_EMULATION_TOOL_H_
