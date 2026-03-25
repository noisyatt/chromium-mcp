// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/scroll_tool.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

namespace {

// 한 틱당 픽셀 수 (마우스 휠 한 눈금 = 120px)
constexpr double kScrollPixelsPerTick = 120.0;

// direction 문자열과 amount 틱 수를 deltaX/deltaY로 변환한다.
// CDP mouseWheel: deltaY 양수=위, 음수=아래 (브라우저 표준과 반대)
void DirectionToDeltas(const std::string& direction,
                       double amount,
                       double* out_delta_x,
                       double* out_delta_y) {
  double pixels = amount * kScrollPixelsPerTick;
  *out_delta_x = 0.0;
  *out_delta_y = 0.0;
  if (direction == "down") {
    *out_delta_y = -pixels;  // 아래로 스크롤
  } else if (direction == "up") {
    *out_delta_y = pixels;   // 위로 스크롤
  } else if (direction == "right") {
    *out_delta_x = -pixels;  // 오른쪽으로 스크롤
  } else if (direction == "left") {
    *out_delta_x = pixels;   // 왼쪽으로 스크롤
  }
}

}  // namespace

// ============================================================
// ScrollTool 구현
// ============================================================

ScrollTool::ScrollTool() = default;
ScrollTool::~ScrollTool() = default;

std::string ScrollTool::name() const {
  return "scroll";
}

std::string ScrollTool::description() const {
  return "페이지 또는 특정 요소에서 스크롤을 발생시킵니다. "
         "direction(up/down/left/right)과 amount(틱 수)로 간편하게 스크롤하거나, "
         "role/name, text, selector 등으로 특정 요소 내부를 스크롤할 수 있습니다. "
         "toTop/toBottom으로 페이지 맨 위/아래로 즉시 이동할 수 있습니다.";
}

base::DictValue ScrollTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // ---- 공통 로케이터 파라미터 ----

  // role: ARIA 역할
  base::DictValue role_prop;
  role_prop.Set("type", "string");
  role_prop.Set("description",
                "스크롤할 요소의 ARIA 역할 (예: \"list\", \"region\"). "
                "name 파라미터와 함께 사용합니다.");
  properties.Set("role", std::move(role_prop));

  // name: 접근성 이름
  base::DictValue name_prop;
  name_prop.Set("type", "string");
  name_prop.Set("description",
                "요소의 접근성 이름. role 파라미터와 함께 사용합니다.");
  properties.Set("name", std::move(name_prop));

  // text: 표시 텍스트
  base::DictValue text_prop;
  text_prop.Set("type", "string");
  text_prop.Set("description",
                "요소의 표시 텍스트로 탐색. exact=false이면 부분 일치 허용.");
  properties.Set("text", std::move(text_prop));

  // selector: 스크롤을 발생시킬 요소의 CSS 셀렉터
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description",
                    "스크롤을 발생시킬 요소의 CSS 셀렉터. "
                    "지정 시 해당 요소의 중심 좌표에서 스크롤 이벤트가 발생합니다.");
  properties.Set("selector", std::move(selector_prop));

  // xpath: XPath 표현식
  base::DictValue xpath_prop;
  xpath_prop.Set("type", "string");
  xpath_prop.Set("description",
                 "스크롤할 요소의 XPath 표현식.");
  properties.Set("xpath", std::move(xpath_prop));

  // ref: backendNodeId 참조
  base::DictValue ref_prop;
  ref_prop.Set("type", "string");
  ref_prop.Set("description",
               "접근성 스냅샷 또는 element 도구에서 얻은 요소 ref (backendNodeId).");
  properties.Set("ref", std::move(ref_prop));

  // exact: 텍스트/이름 정확히 일치 여부
  base::DictValue exact_prop;
  exact_prop.Set("type", "boolean");
  exact_prop.Set("default", true);
  exact_prop.Set("description",
                 "true이면 name/text 파라미터를 정확히 일치, "
                 "false이면 부분 문자열 일치로 탐색 (기본: true).");
  properties.Set("exact", std::move(exact_prop));

  // direction: 스크롤 방향
  base::DictValue direction_prop;
  direction_prop.Set("type", "string");
  base::ListValue direction_enum;
  direction_enum.Append("up");
  direction_enum.Append("down");
  direction_enum.Append("left");
  direction_enum.Append("right");
  direction_prop.Set("enum", std::move(direction_enum));
  direction_prop.Set("description",
                     "스크롤 방향. up/down/left/right. "
                     "direction 지정 시 amount 파라미터와 함께 사용.");
  properties.Set("direction", std::move(direction_prop));

  // amount: 스크롤 틱 수
  base::DictValue amount_prop;
  amount_prop.Set("type", "number");
  amount_prop.Set("default", 3);
  amount_prop.Set("description",
                  "스크롤 틱 수. 1틱 = 120px. 기본값: 3 (360px). "
                  "direction 파라미터와 함께 사용.");
  properties.Set("amount", std::move(amount_prop));

  // x: 스크롤 이벤트를 발생시킬 X 좌표
  base::DictValue x_prop;
  x_prop.Set("type", "number");
  x_prop.Set("description",
             "스크롤 이벤트를 발생시킬 X 좌표 (픽셀). 생략 시 뷰포트 중앙.");
  properties.Set("x", std::move(x_prop));

  // y: 스크롤 이벤트를 발생시킬 Y 좌표
  base::DictValue y_prop;
  y_prop.Set("type", "number");
  y_prop.Set("description",
             "스크롤 이벤트를 발생시킬 Y 좌표 (픽셀). 생략 시 뷰포트 중앙.");
  properties.Set("y", std::move(y_prop));

  // deltaX: 수평 스크롤량 (픽셀 직접 지정)
  base::DictValue delta_x_prop;
  delta_x_prop.Set("type", "number");
  delta_x_prop.Set("default", 0);
  delta_x_prop.Set("description",
                   "수평 스크롤량 (픽셀). 양수: 왼쪽, 음수: 오른쪽. "
                   "direction 파라미터가 없을 때 직접 지정.");
  properties.Set("deltaX", std::move(delta_x_prop));

  // deltaY: 수직 스크롤량 (픽셀 직접 지정)
  base::DictValue delta_y_prop;
  delta_y_prop.Set("type", "number");
  delta_y_prop.Set("default", -300);
  delta_y_prop.Set("description",
                   "수직 스크롤량 (픽셀). 양수: 위쪽, 음수: 아래쪽. "
                   "direction 파라미터가 없을 때 직접 지정. 기본값: -300.");
  properties.Set("deltaY", std::move(delta_y_prop));

  // toTop: 페이지 맨 위로 스크롤
  base::DictValue to_top_prop;
  to_top_prop.Set("type", "boolean");
  to_top_prop.Set("description",
                  "true이면 window.scrollTo(0, 0) 실행. "
                  "다른 스크롤 파라미터보다 우선 적용됩니다.");
  properties.Set("toTop", std::move(to_top_prop));

  // toBottom: 페이지 맨 아래로 스크롤
  base::DictValue to_bottom_prop;
  to_bottom_prop.Set("type", "boolean");
  to_bottom_prop.Set("description",
                     "true이면 window.scrollTo(0, document.body.scrollHeight) 실행. "
                     "toTop보다 낮은 우선순위. 다른 파라미터보다 우선 적용됩니다.");
  properties.Set("toBottom", std::move(to_bottom_prop));

  schema.Set("properties", std::move(properties));

  // 모든 파라미터가 선택적
  base::ListValue required;
  schema.Set("required", std::move(required));

  return schema;
}

void ScrollTool::Execute(const base::DictValue& arguments,
                         McpSession* session,
                         base::OnceCallback<void(base::Value)> callback) {
  // toTop/toBottom 처리 (최우선순위)
  std::optional<bool> to_top = arguments.FindBool("toTop");
  std::optional<bool> to_bottom = arguments.FindBool("toBottom");

  if (to_top.value_or(false)) {
    LOG(INFO) << "[ScrollTool] toTop 모드: window.scrollTo(0, 0)";
    EvaluateScroll("window.scrollTo(0, 0)", session, std::move(callback));
    return;
  }

  if (to_bottom.value_or(false)) {
    LOG(INFO) << "[ScrollTool] toBottom 모드: window.scrollTo(0, scrollHeight)";
    EvaluateScroll(
        "window.scrollTo(0, document.documentElement.scrollHeight || "
        "document.body.scrollHeight)",
        session, std::move(callback));
    return;
  }

  // direction 모드: 방향과 틱 수로 deltaX/deltaY 계산
  const std::string* direction = arguments.FindString("direction");
  double delta_x = 0.0;
  double delta_y = -300.0;  // 기본값: 아래로 300px

  if (direction && !direction->empty()) {
    std::optional<double> amount_opt = arguments.FindDouble("amount");
    double amount = amount_opt.value_or(3.0);
    if (amount < 0) amount = 3.0;
    DirectionToDeltas(*direction, amount, &delta_x, &delta_y);
    LOG(INFO) << "[ScrollTool] direction 모드: direction=" << *direction
              << " amount=" << amount
              << " deltaX=" << delta_x << " deltaY=" << delta_y;
  } else {
    // deltaX/deltaY 직접 지정
    std::optional<double> dx_opt = arguments.FindDouble("deltaX");
    std::optional<double> dy_opt = arguments.FindDouble("deltaY");
    delta_x = dx_opt.value_or(0.0);
    delta_y = dy_opt.value_or(-300.0);
  }

  // 로케이터 파라미터가 있는지 확인
  const bool has_locator =
      arguments.FindString("role") || arguments.FindString("name") ||
      arguments.FindString("text") || arguments.FindString("selector") ||
      arguments.FindString("xpath") || arguments.FindString("ref");

  if (has_locator) {
    LOG(INFO) << "[ScrollTool] 로케이터 모드: deltaX=" << delta_x
              << " deltaY=" << delta_y;
    locator_.Locate(
        session, arguments,
        base::BindOnce(&ScrollTool::OnLocated, weak_factory_.GetWeakPtr(),
                       delta_x, delta_y, session, std::move(callback)));
    return;
  }

  // 좌표 직접 지정 (없으면 0, 0)
  std::optional<double> x_opt = arguments.FindDouble("x");
  std::optional<double> y_opt = arguments.FindDouble("y");
  double x = x_opt.value_or(0.0);
  double y = y_opt.value_or(0.0);

  LOG(INFO) << "[ScrollTool] 좌표 모드: x=" << x << " y=" << y
            << " deltaX=" << delta_x << " deltaY=" << delta_y;
  DispatchScroll(x, y, delta_x, delta_y, session, std::move(callback));
}

// toTop/toBottom: Runtime.evaluate로 window.scrollTo 실행
void ScrollTool::EvaluateScroll(const std::string& expression,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("expression", expression);
  params.Set("returnByValue", false);
  params.Set("awaitPromise", false);

  session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&ScrollTool::OnEvaluateScroll,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// Runtime.evaluate 완료 콜백
void ScrollTool::OnEvaluateScroll(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (mcp::HandleCdpError(response, "Runtime.evaluate(scroll)", callback)) {
    return;
  }
  LOG(INFO) << "[ScrollTool] 페이지 스크롤 이동 완료";
  std::move(callback).Run(MakeSuccessResult("스크롤이 성공적으로 완료되었습니다."));
}

// 좌표로 직접 mouseWheel 이벤트 발송
void ScrollTool::DispatchScroll(double x,
                                double y,
                                double delta_x,
                                double delta_y,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("type", "mouseWheel");
  params.Set("x", x);
  params.Set("y", y);
  params.Set("deltaX", delta_x);
  params.Set("deltaY", delta_y);
  params.Set("modifiers", 0);

  session->SendCdpCommand(
      "Input.dispatchMouseEvent", std::move(params),
      base::BindOnce(&ScrollTool::OnScrollDispatched,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// ElementLocator 콜백: 좌표 해상도 완료 후 스크롤 발송
void ScrollTool::OnLocated(double delta_x,
                           double delta_y,
                           McpSession* session,
                           base::OnceCallback<void(base::Value)> callback,
                           std::optional<ElementLocator::Result> result,
                           std::string error) {
  if (!error.empty()) {
    LOG(WARNING) << "[ScrollTool] ElementLocator 실패: " << error;
    std::move(callback).Run(MakeErrorResult(error));
    return;
  }

  LOG(INFO) << "[ScrollTool] 스크롤 좌표: (" << result->x << ", " << result->y << ")";
  DispatchScroll(result->x, result->y, delta_x, delta_y, session,
                 std::move(callback));
}

// mouseWheel 완료 콜백
void ScrollTool::OnScrollDispatched(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (mcp::HandleCdpError(response, "Input.dispatchMouseEvent(mouseWheel)",
                           callback)) {
    return;
  }
  LOG(INFO) << "[ScrollTool] 스크롤 완료";
  std::move(callback).Run(MakeSuccessResult("스크롤이 성공적으로 완료되었습니다."));
}

}  // namespace mcp
