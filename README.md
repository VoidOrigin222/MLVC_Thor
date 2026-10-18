# MLVC Thor

五路原图与语义压缩的配对监控界面、独立参数文件和 Windows 启动器见
[五路视频 UI](tools/ui/README.md)。

一路静态、通道 2 实时计时器和三路不同动态场景的测试源用 `bash scripts/run_test_sources.sh`
在编码板启动，参数见 [Thor 运行说明](ports/thor/README.md#五路-rtsp)。
通道 2 计时器及三个不同动态场景的验证见
[测试源布局记录](ports/thor/TEST_SOURCES_VALIDATION.md)。
本机及双板的目录、归档位置和清理记录见 [目录说明](ports/thor/WORKSPACE_LAYOUT.md)。
混合五路的积压定位、时延修复和长测见
[混合场景验证](ports/thor/MIXED_LATENCY_VALIDATION.md)。
五路编码使用 [encoder.toml](ports/thor/configs/encoder.toml)，解码使用
[decoder.toml](ports/thor/configs/decoder.toml)，启动方式见
[总配置说明](ports/thor/MULTI_CONFIG.md)。

MLVC 编解码在 NVIDIA Jetson AGX Thor 上的双板部署。模型使用 TensorRT FP16，
RTSP 输入使用 NVDEC，运动代理及重建视频输出使用 NVENC；熵编解码和
H.264 运动向量提取保留 CPU 实现。Thor 目标不依赖 Ascend/CANN，已移除软件
视频解码回退和 libx264 编码路径。

生产编码接口仅接受 `input_rtsp`，使用 NVDEC 解码及 GPU 预处理，按目标帧率抽帧。
离线视频、摄像头、FP16 及合成输入配置不再接受；需要播放文件或相机时，先在外部发布为 RTSP。
五路运行可选独立进程、单进程独立 TensorRT 上下文或实际 batch=5。
各路参考状态、熵编解码及 RTP 会话独立。RTP 接收提供完整验帧和实时恢复策略。

## 构建

已验证环境：Ubuntu 24.04 aarch64、CUDA 13.0、TensorRT 10.13.3.9、OpenCV 4.8、
FFmpeg 6.1 共享库。需要对应开发文件及 NVIDIA NVDEC/NVENC 驱动。

```bash
cmake -S . -B build -DMLVC_BACKEND=tensorrt \
  -DCMAKE_BUILD_TYPE=Release -DCUDAToolkit_ROOT=/usr/local/cuda
cmake --build build -j6
ctest --test-dir build --output-on-failure
```

程序生成于 `build/ports/thor/`：`mlvc_encode_thor`、`mlvc_decode_thor` 和
`mlvc_multi_thor`。仓库包含完整源码、第三方头文件、配置模板和工具。
ONNX、TensorRT 引擎、运行库和输入素材需单独准备，部署布局、模型转换、
manifest 校验与启动步骤见 [Thor 运行说明](ports/thor/README.md)。
TensorRT 引擎应按目标 GPU 和 TensorRT 版本构建。

## 运行与验证

安装运行目录中的程序、模型和配置后，先启动解码，再启动编码：

```bash
# 解码板
bash scripts/run_five.sh decode contexts
# 编码板
bash scripts/run_five.sh encode contexts
```

将 `contexts` 改为 `batch5` 或 `processes` 可切换方案；单路及输入配置见
[运行说明](ports/thor/README.md)。MediaMTX 由使用者单独启动，IP、输入 URL、
输出 URL 和 RTP 端口均通过 TOML 配置修改。

五上下文默认目标为每路 12 FPS，解码熵窗口为 1 帧。2026-10-16 修复后，
五路连续各 3120 帧完整传输，去掉前 96 帧的平均读帧至解码回执时延约 275 ms，
P95 约 373 ms，最后一分钟平均约 273 ms；测量不包含采集前段和显示端。
定位过程、启动暂态、尖峰及复现条件见 [时延修复记录](ports/thor/LATENCY_FIX_VALIDATION.md)。
此前两轮低时延结果保留在 [解码窗口验证](ports/thor/DECODE_LATENCY_VALIDATION.md)。

2026-10-14 的 13 FPS 双板验证：五上下文和 batch=5 均完整完成每路 780 帧，无丢包丢帧。
五上下文编码/解码稳态平均为 12.65/11.85 FPS/路，batch=5 为 12.47/10.58 FPS/路，
当前均未达到持续五路各 13 FPS。完整源码独立构建，15 项测试通过。
详见 [五路验证记录](ports/thor/FIVE_STREAM_VALIDATION.md)、
[NVDEC 验证](ports/thor/NVDEC_VALIDATION.md) 和
[性能优化记录](ports/thor/OPTIMIZATION.md)。
五路测试的环境准备、输入素材和计时口径见
[基准复现说明](ports/thor/BENCHMARKING.md)。

目录 `common/`、`mlvc/` 和 `application/` 为共享编解码与传输代码，`ports/thor/`
为 TensorRT/CUDA、硬件视频、五路调度、测试和部署工具。
[代码风格约定](CODING_STYLE.md) 说明格式、命名及保留的异常机制。
原 Ascend 目标及历史配置保留，说明见 [Ascend 部署文档](README_ASCEND.md)。
