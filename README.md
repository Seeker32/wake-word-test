# ESP32 唤醒词测试固件

这是放在项目根目录的原生 ESP-IDF C++ 工程。ESP32-S3-BOX-3 上电后用板载麦克风监听 **Hey Kamana**（hey kah-MAH-nah），命中后用板载扬声器播放一次 `voice_file/wake_up.wav`，然后恢复监听。模型和提示音随固件写入 Flash，运行时可离线测试。

## 编译和烧录

使用 ESP-IDF 6.0.1 或更新的 6.0.x 版本。激活 ESP-IDF 环境后，在本目录执行以下命令。首次构建需要下载 ESP-IDF 组件和训练所用的 MicroFrontend 源码。

构建前，将 `hey_kamana.json` 和清单的 `model` 字段指定的 `.tflite` 文件放到本项目的 `models/` 目录。

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

固件固定使用 ESP32-S3-BOX-3。需要调整麦克风增益 `WAKE_MIC_GAIN` 或扬声器音量 `WAKE_SPEAKER_VOLUME` 时，在构建前运行 `idf.py menuconfig`，进入 **Wake Word Audio** 菜单。

串口出现 `Listening for Hey Kamana` 后说一次唤醒词。确认日志出现 `Wake word detected: Hey Kamana`，板载扬声器完整播放一次提示音。等监听恢复后再说一次，确认两次均能触发。使用 `Ctrl+]` 退出串口监视。

## 源码目录

- `main/main.cpp` 负责监听、播放和恢复检测。
- `main/wake_detector.cpp` 提取 16 kHz 单声道特征，使用 TensorFlow Lite Micro 运行量化流式模型，并对连续输出做滑动平均。
- `boards/esp32-s3-box-3.cpp` 负责板载 ES7210、ES8311 和共享 I²S（LRCK GPIO45）。录音和播放使用 24 kHz 时钟，录音重采样到 16 kHz 供模型使用。播完提示音后排空录音缓冲并重置采集状态。
- `boards/resample_util.h` 提供提示音的一次性重采样。
- `models/hey_kamana.json` 指定模型文件、唤醒词、初始阈值 `0.99`、滑动窗口 `5` 和内存大小。修改参数后重新编译即可。
- `voice_file/wake_up.wav` 是 24 kHz、16 位单声道提示音。更换为其他采样率的提示音时，板级驱动会在播放前重采样。
