@echo off
set WAVE86=LAUNCH
WAVE86.EXE %1 %2
:loop
if not exist WAVERUN.BAT goto done
call WAVERUN.BAT
if exist WAVERUN.BAT del WAVERUN.BAT
if exist RUNGAME.BAT del RUNGAME.BAT
WAVE86.EXE
goto loop
:done
set WAVE86=
