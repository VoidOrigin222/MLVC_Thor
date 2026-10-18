# NVIDIA Thor 双板运行

编码板：`xidian@192.168.5.16`；解码板：`xidian@192.168.5.17`。
两块板的运行目录均为 `/home/xidian/workspace/MLVC_Thor`。

MLVC 使用 TensorRT/CUDA 推理，运动估计使用 NVENC 单参考 H.264 代理及原有运动提取器，熵编解码保留原有 CPU 实现。此目标不链接 Ascend/CANN。旧 Ascend 实现仍由默认 `MLVC_BACKEND=acl` 构建；Thor 使用独立目标。

## 启动

先启动解码端，再启动编码端。MediaMTX 由使用者单独运行。

```bash
# 解码板
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_decoder.sh configs/decoder.toml

# 编码板
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_encoder.sh configs/encoder.toml
# 自定义配置：bash scripts/run_encoder.sh configs/my_encoder.toml
```

编码仅接受 RTSP 输入。五路使用 `bash scripts/run_encoder.sh configs/encoder.toml`，
对应解码使用 `bash scripts/run_decoder.sh configs/decoder.toml`，默认每路目标 12 FPS。
单路 `configs/encoder_rtsp.toml` 默认目标 30 FPS。按 Ctrl+C 停止。

## 输入和硬件视频

```toml
input_rtsp = "rtsp://192.168.5.3:8554/camera-original"
video_decode_backend = "nvdec"
input_device_resident = true
motion_backend = "nvenc"
target_fps = 30
```

只保留 `input_rtsp`。支持 `rtsp://`、`rtsps://` 和 NVDEC 支持的 H.264/HEVC 码流；
离线视频、摄像头、FP16、合成输入及 `input`/`input_video` 别名都不再接受。
旧参数会在加载配置时明确报错。文件或摄像头应在外部先发布为 RTSP。
底层 NVDEC 文件读取仅保留给硬件回归测试和诊断工具，不作为生产编码入口。
原 Ascend 构建保持原有接口。

处理链路为 RTSP 解封装 → NVDEC → CUDA FP16 预处理 → TensorRT 语义编码
→ RTP → TensorRT 语义解码 → CUDA NV12 转换 → NVENC → RTSP 发布。
NVENC 同时负责编码侧的 H.264 运动代理，不能替代语义模型。CPU 保留熵编解码、
网络封装和运动向量提取；视频输入没有 OpenCV/V4L2 读取或软件解码回退。
RTSP 输入要求 `input_device_resident=true`，不支持退回整帧 CPU BGR 中转路径。
解码发布配置 `output_transport_rtsp_encoder="nvenc"`。

Thor 默认用 CUDA Graph 提交固定形状的单帧模型。每个执行上下文使用独立的固定
GPU 输入输出缓冲，复制调用方的数据后重放图，避免参考缓冲和发布缓冲轮换时
改变已捕获的地址。三个模型均有普通提交与图重放的逐字节输出对照工具。
图捕获不支持时记录原因并使用普通 `enqueueV3`；诊断对照可在启动前设置
`MLVC_THOR_CUDA_GRAPH=0`。实际捕获/重放计数记录为 `cuda_graph model=...`。

五上下文模式默认按 GPU 设备串行执行模型调用，采集、熵编解码、NVENC 和
网络仍并行，避免整帧图同时运行时积压。CPU 输入直接上传至模型固定缓冲。
实时无 B 帧输入启用 CUVID 低延迟；有 B 帧时保留重排。调度、拷贝与显示缓冲的
测量口径及复现命令见 [混合五路时延验证](MIXED_LATENCY_VALIDATION.md)。
batch5 继续使用原批调度器，不使用此单上下文图路径。

参考特征平移缓存按路复用并使用各自的计算流；NVENC 使用独立非阻塞复制流，
接收 GPU NV12 图像。吞吐和积压验证见 [时延修复记录](LATENCY_FIX_VALIDATION.md)，
后续启动积压和抖动改进见 [五路抖动验证](JITTER_FIX_VALIDATION.md)。

`target_fps` 可调整，例如 30、25、12；设为 0 关闭抽帧。低于目标帧率时不补帧。
通过预读五帧的 PTS 间隔探测真实输入帧率，预读帧继续参与处理。
抽帧发生在 GPU 预处理、运动估计和语义编码前，保留帧重新连续编号。
RTSP 使用 TCP，打开和单次读取超时 3 秒，流探测最多 8 秒。断流报错退出，
不将断流当作正常结束。`frame_num=-1` 表示持续运行；正值表示完整处理指定帧数。

更换 IP 时修改编码端 `input_rtsp` 和 `output_transport_host`（解码板 IP），
以及解码端 `output_transport_rtsp_url`。默认 RTP/RTCP 端口为 39340/39341，
五路模板使用 39480–39489；两端配置必须对应，避免与 UI 统计端口冲突。

模型为 `[1,3,1088,1920]` FP16、BT.709 全范围 YUV444，1080 行输入底部补齐
8 行，保持原分辨率。运动代理尺寸至少 128×128，NV12 可见尺寸必须为偶数。
GPU 预处理维持原颜色映射和双线性色度插值；不增加 HDR 色调映射。
运动子步骤计时及历史性能测量见 [OPTIMIZATION.md](OPTIMIZATION.md)，
NVDEC 一致性验证见 [NVDEC_VALIDATION.md](NVDEC_VALIDATION.md)。其中离线/FP16
记录属于历史验证，不代表当前生产输入接口。

## 模型和运行依赖

`models/manifest_thor.json` 引用 `engines/` 和 `models/onnx/` 中的模型、PMF、sidecar。两块板使用相同引擎及 manifest，启动时校验大小和 SHA-256；改变引擎后需重新生成 manifest，并把整套文件同步到两端。

本次运行环境：Jetson AGX Thor、Ubuntu 24.04 aarch64、CUDA 13.0、TensorRT 10.13.3.9、NVIDIA 驱动 580.00、OpenCV 4.8 和 FFmpeg 6.1 系列共享库。可执行文件依赖板卡系统安装的这些库；运行包没有复制驱动、CUDA 和 TensorRT 系统库。详见交付包的 `runtime_dependencies.txt`。这不是可直接用于其他 GPU/系统版本的通用 TensorRT 引擎包。

TensorRT 在编码板加载解码板构建的引擎时会报告设备型号兼容性警告。已核对两块板为不同序列号的 Thor，GPU 都是 SM 11.0、20 个 SM，并完成实测；更换硬件/软件版本时应重新构建并联调，不能仅依赖文件复制。

## 构建

```bash
cmake -S src/MLVC -B build -DMLVC_BACKEND=tensorrt \
  -DCMAKE_BUILD_TYPE=Release -DCUDAToolkit_ROOT=/usr/local/cuda
cmake --build build -j6
ctest --test-dir build --output-on-failure
```

本次解码板没有系统 CMake 和 FFmpeg 开发包，已在 `deps/sysroot` 私有安装开发依赖，没有修改系统包。该板上的构建命令为：

```bash
export LD_LIBRARY_PATH="$PWD/deps/sysroot/usr/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
deps/sysroot/usr/bin/cmake -S src/MLVC -B build -DMLVC_BACKEND=tensorrt \
  -DCMAKE_BUILD_TYPE=Release -DCUDAToolkit_ROOT=/usr/local/cuda \
  -DCMAKE_PREFIX_PATH="$PWD/deps/sysroot/usr"
deps/sysroot/usr/bin/cmake --build build -j6
deps/sysroot/usr/bin/ctest --test-dir build --output-on-failure
```

TensorRT 引擎构建方法（逐个模型，完成后同步整套引擎到另一块板）：

```bash
/usr/src/tensorrt/bin/trtexec --onnx=models/onnx/MLVCEncoder.onnx \
  --saveEngine=engines/MLVCEncoder.engine --fp16 --skipInference
# 对 MLVCDecoder 和 MLVCReferenceFromFrame 同样执行
python3 src/MLVC/ports/thor/tools/create_manifest.py "$PWD"
```

`--skipInference` 避免 trtexec 随机生成无效的 QP 输入；实际有效输入由编解码联调验证。

## 五路 RTSP

编码端使用 `configs/encoder.toml`，解码端使用 `configs/decoder.toml`，每个文件集中配置五路。
每路拥有独立参考帧、运动估计、熵编解码和 RTP 会话。使用五个互不重叠的
RTP/RTCP 端口对。`mlvc_multi_thor` 提供两种模式：

- `contexts`：一个进程、每路独立 TensorRT 执行上下文和 CUDA stream，共享引擎权重。
- `batch5`：模型输入输出实际为 N=5，每次 enqueue 处理一批；每路流程保持 N=1
  的逻辑接口，由批量调度器在 GPU 上合并输入、拆分输出。

每路管线使用三个模型，因此 contexts 模式每个模型有五个执行上下文，合计 15 个。
batch5 模式每个模型有一个执行上下文，合计三个。首批最多等待 5 秒对齐启动，之后批量调度等待最多 40 ms；
不足五路时复制第一路填满未使用槽位，丢弃填充槽位输出，防止断流或 EOF 卡住其他路。
`MLVC_THOR_BATCH_WAIT_MS` 可调整后续批次等待上限（1–1000 ms）。满五路立即执行。
`batch_stage=...` 日志记录真实 enqueue 次数、满批次数、请求数量及等待上限。

先启动解码板：

```bash
bash scripts/run_decoder.sh configs/decoder.toml
# 或：bash scripts/run_five.sh decode batch5
# 或：bash scripts/run_five.sh decode processes
```

再启动编码板：

```bash
bash scripts/run_encoder.sh configs/encoder.toml
# 或：bash scripts/run_five.sh encode batch5
# 或：bash scripts/run_five.sh encode processes
```

修改 `configs/five/<模式>/encode-0.toml` 至 `encode-4.toml` 的五个输入地址、
解码板 IP，以及对应解码配置的 RTSP 发布地址。contexts 模板以每路 12 FPS 为目标，
解码 entropy_workers=1；batch5 模板保留每路 13 FPS 和 4 worker。
12 FPS 的重复验证和延迟说明见 [DECODE_LATENCY_VALIDATION.md](DECODE_LATENCY_VALIDATION.md)。
使用公开配置模板复现五路基准的方法见 [BENCHMARKING.md](BENCHMARKING.md)。
MediaMTX 由使用者启动。

五路测试输入固定为两路静态、两路动态和一路实时计时器。先启动电脑上的 MediaMTX，
再在编码板单独的终端运行：

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_test_sources.sh
# 更换服务器：bash scripts/run_test_sources.sh --rtsp-base rtsp://新IP:8554
```

独立参数文件为 `configs/five/test_sources.json`，地址默认
`rtsp://192.168.5.3:8554`，分辨率 1920×1080、12 FPS。各路如下：

| 路径 | 场景 | 处理 |
|---|---|---|
| input0、input1 | 静态矿井 | 固定静态素材首帧，循环发布 |
| input2、input3 | 动态矿井 | 动态素材循环发布 |
| input4 | 实时计时器 | 显示经过时间和帧号，持续递增 |

静态缓存首次启动用 NVDEC 解码、NVENC 编码生成到 `data/video/generated/`。
随后四路素材只复制 H.264 码流，计时器一路使用 NVENC 实时编码；没有软件编码回退。
动态素材必须预先准备为配置中的分辨率和帧率，脚本会检查，不会静默改变速率。
两路静态使用同一张图，两路动态使用同一素材，不代表四个独立采集设备。
保持终端打开，在另一个终端启动编码程序；Ctrl+C 或终端断开会停止本次五路源。
若任一路退出，脚本会停止本次其余源，并保留 `logs/test-sources/<启动编号>/`。

计时器通过原图与重建图的读数差观察显示时差；12 FPS 的时间步长约 83 ms。
测量时先在 UI 参数中将原图显示延时设为 0；原有 200 ms 补偿会缩小可见差值。
读数差包含两条播放链路的相对缓冲差，不能直接当成相机曝光到屏幕的绝对总时延。

batch5 引擎放在 `engines/batch5/`，使用独立的 `models/manifest_thor_batch5.json`。
该 manifest 的 I/O shape 描述每路 N=1 逻辑接口，实际 N=5 引擎 shape 保存在
受 SHA-256 校验的 `models/batch5/execution.json`。运行时逐项检查实际引擎的
shape、dtype、方向和连续布局。两块板必须同步整套 batch 引擎、manifest、
ONNX、执行元数据和原 sidecar/PMF；单路程序不直接加载该批量 manifest。

实测结果和测量边界见 [FIVE_STREAM_VALIDATION.md](FIVE_STREAM_VALIDATION.md)。
格式化与命名约定见仓库根目录 `CODING_STYLE.md`。

## RTP 接收策略

每路拥有独立的 socket、RTP 重组器、帧队列、序列号/SSRC、NACK/PLI 和参考状态。
收包线程与推理线程分离。五路模板采用实时恢复策略：帧队列有界，满时只丢弃旧的
完整 EFU，保留 SCU 和 EOS；发现引用链缺口后不解码依赖坏参考的 P 帧，等待
合法的随机访问帧或已有 LTR 恢复点。发送限频 PLI；当前 translation_warp 模式
保持固定 GOP，仍依赖周期性 I 帧恢复，不保证立即响应 PLI。

| 配置 | 单路兼容默认 | 五路实时模板 | 完整帧基准 |
|---|---:|---:|---:|
| `input_rtp_queue_capacity` | 64 | 64 | 256 |
| `input_rtp_queue_bytes` | 134217728 | 67108864 | 67108864 |
| `input_rtp_overflow_policy` | block | recover | fail |
| `input_rtp_pending_frames` | 64 | 256 | 256 |
| `input_rtp_reorder_wait_ms` | 100 | 500 | 500 |
| `input_rtp_eos_wait_ms` | 2000 | 5000 | 5000 |
| `input_rtp_loss_policy` | recover | recover | fail |

完整帧基准不允许靠丢帧提高 FPS：缓存溢出、引用缺口或缺失首帧均报错，并核验
编码数、解码数、解码回执数和 RTSP 发布数全部相同。实时模板允许丢弃并恢复，
因此不应把它与完整帧基准混用。增大缓存只避免短时 UDP 收包阻塞，不增加模型吞吐，
也不能保证长期运行无积压。

日志分别记录 `rtp_lost_packets`、`rtp_queue_overflows`、`rtp_queue_dropped_units`、
`rtp_crc_failed_units`、`rtp_dependency_gaps`、`rtp_skipped_frames`、`rtp_recoveries`、
`rtp_queue_max_depth`，以及 RTSP 的 `forward_dropped_frames`。请求的 socket 缓冲为
16 MiB，但系统可能限制该值；实际值由 `rtp_socket_receive_buffer` 报告。
