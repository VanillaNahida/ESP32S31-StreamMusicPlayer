/*
 * ime_dict_stub.c —— 给 lvgl_pinyin_ime 的"内嵌词库"符号补上空定义
 *
 * 背景（这是一个必须补的坑）：
 *   lvgl_pinyin_ime 的 src/data/ime_dict.c 里 prepare_embedded() 会**无条件**引用
 *   `_binary_dict_pinyin_dat_start/_end` —— 这两个符号由组件的 CMake 在
 *   `LV_PINYIN_IME_EMBED_DICT=ON` 时用 target_add_binary_data() 注入（那份内嵌副本
 *   要 1 MB flash）。
 *
 *   本工程为了省 flash 把它设成了 OFF（见根 CMakeLists.txt），词库走 `dict` 分区
 *   （CONFIG_LV_PINYIN_IME_DICT_SRC_PARTITION=y，分区在 partitions.csv 里是
 *   dict @0x22000 共 1.5 MB，用 lvgl_pinyin_ime/tools/flash_dict.ps1 烧写）。
 *   由于组件把那个函数编进来且不可被 GC 掉，OFF 之后链接期就会报
 *   `undefined reference to _binary_dict_pinyin_dat_start`。
 *
 *   于是这里补一对**长度为 0** 的符号：prepare_embedded() 算出来的 blob_size 是 0，
 *   会打印 "embedded dictionary is empty" 并返回 false，随后按配置走 dict 分区。
 *   （用 alias 让 start/end 指向同一地址，这样指针相减必然是 0。）
 *
 *   不去改 lvgl_pinyin_ime 仓库是刻意的：那是另一个项目，改它会影响它的其他使用者。
 *   如果上游愿意修，正确做法是"EMBED_DICT=OFF 时也提供一个空符号"或把那条回退路径
 *   用编译宏隔开。
 */
#include <stdint.h>

/* 占位 1 字节；真正的长度由 start/end 指向同一地址表达（= 0） */
const uint8_t _binary_dict_pinyin_dat_start[1]
    __attribute__((section(".rodata.ime_dict_stub"), used)) = { 0 };

extern const uint8_t _binary_dict_pinyin_dat_end[1]
    __attribute__((alias("_binary_dict_pinyin_dat_start")));
