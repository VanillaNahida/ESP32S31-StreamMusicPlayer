idf_component_register(
    SRCS "storage_service.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        bsp
        fatfs
        media_meta    # 扫描时读内嵌 ID3/Vorbis 标签（标题/歌手优先用标签）
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)