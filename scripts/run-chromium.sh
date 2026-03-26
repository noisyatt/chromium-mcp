#!/usr/bin/env bash
# run-chromium.sh — Google API 키를 로드하여 Chromium-MCP를 실행
#
# 사용법:
#   ./scripts/run-chromium.sh [chromium 인자들...]
#
# 데몬이나 런처를 거치지 않고 직접 실행할 때 사용.
# API 키가 환경변수로 주입되어 "Google API keys are missing" 경고가 나타나지 않음.

set -euo pipefail

# Google API 키 로드
API_ENV="${HOME}/.chromium-mcp/google-api.env"
if [[ -f "${API_ENV}" ]]; then
    while IFS='=' read -r key value; do
        [[ -z "$key" || "$key" == \#* ]] && continue
        export "${key}=${value}"
    done < "${API_ENV}"
fi

# Chromium 경로 결정
CHROMIUM="${CHROMIUM_MCP_BROWSER_PATH:-/Users/daniel/chromium/src/out/Default/Chromium.app/Contents/MacOS/Chromium}"

exec "${CHROMIUM}" --no-first-run --disable-default-apps "$@"
