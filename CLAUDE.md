# pspgo — PSP Go 向け音楽ストリーミングアプリ

PSP Go (6.61 CFW) で動く自作音楽プレイヤーと、その配信サーバーを作るプロジェクト。
実現可否の検証結果と設計判断は [docs/verification-youtube-music.md](docs/verification-youtube-music.md) を必ず読むこと。

## 構成

| ディレクトリ | 内容 |
|---|---|
| `psp-client/poc1-audio/` | PoC1: sceAudio 音声出力の最小検証 |
| `psp-client/poc2-stream/` | PoC2: HTTP → sceMp3 → sceAudio ストリーミング再生 |
| `server/` | 配信プロキシ (Python, ffmpeg + yt-dlp) |
| `docs/` | 検証レポート・設計ドキュメント |

## ビルド環境

- ツールチェーン: `~/pspdev-install/pspdev` (pspdev v20260701 プリビルト, macOS arm64)
- ビルド前に必ず:
  ```
  export PSPDEV="$HOME/pspdev-install/pspdev"
  export PATH="$PSPDEV/bin:$PATH"
  ```
- 各 PoC ディレクトリで `make` → `EBOOT.PBP` が生成される
- エミュレータ: PPSSPP 1.20.4 (`/Applications/PPSSPPSDL.app`)。
  **EBOOT.PBP は絶対パスで渡す**（相対パスは umd0:/ 扱いで失敗する）

## ポート割り当て

| 用途 | ポート |
|---|---|
| 配信プロキシ (server/proxy.py) | 8080 |

## PSP 固有の地雷（検証で踏んだもの）

- **BSD ソケット関数 (socket/connect/recv) を使わない** — 新 NID で PPSSPP 未実装。
  `sceNetInet*` を直接呼ぶ
- **PPSSPP のソケットは EAGAIN を返す** — recv はリトライラッパー経由で呼ぶ (poc2 の recv_wait)
- sceMp3 のバッファは 64byte アライン、終端不明ストリームは `mp3StreamEnd = 0x7FFFFFFF`
- 配信フォーマットは MP3 CBR 128kbps / 44.1kHz / stereo に固定（Opus は PSP で不可）
- PPSSPP のネットワークは `~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini` の
  `[Network] EnableWLAN = True` が必要（設定済み）

## 方針

- 音源は差し替え可能にする。常用は Navidrome (Subsonic API) を第一候補、
  YouTube Music 経路 (`/stream?yt=`) は実験用（規約違反側であることをレポート参照）
- PSP への配信は LAN 内の平文 HTTP のみ。外部公開しない
