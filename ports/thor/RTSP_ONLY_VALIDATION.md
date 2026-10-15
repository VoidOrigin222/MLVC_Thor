# RTSP 输入收敛验证（2026-10-15）

Thor 生产编码入口仅接受 `input_rtsp`。TOML 顶层及 `[model]` 中的离线、FP16、
摄像头和合成输入键被拒绝；直接调用编码 API 时也校验 RTSP、NVDEC 和 GPU 输入。
删除了 Thor 的 V4L2/BGR 采集分支，并取消 OpenCV videoio 链接。
底层 NVDEC 文件读取仅供现有硬件测试与诊断工具使用，Ascend 目标保持原接口。

## 构建与测试

- 解码板独立目录 `build-rtsp-only-20261015` 从头构建全部程序，CTest 15/15 通过。
- 新配置测试覆盖空/错误 RTSP URL、旧输入键、嵌套别名、CPU 输入、软件编解码后端，
  以及绕过配置直接调用编码 API 的离线和合成输入。
- 11 份单路/五路编码模板均显式配置 NVDEC、GPU 输入和 NVENC 运动代理。
- 启动脚本通过 Bash 语法和旧 camera 模式拒绝检查；默认使用 RTSP 配置。
- 修改的 5 份 C++ 文件通过 clang-format 20.1.8 和仓库现用 cpplint 规则。

早期增量构建因源码归档的时间戳未更新而复用了旧程序。该轮联调被中止，结果
明确标记无效；本记录只采用上述独立构建后的新程序，三份二进制 SHA-256 如下：

```text
5cf922bc1e345db0496e5249c2b72ec9d5e9c247a795d8aaf351aa353b41cada  mlvc_encode_thor
5bc2beb900d372422db9d499630b16d3297389026d5a665bf536ea06c0ba32af  mlvc_decode_thor
b2c76739f75cc5f43fb17a8787a07eba9eb63a81dc378570d33f497bac5891c2  mlvc_multi_thor
```

## 双板五路联调

使用同一视频的五个时间偏移在外部实时发布为 1920×1080、13 FPS H.264 RTSP，
生产编码器仅接收 RTSP。五独立上下文，目标每路 12 FPS，解码熵窗口 1。
这不是五个独立摄像头场景，也未做 HEVC/RTSPS 实机测试。

每路 780 帧，共 3900 帧全部编码、解码、回执并通过 NVENC 发布 RTSP。
每路 RTP 接收丢包、队列丢帧、发布丢帧均为 0。五路日志均确认
`input_video_decoder=nvdec codec=h264_cuvid surface=nv12`，输入 FP16
整帧 D2H 统计均为 0；运动代理和发布日志均记录了 NVENC 编码耗时。

本轮平均编码 12.22 FPS/路，解码全程平均 11.40 FPS/路，稳态解码回执平均
11.52 FPS/路。FPS 测量窗口不同，不作为稳定达到五路各 12 FPS 的承诺；本次
修改收敛输入接口，没有声称解决此前记录的解码性能或启动偶发异常。

完整结果保存于本地 `artifacts/rtsp-only-20261015/five-contexts-final.json`，
清洁构建及测试日志、程序和源码归档同目录保留。测试脚本退出后停止了它拥有
的推流、编解码及遥测进程；配套程序和源代码同步到两板运行目录。
