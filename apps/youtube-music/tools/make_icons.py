#!/usr/bin/env python3
"""Material Symbols のアイコンを PSP クライアントに焼き込む C ソースを作る。

本家 YouTube Music は Google の Material Symbols を使っている。
同じ絵柄を使うため、必要なアイコンだけをここでビットマップに落として
`src/icons.c` に埋め込む。

PSP 側にフォント描画機構を持ち込まないのは、
- 実行時にアイコン用フォントを読ませると起動が遅くなる
- オフライン再生時にもアイコンは要る (サーバーから取る形にできない)
という理由。必要な絵柄は数十個で固定なので、焼き込むのが素直。

不透明度だけを 1 バイトで持つ (色は描画時に指定する)。
32x32 で 1 個 1KB。

使い方:
    python3 make_icons.py                 # フォントを取得して生成
    python3 make_icons.py --font <path>   # 手元の TTF を使う

Material Symbols は Apache License 2.0。
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

FONT_URL = ("https://github.com/google/material-design-icons/raw/master/"
            "variablefont/MaterialSymbolsOutlined%5BFILL%2CGRAD%2Copsz%2Cwght%5D.ttf")
CODEPOINTS_URL = FONT_URL.replace(".ttf", ".codepoints")

SIDE = 32           # PSP のテクスチャは辺が 2 の冪
RENDER_PX = 30      # 32 の枠に少し余白を残す

# 使うアイコン。名前は Material Symbols のもの。
# 末尾に "!" を付けると塗りつぶし版 (本家も選択中は塗りつぶしになる)。
ICONS = [
    "play_arrow", "pause", "skip_next", "skip_previous",
    "thumb_up", "thumb_down", "thumb_up!", "thumb_down!",
    "lyrics", "more_vert", "search", "download",
    "radio", "bedtime", "repeat", "repeat_one", "shuffle",
    "music_video", "library_music", "wifi_off",
    "bookmark", "bookmark!", "smart_display",
]


def fetch(url: str, dest: Path) -> None:
    subprocess.run(["curl", "-sL", "-o", str(dest), url], check=True)


def load_codepoints(path: Path) -> dict:
    table = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) == 2:
            table[parts[0]] = int(parts[1], 16)
    return table


def render(font_path: Path, codepoints: dict) -> list:
    from PIL import Image, ImageDraw, ImageFont

    out = []
    for entry in ICONS:
        filled = entry.endswith("!")
        name = entry[:-1] if filled else entry
        if name not in codepoints:
            sys.exit("アイコンが見つかりません: %s" % name)
        ch = chr(codepoints[name])

        # 可変フォントなので軸で塗りつぶしを切り替える (FILL, GRAD, opsz, wght)
        font = ImageFont.truetype(str(font_path), RENDER_PX)
        try:
            font.set_variation_by_axes([1.0 if filled else 0.0, 0.0, 24.0, 400.0])
        except Exception:
            pass

        img = Image.new("L", (SIDE, SIDE), 0)
        draw = ImageDraw.Draw(img)
        # 枠の中央に置く。フォントごとの余白差を吸収するため実測で寄せる
        box = draw.textbbox((0, 0), ch, font=font)
        x = (SIDE - (box[2] - box[0])) // 2 - box[0]
        y = (SIDE - (box[3] - box[1])) // 2 - box[1]
        draw.text((x, y), ch, font=font, fill=255)
        out.append((name + ("_fill" if filled else ""), img.tobytes()))
    return out


def emit(icons: list, src_dir: Path) -> None:
    names = [n for n, _ in icons]

    header = ["#ifndef ICONS_H", "#define ICONS_H", "",
              "/*", " * Material Symbols から起こしたアイコン (make_icons.py が生成)。",
              " * 直接編集しない。絵柄を足すときは tools/make_icons.py の ICONS に加える。",
              " *", " * 不透明度だけを 1 バイトで持つ。色は描画時に指定する。",
              " */", "",
              "#define ICON_SIDE %d" % SIDE, "",
              "typedef enum {"]
    for n in names:
        header.append("    ICON_%s," % n.upper())
    header += ["    ICON_COUNT", "} IconId;", "",
               "/* 辺 ICON_SIDE の不透明度マップ。ICON_COUNT 個ぶん並ぶ */",
               "extern const unsigned char icon_alpha[ICON_COUNT][ICON_SIDE * ICON_SIDE];",
               "", "#endif", ""]
    (src_dir / "icons.h").write_text("\n".join(header), encoding="utf-8")

    body = ["/*", " * Material Symbols から起こしたアイコン (make_icons.py が生成)。",
            " * 直接編集しない。",
            " *", " * 元データ: Google Material Symbols (Apache License 2.0)",
            " */", '#include "icons.h"', "",
            "const unsigned char icon_alpha[ICON_COUNT][ICON_SIDE * ICON_SIDE] = {"]
    for name, data in icons:
        body.append("    { /* %s */" % name)
        for row in range(SIDE):
            chunk = data[row * SIDE:(row + 1) * SIDE]
            body.append("        " + ",".join(str(b) for b in chunk) + ",")
        body.append("    },")
    body += ["};", ""]
    (src_dir / "icons.c").write_text("\n".join(body), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", help="Material Symbols の TTF (省略時は取得する)")
    ap.add_argument("--codepoints", help="対応表 (省略時は取得する)")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent.parent /
                                         "psp-client" / "app" / "src"))
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        font = Path(args.font) if args.font else Path(tmp) / "ms.ttf"
        cps = Path(args.codepoints) if args.codepoints else Path(tmp) / "ms.codepoints"
        if not args.font:
            fetch(FONT_URL, font)
        if not args.codepoints:
            fetch(CODEPOINTS_URL, cps)
        icons = render(font, load_codepoints(cps))

    out_dir = Path(args.out)
    emit(icons, out_dir)
    print("%d 個のアイコンを %s に書き出しました" % (len(icons), out_dir))


if __name__ == "__main__":
    main()
