# XTray

A lightweight, portable Windows tray application for managing Xray-core.

## Project status

The initial cloud-built executable has been downloaded and opened successfully on the user's Windows computer.

The second implementation stage adds Xray start/stop/restart controls, process status icons, live output capture, source filtering, and a native settings dialog for executable/configuration paths and log retention. This stage still needs a new cloud build and runtime acceptance.

Tray integration, one-instance-per-directory enforcement, theme selection, and user-logon startup are planned for later stages. In this development build, closing the main window stops the managed Xray process and exits XTray.

## Project documents

- [Requirements (Chinese)](docs/requirements.md)
- [Technical plan (Chinese)](docs/technical-plan.md)
- [Development progress and acceptance checklist (Chinese)](docs/development.md)

## Build and download

The build environment is GitHub Actions; no local compiler installation is required.

1. Push source, resource, test, or build changes to this repository.
2. Open **Actions**, then **Build**. The workflow also supports **Run workflow**.
3. After a successful run, download **XTray-windows-x64** from **Artifacts**.
4. Extract and run `XTray.exe`.

The workflow builds an x64 Release executable with MSVC, C++20, and a static C++ runtime. It tests hidden-window log rendering and core behavior, then checks the executable's architecture, GUI subsystem, and runtime dependencies. Only `XTray.exe` is uploaded.

Core tests use a dedicated test executable as a fake Xray child. They do not launch the external Xray executable, open a proxy listener, or access the network. Test executables and fixtures are not shipped.

Official version-tag releases will be added in a later implementation stage.

## Run Xray

1. Open **Settings** (设置).
2. Select the external Xray executable, or enter a path relative to `XTray.exe`. The default is `.\bin\xray.exe`.
3. Add configuration files, one per line, and/or choose one configuration directory. Both are empty by default; at least one source is required to run.
4. Save the settings, then click **Run** (运行).

Configuration files are passed in their saved order with repeated `-config` arguments; the directory uses `-confdir`. Xray runs with the XTray executable directory as its working directory and with no console window.

Path changes apply to the next run or restart. Settings are stored as UTF-8 in `.\data\settings.ini`. Relative paths keep their original form, so portable moves do not rewrite them.

Logs stay in memory. XTray and Xray each retain the configured number of recent lines, initially 5000, with `[XTRAY]` / `[XRAY]` labels and distinct colors. The next run clears only the previous Xray output. Xray errors are displayed without changing the user's configuration or automatically restarting the process.

If Xray itself is configured to write log files, those files remain under Xray's own control; XTray captures its standard output and standard error.

## Local Xray files

The root-level `bin/` directory contains an external Xray executable and its accompanying files for local runtime testing. It is excluded from Git tracking and is not part of XTray's build dependencies or bundled EXE.

Xray-core and its configuration are provided and updated separately by the user. XTray does not generate or edit Xray configuration content.
