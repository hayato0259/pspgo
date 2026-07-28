#!/usr/bin/env python3
"""動画ファイルを PSP の PSMF (.pmf) に変換する。

PSP の映像デコーダ (sceMpeg) は生の H.264 ストリームを受け付けず、
Sony 独自コンテナ PSMF を要求する。PSMF の中身は

    先頭 2048 バイトの PSMF ヘッダ + MPEG プログラムストリーム

という構成で、後半は ffmpeg がそのまま作れる。
このスクリプトがやるのは次の 3 つ:

  1. ffmpeg で H.264 Baseline / 2048 バイト単位の MPEG プログラムストリームを作る
  2. 映像のストリーム ID を PSP が期待する 0xE0 に直す
     (ffmpeg は 0xE2 を割り当てるため)
  3. PSMF ヘッダを組み立てて先頭に付ける

音声は PSMF に入れない。PSMF の音声は Atrac3+ 固定で、ffmpeg に
Atrac3+ エンコーダが無いため。音声は別ファイルの MP3 として出力し、
PSP 側では既存の sceMp3 経路で再生する。

使い方:
    python3 make_psmf.py input.mp4 out.pmf --audio out.mp3
"""

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# PSMF ヘッダの構造。値は PPSSPP の Core/HLE/sceMpeg.h と scePsmf.cpp に合わせた。
PSMF_HEADER_SIZE = 2048
PSMF_MAGIC = b"PSMF"
PSMF_VERSION = b"0015"
OFF_STREAM_OFFSET = 0x08      # 大端 32bit。ヘッダの長さ。2048 の倍数でないと弾かれる
OFF_STREAM_SIZE = 0x0C        # 大端 32bit。後続の MPEG プログラムストリームのバイト数
OFF_FIRST_TIMESTAMP = 0x54    # 大端 6 バイト。90kHz 単位の最初の表示時刻
OFF_LAST_TIMESTAMP = 0x5A     # 大端 6 バイト。90kHz 単位の最後の表示時刻
OFF_STREAM_COUNT = 0x80       # 大端 16bit
OFF_STREAM_TABLE = 0x82       # 1 エントリ 16 バイト

VIDEO_STREAM_ID = 0xE0        # PSP が映像として扱うストリーム ID
PACK_SIZE = 2048              # MPEG プログラムストリームの 1 パックの長さ
PTS_HZ = 90000                # MPEG の表示時刻の単位

# 画面いっぱい。PSP の液晶は 480x272
DEFAULT_WIDTH = 480
DEFAULT_HEIGHT = 272


def run_ffmpeg(args, what):
    """ffmpeg を呼ぶ。失敗したら ffmpeg の出力をそのまま見せて終わる。"""
    proc = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args],
        capture_output=True,
        text=True,
    )
    if proc.returncode != 0:
        sys.exit("%s に失敗しました:\n%s" % (what, proc.stderr.strip()))


def encode_program_stream(src, dst, width, height, fps, bitrate):
    """H.264 Baseline に変換して MPEG プログラムストリームで書き出す。

    Baseline profile なのは PSP の Media Engine が Baseline しかデコードできないため。
    -packetsize 2048 で 1 パック 2048 バイトに揃える (PSMF の前提)。
    """
    run_ffmpeg(
        [
            "-i", str(src),
            "-an",
            "-c:v", "libx264",
            "-profile:v", "baseline",
            "-level", "3.0",
            "-pix_fmt", "yuv420p",
            "-vf", "scale=%d:%d:force_original_aspect_ratio=decrease,"
                   "pad=%d:%d:(ow-iw)/2:(oh-ih)/2" % (width, height, width, height),
            "-r", str(fps),
            "-g", str(fps),          # 1 秒ごとに I フレーム。頭出しと復帰のため
            "-bf", "0",              # Baseline は B フレームを持てない
            "-b:v", bitrate,
            "-maxrate", bitrate,
            "-bufsize", bitrate,
            # PSMF は最初の表示時刻が 90000 (90kHz で 1 秒) である前提で作られている。
            # ffmpeg の既定は 0.5 秒 (= 45000) なので 1 秒に伸ばして合わせる
            # (合わせないと PPSSPP が "Unexpected mpeg first timestamp" を出す)。
            "-preload", "1000000",
            "-f", "vob",
            "-packetsize", str(PACK_SIZE),
            str(dst),
        ],
        "映像の変換",
    )


def encode_audio(src, dst, bitrate="128k"):
    """既存の配信と同じ形式 (MP3 CBR 128kbps / 44.1kHz / stereo) で音声を出す。"""
    run_ffmpeg(
        [
            "-i", str(src),
            "-vn",
            "-c:a", "libmp3lame",
            "-b:a", bitrate,
            "-ar", "44100",
            "-ac", "2",
            str(dst),
        ],
        "音声の変換",
    )


def retag_video_stream_id(data):
    """ffmpeg が付けた映像ストリーム ID を PSP が期待する 0xE0 に直す。

    ffmpeg の vob マルチプレクサは H.264 に 0xE2 を割り当てるが、
    PSP は 0xE0 番台の先頭しか映像として扱わない。
    パケット開始コード (00 00 01 xx) の xx が 0xE0〜0xEF なら 0xE0 に寄せる。
    """
    out = bytearray(data)
    found = set()
    i = 0
    while True:
        i = out.find(b"\x00\x00\x01", i)
        if i < 0 or i + 3 >= len(out):
            break
        sid = out[i + 3]
        if 0xE0 <= sid <= 0xEF:
            found.add(sid)
            out[i + 3] = VIDEO_STREAM_ID
        i += 3
    return bytes(out), found


def collect_video_pts(data):
    """映像パケットの表示時刻を集めて (最初, 最後) を返す。

    PSMF ヘッダに書く再生開始・終了時刻は、実データと食い違うと
    PSP 側の再生位置がずれるため、推測せず実際のストリームから読む。
    """
    stamps = []
    i = 0
    while True:
        i = data.find(b"\x00\x00\x01" + bytes([VIDEO_STREAM_ID]), i)
        if i < 0:
            break
        # PES: 開始コード 4 + パケット長 2 + フラグ 2 + ヘッダ長 1 の直後に PTS が来る
        head = i + 6
        if head + 3 > len(data):
            break
        flags = data[head + 1]
        if flags & 0x80:  # PTS あり
            p = head + 3
            if p + 5 <= len(data):
                b = data[p:p + 5]
                pts = (((b[0] >> 1) & 0x07) << 30
                       | b[1] << 22 | ((b[2] >> 1) & 0x7F) << 15
                       | b[3] << 7 | ((b[4] >> 1) & 0x7F))
                stamps.append(pts)
        i += 4
    if not stamps:
        return 0, 0
    return min(stamps), max(stamps)


def put_u48be(buf, offset, value):
    """6 バイトの大端整数を書く。PSMF の時刻はこの幅で持つ。"""
    buf[offset:offset + 6] = struct.pack(">Q", value)[2:]


def build_header(stream_size, first_pts, last_pts, width, height):
    """PSMF ヘッダ (2048 バイト) を組み立てる。"""
    h = bytearray(PSMF_HEADER_SIZE)
    h[0:4] = PSMF_MAGIC
    h[4:8] = PSMF_VERSION
    struct.pack_into(">I", h, OFF_STREAM_OFFSET, PSMF_HEADER_SIZE)
    struct.pack_into(">I", h, OFF_STREAM_SIZE, stream_size)
    put_u48be(h, OFF_FIRST_TIMESTAMP, first_pts)
    put_u48be(h, OFF_LAST_TIMESTAMP, last_pts)

    # ストリーム表: 映像 1 本だけ
    struct.pack_into(">H", h, OFF_STREAM_COUNT, 1)
    e = OFF_STREAM_TABLE
    h[e] = VIDEO_STREAM_ID
    h[e + 1] = 0x00
    struct.pack_into(">I", h, e + 4, 0)   # EPMap の位置。頭出し表は作らないので 0
    struct.pack_into(">I", h, e + 8, 0)   # EPMap の要素数
    h[e + 12] = width // 16               # 幅・高さは 16 画素単位で持つ
    h[e + 13] = height // 16
    return bytes(h)


def main():
    ap = argparse.ArgumentParser(description="動画を PSP の PSMF (.pmf) に変換する")
    ap.add_argument("input", help="入力の動画ファイル")
    ap.add_argument("output", help="出力する .pmf")
    ap.add_argument("--audio", help="音声を MP3 として書き出す先")
    ap.add_argument("--width", type=int, default=DEFAULT_WIDTH)
    ap.add_argument("--height", type=int, default=DEFAULT_HEIGHT)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--bitrate", default="500k")
    args = ap.parse_args()

    if args.width % 16 or args.height % 16:
        sys.exit("幅と高さは 16 の倍数にしてください (PSMF ヘッダが 16 画素単位のため)")

    src = Path(args.input)
    if not src.exists():
        sys.exit("入力ファイルがありません: %s" % src)

    with tempfile.TemporaryDirectory() as tmp:
        ps_path = Path(tmp) / "video.mpg"
        encode_program_stream(src, ps_path, args.width, args.height,
                              args.fps, args.bitrate)
        raw = ps_path.read_bytes()

    if len(raw) % PACK_SIZE:
        sys.exit("MPEG プログラムストリームが %d バイト単位になっていません "
                 "(ffmpeg の -packetsize が効いていない可能性があります)" % PACK_SIZE)

    data, found = retag_video_stream_id(raw)
    first_pts, last_pts = collect_video_pts(data)
    if last_pts <= first_pts:
        sys.exit("映像の表示時刻を読み取れませんでした")

    header = build_header(len(data), first_pts, last_pts, args.width, args.height)
    Path(args.output).write_bytes(header + data)

    seconds = (last_pts - first_pts) / PTS_HZ
    print("PSMF を書き出しました: %s" % args.output)
    print("  解像度 %dx%d / %d fps / 長さ %.1f 秒" %
          (args.width, args.height, args.fps, seconds))
    print("  ストリーム %d バイト (%d パック)" % (len(data), len(data) // PACK_SIZE))
    print("  ストリーム ID %s を 0x%02X に変更しました" %
          (", ".join("0x%02X" % s for s in sorted(found)), VIDEO_STREAM_ID))

    if args.audio:
        encode_audio(src, args.audio)
        print("音声を書き出しました: %s" % args.audio)


if __name__ == "__main__":
    main()
