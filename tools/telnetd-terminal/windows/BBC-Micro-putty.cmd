@echo off
rem Double-click: open PuTTY's saved session "BBC Micro telnet" (add it first with ..\putty\BBC-Micro-telnet.reg).
rem Give another host name or an IP address as the first argument, e.g.  BBC-Micro-putty.cmd 192.168.1.20
set PUTTY=putty.exe
where putty.exe >nul 2>&1 || set PUTTY="%ProgramFiles%\PuTTY\putty.exe"
if "%~1"=="" (
    start "" %PUTTY% -load "BBC Micro telnet"
) else (
    start "" %PUTTY% -load "BBC Micro telnet" -telnet %1
)
