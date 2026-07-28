#!/usr/bin/env python3
"""外部公開用の共有トークンを auth/token.txt に生成する。"""

import os
import secrets
from pathlib import Path


TOKEN_FILE = Path(__file__).resolve().parent.parent / "auth" / "token.txt"


def main() -> None:
    TOKEN_FILE.parent.mkdir(parents=True, exist_ok=True)
    try:
        fd = os.open(TOKEN_FILE, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        raise SystemExit(
            f"{TOKEN_FILE} は既にあります。更新する場合は削除してから再実行してください"
        )

    token = secrets.token_urlsafe(32)
    with os.fdopen(fd, "w", encoding="utf-8") as fp:
        fp.write(token + "\n")
    print(f"共有トークンを生成しました: {TOKEN_FILE}")


if __name__ == "__main__":
    main()
