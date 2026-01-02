# Loopback Recorder

面向 Windows 10/11 的 C++20 命令行录音工具，使用 WASAPI Loopback 在纯用户态捕获系统正在播放的音频。工程默认配合 Visual Studio 2022 + CMake 构建，强调低 CPU、易维护。

## 项目结构
```
.
├── CMakeLists.txt        # 顶层构建脚本
├── README.md             # 使用说明（本文档）
├── DELIVERABLES.txt      # 交付清单
├── docs/
│   └── ROADMAP.md        # 开发路线图
└── src/
    ├── DeviceEnumerator.*  # 设备枚举与选择
    ├── HResultUtils.*      # HRESULT 文本转换
    ├── Logger.*            # 控制台/文件日志
    ├── LoopbackRecorder.*  # 录音主流程（WASAPI + 环形缓冲 + 写线程）
    ├── SpscByteRing.h      # 单生产者单消费者环形缓冲
    ├── WavWriter.*         # WAV Header 写入与回填
    └── main.cpp            # CLI 入口、参数解析
```

## 功能概览
- 仅使用 WASAPI Loopback（共享模式），无驱动、无内核代码。
- 输出与系统 Mix Format 一致的 PCM WAV（16-bit 或 32-bit float）。
- 支持 `--list-devices` / `--device-index` 查看和选择播放设备。
- `--seconds` 限时录制、按 Enter 手动停止、`--out` 指定文件。
- 可调的采集延迟/环形缓冲/看门狗参数：`--latency-ms`、`--buffer-ms`、`--watchdog-ms`、`--fail-on-glitch`。
- `--log-file` 同步写入日志文件，保留结构化状态统计。
- 线程分离：采集线程 + 写盘线程（SPSC 环形缓冲解耦）。
- 设备断开/切换检测，能够优雅退出并提示用户。
- 实时状态：默认每秒打印采集帧数、环形缓冲深度、丢帧计数，可用 `--quiet` 关闭。
- 写盘变慢时采用丢帧策略并记录统计，保证采集线程持续实时运行。

## 编译步骤（VS2022 + CMake）
```powershell
# 1. 生成工程（默认生成 build/ 目录）
cmake -S . -B build

# 2. 使用 MSVC 多配置生成器构建 Release
cmake --build build --config Release

# 生成的 loopback_recorder.exe 位于 build/Release/
```

## 运行示例
```powershell
# 列出可用输出设备
loopback_recorder --list-devices

# 默认设备录制 30 秒并保存到自定义路径
loopback_recorder --seconds 30 --out C:\captures\demo.wav

# 为嘈杂/繁忙系统放大缓冲并在出现抖动时终止
loopback_recorder --latency-ms 250 --watchdog-ms 6000 --buffer-ms 4000 --fail-on-glitch

# 启用文件日志（控制台输出始终开启）
loopback_recorder --log-file C:\captures\loopback.log

# 使用特定设备索引
loopback_recorder --device-index 1 --seconds 10

# 静默模式（关闭实时状态打印）
loopback_recorder --seconds 60 --out C:\captures\bgm.wav --quiet
```

## 设计说明
- **WASAPI Loopback**：通过 `IAudioClient::Initialize(... AUDCLNT_STREAMFLAGS_LOOPBACK ...)` 在共享模式捕获系统混音输出，沿用 `GetMixFormat` 得到的声道/采样率/样本格式，无需手动转换，能够跟随系统设置。
- **线程/缓冲策略**：采集线程使用事件驱动（`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`）写入单生产者单消费者环形缓冲，写盘线程阻塞式读取并写入 WAV。`--latency-ms` 与 `--buffer-ms` 控制缓冲深度，`--watchdog-ms` 防止死等，`--fail-on-glitch` 遇到超时或 `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` 时立即终止；当写盘持续落后时会丢弃最新帧并记录统计，确保采集线程保持实时。
- **日志与统计**：所有 HRESULT 通过 `DescribeHRESULT*` 转成易读文本，既打印到控制台也可选写入日志文件；录音结束后输出帧数、静音帧、数据中断次数、看门狗/环形缓冲等待次数、丢帧数量，并指示设备是否被拔掉。实时状态输出可通过 `--quiet` 关闭。
- **WAV Writer**：`WavWriter` 先写 RIFF 头部占位，结束时回填 `RIFF`/`data` 尺寸，支持 16-bit PCM 与 32-bit float。
- **设备处理**：`DeviceEnumerator` 包装 `IMMDeviceEnumerator` 提供设备列表、默认设备、友好名称，以及设备断开时的清晰错误提示。

## 常见问题（FAQ）
1. **录不到声音/文件静音？**  
   - 确认播放设备非独占模式，且系统确实有音频输出。  
   - 使用 `--device-index` 指定正确设备，或先执行 `--list-devices`。  
   - 若仍静音，尝试提升 `--latency-ms`（例如 200→300）。

2. **提示设备被占用（AUDCLNT_E_DEVICE_IN_USE）？**  
   - 其他应用以独占模式占用了该设备。关闭相关程序或在声音设置取消独占。  
   - 也可切换到未被占用的输出设备后再录制。

3. **采样格式与编辑器不兼容？**  
   - 本工具直接写入 mix format，现代系统多为 32-bit float。  
   - 若编辑器只接受 16-bit PCM，可在 DAW/ffmpeg 中转换，或在系统设置里把默认格式改为 16-bit 再录制。

4. **运行中设备被拔掉/切换？**  
   - 程序会停止录音并记录 “device became unavailable” 日志，请重新连接并指定可用设备后重试。

5. **长时间录音 CPU/磁盘压力大？**  
   - 使用 `--latency-ms` 和 `--buffer-ms` 增大缓冲，或输出到 SSD。  
   - `RecorderStats` 中的 ring waits/timeouts 提示需要继续调大缓冲。

## 开发路线图
后续计划详见 `docs/ROADMAP.md`，包括暂停/继续、分段写文件、可选 Opus/AAC 编码、GUI 等阶段性目标。

