// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/coverage_tool.h"

#include <cmath>
#include <map>
#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

// -----------------------------------------------------------------------
// 생성자 / 소멸자
// -----------------------------------------------------------------------

CoverageTool::CoverageTool() = default;
CoverageTool::~CoverageTool() = default;

// -----------------------------------------------------------------------
// McpTool 인터페이스
// -----------------------------------------------------------------------

std::string CoverageTool::name() const {
  return "coverage";
}

std::string CoverageTool::description() const {
  return "CSS/JS 코드 커버리지 측정. "
         "사용/미사용 CSS 규칙과 JS 함수/블록 실행 여부를 추적한다.";
}

base::DictValue CoverageTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // action
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue enums;
    enums.Append("startCSS");
    enums.Append("stopCSS");
    enums.Append("startJS");
    enums.Append("stopJS");
    prop.Set("enum", std::move(enums));
    prop.Set("description",
             "수행할 동작: "
             "startCSS(CSS 추적 시작), stopCSS(CSS 추적 중지 및 결과 반환), "
             "startJS(JS 커버리지 시작), stopJS(JS 커버리지 중지 및 결과 반환)");
    properties.Set("action", std::move(prop));
  }

  // detailed: 세부 범위 포함 여부
  {
    base::DictValue prop;
    prop.Set("type", "boolean");
    prop.Set("description",
             "세부 범위 포함 여부 (기본값: false). "
             "true이면 CSS는 규칙별 오프셋, JS는 블록 단위 범위를 포함한다.");
    prop.Set("default", false);
    properties.Set("detailed", std::move(prop));
  }

  schema.Set("properties", std::move(properties));

  base::ListValue required;
  required.Append("action");
  schema.Set("required", std::move(required));

  return schema;
}

void CoverageTool::Execute(const base::DictValue& arguments,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback) {
  const std::string* action_ptr = arguments.FindString("action");
  if (!action_ptr) {
    base::DictValue err;
    err.Set("error", "action 파라미터가 필요합니다");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const std::string& action = *action_ptr;
  std::optional<bool> detailed_opt = arguments.FindBool("detailed");
  bool detailed = detailed_opt.value_or(false);

  LOG(INFO) << "[CoverageTool] Execute: action=" << action
            << " detailed=" << detailed;

  if (action == "startCSS") {
    ExecuteStartCSS(session, std::move(callback));
  } else if (action == "stopCSS") {
    ExecuteStopCSS(detailed, session, std::move(callback));
  } else if (action == "startJS") {
    ExecuteStartJS(detailed, session, std::move(callback));
  } else if (action == "stopJS") {
    ExecuteStopJS(detailed, session, std::move(callback));
  } else {
    base::DictValue err;
    err.Set("error", "알 수 없는 action: " + action);
    std::move(callback).Run(base::Value(std::move(err)));
  }
}

// -----------------------------------------------------------------------
// CSS 커버리지 — startCSS
// -----------------------------------------------------------------------

void CoverageTool::ExecuteStartCSS(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (css_tracking_) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "CSS 추적이 이미 진행 중입니다. stopCSS를 먼저 호출하세요.");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[CoverageTool] CSS.enable 호출";

  // 1단계: CSS.enable → CSS 도메인 활성화
  session->SendCdpCommand(
      "CSS.enable", base::DictValue(),
      base::BindOnce(&CoverageTool::OnCssEnabled,
                     weak_factory_.GetWeakPtr(), session,
                     std::move(callback)));
}

void CoverageTool::OnCssEnabled(
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (response.is_dict()) {
    const base::DictValue* err = response.GetDict().FindDict("error");
    if (err) {
      const std::string* msg = err->FindString("message");
      base::DictValue result;
      result.Set("success", false);
      result.Set("error", msg ? *msg : "CSS.enable 실패");
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }
  }

  LOG(INFO) << "[CoverageTool] CSS.startRuleUsageTracking 호출";

  // 2단계: CSS.startRuleUsageTracking → 규칙 사용 추적 시작
  session->SendCdpCommand(
      "CSS.startRuleUsageTracking", base::DictValue(),
      base::BindOnce(&CoverageTool::OnCssTrackingStarted,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void CoverageTool::OnCssTrackingStarted(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (response.is_dict()) {
    const base::DictValue* err = response.GetDict().FindDict("error");
    if (err) {
      const std::string* msg = err->FindString("message");
      base::DictValue result;
      result.Set("success", false);
      result.Set("error", msg ? *msg : "CSS.startRuleUsageTracking 실패");
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }
  }

  css_tracking_ = true;
  LOG(INFO) << "[CoverageTool] CSS 규칙 추적 시작됨";

  base::DictValue result;
  result.Set("success", true);
  result.Set("message",
             "CSS 커버리지 추적을 시작했습니다. "
             "stopCSS 액션으로 결과를 수집하세요.");
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

// -----------------------------------------------------------------------
// CSS 커버리지 — stopCSS
// -----------------------------------------------------------------------

void CoverageTool::ExecuteStopCSS(
    bool detailed,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (!css_tracking_) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error",
               "CSS 추적이 시작되지 않았습니다. startCSS를 먼저 호출하세요.");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[CoverageTool] CSS.stopRuleUsageTracking 호출";

  // CSS.stopRuleUsageTracking: 추적을 중지하고 규칙 사용 목록을 반환한다.
  // 반환 형식:
  //   ruleUsage: [{styleSheetId, startOffset, endOffset, used}]
  session->SendCdpCommand(
      "CSS.stopRuleUsageTracking", base::DictValue(),
      base::BindOnce(&CoverageTool::OnCssTrackingStopped,
                     weak_factory_.GetWeakPtr(), detailed, session,
                     std::move(callback)));
}

void CoverageTool::OnCssTrackingStopped(
    bool detailed,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  css_tracking_ = false;

  // CSS.disable 호출 (정리, 결과와 무관하게 실행)
  session->SendCdpCommand(
      "CSS.disable", base::DictValue(),
      base::BindOnce(&CoverageTool::OnCssDisabled,
                     weak_factory_.GetWeakPtr()));

  if (!response.is_dict()) {
    base::DictValue result;
    result.Set("error", "예상치 못한 CDP 응답 형식");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* err = dict.FindDict("error");
  if (err) {
    const std::string* msg = err->FindString("message");
    base::DictValue result;
    result.Set("error", msg ? *msg : "CSS.stopRuleUsageTracking 실패");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // result.ruleUsage 추출
  const base::DictValue* res = dict.FindDict("result");
  if (!res) {
    base::DictValue result;
    result.Set("error", "CSS.stopRuleUsageTracking 응답에서 result를 찾을 수 없음");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::ListValue* rule_usage = res->FindList("ruleUsage");
  if (!rule_usage) {
    // ruleUsage가 없으면 빈 결과 반환
    base::DictValue result;
    result.Set("success", true);
    result.Set("summary", AggregateCssUsage(base::ListValue(), detailed));
    std::move(callback).Run(MakeJsonResult(std::move(result)));
    return;
  }

  LOG(INFO) << "[CoverageTool] CSS 커버리지 수집 완료: "
            << rule_usage->size() << "개 규칙";

  base::DictValue aggregate = AggregateCssUsage(*rule_usage, detailed);

  base::DictValue result;
  result.Set("success", true);
  result.Set("summary", std::move(aggregate));
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

void CoverageTool::OnCssDisabled(base::Value response) {
  // CSS.disable 응답은 정리 목적으로만 사용. 결과는 이미 반환됨.
  if (response.is_dict()) {
    const base::DictValue* err = response.GetDict().FindDict("error");
    if (err) {
      LOG(WARNING) << "[CoverageTool] CSS.disable 경고 (무시)";
    }
  }
}

// -----------------------------------------------------------------------
// JS 커버리지 — startJS
// -----------------------------------------------------------------------

void CoverageTool::ExecuteStartJS(
    bool detailed,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (js_tracking_) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "JS 커버리지가 이미 진행 중입니다. stopJS를 먼저 호출하세요.");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[CoverageTool] Profiler.enable 호출";

  // 1단계: Profiler.enable → Profiler 도메인 활성화
  session->SendCdpCommand(
      "Profiler.enable", base::DictValue(),
      base::BindOnce(&CoverageTool::OnProfilerEnabled,
                     weak_factory_.GetWeakPtr(), detailed, session,
                     std::move(callback)));
}

void CoverageTool::OnProfilerEnabled(
    bool detailed,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (response.is_dict()) {
    const base::DictValue* err = response.GetDict().FindDict("error");
    if (err) {
      const std::string* msg = err->FindString("message");
      base::DictValue result;
      result.Set("success", false);
      result.Set("error", msg ? *msg : "Profiler.enable 실패");
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }
  }

  LOG(INFO) << "[CoverageTool] Profiler.startPreciseCoverage 호출"
            << " (callCount=true, detailed=" << detailed << ")";

  // 2단계: Profiler.startPreciseCoverage
  //   callCount=true: 함수 호출 횟수 추적
  //   detailed=true: 블록(if/else/for 등) 단위 세부 범위 포함
  base::DictValue params;
  params.Set("callCount", true);
  params.Set("detailed", detailed);

  session->SendCdpCommand(
      "Profiler.startPreciseCoverage", std::move(params),
      base::BindOnce(&CoverageTool::OnJsCoverageStarted,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void CoverageTool::OnJsCoverageStarted(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (response.is_dict()) {
    const base::DictValue* err = response.GetDict().FindDict("error");
    if (err) {
      const std::string* msg = err->FindString("message");
      base::DictValue result;
      result.Set("success", false);
      result.Set("error", msg ? *msg : "Profiler.startPreciseCoverage 실패");
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }
  }

  js_tracking_ = true;
  LOG(INFO) << "[CoverageTool] JS 정밀 커버리지 측정 시작됨";

  base::DictValue result;
  result.Set("success", true);
  result.Set("message",
             "JS 커버리지 측정을 시작했습니다. "
             "stopJS 액션으로 결과를 수집하세요.");
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

// -----------------------------------------------------------------------
// JS 커버리지 — stopJS
// -----------------------------------------------------------------------

void CoverageTool::ExecuteStopJS(
    bool detailed,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  if (!js_tracking_) {
    base::DictValue result;
    result.Set("success", false);
    result.Set("error",
               "JS 커버리지가 시작되지 않았습니다. startJS를 먼저 호출하세요.");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[CoverageTool] Profiler.takePreciseCoverage 호출";

  // Profiler.takePreciseCoverage: 지금까지 측정된 커버리지 데이터를 스냅샷으로 반환한다.
  // 반환 형식:
  //   result: {
  //     result: ScriptCoverage[] — [{scriptId, url, functions[{...}]}]
  //     timestamp: number
  //   }
  session->SendCdpCommand(
      "Profiler.takePreciseCoverage", base::DictValue(),
      base::BindOnce(&CoverageTool::OnJsCoverageTaken,
                     weak_factory_.GetWeakPtr(), detailed, session,
                     std::move(callback)));
}

void CoverageTool::OnJsCoverageTaken(
    bool detailed,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  js_tracking_ = false;

  // 정리: Profiler.stopPreciseCoverage → Profiler.disable
  session->SendCdpCommand(
      "Profiler.stopPreciseCoverage", base::DictValue(),
      base::BindOnce(&CoverageTool::OnJsCoverageStopped,
                     weak_factory_.GetWeakPtr()));

  if (!response.is_dict()) {
    base::DictValue result;
    result.Set("error", "예상치 못한 CDP 응답 형식");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::DictValue& dict = response.GetDict();
  const base::DictValue* err = dict.FindDict("error");
  if (err) {
    const std::string* msg = err->FindString("message");
    base::DictValue result;
    result.Set("error", msg ? *msg : "Profiler.takePreciseCoverage 실패");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // result.result (ScriptCoverage 배열) 추출
  const base::DictValue* outer_result = dict.FindDict("result");
  if (!outer_result) {
    base::DictValue result;
    result.Set("error", "takePreciseCoverage 응답에서 result를 찾을 수 없음");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  const base::ListValue* script_coverage = outer_result->FindList("result");
  if (!script_coverage) {
    base::DictValue result;
    result.Set("success", true);
    result.Set("summary", AggregateJsUsage(base::ListValue(), detailed));
    std::move(callback).Run(MakeJsonResult(std::move(result)));
    return;
  }

  LOG(INFO) << "[CoverageTool] JS 커버리지 수집 완료: "
            << script_coverage->size() << "개 스크립트";

  base::DictValue aggregate = AggregateJsUsage(*script_coverage, detailed);

  base::DictValue result;
  result.Set("success", true);
  result.Set("summary", std::move(aggregate));
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

void CoverageTool::OnJsCoverageStopped(base::Value response) {
  // Profiler.stopPreciseCoverage 완료 후 Profiler.disable 호출
  // 이 응답은 정리 목적으로만 사용됨
}

void CoverageTool::OnProfilerDisabled(base::Value response) {
  if (response.is_dict()) {
    const base::DictValue* err = response.GetDict().FindDict("error");
    if (err) {
      LOG(WARNING) << "[CoverageTool] Profiler.disable 경고 (무시)";
    }
  }
}

// -----------------------------------------------------------------------
// 정적 헬퍼 — CSS 커버리지 집계
// -----------------------------------------------------------------------

// static
base::DictValue CoverageTool::AggregateCssUsage(
    const base::ListValue& rule_usage,
    bool detailed) {
  // ruleUsage 항목: {styleSheetId, startOffset, endOffset, used}
  int total_rules = 0;
  int used_rules   = 0;
  int unused_rules = 0;

  // styleSheet별 사용/미사용 규칙 수 집계
  // key: styleSheetId, value: {used, unused}
  std::map<std::string, std::pair<int, int>> sheet_stats;

  for (const auto& item : rule_usage) {
    if (!item.is_dict()) continue;
    const base::DictValue& rule = item.GetDict();

    std::optional<bool> used_opt = rule.FindBool("used");
    bool used = used_opt.value_or(false);
    const std::string* sheet_id = rule.FindString("styleSheetId");

    ++total_rules;
    if (used) {
      ++used_rules;
      if (sheet_id) sheet_stats[*sheet_id].first++;
    } else {
      ++unused_rules;
      if (sheet_id) sheet_stats[*sheet_id].second++;
    }
  }

  double usage_percent = total_rules > 0
      ? (static_cast<double>(used_rules) / total_rules) * 100.0
      : 0.0;

  base::DictValue summary;
  summary.Set("totalRules",   total_rules);
  summary.Set("usedRules",    used_rules);
  summary.Set("unusedRules",  unused_rules);
  summary.Set("usagePercent", std::round(usage_percent * 100.0) / 100.0);

  // 미사용 규칙이 있는 스타일시트 목록
  base::ListValue unused_sheets;
  for (const auto& [sheet_id, counts] : sheet_stats) {
    if (counts.second > 0) {  // unused > 0
      base::DictValue sheet_info;
      sheet_info.Set("styleSheetId", sheet_id);
      sheet_info.Set("usedRules",   counts.first);
      sheet_info.Set("unusedRules", counts.second);
      unused_sheets.Append(std::move(sheet_info));
    }
  }
  summary.Set("sheetsWithUnusedRules", std::move(unused_sheets));

  // detailed=true이면 규칙별 상세 목록도 포함
  if (detailed) {
    summary.Set("ruleUsage", rule_usage.Clone());
  }

  return summary;
}

// -----------------------------------------------------------------------
// 정적 헬퍼 — JS 커버리지 집계
// -----------------------------------------------------------------------

// static
base::DictValue CoverageTool::AggregateJsUsage(
    const base::ListValue& script_coverage,
    bool detailed) {
  // ScriptCoverage 항목:
  //   scriptId: 스크립트 식별자
  //   url: 스크립트 URL
  //   functions: FunctionCoverage[]
  //     - functionName: 함수 이름
  //     - ranges: CoverageRange[]
  //         - startOffset, endOffset: 범위 (bytes)
  //         - count: 실행 횟수
  //     - isBlockCoverage: 블록 단위 커버리지 여부

  int64_t total_bytes = 0;
  int64_t used_bytes  = 0;

  base::ListValue script_summaries;
  base::ListValue unused_urls;

  for (const auto& script_item : script_coverage) {
    if (!script_item.is_dict()) continue;
    const base::DictValue& script = script_item.GetDict();

    const std::string* url_ptr = script.FindString("url");
    std::string url = url_ptr ? *url_ptr : "(anonymous)";

    // 내부 스크립트(빈 URL) 및 확장 스크립트 제외
    if (url.empty() || url.find("extensions://") != std::string::npos) continue;

    const base::ListValue* functions = script.FindList("functions");
    if (!functions) continue;

    int64_t script_total = 0;
    int64_t script_used  = 0;
    int     total_funcs  = 0;
    int     called_funcs = 0;

    for (const auto& func_item : *functions) {
      if (!func_item.is_dict()) continue;
      const base::DictValue& func = func_item.GetDict();

      ++total_funcs;

      const base::ListValue* ranges = func.FindList("ranges");
      if (!ranges) continue;

      bool func_called = false;
      for (const auto& range_item : *ranges) {
        if (!range_item.is_dict()) continue;
        const base::DictValue& range = range_item.GetDict();

        std::optional<int> start = range.FindInt("startOffset");
        std::optional<int> end   = range.FindInt("endOffset");
        std::optional<int> count = range.FindInt("count");

        if (!start.has_value() || !end.has_value()) continue;

        int64_t range_bytes = static_cast<int64_t>(*end - *start);
        if (range_bytes < 0) range_bytes = 0;

        script_total += range_bytes;

        // count > 0이면 해당 범위가 실행됨
        if (count.value_or(0) > 0) {
          script_used += range_bytes;
          func_called = true;
        }
      }

      if (func_called) ++called_funcs;
    }

    total_bytes += script_total;
    used_bytes  += script_used;

    double script_pct = script_total > 0
        ? (static_cast<double>(script_used) / script_total) * 100.0
        : 0.0;

    base::DictValue script_summary;
    script_summary.Set("url",            url);
    script_summary.Set("totalBytes",     static_cast<int>(script_total));
    script_summary.Set("usedBytes",      static_cast<int>(script_used));
    script_summary.Set("unusedBytes",    static_cast<int>(script_total - script_used));
    script_summary.Set("usagePercent",   std::round(script_pct * 100.0) / 100.0);
    script_summary.Set("totalFunctions", total_funcs);
    script_summary.Set("calledFunctions", called_funcs);

    if (detailed) {
      script_summary.Set("functions", functions->Clone());
    }

    if (script_used < script_total) {
      unused_urls.Append(url);
    }

    script_summaries.Append(std::move(script_summary));
  }

  int64_t unused_bytes = total_bytes - used_bytes;
  double overall_pct = total_bytes > 0
      ? (static_cast<double>(used_bytes) / total_bytes) * 100.0
      : 0.0;

  base::DictValue summary;
  summary.Set("totalBytes",    static_cast<int>(total_bytes));
  summary.Set("usedBytes",     static_cast<int>(used_bytes));
  summary.Set("unusedBytes",   static_cast<int>(unused_bytes));
  summary.Set("usagePercent",  std::round(overall_pct * 100.0) / 100.0);
  summary.Set("scripts",       std::move(script_summaries));
  summary.Set("unusedUrls",    std::move(unused_urls));

  return summary;
}

}  // namespace mcp
