# ESP32 唤醒词测试固件

这是放在项目根目录的原生 ESP-IDF C++ 工程。ESP32-S3-BOX-3 上电后用板载麦克风监听 **Hey Kamana**（hey kah-MAH-nah），命中后用板载扬声器播放一次 `voice_file/wake_up.wav`，然后恢复监听。模型和提示音随固件写入 Flash，运行时可离线测试。

## 编译和烧录

使用 ESP-IDF 6.0.1 或更新的 6.0.x 版本。激活 ESP-IDF 环境后，在本目录执行以下命令。首次构建需要下载 ESP-IDF 组件和训练所用的 MicroFrontend 源码。

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

串口出现 `Listening for Hey Kamana` 后说一次唤醒词。确认日志出现 `Wake word detected: Hey Kamana`，板载扬声器完整播放一次提示音。等监听恢复后再说一次，确认两次均能触发。使用 `Ctrl+]` 退出串口监视。

## 源码目录

- `main/main.cpp` 负责监听、播放和恢复检测。
- `main/wake_detector.cpp` 提取 16 kHz 单声道特征，使用 TensorFlow Lite Micro 运行量化流式模型，并对连续输出做滑动平均。
- `boards/esp32-s3-box-3.cpp` 负责板载 ES7210、ES8311 和共享 I²S。录音和播放使用 24 kHz 时钟，录音重采样到 16 kHz 供模型使用。播放期间暂停推理，播完排空录音缓冲并重置检测状态。
- `models/hey_kamana.json` 指定模型文件、唤醒词、初始阈值 `0.99`、滑动窗口 `5` 和内存大小。修改参数后重新编译即可。
- `voice_file/wake_up.wav` 是提示音，BOX-3 按原始 24 kHz、16 位单声道格式播放。

## 增加板型

在 `boards/` 新增对应的板级实现，按 `main/board_audio.h` 提供音频初始化、16 kHz 单声道采集和 PCM 播放，再在 `main/Kconfig.projbuild` 和 `main/CMakeLists.txt` 登记。每次构建选择一个板型。BOX-3 的接线和 codec 初始化参考了小智的本地板级代码，其他板型分别核对 GPIO、采集声道和扬声器路径。[小智 BOX-3 接线配置](/Users/program/xiaozhi-esp32/main/boards/espressif/esp32-s3-box-3/config.h) · [小智音频驱动](/Users/program/xiaozhi-esp32/main/audio/codecs/box_audio_codec.cc)
