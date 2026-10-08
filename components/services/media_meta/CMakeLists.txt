idf_component_register(
    SRCS "media_meta.cc"
         "gbk_table.c"
    INCLUDE_DIRS "include"
    # 封面解码：JPEG 用 esp_new_jpeg，PNG 用官方 libpng 组件（它自带 zlib）。
    # 两条路都在 image_to_rgb565() 里，按文件魔数分派，输出格式一致。
    REQUIRES
        esp_new_jpeg
        libpng
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)