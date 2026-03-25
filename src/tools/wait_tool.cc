// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/wait_tool.h"

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/element_locator.h"

namespace mcp {

// 기본 대기 파라미터
constexpr int kDefaultTimeoutMs       = 10000;  // 10초
constexpr int kDefaultPollingInterval = 200;    // 0.2초
constexpr int kDefaultIdleTimeMs      = 500;    // networkIdle 0.5초 무활동 기준

// CDP 이벤트 이름
constexpr char kLoadEventFiredEvent[]       = "Page.loadEventFired";
constexpr char kRequestWillBeSentEvent[]    = "Network.requestWillBeSent";
constexpr char kLoadingFinishedEvent[]      = "Network.loadingFinished";
constexpr char kLoadingFailedEvent[]        = "Network.loadingFailed";

WaitTool::WaitContext::WaitContext() = default;
WaitTool::WaitContext::~WaitContext() = default;

WaitTool::WaitTool() = default;
WaitTool::~WaitTool() = default;

std::string WaitTool::name() const {
  return "wait";
}

std::string WaitTool::description() const {
  return "조건 충족까지 대기합니다. "
         "time: 지정 시간 대기, "
         "text: 텍스트 출현 대기, "
         "textGone: 텍스트 소멸 대기, "
         "selector: 요소 출현/가시 대기 (role/name/text/selector/xpath 지원), "
         "navigation: 페이지 로드 완료 대기, "
         "networkIdle: 네트워크 요청 종료 대기.";
}

base::DictValue WaitTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // type: 대기 유형
  {
    base::DictValue prop;
    prop.Set("type", "string");
    base::ListValue type_enum;
    type_enum.Append("time");
    type_enum.Append("text");
    type_enum.Append("textGone");
    type_enum.Append("selector");
    type_enum.Append("navigation");
    type_enum.Append("networkIdle");
    prop.Set("enum", std::move(type_enum));
    prop.Set("description",
             "대기 유형: "
             "time=지정 시간 대기, "
             "text=텍스트 출현 대기, "
             "textGone=텍스트 소멸 대기, "
             "selector=요소 출현/가시 대기, "
             "navigation=페이지 로드 완료 대기, "
             "networkIdle=네트워크 요청 종료 대기.");
    properties.Set("type", std::move(prop));
  }

  // timeout: 최대 대기 시간 (ms)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("default", kDefaultTimeoutMs);
    prop.Set("description",
             "최대 대기 시간 (밀리초). 초과 시 오류 반환. 기본값: 10000 (10초).");
    properties.Set("timeout", std::move(prop));
  }

  // duration: time 모드에서 대기 시간 (ms)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description",
             "type=time 일 때 대기할 시간 (밀리초). "
             "생략 시 timeout 값을 사용.");
    properties.Set("duration", std::move(prop));
  }

  // text: 대기할 텍스트 (text/textGone 모드 또는 selector 모드 로케이터)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "type=text/textGone 일 때 감시할 텍스트 (document.body.innerText 검색). "
             "type=selector 일 때 접근성 텍스트 로케이터로 사용.");
    properties.Set("text", std::move(prop));
  }

  // selector: CSS 선택자 (selector 모드 로케이터)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "type=selector 일 때 대기할 요소의 CSS 선택자.");
    properties.Set("selector", std::move(prop));
  }

  // role: 접근성 역할 (selector 모드 로케이터)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "type=selector 일 때 AX 역할 로케이터. "
             "예: button, link, textbox");
    properties.Set("role", std::move(prop));
  }

  // name: 접근성 이름 (selector 모드, role과 함께 사용)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "type=selector 일 때 AX 접근성 이름 로케이터. "
             "role과 함께 사용하거나 단독으로 사용 가능.");
    properties.Set("name", std::move(prop));
  }

  // xpath: XPath 로케이터 (selector 모드)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "type=selector 일 때 XPath 로케이터.");
    properties.Set("xpath", std::move(prop));
  }

  // visible: 가시성 확인 여부 (selector 모드, 기본 true)
  {
    base::DictValue prop;
    prop.Set("type", "boolean");
    prop.Set("default", true);
    prop.Set("description",
             "type=selector 일 때 요소가 실제로 보이는 상태(가시성)까지 확인할지 여부. "
             "true(기본값)=DOM 존재 + 가시성 확인, false=DOM 존재만 확인.");
    properties.Set("visible", std::move(prop));
  }

  // pollingInterval: 폴링 간격 (ms)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("default", kDefaultPollingInterval);
    prop.Set("description",
             "조건 확인 폴링 간격 (밀리초). "
             "text/textGone/selector 모드에서 사용. 기본값: 200.");
    properties.Set("pollingInterval", std::move(prop));
  }

  schema.Set("properties", std::move(properties));

  // type은 필수
  base::ListValue required;
  required.Append("type");
  schema.Set("required", std::move(required));

  return schema;
}

void WaitTool::Execute(const base::DictValue& arguments,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback) {
  // type 파라미터 추출 (필수)
  const std::string* type_ptr = arguments.FindString("type");
  if (!type_ptr || type_ptr->empty()) {
    base::DictValue err;
    err.Set("error",
            "type 파라미터가 필요합니다 "
            "(time/text/textGone/selector/navigation/networkIdle)");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }
  const std::string& wait_type = *type_ptr;

  // timeout 파라미터 추출 (기본값: kDefaultTimeoutMs)
  std::optional<double> timeout_dbl = arguments.FindDouble("timeout");
  int timeout_ms = timeout_dbl.has_value()
                       ? static_cast<int>(*timeout_dbl)
                       : kDefaultTimeoutMs;
  if (timeout_ms <= 0) timeout_ms = kDefaultTimeoutMs;

  // pollingInterval 파라미터 추출 (기본값: kDefaultPollingInterval)
  std::optional<double> interval_dbl = arguments.FindDouble("pollingInterval");
  int interval_ms = interval_dbl.has_value()
                        ? static_cast<int>(*interval_dbl)
                        : kDefaultPollingInterval;
  if (interval_ms <= 0) interval_ms = kDefaultPollingInterval;

  if (wait_type == "time") {
    std::optional<double> duration_dbl = arguments.FindDouble("duration");
    int duration_ms = duration_dbl.has_value()
                          ? static_cast<int>(*duration_dbl)
                          : timeout_ms;
    if (duration_ms <= 0) duration_ms = kDefaultTimeoutMs;
    WaitForTime(duration_ms, std::move(callback));

  } else if (wait_type == "text") {
    const std::string* text = arguments.FindString("text");
    if (!text || text->empty()) {
      base::DictValue err;
      err.Set("error", "type=text 일 때 text 파라미터가 필요합니다");
      std::move(callback).Run(base::Value(std::move(err)));
      return;
    }
    WaitForText(*text, /*wait_gone=*/false, timeout_ms, interval_ms,
                session, std::move(callback));

  } else if (wait_type == "textGone") {
    const std::string* text = arguments.FindString("text");
    if (!text || text->empty()) {
      base::DictValue err;
      err.Set("error", "type=textGone 일 때 text 파라미터가 필요합니다");
      std::move(callback).Run(base::Value(std::move(err)));
      return;
    }
    WaitForText(*text, /*wait_gone=*/true, timeout_ms, interval_ms,
                session, std::move(callback));

  } else if (wait_type == "selector") {
    // 로케이터 파라미터를 Dict으로 수집
    // (role/name/text/selector/xpath 중 하나 이상 필요)
    base::DictValue locator_params;

    auto copy_str = [&](const char* key) {
      const std::string* val = arguments.FindString(key);
      if (val && !val->empty()) {
        locator_params.Set(key, *val);
      }
    };
    copy_str("role");
    copy_str("name");
    copy_str("text");
    copy_str("selector");
    copy_str("xpath");

    // exact 파라미터도 전달
    std::optional<bool> exact = arguments.FindBool("exact");
    if (exact.has_value()) {
      locator_params.Set("exact", *exact);
    }

    if (locator_params.empty()) {
      base::DictValue err;
      err.Set("error",
              "type=selector 일 때 role/name/text/selector/xpath 중 "
              "하나 이상의 로케이터 파라미터가 필요합니다");
      std::move(callback).Run(base::Value(std::move(err)));
      return;
    }

    // visible 파라미터 (기본값: true)
    bool require_visible = arguments.FindBool("visible").value_or(true);

    WaitForSelector(std::move(locator_params), require_visible,
                    timeout_ms, interval_ms, session, std::move(callback));

  } else if (wait_type == "navigation") {
    WaitForNavigation(timeout_ms, session, std::move(callback));

  } else if (wait_type == "networkIdle") {
    WaitForNetworkIdle(timeout_ms, kDefaultIdleTimeMs, session,
                       std::move(callback));

  } else {
    base::DictValue err;
    err.Set("error",
            "유효하지 않은 type: " + wait_type +
            ". time/text/textGone/selector/navigation/networkIdle 중 하나를 사용하세요");
    std::move(callback).Run(base::Value(std::move(err)));
  }
}

// ============================================================
// type=time: 단순 시간 대기
// ============================================================

void WaitTool::WaitForTime(int duration_ms,
                           base::OnceCallback<void(base::Value)> callback) {
  LOG(INFO) << "[WaitTool] 시간 대기: " << duration_ms << "ms";

  // per-request 타이머: 람다 내부에 OneShotTimer를 소유하여 독립 동작
  auto timer = std::make_shared<base::OneShotTimer>();
  auto cb_ptr = std::make_shared<base::OnceCallback<void(base::Value)>>(
      std::move(callback));

  timer->Start(
      FROM_HERE, base::Milliseconds(duration_ms),
      base::BindOnce(
          [](std::shared_ptr<base::OneShotTimer> /*timer*/,
             std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
             int dur_ms) {
            LOG(INFO) << "[WaitTool] 시간 대기 완료: " << dur_ms << "ms";
            base::DictValue result;
            result.Set("success", true);
            result.Set("message",
                       std::to_string(dur_ms) + "ms 대기가 완료되었습니다");
            result.Set("durationMs", dur_ms);
            std::move(*cb_ptr).Run(base::Value(std::move(result)));
          },
          timer, cb_ptr, duration_ms));
}

// ============================================================
// type=text / textGone: 텍스트 출현/소멸 폴링 대기
// ============================================================

void WaitTool::WaitForText(const std::string& text,
                           bool wait_gone,
                           int timeout_ms,
                           int interval_ms,
                           McpSession* session,
                           base::OnceCallback<void(base::Value)> callback) {
  const char* mode = wait_gone ? "textGone" : "text";
  LOG(INFO) << "[WaitTool] " << mode << " 대기 시작: \"" << text
            << "\" timeout=" << timeout_ms << "ms";

  // 작은따옴표 이스케이프 처리
  std::string escaped_text = text;
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "\\", "\\\\");
  base::ReplaceSubstringsAfterOffset(&escaped_text, 0, "'", "\\'");

  std::string includes_expr =
      "!!(document.body && document.body.innerText.includes('" +
      escaped_text + "'))";
  std::string js_expr = wait_gone
      ? "!(" + includes_expr + ")"
      : includes_expr;

  std::string label = std::string(mode) + ": \"" + text + "\"";
  WaitForCondition(js_expr, label, timeout_ms, interval_ms, session,
                   std::move(callback));
}

// ============================================================
// type=selector: ElementLocator + DOM.getBoxModel 가시성 확인
// ============================================================

void WaitTool::WaitForSelector(base::DictValue locator_params,
                                bool require_visible,
                                int timeout_ms,
                                int interval_ms,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  // 레이블 생성
  std::string label = "selector";
  if (const std::string* s = locator_params.FindString("selector")) {
    label = "selector: " + *s;
  } else if (const std::string* r = locator_params.FindString("role")) {
    label = "role: " + *r;
    if (const std::string* n = locator_params.FindString("name")) {
      label += " name: " + *n;
    }
  } else if (const std::string* t = locator_params.FindString("text")) {
    label = "text: " + *t;
  } else if (const std::string* x = locator_params.FindString("xpath")) {
    label = "xpath: " + *x;
  }

  LOG(INFO) << "[WaitTool] selector 대기 시작: " << label
            << " visible=" << require_visible
            << " timeout=" << timeout_ms << "ms";

  auto ctx = std::make_shared<WaitContext>();
  ctx->condition_label = label;
  ctx->use_locator = true;
  ctx->require_visible = require_visible;
  ctx->timeout_ms = timeout_ms;
  ctx->interval_ms = interval_ms;
  ctx->session = session;
  ctx->callback = std::move(callback);
  ctx->locator_params = std::move(locator_params);
  ctx->locator = std::make_unique<ElementLocator>();

  // timeout 타이머 시작 (per-request)
  ctx->timeout_timer.Start(
      FROM_HERE, base::Milliseconds(timeout_ms),
      base::BindOnce(&WaitTool::OnTimeout, weak_factory_.GetWeakPtr(), ctx));

  // 첫 폴링 즉시 실행 (interval 지연 없이)
  OnSelectorPollTimer(ctx);

  // 폴링 타이머 시작 (per-request): 이미 즉시 실행했으므로 interval 후부터 반복
  ctx->poll_timer.Start(
      FROM_HERE, base::Milliseconds(interval_ms),
      base::BindRepeating(&WaitTool::OnSelectorPollTimer,
                          weak_factory_.GetWeakPtr(), ctx));
}

void WaitTool::OnSelectorPollTimer(std::shared_ptr<WaitContext> ctx) {
  if (ctx->completed) return;

  ctx->elapsed_ms += ctx->interval_ms;

  // ElementLocator::Locate()를 호출하여 요소를 탐색
  // locator는 per-request이므로 재사용 가능
  ctx->locator->Locate(
      ctx->session, ctx->locator_params,
      base::BindOnce(&WaitTool::OnLocateResult,
                     weak_factory_.GetWeakPtr(), ctx));
}

void WaitTool::OnLocateResult(std::shared_ptr<WaitContext> ctx,
                               std::optional<ElementLocator::Result> result,
                               std::string error) {
  if (ctx->completed) return;

  bool condition_met = false;

  if (result.has_value()) {
    // ElementLocator 성공: 요소를 찾았음
    if (!ctx->require_visible) {
      // visible=false: DOM 존재만 확인
      condition_met = true;
    } else {
      // visible=true: 좌표가 유효한지로 가시성 판단
      // ElementLocator::Result에 x, y 좌표가 있으면 getBoxModel 성공 = 가시
      condition_met = (result->backend_node_id > 0);
    }
  }
  // error가 있으면 condition_met = false → 다음 폴링에서 재시도

  if (condition_met) {
    ctx->completed = true;
    ctx->poll_timer.Stop();
    ctx->timeout_timer.Stop();

    LOG(INFO) << "[WaitTool] 조건 충족 (" << ctx->condition_label
              << "), 경과=" << ctx->elapsed_ms << "ms";

    base::DictValue res;
    res.Set("success", true);
    res.Set("elapsedMs", ctx->elapsed_ms);
    res.Set("condition", ctx->condition_label);
    std::move(ctx->callback).Run(base::Value(std::move(res)));
  }
  // 미충족: poll_timer 계속 실행
}

// ============================================================
// 내부 공통: JS 조건 폴링 구현 (text/textGone)
// ============================================================

void WaitTool::WaitForCondition(const std::string& js_expression,
                                const std::string& condition_label,
                                int timeout_ms,
                                int interval_ms,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  auto ctx = std::make_shared<WaitContext>();
  ctx->condition_label = condition_label;
  ctx->js_expression = js_expression;
  ctx->use_locator = false;
  ctx->timeout_ms = timeout_ms;
  ctx->interval_ms = interval_ms;
  ctx->session = session;
  ctx->callback = std::move(callback);

  // per-request timeout 타이머
  ctx->timeout_timer.Start(
      FROM_HERE, base::Milliseconds(timeout_ms),
      base::BindOnce(&WaitTool::OnTimeout, weak_factory_.GetWeakPtr(), ctx));

  // 첫 폴링 즉시 실행 (interval 지연 없이)
  OnPollTimer(ctx);

  // per-request 폴링 타이머: 이미 즉시 실행했으므로 interval 후부터 반복
  ctx->poll_timer.Start(
      FROM_HERE, base::Milliseconds(interval_ms),
      base::BindRepeating(&WaitTool::OnPollTimer,
                          weak_factory_.GetWeakPtr(), ctx));
}

void WaitTool::OnPollTimer(std::shared_ptr<WaitContext> ctx) {
  if (ctx->completed) return;

  ctx->elapsed_ms += ctx->interval_ms;

  base::DictValue params;
  params.Set("expression", ctx->js_expression);
  params.Set("returnByValue", true);
  params.Set("awaitPromise", false);

  ctx->session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&WaitTool::OnEvaluateResponse,
                     weak_factory_.GetWeakPtr(), ctx));
}

void WaitTool::OnEvaluateResponse(std::shared_ptr<WaitContext> ctx,
                                  base::Value response) {
  if (ctx->completed) return;

  bool condition_met = false;

  if (response.is_dict()) {
    const base::DictValue& dict = response.GetDict();
    if (!dict.FindDict("error")) {
      const base::DictValue* result_obj = dict.FindDict("result");
      if (result_obj) {
        const base::DictValue* inner_result = result_obj->FindDict("result");
        const base::DictValue* eval_result =
            inner_result ? inner_result : result_obj;
        const base::Value* val = eval_result->Find("value");
        if (val) {
          if (val->is_bool()) {
            condition_met = val->GetBool();
          } else if (val->is_int()) {
            condition_met = (val->GetInt() != 0);
          } else if (val->is_string()) {
            condition_met = !val->GetString().empty();
          } else if (!val->is_none()) {
            condition_met = true;
          }
        }
      }
    }
  }

  if (condition_met) {
    ctx->completed = true;
    ctx->poll_timer.Stop();
    ctx->timeout_timer.Stop();

    LOG(INFO) << "[WaitTool] 조건 충족 (" << ctx->condition_label
              << "), 경과=" << ctx->elapsed_ms << "ms";

    base::DictValue result;
    result.Set("success", true);
    result.Set("elapsedMs", ctx->elapsed_ms);
    result.Set("condition", ctx->condition_label);
    std::move(ctx->callback).Run(base::Value(std::move(result)));
  }
}

void WaitTool::OnTimeout(std::shared_ptr<WaitContext> ctx) {
  if (ctx->completed) return;

  ctx->completed = true;
  ctx->poll_timer.Stop();

  LOG(WARNING) << "[WaitTool] 대기 timeout (" << ctx->condition_label << ")";

  base::DictValue result;
  result.Set("success", false);
  result.Set("error",
             "대기 시간이 초과되었습니다: " + ctx->condition_label +
             " (" + std::to_string(ctx->timeout_ms) + "ms)");
  result.Set("timeoutMs", ctx->timeout_ms);
  result.Set("condition", ctx->condition_label);
  std::move(ctx->callback).Run(base::Value(std::move(result)));
}

// ============================================================
// type=navigation: Page.loadEventFired 이벤트 대기
// ============================================================

void WaitTool::WaitForNavigation(int timeout_ms,
                                 McpSession* session,
                                 base::OnceCallback<void(base::Value)> callback) {
  LOG(INFO) << "[WaitTool] 네비게이션 대기 시작, timeout=" << timeout_ms << "ms";

  auto completed = std::make_shared<bool>(false);
  auto cb = std::make_shared<base::OnceCallback<void(base::Value)>>(
      std::move(callback));
  // per-request timeout 타이머
  auto timeout_timer = std::make_shared<base::OneShotTimer>();

  // Page.loadEventFired 이벤트 핸들러 등록
  session->RegisterCdpEventHandler(
      kLoadEventFiredEvent,
      base::BindRepeating(
          [](std::shared_ptr<base::OneShotTimer> timer,
             std::shared_ptr<bool> done,
             std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
             McpSession* sess,
             const std::string& /*event_name*/,
             const base::DictValue& /*params*/) {
            if (*done) return;
            *done = true;

            sess->UnregisterCdpEventHandler(kLoadEventFiredEvent);
            timer->Stop();

            LOG(INFO) << "[WaitTool] 페이지 로드 완료";
            base::DictValue result;
            result.Set("success", true);
            result.Set("message", "페이지 로드가 완료되었습니다");
            std::move(*cb_ptr).Run(base::Value(std::move(result)));
          },
          timeout_timer, completed, cb, session));

  // timeout 처리
  timeout_timer->Start(
      FROM_HERE, base::Milliseconds(timeout_ms),
      base::BindOnce(
          [](std::shared_ptr<bool> done,
             std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
             McpSession* sess) {
            if (*done) return;
            *done = true;

            sess->UnregisterCdpEventHandler(kLoadEventFiredEvent);

            LOG(WARNING) << "[WaitTool] 네비게이션 대기 timeout";
            base::DictValue result;
            result.Set("success", false);
            result.Set("error", "페이지 로드 대기 시간이 초과되었습니다");
            std::move(*cb_ptr).Run(base::Value(std::move(result)));
          },
          completed, cb, session));
}

// ============================================================
// type=networkIdle: 네트워크 요청이 없는 상태가 idleTime ms 유지될 때까지 대기
// ============================================================

void WaitTool::WaitForNetworkIdle(int timeout_ms,
                                  int idle_time_ms,
                                  McpSession* session,
                                  base::OnceCallback<void(base::Value)> callback) {
  LOG(INFO) << "[WaitTool] networkIdle 대기 시작, timeout=" << timeout_ms
            << "ms, idleTime=" << idle_time_ms << "ms";

  auto pending_requests = std::make_shared<int>(0);
  auto completed = std::make_shared<bool>(false);
  auto cb = std::make_shared<base::OnceCallback<void(base::Value)>>(
      std::move(callback));

  // per-request 타이머들
  auto timeout_timer = std::make_shared<base::OneShotTimer>();
  auto idle_timer = std::make_shared<base::OneShotTimer>();

  // 완료 공통 처리 람다
  auto complete_fn = [](std::shared_ptr<base::OneShotTimer> t_timer,
                         std::shared_ptr<base::OneShotTimer> i_timer,
                         std::shared_ptr<bool> done,
                         std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
                         McpSession* sess,
                         bool timed_out) {
    if (*done) return;
    *done = true;

    sess->UnregisterCdpEventHandler(kRequestWillBeSentEvent);
    sess->UnregisterCdpEventHandler(kLoadingFinishedEvent);
    sess->UnregisterCdpEventHandler(kLoadingFailedEvent);

    t_timer->Stop();
    i_timer->Stop();

    base::DictValue result;
    if (timed_out) {
      LOG(WARNING) << "[WaitTool] networkIdle 대기 timeout";
      result.Set("success", false);
      result.Set("error", "네트워크 idle 대기 시간이 초과되었습니다");
    } else {
      LOG(INFO) << "[WaitTool] networkIdle 완료";
      result.Set("success", true);
      result.Set("message", "네트워크가 idle 상태가 되었습니다");
    }
    std::move(*cb_ptr).Run(base::Value(std::move(result)));
  };

  // Network.requestWillBeSent: 새 요청 시작
  session->RegisterCdpEventHandler(
      kRequestWillBeSentEvent,
      base::BindRepeating(
          [](std::shared_ptr<base::OneShotTimer> i_timer,
             std::shared_ptr<int> pending,
             std::shared_ptr<bool> done,
             const std::string& /*event_name*/,
             const base::DictValue& /*params*/) {
            if (*done) return;
            (*pending)++;
            i_timer->Stop();
            LOG(INFO) << "[WaitTool] 네트워크 요청 시작, 진행 중=" << *pending;
          },
          idle_timer, pending_requests, completed));

  // Network.loadingFinished: 요청 완료
  session->RegisterCdpEventHandler(
      kLoadingFinishedEvent,
      base::BindRepeating(
          [](std::shared_ptr<base::OneShotTimer> t_timer,
             std::shared_ptr<base::OneShotTimer> i_timer,
             std::shared_ptr<int> pending,
             std::shared_ptr<bool> done,
             int idle_ms,
             std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
             McpSession* sess,
             const std::string& /*event_name*/,
             const base::DictValue& /*params*/) {
            if (*done) return;
            if (*pending > 0) (*pending)--;
            LOG(INFO) << "[WaitTool] 네트워크 요청 완료, 진행 중=" << *pending;
            if (*pending == 0) {
              i_timer->Start(
                  FROM_HERE, base::Milliseconds(idle_ms),
                  base::BindOnce(
                      [](std::shared_ptr<base::OneShotTimer> tt,
                         std::shared_ptr<base::OneShotTimer> it,
                         std::shared_ptr<bool> d,
                         std::shared_ptr<base::OnceCallback<void(base::Value)>> c,
                         McpSession* s) {
                        if (*d) return;
                        *d = true;
                        s->UnregisterCdpEventHandler(kRequestWillBeSentEvent);
                        s->UnregisterCdpEventHandler(kLoadingFinishedEvent);
                        s->UnregisterCdpEventHandler(kLoadingFailedEvent);
                        tt->Stop();
                        it->Stop();
                        LOG(INFO) << "[WaitTool] networkIdle 완료";
                        base::DictValue result;
                        result.Set("success", true);
                        result.Set("message", "네트워크가 idle 상태가 되었습니다");
                        std::move(*c).Run(base::Value(std::move(result)));
                      },
                      t_timer, i_timer, done, cb_ptr, sess));
            }
          },
          timeout_timer, idle_timer, pending_requests, completed,
          idle_time_ms, cb, session));

  // Network.loadingFailed: 요청 실패도 완료로 처리 (idle 타이머 포함)
  session->RegisterCdpEventHandler(
      kLoadingFailedEvent,
      base::BindRepeating(
          [](std::shared_ptr<base::OneShotTimer> t_timer,
             std::shared_ptr<base::OneShotTimer> i_timer,
             std::shared_ptr<int> pending,
             std::shared_ptr<bool> done,
             int idle_ms,
             std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
             McpSession* sess,
             const std::string& /*event_name*/,
             const base::DictValue& /*params*/) {
            if (*done) return;
            if (*pending > 0) (*pending)--;
            LOG(INFO) << "[WaitTool] 네트워크 요청 실패, 진행 중=" << *pending;
            if (*pending == 0) {
              i_timer->Start(
                  FROM_HERE, base::Milliseconds(idle_ms),
                  base::BindOnce(
                      [](std::shared_ptr<base::OneShotTimer> tt,
                         std::shared_ptr<base::OneShotTimer> it,
                         std::shared_ptr<bool> d,
                         std::shared_ptr<base::OnceCallback<void(base::Value)>> c,
                         McpSession* s) {
                        if (*d) return;
                        *d = true;
                        s->UnregisterCdpEventHandler(kRequestWillBeSentEvent);
                        s->UnregisterCdpEventHandler(kLoadingFinishedEvent);
                        s->UnregisterCdpEventHandler(kLoadingFailedEvent);
                        tt->Stop();
                        it->Stop();
                        LOG(INFO) << "[WaitTool] networkIdle 완료";
                        base::DictValue result;
                        result.Set("success", true);
                        result.Set("message", "네트워크가 idle 상태가 되었습니다");
                        std::move(*c).Run(base::Value(std::move(result)));
                      },
                      t_timer, i_timer, done, cb_ptr, sess));
            }
          },
          timeout_timer, idle_timer, pending_requests, completed,
          idle_time_ms, cb, session));

  // 전체 timeout 처리
  timeout_timer->Start(
      FROM_HERE, base::Milliseconds(timeout_ms),
      base::BindOnce(complete_fn, timeout_timer, idle_timer,
                     completed, cb, session, /*timed_out=*/true));

  // 초기에 요청이 0개이면 idle 타이머 즉시 시작
  idle_timer->Start(
      FROM_HERE, base::Milliseconds(idle_time_ms),
      base::BindOnce(complete_fn, timeout_timer, idle_timer,
                     completed, cb, session, /*timed_out=*/false));
}

}  // namespace mcp
