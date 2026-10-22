@echo off
rem Double-click: telnet to the Pi1MHz in Windows Terminal with the BBC Micro font and colours.
rem   BBC-Micro-telnet.cmd [host] [mode]
rem host: "Pi1MHz" (the Pi's network name) if none is given, or another name or an IP address.
rem mode: the Beeb's screen MODE the window is for - 7 (the default: the Beeb starts in MODE 7), 0 or 3, 1, 4 or 6,
rem 2 or 5.  Each width has its own font, so the characters have the shape the Beeb gives them; a window opened
rem for one MODE still shows the others, in that MODE's font.
rem e.g.  BBC-Micro-telnet.cmd 192.168.1.20 0
set HOST=%~1
if "%HOST%"=="" set HOST=Pi1MHz
set MODE=%~2
if "%MODE%"=="" set MODE=7
set PROFILE=BBC Micro MODE 7
set SIZE=40,25
if "%MODE%"=="0" (set "PROFILE=BBC Micro MODE 0/3" & set SIZE=80,32)
if "%MODE%"=="3" (set "PROFILE=BBC Micro MODE 0/3" & set SIZE=80,32)
if "%MODE%"=="1" (set "PROFILE=BBC Micro MODE 1/4/6" & set SIZE=40,32)
if "%MODE%"=="4" (set "PROFILE=BBC Micro MODE 1/4/6" & set SIZE=40,32)
if "%MODE%"=="6" (set "PROFILE=BBC Micro MODE 1/4/6" & set SIZE=40,32)
if "%MODE%"=="2" (set "PROFILE=BBC Micro MODE 2/5" & set SIZE=20,32)
if "%MODE%"=="5" (set "PROFILE=BBC Micro MODE 2/5" & set SIZE=20,32)
start "" wt.exe -w new --size %SIZE% -p "%PROFILE%" cmd.exe /c "%LOCALAPPDATA%\Pi1MHz TelnetD\connect.cmd" %HOST%
