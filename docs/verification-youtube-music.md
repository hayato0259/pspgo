# 検証レポート: PSP Go で YouTube Music を再生できるか

実施日: 2026-07-26 / 検証環境: macOS 26 (arm64), pspdev v20260701, PPSSPP 1.20.4

## 結論

- **技術的には再生できる。** ただし PSP 単体では不可能で、**自宅サーバー経由の2段構成が必須**
  - PSP は現代の TLS を扱えず、Opus もデコードできないため、YouTube に直接つなぐ選択肢は最初から存在しない
  - サーバーが「取得 + MP3 CBR 128kbps 変換 + 平文HTTP配信」をすべて担い、PSP は「HTTP GET して MP3 をハードウェアデコードする」だけに徹する
- **本検証で全経路の疎通をエミュレータ上で確認済み**
  - YouTube → yt-dlp → ffmpeg → HTTP → PSPクライアント(自作) → sceMp3 → sceAudio が実時間で動いた
- **ただし YouTube Music を音源にする構成は利用規約上グレーではなく明確に違反側**（詳細は後述）。
  常用アプリの音源には Navidrome (Subsonic API) など自前ライブラリ配信を推奨する

## 検証結果の詳細

### サーバー側（成功）

- **構成**: [server/proxy.py](../server/proxy.py) — Python 標準ライブラリのみ + ffmpeg + yt-dlp
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

## 実機（PSP Go, 6.61 + CFW）で残る確認事項

エミュレータで検証できていない、実機でのみ確認できる項目:

1. 実機 Wi-Fi (802.11b, 最大実効 ~500KB/s 程度) での安定スループット
2. sceMp3 の実機挙動（PPSSPP は ffmpeg による近似実装）
3. PSP Go 固有: 内蔵ストレージへの EBOOT 配置 (`ms0:/PSP/GAME/`)、スリープ復帰時の Wi-Fi 再接続
4. WPA2 対応状況（PSP は WPA2-AES に公式対応していない世代。ルーター側で
   WPA/TKIP を許可する SSID を用意する必要がある可能性が高い）

## 次の一手

実機に CFW (Ark-4 推奨) を導入したら、PoC2 を `make SERVER_HOST=<MacのLAN IP>` で
ビルドして `ms0:/PSP/GAME/poc2/EBOOT.PBP` に置き、実機での再生とスループットを確認する。
