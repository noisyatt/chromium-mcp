# chromium-mcp 멀티 인스턴스 & 봇 감지 우회 — 3-LLM 컨센서스

> **날짜**: 2026-03-28
> **참여 모델**: Claude Opus 4.6, GPT-5.3-Codex, Gemini 3.1 Pro
> **목적**: chromium-mcp를 여러 독립 환경에서 동시에 실행하는 구조 설계 및 봇 감지 우회 전략 검증

---

## 1. 멀티 인스턴스 구성

### 1.1 배경 및 문제

**현재 구조의 한계:**
- 데몬 1개 → Unix socket 1개(`/tmp/.chromium-mcp.sock`) → 브라우저 1대
- 단일 클라이언트만 동시 연결 가능 (INSTRUCTION.md 명시)
- e2e 테스트를 여러 개 동시에 돌리면 브라우저 세션이 서로 간섭
  - 쿠키/로그인 상태 충돌
  - 탭/윈도우 조작이 다른 테스트에 영향
  - 네트워크 인터셉트 규칙 충돌

**요구사항:**
- 브라우저 인스턴스별 완전 격리 (세션, 파일시스템, 네트워크)
- Claude 서브에이전트에서 병렬로 각 인스턴스를 독립 제어
- 봇 감지를 회피하면서 headed 모드로 실행
- GUI 데스크탑 없는 서버 Linux에서 동작

---

### 1.2 각 모델의 제안

#### Claude (초기 제안)
- Linux 빌드 가능 확인: `build.sh`에 Linux 분기 이미 존재 (231-236줄)
  - macOS: `Chromium.app` 번들 / Linux: `out/Default/chrome` 바이너리
  - GN 빌드 인자도 플랫폼 중립적
  - C++ 소스(`src/`)에 플랫폼 조건 분기 거의 없음 (`#if defined(__APPLE__)`은 소켓 통신 일부에만)
- **데몬에 멀티소켓 라우팅 기능 추가** 제안
  - MCP 도구 호출 시 `target` 파라미터로 브라우저 지정
  - 데몬이 요청을 해당 인스턴스의 소켓으로 라우팅

#### Codex (구현 방식 수정 — 채택됨)
- **멀티소켓 라우터를 만들지 마라. 인스턴스당 데몬 1개가 더 낫다.**
- 근거:
  - 라우터는 **단일 장애점(SPOF)**이 됨 — 라우터가 죽으면 전체 인스턴스 접근 불가
  - 데몬 N개면 하나가 죽어도 **해당 LXC만 영향** → 장애 격리 우수
  - 기존 데몬 코드를 수정할 필요 없이 **그대로 N개 실행**하면 됨
  - 구현 복잡도: 라우터 신규 개발 >> 기존 데몬 복수 실행
- 운영 구조:
  - 각 LXC가 독립된 `user-data-dir`, X display(Xvfb), Unix socket 보유
  - 오케스트레이터가 인스턴스 풀 관리 (healthcheck, restart, 스케일 조정)
- Playwright CI 문서에서도 Xvfb + headed 실행을 공식 지원함을 근거로 제시
  - 출처: https://playwright.dev/docs/ci

#### Gemini (빌드 분리 강조)
- 현재 서버(i5-6600, 16GB RAM)에서 **Chromium 빌드는 사실상 불가능**
  - 빌드 시 수백 개의 프로세스 생성
  - 최소 32GB RAM + 100GB 디스크 필요
  - 공식 문서: https://chromium.googlesource.com/chromium/src/+/refs/tags/138.0.7155.1/docs/linux/build_instructions.md
- **빌드를 외부로 분리**하고 바이너리만 배포할 것
  - GitHub Actions, 고성능 워크스테이션, 클라우드 VM 등
- LXC가 VM보다 오버헤드 적어 멀티 인스턴스에 최적
- **"브라우저 그리드"** 방식 권장:
  - 인스턴스 풀(Pool) 관리
  - 요청마다 가용 인스턴스의 소켓 경로를 동적 할당
  - BFF의 데몬 확장으로 인스턴스 매니저 구현

---

### 1.3 합의된 아키텍처

```
[빌드 단계]
  외부 고사양 빌더 (32GB+ RAM, 100GB+ 디스크)
    → ./scripts/build.sh ~/chromium/src  (Linux 타겟)
    → 산출물: out/Default/chrome + 런타임 의존성

[배포 단계]
  빌드 산출물 → Proxmox LXC 템플릿화
    → 템플릿 내용: chromium-mcp 바이너리 + Xvfb + 데몬 스크립트 + 의존 라이브러리
    → 필요 시 즉시 복제하여 새 인스턴스 생성

[운영 단계]
  Proxmox Host
    ├─ LXC-1
    │   ├─ Xvfb :1 (가상 디스플레이)
    │   ├─ chromium-mcp (DISPLAY=:1, --user-data-dir=/data/profile1)
    │   ├─ 데몬1 → /tmp/.chromium-mcp.sock
    │   └─ 완전 독립: 세션, 쿠키, 파일시스템, 네트워크
    │
    ├─ LXC-2
    │   ├─ Xvfb :1
    │   ├─ chromium-mcp (DISPLAY=:1, --user-data-dir=/data/profile2)
    │   ├─ 데몬2 → /tmp/.chromium-mcp.sock
    │   └─ 완전 독립
    │
    └─ LXC-3
        ├─ Xvfb :1
        ├─ chromium-mcp (DISPLAY=:1, --user-data-dir=/data/profile3)
        ├─ 데몬3 → /tmp/.chromium-mcp.sock
        └─ 완전 독립

[제어 단계 — client.py 내장 SSH]
  Mac (Claude Code)
    ├─ 서브에이전트 A → client.py (SSH 내장) → LXC-1:/tmp/.chromium-mcp.sock → 브라우저1
    ├─ 서브에이전트 B → client.py (SSH 내장) → LXC-2:/tmp/.chromium-mcp.sock → 브라우저2
    └─ 서브에이전트 C → client.py (SSH 내장) → LXC-3:/tmp/.chromium-mcp.sock → 브라우저3

  각 서브에이전트는 독립적으로 MCP 도구 호출 → 진짜 동시 브라우징
  client.py가 SSH 연결/재연결을 자체 관리 → 외부 SSH 터널 불필요
```

### 1.4 핵심 설계 결정

| 결정 | 채택안 | 기각안 | 이유 |
|------|--------|--------|------|
| 인스턴스 관리 | 데몬 N개 (LXC당 1개) | 단일 데몬 + 멀티소켓 라우터 | 장애 격리, 구현 단순성, 기존 코드 수정 불필요 |
| 격리 단위 | LXC 컨테이너 | Docker 컨테이너 / VM | LXC가 VM보다 가볍고 Docker보다 Proxmox 통합성 우수 |
| 빌드 위치 | 외부 빌더 | Proxmox 호스트 | 메모리/디스크 부족 (16GB/100GB+), 빌드 중 서비스 영향 |
| 디스플레이 | Xvfb (가상 프레임버퍼) | GUI 데스크탑 | 오버헤드 최소, headed 브라우저 실행에 충분 |
| 접근 방식 | client.py 내장 SSH → 원격 Unix socket | 외부 SSH 터널 / TCP 직접 노출 | 자체 재연결, 외부 의존 없음, 보안 |

### 1.5 구현 시 고려사항

**LXC 템플릿 구성:**
- 베이스: Ubuntu 22.04 LTS (Chromium 의존성 호환)
- 필수 패키지: `xvfb`, `libx11-6`, `libxcomposite1`, `libxdamage1`, `libxrandr2`, `libgbm1`, `libnss3`, `libatk-bridge2.0-0`, `libcups2`, `libdrm2`, `libxkbcommon0`, `libpango-1.0-0`, `libasound2`
- chromium-mcp 바이너리 + 데몬 스크립트 + systemd unit

**소켓 경로 충돌 없음:**
- 각 LXC는 독립 파일시스템 → `/tmp/.chromium-mcp.sock`이 동일 경로여도 충돌 없음
- `--mcp-socket=` 옵션으로 커스텀 경로 지정도 가능하지만 불필요

**리소스 추정 (인스턴스당):**
- RAM: 약 300~500MB (Chromium 프로세스 + Xvfb)
- 디스크: 약 1~2GB (바이너리 + 프로필)
- CPU: 유휴 시 미미, 페이지 로드 시 순간 스파이크
- 현재 Proxmox 여유 RAM ~6.6GB → 동시 3~4개 인스턴스 가능

**client.py 원격 연결 방식:**

client.py가 환경변수로 로컬/원격을 자동 분기:
- `CHROMIUM_MCP_SOCKET`만 있으면 → 기존 로컬 Unix socket 직접 연결
- `CHROMIUM_MCP_HOST`가 있으면 → paramiko로 SSH 연결 → 원격 Unix socket 포워딩

```python
# client.py 내부 (paramiko 기반)
ssh = paramiko.SSHClient()
ssh.connect(host, username=user, key_filename=key)
channel = ssh.get_transport().open_channel(
    "direct-streamlocal@openssh.com",
    dest_path="/tmp/.chromium-mcp.sock",
    src_addr=("", 0)
)
# channel을 통해 MCP 프로토콜 중계, 끊기면 자동 재연결
```

Claude MCP 서버 등록 예시:
```json
{
  "chromium-lxc1": {
    "command": "python3",
    "args": ["chromium-mcp-client.py"],
    "env": {
      "CHROMIUM_MCP_HOST": "10.0.0.101",
      "CHROMIUM_MCP_SSH_KEY": "~/.ssh/proxmox",
      "CHROMIUM_MCP_SSH_JUMP": "proxmox"
    }
  },
  "chromium-lxc2": {
    "command": "python3",
    "args": ["chromium-mcp-client.py"],
    "env": {
      "CHROMIUM_MCP_HOST": "10.0.0.102",
      "CHROMIUM_MCP_SSH_KEY": "~/.ssh/proxmox",
      "CHROMIUM_MCP_SSH_JUMP": "proxmox"
    }
  }
}
```

로컬 Chromium(기존)과 원격 LXC 인스턴스를 동일한 client.py로 지원.

**오케스트레이터 역할:**
- 인스턴스 풀 관리: 시작/중지/재시작
- Healthcheck: 주기적으로 소켓 연결 + `initialize` RPC 테스트
- 자동 복구: 응답 없는 인스턴스 재시작
- 스케일링: 필요 시 LXC 템플릿에서 새 인스턴스 생성

---

## 2. 봇 감지 우회 전략

### 2.1 chromium-mcp의 기술적 우위

**기존 자동화 도구의 탐지 포인트:**

| 탐지 벡터 | Puppeteer/Playwright | undetected-chromedriver | Camoufox | chromium-mcp |
|-----------|---------------------|------------------------|----------|-------------|
| `navigator.webdriver` | 노출 | 패치로 숨김 | 없음 | **없음** (네이티브) |
| `--remote-debugging-port` | 필요 | 필요 | 불필요 (Firefox) | **불필요** |
| CDP 외부 연결 | 감지됨 | 감지됨 | 해당없음 | **없음** (내부 IPC) |
| 프로세스 플래그 | `--enable-automation` 등 | 일부 패치 | 없음 | **없음** |
| 렌더링 파이프라인 차이 | headless 시 차이 | headed로 우회 | headed | **headed** (Xvfb) |
| Chrome Extension API | CDP로 주입 | CDP로 주입 | 불필요 | **불필요** (C++ 네이티브) |

**chromium-mcp가 감지되지 않는 구조적 이유:**
```
[기존 도구]
  외부 프로세스 → CDP (WebSocket, TCP 포트) → Chrome
  ↑ 이 연결이 감지됨: 포트 스캔, 프로세스 인자, navigator 속성

[chromium-mcp]
  MCP 명령 → Unix socket → Chromium 내장 McpServer
    → DevToolsAgentHost (프로세스 내부 IPC) → Blink/V8
  ↑ 외부에서 관찰 불가: 네트워크 포트 없음, 프로세스 플래그 없음
```

### 2.2 Chrome 보안 강화 추세와 영향

**Codex 단독 제시, Claude/Gemini 동의:**

Chrome이 2025-03-17에 `--remote-debugging-port` 보안을 추가 강화:
- 출처: https://developer.chrome.com/blog/remote-debugging-port
- 기존 CDP 기반 자동화 도구들이 더 탐지되기 쉬워지는 추세
- 이는 chromium-mcp의 **내부 IPC 방식이 미래에도 유효한 이유**를 강화함
- CDP 외부 노출에 의존하는 도구들(Puppeteer, Playwright, undetected-chromedriver)은 점점 불리해짐

### 2.3 봇 감지 다계층 분석

**Codex 핵심 경고: 브라우저 계층만으로 불충분하다.**

현대 WAF(Cloudflare, Akamai, PerimeterX 등)는 다계층으로 탐지:

| 탐지 계층 | 감지 대상 | chromium-mcp 상태 | 추가 대응 필요 |
|-----------|----------|------------------|---------------|
| **브라우저** | CDP 연결, webdriver 플래그, 자동화 확장 | **해결됨** — 내부 IPC, 플래그 없음, 확장 없음 | 없음 |
| **렌더링** | Canvas/WebGL fingerprint, 폰트 목록, headless 차이 | **해결됨** — headed 모드(Xvfb), 네이티브 렌더링 | 없음 |
| **TLS** | JA3/JA4 fingerprint (TLS 핸드셰이크 패턴) | **해결됨** — Chromium 네이티브 TLS 스택 사용, 일반 Chrome과 동일 | 없음 |
| **네트워크** | IP 평판, 데이터센터 IP 감지, 과거 행동 이력 | **미해결** | 주거용 프록시 또는 깨끗한 IP 필요 |
| **행동 패턴** | 요청 간격, 마우스 이동 패턴, 스크롤 패턴, 비정상 속도 | **미해결** | 요청 간격 자연화, 인간적 행동 시뮬레이션 |
| **세션** | 동일 IP에서 비정상적 다중 세션, 쿠키 불일치 | **부분 해결** (LXC 격리) | IP 로테이션, 세션 일관성 유지 |

### 2.4 네트워크 계층 대응 전략

**3개 모델 합의: 브라우저 우회 + 네트워크 관리 이중 전략 필요**

```
[완료 — chromium-mcp가 커버]
  브라우저 계층: CDP 숨김, webdriver 없음, 네이티브 렌더링
  TLS 계층: Chromium 네이티브 TLS, 일반 Chrome과 동일 JA3

[추가 필요 — 별도 구현]
  네트워크 계층:
    ├─ 프록시 품질 관리 (주거용 IP, 데이터센터 IP 회피)
    ├─ IP 로테이션 (세션 단위 또는 요청 단위)
    └─ 세션 일관성 (같은 세션은 같은 IP 유지)

  행동 계층:
    ├─ 요청 간격 랜덤화 (인간적 패턴)
    ├─ 마우스/스크롤 이벤트 자연화
    └─ 페이지 체류 시간 변동
```

### 2.5 Xvfb 기술 상세

**headless가 아닌 headed 브라우저를 GUI 없이 실행하는 방법:**

```bash
# Xvfb 시작 (가상 디스플레이 :1, 해상도 1920x1080, 24bit 색상)
Xvfb :1 -screen 0 1920x1080x24 -ac &

# 해당 디스플레이에서 Chromium 실행
DISPLAY=:1 ./out/Default/chrome --no-first-run --no-default-browser-check
```

**왜 Xvfb가 봇 감지에 안 걸리는가:**
- Xvfb는 실제 X11 서버와 동일한 프로토콜 구현
- Chromium 입장에서는 진짜 모니터가 있는 것과 동일하게 동작
- GPU 렌더링 파이프라인도 headed 모드 그대로 실행
- Canvas/WebGL fingerprint가 headless와 달리 **일반 브라우저와 동일한 결과** 생성
- `navigator.webdriver`가 설정되지 않음
- Playwright CI 환경에서도 공식적으로 이 방식을 사용 (https://playwright.dev/docs/ci)

**Xvfb vs headless 비교:**

| 항목 | `--headless` | Xvfb + headed |
|------|-------------|---------------|
| GPU 렌더링 | SwiftShader(소프트웨어) | X11 기반 (하드웨어 가능) |
| Canvas fingerprint | headless 특유 패턴 | 일반 브라우저와 동일 |
| WebGL | 제한적 | 정상 |
| `navigator.webdriver` | true (패치 필요) | false (기본) |
| 화면 캡처 | `--screenshot` 플래그 | 자유 (Xvfb 버퍼 접근) |
| 탐지 가능성 | **높음** | **매우 낮음** |

---

## 3. 전체 요약

### 합의 사항 (만장일치)

1. **멀티 인스턴스 방향은 올바르다** — LXC 격리 + Xvfb + 독립 데몬
2. **데몬 N개 > 멀티소켓 라우터** — 장애 격리, 구현 단순성, 코드 수정 최소화
3. **빌드는 외부 분리 필수** — Proxmox 16GB에서 Chromium 빌드 불가능
4. **LXC 템플릿으로 배포** — 복제 즉시 새 인스턴스 생성
5. **chromium-mcp + Xvfb가 현존 최선의 봇 감지 우회 방식**
6. **브라우저 우회만으로 불충분** — 네트워크/행동 계층도 병행 관리 필요

### 구현 결과 (2026-04-02)

**컨센서스 → 실제 구현 완료. 설계 변경점:**

1. **데몬 N개 → Pool Daemon 1개로 변경** — MCP 서버 등록 1개 유지하면서 자동 배정
   - daemon.py가 인스턴스 풀 관리, 클라이언트별 자동 배정, 세션 어피니티
   - `~/.chromium-mcp/pool.json`으로 설정
2. **client.py SSH 로직 → daemon.py로 이동** — client.py는 순수 프레이밍 변환만
3. **MCP 응답 정규화 레이어 추가** — C++(mcp_server.cc) + Python(client.py) 이중 적용
4. **click 도구 WeakPtr 버그 수정** — force 경로에서 PollContext 수명 관리

**테스트:**
- Mac 41/41 PASS (35개 도구 전수, client.py 경유)
- Pool 배정 6/6 PASS
- LXC 원격 e2e 기본 동작 확인 (click은 LXC 재빌드 필요)

### 미결 사항

- LXC Chromium 재빌드 (actionability_checker.cc + mcp_server.cc 반영)
- 프록시/IP 관리 전략 구체화
- 행동 패턴 자연화 구현 수준
