# XTray

A lightweight, portable Windows tray application for managing Xray-core.

## Project status

Implementation has started. The first stage contains a minimal native Win32 window, embedded application manifest, CMake configuration, and a GitHub Actions build workflow.

Xray process management, tray controls, settings, themes, and user-logon startup are planned and are not implemented in this initial skeleton. Closing the initial window exits the application.

The first cloud build and Windows runtime validation are pending repository access.

## Project documents

- [Requirements (Chinese)](docs/requirements.md)
- [Technical plan (Chinese)](docs/technical-plan.md)
- [Development progress (Chinese)](docs/development.md)

## Build and download

The intended build environment is GitHub Actions; no local compiler installation is required.

1. Push the project files to this repository.
2. Open **Actions**, then **Build**. Source and build changes trigger the workflow automatically; it also supports **Run workflow**.
3. After a successful run, download **XTray-windows-x64** from **Artifacts**.
4. Extract and run `XTray.exe`.

The workflow builds an x64 Release executable using MSVC and C++20 with a static C++ runtime. It runs a hidden-window startup test and checks the resulting executable's architecture, GUI subsystem, and runtime dependencies before uploading only `XTray.exe`.

Official version-tag releases will be added in a later implementation stage.

## Local Xray files

The root-level `bin/` directory contains an external Xray executable and its accompanying files for local runtime testing. It is excluded from Git tracking and is not part of XTray's build dependencies or bundled EXE.

The default Xray executable path is `.\bin\xray.exe`, resolved relative to the XTray executable. Users can configure another path once settings are implemented.

Xray-core and its configuration are provided and updated separately by the user. XTray does not generate or edit Xray configuration content.
