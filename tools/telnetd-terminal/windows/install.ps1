# install.ps1 - the BBC Micro look for a telnet session to TelnetD, for the current Windows user (no administrator
# rights): the font "BBC Micro MOS" and a Windows Terminal profile "BBC Micro (telnet)" with the BBC's colours, 80 x 32.
# Run by install.cmd.  Remove with: install.ps1 -Remove
param([switch]$Remove)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$font = Join-Path (Split-Path -Parent $here) 'BBCMicroMOS.ttf'
$fontDir = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Fonts'
$fontKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Fonts'
$fontName = 'BBC Micro MOS Regular (TrueType)'
$fragDir = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows Terminal\Fragments\Pi1MHz TelnetD'

if ($Remove) {
    Remove-ItemProperty -Path $fontKey -Name $fontName -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $fontDir 'BBCMicroMOS.ttf') -ErrorAction SilentlyContinue
    Remove-Item $fragDir -Recurse -ErrorAction SilentlyContinue
    Write-Host 'Removed the BBC Micro font and the Windows Terminal profile.'
    return
}

# The font, for this user
New-Item -ItemType Directory -Force $fontDir | Out-Null
$dst = Join-Path $fontDir 'BBCMicroMOS.ttf'
Copy-Item $font $dst -Force
New-ItemProperty -Path $fontKey -Name $fontName -Value $dst -PropertyType String -Force | Out-Null
# The registry entry only takes effect at the next sign-in: load the font into this session now and tell running
# programs (WM_FONTCHANGE to all windows)
Add-Type -Namespace TelnetD -Name Font -MemberDefinition @'
[DllImport("gdi32.dll", CharSet = CharSet.Unicode)] public static extern int AddFontResourceW(string file);
[DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr hWnd, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
'@
[void][TelnetD.Font]::AddFontResourceW($dst)
$r = [IntPtr]::Zero
[void][TelnetD.Font]::SendMessageTimeoutW([IntPtr]0xffff, 0x001D, [IntPtr]::Zero, [IntPtr]::Zero, 2, 1000, [ref]$r)
Write-Host "Font installed: BBC Micro MOS"

# Windows Terminal picks up profile "fragments" from this folder by itself
New-Item -ItemType Directory -Force $fragDir | Out-Null
Copy-Item (Join-Path $here 'telnetd-fragment.json') (Join-Path $fragDir 'telnetd.json') -Force
Write-Host "Windows Terminal profiles added: BBC Micro (WSL telnet), BBC Micro (telnet)"

if (-not (Get-Command telnet.exe -ErrorAction SilentlyContinue)) {
    Write-Host ''
    Write-Host "Windows' telnet client is not installed.  Turn it on (as administrator) with:"
    Write-Host '    dism /online /Enable-Feature /FeatureName:TelnetClient'
    Write-Host 'or use PuTTY (see ..\putty).'
}
Write-Host ''
Write-Host 'Start it with BBC-Micro-telnet.cmd, or from Windows Terminal''s menu.  Close Windows Terminal first if it is open.'
