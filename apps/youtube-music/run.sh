#!/bin/zsh
# youtube-music を PPSSPP で動かす。ビルド → 配置 → サーバー起動 → 起動 まで全部やる。
#
#   ./run.sh              通常ビルドで起動
#   ./run.sh --demo       AUTODEMO ビルド (○を自動入力して画面遷移を辿る)
#   ./run.sh --no-server  配信サーバーを起動しない (既に別で動かしている場合)
#   ./run.sh --clean      make clean してからビルド
set -eu

APP_DIR="${0:A:h}"                      # apps/youtube-music
CLIENT_DIR="$APP_DIR/psp-client/app"
SERVER_DIR="$APP_DIR/server"
DEPLOY_DIR="$HOME/.config/ppsspp/PSP/GAME/YTMUSIC"
PORT=8080

MAKE_ARGS=()
START_SERVER=1
DO_CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --demo)      MAKE_ARGS+=(AUTODEMO=1) ;;
    --no-server) START_SERVER=0 ;;
    --clean)     DO_CLEAN=1 ;;
    *) echo "不明なオプション: $arg" >&2; exit 1 ;;
  esac
done

export PSPDEV="$HOME/pspdev-install/pspdev"
export PATH="$PSPDEV/bin:$PATH"

# --- 配信サーバー (無ければ起動する) ---
if [[ $START_SERVER -eq 1 ]]; then
  if curl -s --max-time 3 "http://127.0.0.1:$PORT/api/status" >/dev/null 2>&1; then
    echo "サーバー: 起動済み (ポート $PORT)"
  else
    if [[ ! -x "$SERVER_DIR/.venv/bin/python" ]]; then
      echo "エラー: $SERVER_DIR/.venv がありません。README のクイックスタートを参照" >&2
      exit 1
    fi
    echo "サーバー: 起動します (ログ: $SERVER_DIR/server.log)"
    (cd "$SERVER_DIR" && nohup ./.venv/bin/python app.py "$PORT" > server.log 2>&1 &)
    for _ in {1..20}; do
      curl -s --max-time 2 "http://127.0.0.1:$PORT/api/status" >/dev/null 2>&1 && break
      sleep 0.5
    done
  fi
fi

# --- ビルドと配置 ---
cd "$CLIENT_DIR"
[[ $DO_CLEAN -eq 1 ]] && make clean >/dev/null
make "${MAKE_ARGS[@]}"

mkdir -p "$DEPLOY_DIR"
cp EBOOT.PBP "$DEPLOY_DIR/"
# PPSSPP は 127.0.0.1 に繋ぐ (実機用の server.txt とは別物)
[[ -f "$DEPLOY_DIR/server.txt" ]] || echo "127.0.0.1:$PORT" > "$DEPLOY_DIR/server.txt"

# --- 起動 (EBOOT は絶対パスで渡すこと。相対だと umd0:/ 扱いで失敗する) ---
pkill -x PPSSPPSDL 2>/dev/null || true
open -a PPSSPPSDL --args "$DEPLOY_DIR/EBOOT.PBP"
echo "PPSSPP を起動しました: $DEPLOY_DIR/EBOOT.PBP"
