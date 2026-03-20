#!/usr/bin/env python3
"""MCP 멀티 클라이언트 소켓 연결 테스트.

3개의 클라이언트가 동시에 MCP 소켓에 연결하여
각각 독립적으로 핸드셰이크 및 도구 호출을 수행한다.
"""
import socket
import json
import sys
import threading
import time

SOCKET_PATH = "/tmp/.chromium-mcp.sock"


def send_msg(sock, obj):
    body = json.dumps(obj)
    header = f"Content-Length: {len(body)}\r\n\r\n"
    sock.sendall((header + body).encode())


def recv_msg(sock):
    header = b""
    while b"\r\n\r\n" not in header:
        chunk = sock.recv(1)
        if not chunk:
            raise ConnectionError("소켓 EOF")
        header += chunk
    length = int(header.decode().split(": ")[1].split("\r\n")[0])
    body = b""
    while len(body) < length:
        chunk = sock.recv(length - len(body))
        if not chunk:
            raise ConnectionError("소켓 EOF (body)")
        body += chunk
    return json.loads(body.decode())


def run_client(client_name, client_id):
    """단일 MCP 클라이언트 세션 실행."""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCKET_PATH)
        print(f"[{client_name}] 연결 성공")

        # 1. Initialize
        send_msg(s, {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": client_name, "version": "1.0"}
            }
        })
        resp = recv_msg(s)
        server_name = resp.get("result", {}).get("serverInfo", {}).get("name", "?")
        print(f"[{client_name}] initialize 응답 수신 — 서버: {server_name}")

        # 2. Initialized 알림
        send_msg(s, {"jsonrpc": "2.0", "method": "notifications/initialized"})
        print(f"[{client_name}] initialized 알림 전송")

        time.sleep(0.5)  # 핸드셰이크 완료 대기

        # 3. tools/list 요청
        send_msg(s, {
            "jsonrpc": "2.0",
            "id": 2,
            "method": "tools/list",
            "params": {}
        })
        resp = recv_msg(s)
        tools = resp.get("result", {}).get("tools", [])
        print(f"[{client_name}] tools/list 응답: {len(tools)}개 도구")

        # 4. browser_info 도구 호출
        send_msg(s, {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {
                "name": "browser_info",
                "arguments": {}
            }
        })
        resp = recv_msg(s)
        content = resp.get("result", {}).get("content", [{}])
        text = content[0].get("text", "?") if content else "?"
        # 텍스트가 길면 앞 80자만
        short_text = text[:80] + "..." if len(text) > 80 else text
        print(f"[{client_name}] browser_info 결과: {short_text}")

        print(f"[{client_name}] ✅ 테스트 성공!")
        s.close()
        return True

    except Exception as e:
        print(f"[{client_name}] ❌ 오류: {e}")
        return False


def main():
    print("=" * 60)
    print("MCP 멀티 클라이언트 소켓 테스트")
    print("=" * 60)

    # 3개 클라이언트를 스레드로 병렬 실행
    threads = []
    results = [None, None, None]

    for i in range(3):
        name = f"client-{i+1}"
        t = threading.Thread(target=lambda idx=i, n=name: results.__setitem__(idx, run_client(n, idx)))
        threads.append(t)

    # 약간의 시간차를 두고 시작
    for i, t in enumerate(threads):
        t.start()
        time.sleep(0.3)

    for t in threads:
        t.join(timeout=15)

    print()
    print("=" * 60)
    success_count = sum(1 for r in results if r)
    print(f"결과: {success_count}/3 클라이언트 성공")
    if success_count == 3:
        print("🎉 멀티 클라이언트 지원 동작 확인!")
    else:
        print("⚠️  일부 클라이언트 실패")
    print("=" * 60)

    sys.exit(0 if success_count == 3 else 1)


if __name__ == "__main__":
    main()
