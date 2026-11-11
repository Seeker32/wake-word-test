set(model_dir "${PROJECT_DIR}/models")
# 要嵌入的清单由 sdkconfig 的 CONFIG_WAKE_MODEL_MANIFEST 选择
# （menuconfig / SDK Configuration Editor → Wake Word Audio → Wake word model manifest），
# 留空 = 自动探测：models/ 下只有一个 *.json 时用它，多个时取排序第一个。
# 选择错误只警告并回退自动模式，不在配置期 FATAL_ERROR（否则 menuconfig 会被堵死）。
set(manifest_path "")
set(manifest_notes "")
if(CONFIG_WAKE_MODEL_MANIFEST)
    set(manifest_path "${model_dir}/${CONFIG_WAKE_MODEL_MANIFEST}")
    if(NOT EXISTS "${manifest_path}")
        string(APPEND manifest_notes
            "CONFIG_WAKE_MODEL_MANIFEST='${CONFIG_WAKE_MODEL_MANIFEST}' not found in ${model_dir}.\n")
        set(manifest_path "")
    endif()
endif()
if(NOT manifest_path)
    file(GLOB manifests "${model_dir}/*.json")
    list(SORT manifests)
    list(LENGTH manifests manifest_count)
    if(manifest_count EQUAL 1)
        list(GET manifests 0 manifest_path)
    elseif(manifest_count EQUAL 0)
        message(FATAL_ERROR "No model manifest (*.json) found in ${model_dir}. Copy the manifest and the .tflite "
                            "file it names from the training output into ${model_dir}/ before building.")
    else()
        list(GET manifests 0 manifest_path)
        get_filename_component(manifest_name "${manifest_path}" NAME)
        list(JOIN manifests "\n  " manifests_pretty)
        string(APPEND manifest_notes
            "Multiple manifests in ${model_dir}:\n  ${manifests_pretty}\n"
            "Defaulting to ${manifest_name}.\n")
    endif()
endif()
if(manifest_notes)
    string(APPEND manifest_notes
        "Pick a model in menuconfig: Wake Word Audio → Wake word model manifest (under models/).")
    message(WARNING "${manifest_notes}")
endif()
if(NOT EXISTS "${manifest_path}")
    message(FATAL_ERROR "Missing ${manifest_path}. Copy the manifest and the .tflite file it names "
                        "from the training output into ${model_dir}/ before building.")
endif()
file(READ "${manifest_path}" model_manifest)
string(JSON model_file GET "${model_manifest}" model)
if(NOT EXISTS "${model_dir}/${model_file}")
    message(FATAL_ERROR "Missing ${model_dir}/${model_file}, named by the model field of ${manifest_path}.")
endif()

idf_component_register(
    SRCS "main.cpp" "display.cpp" "resource_monitor.cpp" "wake_detector.cpp" "../boards/esp32-s3-box-3.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_i2s esp_driver_ledc esp_driver_spi esp_timer esp_lcd
                  esp_lcd_ili9341 esp_lvgl_port lvgl
    EMBED_FILES "${model_dir}/${model_file}" "${PROJECT_DIR}/voice_file/wake_up.wav"
)

if(NOT CONFIG_IDF_TARGET_ESP32S3)
    message(FATAL_ERROR "This firmware requires ESP32-S3-BOX-3 (idf.py set-target esp32s3)")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest_path}")
string(MAKE_C_IDENTIFIER "${model_file}" model_symbol)
string(JSON wake_word GET "${model_manifest}" wake_word)
string(JSON feature_step GET "${model_manifest}" micro feature_step_size)
string(JSON probability_cutoff GET "${model_manifest}" micro probability_cutoff)
string(JSON sliding_window GET "${model_manifest}" micro sliding_window_size)
string(JSON tensor_arena GET "${model_manifest}" micro tensor_arena_size)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    WAKE_MODEL_SYMBOL="_binary_${model_symbol}_start"
    WAKE_WORD="${wake_word}"
    WAKE_FEATURE_STEP=${feature_step}
    WAKE_PROBABILITY_CUTOFF=${probability_cutoff}
    WAKE_SLIDING_WINDOW=${sliding_window}
    WAKE_TENSOR_ARENA=${tensor_arena}
)

# Use the same fixed-point frontend as the training pipeline.
include(FetchContent)
FetchContent_Declare(microfrontend
    URL https://codeload.github.com/rhasspy/pymicro-features/tar.gz/96bd69cfad79aa67697e176570d3dd87052c3def
    URL_HASH SHA256=92ea30a9351418a94f084e2af491140607dd8443dfc91b540c7ba0f48d94340d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(microfrontend)
set(frontend_dir "${microfrontend_SOURCE_DIR}/tensorflow/lite/experimental/microfrontend/lib")
set(fft_dir "${microfrontend_SOURCE_DIR}/kissfft")
set(frontend_sources
    kiss_fft_int16.cc fft.cc fft_util.cc filterbank.cc filterbank_util.cc
    frontend.cc frontend_util.cc log_lut.cc log_scale.cc log_scale_util.cc
    noise_reduction.cc noise_reduction_util.cc pcan_gain_control.cc
    pcan_gain_control_util.cc window.cc window_util.cc
)
list(TRANSFORM frontend_sources PREPEND "${frontend_dir}/")
add_library(wake_microfrontend STATIC ${frontend_sources}
    "${fft_dir}/kiss_fft.cc" "${fft_dir}/tools/kiss_fftr.cc")
target_include_directories(wake_microfrontend PUBLIC "${microfrontend_SOURCE_DIR}" PRIVATE "${fft_dir}")
target_compile_definitions(wake_microfrontend PRIVATE FIXED_POINT=16)
target_link_libraries(${COMPONENT_LIB} PRIVATE wake_microfrontend)
