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
- 默认输出 MP3，可选 WAV（均沿用系统 Mix Format：16-bit PCM 或 32-bit float）。
- 支持 `--list-devices` / `--device-index` 查看和选择播放设备。
- `--seconds` 限时录制、按 Enter 手动停止、`--out` 指定文件。
- 可调的采集延迟/环形缓冲/看门狗参数：`--latency-ms`、`--buffer-ms`、`--watchdog-ms`、`--fail-on-glitch`。
- `--log-file` 同步写入日志文件，保留结构化状态统计。
- 线程分离：采集线程 + 写盘线程（SPSC 环形缓冲解耦）。
- 设备断开/切换检测，GUI/CLI 支持自动重连（默认重试 3 次）。
- 实时状态：默认每秒打印采集帧数、环形缓冲深度、丢帧计数，可用 `--quiet` 关闭。
- 实时 MP3 编码：当输出为 `.mp3` 时，录音过程中直接写入 MP3（默认 192 kbps，可通过 `--mp3-bitrate` 调整）；需搭配 `libmp3lame.dll`/`lame_enc.dll`（与可执行文件同目录，或通过 `LAME_DLL_PATH` 指向 DLL）。
- 实时控制：录音过程中按 Enter 停止，输入 `P` 暂停/继续，输入 `S` 即刻切换到新的输出文件，所有操作都会在控制台提示。
- 分段输出：支持 `--segment-seconds`（按时长滚动）与 `--segment-bytes`（按字节数滚动），也可在运行中通过 `S` 命令手动分段；每个分段都会独立落盘并按 `xxx_001`、`xxx_002` 等顺序命名（扩展名随输出格式变化；WAV 会回填头部）。
- 写盘变慢时采用丢帧策略并记录统计，保证采集线程持续实时运行。

## 今日更新（2026-01-09）
- 修复：二次录音时分段文件 `_001` 被清零的问题，避免覆盖已有分段文件。
- 稳定性：GUI/CLI 增加设备断开自动重连与重新获取默认设备（含重连日志提示）。
- GUI：重连时自动更新输出路径显示；新增暂停/继续按钮；状态栏显示录音时长与当前文件大小。
- GUI：新增输出目录选择与“打开文件夹”按钮，提升文件管理体验。

## 编译步骤（VS2022 + CMake）
```powershell
# 1. 生成工程（默认生成 build/ 目录）
cmake -S . -B build

# 2. 使用 MSVC 多配置生成器构建 Debug
cmake --build build --config Debug

# 生成的 loopback_recorder.exe 位于 build/Debug/
```

## 运行示例
```powershell
# 列出可用输出设备
loopback_recorder --list-devices

# 默认设备录制 30 秒并保存到自定义路径
loopback_recorder --seconds 30 --out C:\captures\demo.mp3

# 为嘈杂/繁忙系统放大缓冲并在出现抖动时终止
loopback_recorder --latency-ms 250 --watchdog-ms 6000 --buffer-ms 4000 --fail-on-glitch

# 启用文件日志（控制台输出始终开启）
loopback_recorder --log-file C:\captures\loopback.log

# 每 5 分钟切一段，输出 meeting_001.wav、meeting_002.wav 文件
loopback_recorder --segment-seconds 300 --out C:\captures\meeting.wav

# 使用特定设备索引
loopback_recorder --device-index 1 --seconds 10

# 静默模式（关闭实时状态打印）
loopback_recorder --seconds 60 --out C:\captures\bgm.mp3 --quiet

# 指定 MP3 比特率
loopback_recorder --seconds 120 --out C:/captures/meeting.mp3 --mp3-bitrate 192
```

## 图形界面（loopback_recorder_gui）
- 构建后会额外得到 `loopback_recorder_gui.exe`。在界面中填写输出路径、勾选“Save as MP3”并设置比特率，然后点击“开始录音”；日志会实时显示，点击“停止”即可结束录音。
- GUI 复用与 CLI 相同的录音及实时 MP3 编码核心，仍需要 `libmp3lame.dll`（或 `lame_enc.dll`）放置在可执行文件旁或通过 `LAME_DLL_PATH` 指定路径。
- GUI 仅作为壳层，CLI 可继续独立使用；后续新功能可以先在 CLI 完成，再在 GUI 中补齐控件。

## 实时控制与分段
- **停止**：直接按 Enter （输入空行）即可停止录音，并结束当前文件。
- **暂停/恢复**：输入 `P` + Enter 可在录音与暂停间切换，暂停期间接收到的音频会被丢弃，统计中会上报 `paused frames`。
- **手动分段**：输入 `S` + Enter 立即结束当前文件并接着写入 `xxx_002`、`xxx_003` 等文件（扩展名随输出格式变化），便于标记重点片段。
- **自动分段**：`--segment-seconds` 每 N 秒滚动，`--segment-bytes` 按写入字节数滚动，可双向组合使用；所有分段都使用 `_001`、`_002` 迭代命名（扩展名随输出格式变化；WAV 会回填头部）。


## MP3 Encoding (Real-time)
- 当 `--out` 以 `.mp3` 结尾时，录音过程中直接编码并写入 MP3，不再需要录音结束后的二次转码。
- 依赖 `libmp3lame.dll`（或 `lame_enc.dll`）。将 DLL 放在 `loopback_recorder.exe` 同目录即可，或通过环境变量 `LAME_DLL_PATH` 指向绝对路径；缺少 DLL 时会提示 “Unable to load libmp3lame...”。
- `--mp3-bitrate K`（32–320）可设置恒定比特率，默认 192 kbps。程序能够处理 16-bit PCM 与 32-bit float 输入，若系统输出是多声道会自动混成立体声/单声道后编码。

## 设计说明
- **WASAPI Loopback**：通过 `IAudioClient::Initialize(... AUDCLNT_STREAMFLAGS_LOOPBACK ...)` 在共享模式捕获系统混音输出，沿用 `GetMixFormat` 得到的声道/采样率/样本格式，无需手动转换，能够跟随系统设置。
- **线程/缓冲策略**：采集线程使用事件驱动（`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`）写入单生产者单消费者环形缓冲，写盘线程阻塞式读取并写入 WAV 或实时编码 MP3（取决于输出格式）。`--latency-ms` 与 `--buffer-ms` 控制缓冲深度，`--watchdog-ms` 防止死等，`--fail-on-glitch` 遇到超时或 `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` 时立即终止；当写盘持续落后时会丢弃最新帧并记录统计，确保采集线程保持实时。
- **日志与统计**：所有 HRESULT 通过 `DescribeHRESULT*` 转成易读文本，既打印到控制台也可选写入日志文件；录音结束后输出帧数、静音帧、数据中断次数、看门狗/环形缓冲等待次数、丢帧数量，并指示设备是否被拔掉。实时状态输出可通过 `--quiet` 关闭。
- **实时控制**：独立线程监听控制台输入，Enter 停止、`P` 暂停/继续、`S` 即时切换到新的文件。暂停会使采集线程保持会话但报告 `paused frames`。所有分段符合 `_001`、`_002` 命名规则（扩展名随输出格式变化；WAV 会回填头部）。
- **WAV Writer**：`WavWriter` 先写 RIFF 头部占位，结束时回填 `RIFF`/`data` 尺寸，支持 16-bit PCM 与 32-bit float。
- **MP3 Writer**：当输出为 `.mp3` 时，使用 `libmp3lame` 进行流式编码，录音线程写入的 PCM 会实时转换并落盘，结束时仅需 flush。
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






