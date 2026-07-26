TARGET = poc2_stream
OBJS = main.o

# PPSSPP でホスト上のサーバーに繋ぐ場合は 127.0.0.1 のまま。
# 実機では Mac の LAN IP に差し替える:
#   make SERVER_HOST=192.168.x.x
SERVER_HOST ?= 127.0.0.1
SERVER_PORT ?= 8080
# 例: make STREAM_PATH='/stream?yt=<videoId>'
STREAM_PATH ?= /stream?file=testdata/test-tone.mp3

INCDIR =
CFLAGS = -O2 -G0 -Wall -DSERVER_HOST=\"$(SERVER_HOST)\" -DSERVER_PORT=$(SERVER_PORT) -DSTREAM_PATH='"$(STREAM_PATH)"'
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
LIBS = -lpspmp3 -lpspaudio -lpsputility -lpspnet_inet -lpspnet_apctl -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PoC2 HTTP Stream

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
