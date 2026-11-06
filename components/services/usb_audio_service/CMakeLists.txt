# usb_audio_service —— USB 声卡（UAC 1.0 Speaker）输出
#
# 只做三件事：装 USB Host + UAC 类驱动、检测插拔、把 48k/16/2 的 PCM 写进 UAC 端点。
# **不碰 UI、不改音频路由** —— 路由由 audio_service 决定，插拔事件通过状态回调通知它。
#
# 依赖的两个组件是 esp_video(uvc) 带进来的托管件（本工程 managed_components 里已有，
# 版本 1.5.0，targets 都包含 esp32s31）：
#   espressif/usb            —— IDF 6.x 的 USB Host 库（IDF 树里已经没有 usb 组件了）
#   espressif/usb_host_uac   —— UAC 1.0 类驱动
idf_component_register(
    SRCS "usb_audio_service.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        usb_host_uac
        usb
        esp_ringbuf
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
