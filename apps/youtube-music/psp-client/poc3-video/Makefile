TARGET = poc3_video
OBJS = main.o

INCDIR =
CFLAGS = -O2 -G0 -Wall
# PPSSPP での自動確認用。モード選択を飛ばして指定モードで起動する
#   make AUTOMODE=0  映像のみ / 1 音声のみ / 2 両方
ifdef AUTOMODE
CFLAGS += -DAUTOMODE=$(AUTOMODE)
endif
CXXFLAGS = $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS = $(CFLAGS)

LIBDIR =
LDFLAGS =
# 映像は sceMpeg、色変換に sceMpegbase、音声は既存経路と同じ sceMp3
LIBS = -lpspmpeg -lpspmpegbase -lpspmp3 -lpspaudio -lpsputility -lm

EXTRA_TARGETS = EBOOT.PBP
PSP_EBOOT_TITLE = PoC3 Video

PSPSDK = $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak
