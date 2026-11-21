# Sunshine + Moonlight 部署（2026-11-20，2026-11-21 起为本机唯一串流路径）

本机**只做 Moonlight + Sunshine 串流主机**，桌面是 Hyprland/Wayland 会话。
早期自建的浏览器链路已整体删除，仓库只保留当前这套方案。
本文记录怎么起、为什么这么起、实测到什么程度、坏了怎么查。

结论先行：

- **部署形态**：`ww-session`（Hyprland 桌面，保留）+ `sunshine-wayland`（Sunshine 用户单元）。
  两个都是 systemd **用户单元**，`loginctl enable-linger` 后开机即起、无需登录桌面。
- **Intel 路径可用**：`capture=wlr` + `h264_vaapi`，抓 headless 输出，1080p60 正常；
- **抓帧卡必须与编码器同卡**：脚本直接问合成器的 `zwp_linux_dmabuf` feedback 主设备来决定
  `vaapi`/`nvenc`，不再依赖会过期的 `~/.config/hypr/display.state`（跨 GPU 抓帧每帧
  `Frame capture failed`，客户端全黑）；
- **NVIDIA 路径对虚拟输出不可用**：Hyprland/Aquamarine 在 nvidia 上导入 screencopy dmabuf 会
  `eglCreateImageKHR ... EGL_BAD_PARAMETER`（每帧 `Frame capture failed` → 客户端黑屏），换 `nvenc` 也没用；
  `capture=kms` 同样不行——KMS 只认真实 CRTC，而独显侧唯一的连接器 `HDMI-A-1` 未接线，
  虚拟输出的连接器类型是 `HEADLESS`，Sunshine 直接报 `Unknown Monitor connector type [HEADLESS]`。
  所以脚本在「独显 + wlr」组合下**直接拒绝启动并打印原因**，不再让用户对着黑屏猜
  （要强行试：`SUN_FORCE_NVIDIA=1`）；
- **4K60 需要真实输出**：给独显 HDMI 口插 **EDID 诱骗器（dummy plug）**，KMS 才有可抓的 CRTC；
  否则本机最高就是集显 1080p60（4K 合成 ~20fps）；
- **只用虚拟输出**：物理屏 eDP-1 在配置里 `disable`（黑屏、不参与布局），桌面只存在于 headless 输出；
- **切换靠脚本**：GPU（集显/独显）与分辨率/缩放统一用 `scripts/switch-display.sh`。

相关文档：项目当前形态与简短背景见 [PROJECT-SUMMARY.md](PROJECT-SUMMARY.md)；
NVIDIA 黑屏的完整排查过程见 [bugs/sunshine-nvidia-black-screen.md](bugs/sunshine-nvidia-black-screen.md)。

## 1. 前置条件

| 项目 | 要求 |
|---|---|
| GPU 路径 | 集显 `/dev/dri/card1`(i915)：可跑 Sunshine；独显 `/dev/dri/card0`(nvidia)：只能合成，Sunshine 抓不到虚拟输出（见 §7） |
| 桌面 | Hyprland 已启动，且存在一个 headless 输出（脚本会找/建） |
| 物理屏 | `hyprland.conf` 里 `monitor = eDP-1, disable`：物理面板黑屏、不参与布局，桌面只存在于 headless 输出 |
| X11 | **纯 Wayland，不再用 X11**：`sddm` / `x11-common` 已 mask，Hyprland 的 `xwayland.enabled = false`，LXQt 的 X11 自启单元已 mask（见 §1.1） |
| Sunshine | `sunshine` 在 PATH（本机为 Debian trixie 包，2026.914.233613） |
| 用户组 | `aio` 需在 `input` 组（`usermod -aG input aio`），否则键鼠注入不生效（脚本会退化成 `sg input` 启动） |
| 发行版单元 | `app-dev.lizardbyte.app.Sunshine.service` 必须被 mask 掉（见 §3.3），它会用 X11 环境抢端口 |

### 1.1 纯 Wayland（2026-11-21 起，X11 全部关闭）

本机只跑 Wayland，X11 相关的启动项全部关掉。`scripts/install-sunshine-host.sh` 会做这些事：

| 对象 | 处理 | 为什么 |
|---|---|---|
| `sddm.service` / `x11-common.service` | `mask` | X11 显示管理器；Xorg 会抢 DRM master，Wayland 合成器拿不到 modeset |
| `hyprland.conf` 的 `xwayland { enabled }` | `false` | 不再需要 `Xwayland`：没有 X11 客户端，也没有 `DISPLAY` |
| `app-lxqt-*@autostart.service` | `mask` | `/etc/xdg/autostart/lxqt-*.desktop` 是 LXQt（X11 桌面）的自启项 |
| `~/.config/autostart/screen-blank-1min.desktop` | 挪到 `autostart/disabled/` | 内容就是 `xset s / xset dpms`，纯 X11 |
| user manager 的 `DISPLAY` / `XAUTHORITY` | `systemctl --user unset-environment` | 残留的 `DISPLAY=:0` 会被**所有**用户单元继承——发行版 Sunshine 单元就是在 X11 环境里跑才黑屏的 |

关闭后的表现：

- `pgrep Xwayland` 为空，`/tmp/.X11-unix` 为空，`echo $DISPLAY` 为空；
- 只跑 Wayland 原生程序（foot / waybar / swaybg / fuzzel / nwg-displays 都是原生）；
- **副作用**：依赖 X11 的程序（X11 版 Chrome/xdotool/xset、部分输入法）不可用。
  真要用时临时把 `xwayland { enabled = true }` 改回来并重启会话（`systemctl --user restart ww-session`）。

### 1.2 开机自启：重启后能直接用 Moonlight 连

能。开机后不需要登录、不需要手工跑脚本，链路是：

```
systemd user manager（linger=yes，无人登录也拉起）
  └─ ww-session.service            → start-hyprland --config ~/.config/hypr/hyprland.conf
       └─ sunshine-wayland.service （Wants/After/PartOf=ww-session）
            → scripts/sunshine-wayland.sh run
                 ├─ 等 Wayland socket + Hyprland（最多 120s，等不到就退出让 systemd 重试）
                 ├─ 建 headless 输出（按 monitors.conf 的档位创建，见 §1.3）
                 ├─ 探测抓帧卡 → 写 sunshine.conf（capture/encoder/output_name）
                 └─ exec sunshine（env -u DISPLAY，带 WAYLAND_DISPLAY=wayland-N）
```

实测（2026-11-21，用「停掉全部单元 → 一次性 start」模拟冷启动，连跑 3 轮）：

| 检查 | 结果 |
|---|---|
| `systemctl --user is-active ww-session sunshine-wayland` | 两个都 active（约 30s 内就绪） |
| Hyprland 实例 | 1 个，非 safe-mode、无嵌套 |
| 虚拟输出 | `HEADLESS-2` 1920x1080@60 **scale=1** → 逻辑 1920×1080（按 `monitors.conf` 创建；字号靠 `font-scale.sh`） |
| Sunshine 端口 | 47984 / 47989 / 47990 全部监听 |
| Sunshine 环境 | `WAYLAND_DISPLAY=wayland-1`，**无 `DISPLAY`** |
| 抓帧日志 | `[wayland] Name: HEADLESS-2` + `Screencasting with Wayland's protocol`，失败帧 0 |
| X11 | 无 Xorg / 无 Xwayland |
| 桌面 | `grim` 截图 ~175KB（非全黑），waybar / swaybg 各 1 个 |

客户端侧还有两件服务端代替不了的事：

1. Moonlight 里**添加过一次主机**（IP `192.168.0.109`，mDNS 不一定通）；
2. **配对**：`https://192.168.0.109:47990` 登录 → 输入 Moonlight 显示的四位 PIN。
   配对结果写在 `~/.config/sunshine/sunshine_state.json` 的 `named_devices`。
   本机当前 `named_devices: 0`（还没有配对设备），所以重启后第一次连之前**先配一次对**，
   之后就不用再配。

`sunshine_name = debian-server-x11` 是配置里遗留的名字，只影响 Moonlight 列表里的显示名，
不影响连接（想改就改 `sunshine.conf`）。

### 1.3 分辨率：虚拟输出的尺寸只能在创建时定

Aquamarine 的 headless 输出一旦创建，`availableModes` 就只剩创建时那一档：
`hyprctl keyword monitor "HEADLESS-2,3840x2160@60,0x0,2"` 会返回 `ok` 但尺寸不变（实测）。
所以脚本在**创建输出之前**先把候选名字（`HEADLESS-1…16`）的 monitor 规则都注册好，
创建出来的那个输出直接就是目标档位。

切换分辨率/缩放：

```bash
scripts/sunshine-wayland.sh mode 3840x2160 2    # 等价于 scripts/switch-display.sh res 3840x2160 2
scripts/sunshine-wayland.sh mode 1920x1080 1    # 当前默认（物理 1080p、逻辑 1080p）
```

`mode` 的动作是「新建目标尺寸的输出 → 删掉旧输出 → 重启 `sunshine-wayland`」，会话不用重启，
但**不要手工 `hyprctl output remove` 掉最后一个输出**——Hyprland 会直接退出
（watchdog 再拉起时是 safe-mode，物理屏会被点亮）。

集显上 4K 合成 ~20fps、1080p ~60fps；4K60 只有独显 + 真 CRTC 才可能（见 §7）。

> **觉得字小怎么办**：不要加 scale（2x 会把逻辑分辨率砍半，分数缩放会让文字发虚），
> 用字号方案 —— `scripts/font-scale.sh 1.3`，细节与原理见 §12。

## 2. 快速开始

```bash
# 0) 一键切到「只做 Moonlight/Sunshine 主机」：
#    mask 发行版 Sunshine 单元、安装并启用下面两个用户单元
scripts/install-sunshine-host.sh                  # 不动正在跑的会话
scripts/install-sunshine-host.sh --restart-session # 连桌面一起重启（应用新环境变量，窗口会关掉）

# 1) 日常运维
systemctl --user status ww-session sunshine-wayland
scripts/sunshine-wayland.sh status
scripts/sunshine-wayland.sh doctor     # 黑屏时先跑这个：一次看完会话/抓帧卡/编码器/服务/错误计数

# 2) 手工起停（systemd 之外，调试用）
scripts/sunshine-wayland.sh start
scripts/sunshine-wayland.sh stop

# 3) 切 GPU / 分辨率（写 monitors.conf，改 GPU 会重启会话）
scripts/switch-display.sh                       # 看当前档位
scripts/switch-display.sh nvidia 3840x2160 2    # 独显 + 4K@2x（Sunshine 抓不到，脚本会拒绝启动）
scripts/switch-display.sh intel 1920x1080 1     # 集显 + 1080p（当前部署，字号见 §12）
scripts/switch-display.sh res 2560x1440 1       # 只改分辨率/缩放，不重启会话（内部走 mode）
scripts/sunshine-wayland.sh mode 3840x2160 2    # 同上，直接改虚拟输出档位
```

启动成功后日志会给出（`journalctl --user -u sunshine-wayland -f`）：

```
[sun] 会话: wayland-1 / Hyprland <sig>  输出: HEADLESS-2
[sun] 抓帧卡: /dev/dri/card1（driver=i915）→ 编码器 vaapi，抓帧方式 wlr
[sun] 配置: written
[sun] exec sunshine（vaapi / wlr / HEADLESS-2）
```

Sunshine 自己会在启动时完成抓帧后端初始化，日志里应看到：

```
Info: [wayland] Name: HEADLESS-2
Info: [wlgrab] Selected monitor [] for streaming
Info: Screencasting with Wayland's protocol
```

> `Selected monitor []` 是这版 Sunshine 的正常输出（它打印的是另一个字段），
> 关键看上下文的 `[wayland] Name` 与实际输出一致、且没有 `Frame capture failed`。

## 3. 脚本做了什么

### 3.1 `scripts/sunshine-wayland.sh`

按顺序做四件事（`start` 手工起、`run` 给 systemd 单元用）：

1. **找/建 headless 输出**：遍历 `$XDG_RUNTIME_DIR/hypr/*/` 找存活的 Hyprland 实例，
   取已有 `HEADLESS-*` 输出，没有就 `hyprctl output create headless`；
   再按 `~/.config/hypr/monitors.conf` 的档位 `hyprctl keyword monitor` 设一次
   （会话重启后 headless 名字会从 `HEADLESS-2` 变成 `HEADLESS-1/3`，脚本会把
   `monitors.conf` 里的名字一起改掉，不会出现「配置指向一个不存在的输出」）。
2. **探测抓帧卡 → 选编码器**：读 `wayland-info` 里 `zwp_linux_dmabuf_v1` 的
   `main device`（合成器实际渲染的卡），再由 `/sys/class/drm/cardN/device/driver` 认驱动：
   `nvidia` → `nvenc`，`i915/xe/amdgpu/radeon` → `vaapi`。
   **不再信任 `~/.config/hypr/display.state`**（那只是「上次切到哪张」，会话被手工重启后就过期了；
   编码器与抓帧卡不一致 = 跨 GPU 抓帧 = 每帧 `Frame capture failed`）。
   独显 + `wlr` 组合直接拒绝启动（见 §7）。
3. **写配置**（内容有变化时才备份成 `sunshine.conf.bak`；去掉 X11 的 `global_prep_cmd`）：

   ```ini
   capture = wlr
   encoder = vaapi          # 按上一步自动得出
   output_name = HEADLESS-2 # 按实际输出自动得出
   min_log_level = info     # 排查时用 SUN_LOG_LEVEL=debug 覆盖
   ```

4. **起进程**：
   - `start`：`setsid` 后台起（只加 `nohup` 时，启动它的终端/IDE 会话一结束 Sunshine 就被收走，
     表现是「刚起来就连不上」），然后等 47984 监听、扫日志里的错误再报结论；
   - `run`：前台 `exec sunshine`，由 `sunshine-wayland.service` 托管（崩溃自动重启）；
   - 环境：`XDG_RUNTIME_DIR` + `WAYLAND_DISPLAY=wayland-N` + `XDG_SESSION_TYPE=wayland`，
     并显式 `env -u DISPLAY -u XDG_SESSION_TYPE`（有 `DISPLAY` 时 Sunshine 会走 X11 分支）；
   - `input` 组：已在组里直接起，否则 `sg input -c` 借组。

### 3.2 单实例与会话自愈

- `ww-session` 启动前会 `pkill -x Hyprland / start-hyprland`，清掉**不属于本单元**的残留
  实例。手工起的 Hyprland 会占住 DRM master 与 `wayland-1.lock`，让单元拉起的实例退化成
  嵌套会话（多一个 `WAYLAND-N` 伪输出、画面合成两遍、鼠标位移减半）。
  **不要手工起 Hyprland**；调试完用 `systemctl --user restart ww-session` 收尾。
- Hyprland 崩溃后 `start-hyprland` 会用 `recoverycfg.conf` 拉起 **safe-mode** 会话
  （没有 `monitor = eDP-1, disable`、没有 waybar/壁纸/键位，物理屏会被点亮）。
  `doctor` 会检测并提示；恢复：`systemctl --user restart ww-session`。
- 脚本只认 `HEADLESS*` 输出，**不会**回退到 eDP-1 / HDMI-A-1：safe-mode 下 eDP-1 是启用状态，
  一旦回退就会抓到物理屏，还会把 `monitors.conf` 改写成物理屏的名字。
- 发现多个 `HEADLESS-*` 时脚本会删掉多余的（同坐标重叠会让 screencopy 抓到错层 → 黑屏）。

### 3.3 开机自启：为什么必须 mask 发行版单元

Debian 包自带的是 **user 单元** `/usr/lib/systemd/user/app-dev.lizardbyte.app.Sunshine.service`，
`enable` 之后由 user manager 直接拉起，环境里只有 `DISPLAY=:0` + `XDG_SESSION_TYPE=x11`，
**没有 `WAYLAND_DISPLAY`**。`capture = wlr` 拿不到 Wayland socket → 抓帧初始化失败 →
Moonlight 连上就黑屏（2026-11-21 现场就是这个状态，`sunshine.log` 停在版本号之后、
既没有 `[wayland] Name:` 也没有 `Screencasting`）。

所以 `scripts/install-sunshine-host.sh` 会：

| 动作 | 对象 | 原因 |
|---|---|---|
| `disable --now` + `mask` | `app-dev.lizardbyte.app.Sunshine.service`（user） | 环境错，且会抢先占住 47984-47990 |
| 安装 + `enable` | `ww-session` | Hyprland 桌面（抓帧目标），单元里不带透明光标环境变量 |
| 安装 + `enable` | `sunshine-wayland` | 本项目的 Sunshine 单元，`PartOf=ww-session`，会话重建跟着重建 |

`loginctl enable-linger` 保持开启，否则开机没人登录时用户单元不会启动。

## 4. 手动配置（不用脚本时）

`~/.config/sunshine/sunshine.conf` 至少要有：

```ini
capture = wlr          # 必须是 wlr；写 wlgrab 会 Unable to initialize capture method
encoder = vaapi        # 集显 vaapi / 独显 nvenc（必须与抓帧卡同卡）
output_name = HEADLESS-2
```

然后带着 Wayland 环境启动：

```bash
env -u DISPLAY XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 \
    XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Hyprland \
    setsid sunshine >>/tmp/sunshine-wayland.log 2>&1 &
```

测量帧延迟时临时在配置里加 `min_log_level = debug`，日志会出现
`Frame processing latency (min/max/avg)`；测完记得删掉（debug 日志量很大）。

## 5. 客户端连接

服务端常驻监听（实测）：

| 端口 | 用途 |
|---|---|
| TCP 47984 | HTTPS（配对/控制） |
| TCP 47989 | HTTP |
| TCP 47990 | Web UI 配置界面 |
| TCP 48010 | RTSP |
| UDP 47998–48010 | 视频/音频/控制/麦克风（串流期间建立） |

**Mac / 其他设备**：

1. Moonlight 里「添加主机」，填本机局域网 IP（本机 `192.168.0.109`）。
   > 本机 `avahi-daemon` **未运行**，所以 Moonlight 的 mDNS 自动发现列表里不会出现这台机器，
   > 按 IP 手动添加即可（想要自动发现：`sudo systemctl enable --now avahi-daemon`）。
   > 主机加过一次就会保存在客户端，重启服务器后直接点它连。
2. 客户端会显示 4 位 PIN，浏览器打开 `https://<IP>:47990`（自签证书，需手动信任），
   登录后在 PIN 输入框提交完成配对。首次用可在同一页面设置 Web UI 账号密码。
3. 选择 `Desktop` 开始串流。应用列表来自 `~/.config/sunshine/apps.json`
   （`Desktop` / `Low Res Desktop` / `Steam Big Picture`）。

**同机自测**：Moonlight 的 Linux 客户端只有 xcb 插件，**需要 Xwayland**；本机 `xwayland` 已关闭，
所以同机自测已不可用，只能用另一台设备（Mac / Win / 手机）连。临时要同机测就把
`config/hypr/hyprland.conf` 的 `xwayland { enabled = true }` 打开并重启会话。

```bash
export DISPLAY=:1 QT_QPA_PLATFORM=xcb
./Moonlight.AppImage list 127.0.0.1
./Moonlight.AppImage stream 127.0.0.1 "Desktop" \
  --1080 --fps 60 --bitrate 20000 --display-mode windowed
```

配对状态存在 `~/.config/sunshine/sunshine_state.json` 的 `named_devices` 里，
删掉对应条目即可让某个客户端重新配对。

## 6. 实测数据（Intel / HEADLESS-2 1920x1080@60 档）

> 本机当前 `monitors.conf` 配的是 `1920x1080@60` + **scale 1**（逻辑 1920×1080，2026-11-21 起的默认档位；字号方案见 §12），
> Sunshine 按 1080p 抓帧编码；下表就是这一档的基线数据。

| 项目 | 实测值 |
|---|---|
| 抓帧 | `wlr`（`zwlr_screencopy_manager_v1` v3），`[wlgrab] Monitor 1 is HEADLESS-2` |
| 编码 | `h264_vaapi`（Intel iHD 25.2.3），RC framerate 60/1 |
| 码率 | 14,988,000 bps（客户端请求 20 Mbps，服务端折算） |
| 色彩 | SDR **Rec.601** / 8-bit / JPEG range |
| 服务端帧处理延迟 | 4 分钟稳态 avg **9.7~10.1ms**；20s 窗口 min 6.9ms / max 27.2ms / avg **9.17ms** |
| 客户端 | Moonlight 6.2.0：`Video stream is 1920x1080x60 (format 0x1)`、`Test decode successful` |
| 会话 | `CLIENT CONNECTED` → `CLIENT DISCONNECTED` 正常，期间无 `Frame capture failed` |

延迟是「抓帧 + 编码」，不含网络与客户端解码。
串流期间 Sunshine 会把默认 sink 临时切到 `sink-sunshine-stereo`，断开后恢复为 `ww-desk`（日志可见）。

## 7. 已知限制

1. **4K60 在本机不可得**。Intel 上 Hyprland 合成 4K 只有 20~22fps；独显虽然能合成 4K，
   但 Sunshine 抓不到虚拟输出（`wlr` 的 dmabuf 导出失败，`kms` 又不认 `HEADLESS` 连接器）。
   要 4K60 就得给独显 HDMI 口插 EDID 诱骗器，把输出变成真实 CRTC 后才走 `capture=kms`。
   完整排查见 [bugs/sunshine-nvidia-black-screen.md](bugs/sunshine-nvidia-black-screen.md)。
2. **画质受编码路径限制**：Sunshine 的 VAAPI 路径只有 Rec.601 / JPEG range / 4:2:0，
   没有 4:4:4 选项，桌面文字/纯色块的锐度因此受限。
3. **输出名会变**：Hyprland 重启后 headless 输出会重建（`HEADLESS-N` 可能变）。
   脚本启动时会按实际名字重放 `monitors.conf` 的档位并把名字改写进去，一般不用管；
   只有在 systemd 之外手工起 Sunshine 时才需要留意 `output_name`。
4. **物理屏被主动禁用**：`monitor = eDP-1, disable`，面板保持黑屏，桌面只存在于 headless 输出
   （本地看不到，只能通过 Moonlight 看）。这是设计选择而不是故障：以前 eDP-1 停在 0x0 是因为
   Xorg/sddm 占着 DRM master 拿不到 modeset，现在已停用 sddm，改为显式禁用。
   想在本地临时看桌面：`hyprctl keyword monitor "eDP-1,1920x1080@60,0x0,1"`，用完再 `disable`。
5. **光标在视频里**：Sunshine 的 wlr 抓帧带 `overlay_cursor`，指针是**合成器画进画面**的，
   所以会话不能用已废弃的全透明光标主题 `ww-invisible`（`ww-session.service` 不设 `XCURSOR_THEME`，
   `hyprland.conf` 里 `cursor:enable_hyprcursor = true`）。
   验证方法：`hyprctl dispatch movecursor 400 300` → `grim` → 再 `movecursor 1200 800` → `grim`，
   两张图在光标位置有差异即说明光标被画进了画面。

## 8. 故障排查

**第一件事永远是 `scripts/sunshine-wayland.sh doctor`**：它一次打印会话/输出/dpms、dmabuf 主设备与驱动、
`sunshine.conf`、进程环境（有没有 `WAYLAND_DISPLAY`、有没有残留 `DISPLAY`）、抓帧失败帧数、
Hyprland 侧 `EGL_BAD_PARAMETER` 计数、相关服务状态，最后给一句结论。

| 现象 | 原因 | 处理 |
|---|---|---|
| 黑屏且 Sunshine 是「发行版单元」起的（进程环境 `DISPLAY=:0`、无 `WAYLAND_DISPLAY`） | 发行版 user 单元在 X11 环境里启动，wlr 抓帧没有 Wayland socket | `scripts/install-sunshine-host.sh`（会 disable + mask 它） |
| 刚启动就连不上，日志停在版本号之后 | 进程被启动它的 shell 收走 | 必须 `setsid`（脚本已处理） |
| 键鼠完全没反应 | `/dev/uinput` 是 `root:input 0660`，用户不在 `input` 组 | `usermod -aG input aio`；重新登录前用 `sg input -c` 启动 |
| 屏幕黑屏/无画面 | `output_name` 与实际 headless 名不一致，或输出被销毁 | `systemctl --user restart sunshine-wayland`（脚本会重新探测并重写配置） |
| 串流里看不到鼠标指针 | 会话错误地设了已废弃的透明光标 `XCURSOR_THEME=ww-invisible` | `scripts/install-sunshine-host.sh --restart-session` |
| 每帧 `Frame capture failed` 且 `doctor` 显示配置 encoder 与抓帧卡不同 | 编码器跨 GPU（例如 display.state 过期导致集显会话用 nvenc） | 重启服务让脚本按实际会话重写配置 |
| 连上但全黑，日志里 `Selected monitor []` 是正常输出 | headless 与 eDP-1 同坐标重叠（都写 `0x0`），screencopy 抓到 eDP-1 那一层 | 把 headless 移到另一个输出右侧；`hyprctl -j monitors` 确认两个输出的 `x` 不同 |
| 多个 `HEADLESS-*` 都不存在，`hyprctl clients` 为空 | Hyprland 崩溃后 watchdog 拉起 **safe-mode** 实例，用的是 `recoverycfg.conf`（无 output 规则、无壁纸） | `systemctl --user restart ww-session` 重新起正常会话 |
| `atomic drm request ... Permission denied`，eDP-1 是 `0x0` | Xorg/sddm 占着 DRM master | `sudo systemctl stop sddm`（本机已 disable），再重启 Hyprland |
| 重启后连不上，`doctor` 提示会话是 **safe-mode**（物理屏亮起、无 waybar/壁纸） | Hyprland 崩溃后 watchdog 用 `recoverycfg.conf` 拉起 | `systemctl --user restart ww-session` |
| `doctor` 里看到多个 `HEADLESS-*`，抓帧黑屏 | 多个虚拟输出同坐标重叠，screencopy 抓到错层 | 脚本会自动删多余的；手工删要注意**别删最后一个**（会让 Hyprland 退出） |
| `hyprctl keyword monitor` 改分辨率返回 ok 但没变 | 虚拟输出尺寸只能在创建时定（§1.3） | `scripts/sunshine-wayland.sh mode <WxH> [scale]` |
| 手工起过 Hyprland 后画面合成两遍、鼠标位移减半 | 嵌套会话：新实例拿不到 DRM master，多一个 `WAYLAND-N` 伪输出 | 不要手工起；`systemctl --user restart ww-session`（ExecStartPre 会清残留实例） |
| 独显上一连就黑，日志每帧 `Frame capture failed`，Hyprland 日志 `eglCreateImageKHR ... EGL_BAD_PARAMETER` | Hyprland/Aquamarine 在 nvidia 上导出 screencopy dmabuf 失败，与编码器无关 | 独显抓不到虚拟输出：改回集显，或插 EDID 诱骗器后改用 `capture=kms` |
| `capture=kms` 启动后 `Unknown Monitor connector type [HEADLESS]`，KMS 列表为空 | KMS 只认真实 CRTC，headless 是虚拟输出 | 同上，必须给独显接一个真实输出 |
| 配置里写 `capture = wlgrab` 起不来 | 该构建的合法值是 `wlr` | 改成 `capture = wlr` |
| 想看延迟数字 | 默认 info 级别不打 | 临时 `min_log_level = debug` |

**键鼠是否真的通了**（不依赖人眼）：串流中向 Moonlight 窗口注入 `SUPER+Return`，
headless 桌面上应新开一个 `foot` 窗口；同时内核会出现
`libvirtualhid Keyboard/Mouse` 设备，Hyprland 显示为 `libvirtualhid-keyboard-1` /
`libvirtualhid-mouse`。

```bash
grep -a 'libvirtualhid' /sys/class/input/*/name
hyprctl devices | grep -a libvirtualhid
```

## 9. 本机临时操作

停 Sunshine：`scripts/sunshine-wayland.sh stop`（systemd 管理时用 `systemctl --user stop sunshine-wayland`）。

需要本地物理屏参与桌面时：

```bash
hyprctl keyword monitor "eDP-1,1920x1080@60,0x0,1"   # 临时启用面板
# 或把 hyprland.conf 里的 monitor = eDP-1, disable 改回带分辨率的写法
```

需要恢复图形登录界面（X11）时：`sudo systemctl enable --now sddm`。注意它会重新占住 DRM master，
Wayland 侧的 eDP-1 又会退化成 `0x0`，两者不要同时用。

`~/.config/sunshine/sunshine.conf.bak.*` 是每次启动前的备份，可直接还原；
`min_log_level = debug` 若加过，记得删。

## 10. 相关文件

- [scripts/sunshine-wayland.sh](../scripts/sunshine-wayland.sh)：起停 + 自动探测 + `doctor`（本文所有配置动作都在这里）
- [scripts/sunshine-wayland.service](../scripts/sunshine-wayland.service)：Sunshine 用户单元（`PartOf=ww-session`）
- [scripts/install-sunshine-host.sh](../scripts/install-sunshine-host.sh)：一键切到 Sunshine-only 部署
- [scripts/font-scale.sh](../scripts/font-scale.sh)：字号档位（1.0/1.15/1.3/1.45）+ 清晰度自检（见 §12）
- [config/foot/foot.ini](../config/foot/foot.ini)、[config/gtk-3.0/settings.ini](../config/gtk-3.0/settings.ini)：终端字号与 GTK 抗锯齿
- [scripts/ww-session.service](../scripts/ww-session.service)：Hyprland 桌面会话（抓帧目标）
- [scripts/switch-display.sh](../scripts/switch-display.sh)：统一切 GPU（集显/独显）与分辨率/缩放
- [scripts/switch-gpu.sh](../scripts/switch-gpu.sh)：重建 Hyprland 会话到指定 GPU（被 `switch-display.sh` 调用）
- `~/.config/hypr/display.state`：上次切到的卡（`intel`/`nvidia`），仅作参考；**编码器不再由它决定**
- `~/.config/sunshine/sunshine.conf`：Sunshine 配置（capture/encoder/output_name/min_log_level 由脚本改写）
- `~/.config/hypr/monitors.conf`：显示器布局（分辨率/位置/缩放），由 nwg-displays 导出，
  在 `hyprland.conf` 里用 `source` 引入；`SUPER+SHIFT+D` 打开设置界面
- `~/.config/sunshine/sunshine.log`：服务端日志（抓帧/编码/连接/延迟）
- `~/.config/sunshine/sunshine_state.json`：Web UI 账号与已配对设备
- [bugs/sunshine-nvidia-black-screen.md](bugs/sunshine-nvidia-black-screen.md)：**仍未修复**的独显虚拟输出抓帧缺陷（唯一遗留 bug）

## 11. 已修复的坑（2026-11-21）

这一轮把「Moonlight 连上全黑」彻底拆开修完了，记录在此备查（只剩独显虚拟输出那条属于上游缺陷，
见 [bugs/sunshine-nvidia-black-screen.md](bugs/sunshine-nvidia-black-screen.md)）。

| 症状 | 根因 | 修法 |
|---|---|---|
| 开机后 Moonlight 连上**全黑**，`sunshine.log` 停在版本号之后、连 `Frame capture failed` 都没有 | 发行版 user 单元 `app-dev.lizardbyte.app.Sunshine.service` 由 user manager 直接拉起，环境是 `DISPLAY=:0` / `XDG_SESSION_TYPE=x11`、**没有 `WAYLAND_DISPLAY`** → wlr 抓帧拿不到 Wayland socket | `disable --now` + `mask` 发行版单元，改由 `sunshine-wayland.service`（脚本注入正确环境、`env -u DISPLAY`） |
| 每帧 `Frame capture failed`，客户端黑屏 | 编码器与抓帧卡不同卡（`display.state` 过期 → 集显会话用 `nvenc`），跨 GPU 导入 dmabuf | 脚本改为按合成器 `zwp_linux_dmabuf` feedback 主设备的驱动选 `vaapi`/`nvenc`；`display.state` 只作参考 |
| 串流里**看不到鼠标指针** | 浏览器链路留下的全透明光标主题 `XCURSOR_THEME=ww-invisible`；Sunshine 的 wlr 抓帧把光标编进视频帧 | `ww-session` 去掉该变量、`cursor:enable_hyprcursor = true` |
| 重启后 headless 名字变了（`HEADLESS-3` → `HEADLESS-2`），`output_name` 失配 | Hyprland 重启后虚拟输出重建、编号可能变 | 脚本启动时按实际输出名重写 `sunshine.conf`，并把 `monitors.conf` 里的名字一起改掉 |
| 抓到的画面是别的一层 / 黑屏 | 存在多个 `HEADLESS-*` 且同坐标 `0x0` 重叠 | 脚本发现多余虚拟输出直接删掉，只保留一个 |
| 发行版单元与手工实例抢 47984-47990 | 两套启动方式并存 | 一套 systemd 单元 + `start` 子命令守卫（检测到单元在跑就让人用 `systemctl restart`） |
| 改分辨率「返回 ok 但不变」 | Aquamarine headless 输出创建后 `availableModes` 固定 | 创建前预注册 monitor 规则；新增 `mode` 子命令（新建→删旧→重启 Sunshine） |
| 手工起 Hyprland 后画面合成两遍、鼠标位移减半 | 嵌套会话（新实例拿不到 DRM master，多出 `WAYLAND-N` 伪输出） | `ww-session` 的 `ExecStartPre` 用 `pkill -x` 清残留；文档明确「不要手工起」 |
| 删掉最后一个输出后 Hyprland 直接退出、再拉起是 safe-mode（物理屏被点亮） | Hyprland 在无输出时会退出，watchdog 用 `recoverycfg.conf` | 脚本只删**多余**输出；`doctor` 检测 safe-mode 并提示 `systemctl --user restart ww-session` |
| X11 仍在开机自启（`sddm`、LXQt 自启项、`xset` dpms 脚本、user manager 里残留的 `DISPLAY`） | 历史部署遗留 | `sddm`/`x11-common` mask、`xwayland.enabled=false`、mask `app-lxqt-*@autostart`、`screen-blank-1min.desktop` 挪到 `autostart/disabled/`、`systemctl --user unset-environment DISPLAY XAUTHORITY` |
| `setcap cap_sys_admin+p`（KMS 实验遗留）、`min_log_level=debug` | 排查期临时改动 | 已 `setcap -r`、日志级别固定 `info`（需要时 `SUN_LOG_LEVEL=debug`） |

## 12. 1080p@1x 的字号放大（不糊）

固定在 **1920×1080@60、`scale=1`**（逻辑也是 1920×1080），把所有「字小」的问题交给**字号**解决，
而不是交给缩放。

### 12.1 为什么不能用 scale 放大

| 做法 | 得到什么 | 为什么糊 / 不合适 |
|---|---|---|
| `monitor ..., 1920x1080@60, 0x0, 2` | 逻辑 960×540，字大 | 不糊，但可用空间直接砍半，窗口塞不下 |
| `scale=1.25 / 1.5`（分数缩放） | 字大一点 | 客户端按分数倍栅格化、合成器再重采样回整数像素 → 文字边缘发虚 |
| `GDK_SCALE=2` / `QT_SCALE_FACTOR=2` | 部分程序变大 | 同 2x（空间砍半），且 Qt 的分数缩放同样糊；只能覆盖部分程序 |
| Chrome `--force-device-scale-factor=1.25` | 浏览器整体变大 | 整窗先放大再降采样 → 糊；标签栏还行、网页正文明显发软 |
| **只调字号（本方案）** | 字变大、逻辑仍是 1920×1080 | 文字**按目标字号直接栅格化到原生像素**，笔画仍是 1px 级过渡，不糊 |

一句话：**画面缩放会引入一次重采样，字号不会。** 所以固定 1080p、scale=1，只调字号。

### 12.2 一条命令调档

```bash
scripts/font-scale.sh            # 默认档 1.3（当前部署）
scripts/font-scale.sh 1.15       # 小一点
scripts/font-scale.sh 1.45       # 大一点
scripts/font-scale.sh 1.0        # 回滚到原始字号
scripts/font-scale.sh status     # 自检：各程序当前字号 + 有没有整屏缩放糊源
```

脚本是幂等的：所有位置都按 `k` 重新算一遍，重复执行结果一致；改前会备份成 `.bak`。
应用后会自动 `hyprctl reload` + 重启 waybar；`foot` / `fuzzel` 需要**重开窗口**才生效。

### 12.3 档位对照（实测/计算值）

| k | fuzzel | foot | waybar 字号 / 栏高 | 光标 | fuzzel 内边距 / 行高 |
|---|---|---|---|---|---|
| 1.0 | 11pt | 14px | 12px / 38px | 24px | 10 / 26 |
| 1.15 | 13pt | 16px | 14px / 42px | 28px | 12 / 30 |
| 1.3 | 14pt | 18px | 16px / 46px | 31px | 13 / 34 |
| 1.45 | 16pt | 20px | 17px / 48px | 35px | 15 / 38 |

### 12.4 各处配置在哪、为什么这么改

| 程序 | 位置 | 做法与原因 |
|---|---|---|
| GTK 程序（`nwg-displays`、对话框、Chrome 的 GTK 主题） | `gsettings org.gnome.desktop.interface text-scaling-factor` + `~/.config/gtk-3.0|4.0/settings.ini` | `text-scaling-factor` **只放大字号**，界面按新字号重绘；settings.ini 只放抗锯齿/微调（Wayland 无子像素渲染，用 grayscale + full hinting）。**不要**同时改 `font-name` 大小，两者会相乘 |
| `foot` 终端 | `~/.config/foot/foot.ini`（仓库 `config/foot/foot.ini`） | `font=monospace:pixelsize=N` + `dpi-aware=no`：字号用像素表达，绕开 DPI 换算带来的半像素 |
| `fuzzel` 启动器 | `~/.config/fuzzel/fuzzel.ini`（仓库同名文件） | 字号 + `horizontal-pad`/`vertical-pad`/`inner-pad`/`line-height` 同比放大；`width`/`lines` 是**字符数**，不用改（字号变大窗口自然变大） |
| `waybar` 顶栏 | `~/.config/waybar/style.css` + `config.jsonc` | CSS 的 `font-size` **优先级高于 GTK 字号**，所以必须显式改（否则「改了 GTK 顶栏没变」）；图标是字体字形，跟着一起变大；`height` 同比加高 |
| 鼠标指针 | `scripts/ww-session.service` 的 `XCURSOR_SIZE` / `HYPRCURSOR_SIZE` | Hyprland **没有** `cursor:size` 选项（`hyprctl getoption cursor:size` → `no such option`），只能在启动时用环境变量给；改值后需重启 `ww-session` |
| Chrome | `~/.local/share/applications/google-chrome.desktop`（`deploy-config.sh` 生成） | `--ozone-platform=wayland --force-device-scale-factor=1`：走 Wayland 原生、固定 1x，**不引入整窗降采样**；网页内容用 Chrome 设置里的「页面缩放 125%」（文字按 125% 重绘，仍然清晰） |

### 12.5 怎么验证「真的没糊」

```bash
scripts/font-scale.sh status     # 第 6 行必须 scale=1.0；第 7 行「整屏缩放变量」必须为空
grim -o HEADLESS-2 /tmp/s.png    # 必须是 1920×1080（1:1，没有任何缩放）
python3 -c "from PIL import Image; print(Image.open('/tmp/s.png').size)"
hyprctl layers -j | grep -A3 '"waybar"'   # 栏高应为 12.3 表里的值（1.3 档 = 46）
```

判据：截图 1:1 放大到 200% 看文字，笔画过渡应是 **1px 级灰阶**；
如果看到整块 **2×2 重复像素**，说明中间有缩放环节（检查 `scale`、`GDK_SCALE`、Chrome DSF）。

另外：**客户端窗口必须接近 1920×1080 的整数倍区域**。Mac Retina 全屏是 2 倍整数放大（可接受）；
如果窗口被拖成奇怪尺寸，糊的是客户端的重采样，不是服务端。

### 12.6 常见坑

- 「改了 GTK 字号但顶栏/终端没变」→ waybar 用 CSS px、foot 用自己 ini，分别显式改（脚本已处理）。
- 「`foot` 还是小字」→ 已开的终端不会热更新，关掉重开。
- 光标变大后**必须重启会话**（`systemctl --user restart ww-session`），环境变量只在启动时读。
- 不要再用 `mode ... 2` 去「顺手放大」——那会把逻辑分辨率砍半，回到 960×540。
