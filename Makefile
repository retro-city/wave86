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
# other people's programs the build needs, kept in the repository (THIRD-PARTY.md)
TP = third-party

# -0: 8086 instructions only  -mm: medium model (far code, so the launcher can pass 64 KB of it; near data)  -os: optimize for size
# -s: no stack-overflow probes; -k4096: twice the default stack in their
# place, so the margin is real rather than checked. The small tools
# (MEMLIM, DRVOFF) stay in the small model they were tested in.
CFLAGS = -q -bcl=dos -0 -mm -os -wx -s -k4096
TOOL_CFLAGS = -q -bcl=dos -0 -ms -os -wx -s -k4096

SRCS = src/wave86.c src/ui.c src/vga.c src/scan.c src/ini.c src/music.c \
       src/mod.c src/midi.c src/cpu.c src/net.c src/theme.c

MUSIC = $(wildcard music/*.IMF music/*.imf music/*.WLF music/*.wlf \
                   music/*.MOD music/*.mod music/*.MID music/*.mid)

all: build/WAVE86.EXE build/WAVE.BAT build/WAVE86.INI build/WAVE86.DEF build/MEMLIM.EXE music-files thumbs net cdrom

# the shipped INI under another name: WAVEGET UPDATE sends it along and
# merges the settings a machine's INI lacks into it, comments and all
build/WAVE86.DEF: WAVE86.INI
	@mkdir -p build
	cp $< $@

# MEMLIM hides extended memory from a game that cannot cope with all of it
build/MEMLIM.EXE: src/memlim.c Makefile
	@mkdir -p build
	cd build && wcl $(TOOL_CFLAGS) -fe=MEMLIM.EXE ../src/memlim.c
	@rm -f build/*.o

# SLOWDOWN (Bret Johnson, freeware: the COM and its DOC travel together,
# unmodified) slows the machine for a game that runs too fast; the launcher
# runs it for a game whose section says slowdown=. Kept in third-party/
# from the FreeDOS package; make update-slowdown refreshes it.
SLOWDOWN_RAW ?= https://gitlab.com/FreeDOS/util/slowdown/-/raw/master
slowdown: build/SLOWDOWN.COM
build/SLOWDOWN.COM: $(TP)/slowdown/SLOWDOWN.COM $(TP)/slowdown/SLOWDOWN.DOC
	@mkdir -p build
	cp $(TP)/slowdown/SLOWDOWN.COM $(TP)/slowdown/SLOWDOWN.DOC build/

build/WAVE86.EXE: $(SRCS) src/wave86.h Makefile
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
# official build, every platform in one zip under third-party/netdrive,
# make update-netdrive refreshes it) into build/netdrive, so waveserve can
# keep CD images on this side of the wire. Only needed for waveserve --netdrive.
ND_VER = 2025-01-10
ND_URL = https://www.brutman.com/mTCP/download/mTCP_NetDrive_server-bin_$(ND_VER).zip
ND_ZIP = $(TP)/netdrive/mTCP_NetDrive_server-bin_$(ND_VER).zip
# (pattern) with both parentheses: a bare pattern) would close the $(shell
ND_BIN = $(shell case "$$(uname -s)-$$(uname -m)" in (Darwin-arm64) echo netdrive_darwin_arm64;; (Linux-x86_64) echo netdrive_linux_amd64;; (Linux-aarch64) echo netdrive_linux_arm64;; (*) echo unknown;; esac)
netdrive: build/netdrive
build/netdrive: $(ND_ZIP)
	@mkdir -p build
	cd build && unzip -q -o -j $(abspath $(ND_ZIP)) "mTCP_NetDrive_server-bin_$(ND_VER)/$(ND_BIN)" "mTCP_NetDrive_server-bin_$(ND_VER)/copying.txt" && mv $(ND_BIN) netdrive && chmod +x netdrive
	@xattr -d com.apple.quarantine build/netdrive 2>/dev/null || true
	@./build/netdrive 2>&1 | head -1

# The server, on this machine: make waveserve starts it with its console
# (q stops it). Its settings - the collection, the port, the torrent, where
# things are kept - are in waveserve.ini, next to this file: yours, out of
# git, made from waveserve.def (the defaults, which the server zip ships as
# its waveserve.ini) the first time; a variable on the command line
# overrides the file for one run. It serves build/ to
# WAVEGET UPDATE, so it depends on all: a fresh build is what goes out.
# SLOWDOWN comes along (build/SLOWDOWN.COM) so the update can carry it.
#   make waveserve
#   make waveserve EXODOS=/Volumes/Games/eXoDOS PORT=8086
#   make waveserve TORRENT=eXoDOS.torrent WAVESERVE_ARGS="--log serve.log"
WAVESERVE_ARGS ?=
# the server's Python: .venv's when there is one (pip install -r requirements.txt)
PYTHON ?= $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)
waveserve: all build/netdrive build/SLOWDOWN.COM
	$(PYTHON) tools/waveserve.py $(if $(EXODOS),$(EXODOS)) $(if $(TDC),--tdc $(TDC)) $(if $(TORRENT),--torrent $(TORRENT)) $(if $(CACHE),--cache $(CACHE)) $(if $(PORT),--port $(PORT)) $(if $(MAX_MB),--max-mb $(MAX_MB)) $(if $(NETDRIVE_DIR),--netdrive $(NETDRIVE_DIR)) $(if $(THUMBS_DIR),--thumbs $(THUMBS_DIR)) $(WAVESERVE_ARGS)

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
build/DRVOFF.EXE: net/DRVOFF.C Makefile
	@mkdir -p build
	cd build && wcl $(TOOL_CFLAGS) -fe=DRVOFF.EXE ../net/DRVOFF.C
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
WAVEGET_EXTRA := music.obj,mod.obj,midi.obj,cpu.obj

build/WAVEGET.EXE: net/WAVEGET.CPP net/WAVEGET.CFG $(MTCP_SRC) build/mtcp-inc/.stamp \
                   src/music.c src/mod.c src/midi.c src/cpu.c src/wave86.h
	@mkdir -p build/net/waveget
	cd build/net/waveget && for f in music mod midi cpu; do wcc -0 -ml -os -wx -q $(abspath src)/$$f.c -fo=$$f.obj || exit 1; done
	$(call MTCP_BUILD,waveget,net,$(WAVEGET_EXTRA))

build/DHCP.EXE: net/dhcp/DHCP.CPP net/dhcp/DHCP.CFG $(MTCP_SRC) build/mtcp-inc/.stamp
	$(call MTCP_BUILD,dhcp,net/dhcp)


# the DOS side, zipped: everything that goes next to the launcher on the
# DOS machine, only our own tunes, the FreeDOS extras, the licences.
# SLOWDOWN comes along (U carries it to the DOS machine); SLOWDOWN_IN_DIST=0
# leaves it out of the zip. Its terms are in SLOWDOWN.DOC and THIRD-PARTY.md.
SLOWDOWN_IN_DIST ?= 1
dist: all $(if $(filter 1,$(SLOWDOWN_IN_DIST)),build/SLOWDOWN.COM)
	rm -rf dist/wave86 && mkdir -p dist/wave86/MUSIC dist/wave86/THUMBS dist/wave86/EXTRAS
	cp build/WAVE86.EXE build/WAVE.BAT WAVE86.INI build/WAVE86.DEF dist/wave86/
	cp $$(git ls-files music) dist/wave86/MUSIC/
	cp -R THUMBS/. dist/wave86/THUMBS/
	cp build/WAVEGET.EXE build/DHCP.EXE build/MTCP.CFG build/NE2000.COM build/NETDRIVE.SYS build/NETDRIVE.EXE build/DRVOFF.EXE dist/wave86/
	cp build/SHCDX86.COM build/SHCDHD86.EXE build/MEMLIM.EXE dist/wave86/
	$(if $(filter 1,$(SLOWDOWN_IN_DIST)),cp build/SLOWDOWN.COM build/SLOWDOWN.DOC dist/wave86/,@echo "  (SLOWDOWN_IN_DIST=0: the zip goes without SLOWDOWN)")
	cp dos/CHOICE.EXE dos/CTMOUSE.EXE dos/JEMMEX.EXE dist/wave86/EXTRAS/
	sed 's/$$/\r/' docs/README-DOS.txt > dist/wave86/README.TXT
	sed 's/$$/\r/' LICENSE > dist/wave86/LICENSE.TXT
	sed 's/$$/\r/' THIRD-PARTY.md > dist/wave86/THIRDPTY.TXT
	sed 's/$$/\r/' cdrom/LICENSE.txt > dist/wave86/SHSUCD.TXT
	cd dist && rm -f wave86-$(RELEASE_VER)-dos.zip && zip -q -r wave86-$(RELEASE_VER)-dos.zip wave86
	@ls -la dist/wave86-$(RELEASE_VER)-dos.zip

# The release packages beside the DOS zip, named after RELEASE_VER (the
# tag without its v in CI; the launcher's own version otherwise):
#   make server-release    wave86-<ver>-server.zip: waveserve and what it
#                          uses, the DOS programs it hands to WAVEGET UPDATE,
#                          NetDrive's server for every platform, the eXoDOS
#                          torrent, requirements.txt, waveserve.def as its
#                          waveserve.ini,
#                          run-server.sh and .bat (tools/release/) and
#                          docs/README-SERVER.txt
#   make picomem-release   wave86-<ver>-picomem-hdd.zip: the EDR-DOS PicoMEM
#                          image (make picomem-image) and docs/README-PICOMEM.txt
RELEASE_VER ?= $(VERSION)
SERVER_PKG = dist/wave86-$(RELEASE_VER)-server
SERVER_TOOLS = waveserve.py waveconsole.py makethumb.py torrentfs.py fat16.py
server-release: dist build/SLOWDOWN.COM
	rm -rf $(SERVER_PKG) && mkdir -p $(SERVER_PKG)/tools $(SERVER_PKG)/build $(SERVER_PKG)/netdrive
	cp $(addprefix tools/,$(SERVER_TOOLS)) $(SERVER_PKG)/tools/
	cp build/WAVE86.EXE build/WAVEGET.EXE build/DRVOFF.EXE build/MEMLIM.EXE build/SLOWDOWN.COM build/SLOWDOWN.DOC build/WAVE86.DEF $(SERVER_PKG)/build/
	unzip -q -o -j $(ND_ZIP) -d $(SERVER_PKG)/netdrive && chmod +x $(SERVER_PKG)/netdrive/netdrive_*
	cp exodos/eXoDOS.torrent $(SERVER_PKG)/
	cp waveserve.def $(SERVER_PKG)/waveserve.ini
	cp docs/README-SERVER.txt $(SERVER_PKG)/README.txt
	cp requirements.txt $(SERVER_PKG)/
	cp LICENSE $(SERVER_PKG)/LICENSE.txt
	cp tools/release/run-server.sh tools/release/run-server.bat $(SERVER_PKG)/
	chmod +x $(SERVER_PKG)/run-server.sh
	cd dist && rm -f wave86-$(RELEASE_VER)-server.zip && zip -q -r wave86-$(RELEASE_VER)-server.zip wave86-$(RELEASE_VER)-server
	@ls -la dist/wave86-$(RELEASE_VER)-server.zip

picomem-release: picomem-image
	rm -rf dist/picomem && mkdir -p dist/picomem
	cp build/pmwave-edrdos.img dist/picomem/
	cp docs/README-PICOMEM.txt dist/picomem/README.txt
	cp THIRD-PARTY.md LICENSE dist/picomem/
	cd dist/picomem && rm -f ../wave86-$(RELEASE_VER)-picomem-hdd.zip && zip -q -9 ../wave86-$(RELEASE_VER)-picomem-hdd.zip pmwave-edrdos.img README.txt THIRD-PARTY.md LICENSE
	@ls -la dist/wave86-$(RELEASE_VER)-picomem-hdd.zip

# A ready-made disk for a PicoMem 2 (tools/mkimage.py): EDR-DOS, the
# launcher in C:\WAVE86, the card's DOS tools in C:\PICOMEM, mTCP in
# C:\MTCP, the Gravis UltraSound files in C:\ULTRASND, booting into WAVE86
# with the SD card on W:. The pieces are in third-party/ (its README and
# THIRD-PARTY.md say what and whose): the PicoMEM D6 release package and
# the card repository's drivers folder (the tools, the Gravis zip), mTCP's
# client zip, CDMKE.SYS (the PicoGUS project's copy), the FreeDOS utilities,
# and the EDR-DOS kernel, each unpacked into build/ on first use; make
# update-third-party refreshes them from where they came (the *_URL, *_VER
# and *_REF variables say where).
# An empty folder variable (CDMKE_DIR=, GUS_DIR=, DOS_DIR=)
# leaves that piece out; a folder of your own (PICOMEM_DIR=, MTCP_TOOLS=,
# CDMKE_DIR=, GUS_DIR=, DOS_DIR=, EDR_DIR=) is used instead
# of the unpacked one. The image boots in dosbox-x before it is declared
# done - twice, LBA and then CHS, with the FreeDOS boot sector (the
# EDR-DOS kernel boots under it too), once with a DOS_DISKS image, whose own boot
# sector the CHS patch does not know; the work files stay in build/
# whatever IMAGE says, so the image can go straight to the SD card.
#   make picomem-image
#   make picomem-image IMAGE=/Volumes/SD/HDD/PMWAVE.IMG IMAGE_MB=500
#   make picomem-image PICOMEM_DIR=$$HOME/picomem-drivers GUS_DIR=$$HOME/ultrasnd CDMKE_DIR=
IMAGE ?=
# The image boots the EDR-DOS (SvarDOS) kernel under the FreeDOS boot
# sector, from its release zip in third-party/edrdos (a grey-area licence:
# fine for this repo, not for anything commercial - THIRD-PARTY.md);
# KERNEL=freedos boots FreeDOS's own kernel instead, and
# DOS_DISKS="disk1.img disk2.img" your own licensed DOS, with its own SYS
# and its own tools (the disks are only read; the list is split on spaces,
# so no path in it can have one). The image is named after what it runs:
# build/pmwave-edrdos.img, -fdos, -msdos, -pcdos; IMAGE= overrides.
KERNEL ?= edrdos
EDR_VER ?= 20260309
EDR_URL ?= https://github.com/SvarDOS/edrdos/releases/download/v$(EDR_VER)/edrdos_$(EDR_VER).zip
EDR_ZIP = $(TP)/edrdos/edrdos_$(EDR_VER).zip
EDR_DIR ?= build/edrdos
DOS_DISKS ?=
IMAGE_MB ?= 512
# the card's tools: C:\PICOMEM on the image is the PICOMEM folder of the
# D6 release package (third-party/picomem/PM_D6_CONFIG.zip, the release
# matched to the firmware) plus NE2000.COM from the repository's drivers
# folder, which is in third-party/picomem/drivers whole (make update-picomem
# refreshes it; PICOMEM_REF pins a branch or commit; its PMINIT can be ahead
# of the release's). PICOMEM_DIR= names a folder of your own instead.
PICOMEM_REF ?= main
PICOMEM_URL = https://raw.githubusercontent.com/FreddyVRetro/ISA-PicoMEM/$(PICOMEM_REF)/drivers
PICOMEM_FILES = ASTCLOCK.COM NE2000.COM PICOMEM.EXE PM2000.COM PMDFS.EXE PMDFS3.EXE PMEMM.EXE PMINIT.EXE PMMOUSE.EXE README.md \
                SBCD/CD/FILE_ID.DIZ SBCD/CD/FILE_ID.OLD SBCD/CD/LICENSE.TXT SBCD/CD/LOCKCD.EXE SBCD/CD/README.COM SBCD/CD/README.NOW \
                SBCD/CD/README.TXT SBCD/CD/SBCD.SYS SBCD/CD/SETUPCD.EXE SBCD/CD/TESTCD.EXE SBCD/CD/UNLOCKCD.EXE SBCD/CD_DOS.ZIP SBCD/readme.txt \
                TEST/EMS/EMMSTAT.EXE TEST/EMS/EMSTEST.COM TEST/EMS/MOVETEST.COM TEST/EMS/OEMSTEST.COM TEST/PICOMEM.EXE ultrasnd.zip
PICOMEM_D6 = $(TP)/picomem/PM_D6_CONFIG.zip
PICOMEM_DIR ?= build/picomem
MTCP_VER ?= 2025-01-10
MTCP_URL = https://www.brutman.com/mTCP/download/mTCP_$(MTCP_VER).zip
MTCP_ZIP = $(TP)/mtcp/mTCP_$(MTCP_VER).zip
MTCP_TOOLS ?= build/mtcp
CDMKE_URL ?= https://picogus.com/drivers/cdmke.zip
CDMKE_ZIP = $(TP)/cdmke/cdmke.zip
CDMKE_DIR ?= build/cdmke
GUS_URL ?= $(PICOMEM_URL)/ultrasnd.zip
GUS_ZIP = $(TP)/picomem/drivers/ultrasnd.zip
GUS_DIR ?= build/ultrasnd
MKIMAGE_ARGS ?=
# FreeDOS utilities for C:\DOS: EDIT and a few friends, and HIMEMX and
# JEMM386, the memory managers (GPL; the FreeDOS 1.3 packages, in
# third-party/dosutils)
FREEDOS_REPO ?= https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base
DOS_PKGS ?= edit mem more xcopy deltree attrib find tree label himemx jemm
DOS_ZIPS = $(foreach p,$(DOS_PKGS),$(TP)/dosutils/$(p).zip)
DOS_DIR ?= build/dosutils
PICOMEM_INPUTS = $(PICOMEM_DIR) $(MTCP_TOOLS) $(CDMKE_DIR) $(GUS_DIR) $(DOS_DIR) $(if $(filter edrdos,$(KERNEL)),$(EDR_DIR))
# the build/ defaults are unpacked by the rules below; anything else must be there already
PICOMEM_FETCH = $(filter build/picomem build/mtcp build/cdmke build/ultrasnd build/dosutils build/edrdos,$(PICOMEM_INPUTS))
picomem-image: picomem-folders dist $(PICOMEM_FETCH)
	python3 tools/mkimage.py --dist dist/wave86 --picomem "$(PICOMEM_DIR)" --mtcp "$(MTCP_TOOLS)" \
	  $(if $(CDMKE_DIR),--cdmke "$(CDMKE_DIR)") $(if $(GUS_DIR),--gus "$(GUS_DIR)") $(if $(DOS_DIR),--dos "$(DOS_DIR)") \
	  --kernel $(KERNEL) $(if $(filter edrdos,$(KERNEL)),--edr "$(EDR_DIR)") $(foreach d,$(DOS_DISKS),--dos-disk $(d)) \
	  --size $(IMAGE_MB) $(if $(IMAGE),-o "$(IMAGE)") $(MKIMAGE_ARGS)

# a folder given by hand that is not there fails here, before the dist is built
picomem-folders:
	@for d in $(filter-out $(PICOMEM_FETCH),$(PICOMEM_INPUTS)); do \
	  test -d "$$d" || { echo "picomem-image: no folder $$d (PICOMEM_DIR, MTCP_TOOLS, CDMKE_DIR, GUS_DIR, DOS_DIR or EDR_DIR)"; exit 1; }; done

# C:\PICOMEM: the D6 package's PICOMEM folder, the packet driver, and any
# program put straight into third-party/picomem (a fix from the author, as
# PMDFS.EXE is), which replaces the package's copy of the same name
PICOMEM_FIXES = $(wildcard $(TP)/picomem/*.EXE $(TP)/picomem/*.COM $(TP)/picomem/*.SYS)
build/picomem: $(PICOMEM_D6) $(TP)/picomem/drivers/NE2000.COM $(PICOMEM_FIXES)
	@rm -rf build/picomem.part && mkdir -p build/picomem.part
	cd build/picomem.part && unzip -q -o -j $(abspath $(PICOMEM_D6)) 'PM_D6_CONFIG/PICOMEM/*'
	cp $(TP)/picomem/drivers/NE2000.COM build/picomem.part/
	$(if $(PICOMEM_FIXES),cp $(PICOMEM_FIXES) build/picomem.part/,@echo "  (no fixes in $(TP)/picomem)")
	@rm -rf build/picomem && mv build/picomem.part build/picomem && ls build/picomem | tr '\n' ' ' && echo

# the pieces, unpacked from third-party/ into build/; a newer archive there
# unpacks again, since each of these depends on its archive
build/edrdos: $(EDR_ZIP)
	@rm -rf build/edrdos.part && mkdir -p build/edrdos.part
	cd build/edrdos.part && unzip -q -o $(abspath $(EDR_ZIP))
	@rm -rf build/edrdos && mv build/edrdos.part build/edrdos && ls build/edrdos | tr '\n' ' ' && echo

build/dosutils: $(DOS_ZIPS) Makefile
	@rm -rf build/dosutils.part && mkdir -p build/dosutils.part
	for z in $(DOS_ZIPS); do unzip -q -j -o $$z 'BIN/*' -d build/dosutils.part || { echo "no BIN/ in $$z"; exit 1; }; done
	@rm -rf build/dosutils && mv build/dosutils.part build/dosutils && ls build/dosutils | tr '\n' ' ' && echo

build/mtcp: $(MTCP_ZIP)
	@mkdir -p build
	rm -rf build/mtcp && unzip -q -o -d build/mtcp $(MTCP_ZIP)
	@ls build/mtcp

build/cdmke: $(CDMKE_ZIP)
	@mkdir -p build
	rm -rf build/cdmke && unzip -q -o -d build/cdmke $(CDMKE_ZIP)
	@ls build/cdmke

# the Gravis UltraSound software (ultrasnd.zip, flat: ULTRASND.INI, ULTRINIT,
# ULTRAMID, the MIDI\ patches...), the copy the PicoMEM wiki points at
build/ultrasnd: $(GUS_ZIP)
	@mkdir -p build
	rm -rf build/ultrasnd && unzip -q -o -d build/ultrasnd $(GUS_ZIP)
	@ls build/ultrasnd | wc -l | sed 's/^ *//;s/$$/ entries in build\/ultrasnd/'

# ---- third-party/: refreshing the pieces from where they came. Each is
# recorded with its origin and SHA-256 in third-party/SOURCES.txt by
# tools/thirdparty.py; a refresh that brings the same bytes changes nothing,
# and one that cannot reach its site leaves what is there and says so - the
# other pieces are still tried, and make ends with an error.
# A new version has a new file name (MTCP_VER=, ND_VER=, EDR_VER=): the old
# archive stays until you delete it. Look at what changed, then commit it.
# check-third-party, which CI runs, says whether every recorded file is
# there and unchanged.
FETCH = python3 tools/thirdparty.py fetch
UPDATES = update-picomem update-mtcp update-netdrive update-cdmke update-gus update-dosutils update-edrdos update-slowdown
update-third-party:
	@rc=0; for t in $(UPDATES); do $(MAKE) -s $$t || rc=1; done; exit $$rc
update-picomem:
	@for f in $(PICOMEM_FILES); do $(FETCH) $(TP)/picomem/drivers/$$f $(PICOMEM_URL)/$$f || exit 1; done
update-mtcp:
	@$(FETCH) $(MTCP_ZIP) $(MTCP_URL)
update-netdrive:
	@$(FETCH) $(ND_ZIP) $(ND_URL)
update-cdmke:
	@$(FETCH) $(CDMKE_ZIP) $(CDMKE_URL)
update-gus:
	@$(FETCH) $(GUS_ZIP) $(GUS_URL)
update-dosutils:
	@for p in $(DOS_PKGS); do $(FETCH) $(TP)/dosutils/$$p.zip $(FREEDOS_REPO)/$$p.zip || exit 1; done
update-edrdos:
	@$(FETCH) $(EDR_ZIP) $(EDR_URL)
update-slowdown:
	@$(FETCH) $(TP)/slowdown/SLOWDOWN.COM $(SLOWDOWN_RAW)/BIN/SLOWDOWN.COM
	@$(FETCH) $(TP)/slowdown/SLOWDOWN.DOC $(SLOWDOWN_RAW)/DOC/SLOWDOWN/SLOWDOWN.DOC
check-third-party:
	@python3 tools/thirdparty.py check

# Open Watcom V2 into toolchain/ (not in git: 50 MB of compilers), the
# host tools for this machine plus h/ and lib286/, from a dated release
# rather than the rolling Current-build, so a build here and one on CI
# use the same compiler; OW_TAG= picks another release.
OW_TAG ?= 2026-09-01-Build
toolchain:
	@if [ -d toolchain/h ] && [ -d toolchain/$(WATCOM_BIN) ]; then echo "toolchain/ is there ($(WATCOM_BIN)); rm -rf toolchain to fetch $(OW_TAG) again"; exit 0; fi; \
	curl -sSfL -o ow-snapshot.tar.xz https://github.com/open-watcom/open-watcom-v2/releases/download/$(OW_TAG)/ow-snapshot.tar.xz && \
	mkdir -p toolchain && tar -xJf ow-snapshot.tar.xz -C toolchain ./$(WATCOM_BIN) ./h ./lib286 && rm ow-snapshot.tar.xz && ls toolchain

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

# NETUP.BAT also sets the DOS clock from NTP (mTCP's SNTP) once DHCP is
# done. SNTP wants the time zone in TZ, as a POSIX rule: this machine's own
# by default (/etc/localtime ends with it, CET-1CEST,M3.5.0,M10.5.0/3 in
# Norway), UTC0 when that cannot be read; DOS_TZ= and NTP_SERVER= override.
DOS_TZ ?= $(or $(shell tail -n 1 /etc/localtime 2>/dev/null | grep -E '^[A-Za-z<][^ ]*$$'),UTC0)
NTP_SERVER ?= pool.ntp.org
build/NETUP.BAT: Makefile
	@mkdir -p build
	@printf '@echo off\r\nZ:\\SYSTEM\\NE2000.COM 0x60 3 0x300\r\nset MTCPCFG=C:\\BUILD\\MTCP.CFG\r\nset WAVESRV=10.0.2.2:8086\r\nDHCP\r\nset TZ=$(DOS_TZ)\r\nC:\\BUILD\\MTCP\\SNTP -set -retries 2 $(NTP_SERVER)\r\n' > $@

# CYCLES is the machine's speed: 50000 is a fast 486, where dosbox-x's own
# default of 3000 is an XT (make run CYCLES=3000 to see the launcher on one)
CYCLES ?= 50000
run: all build/NETUP.BAT build/mtcp
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

.PHONY: all run test dostest dosrun netdrive waveserve dist server-release picomem-release picomem-image picomem-folders bootsector clean music music-files thumbs net cdrom \
        slowdown toolchain check-third-party update-third-party update-picomem update-mtcp update-netdrive update-cdmke update-gus update-dosutils update-edrdos update-slowdown
