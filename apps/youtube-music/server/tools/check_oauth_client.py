#!/usr/bin/env python3
"""OAuth クライアント設定の健康診断.

auth/oauth_client.json が正しく置かれているか、そして
その資格情報で実際に Google のデバイスコードフローが開始できるかを確認する。

client_secret は画面に出さない (先頭数文字のみ表示)。

使い方: .venv/bin/python tools/check_oauth_client.py
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import app  # noqa: E402


def fail(msg: str, hint: str = "") -> None:
    print(f"NG  {msg}")
    if hint:
        print(f"    → {hint}")
    sys.exit(1)


def main() -> None:
    print(f"設定ファイル: {app.CLIENT_FILE}")

    if not app.CLIENT_FILE.exists():
        fail(
            "auth/oauth_client.json がありません",
            "oauth_client.example.json をコピーして値を入れてください",
        )

    try:
        raw = json.loads(app.CLIENT_FILE.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        fail(f"JSON として読めません: {e}", "括弧やカンマの記述を確認してください")

    inner = raw.get("installed") or raw.get("web") or raw
    cid = inner.get("client_id")
    secret = inner.get("client_secret")

    if not cid or not secret:
        fail(
            "client_id / client_secret が見つかりません",
            f"見つかったキー: {sorted(inner.keys())}",
        )
    if "ここに" in str(cid) or "ここに" in str(secret):
        fail("サンプルの値がそのまま残っています", "実際の値に置き換えてください")
    if not str(cid).endswith(".apps.googleusercontent.com"):
        print("!!  client_id が .apps.googleusercontent.com で終わっていません")
        print("    → 値を貼り間違えていないか確認してください")

    print(f"OK  client_id: {cid}")
    print(f"OK  client_secret: {str(secret)[:4]}... ({len(str(secret))} 文字)")

    if raw.get("web"):
        print("!!  種別が 'web' のクライアントに見えます")
        print("    → デバイスコードフローには「テレビと入力が制限されているデバイス」")
        print("      種別のクライアントが必要です。作り直してください")

    print("\nGoogle に接続して、実際にログインを開始できるか試します...")
    try:
        code = app.credentials().get_code()
    except Exception as e:
        msg = str(e)
        if "invalid_client" in msg:
            fail(
                "Google がクライアントを認識しませんでした (invalid_client)",
                "client_id/secret の取り違え、または "
                "YouTube Data API v3 が有効化されていない可能性があります",
            )
        fail(f"デバイスコードの取得に失敗しました: {e}")

    print("OK  デバイスコードを取得できました。設定は有効です。")
    print(f"    入力コード : {code.get('user_code')}")
    print(f"    URL        : {code.get('verification_url')}")
    print(f"    有効時間   : {code.get('expires_in')} 秒")
    print("\nこのコードは検証用なので、承認しなくて構いません。")
    print("PSP アプリからログインすると、同じ流れが QR コードで表示されます。")


if __name__ == "__main__":
    main()
