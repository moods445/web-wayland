# Sunshine + Moonlight 旁挂部署（2026-11-20）

本机把 **Sunshine 作为旁挂服务端**、**Moonlight 作为客户端**跑通 1080p60 的完整记录：
怎么起、为什么这么起、实测到什么程度、坏了怎么查。适用于「想用现成 Moonlight 生态」的场景，
与项目自有的 wwcap + 浏览器/原生客户端链路**互斥**。

结论先行：

- **Intel 路径可用**：`capture=wlr` + `h264_vaapi`，抓 headless 输出，1080p60 正常；
- **NVIDIA 路径不可用**：抓帧每帧 `EGL_BAD_PARAMETER`，该构建没有回退；
- **上限 1080p60**：Intel 上 Hyprland 合成 4K 只有 20~22fps，4K 只能走独显，而独显抓帧失败。

相关文档：自建链路的实测与优化见 [streaming-conclusions.md](streaming-conclusions.md)；
旁挂方案的前期评估见 `.trae/documents/moonlight-sunshine-4k60-feasibility.md`。

## 1. 前置条件

| 项目 | 要求 |
|---|---|
| GPU 路径 | Intel（`scripts/switch-gpu.sh intel`），`/dev/dri/card1` + i915 |
| 桌面 | Hyprland 已启动，且存在一个 headless 输出（脚本会找/建） |
| Sunshine | `sunshine` 在 PATH（本机为 Debian trixie 包，2026.914.233613） |
| 用户组 | `aio` 需在 `input` 组（`usermod -aG input aio`），否则键鼠注入不生效 |
| 互斥 | 启动前必须停 `wwcap`（两者都抢 screencopy，双抓帧会把帧率打对折） |

## 2. 快速开始

```bash
# 1) 若 Hyprland 还没起（会建 HEADLESS 输出并拉起 wwcap，脚本会自己停掉 wwcap）
scripts/switch-gpu.sh intel

# 2) 起 Sunshine（自动停 wwcap、写配置、后台常驻）
scripts/sunshine-wayland.sh start

# 3) 看状态 / 停止
scripts/sunshine-wayland.sh status
scripts/sunshine-wayland.sh stop
```

启动成功后日志会给出：

```
[sun] 已写入 capture=wlr encoder=vaapi output_name=HEADLESS-2（备份 sunshine.conf.bak.<时间戳>）
[sun] 启动 Sunshine（wlr 抓帧 → HEADLESS-2）
[sun] 就绪：Web UI https://127.0.0.1:47990，串流端口 47984-47990
```

## 3. 脚本做了什么

`scripts/sunshine-wayland.sh`（入库文件）按顺序做四件事：

1. **找/建 headless 输出**：遍历 `$XDG_RUNTIME_DIR/hypr/*/` 找存活的 Hyprland 实例，
   取已有 `HEADLESS-*` 输出并固定为 `1920x1080@60`、scale 1；没有就 `hyprctl output create headless`。
2. **停 wwcap**：检测到 `wwcap` 在跑就先 `pkill -x wwcap`，避免双抓帧。
3. **写三项配置**（改前备份 `sunshine.conf.bak.<时间戳>`，并去掉 X11 的 `global_prep_cmd`）：

   ```ini
   capture = wlr
   encoder = vaapi
   output_name = HEADLESS-2
   ```

4. **起进程**：
   - 必须 `setsid`：只加 `nohup` 时，启动它的终端/IDE 会话一结束 Sunshine 就被收走
     （表现是「刚起来就连不上」，日志停在版本号之后、没有退出原因）；
   - 用 `sg input -c` 借 `input` 组启动，这样即便 `usermod -aG input` 还没重新登录也生效；
   - 环境：`XDG_RUNTIME_DIR` + `WAYLAND_DISPLAY=wayland-N` + `XDG_SESSION_TYPE=wayland`，
     并显式 `env -u DISPLAY`（避免 Sunshine 走 X11 分支）。

## 4. 手动配置（不用脚本时）

`~/.config/sunshine/sunshine.conf` 至少要有：

```ini
capture = wlr          # 必须是 wlr；写 wlgrab 会 Unable to initialize capture method
encoder = vaapi        # Intel 路径
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

1. Moonlight 里「添加主机」，填本机局域网 IP（本机 `192.168.0.109`）；mDNS 自动发现不一定通。
2. 客户端会显示 4 位 PIN，浏览器打开 `https://<IP>:47990`（自签证书，需手动信任），
   登录后在 PIN 输入框提交完成配对。首次用可在同一页面设置 Web UI 账号密码。
3. 选择 `Desktop` 开始串流。应用列表来自 `~/.config/sunshine/apps.json`
   （`Desktop` / `Low Res Desktop` / `Steam Big Picture`）。

**同机自测**（Moonlight 只有 xcb 插件，需借 XWayland `:1`）：

```bash
export DISPLAY=:1 QT_QPA_PLATFORM=xcb
./Moonlight.AppImage list 127.0.0.1
./Moonlight.AppImage stream 127.0.0.1 "Desktop" \
  --1080 --fps 60 --bitrate 20000 --display-mode windowed
```

配对状态存在 `~/.config/sunshine/sunshine_state.json` 的 `named_devices` 里，
删掉对应条目即可让某个客户端重新配对。

## 6. 实测数据（Intel / HEADLESS-2 1920x1080@60）

| 项目 | 实测值 |
|---|---|
| 抓帧 | `wlr`（`zwlr_screencopy_manager_v1` v3），`[wlgrab] Monitor 1 is HEADLESS-2` |
| 编码 | `h264_vaapi`（Intel iHD 25.2.3），RC framerate 60/1 |
| 码率 | 14,988,000 bps（客户端请求 20 Mbps，服务端折算） |
| 色彩 | SDR **Rec.601** / 8-bit / JPEG range（项目链路是 BT.709 + full range） |
| 服务端帧处理延迟 | 4 分钟稳态 avg **9.7~10.1ms**；20s 窗口 min 6.9ms / max 27.2ms / avg **9.17ms** |
| 客户端 | Moonlight 6.2.0：`Video stream is 1920x1080x60 (format 0x1)`、`Test decode successful` |
| 会话 | `CLIENT CONNECTED` → `CLIENT DISCONNECTED` 正常，期间无 `Frame capture failed` |

延迟是「抓帧 + 编码」，不含网络与客户端解码。
串流期间 Sunshine 会把默认 sink 临时切到 `sink-sunshine-stereo`，断开后恢复为 `ww-desk`（日志可见）。

## 7. 已知限制

1. **只有 1080p60**。Intel 上 Hyprland 合成 4K 只有 20~22fps（见
   [streaming-conclusions.md §9](streaming-conclusions.md)），4K 需要独显；独显上 Sunshine 抓帧失败。
2. **画质低一档**：Rec.601 + JPEG range + 4:2:0，桌面文字/纯色块不如项目自有的
   BT.709 + full range + 4:4:4 链路；Sunshine 的 VAAPI 路径没有 4:4:4 选项。
3. **与 wwcap 互斥**：同时跑会双抓帧（历史事故 58→30fps）。
4. **输出名会变**：Hyprland 重启后 headless 输出会重建（`HEADLESS-N` 可能变），
   必须重跑脚本让 `output_name` 跟上。
5. **eDP-1 仍是 0x0**：Xorg/sddm 占着 DRM master 时本机面板拿不到 modeset，
   串流内容只存在于 headless 输出（本地屏幕看不到，只能通过 Moonlight 看）。

## 8. 故障排查

| 现象 | 原因 | 处理 |
|---|---|---|
| 刚启动就连不上，日志停在版本号之后 | 进程被启动它的 shell 收走 | 必须 `setsid`（脚本已处理） |
| 键鼠完全没反应 | `/dev/uinput` 是 `root:input 0660`，用户不在 `input` 组 | `usermod -aG input aio`；重新登录前用 `sg input -c` 启动 |
| 屏幕黑屏/无画面 | `output_name` 与实际 headless 名不一致，或输出被销毁 | 重跑 `scripts/sunshine-wayland.sh start` |
| 帧率腰斩 | 同时开了 `wwcap` | `pkill -x wwcap` 后重启 Sunshine |
| 独显上 `Frame capture failed` + `EGL_BAD_PARAMETER` | NVIDIA 路径不支持该抓帧方式 | 改用 Intel 路径，本机没有 4K 备选 |
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

## 9. 恢复与回滚

```bash
scripts/sunshine-wayland.sh stop      # 停 Sunshine
scripts/switch-gpu.sh intel           # 需要恢复项目自有链路时重跑，会重新拉起 wwcap
```

`~/.config/sunshine/sunshine.conf.bak.*` 是每次启动前的备份，可直接还原；
`min_log_level = debug` 若加过，记得删。

## 10. 相关文件

- [scripts/sunshine-wayland.sh](../scripts/sunshine-wayland.sh)：起停脚本（本文所有配置动作都在这里）
- [scripts/switch-gpu.sh](../scripts/switch-gpu.sh)：切换 Intel / NVIDIA 会话
- `~/.config/sunshine/sunshine.conf`：Sunshine 配置（三项由脚本改写）
- `~/.config/sunshine/sunshine.log`：服务端日志（抓帧/编码/连接/延迟）
- `~/.config/sunshine/sunshine_state.json`：Web UI 账号与已配对设备
- [docs/streaming-conclusions.md](streaming-conclusions.md)：项目自有链路的性能结论与优化记录
