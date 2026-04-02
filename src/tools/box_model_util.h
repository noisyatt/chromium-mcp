// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_BOX_MODEL_UTIL_H_
#define CHROME_BROWSER_MCP_TOOLS_BOX_MODEL_UTIL_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace mcp {

// CDP 응답에 "error" 키가 있는지 확인한다.
bool HasCdpError(const base::Value& response);

// CDP 응답에서 에러 메시지를 추출한다.
std::string ExtractCdpErrorMessage(const base::Value& response);

// MCP 성공 응답 Value 생성
base::Value MakeSuccessResult(const std::string& message);

// MCP 에러 응답 Value 생성
base::Value MakeErrorResult(const std::string& message);

// JSON Dict를 MCP 결과 Value로 감싸서 반환
base::Value MakeJsonResult(base::DictValue result_dict);

// base64 이미지를 MCP 이미지 응답으로 감싸서 반환
base::Value MakeImageResult(const std::string& data,
                            const std::string& mime_type = "image/png");

// DOM.getDocument 응답에서 rootNodeId를 추출한다.
int ExtractRootNodeId(const base::Value& response);

// DOM.querySelector 응답에서 nodeId를 추출한다.
int ExtractNodeId(const base::Value& response);

// DOM.getBoxModel 응답에서 content quad 중심 좌표를 계산한다.
bool ExtractBoxModelCenter(const base::Value& response,
                           double* out_x,
                           double* out_y);

// DOM.getBoxModel 응답에서 content quad의 bounding rect를 추출한다.
// out_rect에 {x, y, width, height} 키를 설정한다.
// content quad: [x1,y1, x2,y2, x3,y3, x4,y4] — 시계방향 4꼭짓점
bool ExtractBoundingBox(const base::Value& response,
                        base::DictValue* out_rect);

// CDP 에러 처리 헬퍼: 에러가 있으면 callback을 호출하고 true를 반환한다.
// NOLINTNEXTLINE(runtime/references)
bool HandleCdpError(const base::Value& response,
                    const std::string& step_name,
                    base::OnceCallback<void(base::Value)>& callback);

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_BOX_MODEL_UTIL_H_
