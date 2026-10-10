# audio_service —— 播放服务
#
# 解码用 libs/espaudiocore（**不是** ESP-GMF / esp_audio_simple_player）：
#   解码 → PCM ringbuf → esp_asrc 重采样到 48k/16/2 → 扬声器 / 蓝牙 / 静音
#
# 五个实现文件按职责拆分（对外契约只有 include/audio_service.h）：
#   audio_service.cc  门面（对外 API、会话锁、事件上报）
#   audio_engine.cc   espaudiocore 会话与事件
#   audio_output.cc   输出级 + 重采样 + 时间轴 + 健康度统计
#   audio_http.cc     在线 HTTP Range 传输（进度/速度/重连）
#   audio_seek.cc     拖动定位（本地库内 seek / 在线 Range 重开）
idf_component_register(
    SRCS
        "audio_service.cc"
        "audio_engine.cc"
        "audio_output.cc"
        "audio_http.cc"
        "audio_seek.cc"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        bsp
        esp_codec_dev
        media_meta
        bt_service
        play_state
        # 解码库（根 CMakeLists 里以 EXTRA_COMPONENT_DIRS 注册 libs/espaudiocore）
        espaudiocore
        # 重采样：espaudiocore 不做采样率/声道转换，由输出级补上
        esp_asrc
        # 在线传输（自研，替代 GMF 的 esp_gmf_io_http）
        esp_http_client
        mbedtls            # esp_crt_bundle：https 流也要校验证书
        esp_ringbuf
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
