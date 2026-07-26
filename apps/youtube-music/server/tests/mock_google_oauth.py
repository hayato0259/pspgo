#!/usr/bin/env python3
"""ログインフロー検証用のテスト用フィクスチャ.

Google の OAuth デバイスコードエンドポイントを模したモックを立て、
app.py の OAuth URL をそこへ向けたうえで本物の app.py を起動する。
実際の Google アカウントを使わずに、
「コード取得 → 承認待ち (pending) → 承認完了 (ok)」の状態遷移を検証できる。

本番では使わない。PSP クライアント側の実装検証専用。

使い方: .venv/bin/python tests/mock_google_oauth.py [pending回数] [port]
"""

import json
import sys
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

PENDING_TIMES = int(sys.argv[1]) if len(sys.argv) > 1 else 2
APP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
MOCK_PORT = 8099

_poll_count = 0
_lock = threading.Lock()


class MockGoogle(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock-google] %s\n" % (fmt % args))

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        global _poll_count
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length).decode()
        form = urllib.parse.parse_qs(raw)

        if self.path.endswith("/device/code"):
            self._json({
                "device_code": "MOCK-DEVICE-CODE",
                "user_code": "ABC-DEF-GHJ",
                "expires_in": 1800,
                "interval": 2,
                "verification_url": "https://www.google.com/device",
            })
            return

        if self.path.endswith("/token"):
            # grant_type が device flow のものであることだけ確認
            assert "device" in form.get("grant_type", [""])[0], form
            with _lock:
                _poll_count += 1
                n = _poll_count
            if n <= PENDING_TIMES:
                self.log_message("poll %d -> authorization_pending", n)
                self._json({"error": "authorization_pending"}, code=428)
            else:
                self.log_message("poll %d -> token granted", n)
                self._json({
                    "access_token": "mock-access-token",
                    "refresh_token": "mock-refresh-token",
                    "scope": "https://www.googleapis.com/auth/youtube",
                    "token_type": "Bearer",
                    "expires_in": 3600,
                })
            return

        self._json({"error": "unexpected_path"}, code=404)


def main():
    mock = ThreadingHTTPServer(("127.0.0.1", MOCK_PORT), MockGoogle)
    threading.Thread(target=mock.serve_forever, daemon=True).start()
    print(f"[mock-google] listening on 127.0.0.1:{MOCK_PORT}")

    # ytmusicapi は import 時に定数を取り込むので、モジュール属性を差し替える
    import ytmusicapi.auth.oauth.credentials as creds_mod
    creds_mod.OAUTH_CODE_URL = f"http://127.0.0.1:{MOCK_PORT}/o/oauth2/device/code"
    creds_mod.OAUTH_TOKEN_URL = f"http://127.0.0.1:{MOCK_PORT}/token"

    import app

    # テスト用のダミークライアント資格情報を用意する
    app.AUTH_DIR.mkdir(parents=True, exist_ok=True)
    app.CLIENT_FILE.write_text(json.dumps({
        "client_id": "mock-client-id.apps.googleusercontent.com",
        "client_secret": "mock-client-secret",
    }), encoding="utf-8")
    if app.TOKEN_FILE.exists():
        app.TOKEN_FILE.unlink()

    # ログイン後の YTMusic 生成はモックトークンでは実際の API を叩けないため、
    # メタデータは未認証クライアントで取得する (画面遷移の検証が目的)
    app.get_yt = app.YTMusic

    sys.argv = ["app.py", str(APP_PORT)]
    print(f"[mock-google] pending {PENDING_TIMES} kai -> sono go ok")
    app.main()


if __name__ == "__main__":
    main()
