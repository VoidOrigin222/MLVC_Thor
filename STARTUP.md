# 系统启动流程与命令

适用版本：2026-10-19，可配置路数与逐路帧率版本。编解码在 VSCode Remote-SSH 中操作，Windows UI 使用 EXE 启动。

## 1. 设备与目录

| 设备 | 地址 / 用户 | 用途 |
|---|---|---|
| Windows 电脑 | `192.168.5.3` | MediaMTX、UI |
| 编码板 | `xidian@192.168.5.16`，主机名 `xidian-ve` | 测试源发布、语义编码 |
| 解码板 | `xidian@192.168.5.17`，主机名 `xidian-vd` | 语义解码、重建视频发布 |

两块板卡的运行目录均为：

```text
/home/xidian/workspace/MLVC_Thor
```

日常启动在这个运行目录操作，配置文件位于其 `configs/` 下。
`src/MLVC` 是源码目录，运行时不需要进入其中，也不需要每次编译或执行 `check_runtime.sh`。

运行目录的 `configs/` 仅保留日常配置：

```text
configs/
├── encoder.toml
├── decoder.toml
└── five/
    └── test_sources.json
```

旧联调配置已移到运行目录同级的 `MLVC_Thor_archive/20261019/config-cleanup/`。
基准模板保留在 `src/MLVC/ports/thor/configs/`；五路基准临时生成的配置写入 `logs/`。

默认配置为 `stream_count = 5`、1920×1080、12 FPS，`execution = "contexts"`。两端各预留八路通道定义，未启用的条目可一直保留。需要改变路数或帧率时，先完成第 7 节的配置，再按以下顺序启动。

## 2. 启动顺序

**电脑 MediaMTX → 解码板 → 编码板测试源 → 编码板编码 → Windows UI。**

MediaMTX 由使用者自行开启，使用现有配置即可。当前 RTSP 端口为 `8554`，WebRTC HTTP 端口为 `8889`，API 端口为 `9997`。

## 3. 解码板：启动解码

在 VSCode 中通过 Remote-SSH 连接解码板 `192.168.5.17`，打开集成终端，执行：

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_decoder.sh configs/decoder.toml
```

保持终端运行，等待解码程序进入接收状态，再启动编码。解码端从各路码流头读取 FPS，不必重复设置帧率。

## 4. 编码板：启动循环测试源

在另一个 VSCode 窗口通过 Remote-SSH 连接编码板 `192.168.5.16`，打开终端 1，执行：

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_test_sources.sh
```

脚本默认读取 `configs/five/test_sources.json`，视频循环播放，计时器实时生成，直到按 Ctrl+C 停止。目录名 `five` 为现有配置路径，列表中的路数可以修改。

默认场景如下：

| UI 通道 | 输入 RTSP | 场景 |
|---|---|---|
| 1 | `rtsp://192.168.5.3:8554/input0` | 静态矿井场景 |
| 2 | `rtsp://192.168.5.3:8554/input1` | 实时计时器 |
| 3 | `rtsp://192.168.5.3:8554/input2` | 矿井动态场景 |
| 4 | `rtsp://192.168.5.3:8554/input3` | 实验室动态场景 |
| 5 | `rtsp://192.168.5.3:8554/input4` | 室外动态场景 |

首次使用新的测试尺寸或帧率时，脚本可能先生成视频缓存；完成后才开始发布。匹配的动态视频及已生成的缓存直接循环推流。

**测试源在编码板启动。** 素材位于编码板的 `data/video/`，不要在解码板执行测试源命令。

如果输入来自已经在线的摄像头或其他 RTSP 发布者，直接在 `encoder.toml` 中填写实际 RTSP 地址，本节测试源脚本可以跳过。

## 5. 编码板：启动编码

在编码板的同一个 VSCode 窗口中新建终端 2，执行：

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_encoder.sh configs/encoder.toml
```

保持两个编码板终端与解码板终端运行。语义流通过 RTP 发往解码板，解码后的重建视频发布至电脑 MediaMTX 的 `/ulbvc0` 等路径。

## 6. Windows：启动 UI

关闭旧版本 UI 启动器，然后双击新版程序：

```text
C:\Users\VoidOrigin\Desktop\mlvc\MLVC_c\MLVC_Thor\deliveries\ui-configurable-20261019\SemanticFiveMonitor.exe
```

EXE 同目录保留 `ui_config.json`，启动器会自动打开浏览器。默认页面为：

```text
http://127.0.0.1:8765/
```

端口被占用时，以启动器显示的地址为准。启动器窗口保持打开，关闭启动器会停止本地 UI 及其自有 H.264 发布进程。

也可以在 Windows PowerShell 中启动：

```powershell
& "C:\Users\VoidOrigin\Desktop\mlvc\MLVC_c\MLVC_Thor\deliveries\ui-configurable-20261019\SemanticFiveMonitor.exe"
```

UI 每个通道显示原图、H.264、语义压缩三个画面。H.264 对照默认自动启动，QP 默认 `28`，FFmpeg 已包含在 EXE 内，无需另外运行 H.264 命令。

**UI 可以始终保留五个通道，不必随编解码路数删减。** 未推流的画面显示“无信号”。如果测试源仍在发布五路，而编解码只运行两路，其他通道的原图、H.264 仍可能有画面，语义压缩位置没有对应输出时显示“无信号”。

## 7. 配置路数和帧率

### 7.1 配置文件

| 位置 | 配置文件 | 作用 |
|---|---|---|
| 编码板 | `/home/xidian/workspace/MLVC_Thor/configs/encoder.toml` | 编码路数、输入地址、目标 FPS、发送端口 |
| 解码板 | `/home/xidian/workspace/MLVC_Thor/configs/decoder.toml` | 解码路数、接收端口、重建输出地址 |
| 编码板 | `/home/xidian/workspace/MLVC_Thor/configs/five/test_sources.json` | 可选测试源的路数、场景、帧率 |

使用 `execution = "contexts"`。文件顶部的 `stream_count` 决定实际启动路数，只启用前 N 个 `[[streams]]`。编码端与解码端填写相同 N，后面的备用定义无需删除。`batch5` 仍固定启用五路。

修改 TOML 后重启对应程序；调整帧率不需要重新生成单路 TensorRT 引擎。
以下 TOML 均为修改片段，保留原文件的其他公共参数，不要用片段覆盖整个文件。
当前 RTSP 输入自动检测源帧率，日常只设置 `target_fps`；旧配置的 `fps` 行可以删除。参数位置及含义见 [配置参数清单](ports/thor/MULTI_CONFIG.md#常用可配置参数)。

### 7.2 两路 30 FPS

编码端现有文件按以下位置修改对应项：

```toml
# 文件顶部，在所有 [表名] 之前
execution = "contexts"
stream_count = 2
log_prefix = "logs/encoder"

[encoder]
target_fps = 30
# 此表内的其他公共参数继续保留
```

解码端文件顶部修改为：

```toml
execution = "contexts"
stream_count = 2
log_prefix = "logs/decoder"
```

两端原来的八个 `[[streams]]` 全部保留。程序只连接和处理前两路；第三路及之后不创建编解码任务，也不占用相应接收端口。解码端从码流头读取帧率，不需要另外设置 FPS。

### 7.3 三路 20 FPS

两端文件顶部都设置：

```toml
stream_count = 3
```

编码端已有的 `[encoder]` 中设置：

```toml
target_fps = 20
```

所有备用通道条目继续保留，不必删改。前三路的发送/接收端口分别为 39480、39482、39484。

### 7.4 八路 5 FPS

两端文件顶部都设置：

```toml
stream_count = 8
```

编码端已有的 `[encoder]` 中设置：

```toml
target_fps = 5
```

当前模板已预留八路地址及端口：输入 `/input0`–`/input7`，重建输出 `/ulbvc0`–`/ulbvc7`。
启用前应准备对应的 RTSP 输入；测试源默认仍只发布五路，需要八路测试时为测试源 JSON 配置八项。UI 可以继续保留五个槽位。

如果之后扩展到超过八路，添加对应的通道定义，再增大 `stream_count`。N 超过已配置条目数时程序会明确报错，不自动猜测输入地址。

### 7.5 每路不同帧率

公共 `[encoder]` 中设默认帧率，需要单独调整的通道，在该路已有的 `[streams.encoder]` 下覆盖：

```toml
target_fps = 30
```

`stream_count` 只控制启用路数，逐路覆盖控制各自帧率。例如保留八路定义，设置 N=5、公共 20 FPS，前两路覆盖 30 FPS，则实际启用五路，其目标为 30/30/20/20/20 FPS。

### 7.6 测试源帧率

只修改编码目标帧率，不会把现有 12 FPS 输入变成真实 30 FPS 输入。输入高于目标帧率时抽帧，低于目标时按实际输入输出。

需要测试两路 30 FPS 时，可以将编码板 `configs/five/test_sources.json` 改为以下完整内容：

```json
{
  "rtsp_base": "rtsp://192.168.5.3:8554",
  "fps": 30,
  "width": 1920,
  "height": 1080,
  "font": "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
  "streams": [
    {"type": "dynamic", "video": "data/video/614lab-60fps-test.mp4"},
    {"type": "timer"}
  ]
}
```

三路 20 FPS 时，将公共 `fps` 改为 `20`，并在 `streams` 数组追加：

```json
{"type": "dynamic", "video": "data/video/outdoor-original.mp4"}
```

每个测试源也可以添加独立的 `"fps": 20` 或 `"fps": 30` 覆盖公共值。测试源修改后，在原终端 Ctrl+C 停止，再执行同一启动命令。

UI 无需调整显示路数；若需要原图理论带宽按新的 FPS 计算，可在 UI 参数设置中同步该通道的 FPS。UI 中的 FPS 只影响统计记录，不会控制板端帧率。

## 8. 端口对应关系

| 通道 | 编码输入 | RTP / RTCP | 编码统计 UDP | 解码重建输出 | UI H.264 输出 |
|---|---|---|---|---|---|
| 1 | `/input0` | `39480 / 39481` | `39600` | `/ulbvc0` | `/camera-h2640` |
| 2 | `/input1` | `39482 / 39483` | `39601` | `/ulbvc1` | `/camera-h2641` |
| 3 | `/input2` | `39484 / 39485` | `39602` | `/ulbvc2` | `/camera-h2642` |
| 4 | `/input3` | `39486 / 39487` | `39603` | `/ulbvc3` | `/camera-h2643` |
| 5 | `/input4` | `39488 / 39489` | `39604` | `/ulbvc4` | `/camera-h2644` |
| 6 | `/input5` | `39490 / 39491` | `39605` | `/ulbvc5` | UI 默认未配置 |
| 7 | `/input6` | `39492 / 39493` | `39606` | `/ulbvc6` | UI 默认未配置 |
| 8 | `/input7` | `39494 / 39495` | `39607` | `/ulbvc7` | UI 默认未配置 |

表中的 RTSP 路径均位于电脑 MediaMTX 的 `8554` 端口。每路 RTP 占用本端口与后一个 RTCP 端口，不能交叉复用；统计端口也不能重复。

## 9. 更换设备 IP

| 变动 | 需要修改的位置 |
|---|---|
| 解码板 IP 改变 | 编码板 `encoder.toml` 的 `[encoder].output_transport_host` |
| 电脑 / MediaMTX IP 改变 | 编码端各路 `input_rtsp`、解码端各路 `output_transport_rtsp_url`、测试源 JSON 的 `rtsp_base` |
| 接收统计的电脑 IP 改变 | 编码板 `[encoder].ui_stats_host` |
| 编码板 IP 改变 | VSCode Remote-SSH 的连接地址；当前数据流由编码板主动发送，不需要在解码端另外填写编码板 IP |

UI 和 MediaMTX 都在同一台 Windows 电脑上时，UI 中的 `127.0.0.1` 服务地址可以保留。若两者分离部署，再修改 UI 中对应的 RTSP、WebRTC 和 API 地址。

## 10. 停止顺序

1. Windows：关闭 UI 启动器，停止 UI 及其自有 H.264 对照推流。
2. 编码板终端 2：按 Ctrl+C，停止语义编码。
3. 编码板终端 1：按 Ctrl+C，停止测试源及其子推流进程；使用真实 RTSP 源时跳过。
4. 解码板终端：按 Ctrl+C，停止语义解码。
5. MediaMTX 保留运行。

## 11. 日常启动命令汇总

MediaMTX 已开启后，依次执行以下操作，三个板端终端保持运行。

**解码板 `192.168.5.17`：**

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_decoder.sh configs/decoder.toml
```

**编码板 `192.168.5.16`，终端 1（使用测试源时）：**

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_test_sources.sh
```

**编码板 `192.168.5.16`，终端 2：**

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_encoder.sh configs/encoder.toml
```

**Windows PowerShell：**

```powershell
& "C:\Users\VoidOrigin\Desktop\mlvc\MLVC_c\MLVC_Thor\deliveries\ui-configurable-20261019\SemanticFiveMonitor.exe"
```
