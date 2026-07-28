#!/bin/bash
# Raspberry Pi 側で1回実行するセットアップ。何度実行しても同じ結果になる (idempotent)。
#
# 通常は Mac から ../deploy-pi.sh 経由で自動実行されるので、手で叩く必要はない。
# 手動で使う場合: server/ ディレクトリを Pi に置いて、その中で ./setup-pi.sh
#
# やること: 依存パッケージ導入 → venv 構築 → systemd 常駐設定 →
#           yt-dlp 週次更新タイマー → 起動確認
set -euo pipefail

SERVER_DIR="$(cd "$(dirname "$0")" && pwd)"
SERVICE_USER="$(id -un)"
PORT="${PORT:-8080}"
UNIT=/etc/systemd/system/ytmusic-server.service
UPDATE_UNIT=/etc/systemd/system/ytmusic-ytdlp-update.service
UPDATE_TIMER=/etc/systemd/system/ytmusic-ytdlp-update.timer

echo "==> 環境: $(uname -m) / user=$SERVICE_USER / dir=$SERVER_DIR"

# --- 依存パッケージ ---------------------------------------------------------
# 揃っていれば apt を動かさない (sudo を求める回数と所要時間を減らすため)
MISSING=()
command -v ffmpeg >/dev/null || MISSING+=(ffmpeg)
command -v curl   >/dev/null || MISSING+=(curl)
# yt-dlp は YouTube の JS チャレンジを外部のランタイムに解かせる。
# 無いと「Requested format is not available」で曲が取れない (Cookie 使用時)。
# quickjs は apt で入るが Pi では 1 回 30 秒かかり実用にならないため、
# 保険として入れつつ、本命の deno を後で別に入れる
command -v qjs    >/dev/null || MISSING+=(quickjs)
python3 -c 'import venv, ensurepip' 2>/dev/null || MISSING+=(python3-venv)
if (( ${#MISSING[@]} )); then
    echo "==> apt で導入: ${MISSING[*]}"
    sudo apt-get update -qq
    sudo apt-get install -y -qq "${MISSING[@]}"
else
    echo "==> 依存パッケージは導入済み (apt は実行しない)"
fi

# --- Python 環境 (yt-dlp は apt ではなく venv に入れる) ---------------------
echo "==> venv"
[[ -x "$SERVER_DIR/.venv/bin/python" ]] || python3 -m venv "$SERVER_DIR/.venv"
"$SERVER_DIR/.venv/bin/pip" install -q --upgrade pip
"$SERVER_DIR/.venv/bin/pip" install -q -r "$SERVER_DIR/requirements.txt"
echo "    yt-dlp $("$SERVER_DIR/.venv/bin/yt-dlp" --version)"
echo "    quickjs $(qjs --help 2>&1 | head -1 | awk '{print $3}')"

# --- deno (JS チャレンジの本命。apt に無いので公式バイナリを置く) ----------
# quickjs は JIT が無く Pi 4 で 1 回の取得に CPU を 30 秒使う (実測)。
# deno なら同じ処理が半分以下で済む。sudo 不要の ~/.deno/bin に置けば
# サーバーが自分で見つけて yt-dlp に渡す (app.py の js_runtime)
if ! command -v deno >/dev/null && [[ ! -x "$HOME/.deno/bin/deno" ]]; then
    echo "==> deno を導入 (~/.deno/bin)"
    mkdir -p "$HOME/.deno/bin"
    curl -sL -o /tmp/deno.zip \
      "https://github.com/denoland/deno/releases/latest/download/deno-$(uname -m)-unknown-linux-gnu.zip"
    unzip -o -q /tmp/deno.zip -d "$HOME/.deno/bin"
    chmod +x "$HOME/.deno/bin/deno"
    rm -f /tmp/deno.zip
fi
echo "    deno $("$HOME/.deno/bin/deno" --version 2>/dev/null | head -1 || deno --version | head -1)"
echo "    ffmpeg $(ffmpeg -version | head -1 | cut -d' ' -f3)"

# --- 認証ファイル -----------------------------------------------------------
if [[ -f "$SERVER_DIR/auth/browser.json" ]]; then
    chmod 700 "$SERVER_DIR/auth"
    chmod 600 "$SERVER_DIR/auth"/*
    echo "==> 認証ファイルあり (マイミックス等が表示される)"
else
    echo "==> 注意: auth/browser.json がありません。一般向けフィードで動きます"
    echo "    Mac から: scp -r auth/ ${SERVICE_USER}@<このPi>:$SERVER_DIR/"
fi

# --- systemd 常駐 -----------------------------------------------------------
# メモリ上限は搭載量から決める (1GB機で暴走してOS全体を巻き込むのを防ぐ)
MEM_KB="$(awk '/MemTotal/{print $2}' /proc/meminfo)"
if   (( MEM_KB > 3500000 )); then MEM_MAX=1500M
elif (( MEM_KB > 1800000 )); then MEM_MAX=1G
elif (( MEM_KB > 900000 ));  then MEM_MAX=600M
else                              MEM_MAX=350M
fi
echo "==> systemd ユニット (MemoryMax=$MEM_MAX)"

sudo tee "$UNIT" >/dev/null <<EOF
[Unit]
Description=youtube-music streaming proxy for PSP
Documentation=https://github.com/hayato0259/pspgo
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
WorkingDirectory=$SERVER_DIR
Environment=PATH=$SERVER_DIR/.venv/bin:/usr/local/bin:/usr/bin:/bin
ExecStart=$SERVER_DIR/.venv/bin/python app.py $PORT
Restart=always
RestartSec=5
MemoryMax=$MEM_MAX
Nice=5

[Install]
WantedBy=multi-user.target
EOF

# --- yt-dlp の週次更新 (YouTube 側の変更で定期的に壊れるため) ---------------
sudo tee "$UPDATE_UNIT" >/dev/null <<EOF
[Unit]
Description=Update yt-dlp for ytmusic-server

[Service]
Type=oneshot
User=$SERVICE_USER
ExecStart=$SERVER_DIR/.venv/bin/pip install -q -U 'yt-dlp[default]'
ExecStartPost=/usr/bin/sudo /bin/systemctl restart ytmusic-server
EOF

sudo tee "$UPDATE_TIMER" >/dev/null <<'EOF'
[Unit]
Description=Weekly yt-dlp update

[Timer]
OnCalendar=Sun 04:00
Persistent=true

[Install]
WantedBy=timers.target
EOF

# 更新後の再起動を無人で行えるよう、この操作だけ sudo をパスワード無しにする
sudo tee /etc/sudoers.d/ytmusic-restart >/dev/null <<EOF
$SERVICE_USER ALL=(root) NOPASSWD: /bin/systemctl restart ytmusic-server
EOF
sudo chmod 440 /etc/sudoers.d/ytmusic-restart

# 手動で起動したままのプロセスがポートを掴んでいると systemd 側が bind できない
HOLDER="$(ss -ltnpH "sport = :$PORT" 2>/dev/null | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2 || true)"
if [[ -n "${HOLDER:-}" ]]; then
    echo "==> ポート $PORT を掴んでいるプロセス($HOLDER)を停止"
    kill "$HOLDER" 2>/dev/null || true
    sleep 2
fi

sudo systemctl daemon-reload
sudo systemctl enable -q --now ytmusic-server
sudo systemctl enable -q --now ytmusic-ytdlp-update.timer
sudo systemctl restart ytmusic-server

# --- 起動確認 ---------------------------------------------------------------
echo "==> 起動確認"
for _ in $(seq 1 20); do
    if curl -fs --max-time 3 "http://127.0.0.1:$PORT/api/status" >/dev/null; then break; fi
    sleep 1
done
if ! curl -fs --max-time 3 "http://127.0.0.1:$PORT/api/status"; then
    echo "!! 応答がありません。ログ: journalctl -u ytmusic-server -n 50 --no-pager" >&2
    exit 1
fi

IP="$(hostname -I | awk '{print $1}')"
cat <<EOF

==> 完了しました。

  PSP 側の server.txt にこれを書いてください (ホスト名は不可。IP のみ):

      $IP:$PORT

  ログ:   journalctl -u ytmusic-server -f
  状態:   systemctl status ytmusic-server
EOF
