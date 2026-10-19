# XTray

A lightweight, portable Windows tray application for managing Xray-core.

## Project status

Preparing **v0.1.0**. Xray process control, logs, settings, tray integration, directory instances, appearance/language selection, sign-in startup, and embedded icons are implemented. The user confirmed that the Build for commit 736b28e succeeded and reported that the restored native controls looked better in the desktop trial.

Version resources and version-tag publishing are now implemented. The user confirmed a successful Build for commit 9b8c121, covering the native tests, release simulations, executable version, and artifact checks. Final desktop regression checks and the first real tag-triggered publication remain pending. No v0.1.0 tag or GitHub Release has been created. See the [release checklist](docs/release-checklist.md) for the remaining acceptance record.

## Project documents

- [Requirements (Chinese)](docs/requirements.md)
- [Technical plan (Chinese)](docs/technical-plan.md)
- [Development progress (Chinese)](docs/development.md)
- [Release acceptance checklist (Chinese)](docs/release-checklist.md)
- [v0.1.0 release notes](docs/releases/v0.1.0.md)

## Build and download

The build environment is GitHub Actions; no local compiler installation is required.

1. Push source, resource, test, or build changes to this repository.
2. Open **Actions**, then **Build**. The workflow also supports **Run workflow**.
3. After a successful run, download **XTray-windows-x64** from **Artifacts**.
4. Extract and run `XTray.exe`.

The workflow builds an x64 Release executable with MSVC, C++20, and a static C++ runtime. It tests native window/log rendering, tray lifecycle with a controlled Shell backend, directory instances, and core behavior, then checks the executable's architecture, GUI subsystem, and runtime dependencies. Only `XTray.exe` is uploaded. Version metadata and SHA-256 are checked before upload; release-recovery tests use a simulated GitHub CLI without contacting GitHub.

Native tests use a dedicated test executable as a fake Xray child. The window test briefly shows and hides its own test window to verify restoration, without registering a real tray icon on the runner. They do not launch the external Xray executable, open a proxy listener, or access the network. Test executables and fixtures are not shipped.

## Versioned releases

The project version is defined once in CMakeLists.txt. For v0.1.0, Windows file properties show product version 0.1.0 and file version 0.1.0.0. A release tag must be exactly vMAJOR.MINOR.PATCH and match that project version; each component is limited to 0–65535 for the Windows version resource.

After the candidate build and [release checklist](docs/release-checklist.md) have been reviewed, the maintainer selects a tested commit and pushes its version tag. For example, replace the placeholder below with that commit:

```powershell
git tag -a v0.1.0 <validated-commit> -m "XTray v0.1.0"
git push origin v0.1.0
```

The tag push builds that tagged source, runs the same checks, and downloads the tested artifact into a separate publishing job. Only that job receives contents-write permission through GitHub's built-in GITHUB_TOKEN; no additional personal token is configured. It checks the tag's actual commit and the EXE's SHA-256, creates or reuses an unpublished draft, uploads XTray.exe, downloads the asset to verify its hash, then publishes.

An upload or verification failure leaves the draft unpublished. Use **Re-run failed jobs** on that tag's workflow after resolving the cause; retries can replace an incomplete draft asset. An already published version is rejected without replacement. Fixes to a published version use a new project version, release-notes file, and tag. Ordinary branch pushes and manual runs still produce development artifacts and do not publish a Release.

Release notes live in [docs/releases/v0.1.0.md](docs/releases/v0.1.0.md). The repository remains private, so its Releases follow the repository's access controls.

## Run Xray

1. Open **Settings** (设置).
2. Select the external Xray executable, or enter a path relative to `XTray.exe`. The default is `.\bin\xray.exe`.
3. Add configuration files, one per line, and/or choose one configuration directory. Both are empty by default; at least one source is required to run.
4. Save the settings, then click **Run** (运行).

Configuration files are passed in their saved order with repeated `-config` arguments; the directory uses `-confdir`. Xray runs with the XTray executable directory as its working directory and with no console window.

Path changes apply to the next run or restart. Settings are stored as UTF-8 in `.\data\settings.ini`. Relative paths keep their original form, so portable moves do not rewrite them.

Logs stay in memory. XTray and Xray each retain the configured number of recent lines, initially 5000, with bold `[XTRAY]` / `[XRAY]` labels and distinct orange/gray colors in the light UI, or light orange/light gray on a dark background in the dark UI. Wrapped lines align with the message body; this formatting does not add spaces or line breaks to copied text. The **Clear logs** trash-can icon, at the far right of the source-filter row, clears the selected source or both sources when **All** is selected. New output continues without restarting Xray. The next run clears only the previous Xray output. Xray errors are displayed without changing the user's configuration or automatically restarting the process.

The **Scroll to end** icon immediately to the left of Clear logs cancels the log selection, jumps to the end of the selected source, and resumes automatic following. Scrolling up or selecting text pauses following as before. Both buttons have localized hover tooltips, keyboard-accessible names, and theme-dependent icon colors.

If Xray itself is configured to write log files, those files remain under Xray's own control; XTray captures its standard output and standard error.

## Tray and directory instances

Manual launch displays the main window and a tray icon without starting Xray. The tray, GUI, title bar, and taskbar share the new XTray mark: green while running and red while stopped. The tooltip includes the instance directory to help distinguish portable copies.

- Close the main window to hide it while Xray and log capture continue.
- Click the tray icon once to show the existing window at its previous position.
- Right-click for **Show GUI**, **Run**, **Stop**, **Restart**, and **Exit**.
- Choose **Exit** to stop this instance's Xray and remove its tray icon. Logging off also ends the instance.
- While Settings or its file picker is open, process controls and Exit in the tray menu are disabled; Show GUI brings the existing dialog forward.

An Explorer restart re-registers the tray icon. If the tray is temporarily unavailable, XTray keeps or makes its window visible and retries; closing the window cannot leave the process inaccessible.

Each resolved executable directory allows one XTray process across Windows sessions. Reopening the same directory activates its existing window only for the same user and session, without starting another Xray. A directory occupied by another user/session or an unresponsive instance produces a message. Different directories remain independent.

Runtime instance identity uses the final directory path, a Windows SHA-256 hash, and a global named mutex. It creates no lock file or registration record. The added Advapi32/Bcrypt imports are Windows system libraries; users still need no separate runtime installation.

## Sign-in startup

In **Settings**, select **Start XTray and Xray when I sign in**, then save. This registers XTray for the current Windows user; it does not start or stop the currently managed Xray. On the next sign-in, XTray displays only its tray icon and runs Xray using the saved settings. Xray startup errors remain in the current in-memory logs with a red tray icon; click the icon to inspect them. Manual launch still shows the main window without automatically running Xray.

Clear the checkbox and save to remove this instance's registration. Cancel, Escape, and closing Settings discard the draft. Each time Settings opens, the checkbox reads this instance's current-user Windows Run registration rather than a saved boolean. Windows may delay startup, and system policy or an external disable in Windows startup-app controls can prevent a registered command from running; the checkbox does not override those controls.

A separate portable file, `data/startup.ini`, stores a GUID and the last registered executable path. The Windows entry is named `XTray-{GUID}` and uses the quoted absolute executable path with an internal `--logon` argument. Registration uses only the current user's Run key and needs no administrator prompt, service, or scheduled task.

After moving the entire directory, including `data`, open XTray manually from the new location. Startup shows as off and no registration is automatically repaired. Select it and save to update the same ID only when its old command matches the saved identity and the old executable is confirmed missing. A full copy whose original executable still exists gets a new ID when enabled, preserving the original registration. Unknown ownership, unreadable old paths, or damaged identity data produce an error instead of overwriting another entry. If the identity file was lost, XTray cannot identify or clean up its old registration.

Settings and startup changes are committed together during Save. Ordinary failures restore the previous entry and identity file; a failed recovery is reported explicitly. This is not a cross-file/registry transaction that guarantees recovery from power loss. Automated startup tests use an in-memory registry backend and build-directory fixtures, so they do not register anything for the test runner. Actual Windows sign-in still requires desktop acceptance.

## Icon resources

The XTray mark retains the supplied Xray four-point shape with an added tray base. The editable sources in `resources/icons/` have a transparent background and a white foreground; runtime tinting changes only the fill or stroke color. The EXE file icon is yellow, while runtime state icons are green/red. The log tools use a trash can and the approved short-lines/down-arrow/end-line drawing.

Windows SDK Direct2D SVG rendering supplies DPI-sized pixel icons to the native Windows icon APIs. A short-lived WARP device renders only when sizes or colors change; painting reuses the resulting handles. No external SVG file, graphics package, font icon library, or runtime installer is needed.

The cloud build first compiles a native icon helper and generates a nine-size yellow ICO from the same embedded SVG. Small frames use 32-bit icon bitmaps; the 256-pixel frame uses Windows' built-in PNG encoder. The helper, ICO, and SVG files are build inputs, not additional download requirements. Only XTray.exe is uploaded.

## Appearance

Settings offers **Follow system**, **Light**, and **Dark**, initially Follow system. Appearance and language share one row in the existing settings window. The appearance preference is stored in the portable settings file as the optional key `Theme=system|light|dark`; older settings without it remain valid and use Follow system.

Saving applies the appearance immediately. Cancel, Escape, and closing Settings discard the draft. Theme changes preserve Xray, log content, filtering, text selection, and reading position. Explicit Light/Dark choices override the Windows app mode; Follow system updates while XTray is running, including while Settings is open.

Buttons, checkboxes, combo boxes, input borders, and scrollbars use native Windows drawing. The light tray menu is also native; the dark tray menu retains custom drawing. Window/log backgrounds and source formatting use the selected palette. Log labels remain bold and orange/gray in light mode or light orange/light gray in dark mode; status icons remain green/red. Windows file pickers and Explorer use their own system appearance.

Theme detection uses Windows UISettings and its color-change event, with Windows setting-change messages as a fallback. If theme detection is unavailable, Follow system uses the light palette. Title-bar attributes unsupported by the current OS retain the Windows appearance. All drawing and detection use public Windows SDK APIs; no external theme engine, DLL package, or runtime installer is added.

## UI language

Settings offers **Automatic**, **English**, and **Chinese**, initially Automatic. Automatic uses the current Windows user's display language: Chinese UI languages use Simplified Chinese, and English or any other language uses English. Explicit choices override detection.

Saving applies the language immediately; Cancel keeps the active language. Both translations are embedded in the EXE. Existing XTray logs retain their original text, new management messages use the selected language, and Xray output is never translated. System file pickers and Windows error details keep the system language.

The optional INI key is `Language=auto`, `Language=en`, or `Language=zh`. Existing settings without this key remain valid and use Automatic.

## Local Xray files

The root-level `bin/` directory contains an external Xray executable and its accompanying files for local runtime testing. It is excluded from Git tracking and is not part of XTray's build dependencies or bundled EXE.

Xray-core and its configuration are provided and updated separately by the user. XTray does not generate or edit Xray configuration content.
