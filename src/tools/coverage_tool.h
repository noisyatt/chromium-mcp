// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_COVERAGE_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_COVERAGE_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// CSS 및 JavaScript 코드 커버리지를 측정하는 도구.
//
// CSS 커버리지:
//   CDP CSS 도메인을 활용하여 페이지에서 실제로 사용된/미사용된 CSS 규칙을 추적한다.
//   CDP 흐름:
//     startCSS: CSS.enable → CSS.startRuleUsageTracking
//     stopCSS:  CSS.stopRuleUsageTracking → 규칙 목록 분석 → CSS.disable
//
//   반환:
//     ruleUsage[]: {styleSheetId, startOffset, endOffset, used}
//     summary: {totalRules, usedRules, unusedRules, usagePercent}
//
// JS 커버리지:
//   CDP Profiler 도메인을 활용하여 함수/블록 단위 실행 여부를 추적한다.
//   CDP 흐름:
//     startJS: Profiler.enable →
//              Profiler.startPreciseCoverage(callCount=true, detailed=true)
//     stopJS:  Profiler.takePreciseCoverage → Profiler.stopPreciseCoverage
//              → Profiler.disable
//
//   반환:
//     scripts[]: {scriptId, url, functions[{functionName, ranges[{...}]}]}
//     summary: {totalBytes, usedBytes, unusedBytes, usagePercent, unusedUrls[]}
//
// detailed=true이면 블록 단위(startOffset/endOffset)의 세부 범위를 포함한다.
// detailed=false이면 함수 단위의 요약 정보만 반환한다.
class CoverageTool : public McpTool {
 public:
  CoverageTool();
  ~CoverageTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // -----------------------------------------------------------------------
  // CSS 커버리지
  // -----------------------------------------------------------------------

  // action="startCSS": CSS.enable + CSS.startRuleUsageTracking
  void ExecuteStartCSS(McpSession* session,
                       base::OnceCallback<void(base::Value)> callback);

  // CSS.enable 응답 처리 후 CSS.startRuleUsageTracking 호출
  void OnCssEnabled(McpSession* session,
                    base::OnceCallback<void(base::Value)> callback,
                    base::Value response);

  // CSS.startRuleUsageTracking 응답 처리
  void OnCssTrackingStarted(base::OnceCallback<void(base::Value)> callback,
                             base::Value response);

  // action="stopCSS": CSS.stopRuleUsageTracking → 결과 분석
  void ExecuteStopCSS(bool detailed,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback);

  // CSS.stopRuleUsageTracking 응답 처리 및 커버리지 집계
  void OnCssTrackingStopped(bool detailed,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback,
                             base::Value response);

  // CSS.disable 응답 처리 (정리 목적, 결과는 이미 반환됨)
  void OnCssDisabled(base::Value response);

  // -----------------------------------------------------------------------
  // JS 커버리지
  // -----------------------------------------------------------------------

  // action="startJS": Profiler.enable + Profiler.startPreciseCoverage
  void ExecuteStartJS(bool detailed,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback);

  // Profiler.enable 응답 처리 후 Profiler.startPreciseCoverage 호출
  void OnProfilerEnabled(bool detailed,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // Profiler.startPreciseCoverage 응답 처리
  void OnJsCoverageStarted(base::OnceCallback<void(base::Value)> callback,
                            base::Value response);

  // action="stopJS": Profiler.takePreciseCoverage → Profiler.stopPreciseCoverage
  void ExecuteStopJS(bool detailed,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // Profiler.takePreciseCoverage 응답 처리 및 커버리지 집계
  void OnJsCoverageTaken(bool detailed,
                          McpSession* session,
                          base::OnceCallback<void(base::Value)> callback,
                          base::Value response);

  // Profiler.stopPreciseCoverage + Profiler.disable 응답 처리 (정리)
  void OnJsCoverageStopped(base::Value response);
  void OnProfilerDisabled(base::Value response);

  // -----------------------------------------------------------------------
  // 헬퍼
  // -----------------------------------------------------------------------

  // CSS ruleUsage 배열에서 사용/미사용 통계를 집계한다.
  // detailed=false이면 summary만, detailed=true이면 ruleUsage 배열도 포함.
  static base::Value::Dict AggregateCssUsage(
      const base::Value::List& rule_usage,
      bool detailed);

  // JS ScriptCoverage 배열에서 바이트 단위 커버리지를 집계한다.
  // detailed=false이면 URL별 요약만, detailed=true이면 함수 범위 세부 정보 포함.
  static base::Value::Dict AggregateJsUsage(
      const base::Value::List& script_coverage,
      bool detailed);

  // -----------------------------------------------------------------------
  // 상태
  // -----------------------------------------------------------------------

  // CSS 추적 중 여부
  bool css_tracking_ = false;

  // JS 커버리지 측정 중 여부
  bool js_tracking_ = false;

  base::WeakPtrFactory<CoverageTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_COVERAGE_TOOL_H_
