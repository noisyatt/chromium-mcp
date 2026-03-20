# Chromium-MCP 통합 테스트 시나리오

## 전제 조건
- Chromium 프로세스 완전 종료 (`pkill -9 -f Chromium`)
- 데몬 완전 종료 (`launchctl unload ~/Library/LaunchAgents/com.chromium-mcp.daemon.plist`)
- 소켓 파일 제거 (`rm -f /tmp/.chromium-mcp*.sock`)
- PID 파일 제거 (`rm -f /tmp/chromium-mcp-daemon.pid`)

---

## Stage 1: 콜드 스타트 (Chromium 없음 → 자동 실행)

### 1-1. 데몬 자동 기동
```
조건: 데몬 미실행, Chromium 미실행
행동: Claude CLI에서 chromium-mcp 도구 호출
기대:
  ✅ client.py가 데몬 자동 시작
  ✅ 데몬이 Chromium 자동 실행
  ✅ MCP 소켓 연결 성공
  ✅ 30초 이내 응답 반환
검증: /tmp/chromium-mcp-daemon.log에 "STOPPED → STARTING → READY" 로그
```

### 1-2. browser_info 호출 (세션 불필요 도구)
```
Claude 명령: "현재 브라우저 상태를 알려줘"
MCP 도구: browser_info
기대:
  ✅ Chromium 버전 반환
  ✅ 탭 수 반환
  ✅ 세션 없이도 동작 (requires_session=false)
```

### 1-3. tabs list (세션 불필요 도구)
```
Claude 명령: "열려있는 탭 목록을 보여줘"
MCP 도구: tabs (action=list)
기대:
  ✅ 기본 탭 (chrome://newtab) 1개 반환
  ✅ tabId, url, title 포함
```

---

## Stage 2: 기본 브라우징 (네비게이션 + 스크린샷)

### 2-1. 새 탭에서 URL 이동
```
Claude 명령: "구글에 접속해줘"
MCP 도구: navigate (url=https://www.google.com)
기대:
  ✅ 자동 세션 연결 (assigned_tab 배정)
  ✅ 구글 페이지 로드 성공
  ✅ frameId, loaderId 반환
검증: tabs list로 url이 https://www.google.com/ 인지 확인
```

### 2-2. 스크린샷 촬영
```
Claude 명령: "현재 화면을 스크린샷 찍어줘"
MCP 도구: screenshot
기대:
  ✅ base64 PNG 데이터 반환
  ✅ 이미지가 깨지지 않음
  ✅ 구글 로고/검색창이 보임
```

### 2-3. 페이지 콘텐츠 확인
```
Claude 명령: "페이지 HTML을 보여줘"
MCP 도구: page_content (format=text 또는 html)
기대:
  ✅ "Google" 텍스트 포함
  ✅ 검색 입력 필드 존재
```

---

## Stage 3: 폼 입력 + 키보드 (검색 시나리오)

### 3-1. 검색창 클릭
```
Claude 명령: "검색창을 클릭해줘"
MCP 도구: click (selector="textarea[name='q']" 또는 "input[name='q']")
기대:
  ✅ 검색창에 포커스
  ✅ 클릭 좌표 반환
```

### 3-2. 텍스트 입력 (영문)
```
Claude 명령: "chromium mcp 라고 입력해줘"
MCP 도구: keyboard (action=type, text="chromium mcp")
기대:
  ✅ 검색창에 "chromium mcp" 입력됨
  ✅ 자동완성 드롭다운 표시
검증: screenshot으로 입력 내용 시각적 확인
```

### 3-3. 텍스트 입력 (한글)
```
Claude 명령: "검색어를 '크로미움 테스트'로 바꿔줘"
MCP 도구 순서:
  1. keyboard (action=shortcut, key="a", modifiers=["Meta"]) — 전체 선택
  2. keyboard (action=type, text="크로미움 테스트")
기대:
  ✅ 한글이 깨지지 않고 정확하게 입력됨
  ✅ 검색창에 "크로미움 테스트" 표시
검증: screenshot + page_content로 이중 확인
```

### 3-4. Enter 키로 검색 실행
```
Claude 명령: "엔터를 눌러서 검색해줘"
MCP 도구: keyboard (action=press, key="Enter")
기대:
  ✅ 검색 결과 페이지로 이동
  ✅ URL이 google.com/search?q=... 로 변경
검증: navigate 후 tabs list로 URL 확인
```

---

## Stage 4: 요소 클릭 정확도

### 4-1. 검색 결과 링크 클릭
```
조건: 구글 검색 결과 페이지
Claude 명령: "첫 번째 검색 결과를 클릭해줘"
MCP 도구: click (selector="h3" 또는 CSS 선택자)
기대:
  ✅ 첫 번째 검색 결과 페이지로 이동
  ✅ URL 변경 확인
```

### 4-2. 버튼 클릭 테스트 (GitHub)
```
Claude 명령: "github.com에 접속해서 Sign in 버튼을 클릭해줘"
MCP 도구 순서:
  1. navigate (url=https://github.com)
  2. screenshot — 페이지 로드 확인
  3. click (selector="a[href='/login']" 또는 텍스트 기반)
기대:
  ✅ 로그인 페이지로 이동
  ✅ URL이 github.com/login 으로 변경
검증: screenshot으로 로그인 폼 확인
```

### 4-3. 특정 좌표 클릭 (element_info 활용)
```
Claude 명령: "페이지에서 '검색' 버튼의 위치를 찾고 클릭해줘"
MCP 도구 순서:
  1. element_info (selector="button[type='submit']")
  2. 반환된 boundingBox 좌표로 click
기대:
  ✅ element_info가 x, y, width, height 반환
  ✅ 해당 좌표 클릭 성공
```

---

## Stage 5: 탭 관리

### 5-1. 새 탭 열기
```
Claude 명령: "새 탭에서 네이버를 열어줘"
MCP 도구: tabs (action=new, url=https://www.naver.com)
기대:
  ✅ 새 탭 생성
  ✅ 네이버 페이지 로드
  ✅ 브라우저 창이 앞으로 올라옴 (Activate)
  ✅ assigned_tab이 새 탭으로 배정
```

### 5-2. 탭 전환
```
Claude 명령: "아까 열었던 구글 탭으로 돌아가줘"
MCP 도구 순서:
  1. tabs (action=list) — 탭 목록에서 구글 tabId 확인
  2. tabs (action=select, tabId=<구글탭>)
기대:
  ✅ 구글 탭이 활성화
  ✅ 해당 클라이언트의 assigned_tab 변경
```

### 5-3. 기존 유저 탭 접근
```
조건: 사용자가 수동으로 열어둔 탭이 있는 상태
Claude 명령: "열려있는 탭 중에 YouTube가 있으면 거기로 가줘"
MCP 도구 순서:
  1. tabs (action=list) — 전체 탭 조회
  2. 결과에서 YouTube 탭 찾기
  3. tabs (action=select, tabId=<유튜브탭>)
기대:
  ✅ 사용자가 수동으로 연 탭도 목록에 포함
  ✅ 해당 탭으로 전환 가능
  ✅ 전환 후 스크린샷/조작 가능
```

### 5-4. 탭 닫기
```
Claude 명령: "네이버 탭을 닫아줘"
MCP 도구: tabs (action=close, tabId=<네이버탭>)
기대:
  ✅ 탭 닫힘
  ✅ assigned_tab이 해당 탭이었던 클라이언트는 자동 null 처리
  ✅ 다음 도구 호출 시 자동 재배정
```

---

## Stage 6: 복합 업무 시나리오

### 6-1. 웹사이트 정보 수집 (스크래핑)
```
Claude 명령: "Hacker News의 오늘 톱 5 기사 제목과 URL을 알려줘"
MCP 도구 순서:
  1. navigate (url=https://news.ycombinator.com)
  2. page_content (format=text)
  3. Claude가 파싱하여 결과 반환
기대:
  ✅ 네비게이션 성공
  ✅ 페이지 텍스트에서 기사 제목 추출 가능
  ✅ 결과가 정확하고 읽을 수 있음
```

### 6-2. 폼 작성 + 제출
```
Claude 명령: "httpbin.org/forms/post 에서 폼을 작성해줘"
MCP 도구 순서:
  1. navigate (url=https://httpbin.org/forms/post)
  2. fill (selector="input[name='custname']", value="TestUser")
  3. fill (selector="input[name='custtel']", value="010-1234-5678")
  4. click (selector="button[type='submit']" 또는 submit 버튼)
기대:
  ✅ 각 필드에 값이 입력됨
  ✅ 제출 후 결과 페이지 표시
검증: screenshot으로 제출 결과 확인
```

### 6-3. JavaScript 실행
```
Claude 명령: "현재 페이지의 document.title을 알려줘"
MCP 도구: evaluate (expression="document.title")
기대:
  ✅ 페이지 제목 문자열 반환
  ✅ JavaScript 실행 오류 없음
```

---

## Stage 7: 멀티 클라이언트 동시 접속

> **Python 소켓 스크립트로 자동화** (Claude CLI 2개 동시 실행은 비현실적)

### 7-1. 두 클라이언트 독립 동작
```
방법: Python에서 소켓 2개 동시 연결
순서:
  1. A: initialize → initialized → tabs new (url=https://example.com)
  2. B: initialize → initialized → tabs new (url=https://httpbin.org)
  3. A: evaluate("document.title") → "Example Domain"
  4. B: evaluate("document.title") → 다른 제목
기대:
  ✅ A와 B가 서로 다른 탭에서 독립 동작
  ✅ evaluate 결과가 각각의 탭에 해당
PASS 기준: A의 title ≠ B의 title
```

### 7-2. 탭 목록 공유 확인
```
조건: 7-1 수행 후 (A탭 1개, B탭 1개 + 기본탭)
방법: 양쪽에서 tabs list 호출
기대:
  ✅ 양쪽 모두 동일한 전체 탭 목록 반환
  ✅ tabCount가 동일
PASS 기준: A의 tabCount == B의 tabCount
```

---

## Stage 8: 크래시 복구

### 8-1. Chromium 크래시 후 자동 재시작
```
행동: kill -9 $(pgrep -f Chromium)
대기: 30초
기대:
  ✅ 데몬 로그에 "크래시 감지 → STOPPED → STARTING → READY"
  ✅ Chromium 자동 재시작
  ✅ 소켓 재생성
  ✅ Claude CLI 재연결 후 정상 동작
```

### 8-2. 사용자 수동 종료 (Cmd+Q) 후 재시작 안 함
```
행동: Chromium에서 Cmd+Q (정상 종료)
대기: 30초
기대:
  ✅ 데몬 로그에 "정상 종료 — 재시작 안 함"
  ✅ Chromium 재시작되지 않음
  ✅ Claude CLI에서 다음 도구 호출 시 → 데몬이 Chromium 재시작
```

### 8-3. 데몬 크래시 후 자동 복구
```
행동: kill -9 $(cat /tmp/chromium-mcp-daemon.pid)
기대:
  ✅ launchd가 데몬 자동 재시작
  ✅ 새 데몬이 기존 Chromium에 연결
  ✅ Claude CLI에서 재연결 후 정상 동작
```

---

## Stage 9: 리팩토링 핵심 검증

> 이 Stage는 Phase 1~3 리팩토링의 핵심 변경을 직접 검증한다.
> **반드시 Python 소켓 스크립트로 자동화해야 한다.**

### 9-1. requires_session=false 검증 (Phase 1)
```
조건: 세션이 없는 상태 (HandleInitialized 직후, 탭 미배정)
방법: initialize → initialized 후 즉시 tabs list / browser_info / clipboard read 호출
기대:
  ✅ tabs list: 탭 목록 정상 반환 (세션 없이도)
  ✅ browser_info: 버전/탭수 반환 (세션 없이도)
  ✅ clipboard read: 클립보드 내용 반환 (세션 없이도)
  ❌ navigate: "활성 탭 세션이 없습니다" 에러 반환 (requires_session=true)
검증 명령:
  - tabs list → result.content[0].text에 "tabs" 키 존재
  - navigate → result.isError == true
```

### 9-2. GetSessionForClient 독립 라우팅 (Phase 2)
```
방법: 소켓 2개 동시 연결 (client A, client B)
순서:
  1. A: initialize → initialized
  2. B: initialize → initialized
  3. A: tabs new (url=https://example.com) → tabId=X
  4. B: tabs new (url=https://httpbin.org) → tabId=Y
  5. A: navigate (url=https://example.com/about)  ← A의 탭에서 실행
  6. B: navigate (url=https://httpbin.org/get)     ← B의 탭에서 실행
  7. A: evaluate (expression="location.href")      → example.com/about
  8. B: evaluate (expression="location.href")      → httpbin.org/get
기대:
  ✅ A의 evaluate 결과가 example.com (B의 httpbin이 아님)
  ✅ B의 evaluate 결과가 httpbin.org (A의 example이 아님)
  ✅ 두 클라이언트가 서로의 탭에 간섭하지 않음
PASS 기준: 양쪽 evaluate 결과의 호스트명이 각각 일치
```

### 9-3. DetachFromWebContents 댕글링 방지 (Phase 2)
```
방법: 소켓 1개 연결
순서:
  1. initialize → initialized
  2. tabs new (url=https://example.com) → tabId=X (assigned_tab 배정됨)
  3. navigate → 정상 동작
  4. tabs close (tabId=X) → 탭 닫힘, assigned_tab=null
  5. navigate (url=https://httpbin.org) → assigned_tab이 null
기대:
  ✅ 5단계에서 크래시 없음
  ✅ 자동으로 현재 활성 탭에 재배정
  ✅ navigate 성공 (httpbin 페이지 로드)
PASS 기준: 5단계 응답에 success=true 또는 frameId 포함
```

### 9-4. ChromiumManager 동시 접속 race condition (Phase 3)
```
조건: Chromium 완전 종료, 데몬만 실행
방법: Python에서 5개 소켓을 동시에 (0.1초 이내) proxy 소켓에 연결
순서:
  1. 5개 스레드가 동시에 connect → initialize → tabs list
  2. 데몬 로그 분석
기대:
  ✅ Chromium Popen 호출 1회만 (로그에 "Chromium 실행" 1건)
  ✅ 5개 클라이언트 모두 tabs list 응답 수신
  ✅ 응답 시간 30초 이내
PASS 기준: 로그에서 "PID:" 패턴이 1개만 존재
```

### 9-5. ChromiumManager 크래시 후 자동 복구 (Phase 3)
```
조건: Chromium 실행 중, 데몬 READY 상태
방법:
  1. 소켓 연결 → initialize → tabs list → 정상 응답
  2. kill -9 $(pgrep -f Chromium)
  3. 15초 대기
  4. 새 소켓 연결 → initialize → tabs list
기대:
  ✅ 데몬 로그에 "크래시 감지" → "STOPPED → STARTING → READY"
  ✅ 4단계에서 tabs list 정상 응답
PASS 기준: 4단계 응답에 "tabs" 키 존재
```

### 9-6. Chromium 정상 종료 시 재시작 안 함 (Phase 3)
```
조건: 데몬이 Chromium을 시작한 상태
방법:
  1. 데몬 로그에서 Chromium PID 확인
  2. kill <PID> (SIGTERM, exit code != -9)
  3. 15초 대기
  4. 데몬 로그 확인
기대:
  ✅ 로그에 "정상 종료 — 재시작 안 함" (exit code 0 또는 -15)
  ✅ Chromium 프로세스 없음
  ✅ 새 MCP 호출 시 → 데몬이 다시 Chromium 시작 (on-demand)
주의: macOS에서 SIGTERM 시 exit code가 -15일 수 있으므로,
      데몬의 정상 종료 판단 기준에 SIGTERM(-15)도 포함해야 할 수 있음
```

---

## Stage 10: 엣지 케이스

### 10-1. 존재하지 않는 CSS 선택자 클릭
```
MCP 도구: click (selector="#nonexistent-element-xyz")
기대:
  ✅ 크래시 없음
  ✅ 명확한 에러 메시지 반환: "요소를 찾을 수 없습니다"
```

### 10-2. 유효하지 않은 URL 네비게이션
```
MCP 도구: navigate (url="not-a-valid-url")
기대:
  ✅ 크래시 없음
  ✅ 에러 반환 또는 chrome://error 페이지
```

### 10-3. 매우 긴 텍스트 입력
```
MCP 도구: keyboard (action=type, text="a" * 10000)
기대:
  ✅ 타임아웃 없이 완료
  ✅ 메모리 오버플로 없음
```

### 10-4. 탭 닫힌 후 해당 탭에 명령 시도
```
순서:
  1. tabs new → tabId=X
  2. navigate (url=...) → 정상
  3. tabs close (tabId=X)
  4. navigate (url=...) → assigned_tab이 null
기대:
  ✅ 크래시 없음
  ✅ 자동으로 새 활성 탭에 재배정
  ✅ 네비게이션 성공
```

### 10-5. 빠른 연속 호출 (rapid fire)
```
방법: 소켓 스크립트로 0.05초 간격 연속 호출
순서:
  1. navigate (url=https://example.com)  ← 응답 기다리지 않고
  2. screenshot                          ← 즉시 호출
  3. evaluate (expression="1+1")         ← 즉시 호출
기대:
  ✅ 3개 요청 모두 응답 수신 (id로 구분)
  ✅ 응답 순서는 보장 안 돼도 됨 (비동기)
  ✅ 크래시/hang 없음
PASS 기준: 3개 응답 id가 모두 다르고 전부 수신됨
```

### 10-6. 데몬이 기존 Chromium에 연결하는 경우
```
조건: 데몬 미실행, Chromium은 수동으로 이미 실행 중
방법:
  1. Chromium을 직접 실행 (데몬 없이)
  2. 데몬 시작
  3. 소켓 연결 → initialize → tabs list
기대:
  ✅ 데몬이 기존 Chromium 소켓에 연결
  ✅ 새 Chromium을 실행하지 않음
  ✅ tabs list에 기존 탭 표시
주의: 이 경우 데몬의 self._process는 None.
      모니터가 Popen.poll()로 체크 시 None 처리 필요.
```

---

## Stage 8-2 주의사항: macOS exit code

macOS에서 Chromium 종료 시 exit code 매핑:
| 종료 방법 | 시그널 | returncode |
|-----------|--------|------------|
| Cmd+Q | SIGTERM | -15 |
| kill <pid> | SIGTERM | -15 |
| kill -9 | SIGKILL | -9 |
| 정상 종료 (UI 닫기) | 없음 | 0 |

**결론**: `returncode == 0`만 정상 종료로 판단하면 Cmd+Q(-15)도 크래시로 오판.
데몬에서 **`returncode in (0, -15)`를 정상 종료**로 처리해야 함.
→ `chromium-mcp-daemon.py`의 `_monitor_loop()`에 반영 필요.

---

## 실행 방법

### 자동화 테스트
```bash
python3 scripts/test_integration.py --stage all
```

### 수동 테스트 (Claude CLI)
```bash
# 전제 조건 초기화
pkill -9 -f Chromium
launchctl unload ~/Library/LaunchAgents/com.chromium-mcp.daemon.plist 2>/dev/null
rm -f /tmp/.chromium-mcp*.sock /tmp/chromium-mcp-daemon.pid

# 데몬 재등록
launchctl load ~/Library/LaunchAgents/com.chromium-mcp.daemon.plist

# Claude CLI 시작 후 Stage 1부터 순서대로 실행
claude
```

### 결과 기록
각 테스트 항목의 ✅/❌ 결과를 기록하고, 실패 시 로그 첨부:
- `/tmp/chromium-mcp-daemon.log`
- Chromium stderr 출력
- Claude CLI MCP 응답 원문
