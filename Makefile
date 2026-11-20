PS4_PAYLOAD_SDK ?= /opt/ps4-payload-sdk
ifeq ($(wildcard $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk),)
$(error PS4_PAYLOAD_SDK is not a PS4 payload SDK: '$(PS4_PAYLOAD_SDK)')
endif
include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk

VERSION ?= 1.0.0
PYTHON ?= python3

ELF := ps5-bridge.elf
ICON := $(wildcard web/icon.png)
SRCS := \
	src/main.c \
	src/http_server.c \
	src/ps5.c \
	src/json.c \
	src/config.c \
	src/discover.c \
	src/util.c
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))

CFLAGS := -O2 -g -Wall -Wextra -Wno-unused-parameter \
	-Isrc -Ithird_party \
	-DBRIDGE_VERSION=\"$(VERSION)\"
LDLIBS := -lpthread

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

build/%.o: src/%.c src/webui_assets.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

src/webui_assets.h: web/index.html tools/gen_webui.py $(ICON)
	$(PYTHON) tools/gen_webui.py web/index.html $@ $(ICON)

clean:
	rm -rf build $(ELF) src/webui_assets.h

.PHONY: all clean
