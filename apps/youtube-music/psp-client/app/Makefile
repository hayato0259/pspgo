TARGET = ytmusic
OBJS = src/main.o src/gfx.o src/ui.o src/net.o src/api.o src/player.o src/login.o src/tsv.o src/art.o src/snd.o src/store.o src/dl.o src/osk.o src/trackinfo.o src/video.o src/icons.o

# PPSSPP でホスト上のサーバーに繋ぐ場合は 127.0.0.1 のまま。
# 実機では Mac の LAN IP に差し替える: make SERVER_HOST=192.168.x.x
SERVER_HOST ?= 127.0.0.1
SERVER_PORT ?= 8080

INCDIR =
CFLAGS = -O2 -G0 -Wall -DSERVER_HOST=\"$(SERVER_HOST)\" -DSERVER_PORT=$(SERVER_PORT)
ifdef AUTODEMO
CFLAGS += -DAUTODEMO
endif
# AUTODEMO で一括ダウンロード (□) を自動入力しない。
# 回線を占有してストリーミングが失敗するため、再生まわりの確認時に使う
ifdef AUTODEMO_NODL
CFLAGS += -DAUTODEMO_NODL
endif
# 接続後すぐ指定の曲を再生画面で流す (再生画面の見た目を確認する用)
#   make DEMO_TRACK='\"lYBUbBu4W08\"'
ifdef DEMO_TRACK
CFLAGS += -DDEMO_TRACK=$(DEMO_TRACK)
endif
# 開発時の目視確認用: 画面ごとにフレームバッファを raw で書き出す
ifdef UIDEBUG
CFLAGS += -DUIDEBUG
endif
ifdef SHOTDUMP
CFLAGS += -DSHOTDUMP
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspmpeg -lintrafont -lpspgu -lpspmp3 -lpspaudio -lpsputility -lpspnet_inet -lpspnet_resolver -lpspnet_apctl -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = YouTube Music

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
