# 五路基准复现

五路基准脚本在电脑上运行，通过免密 SSH 启动两块板卡的程序并收集结果。
使用同一个 H.264 视频的五个时间偏移，不等同于五个独立摄像头场景。
脚本不会启动 MediaMTX，也不会下载模型或测试视频。

## 运行目录

两块板均按 [运行说明](README.md) 安装模型、引擎和程序。默认目录为
`/home/xidian/workspace/MLVC_Thor`：

```text
bin/mlvc_encode_thor
bin/mlvc_decode_thor
bin/mlvc_multi_thor
configs/five/contexts/{encode,decode}-0..4.toml
configs/five/batch5/{encode,decode}-0..4.toml
models/manifest_thor.json
models/manifest_thor_batch5.json
engines/
src/MLVC/ports/thor/tools/
```

`src/MLVC` 是本仓库源码。脚本优先使用安装的 `configs/five`，缺失时读取
源码中的配置模板；遥测和回执脚本优先读取源码工具目录，也兼容已安装的
`tools/five_telemetry.py` 和 `tools/collect_latency.py`。无需 style-final 临时配置。
只测 contexts/processes 时不需要 batch5 引擎；测 batch5 前须准备实际 N=5 引擎。

电脑需要 Python 3 和 OpenSSH，能够免密连接两块板卡。脚本顶部四个常量集中
定义 SSH 目标、工作目录和测试 RTSP 服务：`ENCODER`、`DECODER`、`ROOT`、
`RTSP_BASE`。默认为 xidian@192.168.5.16、xidian@192.168.5.17 和
rtsp://192.168.5.3:18554。迁移网络时修改这些常量；业务 TOML 中的地址仍独立配置。

RTP/RTCP 端口为 39480–39489，解码回执 UDP 为编码板上的 39500–39504。
允许板间这些 UDP 端口，以及两板访问测试服务 TCP 18554。

## 输入和服务

编码板放置 `data/video/five-13fps-120s.mp4`：1920×1080、H.264、13 FPS、
连续至少 120 秒。素材不随源码分发。有较短的相同格式视频时，可以先生成
有限长度的连续 MP4，再通过 ffprobe 确认分辨率、帧率和时长：

```bash
mkdir -p data/video
ffmpeg -stream_loop -1 -i input-13fps.mp4 -t 120 -an -c:v copy \
  -movflags +faststart data/video/five-13fps-120s.mp4
ffprobe -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,avg_frame_rate,duration \
  -of json data/video/five-13fps-120s.mp4
```

需要先启动支持 TCP RTSP 发布和读取的独立 MediaMTX，监听 18554，允许
`five-in0..4` 和 `five-out0..4` 路径。素材由脚本通过 FFmpeg 实时发布，
使用 -c:v copy；语义编解码程序的视频输入仍由 NVDEC 处理，输出仍使用 NVENC。
测试期间停止其他 GPU 编解码、模型构建或质量导出，避免混入并发负载。

## 执行

从本仓库根目录在电脑上运行，先测当前默认方案：

```powershell
python ports/thor/tools/benchmark_five.py --scheme contexts --frames 780 `
  --target-fps 12 --decoder-entropy-workers 1 --binary-dir bin `
  --label five-contexts-12 --output artifacts/five-contexts-12.json
```

`--scheme processes` 启动五个独立进程；`--scheme batch5` 使用实际 N=5
引擎。13 FPS 的历史对照应显式指定 `--target-fps 13 --decoder-entropy-workers 4`。
`--binary-dir bin` 使用当前安装程序；`bin/bench` 仅兼容已保存的对照程序。

前 96 帧作为计时预热，建议使用 390 或 780 帧。每次程序超时 100 秒，
测试输入最长 120 秒；不要在不调整超时和素材的情况下使用任意长帧数。
脚本生成独立的带 label 配置和日志，并在退出时清理自己记录的进程。
它不清理外部 MediaMTX 或其他业务服务。

## 结果口径

JSON 记录各路完整帧数、编码 FPS、解码回执 FPS、延迟走势、RTP 和发布丢帧
指标，并保留 GPU/CPU 遥测。`complete=true` 要求每路编码、解码、回执和
RTSP 发布帧数都与要求一致，且没有 RTSP 发布丢帧；RTP 使用 fail 策略。
失败试跑也保存已获得的结果。

延迟从编码板 NVDEC 读帧就绪到同一板卡收到解码完成回执，使用同一时钟，
不含上游 RTSP 缓存、发布完成或浏览器显示。输出回执 FPS 高于目标可能是
消化起始积压；结合最后 120 帧延迟和延迟增长判断，不把追赶速度作为持续
源帧率。GPU 遥测原始时间属于各自板卡，不能直接跨板比较时间戳。

13 FPS 的历史结果见 [五路验证](FIVE_STREAM_VALIDATION.md)，12 FPS 的
解码窗口对照见 [解码延迟验证](DECODE_LATENCY_VALIDATION.md)。

## 本次脚本整理验证

2026-10-14：contexts/processes/batch5 两端共 30 份生成配置均通过 TOML
解析、帧率、端口、模型 manifest 和 RTP 完整验帧策略检查。五路公开模板的
390 帧/路联调完整通过，1950 帧均编码、解码、回执并 RTSP 发布，无接收丢包
或队列丢帧。该短轮编码/解码回执均值为 11.51/10.76 FPS/路，低于此前两轮。

一次 780 帧补测中，编码程序在启动阶段退出，主日志报告
`timeout: the monitored command dumped core`；解码未收到帧而超时，结果
明确标记为不完整。随后在 GDB 下诊断重跑完整通过 3900 帧，无丢包丢帧，
未复现崩溃。该失败尚未定位原因，也未作任何宣称修复它的 C++ 修改。
诊断运行带有调试器，不作为普通性能对照；失败和重跑原始记录均保留。

此前 336–345 ms 是特定两轮的后期统计，不能当作不同运行中的固定延迟保证。
当前 12 FPS 是输入抽帧目标；本轮脚本整理只验证完整运行与配置独立性，
不宣称已经证明长期持续五路 12 FPS。第一方 276 个 C++/CUDA 文件格式检查和
Thor 约定过滤项的 cpplint 检查通过；编解码 C++ 内容保持此前 15 项测试通过
的版本，未因本次脚本与文档整理而改变。
