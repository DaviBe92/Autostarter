# Windows tests

## Automated end-to-end test

`Test-Autostarter.ps1` installs the built plugin into OBS Studio, starts OBS with a temporary test loadout, closes OBS, and checks the results. Your own Autostarter settings are backed up first and restored afterwards.

Requirements: Windows 10/11 x64, OBS Studio 33 or later, PowerShell 7, and a completed build (`.github\scripts\Build-Windows.ps1`). The test compiles its helper app with the .NET Framework `csc.exe` that ships with Windows.

Close OBS, open PowerShell 7 **as administrator** in the repository root, and run:

```powershell
pwsh -ExecutionPolicy Bypass -File .\tests\windows\Test-Autostarter.ps1
```

Options:

- `-SkipInstall` uses the plugin that is already installed and does not need administrator rights.
- `-DllPath <path>` tests a specific `autostarter.dll`.
- `-ObsRoot <path>` points to the OBS installation folder if the script can't find it.
- `-KeepArtifacts` keeps the temporary folder with the helper apps and their output.

The script exits with code 0 when every check passes. If you run `Package-Windows.ps1` first, the script also checks both release ZIP layouts.

| Area | What is checked |
| --- | --- |
| Install | DLL lands in `C:\ProgramData\obs-studio\plugins\autostarter`, and any legacy `obs-plugins\64bit` copy is moved aside |
| Command line | `--autostarter <loadout>` launches without the prompt, even with the plugin disabled |
| Launch order and delays | Apps start top to bottom, and a 5 s delay is honoured |
| Arguments | A quoted argument with a space stays one argument, and plain arguments are passed through |
| Paths | An app in a folder whose name has a space and a non-ASCII character starts with the right working directory |
| Shortcuts | A `.lnk` resolves to its target, keeps its working directory, and user arguments follow the shortcut's own |
| Auto-close | Apps, and a child process one of them starts, are closed when OBS exits normally |
| Per-app opt-out | An app with Auto-close unticked keeps running |
| Log | The OBS log shows the module initialized and no launch failures |

## Manual checks

These need the settings window, so they are not automated:

1. **Tools → Autostarter** opens, and the plug-in manager does not mark Autostarter as Legacy.
2. **Start Menu…** lists shortcuts, search filters the list, and double-click adds an app.
3. **Browse Files…**, **Move Up**, **Move Down**, **Remove**, **Args…**, the delay box, and the Minimized and Auto-close boxes all update the list. **Save** keeps the changes after an OBS restart, and **Close** without saving discards them.
4. **Ask on launch** shows the loadout prompt at startup. Launch starts the apps and Skip doesn't.
5. **Quit Apps** during a pending delay cancels the remaining launches.
6. An app that was already running before OBS started is left open when OBS exits.
