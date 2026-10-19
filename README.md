# XTray

A lightweight, portable Windows tray app for running [Xray-core](https://github.com/XTLS/Xray-core).

Start, stop and restart Xray, view its logs, and manage sign-in startup from a
simple window or the system tray. XTray runs Xray without a console window and
uses configuration files you maintain yourself.

XTray is an independent project. Xray-core is downloaded and updated separately.

## Purpose and audience

Xray-core's bundled Windows scripts are very basic. XTray aims to make running
Xray-core more convenient for Windows users.

XTray is designed to stay simple and lightweight, with a scope limited to basic
runtime management. Xray-core has many configuration options and parameters;
XTray's scope will not expand into a complex management tool to cover them.

**XTray is intended only for advanced users who understand Xray-core well and
can maintain their own Xray-core configurations.**

## Download

- [Download XTray](https://github.com/mclovin-2k/XTray/releases): choose **XTray.exe** under **Assets**.
- [Download Xray-core](https://github.com/XTLS/Xray-core/releases): choose a Windows package compatible with your system (x86 or x64).

The first XTray release has not been published yet. Official EXE downloads will
appear on the Releases page.

XTray is a single Windows x64 EXE that runs without installation or an extra
runtime.

## Quick start

1. Place `XTray.exe` in a folder where your Windows account can write files.
2. Extract the Xray-core package into a `bin` subfolder beside `XTray.exe`, keeping
   its accompanying files.
3. Open `XTray.exe`, then open **Settings**.
4. Confirm the Xray executable path (`.\bin\xray.exe` by default), add configuration
   files (one per line) and/or select one configuration directory, then click **Save**.
5. Click **Run** to start Xray.

Both configuration inputs start empty. Select at least one configuration file or
one configuration directory before running. Multiple configuration files and a
configuration directory can be used together.

You can choose files outside the XTray folder or enter relative paths. Relative
paths are resolved from the folder containing `XTray.exe`.

For help preparing your configuration, see the [Xray documentation](https://xtls.github.io/).
XTray does not generate or edit Xray configuration files.

## Everyday use

- **Run / Stop / Restart:** control Xray from the main window or the tray menu.
- **Status:** the GUI status icon and system tray icon are green while Xray is
  running and red when it is stopped or fails to start.
- **Show or hide the window:** click the tray icon to show the window. Closing
  the window keeps XTray and Xray running. Choose **Exit** in the tray menu to
  stop this copy's Xray and quit XTray.
- **Configuration shortcuts:** open a saved configuration file, its containing
  folder or the configured directory from the main window.
- **Logs:** choose all output, XTray only or Xray only. The trash-can button clears
  the selected source; the scroll-to-end button jumps to the latest output and
  resumes following. XTray keeps logs in memory and does not save historical logs.
- **Language and appearance:** Settings offers automatic, English or Chinese
  language selection, light/dark/system appearance, and a configurable log limit.

If Xray fails to start, read the displayed error and Xray output, correct the
executable path or your configuration, then try again.

## Start when you sign in

Enable **Start XTray and Xray when I sign in** in Settings and save. On your next
Windows sign-in, XTray shows its tray icon and starts Xray using the saved
configuration. This setting applies only to your Windows account.

Opening XTray manually shows the window and waits for you to click **Run**.

## Portable use

Settings are stored in the `data` folder beside `XTray.exe`. For a portable setup,
keep Xray and your configuration files inside the XTray folder and use relative
paths. Move the whole folder, including `data`, to keep your settings.

After moving the folder, open Settings from the new location and re-enable
sign-in startup if you use it.

For multiple independent instances, use separate folders and Xray configurations
with different listening ports. Each copy manages one Xray process.

## License

Copyright (C) 2026 XTray contributors.

XTray's original code and associated project files are licensed under the
[GNU General Public License, version 3 only](LICENSE)
(`SPDX-License-Identifier: GPL-3.0-only`).

When distributing a modified version of XTray, you must comply with GPLv3,
including providing the Corresponding Source under GPLv3. Commercial use and
distribution for a fee are permitted. The license provides no warranty.

Xray-core is supplied separately and remains subject to
[its own license](https://github.com/XTLS/Xray-core/blob/main/LICENSE).

The Xray-derived artwork in
[resources/icons/xtray.svg](resources/icons/xtray.svg), including icons generated
from it, is excluded from the GPL grant above. Its reuse and redistribution
permissions are being reviewed separately.
