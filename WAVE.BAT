@echo off
set WAVE86=LAUNCH
WAVE86.EXE %1 %2
:loop
if not exist RUNGAME.BAT goto done
call RUNGAME.BAT
if exist RUNGAME.BAT del RUNGAME.BAT
WAVE86.EXE
goto loop
:done
set WAVE86=
