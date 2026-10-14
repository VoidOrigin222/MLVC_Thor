# NVIDIA Thor 双板运行

编码板：`xidian@192.168.5.16`；解码板：`xidian@192.168.5.17`。
两块板的运行目录均为 `/home/xidian/workspace/MLVC_Thor`。

MLVC 使用 TensorRT/CUDA 推理，运动估计使用 NVENC 单参考 H.264 代理及原有运动提取器，熵编解码保留原有 CPU 实现。此目标不链接 Ascend/CANN。旧 Ascend 实现仍由默认 `MLVC_BACKEND=acl` 构建；Thor 使用独立目标。

## 启动

先启动解码端，再启动编码端。MediaMTX 由使用者单独运行。

解码板：

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/check_runtime.sh
bash scripts/run_decode.sh
```

编码板，三种输入任选一种：

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/check_runtime.sh
bash scripts/run_encode.sh offline
# 或：bash scripts/run_encode.sh rtsp
# 或：bash scripts/run_encode.sh camera
```

按 Ctrl+C 停止当前进程。脚本允许指定其他 TOML，例如：

```bash
bash scripts/run_encode.sh offline configs/my_encoder.toml
bash scripts/run_decode.sh configs/my_decoder.toml
```

## 修改配置

| 配置 | 输入字段 | 默认值 |
|---|---|---|
| `configs/encoder_offline.toml` | `input_video` | `data/video/614lab.mp4` |
| `configs/encoder_rtsp.toml` | `input_rtsp` | `rtsp://192.168.5.3:8554/camera-original` |
| `configs/encoder_camera.toml` | `input_camera_device` | `/dev/video0`，MJPG，1920×1080，30 FPS |
| `configs/encoder_fp16.toml` | `input_frame_dir` | `data/fp16/614lab` |

编码配置增加 `target_fps`，默认 30，可以改成 25，设为 0 关闭抽帧。视频文件和 RTSP 自动读取输入帧率，摄像头使用采集后协商的帧率；源未提供有效帧率时回退为 30 FPS。输出标称帧率为 `min(输入帧率, target_fps)`：60→30 每两帧保留一帧，50→30 等非整数比例按源时间均匀取帧，输入低于目标时全部保留且不补帧。抽帧发生在 BGR 转换、GPU 预处理、运动估计和语义编码之前；帧编号重新连续排列，参考状态只基于保留帧更新。`frame_num` 仍表示要编码的输出帧数。

离线 MP4 配置默认 `video_decode_backend="nvdec"`：H.264/HEVC 通过 NVIDIA NVDEC 解码，CUDA NV12/P010/P016 帧直接在 GPU 上转换为模型 FP16 输入，再由 TensorRT 进行语义编码。`motion_backend="nvenc"` 使用 NVENC 编码 H.264 运动代理。语义压缩模型本身仍由 TensorRT/CUDA 执行，NVENC 不替代模型。输入 GPU 图像不经过 CPU BGR 中间帧，也不需要整帧下载再上传。

Thor 已移除 OpenCV 视频解码回退，只接受 NVDEC 支持的 H.264/HEVC 视频。其他视频编码须先转码。必须有 NVIDIA 的 `libnvcuvid.so.1`，且程序链接的 FFmpeg 共享库包含 `h264_cuvid` 和 `hevc_cuvid`。RTSP 也使用 NVDEC；摄像头仍使用 V4L2。GPU 预处理保持原 BT.709 全范围 YUV444 模型输入约定，包括原 OpenCV 默认 YUV→BGR8 数值映射；使用双线性色度插值，与软件缩放器的插值、舍入不同，输入和码流不保证逐比特相同。本次不增加 HDR 色调映射。

这是视频时间轴上的采样，不是对离线处理速度做休眠限速；离线处理的性能 FPS 可以高于 `target_fps`，码流标称帧率和播放时长按抽帧后的帧率计算。FP16 验证目录和合成输入维持原有帧序列，不应用此视频采样参数。

只选择一个输入。`input_video` 兼容 RTSP URL，推荐用明确的 `input_rtsp` 字段。RTSP 读取采用 TCP；打开和单次读取超时为 3 秒；直播流信息探测最多 8 秒。RTSP 通过预读五个解码帧的 PTS 间隔确定输入帧率，预读帧仍参与后续处理，避免将 H.264 的场时钟误当作帧率。断流会报错退出，避免把不完整直播流当作正常文件结束；重启编码器和解码器可建立新会话。

IP 更改后，需要修改编码配置的 `output_transport_host`（解码板 IP），以及 RTSP 输入/输出中的服务 IP。
双板 RTP 端口默认 `39340`，两端必须一致；相邻 RTCP 端口也应允许通信。
解码输出地址配置在 `decoder_rtp.toml` 的 `output_transport_rtsp_url`，默认 `rtsp://192.168.5.3:8554/ulbvc`。摄像头原图发布地址配置在 `encoder_camera.toml` 的 `camera_rtsp_url`。

`frame_num=-1` 表示离线文件读到结尾或持续读取直播。离线文件以最大处理速度运行，抽帧后的标称帧率写入码流；需要实时播放的离线源，可在外部以实时速度发布为 RTSP，再使用 RTSP 输入。RTP 文件输入结束后发送现有 EOS 标记，解码端处理完剩余帧后退出。

当前模型固定为 `[1,3,1088,1920]` FP16、BT.709 全范围 YUV444。1920×1080 输入在底部复制边缘补齐 8 行；没有降低分辨率。使用较小图像时复制边缘补齐；图像不能超过模型尺寸，运动代理尺寸须至少 128×128，NV12 可见尺寸须为偶数。实际保留的标称帧率写入码流，代理编码帧率取最近的整数。

摄像头接口保留了 MJPG 采集、原图 RTSP 发布和 NV12 运动输入。未连接摄像头，按要求没有进行摄像头实机测试。原 Ascend 驱动和 `.ko` 不能用于 Thor；Thor 摄像头使用系统 V4L2 驱动。旧的相机预加载基准参数在 Thor 上要求为 0，非零会明确报错。

## FP16 文件联调

```bash
# 编码板
bash scripts/run_encode.sh fp16
# 将 data/bitstreams/fp16.mlvc 复制到解码板的同一路径，然后在解码板执行：
bash scripts/run_decode.sh configs/decoder_file.toml
```

`decoder_file.toml` 输出 PNG。性能测试使用 `format="none"`；需要保存重建张量可用 `format="fp16"` 并设置输出目录。参考特征转换使用第三个 `MLVCReferenceFromFrame` 引擎，须与主编解码模型一起保留。

## 编码性能配置

Thor 默认 `input_device_resident=true`，视频/RTSP/摄像头 FP16 输入和编码参考状态保留在 GPU。默认 NVENC 运动代理 `motion_decode_threads=4`，同时使用四个 H.264 slice 和四个软件 slice 解码线程；单 slice 设为 1。多 slice 可能轻微改变运动决策。需要旧版 CPU 路径和运动行为时，同时设置 `input_device_resident=false`、`motion_decode_threads=1`。Thor 不再接受 `motion_backend="libx264"` 或 `motion_x264_*` 参数。

运动子步骤计时、双板 FPS/延迟、回归质量和测量边界见 [OPTIMIZATION.md](OPTIMIZATION.md)。编码端和解码端同步维护；回执探针只用于基准测试。运行脚本和地址配置方法保持一致。

MP4 硬件输入默认启用 NVDEC；已修复模型并发下的帧拷贝顺序问题。最新逐帧一致性及性能验证见 [NVDEC_VALIDATION.md](NVDEC_VALIDATION.md)。

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

每路拥有独立配置、参考帧、运动估计、熵编解码和 RTP 会话。使用五个互不重叠的
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
bash scripts/run_five.sh decode contexts
# 或：bash scripts/run_five.sh decode batch5
# 或：bash scripts/run_five.sh decode processes
```

再启动编码板：

```bash
bash scripts/run_five.sh encode contexts
# 或：bash scripts/run_five.sh encode batch5
# 或：bash scripts/run_five.sh encode processes
```

修改 `configs/five/<模式>/encode-0.toml` 至 `encode-4.toml` 的五个输入地址、
解码板 IP，以及对应解码配置的 RTSP 发布地址。contexts 模板以每路 12 FPS 为目标，
解码 entropy_workers=1；batch5 模板保留每路 13 FPS 和 4 worker。
12 FPS 的重复验证和延迟说明见 [DECODE_LATENCY_VALIDATION.md](DECODE_LATENCY_VALIDATION.md)。
使用公开配置模板复现五路基准的方法见 [BENCHMARKING.md](BENCHMARKING.md)。
MediaMTX 由使用者启动。

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
