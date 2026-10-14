# NVDEC 重复编码一致性验证（2026-10-14）

已修复模型并发运行时 NVDEC 像素不一致的问题。语义模型使用 TensorRT FP16，MP4 解码使用 NVDEC，运动代理使用 NVENC。

## 定位与修复

测试输入 `614lab.mp4`：HEVC Main 10，1920×1080，30 FPS，537 帧，关闭抽帧及休眠限速。

独立读取三次，全部 537 帧的 NVDEC 原始 P010 像素、CUDA 转换后的 FP16 张量 SHA-256 都相同。有界预读队列与独立 reader 也一致。但在真实模型并发流水线中，两轮分别有 313/295 帧的原始像素与张量不同，PTS 均正确。差异已经发生在运动估计和 TensorRT 推理之前。固定运动位移 CSV 回放后，三次码流仍不同，排除了仅由运动估计造成的解释。

旧三次码流大小为 800482、784409、801335 字节，最大差异约 2.2%；运动位移有 228/212 帧不同，语义 payload 有 493/488 帧不同，QP 相同。完整重建对比的最差帧平均亮度绝对误差约 9%，不能仅按码率差异小忽略。这些误差指标只用于回归定位，不用于运行时质量/码率控制。

当前链接的 FFmpeg 6 CUVID 实现将 `CUVIDPROCPARAMS::output_stream` 留为零，映射/后处理使用 legacy default stream，再向 `AVCUDADeviceContext::stream` 提交 GPU→GPU 异步拷贝。原先设置的 `cudaStreamNonBlocking` 绕过了 default stream 顺序，GPU 并发负载下可能复制尚未正确就绪的像素。在 receive 返回后只等拷贝完成，不能补回拷贝前缺失的顺序。

修复为 `cudaStreamDefault` 的有序拷贝 stream，并保留每次 receive 后的 stream fence。预处理继续使用非阻塞 stream，通过事件等待解码帧。生产路径没有增加整帧 CPU 下载或 BGR 中间帧。接口行为可核对 [FFmpeg 6 官方 CUVID 源码](https://ffmpeg.org/doxygen/6.0/cuviddec_8c_source.html) 的 `cuvid_output_frame`。

## 修复后验证

- 三次真实编码并发诊断：每次 537 帧，原始像素、FP16 张量和 PTS 均与独立读取基准一致。
- 关闭像素下载诊断，再跑三次：输出文件仍逐比特相同，均为 **630604 字节**，运动位移非零帧数均为 **472**。
- 六次文件 SHA-256 均为 `62f2d4b7b666e96cfaa55e9b101a38560db74eba0ecd474e719d3fb0b29ddc7e`；运动元数据、编码 payload、熵编码及文件输出在本测试下均一致。
- 原业务解码程序完整解码 **537/537 帧**。双板 RTP 联调也完整解码 **537/537 帧**，无 forward 丢帧。
- CTest **13/13** 通过，覆盖 NVDEC 顺序、采样、补齐、关闭与保留槽生命周期及原有 CUDA、RTP、码流和模型校验。
- 软件 OpenCV MP4 与原 FP16 文件输入完整回归，保持旧码流 SHA-256：分别为 `1fe56b70546d181181729b1e757c47d7e2639526cc4436ebfd2a3cc0113afb89`、`74ad52172e1e9be362ba49b2879ba88182dfce90c02e93320fe16e28d02fc2a6`。
- 实际 120 帧 60 FPS H.264 视频，抽帧到 30/25 FPS 分别编码 60/50 帧；单元测试另行验证对应采样顺序。

## 性能及边界

537 帧，排除前 96 帧预热，三次均值；详细计时探针开启，像素下载诊断关闭。GPU 利用率按主编码 stage 第 96 帧至最后一帧对齐，100 ms 采样。

| 指标 | 软件解码输入 | 修复后 NVDEC |
|---|---:|---:|
| 文件编码 FPS | 44.25 | **69.77** |
| 三次 NVDEC FPS | — | 69.9643 / 69.7468 / 69.6070 |
| GPU 利用率 | 60.1% | **92.9%** |
| NVDEC 利用率 | 0% | 7.4% |
| NVENC 利用率 | 13.4% | 13.0% |
| NVDEC 取包/解码/拷贝等待墙钟 | — | 0.79 ms/帧 |
| CUDA NVDEC→FP16 stream | — | 0.24 ms/帧 |
| 主编码流程墙钟 | 14.94 ms | 14.24 ms/帧 |

比软件解码输入快约 **57.7%**；比此前不稳定的 70.40 FPS 低约 **0.9%**。NVDEC 调用耗时包含取包、API 调用及拷贝等待，不是硬件纯计算时间。主编码流程还包含参考管理、周期性重建；各流水线阶段不能直接相加。

双板不限速文件推送单次编码 70.27 FPS、解码 67.42 FPS，FP16-ready 到解码回执平均约 1.46 s，包含流初始化及接收积压；本修复不宣称端到端延迟改善，也不能将该数值当作实时 30 FPS 或浏览器显示延迟。

软件与 NVDEC 使用不同色度插值和舍入，软件参考还有末尾三行黑边，所以两条输入路径不保证码流相同。34 帧抽样排除 CPU 黑边后的输入 Y-PSNR 平均 44.50 dB。本次不增加 HDR 色调映射。

## 诊断与原始记录

`tools/probe_nvdec_surfaces.cc` 对原始像素和转换张量记录逐帧 SHA-256；`tools/dump_video_tensor.cc` 的 `--hash-only` 对有界队列路径逐帧记录 SHA-256，避免保存大量 FP16 文件。两个诊断工具需要系统 `libcrypto.so.3`。

真实编码诊断使用 `MLVC_THOR_INPUT_DIGEST=logs/input.csv`，记录帧号、PTS、原始像素和 FP16 的 SHA-256。默认未设置时不下载像素、不加载 libcrypto、不计算哈希。诊断计时不能替代正式性能计时。

原始 CSV、三轮 trace、性能 JSON 及回归日志保存在各板 workspace `logs/` 和 Windows `MLVC_310P1/artifacts/thor-20261013/optimization/`。
