# WAVE86 - MS-DOS game launcher
# Cross-compiled with Open Watcom V2 (toolchain/) to 16-bit real mode.

WATCOM := $(abspath toolchain)
export WATCOM
export PATH := $(WATCOM)/armo64:$(PATH)
export INCLUDE := $(WATCOM)/h

# -0: 8086 instructions only  -ms: small model  -os: optimize for size
CFLAGS = -q -bcl=dos -0 -ms -os -wx

SRCS = src/wave86.c src/ui.c src/vga.c src/scan.c src/ini.c src/music.c \
       src/mod.c src/cpu.c

MUSIC = $(wildcard music/*.IMF music/*.imf music/*.WLF music/*.wlf \
                   music/*.MOD music/*.mod)

all: build/WAVE86.EXE build/WAVE.BAT build/WAVE86.INI music-files thumbs

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

# regenerate the soundtrack from the composers
music:
	python3 tools/makemusic.py music
	python3 tools/makemod.py music/WAVE86.MOD

# interactive run in dosbox-x (project root is C:, launcher in C:\BUILD)
run: all
	dosbox-x -fastlaunch -c "mount c ." -c "c:" -c "cd BUILD" -c "WAVE.BAT"

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

.PHONY: all run test clean music music-files thumbs
