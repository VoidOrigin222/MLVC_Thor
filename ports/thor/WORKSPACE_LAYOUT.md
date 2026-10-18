# 工作目录整理（2026-10-18）

## 本机

源码仓库仍为 `MLVC_Thor/`。`application`、`apps`、`common`、`mlvc`、
`third_party` 和 `ports/thor` 是构建所需代码；其余受版本控制的历史端口和文档
保留，未为整理目录而改变构建功能。

- 当前 UI：`deliveries/ui-five-20261018/SemanticFiveMonitor.exe`，外置参数同目录。
  这是已验证的 10 月 17 日版本的搬移副本，本次没有修改 UI 程序。
- 当前混合场景验证：`artifacts/mixed-five-20261018/`。
- 历史测试、矿井原始素材来源记录、旧 UI 包及构建缓存：已移出仓库，归档到
  同级 `MLVC_Thor_archive/20261018/windows/`。本机递归删除被自动审批拦截，
  因而使用可恢复的移动归档，未宣称释放这些文件占用的磁盘空间。

## 两块板卡

运行目录均为 `/home/xidian/workspace/MLVC_Thor/`：

| 目录 | 内容 |
|---|---|
| `bin/` | 当前三个编解码可执行文件 |
| `models/`、`engines/` | 模型、熵表、manifest 和 TensorRT 引擎 |
| `deps/` | 解码板构建依赖；编码板使用系统依赖 |
| `configs/` | 单路公开配置、五路配置和测试源参数 |
| `scripts/` | 运行与环境校验脚本 |
| `src/MLVC/` | 唯一的完整源码树 |
| `data/video/` | 原始测试视频、12 FPS 素材和静态画面缓存 |
| `logs/` | 后续新运行日志 |

已删除可重新生成的 FP16 输入、解码图像、展开循环视频及逐文件核验过的重复
`source-verify/`。旧日志、配置、候选程序、回滚副本、交付包和构建目录已压缩
归档到各板 `/home/xidian/workspace/MLVC_Thor_archive/20261018/history.tar.gz`。
根目录重复的旧工具副本另移到该归档目录的 `tools/`；当前工具使用
`src/MLVC/ports/thor/tools/`。
归档和删除清单见同目录 `MANIFEST.json`，运行根目录也保留
`CLEANUP_MANIFEST.json`。历史 FP16/离线测试配置仅作记录，当前生产程序只接受 RTSP。

清理前后逐文件校验当前三个程序、models、engines、deps 和五路配置，内容未变。
编码板工作目录约 17.62 GB → 243 MB，解码板约 31.18 GB → 247 MB；
历史压缩归档另占约 474 MB、721 MB，数字均为十进制。
两板清理后均通过 `bash scripts/check_runtime.sh`。
清理后重新启动五路测试源，ffprobe 确认五路均为 H.264、1920×1080、12/1 FPS；
跨越素材循环边界继续发布，向启动脚本发送 SIGINT 后退出码为 0，五个 FFmpeg
子进程全部消失。验证记录为本机 `artifacts/mixed-five-20261018/source-lifecycle.json`。

重新构建时统一使用 `src/MLVC` 和新建的 `build`，不再引用 `source-verify`；
命令见 [运行说明](README.md)。旧日志中的路径保留当时的值，不代表仍有该目录。

## 混合场景短测

`input0/1` 固定静态画面，`input2/3` 循环动态素材，`input4` 为实时计时器，
均为 1920×1080、12 FPS。五上下文双板每路完成 300 帧，共 1500 帧；
编码、解码、RTSP 发布计数一致，接收丢包、CRC 错误和发布丢帧均为 0。
重建后的大号时间读数可辨认，帧号小字有压缩变形。

这轮编码约 11.69–12.00 FPS，解码发布约 11.49–11.89 FPS。剔除每路前
96 帧后，原有 NVDEC 就绪到解码回执口径的各路平均时延约
704、994、847、1047、773 ms。这是新的短轮场景，计时器额外使用编码板的
NVENC。以上是后续修复前的记录；积压位置、修复和最终长测见
[混合场景时延验证](MIXED_LATENCY_VALIDATION.md)。
没有测得相机曝光到浏览器显示的绝对总时延。

输入启动命令与计时器读数说明见 [五路 RTSP](README.md#五路-rtsp)。
