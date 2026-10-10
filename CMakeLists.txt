# ESP32-S31-Korvo-1 流媒体音乐播放器
# 目标芯片 ESP32-S31（ESP-IDF 6.1 preview target），UI 基于 LVGL 9.5
cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# 自研拼音输入法 lvgl_pinyin_ime
#   - 组件本身在 main/idf_component.yml 里以 override_path 引入（见开发指南 3.2）
#   - 但它 REQUIRES 的 libgooglepinyin 是 vendored 引擎，必须用 EXTRA_COMPONENT_DIRS
#     显式注册（组件不递归搜索子目录）
#   可用 -DLVGL_PINYIN_IME_DIR=... 覆盖默认位置
# ---------------------------------------------------------------------------
set(LVGL_PINYIN_IME_DIR "${CMAKE_CURRENT_LIST_DIR}/../lvgl_pinyin_ime"
    CACHE PATH "Path to the lvgl_pinyin_ime repository")

if(NOT EXISTS "${LVGL_PINYIN_IME_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "lvgl_pinyin_ime not found at ${LVGL_PINYIN_IME_DIR}.\n"
        "Clone https://github.com/VanillaNahida/lvgl_pinyin_ime next to this project, "
        "or pass -DLVGL_PINYIN_IME_DIR=<path>.")
endif()

set(EXTRA_COMPONENT_DIRS
    "${LVGL_PINYIN_IME_DIR}/src/third_party/libgooglepinyin"
    # 项目 components/ 的搜索只覆盖一层，故显式登记二级目录
    "${CMAKE_CURRENT_LIST_DIR}/components/services"
    "${CMAKE_CURRENT_LIST_DIR}/components/apps"
    # 解码库 espaudiocore（vendored）：自成一体的组件，放在 libs/ 下
    # 与管道的其它部分隔离。音频链路的解码全部走它，不再是 ESP-GMF。
    "${CMAKE_CURRENT_LIST_DIR}/libs/espaudiocore"
)

# 词库放在独立的 dict 分区（见 partitions.csv），不把 1 MB 回退副本链进 app；
# app 分区只有 4 MB，拼音字体已占约 2.4 MB。
set(LV_PINYIN_IME_EMBED_DICT OFF CACHE BOOL
    "Skip linking the 1 MB dict_pinyin.dat fallback into the application")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s31_stream_music_player)