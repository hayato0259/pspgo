# pspgo — PSP Go (CFW) 向けアプリのモノレポ

PSP Go (6.61 + Ark-4) で動く自作アプリ群。将来 GitHub で公開予定。
アプリは `apps/<名前>/` 配下で管理する。

## アプリ一覧

| パス | 内容 |
|---|---|
| `apps/youtube-music/` | 音楽ストリーミングプレイヤー（PSPクライアント + 配信プロキシ） |

各アプリの詳細・設計判断はそのアプリの `docs/` を読むこと。
youtube-music の実現可否検証は [apps/youtube-music/docs/verification-youtube-music.md](apps/youtube-music/docs/verification-youtube-music.md)。

実機（PSP Go 本体）のセットアップ・CFW 選定・開発ループはアプリ横断の知識として
[docs/psp-go-setup.md](docs/psp-go-setup.md) に置く。
**PSP Go の内蔵ストレージは `ef0:`**（ms0: ではない）。apitype の罠もこのドキュメント参照。

## ビルド環境（共通）

- ツールチェーン: `~/pspdev-install/pspdev` (pspdev v20260701 プリビルト, macOS arm64)
- ビルド前に必ず:
  ```
  export PSPDEV="$HOME/pspdev-install/pspdev"
  export PATH="$PSPDEV/bin:$PATH"
  ```
- 各 PoC / アプリのディレクトリで `make` → `EBOOT.PBP` が生成される
- エミュレータ: PPSSPP 1.20.4 (`/Applications/PPSSPPSDL.app`)。
  **EBOOT.PBP は絶対パスで渡す**（相対パスは umd0:/ 扱いで失敗する）
- **youtube-music を PPSSPP で動かすときは `apps/youtube-music/run.sh` を使う。**
  ビルド・配置・PPSSPP 起動を 1 コマンドでやる
  （`--demo` で AUTODEMO ビルド / `--clean`）
  - **接続先は配置先の `server.txt` が決める。** ラズパイを向いていれば
    Mac 側のサーバーは起動しない。サーバー側を直すときは `--local`、
    戻すときは `--remote`（元の設定はトークンごと退避される）
- PPSSPP のネットワークは `~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini` の
  `[Network] EnableWLAN = True` が必要（設定済み）

## ポート割り当て

| 用途 | ポート |
|---|---|
| youtube-music アプリサーバー (apps/youtube-music/server/app.py) | 8080 |

## PSP 固有の地雷（検証で踏んだもの）

- **日本語表示は jpn0.pgf を「主フォント」にする。**
  ltn8.pgf を主にして `intraFontSetAltFont` で日本語へ逃がす構成だと、
  連続する日本語文字が 1 文字目以降描画されない（実測で確認）。
  jpn0.pgf は ASCII も含むので、これ 1 本で日英とも描ける
  （検証ツール: `apps/youtube-music/psp-client/fontcheck/`）
- **intraFont は深度/アルファテストを有効にしたまま戻さない。**
  文字の後に平面塗りやテクスチャを描くと、テストに弾かれて何も出ない。
  描画直前に `sceGuDisable(GU_DEPTH_TEST/GU_ALPHA_TEST/GU_STENCIL_TEST/GU_CULL_FACE)`
  を呼ぶこと（`gu_state_2d()`）。テクスチャは `GU_TCC_RGBA` +
  `sceGuColor(0xFFFFFFFF)`、CPU で書いた画素は `sceKernelDcacheWritebackRange` が必要
- **画面の目視確認は PPSSPP のウィンドウを画面キャプチャすれば可能。**
  フレームバッファの読み出しはできないが、デスクトップのキャプチャで確認できる
- **PPSSPP ではフレームバッファを CPU から読めない。**
  `sceDisplayGetFrameBuf` の返り値を読んでも古いフレームのままで、
  メインメモリへのオフスクリーン描画も反映されない（ハード/ソフト両レンダラで確認）。
  画面の目視確認は実機でしかできないため、UI の検証は
  「幅の実測」など数値で判定できる方法を使う

- **sceIoRename / sceIoRemove / sceIoOpen は相対パスを解決しない**（PPSSPP で実測。
  ファイルは作られるのに改名・削除だけ失敗する）。EBOOT 相対のファイル操作は
  stdio (`fopen`/`rename`/`remove`) を使う。`sceIoMkdir` は相対でも動く
- **BSD ソケット関数 (socket/connect/recv) を使わない** — 新 NID で PPSSPP 未実装。
  `sceNetInet*` を直接呼ぶ
- **PPSSPP のソケットは EAGAIN を返す** — recv はリトライラッパー経由で呼ぶ
  (poc2-stream の recv_wait)
- sceMp3 のバッファは 64byte アライン、終端不明ストリームは `mp3StreamEnd = 0x7FFFFFFF`
- 配信フォーマットは MP3 CBR 128kbps / 44.1kHz / stereo に固定（Opus は PSP で不可）
- Media Engine は排他リソース。ME を使う他プラグインとは共存できない
- **`sceMpegAvcDecode` の描画先は「ポインタのポインタ」**。フレームバッファのアドレスを
  入れた変数を作り、その変数のアドレスを渡す。直接渡すと**戻り値 0 のまま画面が真っ黒**
  になり、ログを見ないと気付けない（`Ignoring invalid video decode address`）。
  映像の詳細は [apps/youtube-music/docs/verification-video.md](apps/youtube-music/docs/verification-video.md)
- **`sceVideocodec` は PPSSPP 未実装**。生の H.264 を渡す低レベル API だが
  エミュレータで検証できないため、映像は `sceMpeg` + PSMF 経路を使う

## 配布の方針

- **サーバーは各利用者がセルフホストする。** クラウド (Firebase 等) へのデプロイは不可:
  PSP は現代の TLS を話せず、HTTPS 必須のマネージドホスティングには接続できない。
  加えて不特定多数向けの YouTube 配信プロキシは規約・アカウント停止リスクが大きい
- PSP クライアントの接続先は **EBOOT と同じフォルダの `server.txt`** で実行時に決まる
  (無ければ 127.0.0.1 = PPSSPP 開発用)。ビルド済み EBOOT の配布はこれで成立する
- **YouTube 経路のサーバーは住宅回線に置く。** データセンターの IP は
  「ボットではないことを確認」で弾かれ yt-dlp が失敗する。Raspberry Pi への移設手順は
  [apps/youtube-music/docs/raspberry-pi-server.md](apps/youtube-music/docs/raspberry-pi-server.md)
- タグ `v*` を push すると GitHub Actions (pspdev/pspdev コンテナ) が
  EBOOT.PBP をビルドして Release に添付する

## 公開前のチェック（GitHub 公開予定のため）

- 認証情報・LAN の IP アドレス・SSID をコードやドキュメントにハードコードしたまま
  コミットしない（SERVER_HOST は make 引数で渡す設計を維持する）
- YouTube 経路の規約リスクは README に明記する（検証レポート参照）

## UI の方針（最優先）

**目指すのは「Google の公式アプリと間違われる品質」。**
PC 版 YouTube Music に無い要素を画面に足さない。

- **純正に無い情報表示を出さない。** 通信量・デコード速度・内部の状態など、
  開発者には有益でも本家が見せないものは画面に出さない
  （実際に「通信量 約200MB/時」を出して差し戻した）
- 操作の説明は画面下の共通の行にまとめる。部品の脇に個別のヒントを置かない
- 迷ったら PC 版 YouTube Music の実際の画面を基準にする。
  そこに無いなら出さない
- **文字には影を付ける**（`GFX_TEXT_SHADOW`）。本体のシステム画面と同じ見え方になり、
  明るい背景の上でも文字が沈まない。intraFont は影の色を引数で受け取るので
  二度描きは不要
- **アイコンは Google Material Symbols を使う**（本家と同じ絵柄）。
  自前で図形を描き起こさない。`apps/youtube-music/tools/make_icons.py` で
  ビットマップに落として焼き込む形にしてある

## 方針

- 音源は差し替え可能にする。常用は Navidrome (Subsonic API) を第一候補、
  YouTube Music 経路 (`/stream?yt=`) は実験用（規約違反側であることをレポート参照）
- 既定は LAN 内のみ。外部公開する場合は共有トークン認証を必須にする
