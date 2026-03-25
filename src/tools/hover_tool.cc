// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/hover_tool.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

// ============================================================
// HoverTool 구현
// ============================================================

HoverTool::HoverTool() = default;
HoverTool::~HoverTool() = default;

std::string HoverTool::name() const {
  return "hover";
}

std::string HoverTool::description() const {
  return "요소 위에 마우스 커서를 위치시켜 호버 이벤트를 발생시킵니다. "
         "role/name, text, selector, xpath, ref 등 다양한 방법으로 요소를 찾거나, "
         "x/y 좌표를 직접 지정할 수 있습니다. "
         "tooltip 표시, 드롭다운 메뉴 열기 등의 호버 인터랙션에 사용합니다.";
}

base::DictValue HoverTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // ---- 공통 로케이터 파라미터 ----

  // role: ARIA 역할
  base::DictValue role_prop;
  role_prop.Set("type", "string");
  role_prop.Set("description",
                "호버할 요소의 ARIA 역할 (예: \"button\", \"link\"). "
                "name 파라미터와 함께 사용하면 정확도가 높아집니다.");
  properties.Set("role", std::move(role_prop));

  // name: 접근성 이름
  base::DictValue name_prop;
  name_prop.Set("type", "string");
  name_prop.Set("description",
                "요소의 접근성 이름 (레이블 텍스트, aria-label 등). "
                "role 파라미터와 함께 사용합니다.");
  properties.Set("name", std::move(name_prop));

  // text: 표시 텍스트
  base::DictValue text_prop;
  text_prop.Set("type", "string");
  text_prop.Set("description",
                "요소의 표시 텍스트로 탐색. exact=false이면 부분 일치 허용.");
  properties.Set("text", std::move(text_prop));

  // selector: CSS 셀렉터
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description",
                    "호버할 요소의 CSS 셀렉터. "
                    "지정 시 해당 요소의 중심 좌표에서 mouseMoved 이벤트가 발생합니다.");
  properties.Set("selector", std::move(selector_prop));

  // xpath: XPath 표현식
  base::DictValue xpath_prop;
  xpath_prop.Set("type", "string");
  xpath_prop.Set("description",
                 "호버할 요소의 XPath 표현식 (예: \"//button[@id='ok']\").");
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

  // x: 직접 좌표 지정 시 X 좌표
  base::DictValue x_prop;
  x_prop.Set("type", "number");
  x_prop.Set("description", "호버 이벤트를 발생시킬 X 좌표 (픽셀). 로케이터가 없을 때 사용.");
  properties.Set("x", std::move(x_prop));

  // y: 직접 좌표 지정 시 Y 좌표
  base::DictValue y_prop;
  y_prop.Set("type", "number");
  y_prop.Set("description", "호버 이벤트를 발생시킬 Y 좌표 (픽셀). 로케이터가 없을 때 사용.");
  properties.Set("y", std::move(y_prop));

  schema.Set("properties", std::move(properties));

  // 로케이터 또는 x/y 중 하나 필요 (런타임 검증)
  base::ListValue required;
  schema.Set("required", std::move(required));

  return schema;
}

void HoverTool::Execute(const base::DictValue& arguments,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback) {
  // 로케이터 파라미터가 있는지 확인
  const bool has_locator =
      arguments.FindString("role") || arguments.FindString("name") ||
      arguments.FindString("text") || arguments.FindString("selector") ||
      arguments.FindString("xpath") || arguments.FindString("ref");

  if (has_locator) {
    LOG(INFO) << "[HoverTool] 로케이터 모드";
    locator_.Locate(
        session, arguments,
        base::BindOnce(&HoverTool::OnLocated, weak_factory_.GetWeakPtr(),
                       session, std::move(callback)));
    return;
  }

  // 좌표 직접 지정 모드
  std::optional<double> x_opt = arguments.FindDouble("x");
  std::optional<double> y_opt = arguments.FindDouble("y");

  if (!x_opt.has_value() || !y_opt.has_value()) {
    LOG(WARNING) << "[HoverTool] 로케이터 또는 x/y 좌표 파라미터가 필요합니다.";
    std::move(callback).Run(
        MakeErrorResult("로케이터(role/name/text/selector/xpath/ref) 또는 "
                        "x/y 좌표 파라미터가 필요합니다."));
    return;
  }

  LOG(INFO) << "[HoverTool] 좌표 모드: x=" << *x_opt << " y=" << *y_opt;
  DispatchHover(*x_opt, *y_opt, session, std::move(callback));
}

// ElementLocator 콜백: 좌표 해상도 완료 후 호버 발송
void HoverTool::OnLocated(McpSession* session,
                          base::OnceCallback<void(base::Value)> callback,
                          std::optional<ElementLocator::Result> result,
                          std::string error) {
  if (!error.empty()) {
    LOG(WARNING) << "[HoverTool] ElementLocator 실패: " << error;
    std::move(callback).Run(MakeErrorResult(error));
    return;
  }

  LOG(INFO) << "[HoverTool] 호버 좌표: (" << result->x << ", " << result->y << ")";
  DispatchHover(result->x, result->y, session, std::move(callback));
}

// 좌표로 직접 mouseMoved 이벤트 발송
void HoverTool::DispatchHover(double x,
                              double y,
                              McpSession* session,
                              base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  params.Set("type", "mouseMoved");
  params.Set("x", x);
  params.Set("y", y);
  params.Set("button", "none");
  params.Set("buttons", 0);
  params.Set("modifiers", 0);

  session->SendCdpCommand(
      "Input.dispatchMouseEvent", std::move(params),
      base::BindOnce(&HoverTool::OnHoverDispatched,
                     weak_factory_.GetWeakPtr(),
                     std::move(callback)));
}

// mouseMoved 완료 콜백
void HoverTool::OnHoverDispatched(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (mcp::HandleCdpError(response, "Input.dispatchMouseEvent(mouseMoved)",
                           callback)) {
    return;
  }
  LOG(INFO) << "[HoverTool] 호버 완료";
  std::move(callback).Run(MakeSuccessResult("호버가 성공적으로 완료되었습니다."));
}

}  // namespace mcp
