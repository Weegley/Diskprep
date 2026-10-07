# Ventoy injection template

This template starts DiskPrep before Windows Setup while keeping driver preload separate from the Setup launch chain.

Archive layout:

```text
VentoyAutoRun.bat
Windows\System32\winpeshl.ini
DiskPrep\LaunchDiskPrep.cmd
DiskPrep\DiskPrep_x64.exe
DiskPrep\DiskPrep_x86.exe
```

Flow:

```text
Ventoy injection
 -> VentoyAutoRun.bat
 -> winpeshl.ini
 -> wpeinit.exe
 -> LaunchDiskPrep.cmd
 -> DiskPrep /winsetup
 -> X:\sources\setup.exe
```

`VentoyAutoRun.bat` preloads drivers from `$WinPEDriver$` and `X:\Drivers` with `pnputil` and rescans devices.

`LaunchDiskPrep.cmd` selects x86/x64 according to the native WinPE architecture.

Copy the two Release executables into `VentoyInjection\DiskPrep` before creating the injection archive.
