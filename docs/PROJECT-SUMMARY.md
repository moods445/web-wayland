# web-wayland 项目汇总

> 本文记录项目的**当前部署形态**与简短背景。部署、运维与排障的完整手册见
> [sunshine-moonlight.md](sunshine-moonlight.md)；遗留缺陷见
> [bugs/sunshine-nvidia-black-screen.md](bugs/sunshine-nvidia-black-screen.md)。

## 当前形态（2026-11-21 起）

本机是 **Moonlight + Sunshine 串流主机**：Moonlight 作客户端，Sunshine 作服务端；
桌面是 Hyprland/Wayland 会话，只存在于 headless 虚拟输出上。

- **开机自启**：`ww-session`（Hyprland 桌面，抓帧目标）+ `sunshine-wayland`（Sunshine 用户单元），
  两个都是 systemd 用户单元，`loginctl enable-linger` 后开机即起、无需登录。
- **X11 已全面关闭**：`sddm` / `x11-common` 已 mask，Hyprland 的 `xwayland.enabled = false`，
  LXQt（X11）自启单元已 mask，user manager 里残留的 `DISPLAY` 已清除。
- **默认档位**：集显 `/dev/dri/card1` + `1920×1080@60` + scale=1（逻辑 1080p）；
  「字小」用 `scripts/font-scale.sh` 只放大字号解决，不加 scale（避免发虚）。
- **抓帧**：Sunshine 按合成器 `zwp_linux_dmabuf` feedback 主设备自动选 `vaapi`/`nvenc`，
  抓 headless 输出；编码与抓帧卡必须同卡。
- **健康自检**：`scripts/sunshine-wayland.sh doctor` 一次看完会话/抓帧卡/编码器/服务/错误计数。

冷启动已用「停掉全部单元 → 一次性 start」验证三轮：单实例、无嵌套、非 safe-mode、
抓帧失败 0、X11 进程 0。细节见 [sunshine-moonlight.md §1.2](sunshine-moonlight.md#12-开机自启重启后能直接用-moonlight-连)。

## 简短背景

项目最初做的是**浏览器里的 Hyprland 桌面**：服务端常驻 Wayland 会话，整屏经 GPU 编码后通过
WebTransport(UDP/QUIC) 下发，浏览器用 WebCodecs 解码；自研了抓帧编码（`wwcap`，C）、网关
（`wwgw`，Rust）与 Web 前端（`web`，JS），并做过原生客户端实验（`native`）。

在追求 4K60 与输入延迟的过程中，逐步确认了几个关键结论：自建链路的 4K60 需要独显零拷贝
路径，而独显上对虚拟输出的 Wayland 抓帧存在上游缺陷；浏览器链路的光标延迟受抓帧节奏锁死。
2026-11-21 起改走成熟的 Moonlight + Sunshine 方案，自建链路停用，并最终在本分支**整体删除**
（`capture/` / `gateway/` / `web/` / `native/` 及配套服务、测试与历史文档），只保留当前形态。

自建链路的实测结论与踩坑记录曾整理在 `docs/streaming-conclusions.md`、`docs/architecture.md`
等文档，现已随功能删除；其中仍对当前部署有参考价值的内容已并入 sunshine-moonlight.md。

## 待决策：`proto/` 目录的取舍

`proto/` 下有 3 个 Wayland 协议 XML（`wlr-screencopy`、`wlr-virtual-pointer`、
`virtual-keyboard`）和 `wire.h`（自建链路的 24 字节分片传输头）。它们唯一的使用者是已删除的
`capture/`，当前 Sunshine 链路不依赖这些文件。本次未预设删法，保留两种取舍供后续决定：

| 方案 | 做法 | 取舍 |
|---|---|---|
| A（更彻底）| 删除整个 `proto/` 目录 | 仓库不再留死代码；彻底与旧链路切割（可用 git 历史找回）|
| B（留参考）| 保留 3 个 `.xml` 作为协议资料，只删 `wire.h`（纯传输协议，已无消费者）| 保留可读的协议定义供将来参考，代价是留少量不再被构建的文件 |

## 相关文件

- [README.md](../README.md)：上手与快速运维
- [sunshine-moonlight.md](sunshine-moonlight.md)：部署 / 运维 / 排障主文档
- [bugs/sunshine-nvidia-black-screen.md](bugs/sunshine-nvidia-black-screen.md)：独显虚拟输出抓帧黑屏（唯一遗留缺陷）
- 项目规则见 [AGENTS.md](../AGENTS.md)
