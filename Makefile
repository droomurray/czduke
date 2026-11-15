# czduke - Duke Nukem 3D for the M5Stack CardputerZero
#
#   make        build ./build/czduke
#   make deb    build dist/czduke_<version>_arm64.deb
#
# Cross-compiling: make CROSS_COMPILE=aarch64-linux-gnu- PKG_CONFIG=aarch64-linux-gnu-pkg-config

VERSION         ?= 0.1.0
JFDUKE3D_REPO   ?= https://github.com/jonof/jfduke3d.git
JFDUKE3D_COMMIT ?= 55c5f9592d2a78e427a2bbdb13ce3c4c9ccf6f04

CROSS_COMPILE ?=
CC            := $(CROSS_COMPILE)gcc
PKG_CONFIG    ?= pkg-config

VENDOR := vendor/jfduke3d
ENGINE := $(VENDOR)/jfbuild
MACT   := $(VENDOR)/jfmact
AUDIO  := $(VENDOR)/jfaudiolib
GAME   := $(VENDOR)/src
BUILD  := build
OUT    := $(BUILD)/czduke

SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS   := $(shell $(PKG_CONFIG) --libs sdl2)

CFLAGS   ?= -O2 -g0
CPPFLAGS += -I$(ENGINE)/include -I$(ENGINE)/src -I$(MACT) -I$(AUDIO)/include -I$(AUDIO)/src -I$(GAME)
# Software renderer only: no Polymost, no OpenGL, no x86 assembly. RENDERTYPESDL
# selects the engine's generic Unix code paths; the SDL layer itself is
# replaced by src/czlayer.c.
CPPFLAGS += -DRENDERTYPESDL=1 -DHAVE_SDL=2 -DHAVE_FLUIDSYNTH
UPSTREAM_CFLAGS := -std=gnu99 -fno-strict-aliasing -w
LDLIBS   += $(SDL_LIBS) -ldl -lm

ENGINE_SRCS := a-c asmprot baselayer cache1d compat crc32 defs engine kplib mmulti \
	osd pragmas scriptfile textfont talltextfont smalltextfont version
MACT_SRCS   := util_lib file_lib control keyboard mouse mathutil scriplib animlib
# driver_sdl and driver_fluidsynth are replaced by src/czaudio.c
AUDIO_SRCS  := drivers fx_man cd multivoc mix mixst pitch vorbis music midi driver_nosound asssys
GAME_SRCS   := game actors gamedef global menues player premap sector rts config \
	osdfuncs osdcmds grpscan sounds version
PORT_SRCS   := czlayer czaudio

OBJS := $(addprefix $(BUILD)/engine/,$(addsuffix .o,$(ENGINE_SRCS))) \
	$(addprefix $(BUILD)/mact/,$(addsuffix .o,$(MACT_SRCS))) \
	$(addprefix $(BUILD)/audio/,$(addsuffix .o,$(AUDIO_SRCS))) \
	$(addprefix $(BUILD)/game/,$(addsuffix .o,$(GAME_SRCS))) \
	$(addprefix $(BUILD)/port/,$(addsuffix .o,$(PORT_SRCS)))

.PHONY: all deb clean distclean
all: $(OUT)

# JFDuke3D (with its jfbuild, jfmact and jfaudiolib submodules) is fetched at
# a pinned commit rather than copied into this repo.
$(VENDOR)/.fetched:
	rm -rf $(VENDOR)
	git init -q $(VENDOR)
	git -C $(VENDOR) remote add origin $(JFDUKE3D_REPO)
	git -C $(VENDOR) fetch -q --depth 1 origin $(JFDUKE3D_COMMIT)
	git -C $(VENDOR) checkout -q FETCH_HEAD
	git -C $(VENDOR) submodule -q update --init --depth 1
	touch $@

$(BUILD)/engine/%.o: $(VENDOR)/.fetched
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(UPSTREAM_CFLAGS) -c $(ENGINE)/src/$*.c -o $@

$(BUILD)/mact/%.o: $(VENDOR)/.fetched
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(UPSTREAM_CFLAGS) -c $(MACT)/$*.c -o $@

$(BUILD)/audio/%.o: $(VENDOR)/.fetched
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(UPSTREAM_CFLAGS) -c $(AUDIO)/src/$*.c -o $@

$(BUILD)/game/%.o: $(VENDOR)/.fetched
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(UPSTREAM_CFLAGS) -c $(GAME)/$*.c -o $@

$(BUILD)/port/%.o: src/%.c $(VENDOR)/.fetched
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(SDL_CFLAGS) $(CFLAGS) -std=gnu11 -fno-strict-aliasing -W -Wall -c $< -o $@

$(OUT): $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@ $(LDLIBS)

deb: $(OUT)
	VERSION=$(VERSION) STRIP=$(CROSS_COMPILE)strip packaging/mkdeb.sh $(OUT) dist

clean:
	rm -rf $(BUILD) dist

distclean: clean
	rm -rf vendor
