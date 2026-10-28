# Building the SW1000XG x64 driver

Build on a Windows host.

## CI
`.github/workflows/build.yml` builds Debug and Release x64 on every push, to any
branch, that touches `port/` (Markdown-only changes are skipped). A PR shows the
checks of its head commit's push run. The WDK and SDK come from
NuGet (`port/packages.config`, wired in by `port/Directory.Build.props`), so the
runner needs no WDK installer. Each run uploads `logs-*` (errors-only log, full
log, MSBuild binlog) and, on success, the driver package with
`Install-Microcode.ps1` alongside it. Compiler errors are annotated on the run
page.

Release builds are also test-signed and packaged with the install/test scripts
as the **hardware test package** (artifact `hw-test-package`), then smoke-tested.
Pushing a tag `hw-test-NN` publishes that package as a GitHub pre-release for the
tester; see `TESTING.md`.

`.github/workflows/scripts.yml` syntax-checks every script, checks that the
microcode manifest's three copies agree (`tools/check_manifest.py`), and tests the
microcode installer against a synthetic `yswds.sys` under PowerShell 7 (Linux,
Windows) and Windows PowerShell 5.1, including the registry write under a
throwaway service key.

Reading a run from a shell: `gh run list --branch <branch>`, then
`gh run view <id> --log-failed`.

## Local build (Windows 10/11)
Prerequisites: Visual Studio 2022 or later with "Desktop development with C++",
or the Enterprise WDK (EWDK). The WDK itself can come from NuGet as in CI:

    nuget restore port\packages.config -PackagesDirectory port\packages
    msbuild port\ysw1000xg.vcxproj /p:Configuration=Release /p:Platform=x64

Without the NuGet restore, an installed WDK (with its VS extension) is used.
Output goes to `port\bin\x64\<Configuration>\`. Signing is off by default; add
`/p:SignMode=TestSign` for a test-signed package.

## Install / test (on the test machine)
Instructions are in `TESTING.md` (`Install-Driver.ps1` and
`Run-HwTest.ps1` do the steps below). The manual equivalent:

1. `infverif /v /w ysw1000xg.inf` and fix any findings.
2. Turn on Driver Verifier for `ysw1000xg.sys` (IRQL, pool, deadlock, DMA).
3. Test-sign the `.sys` and `.cat`; install the INF (right-click → Install, or
   `pnputil /add-driver ysw1000xg.inf /install`). The device fails to start
   (Code 10) until the microcode is installed: that is by design.
4. Install the microcode from your own Yamaha driver download (next section),
   with `-RestartDevice`.
5. Confirm the device starts and binds (Device Manager, no code 10/39).

## Microcode
The DSP microcode (34 Yamaha tables, 0x2500 bytes) is **not** in this repo or
in the driver package, and cannot be redistributed. The user supplies it
from their own copy of Yamaha's Win2000/XP driver: `yswds.sys` v1.01.0014.1,
SHA-256 `0e475f6a…b4b4`, either the file or the original
`Yamaha_SW10000XG_Win2000-XP.zip` that contains it.

On the Windows machine, after installing the driver, from an elevated
PowerShell in the package folder:

    powershell -ExecutionPolicy Bypass -File .\Install-Microcode.ps1 `
        -Source C:\path\to\Yamaha_SW10000XG_Win2000-XP.zip -RestartDevice

The script checks the source file's SHA-256 and each table's, then writes the
bundle as one `REG_BINARY` value and restarts the card:

    HKLM\SYSTEM\CurrentControlSet\Services\ysw1000xg\Parameters\Microcode

It refuses if the driver service isn't installed yet (install the INF first).
Other modes: `-OutFile <file>` writes the bundle to a file instead (no
elevation), `-Remove` deletes the value, and `-WhatIf` shows what would change.
`-ExecutionPolicy Bypass` is needed because the script is unsigned and, from a
download, carries the mark of the web.

The bundle is every table in `inc/sw1000_blobs.h` `SW_BLOB` order, concatenated,
no header. The driver checks the total length and every table's SHA-256 when it
starts and refuses to run on any mismatch (H-009, H-017). The service name in the
path must match the INF's `AddService` name (`ysw1000xg`).

The manifest (names, VAs, lengths, SHA-256) exists in three places that must
agree, checked in CI by `tools/check_manifest.py`:
- `tools/blobs_sw1000.csv`: the reference;
- `src/blobs.cpp` (`g_SwBlobs`): what the driver verifies;
- `install/Install-Microcode.ps1` (`$Manifest`): what the installer extracts.

On Linux, `python3 -I tools/extract_blobs.py ../yswds.sys ../extracted --bundle
../extracted/microcode.bundle` produces the same bundle (it refuses to write
inside the repo). Both tools print the bundle's SHA-256, so the two can be
compared.
