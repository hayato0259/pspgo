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
- PPSSPP のネットワークは `~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini` の
  `[Network] EnableWLAN = True` が必要（設定済み）

## ポート割り当て

| 用途 | ポート |
|---|---|
| youtube-music 配信プロキシ (apps/youtube-music/server/proxy.py) | 8080 |

## PSP 固有の地雷（検証で踏んだもの）

- **BSD ソケット関数 (socket/connect/recv) を使わない** — 新 NID で PPSSPP 未実装。
  `sceNetInet*` を直接呼ぶ
- **PPSSPP のソケットは EAGAIN を返す** — recv はリトライラッパー経由で呼ぶ
  (poc2-stream の recv_wait)
- sceMp3 のバッファは 64byte アライン、終端不明ストリームは `mp3StreamEnd = 0x7FFFFFFF`
- 配信フォーマットは MP3 CBR 128kbps / 44.1kHz / stereo に固定（Opus は PSP で不可）
- Media Engine は排他リソース。ME を使う他プラグインとは共存できない

## 公開前のチェック（GitHub 公開予定のため）

- 認証情報・LAN の IP アドレス・SSID をコードやドキュメントにハードコードしたまま
  コミットしない（SERVER_HOST は make 引数で渡す設計を維持する）
- YouTube 経路の規約リスクは README に明記する（検証レポート参照）

## 方針

- 音源は差し替え可能にする。常用は Navidrome (Subsonic API) を第一候補、
  YouTube Music 経路 (`/stream?yt=`) は実験用（規約違反側であることをレポート参照）
- PSP への配信は LAN 内の平文 HTTP のみ。外部公開しない
