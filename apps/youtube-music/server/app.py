#!/usr/bin/env python3
"""PSP 向け YouTube Music アプリサーバー.

PSP クライアントの制約 (HTTP/1.0・MP3 CBR・JSON パーサなし) に合わせて、
すべてのメタデータをタブ区切りテキスト (TSV) で返す。

エンドポイント:
  GET /api/status            認証状態:  auth\t0|1 / name\t<表示名> / can_login\t0|1
  GET /api/home              ホーム:    section\t<題> / playlist\t<id>\t<題>\t<副題> / video\t<id>\t<題>\t<アーティスト>
  GET /api/search?q=<検索語> 検索:      section\t<題> / video\t... / playlist\t...
  GET /api/playlist?id=<id>  内容:      meta\t<題> / track\t<videoId>\t<題>\t<アーティスト>\t<秒>
  GET /api/radio?yt=<videoId> ラジオ:   meta\tラジオ / track\t<videoId>\t<題>\t<アーティスト>\t<秒>
  GET /api/lyrics?yt=<videoId> 歌詞:    line\t<歌詞1行> / none\t1
  GET /api/login/start       ログイン開始: code\t<入力コード> / url\t<URL> / interval\t<秒>
  GET /api/login/poll        ログイン待ち: state\tpending|ok  (失敗時 error\t<理由>)
  GET /api/logout            トークン破棄
  GET /stream?yt=<videoId>&t=<秒>  音声を MP3 CBR 128kbps で配信 (t = 頭出し位置)
  GET /video?yt=<videoId>&sec=<長さ>&t=<秒>  映像を PSMF (H.264 Baseline 480x272) で配信
  GET /stream?file=<path>    ローカルファイルを変換して配信 (検証用)

ログイン: Google 公式の OAuth デバイスコードフロー (テレビ・ゲーム機と同じ方式) を使う。
PSP アプリが表示するコードを、手元のスマートフォンや PC で入力するとログインが完了する。
TLS 通信とトークン保管はこのサーバーが担う。事前に Google Cloud で
「テレビと入力制限のあるデバイス」種別の OAuth クライアントを作り、
auth/oauth_client.json に client_id / client_secret を置く (README 参照)。

依存: ffmpeg, yt-dlp, ytmusicapi (.venv)
使い方: .venv/bin/python app.py [port]   (デフォルト 8080)
"""

import hmac
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

# yt-dlp を venv に入れている環境 (Raspberry Pi 等) では、PATH を通さずに
# .venv/bin/python app.py と起動すると yt-dlp が見つからない。
# 自分と同じ場所にある実行ファイルを探せるよう PATH の先頭に足しておく。
os.environ["PATH"] = os.pathsep.join(
    [str(Path(sys.executable).parent), os.environ.get("PATH", "")]
)

from ytmusicapi import OAuthCredentials, YTMusic
from ytmusicapi.auth.oauth import RefreshingToken

BITRATE = "128k"
CHUNK = 16 * 1024

AUTH_DIR = Path(__file__).parent / "auth"
CLIENT_FILE = AUTH_DIR / "oauth_client.json"   # client_id / client_secret (自分で作る)
TOKEN_FILE = AUTH_DIR / "oauth.json"           # ログイン後に自動生成されるトークン
BROWSER_FILE = AUTH_DIR / "browser.json"       # 旧方式 (ブラウザヘッダ) も引き続き使える
ACCESS_TOKEN_FILE = AUTH_DIR / "token.txt"     # 外部公開用の共有トークン

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
    """認証済みクライアント。

    browser.json を OAuth トークンより優先する。
    2026-07 時点で OAuth トークンは YouTube Music の内部 API から
    全エンドポイント 400 で拒否されるが、ブラウザ認証は正常に動作し、
    マイミックス等のパーソナライズされた内容も取得できる
    (上流の既知の問題: sigma67/ytmusicapi#676, #921)。
    """
    global _yt
    with _yt_lock:
        if _yt is None:
            if BROWSER_FILE.exists():
                _yt = YTMusic(str(BROWSER_FILE), language="ja", location="JP")
            elif TOKEN_FILE.exists() and can_login():
                _yt = YTMusic(str(TOKEN_FILE), oauth_credentials=credentials(),
                              language="ja", location="JP")
            else:
                _yt = YTMusic(language="ja", location="JP")
        return _yt


_yt_public = None


def get_yt_public() -> YTMusic:
    """未認証クライアント。認証済みクライアントが失敗したときの退避先。"""
    global _yt_public
    with _yt_lock:
        if _yt_public is None:
            _yt_public = YTMusic(language="ja", location="JP")
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
    return BROWSER_FILE.exists() or (TOKEN_FILE.exists() and can_login())


def auth_kind() -> str:
    if BROWSER_FILE.exists():
        return "browser"
    if TOKEN_FILE.exists() and can_login():
        return "oauth"
    return "none"


# --- ログイン (デバイスコードフロー) -------------------------------------

_login = {"device_code": None, "expires_at": 0}
_login_lock = threading.Lock()


# --- アートワーク配信 ------------------------------------------------------
#
# PSP に画像デコーダを持たせないため、サーバー側で縮小して
# 生ピクセル (RGBA 8888) にしてから送る。PSP のテクスチャは辺が 2 の冪
# である必要があるので、正方形の固定サイズに揃える。

ART_SIZE = 64
_art_urls: dict = {}     # id -> サムネイル URL
_art_cache: dict = {}    # (id, size) -> 生ピクセル
_art_lock = threading.Lock()


def remember_art(item_id: str, thumbnails) -> None:
    """一覧を作るときにサムネイル URL を覚えておく。"""
    if not item_id or not thumbnails:
        return
    # 必要サイズ以上で最小のものを選ぶ (無ければ最大)
    usable = [t for t in thumbnails if t.get("width", 0) >= ART_SIZE]
    chosen = min(usable, key=lambda t: t["width"]) if usable else \
        max(thumbnails, key=lambda t: t.get("width", 0))
    url = chosen.get("url")
    if url:
        with _art_lock:
            _art_urls[item_id] = url


def art_pixels(item_id: str, size: int) -> bytes:
    """id に対応するアートワークを size x size の RGBA 生ピクセルで返す。"""
    key = (item_id, size)
    with _art_lock:
        if key in _art_cache:
            return _art_cache[key]
        url = _art_urls.get(item_id)
    if not url:
        raise KeyError("アートワークの URL が不明です (先に一覧を取得してください)")

    import io
    from PIL import Image
    import requests

    res = requests.get(url, timeout=15)
    res.raise_for_status()
    img = Image.open(io.BytesIO(res.content)).convert("RGBA")
    img = img.resize((size, size), Image.LANCZOS)

    # 角丸 (PC 版 YouTube Music のカードと同じ見た目)。
    # 角のアルファを 0 にして送ると、PSP 側はブレンドだけで角丸になる
    from PIL import ImageDraw
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, size - 1, size - 1], radius=max(3, size // 8), fill=255)
    img.putalpha(mask)

    data = img.tobytes()   # R,G,B,A の並び = PSP の GU_PSM_8888 と同じ

    with _art_lock:
        if len(_art_cache) > 256:
            _art_cache.clear()
        _art_cache[key] = data
    return data


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


def clean(s, limit: int = 88) -> str:
    """TSV を壊す文字を除去し、UTF-8 の文字境界で切り詰める。

    PSP 側のバッファは固定長で、途中で切れた UTF-8 を渡すと
    フォント描画側が終端を読み飛ばして隣の領域まで描いてしまう。
    そのため文字数ではなくバイト数で、必ず文字の境界で切る。
    """
    if s is None:
        return ""
    t = str(s).replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()
    raw = t.encode("utf-8")
    if len(raw) <= limit:
        return t
    # limit バイトに収めたうえで、壊れた末尾のバイトを捨てる
    return raw[:limit].decode("utf-8", "ignore").rstrip() + "…"


def artists_of(item) -> str:
    names = [a.get("name", "") for a in item.get("artists") or [] if a.get("name")]
    # ミュージックビデオの項目には再生回数や高評価数がアーティストと同じ並びで
    # 入ってくる (「17億回視聴」「高評価 1928万 件」など)。PSP の狭い画面では
    # 邪魔になるだけなので落とす。
    noise = ("回視聴", "高評価", " views", " likes")
    names = [n for n in names if not any(k in n for k in noise)]
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


def ffmpeg_cmd(src: str, start_sec: int = 0) -> list:
    seek = ["-ss", str(start_sec)] if start_sec > 0 else []
    return [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-i", src,
        *seek,          # 入力の後に置く = デコードして読み飛ばす形の頭出し
        "-vn",
        "-c:a", "libmp3lame", "-b:a", BITRATE, "-ar", "44100", "-ac", "2",
        "-f", "mp3", "pipe:1",
    ]


# ホームで返すセクション数と、1 セクションあたりの項目数の上限。
# 本家のホームは 20 以上のセクションがあり、6 で切ると
# 「おすすめのミュージック ビデオ」などが丸ごと落ちていた。
# 一方で全部返すと PSP 側の配列と通信量に響くため、両方に上限を置く。
HOME_SECTIONS = 20
HOME_ITEMS_PER_SECTION = 12


# --- 映像配信 (PSMF) ------------------------------------------------------
#
# PSP の映像デコーダは生の H.264 を受け付けず、Sony 独自コンテナ PSMF を要求する。
# 中身は「2048 バイトのヘッダ + MPEG プログラムストリーム」で、後半は ffmpeg が作れる。
# ここでやるのは、ヘッダを組み立てて先頭に付けることと、
# 映像のストリーム ID を PSP が期待する 0xE0 に直すことの 2 つだけ。
#
# 音声は PSMF に入れない。PSMF の音声は Atrac3+ 固定で ffmpeg に実装が無いため。
# 音声は従来どおり /stream から MP3 で配る (PSP 側で別々に再生する)。
# 詳細: docs/verification-video.md

VIDEO_W = 480          # PSP の画面
VIDEO_H = 272
VIDEO_FPS = 24         # 30 だと Media Engine が間に合わない可能性があるため控えめに
VIDEO_BITRATE = 400_000
PSMF_HEADER_SIZE = 2048
PACK_SIZE = 2048
PTS_HZ = 90000
PSMF_VIDEO_STREAM_ID = 0xE0


def psmf_header(stream_size: int, seconds: int) -> bytes:
    """PSMF ヘッダ (2048 バイト) を組み立てる。

    配信しながら作るので実際の長さは分からない。長さは呼び出し側が渡す
    再生時間からの見積もりでよい (PSP 側はデータが尽きた時点で終わる)。
    """
    import struct
    h = bytearray(PSMF_HEADER_SIZE)
    h[0:4] = b"PSMF"
    h[4:8] = b"0015"
    struct.pack_into(">I", h, 0x08, PSMF_HEADER_SIZE)      # 本体の開始位置
    struct.pack_into(">I", h, 0x0C, stream_size)           # 本体の長さ
    first = PTS_HZ                                          # 慣例として 1 秒から始める
    last = first + max(1, seconds) * PTS_HZ
    h[0x54:0x5A] = struct.pack(">Q", first)[2:]            # 最初の表示時刻
    h[0x5A:0x60] = struct.pack(">Q", last)[2:]             # 最後の表示時刻
    struct.pack_into(">H", h, 0x80, 1)                     # ストリーム数
    e = 0x82                                               # ストリーム表 (16 バイト)
    h[e] = PSMF_VIDEO_STREAM_ID
    struct.pack_into(">I", h, e + 4, 0)                    # 頭出し表は作らない
    struct.pack_into(">I", h, e + 8, 0)
    h[e + 12] = VIDEO_W // 16                              # 幅・高さは 16 画素単位
    h[e + 13] = VIDEO_H // 16
    return bytes(h)


def video_procs(video_id: str, start_sec: int = 0):
    """yt-dlp → ffmpeg をつないで MPEG プログラムストリームを吐かせる。

    映像と音声が 1 本にまとまった 360p の形式 (itag 18) を優先する。
    映像だけの形式は分割配信で、標準出力へ流し込む取り方だと
    403 で弾かれるため使えない (音声側で m4a を優先しているのと同じ理由)。
    音声はここでは捨てる。元が 640x360 でも 480x272 に縮めるので画質に影響はない。
    """
    url = video_id
    if not url.startswith("http"):
        url = f"https://music.youtube.com/watch?v={video_id}"
    ydl = subprocess.Popen(
        [
            "yt-dlp", "--no-warnings", "-o", "-",
            "-f", "18/best[height<=480][ext=mp4][acodec!=none]/best[acodec!=none]",
            url,
        ],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    ff = subprocess.Popen(
        [
            "ffmpeg", "-hide_banner", "-loglevel", "error",
            "-i", "pipe:0",
            *(["-ss", str(start_sec)] if start_sec > 0 else []),
            "-an",
            "-c:v", "libx264",
            "-profile:v", "baseline",      # PSP はこれしかデコードできない
            "-level", "3.0",
            "-preset", "veryfast",         # Pi で動かすので速度優先
            "-pix_fmt", "yuv420p",
            "-vf", f"scale={VIDEO_W}:{VIDEO_H}:force_original_aspect_ratio=decrease,"
                   f"pad={VIDEO_W}:{VIDEO_H}:(ow-iw)/2:(oh-ih)/2",
            "-r", str(VIDEO_FPS),
            "-g", str(VIDEO_FPS),          # 1 秒ごとに I フレーム
            "-bf", "0",                    # Baseline は B フレームを持てない
            "-b:v", str(VIDEO_BITRATE),
            "-maxrate", str(VIDEO_BITRATE),
            "-bufsize", str(VIDEO_BITRATE),
            "-preload", "1000000",         # 最初の表示時刻を 90000 に合わせる
            "-f", "vob", "-packetsize", str(PACK_SIZE),
            "pipe:1",
        ],
        stdin=ydl.stdout, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    )
    ydl.stdout.close()
    return ydl, ff


def query_int(q: dict, key: str) -> int:
    """クエリの整数値を取り出す。無い・壊れているなら 0。"""
    try:
        return max(0, int(q.get(key, ["0"])[0] or 0))
    except (ValueError, TypeError):
        return 0


def read_exact(stream, size: int) -> bytes:
    """size バイト読み切る。終端なら読めた分だけ返す (パック境界を保つため)。"""
    chunks = []
    remaining = size
    while remaining > 0:
        part = stream.read(remaining)
        if not part:
            break
        chunks.append(part)
        remaining -= len(part)
    return b"".join(chunks)


def retag_video_stream(buf: bytearray) -> None:
    """ffmpeg が付けた 0xE0 番台のストリーム ID を 0xE0 に揃える (その場で書き換え)。

    ffmpeg は H.264 に 0xE2 を割り当てるが、PSP は 0xE0 しか映像として扱わない。
    """
    i = 0
    while True:
        i = buf.find(b"\x00\x00\x01", i)
        if i < 0 or i + 3 >= len(buf):
            break
        if 0xE0 <= buf[i + 3] <= 0xEF:
            buf[i + 3] = PSMF_VIDEO_STREAM_ID
        i += 3


def tsv_home() -> str:
    sections, fellback = with_fallback(
        lambda yt: yt.get_home(limit=HOME_SECTIONS), "ホーム取得")
    lines = []
    if fellback:
        lines.append("section\t※ ログイン中ですが一般向けの内容を表示しています")
    for section in sections:
        lines.append(f"section\t{clean(section.get('title'))}")
        for item in section.get("contents", [])[:HOME_ITEMS_PER_SECTION]:
            title = clean(item.get("title"))
            # アートワークは、下で書き出すのと同じ id で覚える
            # (食い違うとクライアントが取りに来た id で見つからない)
            remember_art(item.get("videoId") or item.get("playlistId"),
                         item.get("thumbnails"))
            # videoId を先に見る。
            # 「もう一度聴く」などに並ぶ曲は videoId と playlistId の両方を持ち、
            # その playlistId は「この曲のラジオ」(RDAMVM+videoId) でしかない。
            # playlistId を先に見ると 1 曲がプレイリスト扱いになり、
            # 押しても再生が始まらず一覧が開いてしまう。
            if item.get("videoId"):
                lines.append(f"video\t{clean(item['videoId'])}\t{title}\t{clean(artists_of(item))}")
            elif item.get("playlistId"):
                sub = clean(item.get("description")) or clean(artists_of(item))
                lines.append(f"playlist\t{clean(item['playlistId'])}\t{title}\t{sub}")
    return "\n".join(lines) + "\n"


def tsv_search(query: str) -> str:
    songs, _ = with_fallback(
        lambda yt: yt.search(query, filter="songs", limit=20),
        "曲の検索",
    )
    playlists, _ = with_fallback(
        lambda yt: yt.search(query, filter="community_playlists", limit=10),
        "プレイリストの検索",
    )

    lines = ["section\t曲"]
    for item in songs[:20]:
        video_id = item.get("videoId")
        if not video_id:
            continue
        remember_art(video_id, item.get("thumbnails"))
        lines.append(
            f"video\t{clean(video_id)}\t{clean(item.get('title'))}"
            f"\t{clean(artists_of(item))}"
        )

    lines.append("section\tプレイリスト")
    for item in playlists[:10]:
        # ytmusicapi の版により playlistId または browseId で返る。
        playlist_id = item.get("playlistId") or item.get("browseId")
        if not playlist_id:
            continue
        if playlist_id.startswith("VL"):
            playlist_id = playlist_id[2:]
        remember_art(playlist_id, item.get("thumbnails"))
        subtitle = (clean(item.get("description")) or
                    clean(artists_of(item)) or clean(item.get("author")))
        lines.append(
            f"playlist\t{clean(playlist_id)}\t{clean(item.get('title'))}"
            f"\t{subtitle}"
        )
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
        remember_art(vid, t.get("thumbnails"))
        dur = t.get("duration_seconds") or parse_len(t.get("length") or t.get("duration"))
        lines.append(
            f"track\t{clean(vid)}\t{clean(t.get('title'))}\t{clean(artists_of(t))}\t{dur}"
        )
    return "\n".join(lines) + "\n"


def tsv_radio(video_id: str) -> str:
    watch, _ = with_fallback(
        lambda yt: yt.get_watch_playlist(
            videoId=video_id, radio=True, limit=50
        ),
        "ラジオ取得",
    )
    lines = ["meta\tラジオ"]

    for t in watch.get("tracks", [])[:50]:
        vid = t.get("videoId")
        if not vid:
            continue
        remember_art(vid, t.get("thumbnails"))
        dur = t.get("duration_seconds") or parse_len(t.get("length") or t.get("duration"))
        lines.append(
            f"track\t{clean(vid)}\t{clean(t.get('title'))}\t{clean(artists_of(t))}\t{dur}"
        )
    return "\n".join(lines) + "\n"


def video_kind(video_type: str) -> str:
    """YouTube Music の種別を「曲」か「動画」に丸める。

    ATV (Audio Track Video) は音源に静止画を付けたもの = 曲扱い。
    それ以外 (OMV = 公式ミュージックビデオ, UGC = 利用者投稿) は動画扱い。
    """
    return "song" if (video_type or "").endswith("ATV") else "video"


def tsv_counterpart(video_id: str) -> str:
    """曲版とミュージックビデオ版の対応を返す。

    YouTube Music の画面にある「曲 / 動画」の切り替えと同じもので、
    同じ楽曲の別バージョンは別の動画として存在する。
    ytmusicapi はこれを counterpart として返してくれる。

    対応する版が無い曲も多いため、無いときは none を返してクライアント側で
    トグル自体を出さない (押せるのに何も起きない状態を避ける)。
    """
    watch, _ = with_fallback(
        lambda yt: yt.get_watch_playlist(videoId=video_id, limit=1),
        "対応バージョン取得",
    )
    tracks = watch.get("tracks") or []
    if not tracks:
        return "none\t1\n"

    current = tracks[0]
    other = current.get("counterpart")
    if not other or not other.get("videoId"):
        return "none\t1\n"

    dur = other.get("duration_seconds") or parse_len(
        other.get("length") or other.get("duration")
    )
    remember_art(other.get("videoId"), other.get("thumbnails"))
    return (
        f"cur\t{video_kind(current.get('videoType'))}\n"
        f"alt\t{clean(other.get('videoId'))}\t{video_kind(other.get('videoType'))}\t"
        f"{clean(other.get('title'))}\t{clean(artists_of(other))}\t{dur}\n"
    )


def tsv_lyrics(video_id: str) -> str:
    watch, _ = with_fallback(
        lambda yt: yt.get_watch_playlist(videoId=video_id, limit=1),
        "歌詞情報取得",
    )
    browse_id = watch.get("lyrics")
    if not browse_id:
        return "none\t1\n"

    result, _ = with_fallback(
        lambda yt: yt.get_lyrics(browse_id),
        "歌詞取得",
    )
    lyrics = result.get("lyrics") if result else None
    if not lyrics:
        return "none\t1\n"

    lines = [f"line\t{clean(line, limit=120)}"
             for line in lyrics.split("\n")[:200]]
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


def load_access_token():
    """共有トークンを返す。ファイルが無い場合だけ認証を無効にする。"""
    try:
        return ACCESS_TOKEN_FILE.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        return None
    except OSError as e:
        # 読めない設定を「認証なし」と扱うと外部公開時に危険なので fail closed。
        print(f"[server] auth/token.txt を読めません: {e}", file=sys.stderr)
        return ""


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        message = fmt % args
        message = re.sub(r"([?&]k=)[^&\s\"]*", r"\1***", message)
        sys.stderr.write(f"[server] {message}\n")
        sys.stderr.flush()

    def _stream_psmf(self, procs, tail, seconds, source=None):
        """PSMF ヘッダを先に送り、続けて MPEG プログラムストリームを流す。

        配信しながらなので本体の長さは分からない。再生時間とビットレートから
        見積もった値をヘッダに書く (PSP 側はデータが尽きた時点で終わるので、
        多少ずれても再生には影響しない)。
        """
        seconds = seconds if seconds > 0 else 300
        estimate = seconds * VIDEO_BITRATE // 8
        estimate = ((estimate + PACK_SIZE - 1) // PACK_SIZE) * PACK_SIZE

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.end_headers()
        sent = 0
        try:
            self.wfile.write(psmf_header(estimate, seconds))
            while True:
                # パック単位で読む。開始コードがパックをまたぐことは無いので、
                # 境界を揃えておけば書き換えが安全にできる
                buf = read_exact(tail.stdout, PACK_SIZE * 8)
                if not buf:
                    break
                block = bytearray(buf)
                retag_video_stream(block)
                self.wfile.write(block)
                sent += len(block)
        except (BrokenPipeError, ConnectionResetError):
            self.log_message("client disconnected after %d bytes", sent)
        finally:
            for p in procs:
                if p.poll() is None:
                    p.terminate()
            self.log_message("video done: %d bytes", sent)
            if sent == 0 and source is not None and source.stderr is not None:
                try:
                    err = source.stderr.read(4000).decode("utf-8", "replace").strip()
                except Exception:
                    err = ""
                self.log_message("映像を取得できませんでした: %s",
                                 err or "(理由の出力なし)")

    @staticmethod
    def _protected_path(path: str) -> bool:
        return path.startswith("/api/") or path in ("/art", "/stream", "/video")

    def _authorized(self, path: str, query: dict) -> bool:
        if not self._protected_path(path):
            return True
        expected = load_access_token()
        if expected is None:
            return True
        supplied = query.get("k", [""])[0]
        return bool(expected) and hmac.compare_digest(supplied, expected)

    def _text(self, body: str, code: int = 200, ctype: str = "text/tab-separated-values"):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", f"{ctype}; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _stream_process(self, procs, tail, source=None):
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
            # 1 バイトも流れなかったときは、取得側の理由をログに残す。
            # これが無いと「200 を返したのに無音」で原因が追えない。
            if sent == 0 and source is not None and source.stderr is not None:
                try:
                    err = source.stderr.read(4000).decode("utf-8", "replace").strip()
                except Exception:
                    err = ""
                self.log_message("音声を取得できませんでした: %s",
                                 err or "(理由の出力なし)")

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)

        if not self._authorized(url.path, q):
            self._text("error\t認証が必要です\n", code=401)
            return

        if url.path == "/api/lyrics" and "yt" in q:
            try:
                self._text(tsv_lyrics(q["yt"][0]))
            except Exception as e:
                self.log_message("lyrics error: %r", e)
                self._text("none\t1\n")
            return

        try:
            if url.path == "/api/status":
                self._text(tsv_status())
                return
            if url.path == "/api/home":
                self._text(tsv_home())
                return
            if url.path == "/api/search" and "q" in q:
                self._text(tsv_search(q["q"][0]))
                return
            if url.path == "/api/playlist" and "id" in q:
                self._text(tsv_playlist(q["id"][0]))
                return
            if url.path == "/api/radio" and "yt" in q:
                self._text(tsv_radio(q["yt"][0]))
                return
            if url.path == "/api/counterpart" and "yt" in q:
                self._text(tsv_counterpart(q["yt"][0]))
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

        if url.path == "/art" and "id" in q:
            size = int(q.get("s", [str(ART_SIZE)])[0])
            if size not in (32, 64, 128):
                size = ART_SIZE
            try:
                data = art_pixels(q["id"][0], size)
            except Exception as e:
                self.log_message("art 取得失敗 (%s): %r", q["id"][0], e)
                self._text("error\tアートワークを取得できません\n", code=404)
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return

        if url.path == "/stream" and "file" in q:
            ff = subprocess.Popen(ffmpeg_cmd(q["file"][0]), stdout=subprocess.PIPE)
            self._stream_process([ff], ff)
            return

        if url.path == "/video" and "yt" in q:
            try:
                seconds = int(q.get("sec", ["0"])[0] or 0)
            except ValueError:
                seconds = 0
            ydl, ff = video_procs(q["yt"][0], query_int(q, "t"))
            self._stream_psmf([ydl, ff], ff, seconds, source=ydl)
            return

        if url.path == "/stream" and "yt" in q:
            target = q["yt"][0]
            if not target.startswith("http"):
                target = f"https://music.youtube.com/watch?v={target}"
            ydl = subprocess.Popen(
                [
                    "yt-dlp", "--no-warnings", "-o", "-",
                    # m4a (AAC) を優先する。Opus/WebM はパイプ経由で
                    # ffmpeg が扱えない場合があり、無音のまま 0 バイトになる
                    "-f", "bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]/bestaudio",
                    target,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            ff = subprocess.Popen(
                ffmpeg_cmd("pipe:0", query_int(q, "t")),
                stdin=ydl.stdout, stdout=subprocess.PIPE
            )
            ydl.stdout.close()
            self._stream_process([ydl, ff], ff, source=ydl)
            return

        self._text("error\tnot found\n", code=404)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    for tool in ("ffmpeg", "yt-dlp"):
        if not shutil.which(tool):
            sys.exit(
                f"{tool} が見つかりません "
                f"(macOS: brew install {tool} / Debian: apt install {tool}。"
                f"yt-dlp は .venv/bin/pip install -U yt-dlp でも可)"
            )
    print(f"[server] auth: {'browser.json' if is_authed() else 'なし (一般向けホーム)'}")
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"[server] listening on 0.0.0.0:{port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
