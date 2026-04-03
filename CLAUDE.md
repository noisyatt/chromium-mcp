# Chromium-MCP 프로젝트 규칙

## ⚠️ 빌드 전 필수 체크리스트 (절대 생략 금지)

빌드 시작 전에 아래 4항목을 반드시 확인할 것. 하나라도 빠지면 수시간 재빌드.

1. **args.gn에 `dcheck_always_on = false` 포함** — 기본값이 true라 LXC에서 크래시
2. **배포 tar에 `snapshot_blob.bin`, `v8_context_snapshot.bin` 포함** — 없으면 V8 크래시
3. **`chrome/BUILD.gn`에 `//chrome/browser/mcp` deps** — browser/BUILD.gn이 아님!
4. **args.gn 변경 시 사용자에게 풀 재빌드 경고** — GN args 변경 = 거의 전체 재컴파일

## 절대 금지

- `gclient sync --force --reset` — 모든 빌드 캐시 무효화
- `gn clean` / `rm -rf out/Default` — 빌드 디렉토리 삭제

## GN args (Linux + LXC 배포용)

```
is_debug = false
is_component_build = false
symbol_level = 0
blink_symbol_level = 0
enable_nacl = false
use_thin_lto = false
dcheck_always_on = false
dcheck_is_configurable = false
```

## MCP 설정

- `~/.mcp.json`에서 Python 경로는 반드시 `/opt/homebrew/bin/python3` 절대경로 사용
- `python3`으로 하면 Xcode Python이 잡혀 소켓 생성 불가
