// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/click_tool.h"

#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

namespace {

// MCP 성공 응답 Value 생성
base::Value MakeSuccessResult(const std::string& message) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", message);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", false);
  return base::Value(std::move(result));
}

// MCP 에러 응답 Value 생성
base::Value MakeErrorResult(const std::string& message) {
  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", message);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", true);
  return base::Value(std::move(result));
}

// CDP 응답에 "error" 키가 있는지 확인
bool HasCdpError(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return true;
  }
  return dict->Find("error") != nullptr;
}

// CDP 응답에서 에러 메시지 추출
std::string ExtractCdpErrorMessage(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return "CDP 응답이 Dict 형식이 아님";
  }
  const base::DictValue* error = dict->FindDict("error");
  if (!error) {
    return "알 수 없는 CDP 에러";
  }
  const std::string* msg = error->FindString("message");
  if (!msg) {
    return "에러 메시지 없음";
  }
  return *msg;
}

// Input.dispatchMouseEvent 파라미터 Dict 생성
base::DictValue MakeMouseEventParams(const std::string& type,
                                     double x,
                                     double y,
                                     const std::string& button) {
  // CDP 버튼 인덱스: left=0, middle=1, right=2
  int button_index = 0;
  if (button == "middle") {
    button_index = 1;
  } else if (button == "right") {
    button_index = 2;
  }

  base::DictValue params;
  params.Set("type", type);
  params.Set("x", x);
  params.Set("y", y);
  params.Set("button", button);
  params.Set("buttons", button_index == 0 ? 1 : (button_index == 1 ? 4 : 2));
  params.Set("clickCount", 1);
  params.Set("modifiers", 0);
  return params;
}

// waitForNavigation 타임아웃 (5초)
constexpr int kLoadTimeoutMs = 5000;

// waitForNavigation 이벤트 핸들러 키 (UnregisterCdpEventHandler 시 사용)
constexpr char kLoadEventKey[] = "Page.loadEventFired";

}  // namespace

// ============================================================
// ClickTool 구현
// ============================================================

ClickTool::ClickTool() = default;
ClickTool::~ClickTool() = default;

std::string ClickTool::name() const {
  return "click";
}

std::string ClickTool::description() const {
  return "role/name, text, selector, xpath, ref 등 다양한 방법으로 요소를 찾아 "
         "클릭합니다. ActionabilityChecker로 요소 상태를 검증(가시성, 활성화 등)한 후 "
         "클릭을 수행합니다. button으로 마우스 버튼을, "
         "waitForNavigation으로 페이지 이동 완료 대기를 설정할 수 있습니다.";
}

base::DictValue ClickTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // ---- 공통 로케이터 파라미터 ----

  // role: ARIA 역할
  base::DictValue role_prop;
  role_prop.Set("type", "string");
  role_prop.Set("description",
                "클릭할 요소의 ARIA 역할 (예: \"button\", \"link\", \"checkbox\"). "
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
                "요소의 표시 텍스트로 탐색. exact=false 이면 부분 일치 허용.");
  properties.Set("text", std::move(text_prop));

  // selector: CSS 셀렉터
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description",
                    "클릭할 요소의 CSS 셀렉터 (예: \"#submit\", \".btn\").");
  properties.Set("selector", std::move(selector_prop));

  // xpath: XPath 표현식
  base::DictValue xpath_prop;
  xpath_prop.Set("type", "string");
  xpath_prop.Set("description",
                 "클릭할 요소의 XPath 표현식 (예: \"//button[@id='ok']\").");
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

  // ---- auto-wait 파라미터 ----

  // timeout: 최대 대기 시간 (ms)
  base::DictValue timeout_prop;
  timeout_prop.Set("type", "number");
  timeout_prop.Set("default", 5000);
  timeout_prop.Set("description",
                   "요소가 actionable 상태가 될 때까지 최대 대기 시간 (ms, 기본: 5000).");
  properties.Set("timeout", std::move(timeout_prop));

  // force: actionability 체크 건너뜀
  base::DictValue force_prop;
  force_prop.Set("type", "boolean");
  force_prop.Set("default", false);
  force_prop.Set("description",
                 "true이면 actionability 체크를 건너뛰고 강제로 클릭 (기본: false).");
  properties.Set("force", std::move(force_prop));

  // ---- click 전용 파라미터 ----

  // button: 마우스 버튼 종류
  base::DictValue button_prop;
  button_prop.Set("type", "string");
  base::ListValue button_enum;
  button_enum.Append("left");
  button_enum.Append("right");
  button_enum.Append("middle");
  button_prop.Set("enum", std::move(button_enum));
  button_prop.Set("default", "left");
  button_prop.Set("description", "클릭에 사용할 마우스 버튼 (기본: \"left\").");
  properties.Set("button", std::move(button_prop));

  // waitForNavigation: 페이지 이동 완료 대기 여부
  base::DictValue wait_prop;
  wait_prop.Set("type", "boolean");
  wait_prop.Set("default", false);
  wait_prop.Set("description",
                "true이면 클릭 후 Page.loadEventFired 이벤트까지 대기 (기본: false). "
                "링크 클릭으로 페이지 이동이 발생할 때 사용합니다.");
  properties.Set("waitForNavigation", std::move(wait_prop));

  schema.Set("properties", std::move(properties));

  // 로케이터는 런타임에서 검증 (role/name, text, selector, xpath, ref 중 하나 필요)
  base::ListValue required;
  schema.Set("required", std::move(required));

  return schema;
}

void ClickTool::Execute(const base::DictValue& arguments,
                        McpSession* session,
                        base::OnceCallback<void(base::Value)> callback) {
  // button 파라미터 (기본값: "left")
  const std::string* button_param = arguments.FindString("button");
  std::string button =
      (button_param && !button_param->empty()) ? *button_param : "left";
  if (button != "left" && button != "right" && button != "middle") {
    LOG(WARNING) << "[ClickTool] 알 수 없는 버튼 값: " << button << " → left로 대체";
    button = "left";
  }

  // waitForNavigation 파라미터 (기본값: false)
  bool wait_for_navigation = arguments.FindBool("waitForNavigation").value_or(false);

  // timeout / force 파라미터
  ActionabilityChecker::Options options;
  std::optional<double> timeout_opt = arguments.FindDouble("timeout");
  if (timeout_opt.has_value()) {
    options.timeout_ms = static_cast<int>(*timeout_opt);
  }
  options.force = arguments.FindBool("force").value_or(false);

  LOG(INFO) << "[ClickTool] 실행: button=" << button
            << " waitForNavigation=" << wait_for_navigation
            << " timeout=" << options.timeout_ms
            << " force=" << options.force;

  actionability_checker_.VerifyAndLocate(
      session, arguments, ActionabilityChecker::ActionType::kClick, options,
      base::BindOnce(&ClickTool::OnActionable, weak_factory_.GetWeakPtr(),
                     button, wait_for_navigation, session,
                     std::move(callback)));
}

void ClickTool::OnActionable(const std::string& button,
                             bool wait_for_navigation,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback,
                             ElementLocator::Result result,
                             std::string error) {
  if (!error.empty()) {
    LOG(WARNING) << "[ClickTool] ActionabilityChecker 실패: " << error;
    std::move(callback).Run(MakeErrorResult(error));
    return;
  }

  LOG(INFO) << "[ClickTool] 클릭 좌표: (" << result.x << ", " << result.y
            << ") role=" << result.role << " name=" << result.name;

  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mousePressed", result.x, result.y, button),
      base::BindOnce(&ClickTool::OnMousePressed, weak_factory_.GetWeakPtr(),
                     result.x, result.y, button, wait_for_navigation, session,
                     std::move(callback)));
}

void ClickTool::OnMousePressed(double x,
                               double y,
                               const std::string& button,
                               bool wait_for_navigation,
                               McpSession* session,
                               base::OnceCallback<void(base::Value)> callback,
                               base::Value response) {
  if (HandleCdpError(response, "Input.dispatchMouseEvent(mousePressed)",
                     callback)) {
    return;
  }

  session->SendCdpCommand(
      "Input.dispatchMouseEvent",
      MakeMouseEventParams("mouseReleased", x, y, button),
      base::BindOnce(&ClickTool::OnMouseReleased, weak_factory_.GetWeakPtr(),
                     wait_for_navigation, session, std::move(callback)));
}

void ClickTool::OnMouseReleased(bool wait_for_navigation,
                                McpSession* session,
                                base::OnceCallback<void(base::Value)> callback,
                                base::Value response) {
  if (HandleCdpError(response, "Input.dispatchMouseEvent(mouseReleased)",
                     callback)) {
    return;
  }

  if (!wait_for_navigation) {
    LOG(INFO) << "[ClickTool] 클릭 완료";
    std::move(callback).Run(MakeSuccessResult("클릭이 성공적으로 완료되었습니다."));
    return;
  }

  LOG(INFO) << "[ClickTool] 페이지 로드 이벤트 대기 중...";
  WaitForLoad(session, std::move(callback));
}

void ClickTool::WaitForLoad(McpSession* session,
                            base::OnceCallback<void(base::Value)> callback) {
  // OnceCallback을 shared_ptr로 래핑: RepeatingCallback에서 1회만 실행 가능하게 함
  auto shared_callback =
      std::make_shared<base::OnceCallback<void(base::Value)>>(
          std::move(callback));

  // Page.loadEventFired 이벤트 핸들러 등록
  session->RegisterCdpEventHandler(
      kLoadEventKey,
      base::BindRepeating(&ClickTool::OnLoadEventFired,
                          weak_factory_.GetWeakPtr(), session,
                          shared_callback));

  // 5초 타임아웃 타이머 설정
  load_timeout_timer_.Start(
      FROM_HERE, base::Milliseconds(kLoadTimeoutMs),
      base::BindOnce(&ClickTool::OnLoadTimeout, weak_factory_.GetWeakPtr(),
                     session, shared_callback));
}

void ClickTool::OnLoadEventFired(
    McpSession* session,
    std::shared_ptr<base::OnceCallback<void(base::Value)>> shared_callback,
    const std::string& event_name,
    const base::DictValue& params) {
  // shared_callback이 이미 소비된 경우(타임아웃이 먼저 발생) 무시
  if (!shared_callback || !*shared_callback) {
    return;
  }

  // 타임아웃 타이머 취소
  load_timeout_timer_.Stop();
  // 이벤트 핸들러 해제
  session->UnregisterCdpEventHandler(kLoadEventKey);

  LOG(INFO) << "[ClickTool] 페이지 로드 완료";
  std::move(*shared_callback)
      .Run(MakeSuccessResult("클릭 후 페이지 로드가 완료되었습니다."));
}

void ClickTool::OnLoadTimeout(
    McpSession* session,
    std::shared_ptr<base::OnceCallback<void(base::Value)>> shared_callback) {
  // shared_callback이 이미 소비된 경우(이벤트가 먼저 발생) 무시
  if (!shared_callback || !*shared_callback) {
    return;
  }

  // 이벤트 핸들러 해제
  session->UnregisterCdpEventHandler(kLoadEventKey);

  LOG(WARNING) << "[ClickTool] 페이지 로드 타임아웃 (" << kLoadTimeoutMs << "ms)";
  // 타임아웃은 에러가 아닌 경고로 처리: 클릭 자체는 성공했으나 로드를 기다리지 못함
  std::move(*shared_callback)
      .Run(MakeSuccessResult("클릭 완료 (페이지 로드 타임아웃 — 로드가 느리거나 "
                             "탐색이 발생하지 않았을 수 있습니다)."));
}

// 정적 헬퍼: CDP 에러 처리
// NOLINTNEXTLINE(runtime/references)
bool ClickTool::HandleCdpError(
    const base::Value& response,
    const std::string& step_name,
    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[ClickTool] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

}  // namespace mcp
