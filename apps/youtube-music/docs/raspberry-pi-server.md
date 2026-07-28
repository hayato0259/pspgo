# Raspberry Pi にサーバーを移す（Mac を開かなくてよくする）

配信サーバーを Raspberry Pi に載せ替えるための手順。
**想定は Raspberry Pi 4**（Pi 3 でも足りる。後述の実測手順で確認できる）。

これで Mac を起動しておく必要がなくなり、家にいる間はいつでも PSP から聴ける。

## なぜ自宅に置くのか（VPS ではなく）

YouTube はデータセンターの IP アドレスからの取得を「ボットではないことを確認」で
弾くため、VPS 上の yt-dlp は失敗しやすい。回避には住宅用プロキシか
Cookie の持ち込みが必要で、後者はアカウント停止のリスクを増やす。
**住宅回線に置くのが最も確実**という理由で自宅を選んでいる。
（自前ライブラリ配信の Navidrome など、YouTube を経由しない音源なら VPS でも問題ない）

## 必要な性能とモデルごとの適性

このサーバーの負荷は小さい。必要なのは次の 4 つだけ。

| 項目 | 必要量 |
|---|---|
| MP3 変換 | 実時間の 1 倍（libmp3lame は軽い） |
| 同時に走る ffmpeg | 最大 2 本（再生 1 本 + 一括ダウンロード 1 本。クライアントは 1 曲ずつ落とす） |
| ネットワーク | 毎秒 16KB（128kbps） |
| メモリ | Python + yt-dlp で 200〜400MB |

| モデル | CPU / RAM | 適性 |
|---|---|---|
| **Pi 4** | 4コア Cortex-A72 1.5〜1.8GHz / 1〜8GB | **推奨。** 変換に十分な余裕があり、有線が本物のギガビット、無線も 5GHz 対応 |
| Pi 3 / 3B+ | 4コア Cortex-A53 1.2〜1.4GHz / 1GB | 足りる |
| Pi Zero 2 W | 4コア Cortex-A53 1.0GHz / 512MB | 使える見込み。メモリが半分なので `MemoryMax=350M` + スワップ 512MB に調整 |
| Pi Zero / Zero W | 1コア **ARMv6** 1.0GHz / 512MB | **非推奨。** 変換が実時間に追いつかない可能性が高く、ffmpeg の ARMv6 ビルドで `Illegal instruction` を踏む報告も多い |

**OS は Raspberry Pi OS Lite (64-bit) を選ぶ。** デスクトップ版はこのサーバーには不要。

### Pi 4 固有の注意

- **電源は 5V/3A の USB-C を使う**（公式アダプタが確実）。容量不足だと電圧低下で
  自動的にクロックが下がり、変換が遅くなる。`vcgencmd get_throttled` が `0x0` 以外なら疑う
- **発熱対策をする。** Pi 3 より熱くなるのでヒートシンクは実質必須、ケースに入れるならファンも。
  85℃ でクロックが下がる（`vcgencmd measure_temp` で確認）
- **メモリ 2GB 以上なら systemd の上限を緩める。** [../server/ytmusic-server.service](../server/ytmusic-server.service)
  の `MemoryMax=600M` は Pi 3 (1GB) 想定なので、`1G` 程度にしてよい
- **microSD より USB SSD 起動が安定する**（Pi 4 は USB からブートできる）。
  24時間稼働で microSD が壊れるのを避けたい場合の選択肢
- 有線 LAN が使えるならそちらを選ぶ（Pi 3 と違い USB 経由ではない本物のギガビット）

## セットアップ（自動・推奨）

Mac から 1 コマンドで配置とセットアップができる。**リポジトリは private なので
Pi から `git clone` はしない**（Pi に GitHub の認証情報を置かずに済むよう、
必要な `server/` だけを rsync で送る方式にしてある）。

```bash
cd apps/youtube-music
./deploy-pi.sh <user>@<PiのIPまたはホスト名>       # 例: ./deploy-pi.sh pi@192.168.0.50
```

やること: `server/` を Pi の `~/pspgo-server/` へ転送 → [../server/setup-pi.sh](../server/setup-pi.sh)
を実行（apt 導入・venv 構築・systemd 常駐・yt-dlp 週次更新タイマー・起動確認）→
最後に **PSP の `server.txt` に書くべき IP アドレスを表示**する。

- 何度実行しても同じ結果になる。コードを直したら同じコマンドで更新できる
- `--no-auth` を付けると認証ファイル（`auth/`）を送らない（一般向けフィードで動作）
- **既定以外の鍵で接続する場合は環境変数で渡す**（鍵の場所をリポジトリに書かないため）:

  ```bash
  PSPGO_SSH_KEY=~/.ssh/<鍵の名前> ./deploy-pi.sh <user>@<host>
  ```

- **セットアップの後半は `sudo` を使う**（systemd ユニットの更新）。
  パスワードの入力が要るので、対話できる端末から実行すること。
  コードだけを入れ替えたい場合は、転送後にサービスを再起動すれば足りる:

  ```bash
  ssh <user>@<host> 'sudo systemctl restart ytmusic-server'
  ```
- `MemoryMax` は Pi の搭載メモリから自動で決まる（1GB機なら 600M、4GB機なら 1500M）

### 名前解決の注意（`.local` は PSP から使えない）

Mac からは `xxx.local` で繋がるが、**PSP クライアントは DNS も mDNS も引かない**
（`net.c` が IPv4 アドレスを直接パースする実装）。
`server.txt` には**必ず IP アドレスを書く**こと。だから Pi の IP は固定しておく。

### 再インストールしたときの SSH 警告

同じホスト名で OS を焼き直すと、ホスト鍵が変わるため
`REMOTE HOST IDENTIFICATION HAS CHANGED` で接続を拒否される。
**新しい Pi であることが確実な場合のみ**古い鍵を消す:

```bash
ssh-keygen -R <ホスト名>      # 例: ssh-keygen -R raspberrypi.local
```

## セットアップ（手動）

`deploy-pi.sh` が使えない場合や、中で何をしているかを確認したい場合の手順。

### 1. OS と依存パッケージ

```bash
sudo apt update
sudo apt install -y git python3-venv ffmpeg
```

`yt-dlp` は **apt で入れないこと。** Debian のパッケージは古く、YouTube 側の変更で
すぐ壊れる。後述の venv に pip で入れて、定期的に更新する。

### 2. リポジトリとサーバー

```bash
git clone https://github.com/hayato0259/pspgo.git ~/pspgo
cd ~/pspgo/apps/youtube-music/server
python3 -m venv .venv
./.venv/bin/pip install -r requirements.txt
```

`requirements.txt` に yt-dlp が含まれているので、venv の中に新しいものが入る。

### 3. 認証ファイルを Mac から持ってくる

マイミックス等を出すにはブラウザ認証のファイルが必要。**リポジトリには入っていない**
（`.gitignore` 済み）ので、Mac から手で運ぶ。

```bash
# Mac 側で実行
scp -r apps/youtube-music/server/auth pi@<PiのIP>:~/pspgo/apps/youtube-music/server/
```

```bash
# Pi 側で実行（Google のセッション情報なので他人に読ませない）
chmod 700 ~/pspgo/apps/youtube-music/server/auth
chmod 600 ~/pspgo/apps/youtube-music/server/auth/*
```

### 4. IP アドレスを固定する（重要）

**PSP クライアントは DNS を引かず、`server.txt` に書いた IPv4 アドレスへ直接繋ぐ。**
Pi の IP が DHCP で変わると PSP から見えなくなるので、必ず固定する。

いちばん確実なのはルーターの DHCP 予約（機器の MAC アドレスに固定 IP を割り当てる設定）。
Pi 側で固定する場合は NetworkManager で行う:

```bash
nmcli connection show                     # 接続名を調べる (例: preconfigured)
sudo nmcli connection modify "preconfigured" \
  ipv4.method manual \
  ipv4.addresses 192.168.0.50/24 \
  ipv4.gateway 192.168.0.1 \
  ipv4.dns 192.168.0.1
sudo reboot
```

アドレスは自宅の LAN に合わせて読み替える。

### 5. 常駐させる（systemd）

同梱の [../server/ytmusic-server.service](../server/ytmusic-server.service) を使う。
ユーザー名やパスが違う場合は書き換える。

```bash
sudo cp ~/pspgo/apps/youtube-music/server/ytmusic-server.service \
        /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ytmusic-server
systemctl status ytmusic-server           # active (running) を確認
```

動作確認（Pi 上でも Mac からでもよい）:

```bash
curl http://192.168.0.50:8080/api/status
# auth 1 / name <表示名> / can_login 1 が返れば成功
```

ログは `journalctl -u ytmusic-server -f` で追える。

### 6. yt-dlp を自動更新する

YouTube 側の変更で yt-dlp は定期的に壊れる。週 1 回更新するタイマーを入れる。

`/etc/systemd/system/ytmusic-ytdlp-update.service`:

```ini
[Unit]
Description=Update yt-dlp for ytmusic-server

[Service]
Type=oneshot
ExecStart=/home/pi/pspgo/apps/youtube-music/server/.venv/bin/pip install -U yt-dlp
ExecStartPost=/bin/systemctl restart ytmusic-server
User=pi
```

`/etc/systemd/system/ytmusic-ytdlp-update.timer`:

```ini
[Unit]
Description=Weekly yt-dlp update

[Timer]
OnCalendar=Sun 04:00
Persistent=true

[Install]
WantedBy=timers.target
```

```bash
sudo systemctl enable --now ytmusic-ytdlp-update.timer
```

### 7. PSP 側の接続先を書き換える

EBOOT と同じフォルダ（`ef0:/PSP/GAME/YTMUSIC/`）の `server.txt` を Pi の IP にする:

```
192.168.0.50:8080
```

PPSSPP で試すときは `~/.config/ppsspp/PSP/GAME/YTMUSIC/server.txt` を同じ内容にすれば、
Mac のサーバーを止めたまま Pi 側で動作確認できる。

## 性能を実測する

「Pi 3 で本当に足りるか」は変換速度で判定できる。1 曲ぶんを変換して `speed=` を見る:

```bash
cd ~/pspgo/apps/youtube-music/server
./.venv/bin/yt-dlp --no-warnings -o - -f 'bestaudio[ext=m4a]/bestaudio' \
  'https://music.youtube.com/watch?v=<videoId>' \
  | ffmpeg -hide_banner -i pipe:0 -vn -c:a libmp3lame -b:a 128k -ar 44100 -ac 2 \
           -f mp3 -y /dev/null
```

- 出力末尾の `speed=` が **1x を超えていれば途切れずに配信できる**
  （再生と同時に一括ダウンロードもするなら 2x 以上あると安心）
- 1x を下回る場合は、`-b:a 96k` に落とすか、聴く曲を事前にダウンロードして
  オフライン再生にする（PSP 側で □ ボタン）

## 実際に構築して分かったこと (2026-07-28, Raspberry Pi 4 / 4GB / Trixie 64bit)

- **性能は十分**: 4分08秒の曲を32秒で取得・変換（**実時間の 8.8 倍速**）。
  ボトルネックは変換ではなく YouTube からのダウンロード（約142KB/秒）
- **ffmpeg と python3-venv は最初から入っていた**ので apt での追加導入は不要だった
- **`yt-dlp` が PATH から見えず起動に失敗した。** venv に入れる構成のため
  `.venv/bin/python app.py` では見つからない。`app.py` 側で自分と同じ場所を
  PATH に足すよう修正済み（systemd の Environment=PATH でも対策している）
- **一括ダウンロードと再生を同時に行うと再生が落ちた** (`0x807F00FD`)。
  クライアントの「供給停止」判定が 2 秒しかなく、サーバーが 1 曲ごとに
  yt-dlp を起動する待ち時間を誤判定していた。受信量が増えている間は
  待ち続けるよう修正済み。**Mac のサーバーでは再現しない**（起動が速いため）
- **電源不足の警告が出た** (`vcgencmd get_throttled` が `0x50005`)。
  Pi 4 は 5V/3A が必要。`0x0` 以外なら電源かケーブルを疑う
- **ホスト名 `.local` は PSP から使えない**（PSP は DNS も mDNS も引かない）。
  `server.txt` には IP アドレスを書く

## 運用上の注意

- **microSD への書き込みを増やさない。** このサーバーはファイルをキャッシュしないので
  通常は問題ないが、ログを大量に出す変更を入れるときは注意する
- **メモリ上限を入れてある**（`MemoryMax=600M`）。1GB の Pi 3 で暴走したときに
  OS 全体が固まるのを防ぐため。足りなくなったら値を上げる
- **温度**: MP3 変換は軽いので通常は問題ないが、密閉ケースならヒートシンクを付ける
  （80℃ を超えると自動的にクロックが下がる）
- **外出先から使いたい場合**、このサーバーをそのままインターネットに公開しないこと。
  平文 HTTP かつ認証が無いため。HTTPS 化（mbedTLS）とトークン認証を実装するまでは、
  オフライン再生（ダウンロード済みの曲）で運用する
