# XTray

[English](README.md) | 简体中文

一个轻量、便携的 Windows 托盘应用，用于运行 [Xray-core](https://github.com/XTLS/Xray-core)。

通过简单的窗口或系统托盘，即可启动、停止、重启 Xray，查看日志，以及设置登录自启。XTray 运行 Xray 时不会显示命令行窗口，使用的配置文件由你自行维护。

XTray 是一个独立项目。Xray-core 需要单独下载和更新。

## 项目目的与适用人群

Xray-core 自带的 Windows 运行脚本较为简陋。XTray 希望让 Windows 用户更方便地运行 Xray-core。

XTray 尽量保持简单、轻量，功能仅限于基础运行管理。Xray-core 的配置选项和参数很多，XTray 不会为了覆盖这些选项而扩展为复杂的管理工具。

**XTray 仅适合充分理解 Xray-core，并能够自行维护 Xray-core 配置的高级用户。**

## 便携使用

XTray 是一个 Windows x64 单 EXE 程序，无需安装，也无需额外安装运行库。

设置保存在 `XTray.exe` 同级的 `data` 文件夹中。若要便携使用，请将 Xray 及其配置文件放在 XTray 文件夹内，并使用相对路径。移动时请移动整个文件夹，包括 `data`，以保留设置。

移动文件夹后，如需继续使用登录自启，请从新位置打开设置，重新启用该选项。

如需运行多个独立实例，请使用不同的文件夹，并为各自的 Xray 配置不同的监听端口。每个 XTray 实例只管理一个 Xray 进程。

## 下载

- [下载 XTray](https://github.com/mclovin-2k/XTray/releases)：在 **Assets** 中选择 **XTray.exe**。
- [下载 Xray-core](https://github.com/XTLS/Xray-core/releases)：选择与你的系统兼容的 Windows 软件包（x86 或 x64）。

XTray 的首个正式版本尚未发布。正式 EXE 下载将提供在 Releases 页面。

## 快速开始

1. 将 `XTray.exe` 放在当前 Windows 账户具有写入权限的文件夹中。
2. 将 Xray-core 软件包解压到 `XTray.exe` 同级的 `bin` 子文件夹中，保留随附文件。
3. 打开 `XTray.exe`，然后打开**设置**。
4. 确认 Xray 程序路径（默认为 `.\bin\xray.exe`），添加配置文件（每行一个）和/或选择一个配置目录，然后点击**保存**。
5. 点击**运行**，启动 Xray。

配置文件和配置目录均无默认值。运行前必须至少选择一个配置文件或一个配置目录。可以同时使用多个配置文件和一个配置目录。

文件可以位于 XTray 文件夹之外，也可以输入相对路径。相对路径以 `XTray.exe` 所在的文件夹为基准。

如需了解如何编写配置，请参阅 [Xray 文档](https://xtls.github.io/)。XTray 不生成或编辑 Xray 配置文件。

## 日常使用

- **运行 / 停止 / 重启**：通过主窗口或托盘菜单控制 Xray。
- **状态**：Xray 运行时，主窗口状态图标和系统托盘图标显示绿色；停止或启动失败时显示红色。
- **显示或隐藏窗口**：单击托盘图标显示主窗口。关闭窗口后，XTray 和 Xray 仍会继续运行。在托盘菜单中选择**退出**，即可停止这份 XTray 管理的 Xray 进程并退出 XTray。
- **配置快捷操作**：从主窗口打开已保存的配置文件、文件所在目录或设置的配置目录。
- **日志**：可查看全部日志、仅 XTray 日志或仅 Xray 日志。垃圾桶按钮清空当前选定来源的日志；滚动至底部按钮跳到最新输出并恢复自动跟随。日志仅保存在内存中，不保存历史日志。
- **语言和外观**：设置中可选择自动、英文或中文，以及浅色、深色或跟随系统的外观，还可设置日志保留行数。

如果 Xray 启动失败，请查看界面显示的错误及 Xray 输出，修正程序路径或配置后重试。

## 登录自启

在设置中启用**登录后自动启动 XTray 和 Xray**并保存。下次登录 Windows 时，XTray 会显示托盘图标，并使用已保存的配置启动 Xray。此设置仅对当前 Windows 账户生效。

手动打开 XTray 会显示主窗口，点击**运行**后才会启动 Xray。

## 致谢

感谢 [Xray-core](https://github.com/XTLS/Xray-core) 项目及其贡献者开发、维护 Xray-core，并与社区分享这项成果。

## 许可证

Copyright (C) 2026 XTray contributors.

XTray 的原创代码及相关项目文件采用 [GNU 通用公共许可证第 3 版（仅限第 3 版）](LICENSE)（`SPDX-License-Identifier: GPL-3.0-only`）授权。

分发 XTray 的修改版本时，必须遵守 GPLv3，包括按 GPLv3 提供对应源码。允许商业使用和收费分发。许可证不提供任何担保。

Xray-core 单独提供，受[其自身许可证](https://github.com/XTLS/Xray-core/blob/main/LICENSE)约束。

源自 Xray 的图形资源 [resources/icons/xtray.svg](resources/icons/xtray.svg)，以及据此生成的图标，不包含在上述 GPL 授权范围内。其复用和再分发权限另行审查中。
