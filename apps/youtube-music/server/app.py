#!/usr/bin/env python3
"""PSP 向け YouTube Music アプリサーバー.

PSP クライアントの制約 (HTTP/1.0・MP3 CBR・JSON パーサなし) に合わせて、
すべてのメタデータをタブ区切りテキスト (TSV) で返す。

エンドポイント:
  GET /api/status            認証状態:  auth\t0|1 / name\t<表示名>
  GET /api/home              ホーム:    section\t<題> / playlist\t<id>\t<題>\t<副題> / video\t<id>\t<題>\t<アーティスト>
  GET /api/playlist?id=<id>  内容:      meta\t<題> / track\t<videoId>\t<題>\t<アーティスト>\t<秒>
  GET /stream?yt=<videoId>   音声を MP3 CBR 128kbps で配信
  GET /stream?file=<path>    ローカルファイルを変換して配信 (検証用)

認証 (任意): auth/browser.json を置くとログイン済みとして動作し、
マイミックス等のパーソナライズされたホームが返る。
未配置なら一般向けホームで動く。設定方法は README を参照。

依存: ffmpeg, yt-dlp, ytmusicapi (.venv)
使い方: .venv/bin/python app.py [port]   (デフォルト 8080)
"""

import json
import shutil
import subprocess
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from ytmusicapi import YTMusic

BITRATE = "128k"
CHUNK = 16 * 1024
AUTH_FILE = Path(__file__).parent / "auth" / "browser.json"

_yt = None


def get_yt() -> YTMusic:
    global _yt
    if _yt is None:
        if AUTH_FILE.exists():
            _yt = YTMusic(str(AUTH_FILE))
        else:
            _yt = YTMusic()
    return _yt


def is_authed() -> bool:
    return AUTH_FILE.exists()


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
    lines = []
    for section in get_yt().get_home(limit=6):
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
    yt = get_yt()
    lines = []
    try:
        pl = yt.get_playlist(pid, limit=100)
        lines.append(f"meta\t{clean(pl.get('title'))}")
        tracks = pl.get("tracks", [])
    except Exception:
        # ラジオ/ミックス系プレイリストは get_watch_playlist で取れる
        watch = yt.get_watch_playlist(playlistId=pid, limit=100)
        lines.append(f"meta\t{clean(watch.get('title')) or 'Mix'}")
        tracks = watch.get("tracks", [])

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
    return f"auth\t{1 if is_authed() else 0}\nname\t{name}\n"


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
