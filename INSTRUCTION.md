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
      "command": "/opt/homebrew/bin/python3",
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

## 통합 로케이터 (요소 지정 방법)

모든 요소 대상 도구(`click`, `fill`, `hover`, `scroll`, `drag`, `select_option`, `file_upload`, `find`, `wait`, `element`, `element_info`)에서 공통으로 사용 가능한 파라미터다.

### 요소 찾기 방법 (우선순위 순)

| 우선순위 | 파라미터 | 설명 | 예시 |
|----------|----------|------|------|
| 1 | `role` + `name` | ARIA 역할 + 접근성 이름 **(가장 권장)** | `{"role": "button", "name": "로그인"}` |
| 2 | `text` | 요소 텍스트 내용 | `{"text": "로그인"}` |
| 3 | `selector` | CSS 셀렉터 (기존 호환) | `{"selector": "#login-btn"}` |
| 4 | `xpath` | XPath 표현식 | `{"xpath": "//button[@id='login']"}` |
| 5 | `ref` | backendDOMNodeId 직접 지정 | `{"ref": 142}` |

### 공통 옵션 파라미터

| 파라미터 | 타입 | 기본값 | 설명 |
|----------|------|--------|------|
| `exact` | boolean | `false` | `false` = 부분 일치, `true` = 정확 일치 |
| `timeout` | number | `5000` | auto-wait 최대 대기 시간(ms). `0`이면 즉시 실패 |
| `force` | boolean | `false` | `true`면 가시성/활성화 체크를 스킵하고 강제 동작 |

### auto-wait 동작

- **DOM 미존재 시:** `timeout` 내에서 요소가 나타날 때까지 자동 재시도
- **비가시 요소:** 자동으로 `scrollIntoView` 후 재시도
- **애니메이션 중:** 좌표가 안정될 때까지 대기 후 동작

### 하위 호환성

- 기존 `selector` 파라미터는 그대로 동작 (변경 없음)
- 새 파라미터(`role`, `name`, `text`, `exact`, `timeout`, `force`)는 전부 선택적
- **동작 변화:** 기존에는 요소 미발견 시 즉시 에러, 이제는 기본 5초 대기 후 에러. 즉시 실패를 원하면 `timeout: 0` 지정

### 사용 예시

```python
# role + name (권장) — 접근성 기반, 가장 견고함
send_msg(s, {'jsonrpc':'2.0','id':20,'method':'tools/call','params':{
    'name':'click','arguments':{'role':'button','name':'로그인'}
}})

# text — 텍스트로 버튼 찾기
send_msg(s, {'jsonrpc':'2.0','id':21,'method':'tools/call','params':{
    'name':'click','arguments':{'text':'로그인','exact':True}
}})

# ref — find 결과의 backendNodeId를 직접 사용
send_msg(s, {'jsonrpc':'2.0','id':22,'method':'tools/call','params':{
    'name':'click','arguments':{'ref':142}
}})

# timeout: 0 — 즉시 실패 (요소가 반드시 존재해야 할 때)
send_msg(s, {'jsonrpc':'2.0','id':23,'method':'tools/call','params':{
    'name':'element','arguments':{'selector':'.modal','timeout':0}
}})
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
| `click` | CSS 선택자/ref/role+name 기반 클릭 (통합 로케이터 지원) |
| `fill` | 입력 필드 값 입력 (통합 로케이터 지원) |
| `evaluate` | JavaScript 실행 |
| `hover` | 요소 위에 마우스 올리기 (통합 로케이터 지원) |
| `scroll` | 페이지/요소 스크롤 (통합 로케이터 지원) |
| `drag` | 드래그 앤 드롭 (통합 로케이터 지원) |
| `select_option` | 드롭다운 선택 (통합 로케이터 지원) |
| `keyboard` | 키보드 입력 |
| `mouse` | 마우스 이벤트 |
| `element` | 요소 정보 조회 (통합 로케이터 지원) |
| `element_info` | 요소 상세 정보 (통합 로케이터 지원) |
| `find` | 텍스트/선택자/role로 요소 검색 (통합 로케이터 지원) |
| `wait` | 요소/텍스트 대기 (통합 로케이터 지원) |

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

## 주요 도구 사용 예시

### keyboard 도구

```python
# 텍스트 입력 (insertText — 한글/이모지 포함 가능)
send_msg(s, {'jsonrpc':'2.0','id':10,'method':'tools/call','params':{
    'name':'keyboard','arguments':{'action':'type','text':'안녕하세요 MCP 테스트'}
}})

# 글자별 타이핑 (delay > 0이면 각 문자마다 keyDown/keyUp 발송)
send_msg(s, {'jsonrpc':'2.0','id':11,'method':'tools/call','params':{
    'name':'keyboard','arguments':{'action':'type','text':'hello','delay':50}
}})

# 특수키 입력
send_msg(s, {'jsonrpc':'2.0','id':12,'method':'tools/call','params':{
    'name':'keyboard','arguments':{'action':'press','key':'Enter'}
}})

# 단축키 (Cmd+A 전체 선택 등)
send_msg(s, {'jsonrpc':'2.0','id':13,'method':'tools/call','params':{
    'name':'keyboard','arguments':{'action':'shortcut','key':'a','modifiers':['meta']}
}})
```

### Node.js로 직접 소켓 연결

```javascript
const net = require('net');
const sock = net.createConnection({path: '/tmp/.chromium-mcp.sock'});
// Content-Length 프레이밍으로 JSON-RPC 메시지 송수신
```

## 제약 사항

- **단일 클라이언트:** 소켓 모드는 동시에 1개 클라이언트만 연결 가능. 기존 연결이 있으면 새 연결은 거절된다.
- **이전 클라이언트 정리:** EPIPE 에러가 발생하면, 이전에 연결했던 클라이언트 프로세스가 아직 살아있는지 확인하고 종료해야 한다.
  ```bash
  # 소켓을 점유 중인 프로세스 확인
  lsof /tmp/.chromium-mcp.sock
  # 해당 클라이언트 프로세스 종료 후 재연결
  ```

## 트러블슈팅

### 소켓 연결 안 됨
```bash
# 기존 소켓 파일 삭제 후 재시도
rm -f /tmp/.chromium-mcp.sock
# lock 파일 삭제
rm -f ~/.chromium-mcp/instance.lock
```

### EPIPE 에러 (연결 후 즉시 끊김)
- **원인:** 이전 클라이언트가 소켓을 점유 중 (단일 클라이언트 제한)
- **해결:** `lsof /tmp/.chromium-mcp.sock`로 점유 프로세스 확인 후 종료

### MCP 서버가 시작 안 됨
- 플래그 없이 실행해도 기본적으로 MCP 서버가 시작됨
- `CHROMIUM_MCP=0`이 설정되어 있지 않은지 확인

### Content-Length 프레이밍 오류
- 줄바꿈은 반드시 `\r\n` (CRLF) 사용
- 헤더와 바디 사이에 빈 줄 `\r\n` 필요
- `\n`만 사용하면 서버가 헤더를 파싱하지 못함

### macOS Cmd+키 단축키가 동작하지 않음
- CDP `Input.dispatchKeyEvent`에 `commands` 필드가 필요
- `keyboard_tool.cc`에서 `GetMacCommand()`가 자동으로 매핑함
- 지원되는 단축키: selectAll(Cmd+A), copy(Cmd+C), paste(Cmd+V), cut(Cmd+X), undo(Cmd+Z), redo(Cmd+Shift+Z)
