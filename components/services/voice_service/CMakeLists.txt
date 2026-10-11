idf_component_register(
    SRCS
        "voice_service.cc"       # 宿主层：WebSocket + 麦克风 + 自动暂停播放
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl
        lvgl_pinyin_ime          # 语音协议客户端（组件本身不含网络/麦克风）
        esp_websocket_client     # ws_open / ws_send_text / ws_close
        esp_board_manager        # 取板级 audio_adc 器件句柄
        esp_codec_dev            # 麦克风 PCM 读取
        audio_service            # 识别期间暂停/恢复播放
)

# 部分聚合初始化（esp_websocket_client_config_t / esp_codec_dev_sample_info_t）会触发
# -Wmissing-field-initializers，IDF 默认按 error 处理。
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
