// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/screenshot_tool.h"

#include <utility>

#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"

namespace mcp {

ScreenshotTool::ScreenshotTool() = default;
ScreenshotTool::~ScreenshotTool() = default;

std::string ScreenshotTool::name() const {
  return "screenshot";
}

std::string ScreenshotTool::description() const {
  return "현재 페이지 또는 특정 요소의 스크린샷";
}

base::DictValue ScreenshotTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // fullPage: 전체 페이지 스크롤 캡처 여부 (기본값 false = 뷰포트만)
  base::DictValue full_page_prop;
  full_page_prop.Set("type", "boolean");
  full_page_prop.Set("description",
                     "true이면 스크롤 포함 전체 페이지 캡처, "
                     "false이면 현재 뷰포트만 캡처 (기본값: false)");
  full_page_prop.Set("default", false);
  properties.Set("fullPage", std::move(full_page_prop));

  // selector: 특정 요소만 캡처할 때 사용하는 CSS 선택자
  base::DictValue selector_prop;
  selector_prop.Set("type", "string");
  selector_prop.Set("description",
                    "캡처할 특정 요소의 CSS 선택자 (생략 시 전체 페이지)");
  properties.Set("selector", std::move(selector_prop));

  // format: 이미지 형식 (기본값 "png")
  base::DictValue format_prop;
  format_prop.Set("type", "string");
  base::ListValue format_enum;
  format_enum.Append("png");
  format_enum.Append("jpeg");
  format_prop.Set("enum", std::move(format_enum));
  format_prop.Set("description", "이미지 형식: png 또는 jpeg (기본값: png)");
  format_prop.Set("default", "png");
  properties.Set("format", std::move(format_prop));

  schema.Set("properties", std::move(properties));
  return schema;
}

void ScreenshotTool::Execute(const base::DictValue& arguments,
                             McpSession* session,
                             base::OnceCallback<void(base::Value)> callback) {
  // format 파라미터 추출 (기본값 "png")
  const std::string* format_ptr = arguments.FindString("format");
  std::string format = format_ptr ? *format_ptr : "png";

  // 유효하지 않은 format 값 처리
  if (format != "png" && format != "jpeg") {
    LOG(WARNING) << "[ScreenshotTool] 유효하지 않은 format: " << format
                 << ", png로 대체";
    format = "png";
  }

  // selector 파라미터 확인
  const std::string* selector_ptr = arguments.FindString("selector");

  if (selector_ptr && !selector_ptr->empty()) {
    // selector가 지정된 경우: 특정 요소 캡처 (3단계 비동기 체인)
    LOG(INFO) << "[ScreenshotTool] selector 지정 캡처: " << *selector_ptr;
    QuerySelectorForScreenshot(*selector_ptr, format, session,
                               std::move(callback));
  } else {
    // selector 없음: 전체 페이지 또는 뷰포트 캡처
    std::optional<bool> full_page = arguments.FindBool("fullPage");
    bool capture_full_page = full_page.value_or(false);
    LOG(INFO) << "[ScreenshotTool] 전체 캡처, fullPage=" << capture_full_page;
    CaptureFullPageScreenshot(format, capture_full_page, session,
                              std::move(callback));
  }
}

void ScreenshotTool::CaptureFullPageScreenshot(
    const std::string& format,
    bool full_page,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  base::DictValue params;
  // CDP Page.captureScreenshot의 format 파라미터
  params.Set("format", format);

  if (full_page) {
    // 전체 페이지 캡처: captureBeyondViewport=true 설정
    // 단, 스크롤 크기가 매우 큰 경우 메모리 사용량에 주의
    params.Set("captureBeyondViewport", true);
    LOG(INFO) << "[ScreenshotTool] captureBeyondViewport=true로 전체 페이지 캡처";
  } else {
    params.Set("captureBeyondViewport", false);
  }

  session->SendCdpCommand(
      "Page.captureScreenshot", std::move(params),
      base::BindOnce(&ScreenshotTool::OnCaptureScreenshotResponse,
                     weak_factory_.GetWeakPtr(), std::move(callback)));
}

void ScreenshotTool::QuerySelectorForScreenshot(
    const std::string& selector,
    const std::string& format,
    McpSession* session,
    base::OnceCallback<void(base::Value)> callback) {
  // 1단계: DOM.getDocument로 루트 노드 ID를 가져온 후 DOM.querySelector 호출.
  // 단순화를 위해 depth=0으로 루트 노드만 가져온다.
  base::DictValue get_doc_params;
  get_doc_params.Set("depth", 0);
  get_doc_params.Set("pierce", false);

  // DOM.getDocument 응답을 람다로 처리해 nodeId를 추출한 뒤
  // DOM.querySelector를 연쇄 호출한다.
  session->SendCdpCommand(
      "DOM.getDocument", std::move(get_doc_params),
      base::BindOnce(
          [](const std::string& selector, const std::string& format,
             McpSession* session,
             base::OnceCallback<void(base::Value)> callback,
             base::Value get_doc_response) {
            // DOM.getDocument 응답에서 루트 nodeId 추출
            if (!get_doc_response.is_dict()) {
              LOG(ERROR) << "[ScreenshotTool] DOM.getDocument 응답 오류";
              base::DictValue err;
              err.Set("error", "DOM.getDocument 실패");
              std::move(callback).Run(base::Value(std::move(err)));
              return;
            }

            const base::DictValue* root =
                get_doc_response.GetDict().FindDict("root");
            if (!root) {
              LOG(ERROR) << "[ScreenshotTool] root 노드 없음";
              base::DictValue err;
              err.Set("error", "DOM 루트 노드를 찾을 수 없음");
              std::move(callback).Run(base::Value(std::move(err)));
              return;
            }

            std::optional<int> root_node_id = root->FindInt("nodeId");
            if (!root_node_id) {
              LOG(ERROR) << "[ScreenshotTool] 루트 nodeId 없음";
              base::DictValue err;
              err.Set("error", "루트 nodeId 추출 실패");
              std::move(callback).Run(base::Value(std::move(err)));
              return;
            }

            // 2단계: DOM.querySelector로 selector에 해당하는 nodeId 취득
            base::DictValue qs_params;
            qs_params.Set("nodeId", *root_node_id);
            qs_params.Set("selector", selector);

            // McpSession은 raw pointer이므로 캡처 시 수명 주의
            session->SendCdpCommand(
                "DOM.querySelector", std::move(qs_params),
                base::BindOnce(
                    // OnQuerySelectorResponse 바인딩
                    // (WeakPtr 대신 직접 session pointer 사용)
                    [](const std::string& fmt, McpSession* sess,
                       base::OnceCallback<void(base::Value)> cb,
                       base::Value qs_response) {
                      // querySelector 결과 처리: nodeId → getBoxModel 호출
                      if (!qs_response.is_dict()) {
                        LOG(ERROR)
                            << "[ScreenshotTool] DOM.querySelector 응답 오류";
                        base::DictValue err;
                        err.Set("error", "DOM.querySelector 실패");
                        std::move(cb).Run(base::Value(std::move(err)));
                        return;
                      }

                      std::optional<int> node_id =
                          qs_response.GetDict().FindInt("nodeId");
                      if (!node_id || *node_id == 0) {
                        LOG(WARNING)
                            << "[ScreenshotTool] selector에 해당하는 요소 없음";
                        base::DictValue err;
                        err.Set("error", "해당 selector의 요소를 찾을 수 없음");
                        std::move(cb).Run(base::Value(std::move(err)));
                        return;
                      }

                      // 3단계: DOM.getBoxModel로 요소의 위치/크기 취득
                      base::DictValue bm_params;
                      bm_params.Set("nodeId", *node_id);

                      sess->SendCdpCommand(
                          "DOM.getBoxModel", std::move(bm_params),
                          base::BindOnce(
                              [](const std::string& image_format,
                                 McpSession* s,
                                 base::OnceCallback<void(base::Value)> done,
                                 base::Value bm_response) {
                                // getBoxModel 응답에서 border 박스 좌표 추출
                                // border 배열: [x1,y1, x2,y2, x3,y3, x4,y4]
                                // (시계방향 4 꼭짓점)
                                if (!bm_response.is_dict()) {
                                  LOG(ERROR) << "[ScreenshotTool] "
                                                "DOM.getBoxModel 응답 오류";
                                  base::DictValue err;
                                  err.Set("error", "DOM.getBoxModel 실패");
                                  std::move(done).Run(
                                      base::Value(std::move(err)));
                                  return;
                                }

                                const base::DictValue* model =
                                    bm_response.GetDict().FindDict("model");
                                if (!model) {
                                  base::DictValue err;
                                  err.Set("error", "boxModel 데이터 없음");
                                  std::move(done).Run(
                                      base::Value(std::move(err)));
                                  return;
                                }

                                // border 배열: 8개 값 (4 꼭짓점 x,y)
                                const base::ListValue* border =
                                    model->FindList("border");
                                if (!border || border->size() < 8) {
                                  base::DictValue err;
                                  err.Set("error", "border 좌표 데이터 부족");
                                  std::move(done).Run(
                                      base::Value(std::move(err)));
                                  return;
                                }

                                // 좌상단 (x1,y1)과 우하단 (x3,y3)으로 clip 계산
                                double x1 = (*border)[0].GetDouble();
                                double y1 = (*border)[1].GetDouble();
                                double x3 = (*border)[4].GetDouble();
                                double y3 = (*border)[5].GetDouble();
                                double width = x3 - x1;
                                double height = y3 - y1;

                                LOG(INFO)
                                    << "[ScreenshotTool] 요소 clip: x=" << x1
                                    << " y=" << y1 << " w=" << width
                                    << " h=" << height;

                                // 4단계: clip 영역으로 Page.captureScreenshot
                                base::DictValue clip;
                                clip.Set("x", x1);
                                clip.Set("y", y1);
                                clip.Set("width", width);
                                clip.Set("height", height);
                                clip.Set("scale", 1.0);

                                base::DictValue cap_params;
                                cap_params.Set("format", image_format);
                                cap_params.Set("clip", std::move(clip));
                                cap_params.Set("captureBeyondViewport", true);

                                s->SendCdpCommand(
                                    "Page.captureScreenshot",
                                    std::move(cap_params),
                                    base::BindOnce(
                                        [](base::OnceCallback<void(
                                               base::Value)> final_cb,
                                           base::Value cap_response) {
                                          // 최종 스크린샷 응답 처리
                                          if (!cap_response.is_dict()) {
                                            base::DictValue err;
                                            err.Set("error",
                                                    "captureScreenshot 실패");
                                            std::move(final_cb).Run(
                                                base::Value(std::move(err)));
                                            return;
                                          }
                                          const std::string* data =
                                              cap_response.GetDict().FindString(
                                                  "data");
                                          base::DictValue result;
                                          if (data) {
                                            result.Set("data", *data);
                                            result.Set("success", true);
                                          } else {
                                            result.Set("error",
                                                       "이미지 데이터 없음");
                                            result.Set("success", false);
                                          }
                                          std::move(final_cb).Run(
                                              base::Value(std::move(result)));
                                        },
                                        std::move(done)));
                              },
                              fmt, sess, std::move(cb)));
                    },
                    format, session, std::move(callback)));
          },
          selector, format, session, std::move(callback)));
}

void ScreenshotTool::OnCaptureScreenshotResponse(
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  // CDP Page.captureScreenshot 응답 처리.
  // 성공 시 응답에는 base64 인코딩된 이미지 데이터("data" 필드)가 포함된다.
  if (!response.is_dict()) {
    LOG(ERROR) << "[ScreenshotTool] captureScreenshot 응답 형식 오류";
    base::DictValue err;
    err.Set("error", "captureScreenshot 응답 형식 오류");
    err.Set("success", false);
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::DictValue& dict = response.GetDict();

  // CDP 오류 응답 확인
  const base::DictValue* error_dict = dict.FindDict("error");
  if (error_dict) {
    const std::string* msg = error_dict->FindString("message");
    std::string error_msg = msg ? *msg : "알 수 없는 CDP 오류";
    LOG(ERROR) << "[ScreenshotTool] CDP 오류: " << error_msg;

    base::DictValue result;
    result.Set("success", false);
    result.Set("error", error_msg);
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // 성공 응답에서 base64 이미지 데이터 추출
  const std::string* data = dict.FindString("data");
  if (!data || data->empty()) {
    LOG(WARNING) << "[ScreenshotTool] 이미지 데이터 비어있음";
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "이미지 데이터 없음");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[ScreenshotTool] 스크린샷 성공, data 길이=" << data->size();
  base::DictValue result;
  result.Set("success", true);
  result.Set("data", *data);  // base64 인코딩된 이미지
  std::move(callback).Run(base::Value(std::move(result)));
}

}  // namespace mcp
