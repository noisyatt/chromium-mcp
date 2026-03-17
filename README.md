# Chromium-MCP: MCP 서버 내장 Chromium 포크

> 디버그 모드 없이, 확장 없이, 브라우저 자체에 MCP 서버를 내장한 Chromium 포크

## 목표

- Chromium 소스에 MCP(Model Context Protocol) 서버를 직접 내장
- `--remote-debugging-port`, `--remote-debugging-pipe` 등 디버그 플래그 **불필요**
- 크롬 확장 프로그램 **불필요**
- 외부에서 자동화 브라우저임을 **탐지 불가능**
- 일반 브라우저처럼 기본 브라우저로 설정하여 일상 사용 가능

## 왜 필요한가

| 기존 방식 | 문제점 |
|----------|--------|
| CDP (`--remote-debugging-port`) | 포트 스캔, 프로세스 인자, `DevToolsActivePort` 파일로 탐지 |
| CDP Pipe (`--remote-debugging-pipe`) | 프로세스 인자에 플래그 노출 |
| 크롬 확장 (chrome-mcp-bridge) | 확장 설치 필요, `chrome.debugger` 사용 시 노란 배너 |
| Playwright / Puppeteer | 별도 브라우저 인스턴스, 기존 세션 접근 불가 |

**Chromium-MCP는 브라우저 내부에서 직접 API를 호출하므로 이 모든 제약이 없다.**

## 핵심 아키텍처

```
┌─────────────────────────────────────────────┐
│                Chromium-MCP                  │
│                                              │
│  ┌──────────┐    ┌──────────────────────┐   │
│  │  Blink   │    │  내장 MCP 서버       │   │
│  │  렌더러  │◄──►│  (IPC로 직접 접근)    │   │
│  └──────────┘    │                      │   │
│  ┌──────────┐    │  - stdio 모드        │   │
│  │  V8      │◄──►│  - Unix socket 모드  │   │
│  │  엔진    │    │  - 네트워크 포트 없음 │   │
│  └──────────┘    └──────────────────────┘   │
│  ┌──────────┐              ▲                │
│  │ Network  │              │ IPC            │
│  │  Stack   │◄─────────────┘                │
│  └──────────┘                               │
└─────────────────────────────────────────────┘
        ▲
        │ stdio / Unix socket
        ▼
┌──────────────┐
│  AI 클라이언트│  (Claude, 기타 MCP 클라이언트)
└──────────────┘
```

## 문서 구조

| 문서 | 내용 |
|------|------|
| [ARCHITECTURE.md](./ARCHITECTURE.md) | 상세 아키텍처 및 설계 |
| [BUILD.md](./BUILD.md) | Chromium 빌드 환경 구축 가이드 |
| [MCP-SERVER.md](./MCP-SERVER.md) | 내장 MCP 서버 설계 명세 |
| [STEALTH.md](./STEALTH.md) | 은닉성 분석 및 탐지 회피 전략 |
| [ROADMAP.md](./ROADMAP.md) | 로드맵 및 마일스톤 |

## 기존 프로젝트와의 관계

이 프로젝트는 [chrome-control](../chrome-control/)의 연구 결과를 기반으로 한다. chrome-control에서 검증한 확장 기반 MCP 연결 방식의 한계를 극복하기 위해 브라우저 자체를 포크하는 접근.
