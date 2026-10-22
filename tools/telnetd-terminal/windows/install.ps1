# install.ps1 - the BBC Micro look for a telnet session to TelnetD, for the current Windows user (no administrator
# rights): the four BBC Micro fonts (one for each width of the Beeb's screen: MODE 7, MODE 0/3, MODE 1/4/6, MODE 2/5),
# a Windows Terminal profile for each with the BBC's colours, and connect.cmd, which they run.
# Run by install.cmd.  Remove with: install.ps1 -Remove
param([switch]$Remove)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$kit = Split-Path -Parent $here
$fontDir = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows\Fonts'
$fontKey = 'HKCU:\Software\Microsoft\Windows NT\CurrentVersion\Fonts'
$fonts = [ordered]@{
    'BBCMicroTeletext.ttf' = 'BBC Micro Teletext'
    'BBCMicroMOS.ttf'      = 'BBC Micro MOS'
    'BBCMicroMOS40.ttf'    = 'BBC Micro MOS 40'
    'BBCMicroMOS20.ttf'    = 'BBC Micro MOS 20'
}
$fragDir = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows Terminal\Fragments\Pi1MHz TelnetD'
$appDir = Join-Path $env:LOCALAPPDATA 'Pi1MHz TelnetD'

if ($Remove) {
    foreach ($f in $fonts.Keys) {
        Remove-ItemProperty -Path $fontKey -Name "$($fonts[$f]) Regular (TrueType)" -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $fontDir $f) -ErrorAction SilentlyContinue
    }
    Remove-Item $fragDir -Recurse -ErrorAction SilentlyContinue
    Remove-Item $appDir -Recurse -ErrorAction SilentlyContinue
    Write-Host 'Removed the BBC Micro fonts and the Windows Terminal profiles.'
    return
}

# The fonts, for this user.  The registry entries only take effect at the next sign-in: each font is loaded into
# this session now too, and running programs are told (WM_FONTCHANGE to all windows).
Add-Type -Namespace TelnetD -Name Font -MemberDefinition @'
[DllImport("gdi32.dll", CharSet = CharSet.Unicode)] public static extern int AddFontResourceW(string file);
[DllImport("gdi32.dll", CharSet = CharSet.Unicode)] public static extern int RemoveFontResourceW(string file);
[DllImport("user32.dll")] public static extern IntPtr SendMessageTimeoutW(IntPtr hWnd, uint msg, IntPtr w, IntPtr l, uint flags, uint timeout, out IntPtr result);
'@
New-Item -ItemType Directory -Force $fontDir | Out-Null
foreach ($f in $fonts.Keys) {
    $src = Join-Path $kit $f
    $dst = Join-Path $fontDir $f
    if (-not (Test-Path $dst) -or (Get-FileHash $src).Hash -ne (Get-FileHash $dst).Hash) {
        if (Test-Path $dst) { [void][TelnetD.Font]::RemoveFontResourceW($dst) }    # a new version: unload the old
        try { Copy-Item $src $dst -Force }
        catch {
            Write-Host "$($fonts[$f]) is in use (an open Windows Terminal or PuTTY window): close them and run this again."
            exit 1
        }
    }
    New-ItemProperty -Path $fontKey -Name "$($fonts[$f]) Regular (TrueType)" -Value $dst -PropertyType String -Force | Out-Null
    [void][TelnetD.Font]::AddFontResourceW($dst)
    Write-Host "Font installed: $($fonts[$f])"
}
$r = [IntPtr]::Zero
[void][TelnetD.Font]::SendMessageTimeoutW([IntPtr]0xffff, 0x001D, [IntPtr]::Zero, [IntPtr]::Zero, 2, 1000, [ref]$r)

# connect.cmd (the profiles run it) and the profiles: Windows Terminal picks up "fragments" from this folder by itself
New-Item -ItemType Directory -Force $appDir | Out-Null
Copy-Item (Join-Path $here 'connect.cmd') (Join-Path $appDir 'connect.cmd') -Force
New-Item -ItemType Directory -Force $fragDir | Out-Null
Copy-Item (Join-Path $here 'telnetd-fragment.json') (Join-Path $fragDir 'telnetd.json') -Force
Write-Host 'Windows Terminal profiles added: BBC Micro MODE 7, MODE 0/3, MODE 1/4/6, MODE 2/5'

$wsl = $false
try { & wsl.exe -e sh -c 'command -v telnet' *> $null; $wsl = ($LASTEXITCODE -eq 0) } catch { }
if (-not $wsl) {
    Write-Host ''
    Write-Host "WSL has no telnet client, so the profiles use Windows' own telnet.exe, which cannot show the pound sign"
    Write-Host 'or the teletext graphics (UTF-8).  For those, install telnet in WSL (Debian: sudo apt install telnet) or use PuTTY.'
    if (-not (Get-Command telnet.exe -ErrorAction SilentlyContinue)) {
        Write-Host "Windows' telnet client is not installed either.  Turn it on (as administrator) with:"
        Write-Host '    dism /online /Enable-Feature /FeatureName:TelnetClient'
    }
}
Write-Host ''
Write-Host 'Start it with BBC-Micro-telnet.cmd, or from Windows Terminal''s menu.  Close Windows Terminal first if it is open.'
