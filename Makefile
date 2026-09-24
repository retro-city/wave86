# WAVE86 - MS-DOS game launcher
# Cross-compiled with Open Watcom V2 (toolchain/) to 16-bit real mode.

WATCOM := $(abspath toolchain)
export WATCOM
# the host tools for this machine: armo64 (Apple silicon), bino64 (Intel Mac), binl64 (Linux)
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_S),Darwin)
  WATCOM_BIN ?= $(if $(filter arm64,$(UNAME_M)),armo64,bino64)
else
  WATCOM_BIN ?= binl64
endif
export PATH := $(WATCOM)/$(WATCOM_BIN):$(PATH)
export INCLUDE := $(WATCOM)/h
VERSION := $(shell sed -n 's/.*VERSION_STR *"\([^"]*\)".*/\1/p' src/wave86.h)

# -0: 8086 instructions only  -ms: small model  -os: optimize for size
CFLAGS = -q -bcl=dos -0 -ms -os -wx

SRCS = src/wave86.c src/ui.c src/vga.c src/scan.c src/ini.c src/music.c \
       src/mod.c src/cpu.c src/net.c src/theme.c

MUSIC = $(wildcard music/*.IMF music/*.imf music/*.WLF music/*.wlf \
                   music/*.MOD music/*.mod)

all: build/WAVE86.EXE build/WAVE.BAT build/WAVE86.INI build/WAVE86.DEF build/MEMLIM.EXE music-files thumbs net cdrom

# the shipped INI under another name: WAVEGET UPDATE sends it along and
# merges the settings a machine's INI lacks into it, comments and all
build/WAVE86.DEF: WAVE86.INI
	@mkdir -p build
	cp $< $@

# MEMLIM hides extended memory from a game that cannot cope with all of it
build/MEMLIM.EXE: src/memlim.c
	@mkdir -p build
	cd build && wcl $(CFLAGS) -fe=MEMLIM.EXE ../src/memlim.c
	@rm -f build/*.o

# SLOWDOWN (Bret Johnson, freeware: the COM and its DOC travel together,
# unmodified) slows the machine for a game that runs too fast; the launcher
# runs it for a game whose section says slowdown=. Fetched from the FreeDOS
# package rather than kept here - the author's terms ask for a word before
# it is bundled with another program.
SLOWDOWN_RAW ?= https://gitlab.com/FreeDOS/util/slowdown/-/raw/master
slowdown: build/SLOWDOWN.COM
build/SLOWDOWN.COM:
	@mkdir -p build
	curl -sSfL -o build/SLOWDOWN.COM.part $(SLOWDOWN_RAW)/BIN/SLOWDOWN.COM
	curl -sSfL -o build/SLOWDOWN.DOC.part $(SLOWDOWN_RAW)/DOC/SLOWDOWN/SLOWDOWN.DOC
	mv build/SLOWDOWN.DOC.part build/SLOWDOWN.DOC && mv build/SLOWDOWN.COM.part build/SLOWDOWN.COM
	@ls -la build/SLOWDOWN.COM build/SLOWDOWN.DOC

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

thumbs: $(wildcard THUMBS/*.THM THUMBS/*/*.THM)
	@mkdir -p build/THUMBS
	@cp -R THUMBS/. build/THUMBS/ 2>/dev/null || true

music-files: $(MUSIC)
	@mkdir -p build/MUSIC
	@if [ -n "$(MUSIC)" ]; then cp $(MUSIC) build/MUSIC/; fi

# the mTCP NetDrive server for this machine (Go, GPL; Michael Brutman's
# official build) into build/netdrive, so waveserve can keep CD images
# on this side of the wire. Only needed for waveserve --netdrive.
ND_VER = 2025-01-10
# (pattern) with both parentheses: a bare pattern) would close the $(shell
ND_BIN = $(shell case "$$(uname -s)-$$(uname -m)" in (Darwin-arm64) echo netdrive_darwin_arm64;; (Linux-x86_64) echo netdrive_linux_amd64;; (Linux-aarch64) echo netdrive_linux_arm64;; (*) echo unknown;; esac)
netdrive: build/netdrive
build/netdrive:
	@mkdir -p build
	curl -sL -o build/netdrive-server.zip https://www.brutman.com/mTCP/download/mTCP_NetDrive_server-bin_$(ND_VER).zip
	cd build && unzip -q -o -j netdrive-server.zip "mTCP_NetDrive_server-bin_$(ND_VER)/$(ND_BIN)" "mTCP_NetDrive_server-bin_$(ND_VER)/copying.txt" && mv $(ND_BIN) netdrive && chmod +x netdrive && rm netdrive-server.zip
	@xattr -d com.apple.quarantine build/netdrive 2>/dev/null || true
	@./build/netdrive 2>&1 | head -1

# The server, on this machine: make waveserve starts it with its console
# (q stops it). Its settings - the collection, the port, the torrent, where
# things are kept - are in waveserve.ini, next to this file; a variable on
# the command line overrides the file for one run. It serves build/ to
# WAVEGET UPDATE, so it depends on all: a fresh build is what goes out.
# SLOWDOWN is fetched on the way so the update can carry it, but the
# server does not need it, so a failed download is only reported.
#   make waveserve
#   make waveserve EXODOS=/Volumes/Games/eXoDOS PORT=8086
#   make waveserve TORRENT=eXoDOS.torrent WAVESERVE_ARGS="--log serve.log"
WAVESERVE_ARGS ?=
waveserve: all build/netdrive
	@$(MAKE) -s build/SLOWDOWN.COM || echo "  (no SLOWDOWN.COM: the update will not carry it)"
	python3 tools/waveserve.py $(if $(EXODOS),$(EXODOS)) $(if $(TDC),--tdc $(TDC)) $(if $(TORRENT),--torrent $(TORRENT)) $(if $(CACHE),--cache $(CACHE)) $(if $(PORT),--port $(PORT)) $(if $(MAX_MB),--max-mb $(MAX_MB)) $(if $(NETDRIVE_DIR),--netdrive $(NETDRIVE_DIR)) $(if $(THUMBS_DIR),--thumbs $(THUMBS_DIR)) $(WAVESERVE_ARGS)

# CD image drivers for real DOS (cdrom/README.md)
cdrom: $(wildcard cdrom/*.COM cdrom/*.EXE)
	@mkdir -p build
	@cp cdrom/*.COM cdrom/*.EXE build/

# --- the network side: WAVEGET (ours) and DHCP (mTCP's), both on mTCP, GPL
MTCP_DIR  = $(abspath net/mtcp)
MTCP_OPTS = -0 -ml -oh -ok -ot -s -oa -ei -zp2 -zpw -ob -ol+ -oi+ -q \
            -i=$(abspath build/mtcp-inc) -i=$(MTCP_DIR)/TCPINC -i=$(MTCP_DIR)/INCLUDE
MTCP_OBJS = PACKET ARP ETH IP TCP TCPSOCKM UDP UTILS DNS TIMER TRACE
MTCP_SRC  = $(wildcard net/mtcp/TCPLIB/*.CPP) net/mtcp/TCPLIB/IPASM.ASM

net: build/WAVEGET.EXE build/DHCP.EXE build/MTCP.CFG build/NE2000.COM build/NETDRIVE.SYS build/NETDRIVE.EXE build/DRVOFF.EXE

# DRVOFF X: frees a drive letter, so the CD can have D: beside NetDrive
build/DRVOFF.EXE: net/DRVOFF.C
	@mkdir -p build
	cd build && wcl $(CFLAGS) -fe=DRVOFF.EXE ../net/DRVOFF.C
	@rm -f build/*.o

# mTCP NetDrive (GPL): a remote disk image as a drive letter, for CD images kept on the server
build/NETDRIVE.SYS: net/NETDRIVE.SYS
	@mkdir -p build
	cp $< $@

build/NETDRIVE.EXE: net/NETDRIVE.EXE
	@mkdir -p build
	cp $< $@

# the Crynwr NE2000 packet driver (GPL), for the real card and make dosrun
build/NE2000.COM: net/NE2000.COM
	@mkdir -p build
	cp $< $@

build/MTCP.CFG: net/MTCP.CFG
	@mkdir -p build
	cp $< $@

# $(1) = program (lower case), $(2) = directory holding its .CPP and .CFG,
# $(3) = extra objects already built in build/net/$(1), comma separated.
# The sources carry upper-case names, which matters on Linux.
define MTCP_BUILD
	@mkdir -p build/net/$(1)
	cd build/net/$(1) && for o in $(MTCP_OBJS); do wpp $(MTCP_DIR)/TCPLIB/$$o.CPP $(MTCP_OPTS) -DCFG_H=\"$(shell echo $(1) | tr a-z A-Z).CFG\" -i=$(abspath $(2)) -fo=$$o.obj || exit 1; done
	cd build/net/$(1) && wasm -0 -ml $(MTCP_DIR)/TCPLIB/IPASM.ASM -fo=ipasm.obj -q
	cd build/net/$(1) && wpp $(abspath $(2))/$(shell echo $(1) | tr a-z A-Z).CPP $(MTCP_OPTS) -DCFG_H=\"$(shell echo $(1) | tr a-z A-Z).CFG\" -i=$(abspath $(2)) -fo=$(1).obj
	cd build/net/$(1) && wlink system dos option quiet option eliminate option stack=8192 name ../../$(1).exe file $$(echo $(MTCP_OBJS) | sed 's/ /.obj,/g').obj,ipasm.obj,$(1).obj$(if $(3),$(COMMA)$(3))
	@mv build/$(1).exe build/$(shell echo $(1) | tr a-z A-Z).EXE 2>/dev/null || true
	@ls -la build/$(shell echo $(1) | tr a-z A-Z).EXE
endef

# the headers under the names the sources include them by (case matters on Linux)
build/mtcp-inc/.stamp: $(wildcard net/mtcp/*/*.H net/mtcp/*/*.h net/*.H net/dhcp/*.H)
	python3 tools/mtcp_inc.py build/mtcp-inc

# WAVEGET plays the soundtrack while a game comes in, so the launcher's
# music engine and CPU probe are built again in large model and linked in.
COMMA := ,
WAVEGET_EXTRA := music.obj,mod.obj,cpu.obj

build/WAVEGET.EXE: net/WAVEGET.CPP net/WAVEGET.CFG $(MTCP_SRC) build/mtcp-inc/.stamp \
                   src/music.c src/mod.c src/cpu.c src/wave86.h
	@mkdir -p build/net/waveget
	cd build/net/waveget && for f in music mod cpu; do wcc -0 -ml -os -wx -q $(abspath src)/$$f.c -fo=$$f.obj || exit 1; done
	$(call MTCP_BUILD,waveget,net,$(WAVEGET_EXTRA))

build/DHCP.EXE: net/dhcp/DHCP.CPP net/dhcp/DHCP.CFG $(MTCP_SRC) build/mtcp-inc/.stamp
	$(call MTCP_BUILD,dhcp,net/dhcp)


# the DOS side, zipped: everything that goes next to the launcher on the
# DOS machine, only our own tunes, the FreeDOS extras, the licences
dist: all
	rm -rf dist/wave86 && mkdir -p dist/wave86/MUSIC dist/wave86/THUMBS dist/wave86/EXTRAS
	cp build/WAVE86.EXE build/WAVE.BAT WAVE86.INI build/WAVE86.DEF dist/wave86/
	cp $$(git ls-files music) dist/wave86/MUSIC/
	cp -R THUMBS/. dist/wave86/THUMBS/
	cp build/WAVEGET.EXE build/DHCP.EXE build/MTCP.CFG build/NE2000.COM build/NETDRIVE.SYS build/NETDRIVE.EXE build/DRVOFF.EXE dist/wave86/
	cp build/SHCDX86.COM build/SHCDHD86.EXE build/MEMLIM.EXE dist/wave86/
	@if [ -f build/SLOWDOWN.COM ] && [ -f build/SLOWDOWN.DOC ]; then cp build/SLOWDOWN.COM build/SLOWDOWN.DOC dist/wave86/; else echo "  (no build/SLOWDOWN.COM and .DOC: make slowdown fetches them)"; fi
	cp dos/CHOICE.EXE dos/CTMOUSE.EXE dos/JEMMEX.EXE dist/wave86/EXTRAS/
	sed 's/$$/\r/' docs/README-DOS.txt > dist/wave86/README.TXT
	sed 's/$$/\r/' LICENSE > dist/wave86/LICENSE.TXT
	sed 's/$$/\r/' THIRD-PARTY.md > dist/wave86/THIRDPTY.TXT
	sed 's/$$/\r/' cdrom/LICENSE.txt > dist/wave86/SHSUCD.TXT
	cd dist && rm -f wave86-$(VERSION)-dos.zip && zip -q -r wave86-$(VERSION)-dos.zip wave86
	@ls -la dist/wave86-$(VERSION)-dos.zip

# A ready-made disk for a PicoMem 2 (tools/mkimage.py): FreeDOS, the
# launcher in C:\WAVE86, the card's DOS tools in C:\PICOMEM, mTCP in
# C:\MTCP, the Gravis UltraSound files in C:\ULTRASND, booting into WAVE86
# with the SD card on W:. The pieces come down once, like build/netdrive:
# the PicoMEM repository has no release zips, its DOS tools and its copy
# of the Gravis zip are plain files on a branch (PICOMEM_REF), fetched one
# by one; mTCP's client zip is Michael Brutman's; CDMKE.SYS, the
# Panasonic/MKE CD-ROM driver the card's wiki names, is the PicoGUS
# project's copy. An empty URL (CDMKE_URL=, GUS_URL=) leaves that piece
# out; a folder of your own (PICOMEM_DIR=, MTCP_TOOLS=, CDMKE_DIR=,
# GUS_DIR=) is used instead of a download. The image boots in dosbox-x
# before it is declared done - twice, LBA and then CHS, with the FreeDOS
# boot sector (KERNEL=edrdos shares it), once with a DOS_DISKS image, whose
# own boot sector the CHS patch does not know; the work files stay in
# build/ whatever IMAGE says, so the image can go straight to the SD card.
#   make picomem-image
#   make picomem-image IMAGE=/Volumes/SD/HDD/PMWAVE.IMG IMAGE_MB=500
#   make picomem-image PICOMEM_DIR=$$HOME/picomem-drivers GUS_DIR=$$HOME/ultrasnd CDMKE_URL=
IMAGE ?=
# KERNEL=edrdos boots the EDR-DOS (SvarDOS) kernel under the FreeDOS boot
# sector, fetched from its GitHub release (a grey-area licence: fine for this
# repo, not for anything commercial - THIRD-PARTY.md); DOS_DISKS="disk1.img
# disk2.img" builds with your own licensed DOS instead, its own SYS and its
# own tools (the disks are only read; the list is split on spaces, so no
# path in it can have one). The image is named after what it runs:
# build/pmwave-fdos.img, -edrdos, -msdos, -pcdos; IMAGE= overrides that.
KERNEL ?= freedos
EDR_URL ?= https://github.com/SvarDOS/edrdos/releases/download/v20260309/edrdos_20260309.zip
EDR_DIR ?= $(if $(EDR_URL),build/edrdos)
DOS_DISKS ?=
IMAGE_MB ?= 512
PICOMEM_REF ?= main
PICOMEM_URL = https://raw.githubusercontent.com/FreddyVRetro/ISA-PicoMEM/$(PICOMEM_REF)/drivers
PICOMEM_FILES = PMINIT.EXE PMDFS.EXE PMDFS3.EXE PMMOUSE.EXE PMEMM.EXE PM2000.COM NE2000.COM ASTCLOCK.COM PICOMEM.EXE README.md
PICOMEM_DIR ?= build/picomem
MTCP_VER ?= 2025-01-10
MTCP_TOOLS ?= build/mtcp
CDMKE_URL ?= https://picogus.com/drivers/cdmke.zip
CDMKE_DIR ?= $(if $(CDMKE_URL),build/cdmke)
GUS_URL ?= $(PICOMEM_URL)/ultrasnd.zip
GUS_DIR ?= $(if $(GUS_URL),build/ultrasnd)
MKIMAGE_ARGS ?=
# FreeDOS utilities for C:\DOS: EDIT and a few friends, and LBACACHE, a disk
# cache AUTOEXEC loads (GPL; fetched from the FreeDOS 1.3 package repository)
FREEDOS_REPO ?= https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base
DOS_PKGS ?= edit lbacache mem more xcopy deltree attrib find tree label
DOS_DIR ?= $(if $(DOS_PKGS),build/dosutils)
# 4DOS 8.00 as the shell (JP Software's 2004 notice licence: distribute with
# LICENSE.TXT; not OSI-approved); FOURDOS_URL= empty keeps FreeCOM
FOURDOS_URL ?= https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/util/user/4dos/4dos800.zip
FOURDOS_DIR ?= $(if $(FOURDOS_URL),build/4dos)
FOURDOS_FILES = 4DOS.COM 4DOS.HLP 4HELP.EXE KSTACK.COM OPTION.EXE HELPCFG.EXE BATCOMP.EXE LICENSE.TXT README.TXT INTRO.TXT
PICOMEM_INPUTS = $(PICOMEM_DIR) $(MTCP_TOOLS) $(CDMKE_DIR) $(GUS_DIR) $(DOS_DIR) $(FOURDOS_DIR) $(if $(filter edrdos,$(KERNEL)),$(EDR_DIR))
# the build/ defaults are fetched by the rules below; anything else must be there already
PICOMEM_FETCH = $(filter build/picomem build/mtcp build/cdmke build/ultrasnd build/dosutils build/4dos build/edrdos,$(PICOMEM_INPUTS))
picomem-image: picomem-folders dist $(PICOMEM_FETCH)
	@$(MAKE) -s build/SLOWDOWN.COM && cp build/SLOWDOWN.COM build/SLOWDOWN.DOC dist/wave86/ || echo "  (no SLOWDOWN.COM: the image goes without it)"
	python3 tools/mkimage.py --dist dist/wave86 --picomem "$(PICOMEM_DIR)" --mtcp "$(MTCP_TOOLS)" \
	  $(if $(CDMKE_DIR),--cdmke "$(CDMKE_DIR)") $(if $(GUS_DIR),--gus "$(GUS_DIR)") $(if $(DOS_DIR),--dos "$(DOS_DIR)") $(if $(FOURDOS_DIR),--4dos "$(FOURDOS_DIR)") \
	  --kernel $(KERNEL) $(if $(filter edrdos,$(KERNEL)),--edr "$(EDR_DIR)") $(foreach d,$(DOS_DISKS),--dos-disk $(d)) \
	  --size $(IMAGE_MB) $(if $(IMAGE),-o "$(IMAGE)") $(MKIMAGE_ARGS)

# a folder given by hand that is not there fails here, before the dist is built
picomem-folders:
	@for d in $(filter-out $(PICOMEM_FETCH),$(PICOMEM_INPUTS)); do \
	  test -d "$$d" || { echo "picomem-image: no folder $$d (PICOMEM_DIR, MTCP_TOOLS, CDMKE_DIR, GUS_DIR, DOS_DIR or FOURDOS_DIR)"; exit 1; }; done

build/edrdos:
	@rm -rf build/edrdos.part && mkdir -p build/edrdos.part
	curl -sSfL -o build/edrdos.part/edrdos.zip $(EDR_URL)
	cd build/edrdos.part && unzip -q -o edrdos.zip && rm edrdos.zip
	@mv build/edrdos.part build/edrdos && ls build/edrdos | tr '\n' ' ' && echo

build/4dos:
	@rm -rf build/4dos.part && mkdir -p build/4dos.part
	curl -sSfL -o build/4dos.part/4dos.zip $(FOURDOS_URL)
	cd build/4dos.part && unzip -q -o 4dos.zip $(FOURDOS_FILES) && rm 4dos.zip
	@mv build/4dos.part build/4dos && ls build/4dos | tr '\n' ' ' && echo

build/dosutils:
	@rm -rf build/dosutils.part && mkdir -p build/dosutils.part
	for p in $(DOS_PKGS); do curl -sSfL -o build/dosutils.part/$$p.zip $(FREEDOS_REPO)/$$p.zip || { echo "no $(FREEDOS_REPO)/$$p.zip"; exit 1; }; \
	  unzip -q -j -o build/dosutils.part/$$p.zip 'BIN/*' -d build/dosutils.part || { echo "no BIN/ in $$p.zip"; exit 1; }; rm build/dosutils.part/$$p.zip; done
	@mv build/dosutils.part build/dosutils && ls build/dosutils | tr '\n' ' ' && echo

build/picomem:
	@rm -rf build/picomem.part && mkdir -p build/picomem.part
	for f in $(PICOMEM_FILES); do curl -sSfL -o build/picomem.part/$$f $(PICOMEM_URL)/$$f || { echo "no $(PICOMEM_URL)/$$f"; exit 1; }; done
	mv build/picomem.part build/picomem
	@ls build/picomem

build/mtcp:
	@mkdir -p build
	curl -sSfL -o build/mtcp.zip https://www.brutman.com/mTCP/download/mTCP_$(MTCP_VER).zip
	rm -rf build/mtcp && unzip -q -o -d build/mtcp build/mtcp.zip && rm build/mtcp.zip
	@ls build/mtcp

build/cdmke:
	@mkdir -p build
	curl -sSfL -o build/cdmke.zip $(CDMKE_URL)
	rm -rf build/cdmke && unzip -q -o -d build/cdmke build/cdmke.zip && rm build/cdmke.zip
	@ls build/cdmke

# the Gravis UltraSound software (ultrasnd.zip, flat: ULTRASND.INI, ULTRINIT,
# ULTRAMID, the MIDI\ patches...), the copy the PicoMEM wiki points at
build/ultrasnd:
	@mkdir -p build
	curl -sSfL -o build/ultrasnd.zip $(GUS_URL)
	rm -rf build/ultrasnd && unzip -q -o -d build/ultrasnd build/ultrasnd.zip && rm build/ultrasnd.zip
	@ls build/ultrasnd | wc -l | sed 's/^ *//;s/$$/ entries in build\/ultrasnd/'

# the FreeDOS FAT16 boot sector that image gets (dos/FAT16.BS), assembled
# from the kernel's own boot/boot.asm; only needed when dos/boot.asm changes
bootsector:
	cd dos && nasm -dISFAT16 -f bin boot.asm -o FAT16.BS
	@ls -la dos/FAT16.BS

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

# CYCLES is the machine's speed: 50000 is a fast 486, where dosbox-x's own
# default of 3000 is an XT (make run CYCLES=3000 to see the launcher on one)
CYCLES ?= 50000
run: all build/NETUP.BAT
	dosbox-x -fastlaunch $(DOSBOX_NET) -set "cpu cycles=$(CYCLES)" -c "mount c ." -c "c:" -c "cd BUILD" \
	  -c "call NETUP.BAT" -c "WAVE.BAT"

# boot a real DOS (dos/FREEDOS.IMG, or DOS=path to an MS-DOS boot floppy)
# in dosbox-x with the build and GAMES\ on a hard-disk image, run the smoke
# test in tools/dostest.py and show what came back. Needs mtools.
DOS ?= dos/FREEDOS.IMG
dostest: all
	python3 tools/dostest.py --boot $(DOS) $(DOSTEST_ARGS)

# the same real DOS, but on screen and booting into the launcher, with
# sound, the NE2000 (slirp) and DHCP, so N works against a waveserve here
dosrun: all
	python3 tools/dostest.py --boot $(DOS) --run $(DOSTEST_ARGS)

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

.PHONY: all run test dostest dosrun netdrive waveserve dist picomem-image picomem-folders bootsector clean music music-files thumbs net cdrom
