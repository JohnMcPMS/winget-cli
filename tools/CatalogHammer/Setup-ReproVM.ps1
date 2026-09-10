# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.
#
# VM preparation for the WindowsPackageManagerServer CET fail-fast repro (Watson #63388223).
# Run elevated. Reboot is NOT required unless you change the language list.

[CmdletBinding()]
param(
    [string] $DumpFolder = 'C:\CetReproDumps',
    [switch] $AddLanguages,
    [switch] $CheckServer,
    [switch] $CheckHardware,
    [switch] $Cleanup
)

$ErrorActionPreference = 'Stop'
$arpUser = 'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'

function Test-CetHardwareSupport {
    <#
        User shadow stacks require CPU support: Intel 11th gen (Tiger Lake) or newer, or
        AMD Zen 3 (Ryzen 5000 / Threadripper PRO 5000) or newer. On anything older the feature
        is simply unavailable, and every process reports UserShadowStack OFF no matter how the
        policy is configured. In a VM the hypervisor must also expose CET to the guest.

        There is no supported API surfaced to PowerShell for "does this CPU have CET", so this
        probes empirically: on a capable and enabled machine a number of system processes run
        with shadow stacks. Zero out of everything means the platform cannot do it at all.
    #>
    Write-Host 'CPU:' (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()

    $on = 0
    $checked = 0
    foreach ($p in Get-Process) {
        try {
            $s = (Get-ProcessMitigation -Id $p.Id -ErrorAction Stop).UserShadowStack
            $checked++
            if ($s.UserShadowStack -eq 'ON') { $on++ }
        } catch {
            # Access denied on protected or higher-integrity processes; expected, ignore.
        }
    }

    Write-Host "Processes with user shadow stacks active: $on of $checked readable"

    if ($on -eq 0) {
        Write-Warning 'No process on this machine has user shadow stacks active.'
        Write-Warning 'This machine CANNOT reproduce the crash. The cause is almost certainly'
        Write-Warning 'that the CPU predates CET (Intel 11th gen / AMD Zen 3), or that the'
        Write-Warning 'hypervisor is not exposing CET to this guest.'
        return $false
    }

    Write-Host 'User shadow stacks are active on this machine.'
    return $true
}

function Show-ServerShadowStackState {
    # Get-ProcessMitigation -Name reports only what IFEO / policy has been configured for that
    # image name. It does NOT report a process's effective mitigations, so for a packaged process
    # it will normally show shadow stacks "off" even when they are active. Packaged processes take
    # their defaults from the package and from the OS, and user shadow stacks additionally require
    # the binary to be built /CETCOMPAT. The only reliable check is against a live PID.
    $procs = Get-Process -Name 'WindowsPackageManagerServer' -ErrorAction SilentlyContinue
    if (-not $procs) {
        Write-Host 'WindowsPackageManagerServer is not running, so effective mitigations cannot be read.'
        Write-Host 'Start the harness, then re-run:  .\Setup-ReproVM.ps1 -CheckServer'
        return
    }

    foreach ($p in $procs) {
        Write-Host "PID $($p.Id) effective mitigations:"
        try {
            (Get-ProcessMitigation -Id $p.Id).UserShadowStack | Format-List
        } catch {
            Write-Warning "Get-ProcessMitigation -Id $($p.Id) failed: $_"
        }
    }
}

if ($CheckHardware) {
    Test-CetHardwareSupport | Out-Null
    return
}

if ($CheckServer) {
    Show-ServerShadowStackState
    return
}

if ($Cleanup) {
    Get-ChildItem $arpUser -ErrorAction SilentlyContinue |
        Where-Object { $_.PSChildName -like 'WinGetCetRepro_*' } |
        Remove-Item -Recurse -Force
    Write-Host 'Removed WinGetCetRepro_* ARP entries.'
    return
}

# ---------------------------------------------------------------- 1. WER dumps
# The server fail-fasts, so we need a full dump captured at the moment of death.
$werKey = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\WindowsPackageManagerServer.exe'
New-Item -Path $werKey -Force | Out-Null
New-Item -ItemType Directory -Force -Path $DumpFolder | Out-Null
Set-ItemProperty -Path $werKey -Name 'DumpFolder' -Value $DumpFolder -Type ExpandString
Set-ItemProperty -Path $werKey -Name 'DumpType'   -Value 2 -Type DWord   # 2 = full dump
Set-ItemProperty -Path $werKey -Name 'DumpCount'  -Value 50 -Type DWord
Write-Host "WER LocalDumps configured -> $DumpFolder (full dumps, max 50)"

# ------------------------------------------------------ 2. Confirm CET is armed
# If user shadow stacks are not active in the server process, the repro cannot fire.
# Hardware capability is the first gate and the most common reason a machine cannot repro.
Write-Host ''
$cetOk = Test-CetHardwareSupport
Write-Host ''
Write-Host 'System-wide shadow stack policy:'
(Get-ProcessMitigation -System).UserShadowStack | Format-List
Write-Host 'Note: a system-wide setting of NOTSET/OFF does not mean the server lacks shadow stacks.'
Write-Host 'Packaged processes take different defaults, and /CETCOMPAT binaries opt in per-image.'
Write-Host ''
Show-ServerShadowStackState

# ------------------------------------------------- 3. Language amplifier (opt)
# Bucket C faulted in the BCP47mrm language-resolution path, which builds a vector from the
# user's preferred language list. More languages = a larger vector and more allocations
# on exactly the path that faulted.
if ($AddLanguages) {
    $langs = New-WinUserLanguageList en-US
    foreach ($tag in 'fr-FR','de-DE','ja-JP','zh-CN','es-ES','ru-RU','ko-KR','pt-BR','it-IT','ar-SA') {
        $langs.Add($tag)
    }
    Set-WinUserLanguageList $langs -Force
    Write-Host "Preferred language list set to $($langs.Count) entries. Sign out/in to fully apply."
}

# ------------------------------------------------------------ 4. Baseline info
Write-Host ''
Write-Host 'Baseline for the bug report:'
[PSCustomObject]@{
    OSBuild    = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').BuildLabEx
    Cpu        = (Get-CimInstance Win32_Processor | Select-Object -First 1).Name.Trim()
    Ucrtbase   = (Get-Item "$env:SystemRoot\System32\ucrtbase.dll").VersionInfo.FileVersion
    WinSqlite  = (Get-Item "$env:SystemRoot\System32\winsqlite3.dll").VersionInfo.FileVersion
    Bcp47mrm   = (Get-Item "$env:SystemRoot\System32\BCP47mrm.dll").VersionInfo.FileVersion
    WinGet     = (Get-AppxPackage Microsoft.DesktopAppInstaller).Version
    Processors = $env:NUMBER_OF_PROCESSORS
} | Format-List

Write-Host ''
if (-not $cetOk) {
    Write-Warning 'STOP: user shadow stacks are not active on this machine, so the crash cannot'
    Write-Warning 'reproduce here. Move to CET-capable hardware before running the harness.'
    Write-Host ''
}
Write-Host 'Next:'
Write-Host '  .\bin\x64\Release\CatalogHammer.exe --threads 16 --seed-arp 3000'
Write-Host '  .\Setup-ReproVM.ps1 -CheckServer     (once the server is up, to confirm shadow stacks)'
Write-Host ''
Write-Host 'The raw #CP error code is only recorded in kernel state and cannot be read from a'
Write-Host 'user-mode dump. To capture it, attach a kernel debugger before starting the run and'
Write-Host 'coordinate with the OS team on extraction.'
