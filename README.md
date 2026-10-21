# XTray

English | [简体中文](README.zh-CN.md)

XTray is a lightweight, portable Windows tray app for
[Xray-core](https://github.com/XTLS/Xray-core). It provides a more convenient
alternative to Xray-core's basic bundled Windows scripts: start, stop and
restart Xray, view logs and manage sign-in startup from a simple window or the
system tray, without a console window.

XTray focuses on basic runtime management. Xray-core has many configuration
options and parameters; XTray will not expand into a complex management tool
to cover them.

**XTray is intended only for advanced users who understand Xray-core well and
can maintain their own Xray-core configurations.**

XTray is an independent project. Xray-core is downloaded and updated separately.

## Portable use

XTray is a single Windows x64 EXE that runs without installation or an extra
runtime.

Settings are stored in the `data` folder beside `XTray.exe`. For a portable setup,
keep Xray and your configuration files inside the XTray folder and use relative
paths. Move the whole folder, including `data`, to keep your settings.

After moving the folder, open Settings from the new location and re-enable
sign-in startup if you use it.

For multiple independent instances, use separate folders and Xray configurations
with different listening ports. Each copy manages one Xray process.

## Download

- [Download XTray](https://github.com/mclovin-2k/XTray/releases): choose **XTray.exe** under **Assets**.
- [Download Xray-core](https://github.com/XTLS/Xray-core/releases): choose a Windows package compatible with your system (x86 or x64).

## Quick start

1. Place `XTray.exe` in a folder where your Windows account can write files.
2. Extract the Xray-core package into a `bin` subfolder beside `XTray.exe`, keeping
   its accompanying files.
3. Open `XTray.exe`, then choose **File → Settings**.
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

### Why allow both configuration files and a directory?

Xray natively supports multiple `-config` arguments together with `-confdir`.
XTray exposes these options so you can organize your configuration into reusable
parts and choose how to load them.

For example, keep shared logging, DNS and routing settings in a directory, then
select separate files for the inbound or outbound connections you want to use.
This lets you reuse common settings and maintain each part independently,
without duplicating a complete configuration or merging files by hand.

XTray passes the selected paths to Xray, which loads and merges the configuration
according to its [multi-file configuration rules](https://xtls.github.io/en/config/features/multiple.html).
You can use a single file, multiple files, one directory, or files and a directory
together, depending on your setup.

## Everyday use

- **Run / Stop / Restart:** control Xray from the main window or the tray menu.
- **Status:** the GUI status icon and system tray icon are green while Xray is
  running and red when it is stopped or fails to start.
- **Show or hide the window:** click the tray icon to show the window. Closing
  the window keeps XTray and Xray running. Choose **File → Exit** or **Exit** in the tray menu to
  stop this copy's Xray and quit XTray.
- **Configuration shortcuts:** open a saved configuration file, its containing
  folder or the configured directory from the main window.
- **Logs:** choose all output, XTray only or Xray only. The buttons, from left to
  right, copy all retained logs for the selected source, clear that source,
  scroll to the end, and toggle word wrap. The scroll-to-end button stays
  pressed while following new output; scrolling up pauses following, and
  scrolling back to the bottom or clicking the button resumes it. Word wrap is
  on by default, and its setting is remembered. Copying includes off-screen
  logs and preserves line breaks regardless of wrapping.
  XTray keeps logs in memory and does not save historical logs.
  To save Xray logs to files, use [Xray's own logging configuration](https://xtls.github.io/config/log.html).
- **Language and appearance:** Settings offers automatic, English or Chinese
  language selection, light/dark/system appearance, and a configurable log limit.

If Xray fails to start, read the displayed error and Xray output, correct the
executable path or your configuration, then try again.

## Xray tools

The **Xray Tools** menu uses the Xray executable and settings saved in
**File → Settings**:

- **Check Xray configuration:** runs Xray's configuration check and shows the
  original output and exit code. It does not start the proxy server.
- **Preview Xray configuration:** shows the merged configuration from the
  saved sources currently on disk. This is a read-only preview, not the
  running process's in-memory configuration.
- **Show Xray version:** shows the executable path and Xray's version/build
  information. Configuration files are not required.

Tools run independently of the managed proxy, with one tool task at a time.
The result window has **Copy all** and a word-wrap toggle above the output.
Word wrap starts off for each new result window; its setting is kept only until
that window closes. With wrapping off, horizontal scrolling is available.
Copying always includes the full result with its original line breaks.
The full output is separate from the runtime log limit. Closing the result
window cancels an unfinished task. Results are kept only in memory until that
window is closed.

## Start when you sign in

Enable **Start XTray and Xray when I sign in** in Settings and save. On your next
Windows sign-in, XTray shows its tray icon and starts Xray using the saved
configuration. This setting applies only to your Windows account.

Opening XTray manually shows the window and waits for you to click **Run**.

## Acknowledgements

Thanks to the [Xray-core](https://github.com/XTLS/Xray-core) project and its
contributors for developing, maintaining and sharing Xray-core with the
community.

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
