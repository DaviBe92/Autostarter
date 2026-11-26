#Requires -Version 7
<#
.SYNOPSIS
    End-to-end test of Autostarter in a real OBS Studio installation on Windows.

.DESCRIPTION
    1. Checks the packaged ZIP layouts (if release ZIPs exist).
    2. Installs the built DLL into the OBS 33 plugin folder (unless -SkipInstall).
    3. Builds a small marker app that records its command line, working directory and start time.
    4. Writes a temporary Autostarter config with a test loadout (the user's config is backed up and restored).
    5. Starts OBS with --autostarter AutostarterTest, waits for the apps, closes OBS normally.
    6. Checks launch order, delays, arguments, shortcut handling, paths with spaces and non-ASCII
       characters, auto-close (including child processes) and the per-app auto-close opt-out.

    Close OBS before running. Run from an elevated PowerShell 7 prompt unless -SkipInstall is used.

.EXAMPLE
    pwsh -ExecutionPolicy Bypass -File .\tests\windows\Test-Autostarter.ps1
#>
[CmdletBinding()]
param(
    [string] $DllPath,
    [string] $ObsRoot,
    [switch] $SkipInstall,
    [switch] $KeepArtifacts,
    [int] $TimeoutSeconds = 90
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = Resolve-Path "$PSScriptRoot/../.."
$LoadoutName = 'AutostarterTest'
$Results = [System.Collections.Generic.List[object]]::new()

function Check([string] $Name, [bool] $Ok, [string] $Detail = '') {
    $Results.Add([pscustomobject]@{ Result = $(if ($Ok) { 'PASS' } else { 'FAIL' }); Check = $Name; Detail = $Detail })
    Write-Host ("[{0}] {1} {2}" -f $(if ($Ok) { 'PASS' } else { 'FAIL' }), $Name, $Detail) -ForegroundColor $(if ($Ok) { 'Green' } else { 'Red' })
}

function Find-ObsRoot {
    foreach ($key in 'HKLM:\SOFTWARE\OBS Studio', 'HKLM:\SOFTWARE\WOW6432Node\OBS Studio') {
        $value = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).'(default)'
        if ($value -and (Test-Path "$value\bin\64bit\obs64.exe")) { return $value }
    }
    return "$env:ProgramFiles\obs-studio"
}

if (-not $IsWindows) { throw 'This test only runs on Windows.' }
if (Get-Process obs64 -ErrorAction SilentlyContinue) { throw 'Close OBS Studio before running the test.' }

if (-not $ObsRoot) { $ObsRoot = Find-ObsRoot }
$ObsExe = Join-Path $ObsRoot 'bin\64bit\obs64.exe'
if (-not (Test-Path $ObsExe)) { throw "obs64.exe not found at $ObsExe. Pass -ObsRoot." }
if (-not $DllPath) { $DllPath = Join-Path $ProjectRoot 'release\RelWithDebInfo\obs-plugins\64bit\autostarter.dll' }

$Work = Join-Path $env:TEMP ("autostarter-test-{0:yyyyMMdd-HHmmss}" -f (Get-Date))
$Out = Join-Path $Work 'out'
$Apps = Join-Path $Work 'apps'
$null = New-Item -ItemType Directory -Force -Path $Out, $Apps
$ConfigDir = Join-Path $env:APPDATA 'obs-studio\plugin_config\autostarter'
$ConfigFile = Join-Path $ConfigDir 'config.json'
$ConfigBackup = Join-Path $Work 'config.backup.json'
$obs = $null

try {
    # --- 1. Package layout -------------------------------------------------
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zips = Get-ChildItem (Join-Path $ProjectRoot 'release') -Filter 'autostarter-*-windows-x64*.zip' -ErrorAction SilentlyContinue
    foreach ($zip in $zips) {
        $archive = [System.IO.Compression.ZipFile]::OpenRead($zip.FullName)
        $entries = $archive.Entries.FullName -replace '\\', '/'
        $archive.Dispose()
        $expected = if ($zip.Name -like '*-obs33.zip') { 'autostarter/autostarter.dll' } else { 'obs-plugins/64bit/autostarter.dll' }
        Check "Package layout $($zip.Name)" ($entries -contains $expected) "expects $expected"
    }
    if (-not $zips) { Write-Host 'No release ZIPs found; skipping package layout checks (run Package-Windows.ps1 to include them).' }

    # --- 2. Install --------------------------------------------------------
    if (-not $SkipInstall) {
        $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
        if (-not $admin) { throw 'Run from an elevated prompt, or pass -SkipInstall if the plugin is already installed.' }
        if (-not (Test-Path $DllPath)) { throw "Built DLL not found at $DllPath. Run .github\scripts\Build-Windows.ps1 first or pass -DllPath." }

        $PluginDir = Join-Path $env:ProgramData 'obs-studio\plugins\autostarter'
        $null = New-Item -ItemType Directory -Force -Path $PluginDir
        Copy-Item $DllPath $PluginDir -Force
        $pdb = [IO.Path]::ChangeExtension($DllPath, '.pdb')
        if (Test-Path $pdb) { Copy-Item $pdb $PluginDir -Force }

        $LegacyBackup = Join-Path $Work 'legacy-plugin-backup'
        foreach ($legacy in 'autostarter.dll', 'autostarter.pdb') {
            $path = Join-Path $ObsRoot "obs-plugins\64bit\$legacy"
            if (Test-Path $path) {
                $null = New-Item -ItemType Directory -Force -Path $LegacyBackup
                Move-Item $path $LegacyBackup -Force
                $KeepArtifacts = $true   # never delete the user's previous plugin
                Write-Host "Moved legacy $legacy to $LegacyBackup"
            }
        }
        Check 'Installed to OBS 33 plugin folder' (Test-Path (Join-Path $PluginDir 'autostarter.dll')) $PluginDir
    }

    # --- 3. Marker apps ----------------------------------------------------
    $csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $marker = Join-Path $Work 'Marker.exe'
    & $csc /nologo /target:winexe "/out:$marker" (Join-Path $PSScriptRoot 'Marker.cs') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'Could not compile Marker.cs' }

    # B lives in a folder with a space and a non-ASCII character to test quoting and Unicode paths.
    $DirB = Join-Path $Apps ("Space Dir " + [char]0x00FC)
    $ShortcutWd = Join-Path $Apps 'shortcut-wd'
    $null = New-Item -ItemType Directory -Force -Path $DirB, $ShortcutWd
    $ExeA = Join-Path $Apps 'marker-a.exe'; Copy-Item $marker $ExeA
    $ExeB = Join-Path $DirB 'marker-b.exe'; Copy-Item $marker $ExeB
    $ExeC = Join-Path $Apps 'marker-c.exe'; Copy-Item $marker $ExeC
    $ExeD = Join-Path $Apps 'marker-d.exe'; Copy-Item $marker $ExeD

    $LinkC = Join-Path $Apps 'Marker C.lnk'
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($LinkC)
    $link.TargetPath = $ExeC
    $link.Arguments = "--name C --out `"$Out`" --from-shortcut"
    $link.WorkingDirectory = $ShortcutWd
    $link.Save()

    # --- 4. Test config ----------------------------------------------------
    if (Test-Path $ConfigFile) { Copy-Item $ConfigFile $ConfigBackup -Force }
    $null = New-Item -ItemType Directory -Force -Path $ConfigDir
    $config = [ordered]@{
        enabled        = $false   # --autostarter must launch even when disabled
        askToLaunch    = $true    # ... and without showing the prompt
        autoclose      = $true
        currentLoadout = $LoadoutName
        loadouts       = @(@{
            name     = $LoadoutName
            programs = @(
                @{ path = $Apps; executable = 'marker-a.exe'; arguments = "--name A --out `"$Out`" --spawn-child"
                   delaySeconds = 0; autoclose = $true; minimized = $false },
                @{ path = $DirB; executable = 'marker-b.exe'; arguments = "--name B --out `"$Out`" `"two words`" --flag=x"
                   delaySeconds = 5; autoclose = $true; minimized = $false },
                @{ path = $Apps; executable = 'Marker C.lnk'; arguments = '--user-arg'
                   delaySeconds = 0; autoclose = $true; minimized = $false },
                @{ path = $Apps; executable = 'marker-d.exe'; arguments = "--name D --out `"$Out`""
                   delaySeconds = 0; autoclose = $false; minimized = $true }
            )
        })
    }
    $config | ConvertTo-Json -Depth 6 | Set-Content -Path $ConfigFile -Encoding utf8NoBOM

    # --- 5. Run OBS --------------------------------------------------------
    $obs = Start-Process -FilePath $ObsExe -WorkingDirectory (Split-Path $ObsExe) -PassThru `
        -ArgumentList '--autostarter', $LoadoutName, '--disable-shutdown-check'
    $names = 'A', 'A-child', 'B', 'C', 'D'
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline -and ($names | Where-Object { -not (Test-Path (Join-Path $Out "$_.json")) })) {
        Start-Sleep -Milliseconds 500
    }
    Start-Sleep -Seconds 2

    $m = @{}
    foreach ($name in $names) {
        $file = Join-Path $Out "$name.json"
        Check "Launched $name" (Test-Path $file)
        if (Test-Path $file) { $m[$name] = Get-Content $file -Raw -Encoding utf8 | ConvertFrom-Json }
    }

    if ($m.B) {
        Check 'Path with space and non-ASCII character' ($m.B.exe -ieq $ExeB) $m.B.exe
        Check 'Quoted argument kept as one argument' ($m.B.args -contains 'two words') ($m.B.args -join ' | ')
        Check 'Plain argument passed' ($m.B.args -contains '--flag=x')
        Check 'Working directory is app folder' ($m.B.cwd.TrimEnd('\') -ieq $DirB) $m.B.cwd
    }
    if ($m.A -and $m.B) {
        $gap = ([datetime]$m.B.time - [datetime]$m.A.time).TotalSeconds
        Check 'B waits for its 5 s delay after A' ($gap -ge 4.5 -and $gap -le 20) ("{0:N1} s" -f $gap)
    }
    if ($m.B -and $m.C -and $m.D) {
        Check 'C and D launch after B (order kept)' (([datetime]$m.C.time -ge ([datetime]$m.B.time).AddSeconds(-0.5)) -and
            ([datetime]$m.D.time -ge ([datetime]$m.B.time).AddSeconds(-0.5)))
    }
    if ($m.C) {
        $iShortcut = [array]::IndexOf($m.C.args, '--from-shortcut')
        $iUser = [array]::IndexOf($m.C.args, '--user-arg')
        Check 'Shortcut resolved to its target' ($m.C.exe -ieq $ExeC) $m.C.exe
        Check 'User arguments appended after shortcut arguments' ($iShortcut -ge 0 -and $iUser -gt $iShortcut) ($m.C.args -join ' | ')
        Check 'Shortcut working directory kept' ($m.C.cwd.TrimEnd('\') -ieq $ShortcutWd) $m.C.cwd
    }

    # --- 6. Close OBS and check auto-close --------------------------------
    $null = $obs.CloseMainWindow()
    $closed = $obs.WaitForExit(60000)
    Check 'OBS closed normally' $closed
    if (-not $closed) { Stop-Process -Id $obs.Id -Force }
    Start-Sleep -Seconds 3

    foreach ($name in 'A', 'A-child', 'B', 'C') {
        if ($m[$name]) {
            $alive = [bool](Get-Process -Id $m[$name].pid -ErrorAction SilentlyContinue)
            Check "Auto-closed $name on OBS exit" (-not $alive)
        }
    }
    if ($m.D) {
        Check 'D left running (per-app auto-close off)' ([bool](Get-Process -Id $m.D.pid -ErrorAction SilentlyContinue))
    }

    # --- 7. OBS log ---------------------------------------------------------
    $log = Get-ChildItem (Join-Path $env:APPDATA 'obs-studio\logs') -Filter '*.txt' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($log) {
        $lines = Select-String -Path $log.FullName -Pattern 'autostarter' -SimpleMatch
        Check 'Log: module initialized' ([bool]($lines | Where-Object Line -match 'Module initialized')) $log.Name
        Check 'Log: no launch failures' (-not ($lines | Where-Object Line -match 'Could not (launch|open|resume)'))
        Write-Host "`nAutostarter lines in $($log.FullName):"
        $lines | ForEach-Object { Write-Host "  $($_.Line)" }
    } else {
        Check 'OBS log found' $false
    }
}
catch {
    Check 'Test run completed' $false $_.Exception.Message
}
finally {
    if ($obs -and -not $obs.HasExited) { Stop-Process -Id $obs.Id -Force -ErrorAction SilentlyContinue }
    Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($Work, [StringComparison]::OrdinalIgnoreCase) } |
        Stop-Process -Force -ErrorAction SilentlyContinue
    if (Test-Path $ConfigBackup) { Copy-Item $ConfigBackup $ConfigFile -Force }
    elseif (Test-Path $ConfigFile) { Remove-Item $ConfigFile -Force }

    $failed = @($Results | Where-Object Result -eq 'FAIL').Count
    Write-Host ''
    $Results | Format-Table -AutoSize | Out-Host
    Write-Host ("{0} passed, {1} failed" -f ($Results.Count - $failed), $failed)
    if ($KeepArtifacts -or $failed) {
        Write-Host "Artifacts kept in $Work"
    } else {
        Start-Sleep -Seconds 1
        Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue
    }
    exit [int]($failed -gt 0)
}
