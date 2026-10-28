# Testing the SW1000XG driver on real hardware

## One-time setup

### Before you start
- **Expect crashes.** This is an unfinished kernel driver: it can blue-screen
  the PC, and preferably don't use a PC that
  you can't afford to have misbehave.
- **BitLocker recovery key.** If the PC uses BitLocker or Device Encryption,
  turning off Secure Boot (below) can make Windows ask for the recovery key at
  the next start. Find it first: <https://account.microsoft.com/devices/recoverykey>,
  or Settings > Privacy & security > Device encryption.
- **Restore point** (optional, recommended): Start, type "Create a restore point",
  then Create.
- **The Yamaha driver download.** The card's DSP needs microcode from Yamaha's
  original Windows 2000/XP driver, which this project cannot redistribute. Put `Yamaha_SW10000XG_Win2000-XP.zip`, still
  zipped, in your **Downloads** folder. Don't pass it on.

### Turn off Secure Boot
Windows only loads test-signed drivers in test mode, and test mode needs Secure
Boot off.
1. Settings > System > Recovery > Advanced startup > **Restart now**, then
   Troubleshoot > Advanced options > **UEFI Firmware Settings** > Restart.
   (Or press the setup key while the PC starts: often Del, F2 or F10.)
2. Find **Secure Boot** (usually on a Boot, Security or Authentication page)
   and set it to **Disabled**.
3. Save and exit (often F10).

### Turn on test mode
1. Start, type `cmd`, right-click **Command Prompt** > **Run as administrator**.
2. Run:

       bcdedit /set testsigning on

   It should say "The operation completed successfully". If it says the value
   is protected by Secure Boot policy, Secure Boot is still on.
3. Restart. "Test Mode" now shows in the bottom-right corner of the desktop.

## Installing

1. Go to releases and download `SW1000XG-hw-test-NN.zip`
   (under **Assets**).
3. Right-click the zip > **Extract All**, into Downloads.
4. Open PowerShell as administrator: Start, type `powershell`, right-click
   **Windows PowerShell** > **Run as administrator**.
5. Go to the extracted folder and allow its scripts to run in this window
   (replace NN with the release's number):

       cd "$env:USERPROFILE\Downloads\SW1000XG-hw-test-NN"
       Set-ExecutionPolicy -Scope Process Bypass

   Answer `Y` if asked.
6. Install the build:

       .\Install-Driver.ps1

   It checks test mode and the Yamaha download, installs the driver and
   microcode, and ends with "Installed". If it stops with an error, let us know.
7. Run the test:

       .\Run-HwTest.ps1

   It first shows what this round needs (for example "listen to the card's
   output") and waits for Enter. When the test has run, it asks one or two
   questions about what you saw or heard: type the answer and press Enter. It
   takes about a minute and prints a report that includes your answers.
8. The report is already on your clipboard. In the issue, click in the comment
   box, press **Ctrl+V**, then **Comment**. Paste the text rather than
   attaching the file: attachments in this private repo can't be read by the
   tooling. (A copy is saved on your Desktop as `SW1000XG-report-....txt`.)
9. Add anything you noticed in the same comment: sounds, freezes, error
   pop-ups.

## If the PC crashes (blue screen)

Let it restart. The driver records the step it crashed in, and stays away
from the card from then on. After logging in, repeat steps 4 and 5, then run
`.\Run-HwTest.ps1`. It reports the crash, so paste that report into the issue.
If you saw a stop code on the blue screen, add it too.

## If Windows won't start normally

1. Start in Safe Mode. Hold **Shift** while clicking Restart (or let Windows
   fail to start twice), then Troubleshoot > Advanced options > Startup
   Settings > Restart > press **4**.
2. In an administrator PowerShell, in the test folder:

       Set-ExecutionPolicy -Scope Process Bypass
       .\Uninstall-Driver.ps1

3. Restart normally and tell us in the issue.

## Removing everything afterwards

1. In an administrator PowerShell, in any test folder: `.\Uninstall-Driver.ps1`
2. In an administrator Command Prompt: `bcdedit /set testsigning off`, then restart.
3. Turn Secure Boot back on in the firmware settings.

## What the scripts do

| Script | What it does |
|---|---|
| `Install-Driver.ps1` | checks test mode and the Yamaha zip; trusts this build's test certificate; replaces any earlier test driver; writes the microcode to the registry; starts the driver in a safe "identify only" mode |
| `Run-HwTest.ps1` | lets the driver run this round's stages, restarts the card, and collects the report |
| `Install-Microcode.ps1` | used by `Install-Driver.ps1`; extracts the microcode from the Yamaha zip |
| `Uninstall-Driver.ps1` | removes the driver, its settings and the test certificates |

The report contains: the Windows version, whether test mode and Secure Boot are
on, the card's status in Windows, the driver's own log, and any recent
blue screens. It contains **no user name or computer name**; the script
replaces them with `<redacted>` if they would appear.
