@echo off
rem connect.cmd [host]: telnet to the Pi1MHz's TelnetD (host "Pi1MHz" if none is given), with WSL's telnet when WSL
rem has one (it shows UTF-8: the pound sign and the teletext graphics), else Windows' own telnet.exe.
rem The Windows Terminal profiles run this (install.ps1 puts it in %LOCALAPPDATA%\Pi1MHz TelnetD).
set HOST=%~1
if "%HOST%"=="" set HOST=Pi1MHz
wsl.exe -e sh -c "command -v telnet" >nul 2>&1
if %errorlevel%==0 (
    wsl.exe -e telnet %HOST%
) else (
    telnet.exe %HOST%
)
