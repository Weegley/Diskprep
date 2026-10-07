# Changelog

## v1.0

Initial public release.

### Disk preparation

- Native Win32 DiskPart GUI for WinPE/Windows Setup environments.
- Physical disk, partition, and unallocated-space display.
- Create DATA partitions without adding Windows service partitions.
- Create GPT/UEFI and MBR/Legacy Windows layouts.
- Delete, format, label, shrink, extend, convert, and wipe operations.
- Firmware-aware GPT/MBR guidance and warnings.
- Unicode volume-label handling through `SetVolumeLabelW`.
- Per-session diagnostic logging.

### Windows Setup integration

- Optional `/winsetup` mode with **Install Windows here**.
- Explicit `DiskID` / `PartitionID` injection into a copied or minimal answer file.
- Existing answer files are used only through `HKLM\SYSTEM\Setup\UnattendFile`.
- Existing source XML is never modified in place.
- Registry pointer changes only after a successful output-file write.
- Active unattended disk-repartition entries are removed from the patched copy.
- GPT/MBR vs firmware mismatch warning is combined with the install confirmation, avoiding a second confirmation dialog.
- Late replacement after Setup has already processed an external `Autounattend.xml` is documented as unsupported.

### Ventoy

- Optional injection template.
- Driver preload before Windows Setup.
- `winpeshl.ini` launch chain runs DiskPrep before `setup.exe`.
- Architecture-aware launcher selects x86 or x64 DiskPrep.
