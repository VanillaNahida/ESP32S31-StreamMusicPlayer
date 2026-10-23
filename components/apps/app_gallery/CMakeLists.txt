idf_component_register(
    SRCS
        "gallery_app.cc"      # 页面骨架 + 当前索引 + 轮询解码结果 + 注册
        "gallery_scan.cc"     # 扫 SD 卡（工作线程）+ 文件头读原始分辨率
        "gallery_decode.cc"   # 解码工作线程（core 0）：静图 -> RGB565，GIF -> 原始字节
        "gallery_view.cc"     # 查看器：双指缩放 / 双击 / 拖动 / 左右滑 / ⓘ 详情
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."     # gallery_internal.h
    REQUIRES
        lvgl
        ui_core
        media_meta
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
