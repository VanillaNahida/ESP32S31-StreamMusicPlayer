idf_component_register(
    SRCS "media_meta.cc"        # 对外门面：只把 media_meta_* 转发给 mm_*.cc
         "mm_bits.cc"           # 字节序 / synchsafe / MIME 判定（纯逻辑，无 IO）
         "mm_lrc.cc"            # LRC 解析 + 时间轴 + 编码归一化（UTF-8/UTF-16/GBK）
         "mm_id3.cc"            # ID3v2：APIC/PIC 封面帧、USLT/SYLT 歌词、文本编码
         "mm_cover.cc"          # 封面提取编排 + 独立图文件 / 内存缓冲入口
         "mm_image.cc"          # JPEG/PNG → RGB565 + 盒式降采样
         "mm_probe.cc"          # MP3/FLAC/WAV/ADTS 参数探测 + 魔数分派
         "mm_tags.cc"           # 文本标签：MP3(ID3v2) / FLAC(VORBIS_COMMENT) 的专辑·作曲家
         "mm_lyrics_file.cc"    # 歌词三级查找 + /lrc 落盘
         "gbk_table.c"
    INCLUDE_DIRS "include"
    # 封面解码：JPEG 用 esp_new_jpeg，PNG 用官方 libpng 组件（它自带 zlib）。
    # 两条路都在 mm_image.cc 的 mm_image_to_rgb565() 里，按文件魔数分派，输出格式一致。
    # esp_driver_jpeg = S31 的**硬件** JPEG 解码器（SOC_JPEG_CODEC_SUPPORTED=1）。
    # 只用在"大图"那条路上（mm_photo_file_rgb565 → hw_jpeg_to_rgb565），
    # 输出封顶 4 MB；更大的图仍走 esp_new_jpeg 软解（它能解码期缩放）。
    # 封面/歌词那条老路（mm_image_to_rgb565）**不动**，仍全软解。
    REQUIRES
        esp_new_jpeg
        esp_driver_jpeg
        libpng
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
