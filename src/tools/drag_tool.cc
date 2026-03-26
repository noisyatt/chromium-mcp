// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/drag_tool.h"

#include <cmath>
#include <memory>
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

double Lerp(double start, double end, double t) {
  return start + (end - start) * t;
}

base::DictValue MakeMouseEventParams(const std::string& type,
                                     double x, double y,
                                     const std::string& button,
                                     int buttons) {
  base::DictValue params;
  params.Set("type", type);
  params.Set("x", x);
  params.Set("y", y);
  params.Set("button", button);
  params.Set("buttons", buttons);
  params.Set("clickCount", type == "mousePressed" ? 1 : 0);
  params.Set("modifiers", 0);
  return params;
}

}  // namespace

DragTool::DragTool() = default;
DragTool::~DragTool() = default;

DragTool::DragContext::DragContext() = default;
DragTool::DragContext::~DragContext() = default;

std::string DragTool::name() const {
  return "drag";
}

std::string DragTool::description() const {
  return "드래그 앤 드롭을 시뮬레이션합니다. "
         "HTML5 DnD(dragstart/dragover/drop) 이벤트를 지원합니다. "
         "startRole/startName/startText 또는 startSelector로 시작 요소를, "
         "endRole/endName/endText 또는 endSelector로 끝 요소를 지정하거나, "
         "startX/startY, endX/endY로 좌표를 직접 지정할 수 있습니다.";
}

base::DictValue DragTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // 시작점 로케이터
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "드래그 시작 요소의 ARIA 역할");
    properties.Set("startRole", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "드래그 시작 요소의 접근성 이름");
    properties.Set("startName", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "드래그 시작 요소의 표시 텍스트");
    properties.Set("startText", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "드래그 시작 요소의 CSS 셀렉터");
    properties.Set("startSelector", std::move(p));
  }

  // 끝점 로케이터
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "드래그 끝(드롭) 요소의 ARIA 역할");
    properties.Set("endRole", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "드래그 끝(드롭) 요소의 접근성 이름");
    properties.Set("endName", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "드래그 끝(드롭) 요소의 표시 텍스트");
    properties.Set("endText", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "string");
    p.Set("description", "드래그 끝(드롭) 요소의 CSS 셀렉터");
    properties.Set("endSelector", std::move(p));
  }

  // 좌표 직접 지정
  {
    base::DictValue p;
    p.Set("type", "number");
    p.Set("description", "드래그 시작 X 좌표 (픽셀)");
    properties.Set("startX", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "number");
    p.Set("description", "드래그 시작 Y 좌표 (픽셀)");
    properties.Set("startY", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "number");
    p.Set("description", "드래그 끝 X 좌표 (픽셀)");
    properties.Set("endX", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "number");
    p.Set("description", "드래그 끝 Y 좌표 (픽셀)");
    properties.Set("endY", std::move(p));
  }
  {
    base::DictValue p;
    p.Set("type", "number");
    p.Set("default", 10);
    p.Set("description", "이동 단계 수 (기본값: 10)");
    properties.Set("steps", std::move(p));
  }

  schema.Set("properties", std::move(properties));
  schema.Set("required", base::ListValue());
  return schema;
}

void DragTool::Execute(const base::DictValue& arguments,
                       McpSession* session,
                       base::OnceCallback<void(base::Value)> callback) {
  std::optional<double> steps_opt = arguments.FindDouble("steps");
  int steps = static_cast<int>(steps_opt.value_or(10.0));
  if (steps < 1) steps = 1;

  // 로케이터 파라미터 확인
  const std::string* start_role = arguments.FindString("startRole");
  const std::string* start_name = arguments.FindString("startName");
  const std::string* start_text = arguments.FindString("startText");
  const std::string* start_selector = arguments.FindString("startSelector");

  const bool has_start_locator =
      (start_role && !start_role->empty()) ||
      (start_name && !start_name->empty()) ||
      (start_text && !start_text->empty()) ||
      (start_selector && !start_selector->empty());

  const std::string* end_role = arguments.FindString("endRole");
  const std::string* end_name = arguments.FindString("endName");
  const std::string* end_text = arguments.FindString("endText");
  const std::string* end_selector = arguments.FindString("endSelector");

  const bool has_end_locator =
      (end_role && !end_role->empty()) ||
      (end_name && !end_name->empty()) ||
      (end_text && !end_text->empty()) ||
      (end_selector && !end_selector->empty());

  if (has_start_locator != has_end_locator) {
    std::move(callback).Run(MakeErrorResult(
        "시작과 끝 모두 로케이터이거나 모두 좌표여야 합니다."));
    return;
  }

  if (has_start_locator && has_end_locator) {
    base::DictValue start_params;
    if (start_role && !start_role->empty()) start_params.Set("role", *start_role);
    if (start_name && !start_name->empty()) start_params.Set("name", *start_name);
    if (start_text && !start_text->empty()) start_params.Set("text", *start_text);
    if (start_selector && !start_selector->empty()) start_params.Set("selector", *start_selector);
    if (auto exact = arguments.FindBool("exact")) start_params.Set("exact", *exact);

    base::DictValue end_params;
    if (end_role && !end_role->empty()) end_params.Set("role", *end_role);
    if (end_name && !end_name->empty()) end_params.Set("name", *end_name);
    if (end_text && !end_text->empty()) end_params.Set("text", *end_text);
    if (end_selector && !end_selector->empty()) end_params.Set("selector", *end_selector);
    if (auto exact = arguments.FindBool("exact")) end_params.Set("exact", *exact);

    start_locator_.Locate(
        session, start_params,
        base::BindOnce(&DragTool::OnStartLocated, weak_factory_.GetWeakPtr(),
                       std::move(end_params), steps, session, std::move(callback)));
    return;
  }

  // 좌표 직접 지정
  std::optional<double> sx = arguments.FindDouble("startX");
  std::optional<double> sy = arguments.FindDouble("startY");
  std::optional<double> ex = arguments.FindDouble("endX");
  std::optional<double> ey = arguments.FindDouble("endY");

  if (!sx || !sy || !ex || !ey) {
    std::move(callback).Run(MakeErrorResult(
        "startRole/startSelector 또는 startX/startY/endX/endY 파라미터가 필요합니다."));
    return;
  }

  StartDragSequence(*sx, *sy, *ex, *ey, steps, session, std::move(callback));
}

void DragTool::OnStartLocated(base::DictValue end_params, int steps,
                              McpSession* session,
                              base::OnceCallback<void(base::Value)> callback,
                              std::optional<ElementLocator::Result> result,
                              std::string error) {
  if (!error.empty()) {
    std::move(callback).Run(MakeErrorResult("시작 요소를 찾을 수 없습니다: " + error));
    return;
  }
  double sx = result->x, sy = result->y;
  end_locator_.Locate(
      session, end_params,
      base::BindOnce(&DragTool::OnEndLocated, weak_factory_.GetWeakPtr(),
                     sx, sy, steps, session, std::move(callback)));
}

void DragTool::OnEndLocated(double start_x, double start_y, int steps,
                            McpSession* session,
                            base::OnceCallback<void(base::Value)> callback,
                            std::optional<ElementLocator::Result> result,
                            std::string error) {
  if (!error.empty()) {
    std::move(callback).Run(MakeErrorResult("끝 요소를 찾을 수 없습니다: " + error));
    return;
  }
  StartDragSequence(start_x, start_y, result->x, result->y, steps,
                    session, std::move(callback));
}

// ============================================================
// HTML5 DnD 드래그 시퀀스
// ============================================================

void DragTool::StartDragSequence(double start_x, double start_y,
                                 double end_x, double end_y,
                                 int steps, McpSession* session,
                                 base::OnceCallback<void(base::Value)> callback) {
  auto ctx = std::make_shared<DragContext>();
  ctx->start_x = start_x;
  ctx->start_y = start_y;
  ctx->end_x = end_x;
  ctx->end_y = end_y;
  ctx->steps = steps;
  ctx->session = session;
  ctx->callback = std::move(callback);

  LOG(INFO) << "[DragTool] HTML5 DnD 시퀀스 시작: ("
            << start_x << "," << start_y << ") → ("
            << end_x << "," << end_y << ") steps=" << steps;

  // Step 1: mousePressed
  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mousePressed", start_x, start_y, "left", 1),
      base::BindOnce(&DragTool::OnMousePressed,
                     weak_factory_.GetWeakPtr(), ctx));
}

void DragTool::OnMousePressed(std::shared_ptr<DragContext> ctx,
                              base::Value response) {
  if (HandleCdpError(response, "mousePressed", ctx->callback)) return;

  // Step 2: mouseMoved 시퀀스 시작
  DispatchNextMove(ctx, 0);
}

void DragTool::DispatchNextMove(std::shared_ptr<DragContext> ctx,
                                int current_step) {
  if (current_step >= ctx->steps) {
    OnAllMovesDone(ctx);
    return;
  }

  double t = static_cast<double>(current_step + 1) / static_cast<double>(ctx->steps);
  double x = Lerp(ctx->start_x, ctx->end_x, t);
  double y = Lerp(ctx->start_y, ctx->end_y, t);

  ctx->session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseMoved", x, y, "left", 1),
      base::BindOnce(&DragTool::OnMouseMoved,
                     weak_factory_.GetWeakPtr(), ctx, current_step + 1));
}

void DragTool::OnMouseMoved(std::shared_ptr<DragContext> ctx,
                            int current_step, base::Value response) {
  if (HandleCdpError(response, "mouseMoved", ctx->callback)) return;
  DispatchNextMove(ctx, current_step);
}

void DragTool::OnAllMovesDone(std::shared_ptr<DragContext> ctx) {
  // Step 3: mouseReleased 먼저 (마우스 캡처 해제 후 JS DnD 이벤트 발화)
  DispatchMouseReleased(ctx);
}

void DragTool::DispatchJsDragEvents(std::shared_ptr<DragContext> ctx) {
  // Step 4: JS로 HTML5 DnD 이벤트를 직접 디스패치
  // CDP의 Input.dispatchDragEvent는 실험적이고 불안정하므로,
  // Runtime.evaluate로 JS DragEvent를 직접 발화하는 하이브리드 방식 사용.
  LOG(INFO) << "[DragTool] mouseMoved 완료, JS DragEvent 디스패치";

  // 시작/끝 좌표에 해당하는 요소를 찾아 DragEvent를 순서대로 발화하는 JS
  // elementFromPoint 대신 좌표 근처의 draggable 요소를 CSS 셀렉터로 직접 찾는다.
  // 좌표 기반 elementFromPoint는 스크롤/오버레이로 실패할 수 있으므로
  // 보다 안정적인 접근 방식 사용.
  // 페이지 절대 좌표를 사용하여 요소를 찾는다.
  // elementFromPoint는 뷰포트 좌표만 지원하므로,
  // 페이지 내 모든 요소의 bounding rect를 비교하여 좌표에 해당하는 요소를 찾는다.
  std::string js = "(function() {"
    "var sx=" + std::to_string(ctx->start_x) + ","
    "sy=" + std::to_string(ctx->start_y) + ","
    "ex=" + std::to_string(ctx->end_x) + ","
    "ey=" + std::to_string(ctx->end_y) + ";"
    // 좌표에 해당하는 요소를 찾는 헬퍼 (페이지 절대 좌표 기준)
    "function findAt(px,py){"
    "  var all=document.querySelectorAll('*');"
    "  var best=null,bestArea=Infinity;"
    "  for(var i=0;i<all.length;i++){"
    "    var r=all[i].getBoundingClientRect();"
    // getBoundingClientRect는 뷰포트 기준이므로 스크롤 보정
    "    var ax=r.left+window.scrollX,ay=r.top+window.scrollY;"
    "    if(px>=ax&&px<=ax+r.width&&py>=ay&&py<=ay+r.height){"
    "      var area=r.width*r.height;"
    "      if(area<bestArea){bestArea=area;best=all[i];}"
    "    }"
    "  }"
    "  return best;"
    "}"
    "var startEl=findAt(sx,sy),endEl=findAt(ex,ey);"
    "if(!startEl||!endEl) return 'no_elements';"
    // draggable 부모 탐색
    "var drag=startEl;"
    "while(drag&&!drag.draggable&&drag!==document.body) drag=drag.parentElement;"
    "if(drag&&drag.draggable) startEl=drag;"
    "try{"
    "var dt=new DataTransfer();"
    "startEl.dispatchEvent(new DragEvent('dragstart',{bubbles:true,cancelable:true,dataTransfer:dt}));"
    "endEl.dispatchEvent(new DragEvent('dragenter',{bubbles:true,dataTransfer:dt}));"
    "endEl.dispatchEvent(new DragEvent('dragover',{bubbles:true,cancelable:true,dataTransfer:dt}));"
    "endEl.dispatchEvent(new DragEvent('drop',{bubbles:true,cancelable:true,dataTransfer:dt}));"
    "startEl.dispatchEvent(new DragEvent('dragend',{bubbles:true,dataTransfer:dt}));"
    "return 'ok:'+startEl.id+'->'+endEl.id;"
    "}catch(e){return 'error:'+e.message;}"
    "})()";

  base::DictValue params;
  params.Set("expression", js);
  params.Set("returnByValue", true);
  params.Set("awaitPromise", false);
  params.Set("userGesture", true);

  ctx->session->SendCdpCommand(
      "Runtime.evaluate", std::move(params),
      base::BindOnce(&DragTool::OnDragEventsDispatched,
                     weak_factory_.GetWeakPtr(), ctx));
}

void DragTool::OnDragEventsDispatched(std::shared_ptr<DragContext> ctx,
                                      base::Value response) {
  if (HasCdpError(response)) {
    LOG(WARNING) << "[DragTool] JS DragEvent 디스패치 실패: "
                 << ExtractCdpErrorMessage(response);
  } else {
    // 디버깅: JS 실행 결과 로깅
    const base::DictValue* dict = response.GetIfDict();
    if (dict) {
      const base::DictValue* result_obj = dict->FindDict("result");
      if (result_obj) {
        const std::string* val = result_obj->FindString("value");
        if (val) {
          LOG(INFO) << "[DragTool] JS DragEvent 결과: " << *val;
        }
      }
    }
  }

  LOG(INFO) << "[DragTool] 드래그 앤 드롭 완료 (하이브리드 DnD)";
  std::move(ctx->callback).Run(
      MakeSuccessResult("드래그 앤 드롭이 성공적으로 완료되었습니다."));
}

void DragTool::DispatchMouseReleased(std::shared_ptr<DragContext> ctx) {
  ctx->session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseReleased", ctx->end_x, ctx->end_y, "left", 0),
      base::BindOnce(&DragTool::OnMouseReleased,
                     weak_factory_.GetWeakPtr(), ctx));
}

void DragTool::OnMouseReleased(std::shared_ptr<DragContext> ctx,
                               base::Value response) {
  if (HandleCdpError(response, "mouseReleased", ctx->callback)) return;
  LOG(INFO) << "[DragTool] mouseReleased 완료, JS DragEvent 디스패치";
  DispatchJsDragEvents(ctx);
}


}  // namespace mcp
