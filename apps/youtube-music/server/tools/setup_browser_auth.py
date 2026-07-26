#!/usr/bin/env python3
"""ブラウザ認証 (auth/browser.json) を作るツール.

Chrome の DevTools からコピーしたリクエスト情報をファイルに保存しておき、
このツールに渡すと auth/browser.json を作る。
Cookie の値を画面に出したり、チャットに貼ったりせずに設定できる。

対応する入力形式:
  1. DevTools の「Copy as fetch」で得られる JavaScript のスニペット
  2. 「name: value」形式の生ヘッダー (Copy request headers)

使い方:
  1. music.youtube.com を開いた状態で DevTools の Network タブを開く
  2. `browse` などのリクエストを右クリック → Copy → Copy as fetch
  3. 適当なファイルに保存する (例: ~/ytm-headers.txt)
  4. .venv/bin/python tools/setup_browser_auth.py ~/ytm-headers.txt
  5. 保存したファイルは削除する (認証情報が入っている)

生成物は auth/browser.json (.gitignore 済み)。
Cookie が漏れると第三者がアカウントの YouTube Music を操作できるため、
入力ファイルは作業後に必ず削除すること。
"""

import json
import re
import sys
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parent.parent
AUTH_DIR = SERVER_DIR / "auth"
OUT_FILE = AUTH_DIR / "browser.json"

# ytmusicapi が必要とするもの以外は渡さない (余計な情報を保存しないため)
KEEP = {
    "cookie", "user-agent", "accept", "accept-language", "accept-encoding",
    "content-type", "origin", "referer", "authorization",
    "x-goog-authuser", "x-goog-visitor-id", "x-origin",
    "x-youtube-client-name", "x-youtube-client-version",
}


def headers_from_fetch(text: str) -> dict:
    """Copy as fetch のスニペットから headers オブジェクトを取り出す。"""
    m = re.search(r'"headers"\s*:\s*\{', text)
    if not m:
        return {}
    # 対応する閉じ括弧まで数える
    start = text.index("{", m.end() - 1)
    depth, i = 0, start
    while i < len(text):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                break
        i += 1
    try:
        return json.loads(text[start:i + 1])
    except json.JSONDecodeError:
        return {}


def headers_from_raw(text: str) -> dict:
    """「name: value」形式の行からヘッダーを取り出す。"""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or ":" not in line:
            continue
        name, _, value = line.partition(":")
        name = name.strip().lower()
        if name and value.strip() and not name.startswith(":"):
            out[name] = value.strip()
    return out


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    src = Path(sys.argv[1]).expanduser()
    if not src.exists():
        sys.exit(f"ファイルがありません: {src}")

    text = src.read_text(encoding="utf-8", errors="replace")
    headers = headers_from_fetch(text) or headers_from_raw(text)
    if not headers:
        sys.exit("ヘッダーを読み取れませんでした。Copy as fetch の内容を保存してください")

    headers = {k.lower(): v for k, v in headers.items() if k.lower() in KEEP}

    if "cookie" not in headers:
        sys.exit("Cookie が見つかりません。music.youtube.com にログインした状態の"
                 "リクエストをコピーしてください")

    # 値そのものは出さず、何が取れたかだけ報告する
    print("読み取れたヘッダー:")
    for k in sorted(headers):
        print(f"  {k}  ({len(headers[k])} 文字)")

    sys.path.insert(0, str(SERVER_DIR))
    from ytmusicapi import setup as ytm_setup

    AUTH_DIR.mkdir(parents=True, exist_ok=True)
    raw = "\n".join(f"{k}: {v}" for k, v in headers.items())
    ytm_setup(filepath=str(OUT_FILE), headers_raw=raw)
    print(f"\n作成しました: {OUT_FILE}")

    # 実際に使えるか確認する
    from ytmusicapi import YTMusic
    try:
        yt = YTMusic(str(OUT_FILE))
        sections = yt.get_home(limit=3)
        print(f"\n認証の確認: OK  ホームのセクション {len(sections)} 件")
        for s in sections[:6]:
            print(f"  ▸ {s.get('title')}")
    except Exception as e:
        print(f"\n認証の確認: 失敗 ({e})")
        print("  Cookie が古い可能性があります。ブラウザで再読み込みしてから"
              "もう一度コピーしてください")
        return

    print(f"\n{src} は認証情報を含むので削除してください:")
    print(f"  rm {src}")


if __name__ == "__main__":
    main()
