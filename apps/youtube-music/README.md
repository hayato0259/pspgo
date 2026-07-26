# youtube-music — PSP Go 音楽ストリーミングプレイヤー

PSP Go (6.61 + CFW) で音楽をストリーミング再生する homebrew アプリと、その配信サーバー。

```
[YouTube Music] → [自宅サーバー: メタデータ取得 + MP3 CBR 128k 変換] --LAN(HTTP)--> [PSP Go アプリ]
```

PSP は現代の TLS を扱えず Opus もデコードできないため、サーバー側がすべて吸収し、
PSP は「HTTP GET + ハードウェア MP3 デコード」だけを行う。

## 画面と操作

| 画面 | 内容 |
|---|---|
| 接続 | Wi-Fi 接続 → サーバーの認証状態を確認 → ホーム取得 |
| ホーム | セクション見出し付きのプレイリスト一覧（マイミックス等） |
| プレイリスト | 曲一覧（曲名・アーティスト・長さ） |
| 再生 | 曲名・状態・経過時間・進捗バー・次曲表示 |

| ボタン | 動作 |
|---|---|
| 上下 | 選択移動（長押しリピート） |
| ○ | 決定（プレイリストを開く / 再生開始） |
| × | 前の画面へ戻る |
| △ | 一時停止 / 再開 |
| L / R | 前の曲 / 次の曲 |
| START | 終了 |

曲の終端に達すると自動で次の曲へ進む。

## 構成

| パス | 内容 |
|---|---|
| `psp-client/app/` | アプリ本体（C）。`src/net.c` 通信 / `src/api.c` TSV解析 / `src/player.c` 再生スレッド / `src/main.c` UI |
| `psp-client/poc1-audio/`, `poc2-stream/` | 実現可否検証に使った PoC（残置） |
| `server/app.py` | アプリサーバー（メタデータ API + 音声配信） |
| `docs/` | 検証レポート・設計ドキュメント |

## 使用ライブラリ

- **PSP 側**: intraFont（PSP 内蔵 PGF フォント描画。日本語表示可）/ sceGu（描画）/
  sceMp3 + sceAudio（Media Engine でのハードウェア MP3 デコード）/ sceNetInet（生ソケット）
  - **JSON パーサは使わない**。サーバーがタブ区切りテキスト（TSV）を返すことで、
    PSP 側の解析を最小限に抑えている
- **サーバー側**: ytmusicapi（メタデータ）/ yt-dlp（音声取得）/ ffmpeg（MP3 変換）

## サーバーの起動

```bash
cd server
python3 -m venv .venv && ./.venv/bin/pip install ytmusicapi
./.venv/bin/python app.py 8080
```

ffmpeg と yt-dlp が PATH にあること（`brew install ffmpeg yt-dlp`）。

### ログイン（任意）

PSP 上で Google ログインは技術的に不可能（現代 TLS と JavaScript 実行が必要）なため、
**認証はサーバー側に一度だけ設定する**。アプリの接続画面はその認証状態を表示する。

未設定でも一般向けのホームフィードで動作する。マイミックス等の
パーソナライズされた内容を出したい場合のみ設定する。

```bash
cd server
./.venv/bin/ytmusicapi browser   # 指示に従いブラウザのリクエストヘッダを貼る
mkdir -p auth && mv browser.json auth/
```

`server/auth/` は `.gitignore` 済み。**認証ファイルをコミットしないこと。**

## PSP アプリのビルド

```bash
export PSPDEV="$HOME/pspdev-install/pspdev"
export PATH="$PSPDEV/bin:$PATH"
cd psp-client/app
make SERVER_HOST=192.168.x.x     # サーバーを動かしている PC の LAN IP
```

`EBOOT.PBP` ができるので、実機では **`ef0:/PSP/GAME/ytmusic/`**（PSP Go の内蔵ストレージ）
に置く。実機セットアップは [../../docs/psp-go-setup.md](../../docs/psp-go-setup.md) を参照。

### PPSSPP で動かす

```bash
make          # SERVER_HOST=127.0.0.1 のまま
cp EBOOT.PBP ~/.config/ppsspp/PSP/GAME/YTMUSIC/
```

- PPSSPP は `flash0` が仮想ファイルシステムでフォントを開けないことがある。
  その場合はアプリと同じ階層に `font/ltn8.pgf` と `font/jpn0.pgf` を置くと読み込まれる
  （PPSSPP.app 内の `Contents/Resources/assets/flash0/font/` からコピーできる）。
  **フォントファイルはリポジトリにコミットしない**（`.gitignore` 済み）
- 自動操作でのテストは `make AUTODEMO=1`（一定フレーム後に○を自動入力する）

## 検証済みの状態

- PPSSPP 上で 接続 → ホーム → プレイリスト → 再生 の全経路が動作
- 連続再生の安定性: 10 秒あたり 384 デコードフレーム（理論値 380）で実時間ちょうど
- 実機（PSP Go + CFW）での動作確認は未実施

## 注意

YouTube を音源にする経路は YouTube API 利用規約に抵触する
（詳細: [docs/verification-youtube-music.md](docs/verification-youtube-music.md)）。
実験用途に留め、常用音源には Navidrome 等の自前ライブラリ配信を推奨する。
