# 로드맵

## Phase 0: 환경 구축 (1-2일)

- [ ] depot_tools 설치 및 설정
- [ ] Chromium 소스 체크아웃 (stable 브랜치)
- [ ] 기본 빌드 성공 확인 (수정 없이 vanilla Chromium)
- [ ] 빌드된 Chromium.app 실행 확인
- [ ] 개발용 GN 설정 최적화 (component build, symbol level)

### 완료 기준
- `autoninja -C out/Default chrome` 성공
- 빌드된 Chromium으로 웹 브라우징 정상 동작

---

## Phase 1: 최소 MCP 서버 (3-5일)

### 목표
stdin/stdout으로 MCP 핸드셰이크가 되는 최소 구현

- [ ] `chrome/browser/mcp/` 디렉토리 생성
- [ ] `BUILD.gn` 통합
- [ ] `--mcp-stdio` 플래그 파싱
- [ ] MCP `initialize` / `initialized` 핸드셰이크 구현
- [ ] `tools/list` 응답 (빈 도구 목록이라도)
- [ ] Claude Code에서 MCP 서버로 연결 테스트

### 완료 기준
```bash
# Claude Code 설정
{
  "mcpServers": {
    "chromium-mcp": {
      "command": "/path/to/Chromium.app/Contents/MacOS/Chromium",
      "args": ["--mcp-stdio"]
    }
  }
}
```
Claude Code에서 연결 성공, `tools/list` 응답 수신

---

## Phase 2: 핵심 도구 구현 (5-7일)

### 목표
실제 브라우저 제어가 가능한 도구 구현

- [ ] `navigate` — URL 이동, 뒤로/앞으로
- [ ] `page_content` — 접근성 트리 / HTML / 텍스트 추출
- [ ] `screenshot` — 페이지 스크린샷 (base64 반환)
- [ ] `click` — CSS 선택자 또는 ref 기반 클릭
- [ ] `fill` — 입력 필드 값 설정
- [ ] `evaluate` — JavaScript 실행 (Runtime.enable 없이)
- [ ] `tabs` — 탭 목록/생성/닫기/전환

### 핵심 기술 과제
- `DevToolsAgentHost`에 내부 세션 연결하는 코드 작성
- CDP 명령 송수신을 MCP JSON-RPC로 래핑
- 비동기 응답 처리 (CDP는 비동기, MCP도 비동기)

### 완료 기준
Claude Code에서 실제 웹사이트 접속 → 페이지 읽기 → 클릭 → JS 실행 가능

---

## Phase 3: 네트워크 캡처 (3-5일)

### 목표
디버거 배너 없이 네트워크 트래픽 전체 캡처

- [ ] `network_capture` — start/stop
- [ ] 요청/응답 헤더 캡처
- [ ] 응답 본문 캡처 (바이너리는 base64)
- [ ] 리소스 타입 필터링
- [ ] URL 패턴 필터링

### 검증 항목
- [ ] 네트워크 캡처 중 **노란 배너가 표시되지 않음** 확인
- [ ] `navigator.webdriver === false` 확인
- [ ] 봇 탐지 테스트 사이트에서 정상 통과 확인

### 완료 기준
API 호출의 요청/응답 헤더 + 본문을 배너 없이 캡처

---

## Phase 4: 은닉성 강화 (3-5일)

### 목표
모든 알려진 탐지 벡터에서 일반 브라우저와 구분 불가

- [ ] 프로세스 인자 은닉 (환경변수 모드)
- [ ] 환경변수 시작 후 `unsetenv()` 호출
- [ ] `Runtime.enable` 호출 없이 JS 실행 확인
- [ ] Isolated World 실행 모드 구현
- [ ] Unix socket 전송 모드 구현

### 탐지 테스트
- [ ] [bot.sannysoft.com](https://bot.sannysoft.com) 통과
- [ ] [browserleaks.com](https://browserleaks.com) 정상 지문
- [ ] [fingerprintjs.github.io/fingerprintjs](https://fingerprintjs.github.io/fingerprintjs) 정상 ID
- [ ] `creepjs` 테스트 통과

### 완료 기준
위 4개 사이트에서 일반 Chromium과 동일한 결과

---

## Phase 5: 패키징 및 배포 (2-3일)

- [ ] macOS .app 번들 패키징
- [ ] Info.plist 커스텀 (앱 이름, 아이콘, 스킴 핸들러)
- [ ] 기본 브라우저 등록 테스트
- [ ] DMG 또는 zip 배포 형태
- [ ] 자동 빌드 스크립트 (build.sh)

### 완료 기준
`/Applications`에 설치 → 기본 브라우저 설정 → 일상 브라우징 + MCP 동시 사용

---

## Phase 6: 고급 기능 (선택적)

- [ ] 행동 시뮬레이션 (베지어 곡선 마우스, 가우시안 타이밍)
- [ ] 쿠키/로컬스토리지 직접 접근 도구
- [ ] 파일 다운로드/업로드 도구
- [ ] 콘솔 메시지 캡처 도구
- [ ] 성능 트레이스 도구
- [ ] 다중 클라이언트 동시 접속 (socket 모드)
- [ ] 자동 업데이트 메커니즘

---

## 일정 요약

| Phase | 내용 | 예상 기간 | 의존성 |
|-------|------|----------|--------|
| 0 | 환경 구축 | 1-2일 | 없음 |
| 1 | 최소 MCP 서버 | 3-5일 | Phase 0 |
| 2 | 핵심 도구 | 5-7일 | Phase 1 |
| 3 | 네트워크 캡처 | 3-5일 | Phase 2 |
| 4 | 은닉성 강화 | 3-5일 | Phase 3 |
| 5 | 패키징/배포 | 2-3일 | Phase 4 |
| 6 | 고급 기능 | TBD | Phase 5 |

**Phase 0-5 총 예상: 3-4주**

## 리스크

| 리스크 | 영향 | 완화 방안 |
|--------|------|----------|
| Chromium 빌드 실패 | Phase 0 차단 | 공식 문서 + 커뮤니티 지원 |
| DevToolsAgentHost 내부 API 변경 | Phase 2 재작업 | stable 브랜치 기반 작업, API 변경 추적 |
| 빌드 시간 (초회 1-3시간) | 개발 속도 저하 | ccache, component build, 증분 빌드 |
| 디스크 공간 부족 (100GB+) | 빌드 불가 | 외장 SSD 또는 클라우드 빌드 |
| Upstream 리베이스 충돌 | 업데이트 어려움 | MCP 코드를 `chrome/browser/mcp/`에 격리 |
