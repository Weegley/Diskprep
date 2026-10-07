# DiskPrep v1.0

Initial public release of DiskPrep.

DiskPrep is a standalone native DiskPart GUI and Windows Setup disk-preparation helper for WinPE. It provides a compact graphical view of physical disks, partitions, and free space, and exposes common Windows Setup preparation operations without trying to become a full partition manager.

Highlights:

- native Win32 x86 and x64 builds with no .NET dependency;
- GPT/MBR/RAW disk and partition display;
- DATA and Windows partition creation;
- format, label, delete, shrink, extend, convert, and wipe operations;
- firmware-aware GPT/MBR warnings;
- `/winsetup` mode with **Install Windows here** to select the exact Setup target;
- safe copy-and-patch handling for an existing `HKLM\SYSTEM\Setup\UnattendFile`;
- minimal answer-file generation when no usable registry answer-file pointer exists;
- optional Ventoy injection template that starts DiskPrep before Windows Setup;
- detailed per-session diagnostics.

Important `/winsetup` note: target injection must happen before Windows Setup has already discovered and started processing a separate external `Autounattend.xml`. The recommended automated workflow starts DiskPrep before `setup.exe`. Manual `Shift+F10` use is supported from the first language-selection page when Setup was started without an external `Autounattend.xml`.

Existing answer files must use the correct `processorArchitecture` for the WinPE/Setup architecture; DiskPrep does not translate an existing XML between `amd64` and `x86`.
