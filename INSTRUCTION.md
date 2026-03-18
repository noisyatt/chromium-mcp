# Chromium-MCP 사용 가이드

외부 MCP 클라이언트(Claude Code 등)에서 MCP가 내장된 Chromium을 사용하는 방법을 설명한다.

## 전제 조건

- macOS (Apple Silicon)
- 빌드된 Chromium-MCP 바이너리: `/Users/daniel/chromium/src/out/Default/Chromium.app`
- Python 3.10+

## MCP 활성화 방식

Chromium-MCP는 **플래그 없이도 MCP 서버가 항상 자동으로 시작된다.**

- 기본 동작: socket 모드 (`/tmp/.chromium-mcp.sock`)로 자동 시작
- `--mcp-stdio` 플래그: stdin/stdout 모드로 전환
- `--mcp-socket=<경로>`: 커스텀 소켓 경로 지정
- `CHROMIUM_MCP=0` 환경변수: MCP 서버 비활성화 (유일한 비활성화 방법)

## 방법 1: 런처 스크립트 (권장)

`chromium-mcp-launcher.py`는 Claude Code 같은 MCP 클라이언트와 Chromium 사이의 프록시 역할을 한다.

### 동작 흐름

```
Claude Code (stdin/stdout)
    ↕ JSON-RPC
chromium-mcp-launcher.py
    ↕ Unix socket
Chromium 내장 MCP 서버
```

1. `~/.chromium-mcp/instance.lock` 확인
2. 기존 Chromium 인스턴스가 있으면 → 해당 소켓에 연결
3. 없으면 → Chromium을 `--mcp-socket` 모드로 자동 실행, 소켓 준비 대기 후 연결
4. stdin ↔ socket 양방향 프록시 시작

### Claude Code 설정 (`~/.claude.json`)

```json
{
  "mcpServers": {
    "chromium-mcp": {
      "type": "stdio",
      "command": "python3",
      "args": ["/Users/daniel/projects-noisyatt/chromium-mcp/scripts/chromium-mcp-launcher.py"],
      "env": {
        "CHROMIUM_MCP_BROWSER_PATH": "/Users/daniel/chromium/src/out/Default/Chromium.app/Contents/MacOS/Chromium"
      }
    }
  }
}
```

### 환경변수

| 변수명 | 설명 | 기본값 |
|--------|------|--------|
| `CHROMIUM_MCP_BROWSER_PATH` | Chromium 바이너리 경로 | 자동 탐색 |
| `CHROMIUM_MCP_SOCKET_PATH` | Unix socket 경로 | `/tmp/.chromium-mcp.sock` |

### 설정 파일 (`~/.chromium-mcp/config.json`)

```json
{
  "browser_path": "/Users/daniel/chromium/src/out/Default/Chromium.app/Contents/MacOS/Chromium",
  "socket_path": "/tmp/.chromium-mcp.sock",
  "auto_launch": true,
  "launch_timeout": 30,
  "extra_args": ["--no-first-run", "--disable-default-apps"]
}
```

## 방법 2: 수동 실행 + 소켓 연결

```bash
# 1. Chromium을 MCP 소켓 모드로 실행
/Users/daniel/chromium/src/out/Default/Chromium.app/Contents/MacOS/Chromium \
  --mcp-socket=/tmp/.chromium-mcp.sock \
  --no-first-run \
  --disable-default-apps

# 2. 소켓 연결 테스트
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/tmp/.chromium-mcp.sock')
print('연결 성공')
s.close()
"
```

## 방법 3: stdio 모드 (직접 파이프)

```bash
# Chromium을 stdio 모드로 실행 (stdin/stdout으로 MCP 통신)
CHROMIUM_MCP=1 /Users/daniel/chromium/src/out/Default/Chromium.app/Contents/MacOS/Chromium \
  --mcp-stdio \
  --no-first-run
```

## MCP 프로토콜 통신

### 메시지 프레이밍

Content-Length 기반 프레이밍을 사용한다 (LSP와 동일):

```
Content-Length: 123\r\n
\r\n
{"jsonrpc": "2.0", "id": 1, "method": "initialize", ...}
```

### 핸드셰이크 순서

```
1. 클라이언트 → 서버: initialize (id 포함)
2. 서버 → 클라이언트: initialize 응답
3. 클라이언트 → 서버: notifications/initialized (id 없음, 알림)
4. 이후 tools/list, tools/call 등 사용 가능
```

### 핸드셰이크 예시 (Python)

```python
import socket, json

def send_msg(sock, obj):
    body = json.dumps(obj)
    header = f'Content-Length: {len(body)}\r\n\r\n'
    sock.sendall((header + body).encode())

def recv_msg(sock):
    header = b''
    while b'\r\n\r\n' not in header:
        header += sock.recv(1)
    length = int(header.decode().split(': ')[1].split('\r\n')[0])
    body = b''
    while len(body) < length:
        body += sock.recv(length - len(body))
    return json.loads(body.decode())

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/tmp/.chromium-mcp.sock')

# 1. Initialize
send_msg(s, {
    'jsonrpc': '2.0', 'id': 1, 'method': 'initialize',
    'params': {
        'protocolVersion': '2024-11-05',
        'capabilities': {},
        'clientInfo': {'name': 'my-client', 'version': '1.0'}
    }
})
resp = recv_msg(s)  # serverInfo 포함 응답

# 2. Initialized 알림
send_msg(s, {'jsonrpc': '2.0', 'method': 'notifications/initialized'})

# 3. 도구 목록 조회
send_msg(s, {'jsonrpc': '2.0', 'id': 2, 'method': 'tools/list', 'params': {}})
tools = recv_msg(s)
```

## 사용 가능한 도구 (35개)

### 페이지 제어
| 도구 | 설명 |
|------|------|
| `navigate` | URL 이동, 뒤로/앞으로/새로고침 |
| `screenshot` | 페이지/요소 스크린샷 (png/jpeg) |
| `page_content` | 접근성 트리, HTML, 텍스트 반환 |

### DOM 조작
| 도구 | 설명 |
|------|------|
| `click` | CSS 선택자/ref 기반 클릭 |
| `fill` | 입력 필드 값 입력 |
| `evaluate` | JavaScript 실행 |
| `hover` | 요소 위에 마우스 올리기 |
| `scroll` | 페이지/요소 스크롤 |
| `drag` | 드래그 앤 드롭 |
| `select_option` | 드롭다운 선택 |
| `keyboard` | 키보드 입력 |
| `mouse` | 마우스 이벤트 |
| `element` | 요소 정보 조회 |
| `element_info` | 요소 상세 정보 |
| `find` | 텍스트/선택자로 요소 검색 |
| `wait` | 요소/텍스트 대기 |

### 네트워크
| 도구 | 설명 |
|------|------|
| `network_capture` | 네트워크 요청 캡처 시작/중지 |
| `network_requests` | 캡처된 요청 목록 반환 |
| `network_intercept` | 네트워크 요청 가로채기 |

### 탭/윈도우
| 도구 | 설명 |
|------|------|
| `tabs` | 탭 목록/생성/닫기/전환 |
| `window` | 윈도우 관리 |

### 브라우저
| 도구 | 설명 |
|------|------|
| `browser_info` | 버전, 활성 탭 정보 |
| `console` | 콘솔 메시지 조회 |
| `cookies` | 쿠키 관리 |
| `storage` | localStorage/sessionStorage |
| `history` | 브라우저 히스토리 |
| `bookmarks` | 북마크 관리 |
| `clipboard` | 클립보드 읽기/쓰기 |
| `dialog` | alert/confirm/prompt 처리 |
| `download` | 다운로드 관리 |
| `file_upload` | 파일 업로드 |
| `emulate` | 디바이스/뷰포트 에뮬레이션 |

### 분석
| 도구 | 설명 |
|------|------|
| `coverage` | 코드 커버리지 |
| `pdf` | PDF 생성 |
| `performance` | 성능 메트릭 |

## 핵심 차별점 (vs 외부 CDP/확장)

| 항목 | 외부 CDP | Chrome 확장 | Chromium-MCP |
|------|----------|-------------|--------------|
| 연결 방식 | WebSocket 포트 | chrome.debugger API | 내부 IPC |
| 디버거 배너 | 없음 | 노란 배너 표시 | 없음 |
| `navigator.webdriver` | 설정됨 | 미설정 | 미설정 |
| `Runtime.enable` | 필요 | 필요 | 불필요 |
| 네트워크 노출 | 포트 오픈 | 없음 | 없음 |
| 프로세스 인자 | `--remote-debugging-port` 노출 | 없음 | MCP 플래그 자동 제거 |

## 트러블슈팅

### 소켓 연결 안 됨
```bash
# 기존 소켓 파일 삭제 후 재시도
rm -f /tmp/.chromium-mcp.sock
# lock 파일 삭제
rm -f ~/.chromium-mcp/instance.lock
```

### MCP 서버가 시작 안 됨
- `--mcp-socket` 또는 `CHROMIUM_MCP=1` 플래그 확인
- `CHROMIUM_MCP=0`이 설정되어 있지 않은지 확인

### Content-Length 프레이밍 오류
- 줄바꿈은 반드시 `\r\n` (CRLF) 사용
- 헤더와 바디 사이에 빈 줄 `\r\n` 필요
- `\n`만 사용하면 서버가 헤더를 파싱하지 못함
