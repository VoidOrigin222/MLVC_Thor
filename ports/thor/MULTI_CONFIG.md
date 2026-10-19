# 多路 TOML 配置

启动命令、两路 30 FPS / 三路 20 FPS 示例、测试源和 IP 修改统一见 [STARTUP.md](../../STARTUP.md)。本页说明配置结构和约束。

## 两端文件

- 编码板：`/home/xidian/workspace/MLVC_Thor/configs/encoder.toml`。
- 解码板：`/home/xidian/workspace/MLVC_Thor/configs/decoder.toml`。

程序在内存中合并公共值与逐路覆盖，不生成临时 TOML。相对路径以运行目录为基准；脚本会切换到运行目录。修改配置后重启对应程序。

## 结构与继承

| 配置项 | 作用 |
|---|---|
| `execution = "contexts"` | 每路独立上下文的运行模式 |
| `stream_count = 5` | 显式启用 N 路，启动列表中的前 N 项；备用定义可保留 |
| `log_prefix` | 分路日志前缀，如 `logs/encoder-lane0.log` |
| `[encoder]` / `[decoder]` | 本端公共参数 |
| `[encoder.pipeline]` / `[decoder.pipeline]` | 本端公共流水线参数 |
| `[[streams]]` | 一个可用通道定义，按顺序编号；条目数量是可配置容量，不是实际启用数 |
| `[streams.encoder]` / `[streams.decoder]` | 该通道覆盖公共参数 |

嵌套表逐项继承。例如只覆盖 `pipeline.queue_capacity`，不会清除公共 `entropy_workers` 等值。两端 `stream_count` 与活动 RTP 端口按路对应，不能只改其中一端。

`batch5` 仍要求 `stream_count = 5` 和对应 N=5 引擎，不能通过改变列表修改批量大小。逐路基准模板保留在源码的 `ports/thor/configs/five/contexts/`、`ports/thor/configs/five/batch5/`，不再复制到板端运行 `configs/`；日常两个总配置不引用这些分路 TOML。五路基准的临时配置写入 `logs/`。

## 显式路数

`stream_count` 写在文件顶部、任何 `[encoder]` / `[decoder]` 表之前。编码和解码两端填写相同值。
当前总配置默认 `stream_count = 5`，两端各预留八个通道定义。切换到两路 30 FPS 只需两端 N=2、编码目标帧率 30；八路 5 FPS 则两端 N=8、编码目标帧率 5。

N 不得超过列表长度，程序不会自动生成摄像头地址；扩展更多路时补充对应定义即可。
旧文件没有 `stream_count` 时继续沿用“启用全部定义”的兼容行为，新配置应明确填写该项。

## 帧率

在 `[encoder]` 中设置公共默认值，例如：

```toml
target_fps = 20
```

需要某路 30 FPS 时，在该路已有的 `[streams.encoder]` 下覆盖：

```toml
target_fps = 30
```

`target_fps` 控制 RTSP 采样；实际输出由源帧率与目标共同决定，输入不足时不补帧。常用配置只写 `target_fps`；`fps` 是兼容字段，不用再同步设置。解码器从各路码流头读取 FPS，无需另设解码帧率；改变路数与采样率不需要重新生成单路 TensorRT 引擎。

默认五路 12 FPS。已实测 2×30、3×20、混合 30+20，以及八路独立 RTSP 会话的 8×5；任意配置组合的实时吞吐仍需实测，详见 [验证记录](CONFIGURABLE_STREAMS_VALIDATION.md)。

## 端口与统计

- 编码 `output_transport_host` 为解码板 IP，`output_transport_port` 对应解码 `input_transport_port`。
- 每路 RTP 占本端口与后一个 RTCP 端口，默认端口对从 `39480/39481` 开始，每路递增 2。
- UI 统计默认开启：公共 `ui_stats_host="192.168.5.3"`，逐路 `ui_stats_port` 从 `39600` 开始递增。
- `ui_stats_port=0` 关闭该路统计。统计接收端离线不会阻塞编解码。
- 加载时拒绝非正整数路数、N 超过定义数量、空列表、未知键、无效模式，以及启用通道的端口交叉或重复；活动配置校验完成后才启动 GPU 工作。备用条目不做通道参数校验、不连接、不分配运行资源，但整个文件仍须符合 TOML 语法。

## UI 和测试源

UI 可以一直保留五个槽位，少跑路时无需删减 UI 通道；未推流的画面显示“无信号”。若其他原图源仍在线，其原图和 H.264 仍会播放，只有未发布的语义输出没有信号。

UI 的 `fps` 是统计记录，不远程控制板端；需要按新 FPS 显示原图理论带宽时可同步修改该记录。

可选测试源读取 `configs/five/test_sources.json`。`streams` 数组决定源路数，公共 `fps` 可逐项覆盖。尺寸或帧率不匹配的视频先由 NVDEC/NVENC 生成缓存，再循环推流。低帧率素材升帧仅重复已有帧，不构成真实更高帧率采集。

## 已明确的配置要求

- 用文件顶部 `stream_count` 显式配置 N，支持 2 路 30 FPS、3 路 20 FPS、8 路 5 FPS 等组合；不要求增减条目来切换已有路数。
- 允许预先保留八路或更多通道定义，程序仅启动前 N 路，剩余定义备用。
- 公共参数只写一次，每路按需覆盖；每端保留一个总 TOML。
- RTSP 源帧率自动检测，日常编码配置只保留 `target_fps`，不要求同时填写 `fps`。
- 网络地址、各路输入与输出、RTP 和统计端口可修改，方便更换设备或网段。
- UI 可以保持五路槽位；未推流的位置显示“无信号”，无需和板端同步删减槽位。
- H.264 对照 QP 可配置且有默认值，使用 UI 的独立 JSON，不放进板端编码 TOML。

## 常用可配置参数

下面是当前已实现的参数位置，不需要新增相同字段。公共参数也可在对应的
`[streams.encoder]` / `[streams.decoder]` 中逐路覆盖；地址和端口须两端配对。

| 想调整的内容 | 位置 / 参数 | 含义与使用方法 |
|---|---|---|
| 路数 | 两端文件顶部的 `stream_count` | 设置 2 / 3 / 8 等正整数；列表可一直保留八项，只启用前 N 项 |
| 目标帧率 | 编码端 `target_fps` | 如 12、20、25、30；源更快时抽帧，源不足时不补帧，0 关闭采样 |
| 每路输入源 | 编码端 `input_rtsp` | 完整 RTSP URL，包含流路径 |
| 语义质量档位 | 编码端 `qp` | 当前为 11，合法档位受模型/sidecar 限制，与 H.264 QP 不同体系 |
| I 帧间隔 | 编码端 `gop` | 单位为帧，当前为 96；调整会影响随机访问周期与带宽 |
| 参考重置间隔 | 编码端 `reset_interval` | 当前为 32；属于编码状态参数，不表示路数 |
| 运行帧数 | 本端 `frame_num` | -1 持续运行，正整数处理指定帧数后结束 |
| 解码板 IP | 编码端 `output_transport_host` | 语义 RTP 的接收板地址 |
| 每路语义传输端口 | 编码 `output_transport_port` ↔ 解码 `input_transport_port` | RTP 端口一致；后一端口由 RTCP 使用 |
| 每路重建输出地址 | 解码端 `output_transport_rtsp_url` | 完整 MediaMTX RTSP 发布地址，默认 `/ulbvc0` 等 |
| UI 统计接收地址 | 编码端 `ui_stats_host` | Windows 电脑的局域网 IP，不是板卡自身地址 |
| 每路统计端口 | 编码端 `ui_stats_port` | 默认 39600 起逐路递增；0 关闭该路统计 |
| 网络发送整形 | 编码端 `output_transport_pacing_rate_bps` | 限制发送节奏，不是语义编码目标码率，不能据此保证等码率 |
| 日志位置 | 本端 `log_prefix` | 分路日志前缀 |

## 高级配置与固定约束

`pipeline.queue_capacity`、`frame_buffer_slots`、`graph_packet_capacity`、
`entropy_workers`、`motion_prefetch_frames`、`motion_decode_threads` 及解码 RTP
接收队列/恢复策略已有配置入口，属于并发、内存和积压调优参数。
`stream_workers` 是每路内部的工作线程数，**不是视频路数**。

`manifest` 和 `device` 选择模型资源与设备。`video_decode_backend="nvdec"`、
`motion_backend="nvenc"`、`input_device_resident=true`、
`output_transport_rtsp_encoder="nvenc"` 是当前硬件实现的约束，不提供软件回退选项。
保留模型配套的 `translation_warp` 和 LTR 设置，不能把硬件后端名称当作自由切换的软件编码器选项。

## 三处帧率的区别

| 文件 | 参数 | 实际作用 |
|---|---|---|
| 编码端 `encoder.toml` | `target_fps` | 语义编码保留多少帧，公共值可逐路覆盖 |
| 测试源 `test_sources.json` | `fps` | 测试源本身按多少 FPS 发布 |
| UI `ui_config.json` | 通道 `fps` | 统计用的帧率记录，不控制源和板端 |

这三处用途不同，不能互相替代。编码 TOML 的旧 `fps` 字段仍允许解析，
在历史名义帧率与目标码率预算校验中有引用；当前 RTSP 的实际源帧率会覆盖
该名义值，当前固定 QP 配置不需要填写它。兼容解析保留，日常总配置已省去。
