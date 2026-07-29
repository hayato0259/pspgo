#!/bin/bash
# Mac から Raspberry Pi へサーバーを配置してセットアップする。
#
#   ./deploy-pi.sh <user>@<host>             例: ./deploy-pi.sh pi@192.168.0.50
#   ./deploy-pi.sh <user>@<host> --no-auth   認証ファイル (auth/) を送らない
#   ./deploy-pi.sh <user>@<host> --code-only コードを送ってサービスを再起動するだけ
#                                            (初回セットアップ済みの更新用。sudo は
#                                             再起動にしか使わないので入力を求められない)
#
# 既定以外の鍵で接続する場合は環境変数で渡す:
#   PSPGO_SSH_KEY=~/.ssh/pspgo_pi ./deploy-pi.sh pi@192.168.0.50
#
# 2回目以降は同じコマンドで更新できる (rsync + セットアップの再実行)。
#
# リポジトリは private なので Pi から git clone しない。
# 必要な server/ だけを rsync で送る方式にしてある
# (Pi に GitHub の認証情報を置かずに済む)。
set -euo pipefail

TARGET="${1:-}"
if [[ -z "$TARGET" ]]; then
    echo "使い方: $0 <user>@<host> [--no-auth]" >&2
    exit 1
fi
SEND_AUTH=1
CODE_ONLY=0
for arg in "${@:2}"; do
    case "$arg" in
        --no-auth)   SEND_AUTH=0 ;;
        --code-only) CODE_ONLY=1 ;;
        *) echo "不明なオプション: $arg" >&2; exit 1 ;;
    esac
done

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$APP_DIR/server"
DEST="pspgo-server"          # Pi のホームからの相対パス
SERVICE="ytmusic-server"     # systemd のユニット名

# 鍵を指定されていれば ssh と rsync の両方に効かせる
SSH_CMD=(ssh)
if [[ -n "${PSPGO_SSH_KEY:-}" ]]; then
    SSH_CMD=(ssh -i "${PSPGO_SSH_KEY/#\~/$HOME}")
fi

echo "==> 転送先: $TARGET:~/$DEST"

EXCLUDES=(
    --exclude '.venv/'
    --exclude '__pycache__/'
    --exclude '*.pyc'
    --exclude 'server.log'
    --exclude 'cache/'         # 先読みキャッシュ。Pi 側で育つので消さない
    --exclude 'tests/__pycache__/'
    --exclude '.DS_Store'      # macOS が撒くメタデータを Linux 側へ持ち込まない
    --exclude '._*'
    # 外部公開用の共有トークンは Pi 側で生成したものが正。
    # Mac 側に無い状態で --delete すると Pi の token.txt が消え、
    # 開いているポートが無認証に戻ってしまうため必ず除外する
    --exclude 'auth/token.txt'
)
# 認証ファイルは Google のセッション情報を含む。送るかどうかを明示的に選ぶ
if [[ $SEND_AUTH -eq 0 ]]; then
    EXCLUDES+=(--exclude 'auth/')
    echo "    auth/ は送りません (一般向けフィードで動作)"
elif [[ -f "$SRC/auth/browser.json" ]]; then
    echo "    auth/ を送ります (Google のセッション情報を含むので取り扱い注意)"
else
    echo "    auth/browser.json が無いのでそのまま進みます"
fi

"${SSH_CMD[@]}" "$TARGET" "mkdir -p ~/$DEST"
rsync -az --delete -e "${SSH_CMD[*]}" "${EXCLUDES[@]}" "$SRC/" "$TARGET:~/$DEST/"

if [[ $CODE_ONLY -eq 1 ]]; then
    echo "==> サービスを再起動"
    "${SSH_CMD[@]}" "$TARGET" "sudo -n systemctl restart $SERVICE"
    sleep 2
    "${SSH_CMD[@]}" "$TARGET" "systemctl is-active $SERVICE"
else
    echo "==> Pi 側のセットアップを実行"
    "${SSH_CMD[@]}" -t "$TARGET" "chmod +x ~/$DEST/setup-pi.sh && ~/$DEST/setup-pi.sh"
fi
