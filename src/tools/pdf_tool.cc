// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/pdf_tool.h"

#include <utility>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_session.h"
#include "chrome/browser/mcp/tools/box_model_util.h"

namespace mcp {

// -------------------------------------------------------------------------
// 기본값 상수 (인치 단위)
// -------------------------------------------------------------------------
constexpr double kDefaultPaperWidth  = 8.5;   // Letter 폭
constexpr double kDefaultPaperHeight = 11.0;  // Letter 높이
constexpr double kDefaultScale       = 1.0;   // 100% 스케일
constexpr double kDefaultMargin      = 0.4;   // 기본 여백

PdfTool::PdfTool() = default;
PdfTool::~PdfTool() = default;

std::string PdfTool::name() const {
  return "pdf";
}

std::string PdfTool::description() const {
  return "현재 페이지를 PDF로 저장";
}

base::DictValue PdfTool::input_schema() const {
  base::DictValue schema;
  schema.Set("type", "object");

  base::DictValue properties;

  // savePath: 저장 경로. 없으면 base64 데이터를 응답으로 반환.
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "PDF 저장 경로 (예: \"/tmp/page.pdf\"). "
             "생략하면 base64 인코딩 데이터를 응답으로 반환.");
    properties.Set("savePath", std::move(prop));
  }

  // landscape: 가로 방향 출력 여부 (기본값: false = 세로)
  {
    base::DictValue prop;
    prop.Set("type", "boolean");
    prop.Set("description", "가로 방향 출력 여부 (기본값: false)");
    prop.Set("default", false);
    properties.Set("landscape", std::move(prop));
  }

  // printBackground: 배경색/배경 이미지 포함 여부 (기본값: true)
  {
    base::DictValue prop;
    prop.Set("type", "boolean");
    prop.Set("description", "배경색 및 배경 이미지 포함 여부 (기본값: true)");
    prop.Set("default", true);
    properties.Set("printBackground", std::move(prop));
  }

  // scale: 페이지 스케일 (0.1 ~ 2.0, 기본값: 1.0)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description",
             "페이지 출력 스케일 (기본값: 1.0, 범위: 0.1~2.0)");
    prop.Set("default", kDefaultScale);
    prop.Set("minimum", 0.1);
    prop.Set("maximum", 2.0);
    properties.Set("scale", std::move(prop));
  }

  // paperWidth: 용지 너비 인치 (기본값: 8.5)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description",
             "용지 너비 인치 단위 (기본값: 8.5 = US Letter)");
    prop.Set("default", kDefaultPaperWidth);
    properties.Set("paperWidth", std::move(prop));
  }

  // paperHeight: 용지 높이 인치 (기본값: 11.0)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description",
             "용지 높이 인치 단위 (기본값: 11.0 = US Letter)");
    prop.Set("default", kDefaultPaperHeight);
    properties.Set("paperHeight", std::move(prop));
  }

  // marginTop: 위쪽 여백 인치 (기본값: 0.4)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description", "위쪽 여백 인치 (기본값: 0.4)");
    prop.Set("default", kDefaultMargin);
    properties.Set("marginTop", std::move(prop));
  }

  // marginRight: 오른쪽 여백 인치 (기본값: 0.4)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description", "오른쪽 여백 인치 (기본값: 0.4)");
    prop.Set("default", kDefaultMargin);
    properties.Set("marginRight", std::move(prop));
  }

  // marginBottom: 아래쪽 여백 인치 (기본값: 0.4)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description", "아래쪽 여백 인치 (기본값: 0.4)");
    prop.Set("default", kDefaultMargin);
    properties.Set("marginBottom", std::move(prop));
  }

  // marginLeft: 왼쪽 여백 인치 (기본값: 0.4)
  {
    base::DictValue prop;
    prop.Set("type", "number");
    prop.Set("description", "왼쪽 여백 인치 (기본값: 0.4)");
    prop.Set("default", kDefaultMargin);
    properties.Set("marginLeft", std::move(prop));
  }

  // pageRanges: 인쇄할 페이지 범위 ("1-5, 8" 형식, 생략 시 전체)
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "인쇄할 페이지 범위 (예: \"1-5, 8\"). 생략 시 전체 페이지.");
    properties.Set("pageRanges", std::move(prop));
  }

  // headerTemplate: 머리글 HTML 템플릿
  // 사용 가능한 CSS 클래스: date, title, url, pageNumber, totalPages
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "머리글 HTML 템플릿. CSS 클래스: date, title, url, "
             "pageNumber, totalPages. displayHeaderFooter=true 시 활성화.");
    properties.Set("headerTemplate", std::move(prop));
  }

  // footerTemplate: 바닥글 HTML 템플릿
  {
    base::DictValue prop;
    prop.Set("type", "string");
    prop.Set("description",
             "바닥글 HTML 템플릿. CSS 클래스: date, title, url, "
             "pageNumber, totalPages. displayHeaderFooter=true 시 활성화.");
    properties.Set("footerTemplate", std::move(prop));
  }

  // displayHeaderFooter: 머리글/바닥글 표시 여부 (기본값: false)
  {
    base::DictValue prop;
    prop.Set("type", "boolean");
    prop.Set("description",
             "머리글/바닥글 표시 여부 (기본값: false). "
             "true 시 headerTemplate/footerTemplate 적용.");
    prop.Set("default", false);
    properties.Set("displayHeaderFooter", std::move(prop));
  }

  // preferCSSPageSize: CSS @page 크기를 paperWidth/paperHeight보다 우선 적용
  {
    base::DictValue prop;
    prop.Set("type", "boolean");
    prop.Set("description",
             "CSS @page 크기를 paperWidth/paperHeight보다 우선 적용 여부 "
             "(기본값: false)");
    prop.Set("default", false);
    properties.Set("preferCSSPageSize", std::move(prop));
  }

  schema.Set("properties", std::move(properties));
  return schema;
}

void PdfTool::Execute(const base::DictValue& arguments,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback) {
  // 저장 경로 추출 (없으면 빈 문자열 → base64 반환)
  const std::string* save_path_ptr = arguments.FindString("savePath");
  std::string save_path = save_path_ptr ? *save_path_ptr : "";

  // -----------------------------------------------------------------------
  // CDP Page.printToPDF 파라미터 구성
  // -----------------------------------------------------------------------
  base::DictValue params;

  // landscape: 가로/세로 방향 (기본값: false)
  params.Set("landscape", arguments.FindBool("landscape").value_or(false));

  // printBackground: 배경 포함 여부 (기본값: true)
  params.Set("printBackground",
             arguments.FindBool("printBackground").value_or(true));

  // scale: 출력 스케일, 범위 [0.1, 2.0] 클램핑
  double scale = arguments.FindDouble("scale").value_or(kDefaultScale);
  scale = std::max(0.1, std::min(2.0, scale));
  params.Set("scale", scale);

  // 용지 크기
  params.Set("paperWidth",
             arguments.FindDouble("paperWidth").value_or(kDefaultPaperWidth));
  params.Set("paperHeight",
             arguments.FindDouble("paperHeight").value_or(kDefaultPaperHeight));

  // 여백 (위/오른쪽/아래/왼쪽)
  params.Set("marginTop",
             arguments.FindDouble("marginTop").value_or(kDefaultMargin));
  params.Set("marginRight",
             arguments.FindDouble("marginRight").value_or(kDefaultMargin));
  params.Set("marginBottom",
             arguments.FindDouble("marginBottom").value_or(kDefaultMargin));
  params.Set("marginLeft",
             arguments.FindDouble("marginLeft").value_or(kDefaultMargin));

  // 페이지 범위 (지정된 경우에만 설정)
  const std::string* page_ranges = arguments.FindString("pageRanges");
  if (page_ranges && !page_ranges->empty()) {
    params.Set("pageRanges", *page_ranges);
  }

  // 머리글/바닥글 표시 여부
  bool display_hf =
      arguments.FindBool("displayHeaderFooter").value_or(false);
  params.Set("displayHeaderFooter", display_hf);

  // 머리글 HTML 템플릿 (displayHeaderFooter=true 시 사용됨)
  const std::string* header_tmpl = arguments.FindString("headerTemplate");
  if (header_tmpl && !header_tmpl->empty()) {
    params.Set("headerTemplate", *header_tmpl);
  }

  // 바닥글 HTML 템플릿 (displayHeaderFooter=true 시 사용됨)
  const std::string* footer_tmpl = arguments.FindString("footerTemplate");
  if (footer_tmpl && !footer_tmpl->empty()) {
    params.Set("footerTemplate", *footer_tmpl);
  }

  // CSS @page 크기 우선 적용 여부
  params.Set("preferCSSPageSize",
             arguments.FindBool("preferCSSPageSize").value_or(false));

  // PDF 데이터 전송 방식: base64 문자열로 반환
  params.Set("transferMode", "ReturnAsBase64");

  LOG(INFO) << "[PdfTool] Page.printToPDF 호출, landscape="
            << params.FindBool("landscape").value_or(false)
            << " scale=" << scale
            << " savePath=" << (save_path.empty() ? "(base64 반환)" : save_path);

  session->SendCdpCommand(
      "Page.printToPDF", std::move(params),
      base::BindOnce(&PdfTool::OnPrintToPdfResponse,
                     weak_factory_.GetWeakPtr(), save_path,
                     std::move(callback)));
}

void PdfTool::OnPrintToPdfResponse(
    const std::string& save_path,
    base::OnceCallback<void(base::Value)> callback,
    base::Value response) {
  if (!response.is_dict()) {
    LOG(ERROR) << "[PdfTool] Page.printToPDF 응답 형식 오류";
    base::DictValue err;
    err.Set("error", "Page.printToPDF 응답 형식 오류");
    std::move(callback).Run(base::Value(std::move(err)));
    return;
  }

  const base::DictValue& dict = response.GetDict();

  // CDP 오류 응답 확인
  const base::DictValue* err_dict = dict.FindDict("error");
  if (err_dict) {
    const std::string* msg = err_dict->FindString("message");
    std::string err_msg = msg ? *msg : "PDF 변환 실패";
    LOG(ERROR) << "[PdfTool] CDP 오류: " << err_msg;
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", err_msg);
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  // PDF base64 데이터 추출
  // Page.printToPDF 응답: {"result": {"data": "<base64>", "stream": ...}}
  const base::DictValue* result_dict = dict.FindDict("result");
  const std::string* b64_data = nullptr;
  if (result_dict) {
    b64_data = result_dict->FindString("data");
  }
  // transferMode=ReturnAsBase64 시 최상위 "data" 키로 오는 구현도 있음
  if (!b64_data) {
    b64_data = dict.FindString("data");
  }

  if (!b64_data || b64_data->empty()) {
    LOG(WARNING) << "[PdfTool] PDF 데이터가 비어있음";
    base::DictValue result;
    result.Set("success", false);
    result.Set("error", "PDF 데이터가 비어있습니다");
    std::move(callback).Run(base::Value(std::move(result)));
    return;
  }

  LOG(INFO) << "[PdfTool] PDF 변환 성공, base64 길이=" << b64_data->size();

  // -------------------------------------------------------------------------
  // savePath가 지정된 경우: base64를 디코딩하여 파일로 저장
  // -------------------------------------------------------------------------
  if (!save_path.empty()) {
    // base64 디코딩
    std::string pdf_bytes;
    if (!base::Base64Decode(*b64_data, &pdf_bytes)) {
      LOG(ERROR) << "[PdfTool] base64 디코딩 실패";
      base::DictValue result;
      result.Set("success", false);
      result.Set("error", "PDF 데이터 디코딩에 실패했습니다");
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }

    // base::WriteFile 로 파일 저장
    // WriteFile은 파일 크기(bytes)를 반환하며, -1이면 실패
    base::FilePath file_path(save_path);
    bool write_ok = base::WriteFile(file_path, pdf_bytes);
    if (!write_ok) {
      LOG(ERROR) << "[PdfTool] 파일 저장 실패: " << save_path;
      base::DictValue result;
      result.Set("success", false);
      result.Set("error", "파일 저장에 실패했습니다: " + save_path);
      std::move(callback).Run(base::Value(std::move(result)));
      return;
    }

    LOG(INFO) << "[PdfTool] PDF 파일 저장 완료: " << save_path
              << " (" << pdf_bytes.size() << " bytes)";

    base::DictValue result;
    result.Set("success", true);
    result.Set("savePath", save_path);
    result.Set("size", static_cast<int>(pdf_bytes.size()));
    result.Set("mimeType", "application/pdf");
    std::move(callback).Run(MakeJsonResult(std::move(result)));
    return;
  }

  // -------------------------------------------------------------------------
  // savePath 미지정: base64 데이터를 그대로 응답으로 반환
  // -------------------------------------------------------------------------
  base::DictValue result;
  result.Set("success", true);
  result.Set("data", *b64_data);  // base64 인코딩된 PDF
  result.Set("mimeType", "application/pdf");
  std::move(callback).Run(MakeJsonResult(std::move(result)));
}

}  // namespace mcp
