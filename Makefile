# WAVE86 - MS-DOS game launcher
# Cross-compiled with Open Watcom V2 (toolchain/) to 16-bit real mode.

WATCOM := $(abspath toolchain)
export WATCOM
export PATH := $(WATCOM)/armo64:$(PATH)
export INCLUDE := $(WATCOM)/h

# -0: 8086 instructions only  -ms: small model  -os: optimize for size
CFLAGS = -q -bcl=dos -0 -ms -os -wx

SRCS = src/wave86.c src/ui.c src/vga.c src/scan.c src/ini.c src/music.c \
       src/mod.c src/cpu.c src/net.c

MUSIC = $(wildcard music/*.IMF music/*.imf music/*.WLF music/*.wlf \
                   music/*.MOD music/*.mod)

all: build/WAVE86.EXE build/WAVE.BAT build/WAVE86.INI music-files thumbs net cdrom

build/WAVE86.EXE: $(SRCS) src/wave86.h
	@mkdir -p build
	cd build && wcl $(CFLAGS) -fe=WAVE86.EXE $(addprefix ../,$(SRCS))
	@rm -f build/*.o
	@ls -la build/WAVE86.EXE

build/WAVE.BAT: WAVE.BAT
	@mkdir -p build
	cp $< $@

build/WAVE86.INI: WAVE86.INI
	@mkdir -p build
	cp $< $@

thumbs: $(wildcard THUMBS/*.THM)
	@mkdir -p build/THUMBS
	@cp THUMBS/*.THM build/THUMBS/ 2>/dev/null || true

music-files: $(MUSIC)
	@mkdir -p build/MUSIC
	@if [ -n "$(MUSIC)" ]; then cp $(MUSIC) build/MUSIC/; fi

# CD image drivers for real DOS (cdrom/README.md)
cdrom: $(wildcard cdrom/*.COM cdrom/*.EXE)
	@mkdir -p build
	@cp cdrom/*.COM cdrom/*.EXE build/

# --- the network side: WAVEGET (ours) and DHCP (mTCP's), both on mTCP, GPL
MTCP_DIR  = $(abspath net/mtcp)
MTCP_OPTS = -0 -ml -oh -ok -ot -s -oa -ei -zp2 -zpw -ob -ol+ -oi+ -q \
            -i=$(MTCP_DIR)/TCPINC -i=$(MTCP_DIR)/INCLUDE
MTCP_OBJS = packet arp eth ip tcp tcpsockm udp utils dns timer trace
MTCP_SRC  = $(wildcard net/mtcp/TCPLIB/*.CPP) net/mtcp/TCPLIB/IPASM.ASM

net: build/WAVEGET.EXE build/DHCP.EXE build/MTCP.CFG

build/MTCP.CFG: net/MTCP.CFG
	@mkdir -p build
	cp $< $@

# $(1) = program (lower case), $(2) = directory holding its .CPP and .CFG
define MTCP_BUILD
	@mkdir -p build/net/$(1)
	cd build/net/$(1) && for o in $(MTCP_OBJS); do wpp $(MTCP_DIR)/TCPLIB/$$o.cpp $(MTCP_OPTS) -DCFG_H=\"$(1).cfg\" -i=$(abspath $(2)) -fo=$$o.obj || exit 1; done
	cd build/net/$(1) && wasm -0 -ml $(MTCP_DIR)/TCPLIB/ipasm.asm -fo=ipasm.obj -q
	cd build/net/$(1) && wpp $(abspath $(2))/$(1).cpp $(MTCP_OPTS) -DCFG_H=\"$(1).cfg\" -i=$(abspath $(2)) -fo=$(1).obj
	cd build/net/$(1) && wlink system dos option quiet option eliminate option stack=8192 name ../../$(1).exe file $$(echo $(MTCP_OBJS) | sed 's/ /.obj,/g').obj,ipasm.obj,$(1).obj
	@mv build/$(1).exe build/$(shell echo $(1) | tr a-z A-Z).EXE 2>/dev/null || true
	@ls -la build/$(shell echo $(1) | tr a-z A-Z).EXE
endef

build/WAVEGET.EXE: net/WAVEGET.CPP net/WAVEGET.CFG $(MTCP_SRC)
	$(call MTCP_BUILD,waveget,net)

build/DHCP.EXE: net/dhcp/DHCP.CPP net/dhcp/DHCP.CFG $(MTCP_SRC)
	$(call MTCP_BUILD,dhcp,net/dhcp)


# regenerate the soundtrack from the composers
music:
	python3 tools/makemusic.py music
	python3 tools/makemod.py music/WAVE86.MOD

# interactive run in dosbox-x (project root is C:, launcher in C:\BUILD)
# with NE2000 networking through slirp: the Mac is 10.0.2.2, so N works
# against a waveserve running here.
DOSBOX_NET = -set "ne2000 ne2000=true" -set "ne2000 backend=slirp" \
             -set "ne2000 nicbase=300" -set "ne2000 nicirq=3"

build/NETUP.BAT: Makefile
	@mkdir -p build
	@printf '@echo off\r\nZ:\\SYSTEM\\NE2000.COM 0x60 3 0x300\r\nset MTCPCFG=C:\\BUILD\\MTCP.CFG\r\nset WAVESRV=10.0.2.2:8086\r\nDHCP\r\n' > $@

run: all build/NETUP.BAT
	dosbox-x -fastlaunch $(DOSBOX_NET) -c "mount c ." -c "c:" -c "cd BUILD" \
	  -c "call NETUP.BAT" -c "WAVE.BAT"

# boot a real DOS (dos/FREEDOS.IMG, or DOS=path to an MS-DOS boot floppy)
# in dosbox-x with the build and GAMES\ on a hard-disk image, run the smoke
# test in tools/dostest.py and show what came back. Needs mtools.
DOS ?= dos/FREEDOS.IMG
dostest: all
	python3 tools/dostest.py --boot $(DOS) $(DOSTEST_ARGS)

# headless self-test: renders the UI, dumps screen+font+palette,
# then the host renders a pixel-perfect PNG
test: all
	@rm -f build/SCREEN.BIN build/FONT.BIN build/PAL.BIN
	SDL_VIDEODRIVER=dummy dosbox-x -nogui -fastlaunch \
	  -c "mount c ." -c "c:" -c "cd BUILD" \
	  -c "WAVE86 /dump" -c "exit" >/dev/null 2>&1
	python3 tools/rendscr.py build/SCREEN.BIN build/FONT.BIN build/PAL.BIN \
	  -o build/screen.png

clean:
	rm -rf build

.PHONY: all run test dostest clean music music-files thumbs net cdrom
