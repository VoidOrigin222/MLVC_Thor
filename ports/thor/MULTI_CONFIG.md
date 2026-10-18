# 五路总配置

入口、配置合并及双板验证见 [验证记录](ENCODER_DECODER_VALIDATION.md)。

编码端使用 `/home/xidian/workspace/MLVC_Thor/configs/encoder.toml`，
解码端使用 `/home/xidian/workspace/MLVC_Thor/configs/decoder.toml`。
每个文件集中配置本端五路，公共参数只写一次，不需要逐路维护 TOML。
程序直接在内存中合并、校验参数，不生成五个临时配置。修改后重启对应程序生效。

## 启动

在 VS Code Remote-SSH 的集成终端操作。先在解码板启动：

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_decoder.sh configs/decoder.toml
```

电脑 MediaMTX 启动后，在编码板终端 1 启动循环测试源：

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_test_sources.sh
```

在编码板终端 2 启动语义编码：

```bash
cd /home/xidian/workspace/MLVC_Thor
bash scripts/run_encoder.sh configs/encoder.toml
```

三个终端保持运行；对应终端按 Ctrl+C 停止。不需要每次运行 `check_runtime.sh`。
Windows UI 的 EXE 和 `ui_config.json` 继续独立使用。
两个脚本不传参数时，也会分别默认加载 `configs/encoder.toml`、`configs/decoder.toml`。

直接运行可执行文件的等价命令：

```bash
bin/decoder --config configs/decoder.toml
bin/encoder --config configs/encoder.toml
```

## 文件结构

- `execution = "contexts"`：五个独立推理上下文；在配置中选择，不再通过命令指定。
- `log_prefix = "logs/encoder"`：生成 `encoder-lane0.log` 等分路日志。
- `[encoder]`、`[encoder.pipeline]`：五路共用的编码参数及流水线参数。
- `[decoder]`、`[decoder.pipeline]`：五路共用的解码参数及流水线参数。
- 五个 `[[streams]]`：按文件顺序对应通道 1–5。
- `[streams.encoder]`、`[streams.decoder]`：该路覆盖公共参数。
  嵌套的 pipeline 参数逐项继承，覆盖一个值不会丢失其他公共值。

例如：

```toml
execution = "contexts"

[encoder]
qp = 11
target_fps = 12
output_transport_host = "192.168.5.17"
# 其余完整公共参数见 encoder.toml。

[[streams]]
[streams.encoder]
input_rtsp = "rtsp://192.168.5.3:8554/input0"
output_transport_port = 39480
# 其余四路继续添加 [[streams]]，完整配置见 encoder.toml。
```

以上只是结构片段，运行时使用仓库提供的完整文件。当前入口固定五路；加载时
拒绝路数错误、未知字段、无效运行模式、重复或交叉的 RTP/RTCP 端口，并复用
单路配置的全部参数检查。五路全部校验完成后才启动 GPU 工作。

相对路径仍以运行工作目录为基准；运行脚本会切换到板端工作目录。
两个总配置不引用原来的分路 TOML，五路启动不再依赖那些文件。
`configs/five/contexts/` 和 `configs/five/batch5/` 仅保留给历史基准和旧命令兼容。
显式旧命令 `decode contexts`、`encode batch5`、`encode processes` 仍按原逻辑运行。
总配置目前支持 `contexts` 和 `batch5`；切换到 `batch5` 时还需把公共编码、解码
manifest 都改为 `models/manifest_thor_batch5.json`，并准备对应引擎。

## 更换地址和统计

更换解码板地址，修改 `[encoder].output_transport_host`。
更换 MediaMTX 地址，修改五路输入 `input_rtsp` 和五路输出 `output_transport_rtsp_url`；
测试源 `configs/five/test_sources.json` 及 Windows UI 参数也要对应修改。
RTP 发送与接收端口按路一一对应，RTP/RTCP 分别使用 39480/39481 至 39488/39489。

UI 的带宽、bpp 和压缩比统计默认开启。`[encoder].ui_stats_host` 为 UI 电脑的
局域网地址 `192.168.5.3`，五路 `ui_stats_port` 为 39600–39604，与 UI 默认参数对应。
更换电脑 IP 时修改 `ui_stats_host`；如需关闭某路统计，将该路 `ui_stats_port` 设为 0。
原图补偿和 WebRTC 参数仍在 UI 的 JSON 中设置。
