# 五路视频监控

五个通道，每路原图和语义压缩固定并排。超宽屏三列、普通桌面两列、窄屏单列，
每个通道内的两个画面始终并排。通过 MediaMTX WHEP/WebRTC 播放十个视频，
支持重新连接、暂停/继续、单画面全屏和独立通道设置。页面不提供运行日志、
服务状态面板、H.264 比较器或 PSNR 控制。

## 启动

Windows 双击 `SemanticFiveMonitor.exe`，同目录保留 `ui_config.json`。
启动器打开浏览器，关闭启动器停止本地页面及它的播放会话，板端程序独立运行。
同一参数文件的实例会重新读取文件再打开页面；不同文件使用独立端口，不复用旧配置。

Python 方式需要 Python 3.10+，后端只用标准库，前端无需 npm：

```powershell
python tools/ui/launch_ui.py
# 或仅启动 HTTP 服务：
python tools/ui/server.py --port 8765
```

默认网页 `http://127.0.0.1:8765/`。编码板和解码板的启动命令保持原样，
MediaMTX 由使用者单独启动，开启 RTSP、WebRTC 和 API。
WHEP 支持的 H.264 流必须已接入该 MediaMTX；界面不会把任意摄像头 RTSP
自动转发到另一个服务器，也不负责启动板端编解码。

## 参数文件

默认读取 EXE 同目录的 `ui_config.json`；源码方式读取本目录文件，不依赖
启动工作目录。“参数设置 → 保存并连接”先验证，再原子写入文件并重建播放会话。
缺失的参数文件从内置模板生成，已有文件不被启动覆盖。可用 `--config` 指定其他文件。

- `webrtc`：MediaMTX WebRTC HTTP 地址，默认 `http://127.0.0.1:8889`。
- `media_api`：MediaMTX API，默认 `http://127.0.0.1:9997`。
- `original_delay_ms`：原图额外显示补偿，默认 200 ms，可设 0–2000。
- 五路 `original`：原图完整 RTSP URL，默认 `/input0` 至 `/input4`。
- 五路 `semantic`：解码重建完整 RTSP URL，默认 `/ulbvc0` 至 `/ulbvc4`。
- `width`、`height`、`fps`：编码源尺寸与帧率记录，默认 1920×1080、12 FPS。此处不修改板端输入帧率；帧率应在板端编码配置中设置。
- `stats_port`：各通道编码统计 UDP 端口，默认 39600–39604，不可重复。

这些 URL 应指向同一 MediaMTX 中的对应路径。可保留多级路径；参数抽屉完整显示
URL，包含流名称。浏览器和 MediaMTX 不在同一电脑时，MediaMTX 还需正确设置
`webrtcAdditionalHosts` 和可达的 WebRTC UDP 端口。UI 代理 WHEP HTTP 信令，
视频本身通过 WebRTC 传输。配置文件含 RTSP 凭据时，应只保留在本地部署目录。

## 接入真实编码统计

每个通道下方的 bpp、带宽和压缩比来自编码器累计的**语义净载荷字节和帧数**，
不使用重建后 H.264 视频的播放带宽替代。带宽为十进制 kB/s，不含 RTP/UDP/IP
包头；bpp 按编码帧数与图像尺寸计算，压缩比使用 RGB24 的 24 bpp 为基准。
视频角标的 FPS 则是浏览器实际接收/解码帧率。

五路总配置 `configs/encoder.toml` 已默认开启统计，接收地址为 `192.168.5.3`，
五路 UDP 端口为 39600–39604，与 UI 默认参数对应。更换 UI 电脑 IP 时修改
`[encoder].ui_stats_host`。需要关闭某路统计时，将对应的 `ui_stats_port` 设为 0。
单路配置仍可直接加入：

```toml
# 填运行 UI 的电脑局域网 IP，板卡上不能填写 127.0.0.1。
ui_stats_host = "192.168.5.3"
ui_stats_port = 39600
```

`ui_stats_port=0` 不创建统计网络连接。启用后最多每 500 ms
通过非阻塞 UDP 发送一次累计值；统计接收端不在线不会阻塞或中断编解码。
UI 必须与新增统计接口的可执行程序配套使用，旧二进制不识别这两个配置项。
允许板卡访问电脑上的对应 UDP 端口，五路一一对应，避免占用 RTP/RTCP 端口。

统计未接入、过期或编码器重启时，指标显示 `—`，重新建立计数基准；不显示
虚构的 0 或演示数据。界面的带宽变化保留最近 30 秒。原图 200 ms 补偿为
固定的显示队列，不是逐帧同步或端到端延迟测量。

## 验证和打包

```powershell
python -m unittest discover -s tools/ui -p test_server.py -v
node --check tools/ui/app.js
node tools/ui/test_frame_delay.cjs
python tools/ui/launch_ui.py --self-test artifacts/ui-self-test.json
python -m pip install -r tools/ui/requirements-build.txt
python tools/ui/build_windows_exe.py
```

源码包含 HTTP/WHEP 会话归属、参数持久化/保存失败、重复端口、计数重置及
陈旧统计测试。Windows EXE 只打包 UI 所需的 Python、Tk 和网页资源；不需要
在 UI 中打包 FFmpeg。模型、MediaMTX 和板端程序依照各自部署说明独立提供。
