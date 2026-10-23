@echo off
rem Double-click: open a PuTTY saved session for TelnetD (add them first with ..\putty\BBC-Micro-telnet.reg).
rem   BBC-Micro-putty.cmd [host] [mode]
rem host: the session's own ("Pi1MHz") if none is given, or another name or an IP address.
rem mode: the Beeb's screen MODE the window is for: 7 (the default), 0 or 3, 1, 4 or 6, 2 or 5.
set PUTTY=putty.exe
where putty.exe >nul 2>&1 || set PUTTY="%ProgramFiles%\PuTTY\putty.exe"
set MODE=%~2
if "%MODE%"=="" set MODE=7
set SESSION=BBC Micro MODE 7
if "%MODE%"=="0" set "SESSION=BBC Micro MODE 0-3"
if "%MODE%"=="3" set "SESSION=BBC Micro MODE 0-3"
if "%MODE%"=="1" set "SESSION=BBC Micro MODE 1-4-6"
if "%MODE%"=="4" set "SESSION=BBC Micro MODE 1-4-6"
if "%MODE%"=="6" set "SESSION=BBC Micro MODE 1-4-6"
if "%MODE%"=="2" set "SESSION=BBC Micro MODE 2-5"
if "%MODE%"=="5" set "SESSION=BBC Micro MODE 2-5"
if "%~1"=="" (
    start "" %PUTTY% -load "%SESSION%"
) else (
    start "" %PUTTY% -load "%SESSION%" -telnet %1
)
