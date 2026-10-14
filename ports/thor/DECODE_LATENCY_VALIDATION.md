# 五上下文解码延迟验证：12 FPS

日期：2026-10-14。编码板 192.168.5.16，解码板 192.168.5.17。
使用已验证的 fe60d53 版本可执行程序，没有修改 C++、模型、QP 或分辨率。
五路仍使用同一 1920×1080、13 FPS H.264 RTSP 视频的不同时间偏移；编码器
设置 target_fps=12，按时间轴抽帧。每轮每路 780 帧，约 65 秒输入。

测试顺序为 entropy_workers=4、1、1、4。每次启动新的进程、会话和参考状态，
两板不同时执行引擎构建或其他编解码测试。RTP 使用完整验帧的 fail 策略，
接收队列 256 个单元、64 MiB。四轮均完整编码、解码、回执并 RTSP 发布
5×780=3900 帧，接收丢包、队列丢帧、引用缺口和 RTSP 发布丢帧均为 0。

| 轮次 | entropy_workers | 编码平均 FPS/路 | 解码回执 FPS/路 | 全窗口平均延迟 | 最后 120 帧平均延迟 |
|---|---:|---:|---:|---:|---:|
| w4 | 4 | 11.80 | 11.02 | 5.09 秒 | 6.48 秒 |
| w1 | 1 | 12.00 | 12.31 | 0.93 秒 | 345 ms |
| w1-repeat | 1 | 11.98 | 12.22 | 0.62 秒 | 336 ms |
| w4-repeat | 4 | 11.98 | 12.28 | 1.76 秒 | 1.29 秒 |

计时排除前 96 帧；最后 120 帧统计额外用于检查积压走势。解码回执 FPS 超过
12，表示消化起始积压，并不是输入变为更高帧率。w1-repeat 最后 120 帧各路
回执 FPS 为 12.03–12.05。w1 两轮每路后期延迟均低于前期，没有发现持续增长；
接收队列峰值分别为 11–24、10–20 个单元。原 13 FPS 结果保留在
[五路验证](FIVE_STREAM_VALIDATION.md)。

4 worker 两轮吞吐差异较大；GPU 调度、启动状态和 RTSP 取到的视频片段有波动，
没有锁定 GPU 时钟。因此不能声称将 worker 从 4 改为 1 必然提高某个固定比例的
模型吞吐，也不能只用第一轮 4 worker 数据证明稳定的 5 秒到 0.3 秒收益。
1 worker 两次较一致的低延迟结果支持采用该配置，但尚未完成长时间真实五路源测试。

## 慢在哪里

消费线程在熵解码窗口满时才取出最早一帧，并先提交新帧后执行该旧帧的模型。
窗口容量绑定 entropy_workers。实时 12 FPS 下，4 帧窗口会等待约 4/12 秒，
1 帧窗口等待约 1/12 秒；配置改变可减少约 250 ms 的固定帧间等待。下一帧的
CPU 熵解码仍可与当前帧的模型执行重叠。

窗口之外还存在吞吐不足引起的动态积压：上一轮 13 FPS 输入的解码约 11.85
FPS/路，无法长期追上源。RTP 扩大缓存只保证有限测试完整接收，不能消除积压。

1 worker 两轮稳态模块计时（前 96 帧排除，五路均值）：

| 模块 | 每帧平均墙钟耗时 |
|---|---:|
| TensorRT 解码 stage，含准备/同步及并发竞争 | 69.30–72.56 ms |
| CPU 熵解码 | 12.02–12.05 ms |
| GPU YUV444 FP16→NV12 | 0.58–0.61 ms |
| NVENC 输出编码及取包 | 7.38–7.74 ms |
| RTSP 输出入队 | 1.89–2.04 ms |

模块运行在流水线不同线程，部分耗时相互重叠，不能直接相加或取倒数预测
吞吐。TensorRT stage 是当前最大处理阶段，但该计时包含五路并发及同步等待，
不能称为 GPU 纯 kernel 时间。RTP 收帧等待是等待上游供帧，也不是模型计算。

## 默认配置与复现

contexts 配置改为每路 target_fps=12、解码 entropy_workers=1；processes 模式
使用同一 contexts 配置。batch5 模板仍保留此前的 13 FPS/4 worker 设置，
这次没有重新测量或优化 batch5。

```bash
# 解码板先启动
bash scripts/run_five.sh decode contexts
# 编码板随后启动
bash scripts/run_five.sh encode contexts
```

窗口为 1 不表示串行处理五路：五个视频路仍各有独立模型执行上下文、CUDA
stream 和 CPU 熵工作线程。默认实时配置仍是 RTP recover；本次完整验帧使用
fail，不能通过丢帧降低统计延迟。

私有基准服务器 18554 和输入素材准备好后，可以运行：

```powershell
python ports/thor/tools/benchmark_five.py --scheme contexts --frames 780 `
  --target-fps 12 --decoder-entropy-workers 1 --binary-dir bin `
  --label five-contexts-12-w1 --output five-contexts-12-w1.json
```

基准工具从公开的 `configs/five/` 模板生成各路配置，并使用源码内的遥测和
回执工具，不依赖 style-final 临时配置。输入素材和独立 MediaMTX 仍需准备，
完整步骤见 [基准复现](BENCHMARKING.md)。原始结果、逐帧回执、trace 及摘要保存在板卡 logs 和 Windows
交付目录 validation。延迟从 NVDEC 读帧就绪到语义解码回执，不含源端曝光、
上游 RTSP 缓存、后续 NVENC 发布完成及浏览器显示。
