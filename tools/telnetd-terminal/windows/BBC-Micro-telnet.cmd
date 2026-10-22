@echo off
rem Double-click: telnet to the Pi1MHz in Windows Terminal with the BBC Micro font and colours, 80 x 32.
rem The host is "Pi1MHz" (the Pi's network name); give another name or an IP address as the first argument,
rem e.g.  BBC-Micro-telnet.cmd 192.168.1.20
rem It uses WSL's telnet when WSL has one (it shows the pound sign and the teletext graphics, which need UTF-8),
rem otherwise Windows' own telnet client (which cannot show UTF-8).
set HOST=%~1
if "%HOST%"=="" set HOST=Pi1MHz
wsl.exe -e sh -c "command -v telnet" >nul 2>&1
if %errorlevel%==0 (
    start "" wt.exe -w new --size 80,32 -p "BBC Micro (WSL telnet)" wsl.exe -e telnet %HOST%
) else (
    start "" wt.exe -w new --size 80,32 -p "BBC Micro (telnet)" telnet.exe %HOST%
)
