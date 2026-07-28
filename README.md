# pspgo

PSP Go (PSP-N1000) を現役で使うための自作アプリ集。
実機 (FW 6.61 + CFW) と PPSSPP の両方で動作する。

| アプリ | 内容 |
|---|---|
| [apps/youtube-music/](apps/youtube-music/) | 音楽ストリーミングプレイヤー。PSP クライアント (C) + セルフホスト型の配信サーバー (Python) |

## youtube-music の概要

```
[音源] → [自分で立てるサーバー: メタデータ取得 + MP3 CBR 128k 変換] --LAN(HTTP)--> [PSP]
```

PSP は現代の TLS を扱えず Opus もデコードできないため、サーバー側がすべてを吸収し、
PSP は「HTTP GET + ハードウェア MP3 デコード」だけを行う設計になっている。

- PC 版 YouTube Music に寄せた UI (カルーセル・アートワーク・日本語表示)
- 検索 / ラジオ / 歌詞 / 再生モード (シャッフル・リピート) / スリープタイマー
- **オフライン再生**: 曲を本体ストレージへダウンロードしておけば、
  サーバーもネットワークも無しで再生できる
- 接続先は EBOOT と同じ場所の `server.txt` で決まるため、
  ビルド済みバイナリをそのまま配布できる

導入手順・操作方法は [apps/youtube-music/README.md](apps/youtube-music/README.md) を参照。
ビルド済みの `EBOOT.PBP` は [Releases](../../releases) から入手できる。

## サーバーは各自でセルフホストする

配信サーバーは利用者が自分の環境で動かす前提で、公開のホスティングサービスは提供しない。

- PSP は現代の TLS を話せないため、HTTPS 必須のマネージドホスティングには接続できない
- YouTube を音源にする経路を不特定多数へ提供すると、規約・アカウント停止のリスクが大きい

常時稼働マシン (Raspberry Pi など) への設置手順は
[apps/youtube-music/docs/raspberry-pi-server.md](apps/youtube-music/docs/raspberry-pi-server.md)。

## 注意

- **YouTube を音源にする経路は YouTube の利用規約に抵触する。**
  実験用途に留め、常用には Navidrome など自前ライブラリの配信を推奨する
  (詳細: [検証レポート](apps/youtube-music/docs/verification-youtube-music.md))
- PSP の内蔵フォント (`*.pgf`) は著作物のためリポジトリに含めない。
  PPSSPP で動かす場合は各自の環境からコピーする
- 実機での改造行為・データの取り扱いは自己責任で

## 開発

- ツールチェーン: [pspdev](https://github.com/pspdev/pspdev)
- 実機セットアップと CFW の選定: [docs/psp-go-setup.md](docs/psp-go-setup.md)
- 設計上の判断や踏んだ問題は各 `docs/` に記録している
