@echo off
rem WAVE86 server for Windows. It checks for what the server needs - an
rem x64 or x86 Python from 3.9 to 3.13 (pip has libtorrent for those only),
rem the Visual C++ runtime libtorrent uses (14.40 or newer), and ffmpeg (for the games'
rem pictures) - and offers to install what is missing with winget. Then it
rem puts the Python packages from requirements.txt into .venv here, once,
rem and starts the server. Anything after run-server.bat goes to the
rem server: run-server.bat --port 8090. README.txt says more.
setlocal
pushd "%~dp0" 2>nul || goto nofolder
if not exist "tools\waveserve.py" goto notunpacked
if not exist "requirements.txt" goto notunpacked
set "VPY=.venv\Scripts\python.exe"
set "OK=.venv\wave86.ok"
set "PYTHON_MANAGER_AUTOMATIC_INSTALL=false"
set "ARCH=%PROCESSOR_ARCHITECTURE%"
if defined PROCESSOR_ARCHITEW6432 set "ARCH=%PROCESSOR_ARCHITEW6432%"
rem the 64-bit Program Files and System32, also from a 32-bit cmd.exe
set "PF=%ProgramFiles%"
if defined ProgramW6432 set "PF=%ProgramW6432%"
set "SYS64=%SystemRoot%\System32"
if defined PROCESSOR_ARCHITEW6432 set "SYS64=%SystemRoot%\Sysnative"
rem what a Python has to be: 3.9 to 3.13, for x64 or x86 Windows
set "PYTEST=import sys, sysconfig; sys.exit(not ((3, 9) <= sys.version_info[:2] <= (3, 13) and sysconfig.get_platform() in ('win32', 'win-amd64')))"

rem ---- Python, and .venv made with it
if not exist "%VPY%" goto novenv
"%VPY%" -c "%PYTEST%" <nul >nul 2>&1 || goto badvenv
if exist "%OK%" goto ffmpeg
"%VPY%" -m pip --version <nul >nul 2>&1 || goto badvenv
goto vcrt
:badvenv
echo The .venv here does not work with this server (its Python is gone, is
echo not 3.9 to 3.13 for x64 or x86 Windows, or has no pip). Making it again.
rmdir /s /q ".venv" 2>nul
:novenv
call :findpy
if defined PYRUN goto makevenv
echo The server needs Python 3.9 to 3.13 for x64 or x86, and none was found.
call :ask "Install Python 3.13 with winget"
if errorlevel 1 goto nopython
rem x64 on anything but 32-bit Windows: ARM64 Windows runs it, pip has no ARM64 libtorrent
set "WGARCH="
if /i not "%ARCH%"=="x86" set "WGARCH=--architecture x64"
call :winget Python.Python.3.13 %WGARCH%
call :findpy
if not defined PYRUN goto nopython
:makevenv
echo Setting up .venv with %PYRUN%
%PYRUN% -m venv --clear .venv || goto venvfailed

rem ---- the Visual C++ runtime libtorrent needs
:vcrt
set "VCID=Microsoft.VCRedist.2015+.x64"
set "DLL=%SYS64%\msvcp140.dll"
"%VPY%" -c "import struct, sys; sys.exit(struct.calcsize('P') != 4)" <nul >nul 2>&1 || goto vcrtcheck
set "VCID=Microsoft.VCRedist.2015+.x86"
if exist "%SystemRoot%\SysWOW64" set "DLL=%SystemRoot%\SysWOW64\msvcp140.dll"
:vcrtcheck
rem libtorrent's wheels need msvcp140.dll 14.40 or newer (an older one crashes it)
powershell -NoProfile -NonInteractive -Command "try { $v = (Get-Item -LiteralPath $env:DLL -ErrorAction Stop).VersionInfo; if ($v.FileMajorPart * 100 + $v.FileMinorPart -ge 1440) { exit 0 } } catch {}; exit 1" <nul >nul 2>&1 && goto ffmpeg
echo libtorrent needs the Visual C++ runtime (msvcp140.dll) 14.40 or newer,
echo and it is missing or older here.
call :ask "Install the Visual C++ runtime with winget"
if errorlevel 1 goto ffmpeg
call :winget %VCID%

rem ---- ffmpeg, where winget puts it too
:ffmpeg
set "PATH=%PATH%;%LOCALAPPDATA%\Microsoft\WinGet\Links;%PF%\WinGet\Links"
call :ffmpegpath
where ffmpeg >nul 2>&1
if not errorlevel 1 goto packages
if exist "ffmpeg-no.txt" goto packages
echo ffmpeg was not found. The server makes the games' pictures with it,
echo and runs without them otherwise.
call :ask "Install FFmpeg with winget"
if errorlevel 1 goto noffmpeg
call :winget Gyan.FFmpeg
if errorlevel 1 goto packages
call :ffmpegpath
where ffmpeg >nul 2>&1
if errorlevel 1 echo FFmpeg is installed; the server finds it from the next start on.
goto packages
:noffmpeg
echo no> "ffmpeg-no.txt"
echo Not asking again: delete ffmpeg-no.txt here to be asked next time.

rem ---- the Python packages from requirements.txt
:packages
if exist "%OK%" goto run
echo Installing the server's Python packages (requirements.txt) into .venv
"%VPY%" -m pip install --disable-pip-version-check -r requirements.txt || goto pipfailed
"%VPY%" -c "import libtorrent, curses" || goto pipfailed
echo ok> "%OK%"
goto run
:pipfailed
echo The Python packages are not all in place. The server starts anyway and
echo says what it misses (the torrent needs libtorrent); run-server.bat tries
echo again next time. Behind a proxy, set HTTPS_PROXY first.

:run
"%VPY%" tools\waveserve.py %*
set "RC=%errorlevel%"
if not "%RC%"=="0" pause
popd
exit /b %RC%

rem ---- the pieces

:findpy
rem PYRUN = how to start the newest Python that passes PYTEST: through the
rem py launcher, python on the PATH, or where python.org (and winget) put it
set "PYRUN="
for %%V in (3.13 3.12 3.11 3.10 3.9) do if not defined PYRUN py -%%V -c "%PYTEST%" <nul >nul 2>&1 && set "PYRUN=py -%%V"
if not defined PYRUN python -c "%PYTEST%" <nul >nul 2>&1 && set "PYRUN=python"
for %%V in (313 312 311 310 39) do if not defined PYRUN if exist "%LOCALAPPDATA%\Programs\Python\Python%%V\python.exe" "%LOCALAPPDATA%\Programs\Python\Python%%V\python.exe" -c "%PYTEST%" <nul >nul 2>&1 && set PYRUN="%LOCALAPPDATA%\Programs\Python\Python%%V\python.exe"
for %%V in (313 312 311 310 39) do if not defined PYRUN if exist "%PF%\Python%%V\python.exe" "%PF%\Python%%V\python.exe" -c "%PYTEST%" <nul >nul 2>&1 && set PYRUN="%PF%\Python%%V\python.exe"
exit /b 0

:ffmpegpath
for /d %%D in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg_*") do for /d %%E in ("%%~D\ffmpeg-*") do if exist "%%~E\bin\ffmpeg.exe" set "PATH=%PATH%;%%~E\bin"
exit /b 0

:ask
rem errorlevel 0 for Y, 1 for N
choice /C YN /M "%~1"
if errorlevel 2 exit /b 1
exit /b 0

:winget
where winget >nul 2>&1
if not errorlevel 1 goto haswinget
echo winget was not found: it comes with App Installer, from the Microsoft Store.
exit /b 1
:haswinget
winget install -e --id %* --accept-package-agreements --accept-source-agreements
rem winget's failures are negative (0x8A15....): compare with 0
if not "%errorlevel%"=="0" exit /b 1
exit /b 0

:nopython
echo Install Python 3.13 for x64 (python.org, or: winget install -e --id
echo Python.Python.3.13 --architecture x64) and start run-server.bat again.

:venvfailed
rmdir /s /q ".venv" 2>nul
:failed
pause
popd
exit /b 1

:notunpacked
echo Unpack the whole zip first (Extract All in Explorer) and start
echo run-server.bat in the unpacked folder.
pause
popd
exit /b 1

:nofolder
echo run-server.bat cannot get into its own folder: "%~dp0"
pause
exit /b 1
