# PSP Go (PSP-N1000) 実機セットアップと開発ループ

2026-07 時点の Web リサーチに基づく。アプリを問わない本体側の知識はここに集約する。

## CFW の選定（2026年時点）

- **ARK-4 は 2026-05-15 に開発終了（EOL）を宣言済み。** 現行は ARK-5
- ユーザーが導入に使うのは **FasterARK**（ARK 5.0.2 のインストーラ同梱パッケージ）
  - https://github.com/PSP-Arkfive/FasterARK/releases/tag/latest
- **ただし ARK-5 は pre-release で PSP Go 固有の未解決バグがある**（2026-07時点）
  - #65: Pause 機能が複数ゲームで動かない / #72: スリープでゲームがクラッシュ / #57: Clock Plugin
  - 安定を優先するなら凍結済みの **ARK-4 最終版 (v42069r206)** が保守的な選択。cIPL は共通
- **PSP Go でもコールドブート CFW（cIPL）が使える**（2024-04 の新 cIPL で対応済み）。
  Infinity は cIPL 優先で非推奨になった。6.61 PRO-C は 2015 年で停止しており使わない
- ネット上の PSP Go 向け CFW 記事はほぼ全部古い。en.wikibooks の Motherboards 表も
  「Go は cIPL 不可」と書いてあるが 2024 年以降は誤り

## 導入手順（FasterARK の場合）

1. 本体設定 → 本体情報で System Software `6.61` を確認
2. `FasterARK_psp_full.zip` を取得し、中の `PSP/` フォルダを **`ef0:` のルート**へ展開
3. XMB → ゲーム → **FasterARK** を実行（ライブCFW化）
4. XMB → ゲーム → **Custom IPL** を実行して再起動（コールドブート永続化）
5. 復旧経路: 電源投入時 **L 押しっぱなし** → DC10（リカバリ）、**HOME 押しっぱなし** → OFW 起動
   - PSP Go は電池を外せず Pandora バッテリーが使えないため、
     **カーネル周りを触る前に必ず cIPL を焼いておく**

## PSP Go 固有の罠

- **内蔵16GBは `ef0:`、M2カードスロットが `ms0:`。** homebrew の設置先は
  `ef0:/PSP/GAME/<名前>/EBOOT.PBP`（M2 を使わない限り ef0: が主ターゲット）
- **apitype によるアクセス制限が最大の罠**:
  内蔵ストレージから起動したアプリ（apitype 0x152）は **`ms0:` に一切アクセスできない**。
  パス文字列を直しても回避できない。逆（ms0: 起動 → 両方見える）は可能
- 起動ドライブに依存しないコードにするには**相対パスを使う**
  （pspsdk の crt0 が起動パスへ chdir 済み。`getcwd()` で自分の場所が分かる）
- プラグイン設定は `ef0:/SEPLUGINS/PLUGINS.TXT`
- ARK-5 は既定で `ms0:` → `ef0:` リダイレクト（oldplugin）が ON。
  この挙動に依存したコードを書かないこと
- Extra RAM（MEMSIZE=1, 最大55MB）や Inferno Cache を有効にすると
  **PSP Go の Pause 機能が自動的に無効になる**

## 開発ループ（Mac → 実機）

- **USB 抜き差しより FTP が速い**: ARK は FTP サーバ（PSPFTP.PRX）を同梱。
  Custom Launcher のファイルマネージャで L → FTP モード。Mac から FTP で EBOOT を差し替える
- USB マスストレージを使う場合の注意（内蔵ストレージは FAT32）:
  - exFAT 不可・1ファイル4GB上限。フォーマットは MS-DOS (FAT) + MBR
  - macOS が書く `.DS_Store` / `._*` / `.Spotlight-V100` を掃除してから取り出す:
    `dot_clean -m /Volumes/PSPGO && find /Volumes/PSPGO -name '.DS_Store' -delete`
  - 取り出しは必ず `diskutil eject`（書き込み中に抜くと FAT が壊れて XMB がハングする既知事故）

## PPSSPP でのテスト（補足）

- homebrew の置き場所: `~/.config/ppsspp/PSP/GAME/<名前>/EBOOT.PBP`
  （ゲーム一覧の Homebrew & Demos タブに出る）
- ログ: `--log=<file>` と `-d` を併用。既定ログは `~/.config/ppsspp/PSP/SYSTEM/DUMP/log.txt`
- **sceSsl / sceHttps は空スタブ**。HTTPS を使うコードは PPSSPP では検証不能
  （本プロジェクトは平文 HTTP + 生ソケットなので影響なし）
- sceNetApctl が返す IP は Mac の実 LAN IP。DNS 解決もホスト OS 経由で動く
- ブロッキングソケットのタイムアウト・errno 挙動は実機と異なる
  （connect は5秒固定タイムアウト、ETIMEDOUT が EAGAIN に化ける）。実機で要再検証
