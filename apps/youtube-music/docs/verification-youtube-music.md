# 検証レポート: PSP Go で YouTube Music を再生できるか

実施日: 2026-07-26 / 検証環境: macOS 26 (arm64), pspdev v20260701, PPSSPP 1.20.4

## 結論

- **技術的には再生できる。** ただし PSP 単体では不可能で、**自宅サーバー経由の2段構成が必須**
  - PSP から YouTube へ直接つなぐ選択肢は実質存在しない。TLS 1.2 自体は mbedTLS の静的リンクで
    実機動作の前例があるが（Moonlight PSP・tls4psp）、YouTube 側の PO Token / SABR 強制は
    ブラウザ相当の JS 実行環境を要求するため、PSP 上では解決不能
  - Opus（YouTube の主流音声）は PSP でハードウェアデコードできず、ソフトデコードは
    fixed-point 再ビルドが必要で CPU の 20-35% を食う（推定）。サーバー側変換が合理的
  - サーバーが「取得 + MP3 CBR 128kbps 変換 + 平文HTTP配信」をすべて担い、PSP は「HTTP GET して MP3 をハードウェアデコードする」だけに徹する
- **本検証で全経路の疎通をエミュレータ上で確認済み**
  - YouTube → yt-dlp → ffmpeg → HTTP → PSPクライアント(自作) → sceMp3 → sceAudio が実時間で動いた
- **ただし YouTube Music を音源にする構成は利用規約上グレーではなく明確に違反側**（詳細は後述）。
  常用アプリの音源には Navidrome (Subsonic API) など自前ライブラリ配信を推奨する

## 検証結果の詳細

### サーバー側（成功）

- **構成**: server/proxy.py (検証当時の PoC。現在は [server/app.py](../server/app.py) に統合済み) — Python 標準ライブラリのみ + ffmpeg + yt-dlp
- **確認できたこと**
  - `/search?q=...` : yt-dlp の ytsearch で検索結果 JSON を返せる
  - `/stream?yt=<videoId>` : yt-dlp (bestaudio) → ffmpeg → MP3 CBR 128kbps / 44.1kHz / stereo に変換して配信
  - 実測スループット: 25秒で約15分ぶんの音声を配信（PSP再生に必要な 16KB/s の約40倍）
  - 変換後ストリームは ffprobe で正当な MP3 と確認

### PSP クライアント側（成功）

- **PoC1** [psp-client/poc1-audio](../psp-client/poc1-audio/main.c): sceAudio による PCM 出力
  - sceAudioChReserve 成功、1024サンプル単位の出力が実時間ペース（約23ms間隔）で駆動
- **PoC2** [psp-client/poc2-stream](../psp-client/poc2-stream/main.c): ストリーミング再生の結合検証
  - Wi-Fi 接続 (sceNetApctl) → 接続確立まで約6秒（PPSSPPのシミュレーション値）
  - TCP + 手書き HTTP/1.0 GET でプロキシに接続
  - sceMp3 がストリームを正しく解釈（44100Hz / stereo / 128kbps を自動検出）
  - ローカル音源: 1167フレーム（約30秒）を実時間でデコード・出力
  - YouTube 経由: 同一クライアントで 347フレーム以上を連続再生

### 検証中に踏んだ地雷（実装時に再び踏まないこと）

1. **newlib の BSD ソケット関数は使わない。**
   最新 pspdev の socket/connect/recv などは新しい NID でインポートされ、
   PPSSPP 1.20.4 が未実装（Unknown syscall）。古典的な `sceNetInet*` を直接呼ぶ。
   実機 6.61 でも `sceNetInet*` が本来の API なので、こちらに寄せて損はない
2. **PPSSPP のソケットはノンブロッキング。** `sceNetInetRecv` が EAGAIN (errno 11) を
   頻繁に返すため、リトライラッパー（recv_wait）が必須。実機はブロッキングなのでそのまま動く
3. **PPSSPP へは EBOOT.PBP を絶対パスで渡す。** 相対パスは `umd0:/` 扱いになり読めない
4. **sceMp3 のバッファは 64byte アライン必須**、ストリーム終端が不明な場合は
   `mp3StreamEnd = 0x7FFFFFFF` で通る
5. Homebrew tap `pspdev/pspdev` は存在しない。導入は GitHub Releases のプリビルト
   (`pspdev-macos-latest-arm64.tar.gz`) を展開する

## ログイン（OAuth）の検証結果 — 2026-07-26

実アカウントで検証した結果を記録する。**ログイン自体は成功するが、
得られたトークンで YouTube Music のパーソナライズ情報は取得できない。**

- **成功したこと**
  - Google Cloud の「テレビと入力が制限されているデバイス」種別クライアントで
    デバイスコードフローが動作（コード発行 → QR 表示 → 承認 → トークン保存）
  - トークンは正しく保存され、`/api/status` が `auth 1` を返す
- **失敗したこと: ytmusicapi + OAuth トークンで全エンドポイントが HTTP 400**
  - `get_home` / `get_library_playlists` / `get_liked_songs` / `get_history` /
    `get_account_info` / `search` すべてが
    `400 Bad Request: Request contains an invalid argument`
  - 未認証クライアントでは同じ操作が成功するため、実装側の不備ではなく
    **YouTube Music 内部 API が OAuth（TV クライアント）トークンを受け付けない**
    ことによるもの
  - 対策として、認証クライアントが失敗したら未認証クライアントへ退避する実装を入れた
    （ホーム画面の先頭に「一般向けの内容を表示しています」と明示する）。
    ログインしたことでアプリが使えなくなる状態は避けられている
- **未検証: 公式 YouTube Data API v3 経由のパーソナライズ**
  - トークン自体は有効だが、プロジェクトで **YouTube Data API v3 が未有効化**
    のため 403（`has not been used in project ... before or it is disabled`）
  - 有効化すれば `playlists.list(mine=true)`（自分のプレイリスト）や
    `playlistItems.list(playlistId=LL)`（高評価した動画）は取得できる見込み。
    これは公式 API なので規約上もクリーン
  - ただし「マイミックス」は YouTube Music が生成するもので Data API には無い。
    公式 API で出せるのは自分のプレイリスト・高評価・登録チャンネルまで

### 結論として取るべき方針

- **マイミックスが欲しいなら、ブラウザ認証 (`auth/browser.json`) を使う。**
  これは上流でも推奨されている回避策で、OAuth が 400 になる一方
  ブラウザ認証は正常に動作することが複数報告されている
  （[ytmusicapi#676](https://github.com/sigma67/ytmusicapi/issues/676),
  [#682](https://github.com/sigma67/ytmusicapi/discussions/682),
  [#921](https://github.com/sigma67/ytmusicapi/issues/921)）。
  サーバーは browser.json を OAuth トークンより優先して使う実装にした
- QR ログイン（OAuth）は仕組みとして完成しているので残す。
  将来 OAuth が復旧した場合、および公式 Data API v3 を使う場合に活かせる
- 一般向けフィードは未認証クライアントで引き続き取得できる

## 音声取得の地雷: `bestaudio` は Opus/WebM を選ぶ — 2026-07-26

`/stream?yt=` が **HTTP 200 を返しながら 0 バイト**になる事象が発生した。

- 原因: `yt-dlp -f bestaudio` が Opus/WebM を選び、それをパイプ経由で
  ffmpeg に渡すと変換が成立せず、無音のまま終了することがある
- 対策: **m4a (AAC) を優先する**書式指定に変更した
  `-f "bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]/bestaudio"`
  → 失敗していた曲が 3 分 11 秒ぶん正常に取得できるようになった
- あわせて、1 バイトも流れなかった場合に **yt-dlp の標準エラーをログに出す**
  ようにした。これが無いと「200 なのに無音」で原因が追えない
- なお同じ曲でも時間帯によって成否が変わることがあり、
  YouTube 側のスロットリングの影響は残る（PO Token / SABR の節を参照）

## YouTube Music を音源にすることの規約リスク（誠実な整理)

- YouTube の音声ストリームを取得する**公認 API は存在しない**。Data API v3 はメタデータのみ
- YouTube API デベロッパーポリシーは以下を明文で禁止しており、本構成はその複数に該当する
  - III.E.1: 音声映像コンテンツのダウンロード・キャッシュ・保存
  - III.I.7: 音声コンポーネントの分離
  - III.I.9: バックグラウンドプレイヤー
  - III.I.14: API 以外の手段でのデータ取得
- これは**契約違反（規約違反）であって直ちに刑事罰の話ではない**が、
  現実的リスクとして Google アカウント停止・IP ブロックがあり得る
- 技術面でも 2025-2026 年に PO Token / SABR 強制で yt-dlp 経路の難易度が上がっており、
  **「一度作れば動き続ける」構成にはならない**（壊れる前提で保守が必要）
- 今日の検証では yt-dlp 2026.07.04 で問題なく取得できた（現時点では動く）

## 本番アプリへの推奨アーキテクチャ

```
[音源] → [自宅サーバー: 取得/変換/配信] --平文HTTP(LAN内)--> [PSP Go 自作アプリ]
```

- **PSP アプリ**: 検索/プレイリスト UI + HTTP GET + sceMp3 再生（PoC2 の発展形）
- **サーバー**: 音源を差し替え可能にする
  - 第一推奨: **Navidrome (Subsonic API)** — `GET /rest/stream?id=..&format=mp3&maxBitRate=128`
    1本で完結。認証も MD5+salt のみで PSP 実装が軽い。規約リスクなし
  - YouTube Music は「実験用モード」として分離し、常用しない
- **配信フォーマットは MP3 CBR 128kbps / 44.1kHz 固定**に正規化
  - PSP の Media Engine で最も素性が良く、CPU をほぼ使わずデコードできる
  - Opus は PSP でデコード不可能なので必ずサーバー側で変換する
- **LAN 内限定運用**（平文 HTTP のため外部公開しない）

## 実機ハードウェアの制約（Webリサーチによる裏付け）

- **CPU コスト実測（LightMP3 の実績値）**: Media Engine での MP3 デコードは CPU 20MHz 相当で足りる
  （333MHz の約6%）。MP3 CBR 配信という設計は実機でも最も軽い経路
- **AAC もハードウェアデコード可能**: `sceAudiocodec` (PSP_CODEC_AAC=0x1003) / `sceAac`
  （FW 3.95以降、6.61 にあり）。前例は OpenTube（YouTube クライアント、ソース公開）。
  MP3 で問題が出た場合の代替経路として確保
- **Wi-Fi**: PSP Go は 802.11b のみ（2.4GHz 専用）。実効スループットの公開実測は
  100〜500KB/s と情報が食い違っており実機計測が必要。ただし音声 16KB/s には余裕
- **WPA2**: 純正は WEP / WPA-PSK まで（WPA2 非対応）だが、**ARK-4 r160 以降は
  WPA2-PSK(AES/CCMP) パッチを内蔵**。CCMP のみ・有効化中は WEP/WPA 不可という制約あり
- **メモリ**: ユーザー空間は既定 24MB。ARK-4 では EBOOT の PARAM.SFO に `MEMSIZE=1` を
  書けば最大 55MB に拡張可（ただし PSP Go のポーズ機能が無効になる）。
  大きめのジッタバッファを取りたくなったら使う
- **Media Engine は排他リソース**: ME を使う他プラグイン（Music.prx 等）と共存不可
- **Subsonic / Navidrome クライアントの PSP homebrew は存在しない** — 作れば初の事例

## 実機（PSP Go, 6.61 + CFW）で残る確認事項

エミュレータで検証できていない、実機でのみ確認できる項目:

1. 実機 Wi-Fi での安定スループットとパケットロス（ジッタバッファ設計の根拠になる）
2. sceMp3 の実機挙動（PPSSPP は ffmpeg による近似実装）
3. PSP Go 固有: 内蔵ストレージへの EBOOT 配置 (`ms0:/PSP/GAME/`)、スリープ復帰時の Wi-Fi 再接続
4. ARK-4 の WPA2 パッチが PSP Go (09g) で動くか（機種分岐はないので動く見込み）

## 次の一手

実機に CFW を導入したら（選定と手順は [../../../docs/psp-go-setup.md](../../../docs/psp-go-setup.md)。
ARK-4 は EOL、現行は FasterARK/ARK-5 だが PSP Go 固有バグありの pre-release）、
PoC2 を `make SERVER_HOST=<MacのLAN IP>` でビルドして
**`ef0:/PSP/GAME/poc2/EBOOT.PBP`**（PSP Go の内蔵ストレージ。ms0: ではない）に置き、
実機での再生とスループットを確認する。
