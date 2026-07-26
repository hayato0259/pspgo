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
| 接続 | Wi-Fi 接続 → サーバーの認証状態を確認 |
| ログイン選択 | ログインする / ログインせずに使う |
| ログイン | 入力コードと URL を表示し、承認されるまで待機 |
| ホーム | セクション見出し付きのプレイリスト一覧（マイミックス等） |
| プレイリスト | 曲一覧（曲名・アーティスト・長さ） |
| 再生 | 曲名・状態・経過時間・進捗バー・次曲表示 |

| ボタン | 動作 |
|---|---|
| 上下 | 選択移動（長押しリピート） |
| ○ | 決定（プレイリストを開く / 再生開始 / ログイン開始） |
| × | 前の画面へ戻る（ログイン中は中止） |
| △ | 一時停止 / 再開 |
| L / R | 前の曲 / 次の曲 |
| SELECT | ホーム画面でログイン / ログアウト |
| START | 終了 |

曲の終端に達すると自動で次の曲へ進む。

## アプリ内ログイン

**Google 公式の OAuth デバイスコードフロー**（テレビやゲーム機の YouTube アプリと同じ方式）を使う。

1. アプリでログインを選ぶ
2. **画面に QR コードが表示される**
3. スマートフォンで読み取ると、コードが入力済みの承認ページが開く。承認する
4. アプリが自動で承認を検知し、マイミックス等が並ぶホームへ進む

QR が読み取れない場合に備えて、URL と入力コードも同じ画面に併記している。
QR コードはサーバーが白黒マスの配列として送り、PSP 側はそれを四角形として
描くだけなので、画像デコーダを持たずに表示できる。

PSP 自体では現代の TLS も JavaScript も扱えないため、Google との通信とトークンの保管は
サーバーが担う。PSP 側の役割はコードの表示と承認完了のポーリングのみ。

ログインせずに使うこともでき、その場合は一般向けのホームフィードが表示される。

### OAuth クライアントの用意（初回のみ）

Google の仕様上、**自分の Google Cloud プロジェクトで OAuth クライアントを作る必要がある**。
Firebase プロジェクトは Google Cloud プロジェクトと同一なので、既存の Firebase
プロジェクトをそのまま使える（ただし設定場所は Firebase コンソールではなく
**Google Cloud Console** 側）。

1. [Google Cloud Console](https://console.cloud.google.com/) で対象プロジェクトを選ぶ
2. 「APIとサービス」→「ライブラリ」→ **YouTube Data API v3** を有効化
3. 「APIとサービス」→「OAuth 同意画面」を設定
   - User Type は「外部」
   - 公開状態が「テスト中」の間は、**自分の Google アカウントを
     「テストユーザー」に追加する**。追加していないとログイン承認が拒否される
4. 「APIとサービス」→「認証情報」→「認証情報を作成」→「OAuth クライアント ID」。
   アプリケーションの種類は **「テレビと入力が制限されているデバイス」** を選ぶ
   （「ウェブアプリケーション」ではデバイスコードフローが使えない）
5. 発行された client_id / client_secret を `server/auth/oauth_client.json` に置く。
   `oauth_client.example.json` をコピーして書き換えるとよい:

```json
{ "client_id": "...apps.googleusercontent.com", "client_secret": "..." }
```

Google Cloud からダウンロードした JSON（`installed` でネストした形式）もそのまま置ける。

6. 設定が正しいかを確認する:

```bash
cd server
./.venv/bin/python tools/check_oauth_client.py
```

ファイルの形式、クライアント種別、そして実際に Google からデバイスコードを
取得できるかまで検査する。`client_secret` は画面に出力しない。

未設定の場合、アプリはログイン選択画面を出さず、一般向けホームで起動する。

`server/auth/` は `.gitignore` 済み。**client_secret とトークンをコミットしないこと。**

## 構成

| パス | 内容 |
|---|---|
| `psp-client/app/` | アプリ本体（C）。`src/net.c` 通信 / `src/api.c` TSV解析 / `src/player.c` 再生スレッド / `src/main.c` UI |
| `psp-client/poc1-audio/`, `poc2-stream/` | 実現可否検証に使った PoC（残置） |
| `server/app.py` | アプリサーバー（メタデータ API + 音声配信） |
| `docs/` | 検証レポート・設計ドキュメント |

## 使用ライブラリ

- **PSP 側**: intraFont（PSP 内蔵 PGF フォント描画。UI は日本語表示）/ sceGu（描画）/
  sceMp3 + sceAudio（Media Engine でのハードウェア MP3 デコード）/ sceNetInet（生ソケット）
  - **JSON パーサは使わない**。サーバーがタブ区切りテキスト（TSV）を返すことで、
    PSP 側の解析を最小限に抑えている
  - **フォントは jpn0.pgf を主フォントにする**。ltn8.pgf を主にして代替フォントで
    日本語へ逃がす構成では、連続する日本語文字が 1 文字目以降描画されない
    （検証ツール: `psp-client/fontcheck/`）
- **サーバー側**: ytmusicapi（メタデータ）/ yt-dlp（音声取得）/ ffmpeg（MP3 変換）/
  qrcode（ログイン用 QR。未インストールでもコード入力で動く）

## サーバーの起動

```bash
cd server
python3 -m venv .venv && ./.venv/bin/pip install ytmusicapi qrcode
./.venv/bin/python app.py 8080
```

ffmpeg と yt-dlp が PATH にあること（`brew install ffmpeg yt-dlp`）。

ログインはアプリ内で行う（上記「アプリ内ログイン」を参照）。
`server/auth/oauth_client.json` を置いておけば、あとは PSP の画面だけで完結する。

旧方式として、ブラウザのリクエストヘッダを使う `browser.json` も
`server/auth/` に置けば認識される（`./.venv/bin/ytmusicapi browser` で生成）。

### ログインフローのテスト

実際の Google アカウントを使わずに状態遷移を検証できるフィクスチャがある。
Google のデバイスコードエンドポイントをモックし、本物の `app.py` を起動する。

```bash
cd server
./.venv/bin/python tests/mock_google_oauth.py 2 8080   # pending 2回 → 承認
```

**テスト用の偽 client_id を `auth/` に書き込むので、終わったら
`rm -f auth/oauth_client.json auth/oauth.json` で消すこと。**

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
- 連続再生の安定性: 10 秒あたり 383 デコードフレーム（理論値 380）で実時間ちょうど
- ログイン: モック（`tests/mock_google_oauth.py`）で
  コード取得 → 承認待ち → 承認 → トークン保存 → ホーム再取得までを確認
- **未確認**: 画面の見た目。PPSSPP はフレームバッファを CPU から読み出せないため、
  日本語表示や QR コードが意図通りに描かれているかは実機でしか確認できない
  （文字送り幅は `psp-client/fontcheck/` で数値検証済み）
- 実機（PSP Go + CFW）での動作確認は未実施

## 注意

YouTube を音源にする経路は YouTube API 利用規約に抵触する
（詳細: [docs/verification-youtube-music.md](docs/verification-youtube-music.md)）。
実験用途に留め、常用音源には Navidrome 等の自前ライブラリ配信を推奨する。
