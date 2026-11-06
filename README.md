# ESP32 唤醒词测试固件

这是放在项目根目录的原生 ESP-IDF C++ 工程。ESP32-S3-BOX-3 上电后用板载麦克风监听 **Hey Kamana**（hey kah-MAH-nah），命中后用板载扬声器播放一次 `voice_file/wake_up.wav`，然后恢复监听。模型和提示音随固件写入 Flash，运行时可离线测试。

## 编译和烧录

使用 ESP-IDF 6.0.1 或更新的 6.0.x 版本。激活 ESP-IDF 环境后，在本目录执行以下命令。首次构建需要下载 ESP-IDF 组件和训练所用的 MicroFrontend 源码。

```sh
idf.py set-target esp32s3
idf.py menuconfig   # Wake Word Board -> Wake-word test board 选择板型
idf.py build
idf.py flash monitor
```

在 menuconfig（即 SDK Configuration Editor）的 **Wake Word Board** 菜单中选择板型，并按需调整麦克风增益 `WAKE_MIC_GAIN` 和扬声器音量 `WAKE_SPEAKER_VOLUME`（仅编解码器板型提供这两项）。

## 支持的板型

| 板型 | 音频链路 | 备注 |
| --- | --- | --- |
| `WAKE_BOARD_BOX_3` ESP32-S3-BOX-3 | ES7210 + ES8311，24 kHz 双工 I²S（LRCK GPIO45） | 默认板型，已在真机验证 |
| `WAKE_BOARD_BOX` ESP32-S3-BOX（初代） | 同上，LRCK GPIO47 | 接线参考小智初代 BOX 配置 |
| `WAKE_BOARD_BOX_LITE` ESP32-S3-BOX-Lite | 同上，音频 16 kHz | 24 kHz 提示音播放时自动重采样 |
| `WAKE_BOARD_BREAD_COMPACT_WIFI` 面包板 | INMP441 麦（16 kHz，WS4/SCK5/DIN6，I2S1）+ MAX98357A 功放（24 kHz，BCLK15/LRCK16/DOUT7，I2S0） | 无编解码器控制接口，接线来自小智 bread-compact-wifi |

串口出现 `Listening for Hey Kamana` 后说一次唤醒词。确认日志出现 `Wake word detected: Hey Kamana`，板载扬声器完整播放一次提示音。等监听恢复后再说一次，确认两次均能触发。使用 `Ctrl+]` 退出串口监视。

## 源码目录

- `main/main.cpp` 负责监听、播放和恢复检测。
- `main/wake_detector.cpp` 提取 16 kHz 单声道特征，使用 TensorFlow Lite Micro 运行量化流式模型，并对连续输出做滑动平均。
- `boards/box_family.cpp` 是 BOX 家族（BOX / BOX-3 / BOX-Lite）共享的板级实现，负责板载 ES7210、ES8311 和共享 I²S，引脚与采样率由 Kconfig 决定。录音和播放使用板级时钟，录音重采样到 16 kHz 供模型使用；播放期间暂停推理，播完排空录音缓冲并重置检测状态。
- `boards/bread-compact-wifi.cpp` 是面包板（INMP441 + MAX98357A）的无编解码器实现，两个 I²S 端口各管一头。
- `boards/resample_util.h` 提供提示音的一次性重采样。
- `models/hey_kamana.json` 指定模型文件、唤醒词、初始阈值 `0.99`、滑动窗口 `5` 和内存大小。修改参数后重新编译即可。
- `voice_file/wake_up.wav` 是提示音（24 kHz、16 位单声道）；板级采样率不一致时由板级层重采样后播放。

## 增加板型

在 `boards/` 新增对应的板级实现，按 `main/board_audio.h` 提供音频初始化、16 kHz 单声道采集和 PCM 播放，再在 `main/Kconfig.projbuild` 和 `main/CMakeLists.txt` 登记。每次构建选择一个板型。新板需要注意：

- `ReadWakePcm` 每次最多返回 256 个 16 kHz 单声道样本，非 16 kHz 采集需在板级层重采样（参考 `box_family.cpp`）。
- `PlayWakePrompt` 收到 16 位单声道 PCM，采样率与板级不一致时应重采样；播放结束后必须排空 TX/RX DMA 并复位采集状态，避免提示音经麦克风回声把自己再次唤醒（两个板级实现都有此逻辑，勿省略）。
- 编解码器板型的增益/音量用 `CONFIG_WAKE_MIC_GAIN`、`CONFIG_WAKE_SPEAKER_VOLUME`。
