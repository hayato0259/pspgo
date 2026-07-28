#!/bin/zsh
# youtube-music を PPSSPP で動かす。ビルド → 配置 → 起動 まで全部やる。
#
#   ./run.sh              通常ビルドで起動
#   ./run.sh --demo       AUTODEMO ビルド (○を自動入力して画面遷移を辿る)
#   ./run.sh --clean      make clean してからビルド
#
# 接続先の切り替え:
#   ./run.sh --local      Mac のローカルサーバーに繋ぐ (サーバー側を直すとき)
#   ./run.sh --remote     元の接続先 (Raspberry Pi など) に戻す
#   ./run.sh --no-server  ローカルサーバーを起動しない (別で動かしている場合)
#
# 接続先は配置先の server.txt が決める。既定ではそれを尊重し、
# 127.0.0.1 を向いているときだけローカルサーバーを起動する。
# ラズパイを向いているのに毎回ローカルサーバーを立てるのは無駄なので、
# 接続先を見て判断する。
set -eu

APP_DIR="${0:A:h}"                      # apps/youtube-music
CLIENT_DIR="$APP_DIR/psp-client/app"
SERVER_DIR="$APP_DIR/server"
DEPLOY_DIR="$HOME/.config/ppsspp/PSP/GAME/YTMUSIC"
SERVER_TXT="$DEPLOY_DIR/server.txt"
REMOTE_BAK="$DEPLOY_DIR/server.txt.remote"   # --local に切り替える前の接続先
PORT=8080

MAKE_ARGS=()
ALLOW_SERVER=1
SWITCH=""
DO_CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --demo)      MAKE_ARGS+=(AUTODEMO=1) ;;
    --no-server) ALLOW_SERVER=0 ;;
    --local)     SWITCH="local" ;;
    --remote)    SWITCH="remote" ;;
    --clean)     DO_CLEAN=1 ;;
    *) echo "不明なオプション: $arg" >&2; exit 1 ;;
  esac
done

export PSPDEV="$HOME/pspdev-install/pspdev"
export PATH="$PSPDEV/bin:$PATH"

mkdir -p "$DEPLOY_DIR"

# --- 接続先の切り替え ---
# server.txt にはトークンが入っていることがあるので、上書きする前に必ず退避する
if [[ "$SWITCH" == "local" ]]; then
  if [[ -f "$SERVER_TXT" ]] && ! grep -q '^127\.0\.0\.1' "$SERVER_TXT"; then
    cp "$SERVER_TXT" "$REMOTE_BAK"
    echo "接続先: 元の設定を $REMOTE_BAK に退避しました"
  fi
  echo "127.0.0.1:$PORT" > "$SERVER_TXT"
elif [[ "$SWITCH" == "remote" ]]; then
  if [[ -f "$REMOTE_BAK" ]]; then
    mv "$REMOTE_BAK" "$SERVER_TXT"
  else
    echo "退避した接続先がありません ($REMOTE_BAK)" >&2
    exit 1
  fi
fi

[[ -f "$SERVER_TXT" ]] || echo "127.0.0.1:$PORT" > "$SERVER_TXT"
TARGET="$(head -n 1 "$SERVER_TXT")"
echo "接続先: $TARGET"

# --- ローカルサーバー (接続先が自分自身のときだけ起動する) ---
IS_LOCAL=0
case "$TARGET" in
  127.0.0.1*|localhost*) IS_LOCAL=1 ;;
esac

if [[ $IS_LOCAL -eq 1 && $ALLOW_SERVER -eq 1 ]]; then
  if curl -s --max-time 3 "http://127.0.0.1:$PORT/api/status" >/dev/null 2>&1; then
    echo "サーバー: 起動済み (ポート $PORT)"
  else
    if [[ ! -x "$SERVER_DIR/.venv/bin/python" ]]; then
      echo "エラー: $SERVER_DIR/.venv がありません。README のクイックスタートを参照" >&2
      exit 1
    fi
    echo "サーバー: 起動します (ログ: $SERVER_DIR/server.log)"
    # -u を付けてログを貯めこませない (障害調査中に空のログを見て混乱するため)
    (cd "$SERVER_DIR" && nohup ./.venv/bin/python -u app.py "$PORT" > server.log 2>&1 &)
    for _ in {1..20}; do
      curl -s --max-time 2 "http://127.0.0.1:$PORT/api/status" >/dev/null 2>&1 && break
      sleep 0.5
    done
  fi
elif [[ $IS_LOCAL -eq 0 ]]; then
  echo "サーバー: 起動しません (接続先が Mac ではないため)"
fi

# --- ビルドと配置 ---
cd "$CLIENT_DIR"
[[ $DO_CLEAN -eq 1 ]] && make clean >/dev/null
make "${MAKE_ARGS[@]}"
cp EBOOT.PBP "$DEPLOY_DIR/"

# --- 起動 (EBOOT は絶対パスで渡すこと。相対だと umd0:/ 扱いで失敗する) ---
pkill -x PPSSPPSDL 2>/dev/null || true
open -a PPSSPPSDL --args "$DEPLOY_DIR/EBOOT.PBP"
echo "PPSSPP を起動しました: $DEPLOY_DIR/EBOOT.PBP"
