Русская версия: [README_RU.md](README_RU.md)

# DiskPrep v1.0

A standalone DiskPart GUI and Windows Setup disk preparation helper for WinPE, with optional `/winsetup` target selection and Ventoy integration.

DiskPrep is intentionally small: it exposes common disk-preparation operations in a native Win32 GUI, warns about risky or incompatible choices, and leaves the final decision to the user.

<img alt="изображение" src="https://github.com/user-attachments/assets/053b5c21-a551-41a3-8b59-eb12c13a1be0" />


## Design

- Native Win32 C++17.
- No .NET, PowerShell, WMI, or external runtime DLL dependency.
- Static C/C++ runtime in Release builds.
- Separate x86 and x64 binaries from one source tree.
- Disk/partition discovery through Win32 storage APIs, not localized DiskPart output parsing.
- DiskPart is used only to perform requested partitioning/formatting operations.
- Existing partition tables are preserved unless the user explicitly chooses **Wipe...**.
- DiskPart runs on a worker thread and its output is captured to a per-session UTF-8 log in `%TEMP%`.

## Main features

- Physical disk selector showing model, capacity, and GPT / MBR / RAW style.
- Graphical partition map and detailed partition/free-space table.
- All separate unallocated ranges are shown and selectable.
- Delete selected partition.
- Quick-format as NTFS / FAT32 / exFAT.
- Set/change/clear volume labels through Unicode `SetVolumeLabelW`.
- Create a normal DATA partition in selected free space.
- Create Windows partition layouts in selected free space.
- Extend and shrink supported NTFS basic-data partitions.
- Convert GPT / MBR when DiskPart allows it.
- Explicit **Wipe...** command using `clean + rescan`, with confirmation.
- Firmware-aware GPT/MBR warnings without silently changing the user's requested operation.
- Detailed per-session diagnostics and **Open log** button.

### Windows partition layouts

For GPT/UEFI-style layouts DiskPrep can create:

- EFI System Partition: 200 MB on 512/512e media, 300 MB on 4Kn media;
- MSR: 16 MB;
- Windows NTFS partition;
- optional Recovery partition: 1024 MB.

For MBR/Legacy-style layouts DiskPrep can create:

- 550 MB active System Reserved partition;
- Windows NTFS partition.

Existing suitable EFI/MSR/Recovery/System Reserved partitions are not recreated unnecessarily.

DATA creation creates only the requested data partition. It does not add Windows service partitions.

## `/winsetup` mode

Run:

```text
DiskPrep_x64.exe /winsetup
```

or:

```text
DiskPrep_x86.exe /winsetup
```

This adds **Install Windows here**. The button is enabled only for a selected NTFS basic-data partition.

Before preparing the target, DiskPrep checks the current firmware mode against the disk partition style:

- UEFI + GPT: normal confirmation;
- Legacy BIOS + MBR: normal confirmation;
- UEFI + MBR or Legacy BIOS + GPT: one combined warning/confirmation explains that Windows Setup may refuse the target. DiskPrep does not convert the disk automatically.

After confirmation DiskPrep prepares an answer file containing the selected `DiskID` and `PartitionID`:

- If `HKLM\SYSTEM\Setup\UnattendFile` points to an existing XML file, DiskPrep copies it to `X:\DiskPrep\Unattend.xml`, patches the copy, and leaves the source file untouched.
- If no usable registry pointer exists, DiskPrep creates a minimal answer file for the current executable architecture (`amd64` or `x86`).
- Active `DiskConfiguration/Disk` entries are removed from a copied answer file so an old unattended repartition recipe cannot overwrite the disk layout prepared by DiskPrep.
- `InstallToAvailablePartition` is removed when explicit `InstallTo` is used.
- `ImageInstall/OSImage/InstallTo` is set to the selected disk and partition.
- The registry pointer is changed only after the output XML has been written successfully.
- Closing DiskPrep without pressing **Install Windows here** does not change the Setup answer-file pointer.

### Important `/winsetup` limitation

**Install Windows here must be used before Windows Setup has already discovered and started processing a separate `Autounattend.xml`.**

Supported workflows are:

1. Start DiskPrep before `setup.exe` (the recommended automated workflow).
2. Start Windows Setup without `Autounattend.xml`, open `Shift+F10` on the first language-selection page, then run DiskPrep with `/winsetup`.

If the running Setup instance has already discovered and processed an external `Autounattend.xml`, changing `HKLM\SYSTEM\Setup\UnattendFile` later does not reliably make that Setup instance re-read the replacement file. This late-injection scenario is therefore unsupported.

DiskPrep deliberately does not search disks or `setupact.log` for other unattend files.

If you provide your own answer file, its `processorArchitecture` entries must match the Windows PE/Setup architecture. DiskPrep patches the selected target but does not convert an existing answer file between `amd64` and `x86`.

## Ventoy integration

Ventoy is optional. DiskPrep does not require Ventoy for standalone use.

The included `VentoyInjection` template uses this startup order:

```text
VentoyAutoRun.bat
    -> preload WinPE drivers
    -> winpeshl.ini
    -> wpeinit.exe
    -> DiskPrep\LaunchDiskPrep.cmd
    -> DiskPrep /winsetup
    -> X:\sources\setup.exe
```

This is the preferred automated `/winsetup` workflow because DiskPrep patches or creates the answer file before Windows Setup starts.

If Ventoy auto-install has already prepared an answer file and set `HKLM\SYSTEM\Setup\UnattendFile`, DiskPrep patches a copy of that file. If no usable registry answer-file pointer exists, DiskPrep creates its minimal answer file instead.

`LaunchDiskPrep.cmd` selects `DiskPrep_x86.exe` or `DiskPrep_x64.exe` according to the WinPE architecture.

To build the injection archive, place both Release binaries in:

```text
VentoyInjection\DiskPrep\
```

The archive root should then contain:

```text
VentoyAutoRun.bat
DiskPrep\LaunchDiskPrep.cmd
DiskPrep\DiskPrep_x86.exe
DiskPrep\DiskPrep_x64.exe
Windows\System32\winpeshl.ini
```

## Build

Open `DiskPrep.sln` in Visual Studio 2022 with **Desktop development with C++** and a Windows 10/11 SDK installed.

Build:

```text
Release | x86   -> bin\Release\DiskPrep_x86.exe
Release | x64   -> bin\Release\DiskPrep_x64.exe
```

Both Release configurations use the static C/C++ runtime.


## Notes and limitations

- DiskPrep is not a full partition manager. It does not move partitions, clone disks, manage dynamic disks, or perform filesystem conversion.
- Extend works only into suitable contiguous unallocated space immediately to the right, matching DiskPart limitations.
- Shrink availability is determined by DiskPart `shrink querymax`.
- MBR extended/logical layouts are not a primary target; creation logic is intended for ordinary basic GPT/MBR Windows Setup disks.
- Windows/DiskPart remains the final authority for environment-dependent formatting and resize restrictions.
- Native x86 and x64 builds are required; there is no single native C++ “AnyCPU” binary for both WinPE architectures.
- Destructive operations should be tested on disposable media/VMs before use on valuable data.

DiskPart output follows the language of the current Windows/WinPE environment. DiskPrep v1.0 UI is English-only.

## Version history

### v1.0

Initial public release. The current standalone partitioning behavior, `/winsetup` target-selection workflow, and optional Ventoy integration are the v1.0 baseline.
