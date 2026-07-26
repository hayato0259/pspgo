#!/usr/bin/env python3
"""PSP 向け YouTube Music アプリサーバー.

PSP クライアントの制約 (HTTP/1.0・MP3 CBR・JSON パーサなし) に合わせて、
すべてのメタデータをタブ区切りテキスト (TSV) で返す。

エンドポイント:
  GET /api/status            認証状態:  auth\t0|1 / name\t<表示名> / can_login\t0|1
  GET /api/home              ホーム:    section\t<題> / playlist\t<id>\t<題>\t<副題> / video\t<id>\t<題>\t<アーティスト>
  GET /api/playlist?id=<id>  内容:      meta\t<題> / track\t<videoId>\t<題>\t<アーティスト>\t<秒>
  GET /api/login/start       ログイン開始: code\t<入力コード> / url\t<URL> / interval\t<秒>
  GET /api/login/poll        ログイン待ち: state\tpending|ok  (失敗時 error\t<理由>)
  GET /api/logout            トークン破棄
  GET /stream?yt=<videoId>   音声を MP3 CBR 128kbps で配信
  GET /stream?file=<path>    ローカルファイルを変換して配信 (検証用)

ログイン: Google 公式の OAuth デバイスコードフロー (テレビ・ゲーム機と同じ方式) を使う。
PSP アプリが表示するコードを、手元のスマートフォンや PC で入力するとログインが完了する。
TLS 通信とトークン保管はこのサーバーが担う。事前に Google Cloud で
「テレビと入力制限のあるデバイス」種別の OAuth クライアントを作り、
auth/oauth_client.json に client_id / client_secret を置く (README 参照)。

依存: ffmpeg, yt-dlp, ytmusicapi (.venv)
使い方: .venv/bin/python app.py [port]   (デフォルト 8080)
"""

import json
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from ytmusicapi import OAuthCredentials, YTMusic
from ytmusicapi.auth.oauth import RefreshingToken

BITRATE = "128k"
CHUNK = 16 * 1024

AUTH_DIR = Path(__file__).parent / "auth"
CLIENT_FILE = AUTH_DIR / "oauth_client.json"   # client_id / client_secret (自分で作る)
TOKEN_FILE = AUTH_DIR / "oauth.json"           # ログイン後に自動生成されるトークン
BROWSER_FILE = AUTH_DIR / "browser.json"       # 旧方式 (ブラウザヘッダ) も引き続き使える

_yt = None
_yt_lock = threading.Lock()


def load_client() -> tuple:
    """OAuth クライアント情報を返す。未設定なら (None, None)。"""
    if CLIENT_FILE.exists():
        data = json.loads(CLIENT_FILE.read_text(encoding="utf-8"))
        # Google Cloud からダウンロードした JSON (installed/web でネストする形) も許容
        inner = data.get("installed") or data.get("web") or data
        cid, secret = inner.get("client_id"), inner.get("client_secret")
        if cid and secret:
            return cid, secret
    return None, None


def can_login() -> bool:
    return load_client()[0] is not None


def credentials() -> OAuthCredentials:
    cid, secret = load_client()
    if not cid:
        raise RuntimeError(
            "OAuth クライアント未設定 (auth/oauth_client.json)。README の手順を参照"
        )
    return OAuthCredentials(client_id=cid, client_secret=secret)


def reset_yt():
    global _yt
    with _yt_lock:
        _yt = None


def get_yt() -> YTMusic:
    global _yt
    with _yt_lock:
        if _yt is None:
            if TOKEN_FILE.exists() and can_login():
                _yt = YTMusic(str(TOKEN_FILE), oauth_credentials=credentials())
            elif BROWSER_FILE.exists():
                _yt = YTMusic(str(BROWSER_FILE))
            else:
                _yt = YTMusic()
        return _yt


_yt_public = None


def get_yt_public() -> YTMusic:
    """未認証クライアント。認証済みクライアントが失敗したときの退避先。"""
    global _yt_public
    with _yt_lock:
        if _yt_public is None:
            _yt_public = YTMusic()
        return _yt_public


def with_fallback(fn, what: str):
    """認証済みクライアントで試し、失敗したら未認証クライアントで再試行する。

    2026-07 時点で、OAuth トークンは YouTube Music の内部 API から
    全エンドポイント 400 で拒否される (ytmusicapi 側の既知の制約)。
    ログインした結果アプリが使えなくなるのを避けるため、
    ここで一般向けの内容に退避する。
    戻り値: (結果, 退避したか)
    """
    if is_authed():
        try:
            return fn(get_yt()), False
        except Exception as e:
            print(f"[server] 認証クライアントで{what}に失敗 → 一般向けに退避: {e}",
                  flush=True)
    return fn(get_yt_public()), is_authed()


def is_authed() -> bool:
    return (TOKEN_FILE.exists() and can_login()) or BROWSER_FILE.exists()


# --- ログイン (デバイスコードフロー) -------------------------------------

_login = {"device_code": None, "expires_at": 0}
_login_lock = threading.Lock()


def qr_line(url: str) -> str:
    """QR コードを 1 行の TSV で返す: qr\t<辺のモジュール数>\t<0/1 の羅列>

    PSP 側に画像デコーダを持たせないため、白黒モジュールをそのまま送る。
    qrcode パッケージが無ければ空文字を返す (アプリはコード入力のみで動く)。
    """
    try:
        import qrcode
    except ImportError:
        return ""
    qr = qrcode.QRCode(
        error_correction=qrcode.constants.ERROR_CORRECT_L, box_size=1, border=0
    )
    qr.add_data(url)
    qr.make(fit=True)
    matrix = qr.get_matrix()
    size = len(matrix)
    if size > 64:  # PSP 側のバッファ上限
        return ""
    flat = "".join("1" if cell else "0" for row in matrix for cell in row)
    return f"qr\t{size}\t{flat}\n"


def login_start() -> str:
    """Google からデバイスコードを取得し、PSP に見せる情報を返す。"""
    creds = credentials()
    code = creds.get_code()
    with _login_lock:
        _login["device_code"] = code["device_code"]
        _login["expires_at"] = time.time() + int(code.get("expires_in", 1800))

    # user_code を付けた URL にしておくと、QR から開いた時点でコードが入力済みになる
    verify_url = code["verification_url"]
    deep_url = f"{verify_url}?user_code={code['user_code']}"

    # 画面が見えない環境 (ヘッドレス検証や実機デバッグ) でも承認できるよう記録する。
    # user_code は本人に見せるための値で、秘密情報ではない。
    print(f"[server] ログイン待ち: {deep_url}", flush=True)

    return (
        f"code\t{clean(code['user_code'])}\n"
        f"url\t{clean(verify_url)}\n"
        f"interval\t{int(code.get('interval', 5))}\n"
        f"expires\t{int(code.get('expires_in', 1800))}\n"
        + qr_line(deep_url)
    )


def login_poll() -> str:
    """1 回だけトークン交換を試す。まだなら pending を返す。"""
    with _login_lock:
        device_code = _login["device_code"]
        expires_at = _login["expires_at"]
    if not device_code:
        return "error\tログインが開始されていません\n"
    if time.time() > expires_at:
        return "error\tコードの有効期限切れ。やり直してください\n"

    creds = credentials()
    raw = creds.token_from_code(device_code)

    if "error" in raw:
        reason = raw["error"]
        if reason in ("authorization_pending", "slow_down"):
            return "state\tpending\n"
        if reason == "access_denied":
            return "error\tユーザーが許可を拒否しました\n"
        return f"error\t{clean(reason)}\n"

    # 公式 CLI (prompt_for_token) と同じ手順でトークンを組み立てて保存する
    expires_in = raw.get("refresh_token_expires_in", raw["expires_in"])
    token = RefreshingToken(
        credentials=creds,
        access_token=raw["access_token"],
        refresh_token=raw["refresh_token"],
        scope=raw["scope"],
        token_type=raw["token_type"],
        expires_in=expires_in,
    )
    token.update(raw)
    AUTH_DIR.mkdir(parents=True, exist_ok=True)
    token.local_cache = TOKEN_FILE   # setter が store_token() を呼ぶ

    with _login_lock:
        _login["device_code"] = None
    reset_yt()
    return "state\tok\n"


def logout() -> str:
    if TOKEN_FILE.exists():
        TOKEN_FILE.unlink()
    reset_yt()
    return "state\tok\n"


def clean(s) -> str:
    """TSV を壊す文字を除去する。"""
    if s is None:
        return ""
    return str(s).replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()


def artists_of(item) -> str:
    names = [a.get("name", "") for a in item.get("artists") or [] if a.get("name")]
    return ", ".join(n for n in names if n)


def parse_len(s) -> int:
    """"3:45" -> 225。パース不能は 0。"""
    try:
        parts = [int(p) for p in str(s).split(":")]
        sec = 0
        for p in parts:
            sec = sec * 60 + p
        return sec
    except (ValueError, TypeError):
        return 0


def ffmpeg_cmd(src: str) -> list:
    return [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-i", src,
        "-vn",
        "-c:a", "libmp3lame", "-b:a", BITRATE, "-ar", "44100", "-ac", "2",
        "-f", "mp3", "pipe:1",
    ]


def tsv_home() -> str:
    sections, fellback = with_fallback(lambda yt: yt.get_home(limit=6), "ホーム取得")
    lines = []
    if fellback:
        lines.append("section\t※ ログイン中ですが一般向けの内容を表示しています")
    for section in sections:
        lines.append(f"section\t{clean(section.get('title'))}")
        for item in section.get("contents", []):
            title = clean(item.get("title"))
            if item.get("playlistId"):
                sub = clean(item.get("description")) or clean(artists_of(item))
                lines.append(f"playlist\t{clean(item['playlistId'])}\t{title}\t{sub}")
            elif item.get("videoId"):
                lines.append(f"video\t{clean(item['videoId'])}\t{title}\t{clean(artists_of(item))}")
    return "\n".join(lines) + "\n"


def tsv_playlist(pid: str) -> str:
    def fetch(yt):
        try:
            pl = yt.get_playlist(pid, limit=100)
            return clean(pl.get("title")), pl.get("tracks", [])
        except Exception:
            # ラジオ/ミックス系プレイリストは get_watch_playlist で取れる
            watch = yt.get_watch_playlist(playlistId=pid, limit=100)
            return clean(watch.get("title")) or "Mix", watch.get("tracks", [])

    (title, tracks), _ = with_fallback(fetch, "プレイリスト取得")
    lines = [f"meta\t{title}"]

    for t in tracks:
        vid = t.get("videoId")
        if not vid:
            continue
        dur = t.get("duration_seconds") or parse_len(t.get("length") or t.get("duration"))
        lines.append(
            f"track\t{clean(vid)}\t{clean(t.get('title'))}\t{clean(artists_of(t))}\t{dur}"
        )
    return "\n".join(lines) + "\n"


def tsv_status() -> str:
    name = "-"
    if is_authed():
        try:
            info = get_yt().get_account_info()
            name = clean(info.get("accountName")) or "-"
        except Exception:
            pass
    return (
        f"auth\t{1 if is_authed() else 0}\n"
        f"name\t{name}\n"
        f"can_login\t{1 if can_login() else 0}\n"
    )


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[server] %s\n" % (fmt % args))

    def _text(self, body: str, code: int = 200, ctype: str = "text/tab-separated-values"):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", f"{ctype}; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _stream_process(self, procs, tail):
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.end_headers()
        sent = 0
        try:
            while True:
                buf = tail.stdout.read(CHUNK)
                if not buf:
                    break
                self.wfile.write(buf)
                sent += len(buf)
        except (BrokenPipeError, ConnectionResetError):
            self.log_message("client disconnected after %d bytes", sent)
        finally:
            for p in procs:
                if p.poll() is None:
                    p.terminate()
            self.log_message("stream done: %d bytes", sent)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)

        try:
            if url.path == "/api/status":
                self._text(tsv_status())
                return
            if url.path == "/api/home":
                self._text(tsv_home())
                return
            if url.path == "/api/playlist" and "id" in q:
                self._text(tsv_playlist(q["id"][0]))
                return
        except Exception as e:
            # 保存済みトークンが失効・無効化されていると Google が 401 を返す。
            # 生のエラーを出す代わりに再ログインを促す (トークンは消さない)。
            if TOKEN_FILE.exists() and "401" in str(e):
                self.log_message("token invalid -> reauth: %r", e)
                self._text("error\tログインの有効期限が切れました\nreauth\t1\n", code=200)
                return
            self.log_message("api error: %r", e)
            self._text(f"error\t{clean(e)}\n", code=502)
            return

        try:
            if url.path == "/api/login/start":
                self._text(login_start())
                return
            if url.path == "/api/login/poll":
                self._text(login_poll())
                return
            if url.path == "/api/logout":
                self._text(logout())
                return
        except Exception as e:
            self.log_message("api error: %r", e)
            self._text(f"error\t{clean(e)}\n", code=502)
            return

        if url.path == "/stream" and "file" in q:
            ff = subprocess.Popen(ffmpeg_cmd(q["file"][0]), stdout=subprocess.PIPE)
            self._stream_process([ff], ff)
            return

        if url.path == "/stream" and "yt" in q:
            target = q["yt"][0]
            if not target.startswith("http"):
                target = f"https://music.youtube.com/watch?v={target}"
            ydl = subprocess.Popen(
                ["yt-dlp", "-q", "-f", "bestaudio", "-o", "-", target],
                stdout=subprocess.PIPE,
            )
            ff = subprocess.Popen(
                ffmpeg_cmd("pipe:0"), stdin=ydl.stdout, stdout=subprocess.PIPE
            )
            ydl.stdout.close()
            self._stream_process([ydl, ff], ff)
            return

        self._text("error\tnot found\n", code=404)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    for tool in ("ffmpeg", "yt-dlp"):
        if not shutil.which(tool):
            sys.exit(f"{tool} is required (brew install {tool})")
    print(f"[server] auth: {'browser.json' if is_authed() else 'なし (一般向けホーム)'}")
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"[server] listening on 0.0.0.0:{port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
