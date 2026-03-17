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

WaitTool::PollContext::PollContext() = default;
WaitTool::PollContext::~PollContext() = default;

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
         "selector: 요소 출현 대기, "
         "navigation: 페이지 로드 완료 대기, "
         "networkIdle: 네트워크 요청 종료 대기.";
}

base::DictValue WaitTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // type: 대기 유형
  base::DictValue type_prop;
  type_prop.Set("type", "string");
  base::ListValue type_enum;
  type_enum.Append("time");
  type_enum.Append("text");
  type_enum.Append("textGone");
  type_enum.Append("selector");
  type_enum.Append("navigation");
  type_enum.Append("networkIdle");
  type_prop.Set("enum", std::move(type_enum));
  type_prop.Set("description",
                "대기 유형: "
                "time=지정 시간 대기, "
                "text=텍스트 출현 대기, "
                "textGone=텍스트 소멸 대기, "
                "selector=요소 출현 대기, "
                "navigation=페이지 로드 완료 대기, "
                "networkIdle=네트워크 요청 종료 대기.");
  properties.Set("type", std::move(type_prop));

  // timeout: 최대 대기 시간 (ms)
  base::DictValue timeout_prop;
  timeout_prop.Set("type", "number");
  timeout_prop.Set("default", kDefaultTimeoutMs);
  timeout_prop.Set("description",
                   "최대 대기 시간 (밀리초). 초과 시 오류 반환. 기본값: 10000 (10초).");
  properties.Set("timeout", std::move(timeout_prop));

  // duration: time 모드에서 대기 시간 (ms)
  base::DictValue duration_prop;
  duration_prop.Set("type", "number");
  duration_prop.Set("description",
                    "type=time 일 때 대기할 시간 (밀리초). "
                    "생략 시 timeout 값을 사용.");
  properties.Set("duration", std::move(duration_prop));

  // text: 대기할 텍스트 (text/textGone 모드)
  base::DictValue text_prop;
  text_prop.Set("type", "string");
  text_prop.Set("description",
                "type=text 또는 textGone 일 때 감시할 텍스트. "
                "document.body.innerText에서 검색.");
  properties.Set("text", std::move(text_prop));

  // selector: CSS 선택자 (selector 모드)
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description",
                    "type=selector 일 때 대기할 요소의 CSS 선택자. "
                    "document.querySelector로 존재 여부 확인.");
  properties.Set("selector", std::move(selector_prop));

  // pollingInterval: 폴링 간격 (ms)
  base::DictValue interval_prop;
  interval_prop.Set("type", "number");
  interval_prop.Set("default", kDefaultPollingInterval);
  interval_prop.Set("description",
                    "조건 확인 폴링 간격 (밀리초). "
                    "text/textGone/selector 모드에서 사용. 기본값: 200.");
  properties.Set("pollingInterval", std::move(interval_prop));

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
    // duration 파라미터 우선, 없으면 timeout 값 사용
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
    const std::string* selector = arguments.FindString("selector");
    if (!selector || selector->empty()) {
      base::DictValue err;
      err.Set("error", "type=selector 일 때 selector 파라미터가 필요합니다");
      std::move(callback).Run(base::Value(std::move(err)));
      return;
    }
    WaitForSelector(*selector, timeout_ms, interval_ms, session,
                    std::move(callback));

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

  time_wait_timer_.Start(
      FROM_HERE, base::Milliseconds(duration_ms),
      base::BindOnce(
          [](base::OnceCallback<void(base::Value)> cb, int dur_ms) {
            LOG(INFO) << "[WaitTool] 시간 대기 완료: " << dur_ms << "ms";
            base::DictValue result;
            result.Set("success", true);
            result.Set("message",
                       std::to_string(dur_ms) + "ms 대기가 완료되었습니다");
            result.Set("durationMs", dur_ms);
            std::move(cb).Run(base::Value(std::move(result)));
          },
          std::move(callback), duration_ms));
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

  // JS 표현식 생성:
  //   text: body에 텍스트가 있으면 true
  //   textGone: body에 텍스트가 없으면 true
  std::string includes_expr =
      "!!(document.body && document.body.innerText.includes('" + escaped_text + "'))";
  std::string js_expr = wait_gone
      ? "!(" + includes_expr + ")"   // 사라졌으면 true
      : includes_expr;               // 나타났으면 true

  std::string label = std::string(mode) + ": \"" + text + "\"";
  WaitForCondition(js_expr, label, timeout_ms, interval_ms, session,
                   std::move(callback));
}

// ============================================================
// type=selector: CSS 선택자 요소 출현 폴링 대기
// ============================================================

void WaitTool::WaitForSelector(const std::string& selector,
                                int timeout_ms,
                                int interval_ms,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  LOG(INFO) << "[WaitTool] selector 대기 시작: " << selector
            << " timeout=" << timeout_ms << "ms";

  // 작은따옴표 이스케이프 처리
  std::string escaped_selector = selector;
  base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "\\", "\\\\");
  base::ReplaceSubstringsAfterOffset(&escaped_selector, 0, "'", "\\'");

  std::string js_expr =
      "!!document.querySelector('" + escaped_selector + "')";
  std::string label = "selector: " + selector;

  WaitForCondition(js_expr, label, timeout_ms, interval_ms, session,
                   std::move(callback));
}

// ============================================================
// 내부 공통: JS 조건 폴링 구현
// ============================================================

void WaitTool::WaitForCondition(const std::string& js_expression,
                                const std::string& condition_label,
                                int timeout_ms,
                                int interval_ms,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  auto ctx = std::make_shared<PollContext>();
  ctx->condition_label = condition_label;
  ctx->js_expression = js_expression;
  ctx->timeout_ms = timeout_ms;
  ctx->interval_ms = interval_ms;
  ctx->session = session;
  ctx->callback = std::move(callback);

  // timeout 타이머 시작
  timeout_timer_.Start(
      FROM_HERE, base::Milliseconds(timeout_ms),
      base::BindOnce(&WaitTool::OnTimeout, weak_factory_.GetWeakPtr(), ctx));

  // 폴링 타이머 시작
  poll_timer_.Start(
      FROM_HERE, base::Milliseconds(interval_ms),
      base::BindRepeating(&WaitTool::OnPollTimer,
                          weak_factory_.GetWeakPtr(), ctx));
}

void WaitTool::OnPollTimer(std::shared_ptr<PollContext> ctx) {
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

void WaitTool::OnEvaluateResponse(std::shared_ptr<PollContext> ctx,
                                  base::Value response) {
  if (ctx->completed) return;

  // 응답에서 평가 결과 추출
  bool condition_met = false;

  if (response.is_dict()) {
    const base::DictValue& dict = response.GetDict();
    // CDP 오류가 없는지 확인
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
    poll_timer_.Stop();
    timeout_timer_.Stop();

    LOG(INFO) << "[WaitTool] 조건 충족 (" << ctx->condition_label
              << "), 경과=" << ctx->elapsed_ms << "ms";

    base::DictValue result;
    result.Set("success", true);
    result.Set("elapsedMs", ctx->elapsed_ms);
    result.Set("condition", ctx->condition_label);
    std::move(ctx->callback).Run(base::Value(std::move(result)));
  }
  // 조건 미충족: poll_timer_ 계속 실행
}

void WaitTool::OnTimeout(std::shared_ptr<PollContext> ctx) {
  if (ctx->completed) return;

  ctx->completed = true;
  poll_timer_.Stop();

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

  // Page.loadEventFired 이벤트 핸들러 등록
  session->RegisterCdpEventHandler(
      kLoadEventFiredEvent,
      base::BindRepeating(
          [](base::WeakPtr<WaitTool> tool,
             std::shared_ptr<bool> done,
             std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
             McpSession* sess,
             const std::string& /*event_name*/,
             const base::DictValue& /*params*/) {
            if (*done) return;
            *done = true;

            sess->UnregisterCdpEventHandler(kLoadEventFiredEvent);
            if (tool) {
              tool->timeout_timer_.Stop();
            }

            LOG(INFO) << "[WaitTool] 페이지 로드 완료";
            base::DictValue result;
            result.Set("success", true);
            result.Set("message", "페이지 로드가 완료되었습니다");
            std::move(*cb_ptr).Run(base::Value(std::move(result)));
          },
          weak_factory_.GetWeakPtr(), completed, cb, session));

  // timeout 처리
  timeout_timer_.Start(
      FROM_HERE, base::Milliseconds(timeout_ms),
      base::BindOnce(
          [](base::WeakPtr<WaitTool> /*tool*/,
             std::shared_ptr<bool> done,
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
          weak_factory_.GetWeakPtr(), completed, cb, session));
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

  // 진행 중인 요청 수를 공유 상태로 관리
  auto pending_requests = std::make_shared<int>(0);
  auto completed = std::make_shared<bool>(false);
  auto cb = std::make_shared<base::OnceCallback<void(base::Value)>>(
      std::move(callback));

  // idle 상태 진입 시 완료 처리 람다
  auto complete_fn = [](base::WeakPtr<WaitTool> tool,
                         std::shared_ptr<bool> done,
                         std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
                         McpSession* sess,
                         bool timed_out) {
    if (*done) return;
    *done = true;

    sess->UnregisterCdpEventHandler(kRequestWillBeSentEvent);
    sess->UnregisterCdpEventHandler(kLoadingFinishedEvent);
    sess->UnregisterCdpEventHandler(kLoadingFailedEvent);

    if (tool) {
      tool->timeout_timer_.Stop();
      tool->network_idle_timer_.Stop();
    }

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

  // idle 타이머 재시작 함수: 요청이 0개일 때 idle_time_ms 후 완료
  auto restart_idle_timer = [this, idle_time_ms, pending_requests, completed,
                              cb, session,
                              complete_fn = complete_fn]() mutable {
    if (*completed) return;
    if (*pending_requests > 0) {
      // 아직 진행 중인 요청 있음 → idle 타이머 중지
      network_idle_timer_.Stop();
      return;
    }
    // 요청이 0개 → idle 타이머 시작
    network_idle_timer_.Start(
        FROM_HERE, base::Milliseconds(idle_time_ms),
        base::BindOnce(complete_fn, weak_factory_.GetWeakPtr(), completed, cb,
                       session, /*timed_out=*/false));
  };

  // Network.requestWillBeSent: 새 요청 시작
  session->RegisterCdpEventHandler(
      kRequestWillBeSentEvent,
      base::BindRepeating(
          [](base::WeakPtr<WaitTool> tool,
             std::shared_ptr<int> pending,
             std::shared_ptr<bool> done,
             const std::string& /*event_name*/,
             const base::DictValue& /*params*/) {
            if (*done) return;
            (*pending)++;
            // 요청 중이므로 idle 타이머 중지
            if (tool) {
              tool->network_idle_timer_.Stop();
            }
            LOG(INFO) << "[WaitTool] 네트워크 요청 시작, 진행 중=" << *pending;
          },
          weak_factory_.GetWeakPtr(), pending_requests, completed));

  // Network.loadingFinished: 요청 완료
  session->RegisterCdpEventHandler(
      kLoadingFinishedEvent,
      base::BindRepeating(
          [](base::WeakPtr<WaitTool> tool,
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
            if (*pending == 0 && tool) {
              // 요청이 0개 → idle 타이머 시작
              tool->network_idle_timer_.Start(
                  FROM_HERE, base::Milliseconds(idle_ms),
                  base::BindOnce(
                      [](base::WeakPtr<WaitTool> t,
                         std::shared_ptr<bool> d,
                         std::shared_ptr<base::OnceCallback<void(base::Value)>> c,
                         McpSession* s) {
                        if (*d) return;
                        *d = true;
                        s->UnregisterCdpEventHandler(kRequestWillBeSentEvent);
                        s->UnregisterCdpEventHandler(kLoadingFinishedEvent);
                        s->UnregisterCdpEventHandler(kLoadingFailedEvent);
                        if (t) {
                          t->timeout_timer_.Stop();
                          t->network_idle_timer_.Stop();
                        }
                        LOG(INFO) << "[WaitTool] networkIdle 완료";
                        base::DictValue result;
                        result.Set("success", true);
                        result.Set("message", "네트워크가 idle 상태가 되었습니다");
                        std::move(*c).Run(base::Value(std::move(result)));
                      },
                      tool, done, cb_ptr, sess));
            }
          },
          weak_factory_.GetWeakPtr(), pending_requests, completed,
          idle_time_ms, cb, session));

  // Network.loadingFailed: 요청 실패도 완료로 처리
  session->RegisterCdpEventHandler(
      kLoadingFailedEvent,
      base::BindRepeating(
          [](std::shared_ptr<int> pending,
             const std::string& /*event_name*/,
             const base::DictValue& /*params*/) {
            if (*pending > 0) (*pending)--;
          },
          pending_requests));

  // 전체 timeout 처리
  timeout_timer_.Start(
      FROM_HERE, base::Milliseconds(timeout_ms),
      base::BindOnce(
          [](base::WeakPtr<WaitTool> tool,
             std::shared_ptr<bool> done,
             std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
             McpSession* sess) {
            if (*done) return;
            *done = true;

            sess->UnregisterCdpEventHandler(kRequestWillBeSentEvent);
            sess->UnregisterCdpEventHandler(kLoadingFinishedEvent);
            sess->UnregisterCdpEventHandler(kLoadingFailedEvent);

            if (tool) {
              tool->network_idle_timer_.Stop();
            }

            LOG(WARNING) << "[WaitTool] networkIdle timeout";
            base::DictValue result;
            result.Set("success", false);
            result.Set("error", "네트워크 idle 대기 시간이 초과되었습니다");
            std::move(*cb_ptr).Run(base::Value(std::move(result)));
          },
          weak_factory_.GetWeakPtr(), completed, cb, session));

  // 초기에 요청이 0개이면 idle 타이머 즉시 시작
  network_idle_timer_.Start(
      FROM_HERE, base::Milliseconds(idle_time_ms),
      base::BindOnce(
          [](base::WeakPtr<WaitTool> tool,
             std::shared_ptr<bool> done,
             std::shared_ptr<base::OnceCallback<void(base::Value)>> cb_ptr,
             McpSession* sess) {
            if (*done) return;
            *done = true;

            sess->UnregisterCdpEventHandler(kRequestWillBeSentEvent);
            sess->UnregisterCdpEventHandler(kLoadingFinishedEvent);
            sess->UnregisterCdpEventHandler(kLoadingFailedEvent);

            if (tool) {
              tool->timeout_timer_.Stop();
            }

            LOG(INFO) << "[WaitTool] networkIdle 완료 (초기 idle 상태)";
            base::DictValue result;
            result.Set("success", true);
            result.Set("message", "네트워크가 idle 상태가 되었습니다");
            std::move(*cb_ptr).Run(base::Value(std::move(result)));
          },
          weak_factory_.GetWeakPtr(), completed, cb, session));
}

}  // namespace mcp
