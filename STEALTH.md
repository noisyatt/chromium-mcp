# 은닉성 분석

## 탐지 벡터별 상태

Chromium-MCP가 기존 방식 대비 각 탐지 벡터에서 어떤 위치에 있는지 분석.

## 1. 프로세스 인자 탐지

### 기존 방식
```bash
$ ps aux | grep chrome
chrome --remote-debugging-port=9222    # ← 즉시 탐지
chrome --remote-debugging-pipe         # ← 즉시 탐지
```

### Chromium-MCP
```bash
$ ps aux | grep chromium
chromium-mcp                           # ← 일반 브라우저와 구분 불가
```

**환경변수 모드(`CHROMIUM_MCP=1`)** 사용 시 프로세스 인자에 MCP 관련 흔적 제로. 환경변수는 `/proc/[pid]/environ`(Linux)이나 `ps eww`로 볼 수 있지만:
- macOS에서는 다른 사용자의 프로세스 환경변수 접근이 SIP로 차단됨
- 브라우저 시작 후 환경변수를 `unsetenv()`로 제거 가능

**평가: 완전 회피 가능**

## 2. 네트워크 포트 탐지

### 기존 방식
```bash
$ lsof -i :9222
chrome  12345  user  TCP *:9222 (LISTEN)   # ← 즉시 탐지

$ curl http://localhost:9222/json/version  # ← Chrome 버전, 열린 탭 전부 노출
```

### Chromium-MCP
- stdio 모드: 포트 없음
- socket 모드: Unix socket (TCP 아님, `lsof -i`에 안 잡힘)

```bash
$ lsof -i | grep chromium
(결과 없음)

$ netstat -an | grep chromium
(결과 없음)
```

**평가: 완전 회피 가능**

## 3. DevToolsActivePort 파일 탐지

### 기존 방식
```bash
$ cat ~/Library/Application\ Support/Google/Chrome/DevToolsActivePort
9222
/devtools/browser/abc-123...
```

### Chromium-MCP
- `DevToolsHttpHandler`를 시작하지 않으므로 이 파일이 생성되지 않음

**평가: 완전 회피 가능**

## 4. 웹사이트 JavaScript 탐지

### 4-1. navigator.webdriver

| 방식 | 값 |
|------|---|
| Selenium/ChromeDriver | `true` |
| `--remote-debugging-port` 단독 | `false` |
| Chromium-MCP | `false` (정상 브라우저와 동일) |

`--enable-automation` 플래그를 사용하지 않으므로 `navigator.webdriver`는 `false`.

**평가: 탐지 불가**

### 4-2. Runtime.enable CDP 흔적

기존 자동화 도구는 `Runtime.enable`을 반드시 호출해야 한다. 이때 `console.debug()`에 전달된 객체의 getter가 트리거되는 사이드이펙트로 탐지 가능.

Chromium-MCP의 대응:
1. **`Runtime.enable` 호출 자체를 안 함** — 내부 코드에서 `Runtime.evaluate`를 직접 디스패치
2. **Isolated World 사용** — 페이지 JS와 격리된 실행 컨텍스트
3. V8 2025년 업데이트로 Error.stack getter 탐지도 이미 무력화됨

**평가: 탐지 불가**

### 4-3. chrome.csi / chrome.loadTimes / chrome.app

Chromium-MCP는 일반 Chromium 빌드이므로 이 API들이 정상적으로 존재한다. 헤드리스 브라우저와 달리 실제 렌더러가 동작하므로 값도 정상.

**평가: 탐지 불가**

### 4-4. window.cdc_ 변수 (ChromeDriver 흔적)

ChromeDriver를 사용하지 않으므로 `cdc_` 변수가 존재하지 않음.

**평가: 해당 없음**

### 4-5. Page.evaluateOnNewDocument 주입 탐지

외부 CDP에서 `Page.evaluateOnNewDocument`로 스크립트를 주입하면 `Debugger.scriptParsed()` 이벤트에 `VM###` 형태로 노출된다.

Chromium-MCP는 필요 시 **Blink 내부에서 직접 스크립트를 주입**할 수 있어 CDP 계층을 거치지 않음. 또는 Isolated World에서 실행하여 페이지 컨텍스트와 완전 분리.

**평가: 탐지 불가**

## 5. TLS 지문 (JA3/JA4)

Chromium-MCP는 **표준 Chromium 빌드**이므로 TLS 핸드셰이크가 일반 Chrome/Chromium과 동일하다.

| 브라우저 | JA3 해시 |
|---------|---------|
| Chrome 134 | 정상 해시 |
| Chromium-MCP (동일 버전 기반) | **동일 해시** |
| Puppeteer + Chrome | 동일 (같은 바이너리) |
| 안티디텍트 브라우저 (소스 패치) | **다를 수 있음** (BoringSSL 수정 시) |

MCP 서버 코드는 네트워크 스택(BoringSSL)을 수정하지 않으므로 TLS 지문 변화 없음.

**평가: 탐지 불가**

## 6. 행동 분석 (마우스/키보드 패턴)

MCP 도구로 클릭/타이핑을 수행하면 CDP `Input.dispatchMouseEvent`/`Input.dispatchKeyEvent`를 사용한다. 이는 모든 자동화 도구와 동일한 한계:

- 이벤트 타이밍이 기계적으로 균일
- 마우스 이동 경로 없음 (텔레포트)
- 스크롤 패턴 불규칙

**완화 전략:**
```cpp
// 베지어 곡선 마우스 이동 시뮬레이션
void McpClickTool::SimulateHumanClick(int x, int y) {
  auto current = GetCursorPosition();
  auto path = GenerateBezierPath(current, {x, y});
  for (auto& point : path) {
    DispatchMouseMoveEvent(point.x, point.y);
    SleepWithJitter(5, 15);  // 5-15ms 랜덤 딜레이
  }
  DispatchMouseClickEvent(x, y);
}
```

**평가: 기본 구현으로는 탐지 가능. 행동 시뮬레이션 모듈 추가 시 회피 가능.**

## 7. Canvas/WebGL/Audio 핑거프린트

일반 Chromium 빌드이므로 실제 GPU 렌더링이 동작한다. 핑거프린트 값이 정상.

| 항목 | 헤드리스 Chrome | Chromium-MCP |
|------|---------------|-------------|
| Canvas 렌더링 | SwiftShader (소프트웨어) | **실제 GPU** |
| WebGL 벤더 | "Google Inc. (Google)" | **실제 GPU 벤더** |
| Audio 출력 | 시뮬레이션 | **실제 오디오** |

**평가: 탐지 불가**

## 종합 탐지 벡터 매트릭스

| 탐지 벡터 | CDP 포트 | CDP 파이프 | 확장 | Chromium-MCP |
|----------|:---:|:---:|:---:|:---:|
| 프로세스 인자 | **탐지** | **탐지** | 안전 | **안전** |
| 네트워크 포트 | **탐지** | 안전 | 안전 | **안전** |
| DevToolsActivePort | **탐지** | **탐지** | 안전 | **안전** |
| navigator.webdriver | 조건부 | 조건부 | 안전 | **안전** |
| Runtime.enable 흔적 | **탐지** | **탐지** | **탐지** | **안전** |
| CDP 주입 스크립트 | **탐지** | **탐지** | 부분 | **안전** |
| TLS 지문 | 안전 | 안전 | 안전 | **안전** |
| 행동 분석 | 탐지 | 탐지 | 탐지 | **탐지** (개선 가능) |
| Canvas/WebGL | 안전* | 안전* | 안전 | **안전** |
| 디버거 배너 | 없음 | 없음 | **표시** | **없음** |

*\* 헤드리스 모드에서는 탐지될 수 있음*

## 결론

Chromium-MCP는 **행동 분석을 제외한 모든 탐지 벡터에서 완전 은닉**이 가능하다. 행동 분석도 마우스/키보드 시뮬레이션 모듈을 추가하면 대폭 완화할 수 있다.

기존 방식 대비 가장 큰 차이:
1. **Runtime.enable 없이 JS 실행** — 가장 강력한 CDP 탐지 신호 제거
2. **네트워크 포트/파일 흔적 제로** — 시스템 관점에서 일반 브라우저
3. **디버거 배너 없음** — 네트워크 캡처 시에도 UI 변화 없음
4. **TLS 지문 동일** — 표준 Chromium 빌드
