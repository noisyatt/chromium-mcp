// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_PERFORMANCE_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_PERFORMANCE_TOOL_H_

#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 성능 메트릭 수집 및 트레이스 녹화 도구.
//
// CDP Performance 도메인과 Tracing 도메인을 활용한다.
//
// 지원 액션:
//   getMetrics          — Performance.getMetrics: JSHeapUsedSize, Nodes 등
//   startTrace          — Tracing.start: 트레이스 녹화 시작
//   stopTrace           — Tracing.end: 녹화 중지, 데이터 수집 및 반환
//   getNavigationTiming — Performance.enable 후 Runtime.evaluate로
//                         Navigation Timing API 수집
//
// Tracing 데이터 수집 흐름:
//   1. Tracing.start  → 녹화 시작
//   2. (이벤트 수신)  Tracing.dataCollected → 트레이스 청크를 누적
//   3. Tracing.end    → 수집 종료 신호
//   4. (이벤트 수신)  Tracing.tracingComplete → 누적 데이터 전체 반환
//      dataLossOccurred 필드로 버퍼 오버플로 여부 확인 가능
//
// savePath가 지정된 경우 트레이스 데이터를 JSON 파일로 저장한다.
// (실제 파일 I/O는 브라우저 프로세스의 base::WriteFile을 사용)
class PerformanceTool : public McpTool {
 public:
  PerformanceTool();
  ~PerformanceTool() override;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::DictValue input_schema() const override;
  void Execute(const base::DictValue& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // -----------------------------------------------------------------------
  // 액션별 처리
  // -----------------------------------------------------------------------

  // action="getMetrics": Performance.getMetrics 호출.
  // JSHeapUsedSize, Nodes, LayoutCount, RecalcStyleCount 등을 반환한다.
  void ExecuteGetMetrics(McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  // action="startTrace": Tracing.start 호출.
  // |categories|: 트레이스 카테고리 (쉼표 구분)
  // Tracing.dataCollected 이벤트 핸들러도 함께 등록한다.
  void ExecuteStartTrace(const std::string& categories,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback);

  // action="stopTrace": Tracing.end 호출.
  // Tracing.tracingComplete 이벤트를 기다려 누적된 데이터를 반환한다.
  // |save_path|: 트레이스 파일 저장 경로 (빈 문자열이면 저장하지 않음)
  void ExecuteStopTrace(const std::string& save_path,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback);

  // action="getNavigationTiming":
  // Performance.enable → Runtime.evaluate(performance.getEntriesByType('navigation'))
  void ExecuteGetNavigationTiming(McpSession* session,
                                   base::OnceCallback<void(base::Value)> callback);

  // -----------------------------------------------------------------------
  // CDP 응답 / 이벤트 콜백
  // -----------------------------------------------------------------------

  // Performance.getMetrics 응답 처리
  void OnMetricsReceived(base::OnceCallback<void(base::Value)> callback,
                         base::Value response);

  // Tracing.start 응답 처리
  void OnTraceStarted(base::OnceCallback<void(base::Value)> callback,
                      base::Value response);

  // Tracing.dataCollected 이벤트 핸들러.
  // 트레이스 데이터는 여러 청크로 분할 전달되므로 trace_chunks_에 누적한다.
  void OnTraceDataCollected(const std::string& event_name,
                             const base::DictValue& params);

  // Tracing.tracingComplete 이벤트 핸들러.
  // 모든 데이터가 전달됐음을 알리는 신호. 누적 데이터를 반환 콜백에 전달한다.
  void OnTracingComplete(const std::string& event_name,
                          const base::DictValue& params);

  // Tracing.end 응답 처리
  void OnTraceEnded(base::OnceCallback<void(base::Value)> callback,
                    base::Value response);

  // Performance.enable 응답 처리 후 Navigation Timing을 평가한다.
  void OnPerformanceEnabled(McpSession* session,
                             base::OnceCallback<void(base::Value)> callback,
                             base::Value response);

  // Runtime.evaluate(Navigation Timing) 응답 처리
  void OnNavigationTimingReceived(base::OnceCallback<void(base::Value)> callback,
                                   base::Value response);

  // -----------------------------------------------------------------------
  // 헬퍼
  // -----------------------------------------------------------------------

  // categories 문자열을 base::ListValue로 파싱한다.
  static base::ListValue ParseCategories(const std::string& categories);

  // -----------------------------------------------------------------------
  // 상태
  // -----------------------------------------------------------------------

  // 트레이스 녹화 중 여부
  bool is_tracing_ = false;

  // Tracing.dataCollected 이벤트로 수신된 트레이스 이벤트 청크 누적 버퍼.
  // 각 청크는 base::ListValue 형태이다.
  std::vector<base::ListValue> trace_chunks_;

  // stopTrace 완료 콜백 (Tracing.tracingComplete 이벤트 수신 시 호출됨)
  base::OnceCallback<void(base::Value)> stop_trace_callback_;

  // stopTrace 시 저장 경로 (빈 문자열이면 저장 안 함)
  std::string save_path_;

  // stopTrace 시 사용할 McpSession 포인터 (이벤트 핸들러 해제용)
  McpSession* tracing_session_ = nullptr;

  base::WeakPtrFactory<PerformanceTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_PERFORMANCE_TOOL_H_
