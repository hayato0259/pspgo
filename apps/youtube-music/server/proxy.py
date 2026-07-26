#!/usr/bin/env python3
"""PSP 向け音声ストリーミングプロキシ (PoC).

PSP クライアントは HTTP/1.0 + MP3 CBR しか扱えない前提で、
音源を MP3 CBR 128kbps に変換してストリーム配信する。

エンドポイント:
  GET /stream?file=<path>   ローカル音声ファイルを MP3 に変換して配信 (結合検証用)
  GET /stream?yt=<動画ID or URL>   yt-dlp で音声を取得し MP3 に変換して配信
  GET /search?q=<query>     yt-dlp の ytsearch で候補を JSON で返す

注意: yt= 経路は YouTube の利用規約上グレー (公式プレイヤー外での
ストリーム取得は ToS 違反となりうる)。技術検証目的に留めること。

依存: ffmpeg (必須), yt-dlp (yt=/search 使用時のみ)
使い方: python3 proxy.py [port]   (デフォルト 8080)
"""

import json
import shutil
import subprocess
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BITRATE = "128k"
CHUNK = 16 * 1024


def ffmpeg_cmd(src: str) -> list[str]:
    """src (ファイルパスまたは 'pipe:0') を MP3 CBR に変換するコマンド。"""
    return [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-i", src,
        "-vn",
        "-c:a", "libmp3lame", "-b:a", BITRATE, "-ar", "44100", "-ac", "2",
        "-f", "mp3", "pipe:1",
    ]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        sys.stderr.write("[proxy] %s\n" % (fmt % args))

    def _stream_process(self, procs: list[subprocess.Popen], tail: subprocess.Popen):
        """tail の stdout をクライアントへ流す。切断時はプロセスを畳む。"""
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

    def _error(self, code: int, msg: str):
        body = msg.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(url.query)

        if url.path == "/stream" and "file" in q:
            path = q["file"][0]
            ff = subprocess.Popen(ffmpeg_cmd(path), stdout=subprocess.PIPE)
            self._stream_process([ff], ff)
            return

        if url.path == "/stream" and "yt" in q:
            if not shutil.which("yt-dlp"):
                self._error(500, "yt-dlp not installed")
                return
            target = q["yt"][0]
            if not target.startswith("http"):
                target = f"https://music.youtube.com/watch?v={target}"
            # yt-dlp が音声を stdout に吐き、ffmpeg が MP3 CBR へ変換
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

        if url.path == "/search" and "q" in q:
            if not shutil.which("yt-dlp"):
                self._error(500, "yt-dlp not installed")
                return
            query = q["q"][0]
            out = subprocess.run(
                ["yt-dlp", "-q", "--flat-playlist", "-J", f"ytsearch5:{query}"],
                capture_output=True, text=True,
            )
            if out.returncode != 0:
                self._error(502, out.stderr[-500:])
                return
            data = json.loads(out.stdout)
            results = [
                {"id": e["id"], "title": e.get("title"),
                 "duration": e.get("duration"), "uploader": e.get("uploader")}
                for e in data.get("entries", [])
            ]
            body = json.dumps(results, ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        self._error(404, "usage: /stream?file=... | /stream?yt=... | /search?q=...")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg is required")
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"[proxy] listening on 0.0.0.0:{port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
