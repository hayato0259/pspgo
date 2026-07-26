TARGET = ytmusic
OBJS = src/main.o src/net.o src/api.o src/player.o src/login.o src/tsv.o src/art.o

# PPSSPP でホスト上のサーバーに繋ぐ場合は 127.0.0.1 のまま。
# 実機では Mac の LAN IP に差し替える: make SERVER_HOST=192.168.x.x
SERVER_HOST ?= 127.0.0.1
SERVER_PORT ?= 8080

INCDIR =
CFLAGS = -O2 -G0 -Wall -DSERVER_HOST=\"$(SERVER_HOST)\" -DSERVER_PORT=$(SERVER_PORT)
ifdef AUTODEMO
CFLAGS += -DAUTODEMO
endif
# 開発時の目視確認用: 画面ごとにフレームバッファを raw で書き出す
ifdef SHOTDUMP
CFLAGS += -DSHOTDUMP
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lintrafont -lpspgu -lpspmp3 -lpspaudio -lpsputility -lpspnet_inet -lpspnet_apctl -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = YouTube Music

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
