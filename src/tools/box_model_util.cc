// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/mcp/tools/box_model_util.h"

#include <optional>
#include <string>
#include <utility>

#include "base/json/json_writer.h"

#include "base/logging.h"
#include "base/values.h"

namespace mcp {

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

// JSON Dict를 MCP 결과 Value로 감싸서 반환
// MCP 프로토콜에서 text 필드는 문자열이어야 하므로 Dict를 JSON 문자열로 직렬화
base::Value MakeJsonResult(base::DictValue result_dict) {
  std::string json_str;
  base::JSONWriter::Write(base::Value(std::move(result_dict)), &json_str);

  base::DictValue result;
  base::ListValue content;
  base::DictValue item;
  item.Set("type", "text");
  item.Set("text", json_str);
  content.Append(std::move(item));
  result.Set("content", std::move(content));
  result.Set("isError", false);
  return base::Value(std::move(result));
}

// CDP 응답에 "error" 키가 있는지 확인한다.
bool HasCdpError(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) {
    return true;
  }
  return dict->Find("error") != nullptr;
}

// CDP 응답에서 에러 메시지를 추출한다.
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

// DOM.getDocument 응답에서 rootNodeId를 추출한다.
// SendCdpCommand 편의 오버로드가 "result"를 벗겨서 전달하므로 양쪽 모두 지원.
int ExtractRootNodeId(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) return -1;
  // 편의 오버로드: dict = {"root": {...}} (result 이미 벗겨짐)
  // raw 오버로드: dict = {"result": {"root": {...}}}
  const base::DictValue* result = dict->FindDict("result");
  if (!result) result = dict;
  const base::DictValue* root = result->FindDict("root");
  if (!root) return -1;
  std::optional<int> node_id = root->FindInt("nodeId");
  return node_id.value_or(-1);
}

// DOM.querySelector 응답에서 nodeId를 추출한다.
int ExtractNodeId(const base::Value& response) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) return -1;
  const base::DictValue* result = dict->FindDict("result");
  if (!result) result = dict;
  std::optional<int> node_id = result->FindInt("nodeId");
  return node_id.value_or(-1);
}

// DOM.getBoxModel 응답에서 content quad 중심 좌표를 계산한다.
bool ExtractBoxModelCenter(const base::Value& response,
                           double* out_x,
                           double* out_y) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) return false;
  const base::DictValue* result = dict->FindDict("result");
  if (!result) result = dict;
  const base::DictValue* model = result->FindDict("model");
  if (!model) {
    return false;
  }
  const base::ListValue* content = model->FindList("content");
  if (!content || content->size() < 8) {
    return false;
  }
  // 4개 꼭짓점 좌표의 평균으로 중심점 계산
  double sum_x = 0.0, sum_y = 0.0;
  for (size_t i = 0; i < 8; i += 2) {
    sum_x += (*content)[i].GetIfDouble().value_or(0.0);
    sum_y += (*content)[i + 1].GetIfDouble().value_or(0.0);
  }
  *out_x = sum_x / 4.0;
  *out_y = sum_y / 4.0;
  return true;
}

// DOM.getBoxModel 응답에서 content quad의 bounding rect를 추출한다.
// content quad: [x1,y1, x2,y2, x3,y3, x4,y4] — 시계방향 4꼭짓점
// x = content[0], y = content[1]
// width = content[4] - content[0], height = content[5] - content[1]
bool ExtractBoundingBox(const base::Value& response,
                        base::DictValue* out_rect) {
  const base::DictValue* dict = response.GetIfDict();
  if (!dict) return false;
  const base::DictValue* result = dict->FindDict("result");
  if (!result) result = dict;
  const base::DictValue* model = result->FindDict("model");
  if (!model) {
    return false;
  }
  const base::ListValue* content = model->FindList("content");
  if (!content || content->size() < 8) {
    return false;
  }
  double x = (*content)[0].GetIfDouble().value_or(0.0);
  double y = (*content)[1].GetIfDouble().value_or(0.0);
  double x3 = (*content)[4].GetIfDouble().value_or(0.0);
  double y3 = (*content)[5].GetIfDouble().value_or(0.0);
  double width = x3 - x;
  double height = y3 - y;
  out_rect->Set("x", x);
  out_rect->Set("y", y);
  out_rect->Set("width", width);
  out_rect->Set("height", height);
  return true;
}

// CDP 에러 처리 헬퍼: 에러가 있으면 callback을 호출하고 true를 반환한다.
// NOLINTNEXTLINE(runtime/references)
bool HandleCdpError(const base::Value& response,
                    const std::string& step_name,
                    base::OnceCallback<void(base::Value)>& callback) {
  if (!HasCdpError(response)) {
    return false;
  }
  std::string error_msg = ExtractCdpErrorMessage(response);
  LOG(ERROR) << "[MCP] CDP 에러 (" << step_name << "): " << error_msg;
  std::move(callback).Run(MakeErrorResult(step_name + " 실패: " + error_msg));
  return true;
}

}  // namespace mcp
