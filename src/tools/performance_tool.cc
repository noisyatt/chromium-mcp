// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/performance_tool.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

// -----------------------------------------------------------------------
// CDP 이벤트 이름 상수
// -----------------------------------------------------------------------

// Tracing 도메인: 트레이스 데이터 청크 이벤트 (여러 번 발생)
constexpr char kTracingDataCollectedEvent[] = "Tracing.dataCollected";

// Tracing 도메인: 트레이스 완료 이벤트 (마지막에 한 번 발생)
constexpr char kTracingCompleteEvent[] = "Tracing.tracingComplete";

// 기본 트레이스 카테고리
constexpr char kDefaultCategories[] =
    "devtools.timeline,v8,v8.execute,blink.console,"
    "disabled-by-default-devtools.timeline,"
    "disabled-by-default-v8.cpu_profiler";

// -----------------------------------------------------------------------
// 생성자 / 소멸자
// -----------------------------------------------------------------------

PerformanceTool::PerformanceTool() = default;
PerformanceTool::~PerformanceTool() = default;

// -----------------------------------------------------------------------
// McpTool 인터페이스
// -----------------------------------------------------------------------

std::string PerformanceTool::name() const {
  return "performance";
}

std::string PerformanceTool::description() const {
  return "퍼포먼스 메트릭 수집 및 트레이스 기록. "
         "Performance.getMetrics, Tracing.start/stop, "
         "Navigation Timing API 조회를 지원한다.";
}

base::Value::Dict PerformanceTool::input_schema() const {
  base::Value::Dict schema;
  schema.Set("type", "object");

  base::Value::Dict properties;

  // action
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    base::Value::List enums;
    enums.Append("getMetrics");
    enums.Append("startTrace");
    enums.Append("stopTrace");
    enums.Append("getNavigationTiming");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "수행할 작업: "
             "getMetrics(성능 메트릭 수집), "
             "startTrace(트레이스 시작), "
             "stopTrace(트레이스 중지 및 데이터 반환), "
             "getNavigationTiming(Navigation Timing API 조회)");
    properties.Set("action", std::move(prop));
  }

  // categories: 트레이스 카테고리 (startTrace 시 사용)
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    prop.Set("description",
             "트레이스 카테고리 (쉼표로 구분). "
             "예: \"devtools.timeline,v8\". "
             "기본값: devtools.timeline,v8 외 주요 카테고리");
    prop.Set("default", "devtools.timeline,v8");
    properties.Set("categories", std::move(prop));
  }

  // savePath: 트레이스 결과 저장 경로 (stopTrace 시 사용)
  {
    base::Value::Dict prop;
    prop.Set("type", "string");
    prop.Set("description",
             "트레이스 데이터를 저장할 파일 경로 (JSON 형식). "
             "생략하면 결과를 응답에 직접 포함한다.");
    properties.Set("savePath", std::move(prop));
  }

  schema.Set("properties", std::move(properties));

  base::Value::List required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void PerformanceTool::Execute(const base::Value::Dict& arguments,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback) {
  const std::string* action_ptr = arguments.FindString("action");
  if (!action_ptr) {
    base::Value::Dict err;
    err.Set("error", "action 파라미터가 필요합니다");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const std::string& action = *action_ptr;
  LOG(INFO) << "[PerformanceTool] Execute: action=" << action;

  if (action == "getMetrics") {
    ExecuteGetMetrics(session, std::move(callback));

  } else if (action == "startTrace") {
    const std::string* cats = arguments.FindString("categories");
    std::string categories = cats ? *cats : kDefaultCategories;
    ExecuteStartTrace(categories, session, std::move(callback));

  } else if (action == "stopTrace") {
    const std::string* path_ptr = arguments.FindString("savePath");
    std::string save_path = path_ptr ? *path_ptr : "";
    ExecuteStopTrace(save_path, session, std::move(callback));

  } else if (action == "getNavigationTiming") {
    ExecuteGetNavigationTiming(session, std::move(callback));

  } else {
    base::Value::Dict err;
    err.Set("error", "알 수 없는 action: " + action);
    std::move(callback).Run(base::Value(std::move(err)));
  }
}

// -----------------------------------------------------------------------
// getMetrics
// -----------------------------------------------------------------------

void PerformanceTool::ExecuteGetMetrics(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // Performance.getMetrics: 현재 페이지의 런타임 성능 카운터를 반환한다.
  // 주요 메트릭:
  //   - JSHeapUsedSize: 사용 중인 JS 힙 크기 (bytes)
  //   - JSHeapTotalSize: 전체 JS 힙 크기 (bytes)
  //   - Nodes: DOM 노드 수
  //   - LayoutCount: 레이아웃 수행 횟수
  //   - RecalcStyleCount: 스타일 재계산 횟수
  //   - LayoutDuration: 누적 레이아웃 소요 시간 (초)
  //   - RecalcStyleDuration: 누적 스타일 재계산 소요 시간 (초)
  LOG(INFO) << "[PerformanceTool] Performance.getMetrics 호출";

  session->SendCdpCommand(
      "Performance.getMetrics", base::Value::Dict(),
      base::BindOnce(&PerformanceTool::OnMetricsReceived,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void PerformanceTool::OnMetricsReceived(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    base::Value::Dict result;
    result.Set("error", "예상치 못한 CDP 응답 형식");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::Value::Dict& dict = response.GetDict();

  // CDP 오류 확인
  const base::Value::Dict* err = dict.FindDict("error");
  if (err) {
    const std::string* msg = err->FindString("message");
    base::Value::Dict result;
    result.Set("error", msg ? *msg : "Performance.getMetrics 실패");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // result.metrics 배열 추출
  const base::Value::Dict* res = dict.FindDict("result");
  if (res) {
    const base::Value::List* metrics = res->FindList("metrics");
    if (metrics) {
      // 주요 메트릭 요약 맵 생성
      base::Value::Dict summary;
      for (const auto& item : *metrics) {
        if (!item.is_dict()) continue;
        const base::Value::Dict& m = item.GetDict();
        const std::string* name = m.FindString("name");
        std::optional<double> value = m.FindDouble("value");
        if (name && value.has_value()) {
          summary.Set(*name, *value);
        }
      }

      LOG(INFO) << "[PerformanceTool] 성능 메트릭 수집 완료, count="
                << metrics->size();

      base::Value::Dict result;
      result.Set("success", true);
      result.Set("metrics", metrics->Clone());
      result.Set("summary", std::move(summary));
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }
  }

  base::Value::Dict result;
  result.Set("success", true);
  result.Set("metrics", base::Value::List());
  result.Set("warning",
             "성능 메트릭을 찾을 수 없습니다. "
             "Performance.enable이 필요할 수 있습니다.");
  std::move(callback).Run(base::Value(std::move(result)));
}

// -----------------------------------------------------------------------
// startTrace
// -----------------------------------------------------------------------

void PerformanceTool::ExecuteStartTrace(
    const std::string& categories,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (is_tracing_) {
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error", "이미 트레이스가 진행 중입니다. stopTrace를 먼저 호출하세요.");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // 이전 트레이스 데이터 초기화
  trace_chunks_.clear();
  tracing_session_ = session;

  // Tracing.dataCollected 이벤트 핸들러 미리 등록.
  // startTrace 응답 이전에 이벤트가 발생할 수 있으므로 사전 등록이 필요하다.
  session->RegisterCdpEventHandler(
      kTracingDataCollectedEvent,
      base::BindRepeating(&PerformanceTool::OnTraceDataCollected,
                          weak_factory_.GetWeakPtr()));

  // traceConfig 설정
  base::Value::Dict trace_config;
  trace_config.Set("recordMode", "recordUntilFull");
  trace_config.Set("includedCategories", ParseCategories(categories));

  base::Value::Dict params;
  params.Set("traceConfig", std::move(trace_config));
  // 1초마다 버퍼 사용량 보고 (버퍼 오버플로 조기 감지)
  params.Set("bufferUsageReportingInterval", 1000);

  LOG(INFO) << "[PerformanceTool] Tracing.start 호출, categories=" << categories;

  session->SendCdpCommand(
      "Tracing.start", std::move(params),
      base::BindOnce(&PerformanceTool::OnTraceStarted,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void PerformanceTool::OnTraceStarted(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (response.is_dict()) {
    const base::Value::Dict* err = response.GetDict().FindDict("error");
    if (err) {
      const std::string* msg = err->FindString("message");
      LOG(ERROR) << "[PerformanceTool] 트레이스 시작 실패: "
                 << (msg ? *msg : "알 수 없는 오류");

      // 핸들러 등록 취소
      if (tracing_session_) {
        tracing_session_->UnregisterCdpEventHandler(kTracingDataCollectedEvent);
        tracing_session_ = nullptr;
      }

      base::Value::Dict result;
      result.Set("success", false);
      result.Set("error", msg ? *msg : "트레이스 시작 실패");
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }
  }

  is_tracing_ = true;
  LOG(INFO) << "[PerformanceTool] 트레이스 녹화 시작됨";

  base::Value::Dict result;
  result.Set("success", true);
  result.Set("message",
             "트레이스 녹화가 시작되었습니다. "
             "stopTrace 액션으로 중지하세요.");
  result.Set("status", "recording");
  std::move(callback).Run(base::Value(std::move(result)));
}

// -----------------------------------------------------------------------
// Tracing.dataCollected 이벤트 — 트레이스 청크 누적
// -----------------------------------------------------------------------

void PerformanceTool::OnTraceDataCollected(
    const std::string& event_name,
    const base::Value::Dict& params) {
  // Tracing.dataCollected 이벤트 파라미터:
  //   value: TraceEvent[] — 트레이스 이벤트 배열 (청크)
  //
  // 트레이스 데이터는 버퍼 크기에 따라 여러 청크로 분할 전달된다.
  // 모든 청크를 trace_chunks_에 누적하고 Tracing.tracingComplete 이벤트
  // 수신 시 합산하여 반환한다.
  const base::Value::List* value = params.FindList("value");
  if (value) {
    trace_chunks_.push_back(value->Clone());
    VLOG(1) << "[PerformanceTool] 트레이스 청크 수신: "
            << value->size() << "개 이벤트, 누적 청크=" << trace_chunks_.size();
  }
}

// -----------------------------------------------------------------------
// Tracing.tracingComplete 이벤트 — 트레이스 완료
// -----------------------------------------------------------------------

void PerformanceTool::OnTracingComplete(
    const std::string& event_name,
    const base::Value::Dict& params) {
  // Tracing.tracingComplete 이벤트 파라미터:
  //   dataLossOccurred: bool — 버퍼 오버플로로 데이터 손실 여부
  //   stream: string — IO 스트림 핸들 (stream 모드 사용 시)
  //   traceFormat: string — 트레이스 형식
  std::optional<bool> data_loss = params.FindBool("dataLossOccurred");

  if (data_loss.value_or(false)) {
    LOG(WARNING) << "[PerformanceTool] 트레이스 버퍼 오버플로 발생: 일부 데이터 손실";
  }

  // 이벤트 핸들러 해제
  if (tracing_session_) {
    tracing_session_->UnregisterCdpEventHandler(kTracingDataCollectedEvent);
    tracing_session_->UnregisterCdpEventHandler(kTracingCompleteEvent);
    tracing_session_ = nullptr;
  }
  is_tracing_ = false;

  // 누적된 청크를 하나의 배열로 합산
  base::Value::List all_events;
  int total_events = 0;
  for (auto& chunk : trace_chunks_) {
    total_events += static_cast<int>(chunk.size());
    for (auto& event : chunk) {
      all_events.Append(std::move(event));
    }
  }
  trace_chunks_.clear();

  LOG(INFO) << "[PerformanceTool] 트레이스 완료: 총 이벤트 수=" << total_events;

  // savePath가 지정된 경우 파일로 저장
  if (!save_path_.empty()) {
    // clear 전에 경로 보존
    std::string saved_path = save_path_;

    // {traceEvents: [...]} 형식으로 직렬화
    base::Value::Dict trace_doc;
    trace_doc.Set("traceEvents", std::move(all_events));

    std::string json_str;
    bool write_ok = false;
    if (base::JSONWriter::Write(base::Value(std::move(trace_doc)), &json_str)) {
      base::FilePath file_path(saved_path);
      write_ok = base::WriteFile(file_path, json_str);
      if (write_ok) {
        LOG(INFO) << "[PerformanceTool] 트레이스 파일 저장 완료: " << saved_path;
      } else {
        LOG(ERROR) << "[PerformanceTool] 트레이스 파일 저장 실패: " << saved_path;
      }
    }
    save_path_.clear();

    if (stop_trace_callback_) {
      base::Value::Dict result;
      result.Set("success", true);
      result.Set("message", "트레이스 녹화가 완료되었습니다");
      result.Set("eventCount", total_events);
      result.Set("dataLossOccurred", data_loss.value_or(false));
      result.Set("savedTo", saved_path);
      result.Set("writeSuccess", write_ok);
      std::move(stop_trace_callback_).Run(base::Value(std::move(result)));
    }
    return;
  }

  // savePath 미지정: 이벤트 배열을 응답에 직접 포함
  if (stop_trace_callback_) {
    base::Value::Dict result;
    result.Set("success", true);
    result.Set("message", "트레이스 녹화가 완료되었습니다");
    result.Set("eventCount", total_events);
    result.Set("dataLossOccurred", data_loss.value_or(false));
    result.Set("traceEvents", std::move(all_events));
    std::move(stop_trace_callback_).Run(base::Value(std::move(result)));
  }
}

// -----------------------------------------------------------------------
// stopTrace
// -----------------------------------------------------------------------

void PerformanceTool::ExecuteStopTrace(
    const std::string& save_path,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (!is_tracing_) {
    base::Value::Dict result;
    result.Set("success", false);
    result.Set("error",
               "진행 중인 트레이스가 없습니다. startTrace를 먼저 호출하세요.");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // 콜백과 경로를 멤버에 저장 — Tracing.tracingComplete 이벤트에서 사용
  stop_trace_callback_ = std::move(callback);
  save_path_ = save_path;
  tracing_session_ = session;

  // Tracing.tracingComplete 이벤트 핸들러 등록
  session->RegisterCdpEventHandler(
      kTracingCompleteEvent,
      base::BindRepeating(&PerformanceTool::OnTracingComplete,
                          weak_factory_.GetWeakPtr()));

  LOG(INFO) << "[PerformanceTool] Tracing.end 호출";

  session->SendCdpCommand(
      "Tracing.end", base::Value::Dict(),
      base::BindOnce(&PerformanceTool::OnTraceEnded,
                     weak_factory_.GetWeakPtr(),
                     // stopTrace의 최종 결과는 OnTracingComplete에서 반환되므로
                     // Tracing.end 응답 콜백은 오류 처리 전용으로 사용한다.
                     base::OnceCallback<void(base::Value)>()));
}

void PerformanceTool::OnTraceEnded(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // Tracing.end 응답에서 오류가 발생한 경우에만 처리한다.
  // 정상 케이스에서는 이후 Tracing.tracingComplete 이벤트로 데이터가 전달된다.
  if (response.is_dict()) {
    const base::Value::Dict* err = response.GetDict().FindDict("error");
    if (err) {
      const std::string* msg = err->FindString("message");
      LOG(ERROR) << "[PerformanceTool] Tracing.end 오류: "
                 << (msg ? *msg : "알 수 없는 오류");

      // 오류 시 핸들러 정리
      if (tracing_session_) {
        tracing_session_->UnregisterCdpEventHandler(kTracingDataCollectedEvent);
        tracing_session_->UnregisterCdpEventHandler(kTracingCompleteEvent);
        tracing_session_ = nullptr;
      }
      is_tracing_ = false;
      trace_chunks_.clear();
      save_path_.clear();

      if (stop_trace_callback_) {
        base::Value::Dict result;
        result.Set("success", false);
        result.Set("error", msg ? *msg : "Tracing.end 실패");
        std::move(stop_trace_callback_).Run(base::Value(std::move(result)));
      }
    }
  }
  // callback 파라미터는 사용하지 않음 (stopTrace 콜백은 stop_trace_callback_에 저장)
}

// -----------------------------------------------------------------------
// getNavigationTiming
// -----------------------------------------------------------------------

void PerformanceTool::ExecuteGetNavigationTiming(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // Performance.enable: Performance 도메인을 활성화한다.
  // 이후 Runtime.evaluate로 Navigation Timing API를 조회한다.
  LOG(INFO) << "[PerformanceTool] Performance.enable 호출";

  session->SendCdpCommand(
      "Performance.enable", base::Value::Dict(),
      base::BindOnce(&PerformanceTool::OnPerformanceEnabled,
                     weak_factory_.GetWeakPtr(), session,
                     std::move(callback)));
}

void PerformanceTool::OnPerformanceEnabled(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // Performance.enable 오류 무시: 이미 활성화된 경우도 있으므로 계속 진행한다.
  if (response.is_dict()) {
    const base::Value::Dict* err = response.GetDict().FindDict("error");
    if (err) {
      LOG(WARNING) << "[PerformanceTool] Performance.enable 경고 (무시하고 계속)";
    }
  }

  // Navigation Timing API: performance.getEntriesByType('navigation')[0]
  // PerformanceNavigationTiming 객체의 주요 필드:
  //   startTime, redirectCount, redirectStart, redirectEnd,
  //   fetchStart, domainLookupStart, domainLookupEnd,
  //   connectStart, connectEnd, secureConnectionStart,
  //   requestStart, responseStart, responseEnd,
  //   domInteractive, domContentLoadedEventStart, domContentLoadedEventEnd,
  //   domComplete, loadEventStart, loadEventEnd,
  //   type (navigate/reload/back_forward/prerender), transferSize
  constexpr char kNavTimingScript[] = R"(
    (() => {
      try {
        const entries = performance.getEntriesByType('navigation');
        if (!entries || entries.length === 0) {
          return JSON.stringify({ error: 'Navigation Timing 항목 없음' });
        }
        const nav = entries[0];
        // toJSON()으로 직렬화 가능한 객체를 얻는다.
        const data = nav.toJSON ? nav.toJSON() : {
          startTime: nav.startTime,
          redirectCount: nav.redirectCount,
          fetchStart: nav.fetchStart,
          requestStart: nav.requestStart,
          responseStart: nav.responseStart,
          responseEnd: nav.responseEnd,
          domInteractive: nav.domInteractive,
          domContentLoadedEventEnd: nav.domContentLoadedEventEnd,
          domComplete: nav.domComplete,
          loadEventEnd: nav.loadEventEnd,
          type: nav.type,
          transferSize: nav.transferSize
        };
        // 파생 메트릭 계산
        data.ttfb = nav.responseStart - nav.requestStart;
        data.domContentLoaded = nav.domContentLoadedEventEnd - nav.startTime;
        data.loadTime = nav.loadEventEnd - nav.startTime;
        data.dnsLookup = nav.domainLookupEnd - nav.domainLookupStart;
        data.tcpConnect = nav.connectEnd - nav.connectStart;
        data.url = location.href;
        return JSON.stringify({ success: true, navigationTiming: data });
      } catch (e) {
        return JSON.stringify({ error: e.toString() });
      }
    })()
  )";

  base::Value::Dict params;
  params.Set("expression", kNavTimingScript);
  params.Set("returnByValue", true);
  params.Set("awaitPromise", false);

  LOG(INFO) << "[PerformanceTool] Navigation Timing 평가 시작";

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&PerformanceTool::OnNavigationTimingReceived,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void PerformanceTool::OnNavigationTimingReceived(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    base::Value::Dict result;
    result.Set("error", "예상치 못한 CDP 응답 형식");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::Value::Dict& dict = response.GetDict();
  const base::Value::Dict* err = dict.FindDict("error");
  if (err) {
    const std::string* msg = err->FindString("message");
    base::Value::Dict result;
    result.Set("error", msg ? *msg : "Runtime.evaluate 실패");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // Runtime.evaluate 응답: result.result.value에 JSON 문자열이 들어있다.
  const base::Value::Dict* res = dict.FindDict("result");
  if (res) {
    const base::Value::Dict* inner_result = res->FindDict("result");
    if (inner_result) {
      const std::string* value_str = inner_result->FindString("value");
      if (value_str) {
        LOG(INFO) << "[PerformanceTool] Navigation Timing 수집 완료";
        base::Value::Dict result;
        result.Set("success", true);
        result.Set("data", *value_str);
        std::move(callback).Run(base::Value(std::move(result)));
        return;
      }
    }
  }

  base::Value::Dict result;
  result.Set("success", false);
  result.Set("error", "Navigation Timing 데이터를 파싱할 수 없습니다");
  std::move(callback).Run(base::Value(std::move(result)));
}

// -----------------------------------------------------------------------
// 정적 헬퍼
// -----------------------------------------------------------------------

// static
base::Value::List PerformanceTool::ParseCategories(
    const std::string& categories) {
  base::Value::List list;
  std::string remaining = categories;

  while (!remaining.empty()) {
    size_t comma = remaining.find(',');
    std::string cat;
    if (comma == std::string::npos) {
      cat = remaining;
      remaining.clear();
    } else {
      cat = remaining.substr(0, comma);
      remaining = remaining.substr(comma + 1);
    }
    // 앞뒤 공백 제거
    size_t s = cat.find_first_not_of(' ');
    size_t e = cat.find_last_not_of(' ');
    if (s != std::string::npos) {
      list.Append(cat.substr(s, e - s + 1));
    }
  }

  return list;
}

}  // namespace mcp
