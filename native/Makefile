# 原生客户端（SDL2 + FFmpeg + OpenGL）
#
# 依赖（Debian/Ubuntu）：libsdl2-dev libavcodec-dev libavutil-dev libswscale-dev libgl-dev
PKGS := sdl2 libavcodec libavutil libswscale
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra $(shell pkg-config --cflags $(PKGS))
LDLIBS := $(shell pkg-config --libs $(PKGS)) -lGL -lm

all: wwclient wwsrc

wwclient: wwclient.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

# 测试源只用标准库（Annex-B 切分自己实现），不需要额外依赖
wwsrc: wwsrc.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f wwclient wwsrc

.PHONY: all clean
