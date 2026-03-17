// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_MCP_TOOLS_DOWNLOAD_TOOL_H_
#define CHROME_BROWSER_MCP_TOOLS_DOWNLOAD_TOOL_H_

#include <string>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/mcp/mcp_tool_registry.h"

namespace mcp {

// 파일 다운로드를 관리하는 MCP 도구.
//
// CDP Page.setDownloadBehavior / Browser.setDownloadBehavior 를 사용하여
// 다운로드 허용 여부와 저장 경로를 제어하고, Runtime.evaluate로 다운로드를
// 트리거하거나 Page.downloadWillBegin / Page.downloadProgress 이벤트를 통해
// 진행 상태를 추적한다.
//
// 지원 액션:
//   - start    : 지정 URL에서 파일 다운로드 시작
//   - list     : 현재 세션의 다운로드 목록 조회
//   - cancel   : 진행 중인 다운로드 취소
//   - setPath  : 전역 다운로드 저장 경로 변경
//
// CDP 명령:
//   - Page.setDownloadBehavior  : 다운로드 허용 및 저장 경로 설정
//   - Browser.setDownloadBehavior : 전역 다운로드 동작 설정
//   - Runtime.evaluate          : a.click() 패턴으로 다운로드 트리거
//   이벤트 감시:
//   - Page.downloadWillBegin    : 다운로드 시작 이벤트
//   - Page.downloadProgress     : 다운로드 진행률 이벤트
class DownloadTool : public McpTool {
 public:
  DownloadTool() = default;
  ~DownloadTool() override = default;

  // McpTool 인터페이스 구현
  std::string name() const override;
  std::string description() const override;
  base::Value::Dict input_schema() const override;
  void Execute(const base::Value::Dict& arguments,
               McpSession* session,
               base::OnceCallback<void(base::Value)> callback) override;

 private:
  // -----------------------------------------------------------------------
  // 액션별 처리 메서드
  // -----------------------------------------------------------------------

  // action="start": 지정된 URL에서 다운로드를 시작한다.
  // |url|      : 다운로드할 파일 URL
  // |save_path|: 저장 경로 (빈 문자열이면 기본 다운로드 폴더 사용)
  void ExecuteStart(const std::string& url,
                    const std::string& save_path,
                    McpSession* session,
                    base::OnceCallback<void(base::Value)> callback);

  // action="list": 현재 세션의 다운로드 목록을 조회한다.
  void ExecuteList(McpSession* session,
                   base::OnceCallback<void(base::Value)> callback);

  // action="cancel": 진행 중인 다운로드를 취소한다.
  // |download_id|: 취소할 다운로드 ID
  void ExecuteCancel(int download_id,
                     McpSession* session,
                     base::OnceCallback<void(base::Value)> callback);

  // action="setPath": 전역 다운로드 저장 경로를 변경한다.
  // |path|: 새 저장 경로 (디렉토리)
  void ExecuteSetPath(const std::string& path,
                      McpSession* session,
                      base::OnceCallback<void(base::Value)> callback);

  // -----------------------------------------------------------------------
  // CDP 응답 콜백
  // -----------------------------------------------------------------------

  // Page.setDownloadBehavior 응답 후 Runtime.evaluate로 다운로드 트리거.
  // 설정 완료 여부와 관계없이 a.click() 패턴으로 다운로드를 시작한다.
  void OnDownloadBehaviorSet(const std::string& url,
                             const std::string& save_path,
                             base::OnceCallback<void(base::Value)> callback,
                             McpSession* session,
                             base::Value response);

  // Runtime.evaluate 응답 처리 (다운로드 트리거 결과)
  void OnDownloadTriggered(base::OnceCallback<void(base::Value)> callback,
                           base::Value response);

  // 범용 CDP 응답 처리 (list/cancel/setPath 공통)
  void OnCdpActionResult(base::OnceCallback<void(base::Value)> callback,
                         base::Value response);

  base::WeakPtrFactory<DownloadTool> weak_factory_{this};
};

}  // namespace mcp

#endif  // CHROME_BROWSER_MCP_TOOLS_DOWNLOAD_TOOL_H_
