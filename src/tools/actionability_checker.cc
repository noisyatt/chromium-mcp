// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/actionability_checker.h"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

// ============================================================
// PollContext
// ============================================================

ActionabilityChecker::PollContext::PollContext() = default;
ActionabilityChecker::PollContext::~PollContext() = default;

// ============================================================
// ActionabilityChecker
// ============================================================

ActionabilityChecker::ActionabilityChecker() = default;
ActionabilityChecker::~ActionabilityChecker() = default;

// ============================================================
// 진입점
// ============================================================

void ActionabilityChecker::VerifyAndLocate(McpSession* session,
                                           const base::Value::Dict& params,
                                           ActionType action,
                                           Options options,
                                           Callback callback) {
  auto ctx = std::make_shared<PollContext>();
  ctx->session = session;
  ctx->action = action;
  ctx->params = params.Clone();
  ctx->options = options;
  ctx->callback = std::move(callback);

  // force=true → Locate만 수행하고 체크 건너뜀
  if (options.force) {
    LOG(INFO) << "[ActionabilityChecker] force=true, 체크 건너뜀";
    ctx->locator.Locate(
        session, params,
        base::BindOnce(
            [](Callback cb, std::optional<ElementLocator::Result> result,
               std::string error) {
              if (!result.has_value()) {
                ElementLocator::Result empty;
                std::move(cb).Run(empty, std::move(error));
                return;
              }
              std::move(cb).Run(*result, "");
            },
            std::move(ctx->callback)));
    return;
  }

  // timeout > 0 이면 타임아웃 타이머 시작
  if (options.timeout_ms > 0) {
    ctx->timeout_timer.Start(
        FROM_HERE, base::Milliseconds(options.timeout_ms),
        base::BindOnce(&ActionabilityChecker::OnTimeout, ctx));
  }

  StartPoll(ctx);
}

// ============================================================
// 폴링 루프
// ============================================================

void ActionabilityChecker::StartPoll(std::shared_ptr<PollContext> ctx) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  ctx->locator.Locate(
      ctx->session, ctx->params,
      base::BindOnce(&ActionabilityChecker::OnLocateResult, ctx));
}

void ActionabilityChecker::OnLocateResult(
    std::shared_ptr<PollContext> ctx,
    std::optional<ElementLocator::Result> result,
    std::string error) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  if (!result.has_value()) {
    LOG(INFO) << "[ActionabilityChecker] 요소 탐색 실패: " << error;
    RetryOrFail(ctx, error);
    return;
  }

  LOG(INFO) << "[ActionabilityChecker] 요소 탐색 성공: backendNodeId="
            << result->backend_node_id;

  // 체크 순서 진입
  if (NeedsVisible(ctx->action)) {
    CheckVisible(ctx, *result);
  } else if (NeedsInViewport(ctx->action)) {
    CheckInViewport(ctx, *result);
  } else if (NeedsStable(ctx->action)) {
    CheckStable(ctx, *result);
  } else if (NeedsEnabled(ctx->action)) {
    CheckEnabled(ctx, *result);
  } else {
    Complete(ctx, *result);
  }
}

// ============================================================
// 1단계: VISIBLE
// ============================================================

void ActionabilityChecker::CheckVisible(std::shared_ptr<PollContext> ctx,
                                        ElementLocator::Result result) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  LOG(INFO) << "[ActionabilityChecker] VISIBLE 체크: backendNodeId="
            << result.backend_node_id;

  base::Value::Dict params;
  params.Set("backendNodeId", result.backend_node_id);

  ctx->session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(&ActionabilityChecker::OnCheckVisibleResponse, ctx,
                     result));
}

void ActionabilityChecker::OnCheckVisibleResponse(
    std::shared_ptr<PollContext> ctx,
    ElementLocator::Result result,
    base::Value response) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  if (HasCdpError(response)) {
    std::string err = ExtractCdpErrorMessage(response);
    LOG(INFO) << "[ActionabilityChecker] VISIBLE 실패: " << err;
    RetryOrFail(ctx, "요소가 보이지 않습니다: " + err);
    return;
  }

  // 좌표 업데이트 (getBoxModel에서 최신 위치 반영)
  double cx = 0, cy = 0;
  if (ExtractBoxModelCenter(response, &cx, &cy)) {
    result.x = cx;
    result.y = cy;
  }

  LOG(INFO) << "[ActionabilityChecker] VISIBLE 통과";

  // 다음 체크
  if (NeedsInViewport(ctx->action)) {
    CheckInViewport(ctx, result);
  } else if (NeedsStable(ctx->action)) {
    CheckStable(ctx, result);
  } else if (NeedsEnabled(ctx->action)) {
    CheckEnabled(ctx, result);
  } else {
    Complete(ctx, result);
  }
}

// ============================================================
// 2단계: IN_VIEWPORT
// ============================================================

void ActionabilityChecker::CheckInViewport(std::shared_ptr<PollContext> ctx,
                                           ElementLocator::Result result) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  // 뷰포트 크기를 Page.getLayoutMetrics로 조회 (Runtime.evaluate 사용 시 스텔스 위반)
  LOG(INFO) << "[ActionabilityChecker] IN_VIEWPORT 체크: x=" << result.x
            << " y=" << result.y;

  ctx->session->SendCdpCommand(
      "Page.getLayoutMetrics", base::Value::Dict(),
      base::BindOnce(
          [](std::shared_ptr<PollContext> ctx, ElementLocator::Result result,
             base::Value response) {
            if (ctx->timed_out) {
              ActionabilityChecker::RetryOrFail(ctx, "타임아웃");
              return;
            }
            if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨

            // 뷰포트 크기 파싱: cssVisualViewport.clientWidth/clientHeight
            double vp_w = 1280, vp_h = 720;  // 기본값
            if (response.is_dict()) {
              const base::Value::Dict& resp_dict = response.GetDict();
              // CDP 래퍼: response.result.cssVisualViewport
              const base::Value::Dict* result_obj =
                  resp_dict.FindDict("result");
              const base::Value::Dict* metrics_dict =
                  result_obj ? result_obj : &resp_dict;
              const base::Value::Dict* vp_dict =
                  metrics_dict->FindDict("cssVisualViewport");
              if (vp_dict) {
                std::optional<double> w = vp_dict->FindDouble("clientWidth");
                std::optional<double> h = vp_dict->FindDouble("clientHeight");
                if (w) vp_w = *w;
                if (h) vp_h = *h;
              }
            }

            bool in_viewport = (result.x >= 0 && result.x <= vp_w &&
                                 result.y >= 0 && result.y <= vp_h);

            if (!in_viewport) {
              LOG(INFO) << "[ActionabilityChecker] 뷰포트 밖, scrollIntoView 시도";
              ActionabilityChecker::ScrollIntoViewIfNeeded(ctx, result);
            } else {
              LOG(INFO) << "[ActionabilityChecker] IN_VIEWPORT 통과";
              if (ActionabilityChecker::NeedsStable(ctx->action)) {
                ActionabilityChecker::CheckStable(ctx, result);
              } else if (ActionabilityChecker::NeedsEnabled(ctx->action)) {
                ActionabilityChecker::CheckEnabled(ctx, result);
              } else {
                ActionabilityChecker::Complete(ctx, result);
              }
            }
          },
          ctx, result));
}

void ActionabilityChecker::ScrollIntoViewIfNeeded(
    std::shared_ptr<PollContext> ctx,
    ElementLocator::Result result) {
  base::Value::Dict params;
  params.Set("backendNodeId", result.backend_node_id);

  ctx->session->SendCdpCommand(
      "DOM.scrollIntoViewIfNeeded", std::move(params),
      base::BindOnce(&ActionabilityChecker::OnScrollResponse, ctx, result));
}

void ActionabilityChecker::OnScrollResponse(std::shared_ptr<PollContext> ctx,
                                            ElementLocator::Result result,
                                            base::Value response) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  // 스크롤 후 좌표 재취득
  base::Value::Dict params;
  params.Set("backendNodeId", result.backend_node_id);

  ctx->session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(
          [](std::shared_ptr<PollContext> ctx, ElementLocator::Result result,
             base::Value response) {
            if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
            if (ctx->timed_out) {
              ActionabilityChecker::RetryOrFail(ctx, "타임아웃");
              return;
            }

            double cx = 0, cy = 0;
            if (!HasCdpError(response) &&
                ExtractBoxModelCenter(response, &cx, &cy)) {
              result.x = cx;
              result.y = cy;
            }

            LOG(INFO) << "[ActionabilityChecker] scrollIntoView 후 좌표 갱신: x="
                      << result.x << " y=" << result.y;

            if (ActionabilityChecker::NeedsStable(ctx->action)) {
              ActionabilityChecker::CheckStable(ctx, result);
            } else if (ActionabilityChecker::NeedsEnabled(ctx->action)) {
              ActionabilityChecker::CheckEnabled(ctx, result);
            } else {
              ActionabilityChecker::Complete(ctx, result);
            }
          },
          ctx, result));
}

// ============================================================
// 3단계: STABLE
// ============================================================

void ActionabilityChecker::CheckStable(std::shared_ptr<PollContext> ctx,
                                       ElementLocator::Result result) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  LOG(INFO) << "[ActionabilityChecker] STABLE 체크 시작 (1차 측정)";

  base::Value::Dict params;
  params.Set("backendNodeId", result.backend_node_id);

  ctx->session->SendCdpCommand(
      "DOM.getBoxModel", std::move(params),
      base::BindOnce(
          [](std::shared_ptr<PollContext> ctx, ElementLocator::Result result,
             base::Value response) {
            if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
            if (ctx->timed_out) {
              ActionabilityChecker::RetryOrFail(ctx, "타임아웃");
              return;
            }

            if (HasCdpError(response)) {
              std::string err = ExtractCdpErrorMessage(response);
              ActionabilityChecker::RetryOrFail(ctx, "STABLE 1차 측정 실패: " + err);
              return;
            }

            double first_x = 0, first_y = 0;
            if (!ExtractBoxModelCenter(response, &first_x, &first_y)) {
              ActionabilityChecker::RetryOrFail(ctx, "STABLE 1차 좌표 추출 실패");
              return;
            }

            LOG(INFO) << "[ActionabilityChecker] 1차 측정: x=" << first_x
                      << " y=" << first_y << " — 50ms 대기 후 2차 측정";

            // 50ms 대기 후 2차 측정
            ctx->poll_timer.Start(
                FROM_HERE, base::Milliseconds(50),
                base::BindOnce(
                    [](std::shared_ptr<PollContext> ctx,
                       ElementLocator::Result result, double first_x,
                       double first_y) {
                      if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
                      if (ctx->timed_out) {
                        ActionabilityChecker::RetryOrFail(ctx, "타임아웃");
                        return;
                      }

                      base::Value::Dict p2;
                      p2.Set("backendNodeId", result.backend_node_id);
                      ctx->session->SendCdpCommand(
                          "DOM.getBoxModel", std::move(p2),
                          base::BindOnce(
                              &ActionabilityChecker::OnStableSecondMeasure, ctx,
                              result, first_x, first_y));
                    },
                    ctx, result, first_x, first_y));
          },
          ctx, result));
}

void ActionabilityChecker::OnStableSecondMeasure(
    std::shared_ptr<PollContext> ctx,
    ElementLocator::Result result,
    double first_x,
    double first_y,
    base::Value response) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  if (HasCdpError(response)) {
    std::string err = ExtractCdpErrorMessage(response);
    RetryOrFail(ctx, "STABLE 2차 측정 실패: " + err);
    return;
  }

  double second_x = 0, second_y = 0;
  if (!ExtractBoxModelCenter(response, &second_x, &second_y)) {
    RetryOrFail(ctx, "STABLE 2차 좌표 추출 실패");
    return;
  }

  double dx = std::abs(first_x - second_x);
  double dy = std::abs(first_y - second_y);

  LOG(INFO) << "[ActionabilityChecker] STABLE 2차 측정: x=" << second_x
            << " y=" << second_y << " dx=" << dx << " dy=" << dy;

  if (dx >= 2.0 || dy >= 2.0) {
    LOG(INFO) << "[ActionabilityChecker] STABLE 실패: 요소가 이동 중";
    RetryOrFail(ctx, "요소가 아직 이동 중입니다 (dx=" + std::to_string(dx) +
                         " dy=" + std::to_string(dy) + ")");
    return;
  }

  // 좌표 최신화
  result.x = second_x;
  result.y = second_y;

  LOG(INFO) << "[ActionabilityChecker] STABLE 통과";

  if (NeedsEnabled(ctx->action)) {
    CheckEnabled(ctx, result);
  } else {
    Complete(ctx, result);
  }
}

// ============================================================
// 4단계: ENABLED  / 5단계: EDITABLE
// ============================================================

void ActionabilityChecker::CheckEnabled(std::shared_ptr<PollContext> ctx,
                                        ElementLocator::Result result) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  LOG(INFO) << "[ActionabilityChecker] ENABLED 체크: backendNodeId="
            << result.backend_node_id;

  base::Value::Dict params;
  params.Set("backendNodeId", result.backend_node_id);
  params.Set("fetchRelatives", false);

  ctx->session->SendCdpCommand(
      "Accessibility.getPartialAXTree", std::move(params),
      base::BindOnce(&ActionabilityChecker::OnCheckEnabledResponse, ctx,
                     result));
}

void ActionabilityChecker::OnCheckEnabledResponse(
    std::shared_ptr<PollContext> ctx,
    ElementLocator::Result result,
    base::Value response) {
  if (!ctx->callback) return;  // 이미 타임아웃으로 콜백 호출됨
  if (ctx->timed_out) {
    RetryOrFail(ctx, "타임아웃");
    return;
  }

  // AX Tree 응답 파싱
  // 응답 구조: { "result": { "nodes": [ { "properties": [...] } ] } }
  if (HasCdpError(response)) {
    // AX Tree 조회 실패 시 체크 통과 (보수적 처리)
    LOG(WARNING) << "[ActionabilityChecker] AX Tree 조회 실패, ENABLED 체크 건너뜀";
    Complete(ctx, result);
    return;
  }

  const base::Value::Dict* resp_dict =
      response.is_dict() ? &response.GetDict() : nullptr;
  if (!resp_dict) {
    Complete(ctx, result);
    return;
  }

  // CDP 응답 래퍼 벗기기: result.nodes
  const base::Value::Dict* result_obj = resp_dict->FindDict("result");
  const base::Value::Dict* nodes_container = result_obj ? result_obj : resp_dict;
  const base::Value::List* nodes = nodes_container->FindList("nodes");
  if (!nodes || nodes->empty()) {
    Complete(ctx, result);
    return;
  }

  // 첫 번째 노드의 properties 검사
  const base::Value::Dict* node_dict = (*nodes)[0].GetIfDict();
  if (!node_dict) {
    Complete(ctx, result);
    return;
  }

  const base::Value::List* props = node_dict->FindList("properties");
  if (!props) {
    Complete(ctx, result);
    return;
  }

  for (const base::Value& prop_val : *props) {
    const base::Value::Dict* prop = prop_val.GetIfDict();
    if (!prop) continue;

    const std::string* name = prop->FindString("name");
    if (!name) continue;

    const base::Value::Dict* value_dict = prop->FindDict("value");
    if (!value_dict) continue;

    const base::Value* val = value_dict->Find("value");
    if (!val) continue;

    // disabled 체크 (ENABLED)
    if (*name == "disabled" && val->is_bool() && val->GetBool()) {
      LOG(INFO) << "[ActionabilityChecker] ENABLED 실패: 요소가 비활성 상태";
      RetryOrFail(ctx, "요소가 비활성(disabled) 상태입니다");
      return;
    }

    // readonly 체크 (EDITABLE, fill 전용)
    if (NeedsEditable(ctx->action) && *name == "readonly" &&
        val->is_bool() && val->GetBool()) {
      LOG(INFO) << "[ActionabilityChecker] EDITABLE 실패: 읽기 전용 요소";
      RetryOrFail(ctx, "요소가 읽기 전용(readonly) 상태입니다");
      return;
    }
  }

  LOG(INFO) << "[ActionabilityChecker] ENABLED 통과";
  Complete(ctx, result);
}

// ============================================================
// 완료 / 재시도 / 타임아웃
// ============================================================

void ActionabilityChecker::Complete(std::shared_ptr<PollContext> ctx,
                                    ElementLocator::Result result) {
  ctx->timeout_timer.Stop();
  ctx->poll_timer.Stop();
  LOG(INFO) << "[ActionabilityChecker] 모든 체크 통과, 완료";
  std::move(ctx->callback).Run(result, "");
}

void ActionabilityChecker::RetryOrFail(std::shared_ptr<PollContext> ctx,
                                       const std::string& reason) {
  if (ctx->timed_out) {
    // 타임아웃 → OnTimeout이 이미 콜백을 호출했을 수 있으므로 체크
    if (!ctx->callback) return;  // 이미 OnTimeout에서 콜백 호출됨
    ctx->poll_timer.Stop();
    LOG(WARNING) << "[ActionabilityChecker] 타임아웃으로 실패: " << reason;
    ElementLocator::Result empty;
    std::move(ctx->callback).Run(empty, "타임아웃: " + reason);
    return;
  }

  if (ctx->options.timeout_ms == 0) {
    // 재시도 없음 → 즉시 실패
    LOG(WARNING) << "[ActionabilityChecker] 재시도 없이 실패: " << reason;
    ElementLocator::Result empty;
    std::move(ctx->callback).Run(empty, reason);
    return;
  }

  // poll_interval_ms 후 재시도
  LOG(INFO) << "[ActionabilityChecker] " << ctx->options.poll_interval_ms
            << "ms 후 재시도: " << reason;
  ctx->poll_timer.Start(
      FROM_HERE, base::Milliseconds(ctx->options.poll_interval_ms),
      base::BindOnce(&ActionabilityChecker::StartPoll, ctx));
}

void ActionabilityChecker::OnTimeout(std::shared_ptr<PollContext> ctx) {
  LOG(WARNING) << "[ActionabilityChecker] 타임아웃 발생 ("
               << ctx->options.timeout_ms << "ms)";
  ctx->timed_out = true;
  ctx->poll_timer.Stop();

  // 콜백이 아직 살아 있으면 즉시 실패 호출
  // (이후 도착하는 CDP 응답은 !ctx->callback 체크로 무시됨)
  if (ctx->callback) {
    ElementLocator::Result empty;
    std::move(ctx->callback)
        .Run(empty, "타임아웃: 요소가 actionable 상태가 되지 않았습니다");
  }
}

// ============================================================
// 액션별 필요 체크
// ============================================================

// static
bool ActionabilityChecker::NeedsVisible(ActionType action) {
  return action != ActionType::kScroll;
}

// static
bool ActionabilityChecker::NeedsInViewport(ActionType action) {
  return action != ActionType::kSelectOption &&
         action != ActionType::kFileUpload;
}

// static
bool ActionabilityChecker::NeedsStable(ActionType action) {
  return action == ActionType::kClick || action == ActionType::kFill ||
         action == ActionType::kDrag;
}

// static
bool ActionabilityChecker::NeedsEnabled(ActionType action) {
  return action == ActionType::kClick || action == ActionType::kFill ||
         action == ActionType::kSelectOption ||
         action == ActionType::kFileUpload;
}

// static
bool ActionabilityChecker::NeedsEditable(ActionType action) {
  return action == ActionType::kFill;
}

}  // namespace mcp
