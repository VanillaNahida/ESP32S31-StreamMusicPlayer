idf_component_register(
    SRCS "media_meta.cc"
    INCLUDE_DIRS "include"
    # 内嵌封面解码：esp_jpeg_dec（预编译库）+ cJSON 无关，只此一个依赖
    REQUIRES
        esp_new_jpeg
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)