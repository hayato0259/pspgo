# youtube-music — PSP Go 音楽ストリーミングプレイヤー

PSP Go (6.61 + Ark-4 CFW) で音楽をストリーミング再生する homebrew アプリと、
その配信サーバー。

## 仕組み

```
[音源] → [自宅サーバー: 取得 + MP3 CBR 128kbps 変換 + HTTP配信] --LAN--> [PSP Go]
```

PSP は現代の TLS を直接扱えず Opus もデコードできないため、
サーバー側がすべて吸収し、PSP は「HTTP GET + ハードウェア MP3 デコード」だけを行う。

- `psp-client/` — PSP 側 (C, pspsdk)。PoC 段階
  - `poc1-audio/` — sceAudio での PCM 出力検証
  - `poc2-stream/` — HTTP 受信 → sceMp3 → sceAudio のストリーミング再生検証
- `server/` — 配信プロキシ (Python + ffmpeg + yt-dlp)
- `docs/` — 検証レポート・設計ドキュメント

## ステータス

- [x] PPSSPP エミュレータ上で全経路の疎通確認（2026-07）
- [ ] 実機 (PSP Go + Ark-4) での動作確認
- [ ] プレイヤー UI・プレイリスト
- [ ] Navidrome (Subsonic API) 対応

## 注意

YouTube を音源にする経路 (`/stream?yt=`) は YouTube API 利用規約に抵触する
（詳細: [docs/verification-youtube-music.md](docs/verification-youtube-music.md)）。
実験用途に留め、常用音源には Navidrome 等の自前ライブラリ配信を推奨する。
