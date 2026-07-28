# PoC3: PV (ミュージックビデオ) をアプリ内で再生できるか

映像を PSP 本体でデコードして表示できるか、そして**音声と同時に動かせるか**を
確かめるための単体アプリ。結果は
[docs/verification-video.md](../../docs/verification-video.md) にまとめている。

通信はしない。ローカルに置いた変換済みファイルを読むだけに絞ってある
(通信と混ぜると、失敗したときにどちらが原因か切り分けられなくなるため)。

## 検証用ファイルを作る

`test.pmf` と `test.mp3` は容量が大きいのでリポジトリに含めていない。
手元の動画から作る:

```bash
python3 ../../tools/make_psmf.py 入力動画.mp4 test.pmf --audio test.mp3
```

- `test.pmf` … 映像。H.264 Baseline を PSMF コンテナに入れたもの
- `test.mp3` … 音声。既存の配信と同じ MP3 CBR 128kbps / 44.1kHz / stereo

手元に動画が無ければ、ffmpeg のテスト映像で作れる:

```bash
ffmpeg -f lavfi -i "testsrc2=size=640x360:rate=30:duration=20" -f lavfi -i "sine=frequency=440:duration=20" -c:v libx264 -profile:v baseline -pix_fmt yuv420p -c:a aac -shortest src.mp4
```

## ビルドと実行

```bash
export PSPDEV="$HOME/pspdev-install/pspdev" && export PATH="$PSPDEV/bin:$PATH" && make
```

`EBOOT.PBP` と同じフォルダに `test.pmf` `test.mp3` を置いて起動する。
PPSSPP には**絶対パス**で渡すこと (相対パスは umd0:/ 扱いになって失敗する)。

```bash
open -a PPSSPPSDL --args "$PWD/EBOOT.PBP"
```

起動するとボタンで動作モードを選ぶ。

| ボタン | モード | 何が分かるか |
|---|---|---|
| ○ | 映像のみ | 映像デコードそのものが動くか |
| × | 音声のみ | 音声経路が無事か (比較の基準) |
| △ | 映像 + 音声 | **Media Engine を同時に使えるか** |

3 つを比べると、失敗したときに「映像単体で駄目」なのか
「同時が駄目」なのかを切り分けられる。

PPSSPP で自動確認したいときは、ボタン入力なしで特定モードに入れる:

```bash
make AUTOMODE=2   # 0=映像のみ / 1=音声のみ / 2=両方
```

エミュレータへの入力送信は本体の操作を奪うため使わない方針で、
画面の確認は winshot でウインドウを撮って行う。

## 画面の見かた

再生中は上端に 3 行の数値が出る。終了すると最後のフレームを残したまま
結果を表示する (絵が出ているかどうかも同時に確認できる)。

```
VIDEO + AUDIO  DONE
video frames   601 in  4720 ms  avg 127.3 fps
audio frames   183  stage finished    err 0x00000000
last au 0x80618001  dec 0x00000000
```

- `last au 0x80618001` は「ストリームを読み切った」の意味なので異常ではない
- 準備の途中で失敗した場合は、代わりに呼んだ API と戻り値の一覧が出る

**フレームレートは意図的に待ちを入れずに測っている。** 表示速度ではなく
「デコードがどこまで速く回せるか」の上限を知りたいため。
PPSSPP の数値はホストの CPU の速さなので、実機の目安にはならない。
