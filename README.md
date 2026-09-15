# ResetBootDevice

`ResetBootDevice.exe` is an x64 Windows RE utility that retrieves the Dell BIOS
setup password for the current service tag and uses Dell Command Configure
(`cctk.exe`) to toggle `EmbSataRaid` between AHCI and RAID.

## Build requirements

Open `ResetBootDevice.slnx` in Visual Studio and accept the components in
`.vsconfig`, or install these components with Visual Studio Installer:

- Desktop development with C++
- MSVC v145 x64/x86 build tools
- Windows 11 SDK 10.0.26100.0 with Desktop C++ x64 libraries

Build `Release | x64`. The application uses the static C/C++ runtime so WinRE
does not need the MSVC runtime for `ResetBootDevice.exe`. The build also copies
`vcruntime140.dll`, which Dell's `libcrypto.dll` requires, into the CCTK resource
folder.

The clean deployable output is `ResetBootDevice/x64/Release/WinRE`. Copy that
whole directory into the recovery workflow. Do not copy only
`ResetBootDevice.exe`; its adjacent `resources` directory is required.

## WinRE execution

Use an x64 Windows 10/11 RE environment on a supported Dell computer. The tool
must run with firmware-management privileges; WinRE normally runs as SYSTEM.
Initialize networking before launching it if the recovery workflow has not
already done so:

```bat
wpeinit
X:\path\ResetBootDevice.exe
```

The tool first reads the service tag from the WinRE hardware registry and falls
back to WMI. It sends the tag as `{"Reservation":"<service-tag>"}`, checks for an
HTTP 2xx response, Base64-decodes the returned UTF-8 password, queries
`cctk.exe --EmbSataRaid`, and then executes exactly one of:

```text
cctk.exe --ValSetupPwd=<retrieved password> --EmbSataRaid=Raid
cctk.exe --ValSetupPwd=<retrieved password> --EmbSataRaid=Ahci
```

The password is not written to console output. Dell CCTK requires it on its
command line, so it exists in the child process command line only for the short
life of that process. The utility clears its writable password and command-line
buffers after use.

The existing password-service URL remains the default. To supply a replacement
without rebuilding, set it in the WinRE command shell before launch:

```bat
set RESETBOOTDEVICE_PASSWORD_URL=https://example.invalid/password-endpoint
```

Any failure to obtain the service tag/password, start CCTK, identify the current
mode, or apply the new mode returns a nonzero process exit code. Because changing
AHCI/RAID can make an installed OS unbootable when its storage driver is not
prepared for the target mode, use this only in the intended recovery workflow.
