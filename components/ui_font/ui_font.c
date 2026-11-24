/*
 * ui_font.c —— 从 fonts 分区 mmap 加载位图字体，并串成 fallback 链
 *
 * 背景
 * ----
 * 五个预渲染的 LVGL 位图字体（sc16 / sc22 / jp1 / jp2 / dict16）合计约
 * 2.73 MB 常量数据。以前它们以 .c 数组的形式**编进 app**，把 12.31 MB 的
 * app 槽挤得只剩几十 KB。现在字体放在独立的 fonts 分区（partitions.csv），
 * 运行时用 esp_partition_mmap() 映射成只读指针：
 *
 *   - app 里不再有字形位图，app 小了 ~2.73 MB；
 *   - 字体数据**不复制到 RAM**：lv_font_fmt_txt_glyph_dsc_t / glyph_bitmap
 *     直接指在 flash 映射区，只额外 malloc 几 KB 的 cmap 表；
 *   - 换字体只重烧 fonts 分区（idf.py flash 会自动带上，见 CMakeLists.txt）。
 *
 * 字体镜像是 tools/font/pack_fonts.py 从 lv_font_conv 生成的 .c 里抽数据
 * 打成的 blob（格式见那个脚本顶部的说明）。blob 里的表都按 4/16 字节对齐，
 * 这里只做边界/CRC/布局校验，然后把指针接起来。
 *
 * 为什么不用 lv_binfont_create()
 * ------------------------------
 * LVGL 自带的 .bin 加载器会把整个字体解析并**复制到堆**（glyph_bitmap /
 * glyph_dsc / cmaps 全 malloc），既花 ~2.7 MB PSRAM 又要几百毫秒 —— 和
 * "mmap 直接用"的目标相反，所以这里自己接结构体。
 *
 * 约束
 * ----
 *   - 必须**先 ui_font_init()** 再创建任何标签；在那之前 ui_font_text /
 *     ui_font_title22 是全零，拿它渲染会崩（见 include/ui_font.h）。
 *   - blob 的 glyph_dsc 布局必须与固件的 CONFIG_LV_FONT_FMT_TXT_LARGE 一致；
 *     不一致时这里会报错并退回 LVGL 默认字体，不会画成乱码。
 */
#include "ui_font.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "ui_font";

#define UI_FONT_PARTITION_LABEL "fonts"
#define UI_FONT_BLOB_VERSION    1
/* 最多认这么多字体（当前 5 个，留点余量给以后加字重/字号） */
#define UI_FONT_MAX_FONTS       8
/* 段内各表的对齐：glyph_dsc 含 32 位字段，RISC-V 上必须 4 字节对齐；
 * lv_font_conv 的位图与 unicode 表分别按 4/2 字节对齐即可。 */
#define UI_FONT_ALIGN_DSC       4
#define UI_FONT_ALIGN_LIST      2

/* fonts 分区镜像的头（pack_fonts.py 的 HDR，20 字节） */
typedef struct {
    char     magic[4];          /* "UIFP" */
    uint16_t version;
    uint16_t font_count;
    uint32_t blob_size;
    uint32_t payload_crc;       /* 从本头之后到 blob_size 的 CRC32 */
    uint32_t glyph_dsc_size;    /* sizeof(lv_font_fmt_txt_glyph_dsc_t) */
} ui_font_blob_hdr_t;

/* 每个字体的目录项（pack_fonts.py 的 ENT，60 字节） */
typedef struct {
    char     name[16];
    uint32_t offset;            /* 相对 blob 起点，16 字节对齐 */
    uint32_t size;
    uint32_t glyph_count;
    uint32_t bitmap_size;
    uint32_t cmap_num;
    int32_t  line_height;
    int32_t  base_line;
    int32_t  underline_position;
    int32_t  underline_thickness;
    uint32_t bpp;
    uint32_t subpx;
} ui_font_blob_ent_t;

/* 字体段头（pack_fonts.py 的 SEC，16 字节），偏移都相对段起点 */
typedef struct {
    uint32_t cmaps_off;
    uint32_t glyph_dsc_off;
    uint32_t bitmap_off;
    uint32_t reserved;
} ui_font_sec_hdr_t;

/* cmap 描述（pack_fonts.py 的 CMAP，20 字节） */
typedef struct {
    uint32_t range_start;
    uint32_t unicode_list_off;  /* 0 表示 NULL */
    uint32_t glyph_id_ofs_off;  /* 0 表示 NULL */
    uint16_t range_length;
    uint16_t glyph_id_start;
    uint16_t list_length;
    uint8_t  type;
    uint8_t  pad;
} ui_font_cmap_pack_t;

_Static_assert(sizeof(ui_font_blob_hdr_t) == 20, "blob header layout");
_Static_assert(sizeof(ui_font_blob_ent_t) == 60, "blob entry layout");
_Static_assert(sizeof(ui_font_sec_hdr_t) == 16, "section header layout");
_Static_assert(sizeof(ui_font_cmap_pack_t) == 20, "cmap pack layout");

/* 字体在 blob 里的名字。改名字要同步改 tools/font/pack_fonts.py 的 SPECS。 */
enum {
    FONT_SC16 = 0,
    FONT_SC22,
    FONT_JP1,
    FONT_JP2,
    FONT_DICT16,
    FONT_COUNT,
};

static const char *const k_font_names[FONT_COUNT] = {
    "sc16", "sc22", "jp1", "jp2", "dict16",
};

typedef struct {
    lv_font_t                    font;
    lv_font_fmt_txt_dsc_t       *dsc;
    lv_font_fmt_txt_cmap_t      *cmaps;
    const ui_font_blob_ent_t    *ent;
} ui_font_slot_t;

/* 映射状态（字体永远活着，不做 munmap） */
static const esp_partition_t     *s_part;
static esp_partition_mmap_handle_t s_map_handle;
static const uint8_t             *s_base;
static uint32_t                   s_blob_size;

static ui_font_slot_t s_slots[FONT_COUNT];
static bool           s_ready;

/* 对外的可写包装字体（UI 代码通过 UI_FONT_* 使用） */
lv_font_t ui_font_text;
lv_font_t ui_font_title22;

static const ui_font_blob_ent_t *find_ent(const ui_font_blob_hdr_t *hdr, const char *name)
{
    const ui_font_blob_ent_t *ents =
        (const ui_font_blob_ent_t *)(s_base + sizeof(ui_font_blob_hdr_t));
    for (uint32_t i = 0; i < hdr->font_count; i++) {
        /* 目录项里的 name 不保证以 NUL 结尾，但一定是 16 字节内补 0 */
        if (strncmp(ents[i].name, name, sizeof(ents[i].name)) == 0) {
            return &ents[i];
        }
    }
    return NULL;
}

static bool range_ok(uint32_t off, uint32_t len, uint32_t total)
{
    return off <= total && len <= total - off;
}

static bool load_one(const ui_font_blob_hdr_t *hdr, const ui_font_blob_ent_t *ent,
                     ui_font_slot_t *slot)
{
    /* ---- 位置/边界 ---- */
    if (ent->offset % 16 != 0 || !range_ok(ent->offset, ent->size, hdr->blob_size) ||
        ent->size < sizeof(ui_font_sec_hdr_t)) {
        ESP_LOGE(TAG, "%.16s: 段偏移/长度非法 (off=%lu size=%lu)",
                 ent->name, (unsigned long)ent->offset, (unsigned long)ent->size);
        return false;
    }
    if (ent->cmap_num == 0 || ent->cmap_num > 511 ||
        ent->glyph_count == 0 || ent->glyph_count > 65535) {
        ESP_LOGE(TAG, "%.16s: cmap_num=%lu glyph_count=%lu 非法", ent->name,
                 (unsigned long)ent->cmap_num, (unsigned long)ent->glyph_count);
        return false;
    }
    if (ent->bpp != 1 && ent->bpp != 2 && ent->bpp != 4 && ent->bpp != 8) {
        ESP_LOGE(TAG, "%.16s: bpp=%lu 不支持", ent->name, (unsigned long)ent->bpp);
        return false;
    }

    const uint8_t *sec = s_base + ent->offset;
    const ui_font_sec_hdr_t *sh = (const ui_font_sec_hdr_t *)sec;

    if ((sh->glyph_dsc_off % UI_FONT_ALIGN_DSC) != 0 ||
        (sh->cmaps_off % 4) != 0 ||
        (sh->bitmap_off % 4) != 0) {
        ESP_LOGE(TAG, "%.16s: 段内表没有对齐", ent->name);
        return false;
    }
    const uint32_t dsc_bytes =
        ent->glyph_count * (uint32_t)sizeof(lv_font_fmt_txt_glyph_dsc_t);
    if (!range_ok(sh->cmaps_off, ent->cmap_num * sizeof(ui_font_cmap_pack_t), ent->size) ||
        !range_ok(sh->glyph_dsc_off, dsc_bytes, ent->size) ||
        !range_ok(sh->bitmap_off, ent->bitmap_size, ent->size)) {
        ESP_LOGE(TAG, "%.16s: 表越界（cmap=%lu dsc=%lu bmp=%lu size=%lu）", ent->name,
                 (unsigned long)sh->cmaps_off, (unsigned long)sh->glyph_dsc_off,
                 (unsigned long)sh->bitmap_off, (unsigned long)ent->size);
        return false;
    }

    /* ---- 小结构：只需要几 KB，放内部 RAM（每个字形查找都要读） ---- */
    slot->dsc = heap_caps_calloc(1, sizeof(lv_font_fmt_txt_dsc_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    slot->cmaps = heap_caps_calloc(ent->cmap_num, sizeof(lv_font_fmt_txt_cmap_t),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (slot->dsc == NULL || slot->cmaps == NULL) {
        ESP_LOGE(TAG, "%.16s: 分配 cmap 表失败（%lu 项）", ent->name,
                 (unsigned long)ent->cmap_num);
        return false;
    }

    const ui_font_cmap_pack_t *cp = (const ui_font_cmap_pack_t *)(sec + sh->cmaps_off);
    for (uint32_t i = 0; i < ent->cmap_num; i++) {
        if (cp[i].type > LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) {
            ESP_LOGE(TAG, "%.16s: cmap[%lu].type=%u 非法", ent->name,
                     (unsigned long)i, cp[i].type);
            return false;
        }
        if (cp[i].unicode_list_off != 0 &&
            (!range_ok(cp[i].unicode_list_off, (uint32_t)cp[i].list_length * 2, ent->size) ||
             (cp[i].unicode_list_off % UI_FONT_ALIGN_LIST) != 0)) {
            ESP_LOGE(TAG, "%.16s: cmap[%lu] unicode_list 越界/未对齐", ent->name,
                     (unsigned long)i);
            return false;
        }
        if (cp[i].glyph_id_ofs_off != 0) {
            uint32_t width = (cp[i].type == LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL ||
                              cp[i].type == LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY) ? 1 : 2;
            if (!range_ok(cp[i].glyph_id_ofs_off, (uint32_t)cp[i].list_length * width,
                          ent->size) ||
                (cp[i].glyph_id_ofs_off % UI_FONT_ALIGN_LIST) != 0) {
                ESP_LOGE(TAG, "%.16s: cmap[%lu] glyph_id_ofs 越界/未对齐", ent->name,
                         (unsigned long)i);
                return false;
            }
        }

        lv_font_fmt_txt_cmap_t *cm = &slot->cmaps[i];
        cm->range_start   = cp[i].range_start;
        cm->range_length  = cp[i].range_length;
        cm->glyph_id_start = cp[i].glyph_id_start;
        cm->list_length   = cp[i].list_length;
        cm->type          = (lv_font_fmt_txt_cmap_type_t)cp[i].type;
        cm->unicode_list  = cp[i].unicode_list_off
                            ? (const uint16_t *)(sec + cp[i].unicode_list_off) : NULL;
        cm->glyph_id_ofs_list = cp[i].glyph_id_ofs_off
                                ? (const void *)(sec + cp[i].glyph_id_ofs_off) : NULL;
    }

    /* ---- 字体描述符：glyph_dsc / 位图直接指在 mmap 区 ---- */
    lv_font_fmt_txt_dsc_t *d = slot->dsc;
    d->glyph_bitmap  = sec + sh->bitmap_off;
    d->glyph_dsc     = (const lv_font_fmt_txt_glyph_dsc_t *)(sec + sh->glyph_dsc_off);
    d->cmaps         = slot->cmaps;
    d->kern_dsc      = NULL;
    d->kern_scale    = 0;
    d->cmap_num      = (uint16_t)ent->cmap_num;
    d->bpp           = (uint16_t)ent->bpp;
    d->kern_classes  = 0;
    d->bitmap_format = LV_FONT_FMT_TXT_PLAIN;
    d->stride        = 0;

    /* ---- 公共字体结构：拷贝指标，函数指针用 LVGL 的 fmt_txt 实现 ---- */
    lv_font_t *f = &slot->font;
    memset(f, 0, sizeof(*f));
    f->get_glyph_dsc    = lv_font_get_glyph_dsc_fmt_txt;
    f->get_glyph_bitmap = lv_font_get_bitmap_fmt_txt;
    f->release_glyph    = NULL;          /* 明文位图不需要释放 */
    f->line_height      = ent->line_height;
    f->base_line        = ent->base_line;
    f->subpx            = (uint8_t)ent->subpx;
    f->kerning          = LV_FONT_KERNING_NORMAL;
    f->underline_position  = (int8_t)ent->underline_position;
    f->underline_thickness = (int8_t)ent->underline_thickness;
    f->dsc              = d;
    f->fallback         = NULL;
    f->user_data        = NULL;
    slot->ent = ent;
    return true;
}

static void free_slots(void)
{
    for (int i = 0; i < FONT_COUNT; i++) {
        heap_caps_free(s_slots[i].cmaps);
        heap_caps_free(s_slots[i].dsc);
        s_slots[i].cmaps = NULL;
        s_slots[i].dsc = NULL;
    }
}

/* 出问题时退回 LVGL 默认字体：没有中文（会显示方框），但不会崩。 */
static void use_builtin_fallback(const char *why)
{
    const lv_font_t *def = lv_font_get_default();
    ESP_LOGE(TAG, "fonts 分区不可用：%s", why);
    ESP_LOGE(TAG, "  请执行 idf.py flash 烧写 fonts 分区（分区表见 partitions.csv；"
                  "镜像由 tools/font/pack_fonts.py 生成）");
    if (def != NULL) {
        ui_font_text = *def;
        ui_font_title22 = *def;
        ui_font_text.fallback = NULL;
        ui_font_title22.fallback = NULL;
    }
}

void ui_font_init(void)
{
    if (s_ready) {
        return;
    }

    /* 先把对外字体置成默认字体：即使下面任何一步失败，UI 也不会拿全零结构体渲染 */
    const lv_font_t *def = lv_font_get_default();
    if (def != NULL) {
        ui_font_text = *def;
        ui_font_title22 = *def;
    }

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                      UI_FONT_PARTITION_LABEL);
    if (s_part == NULL) {
        use_builtin_fallback("分区表里没有 fonts 分区");
        return;
    }

    esp_err_t err = esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_DATA,
                                       (const void **)&s_base, &s_map_handle);
    if (err != ESP_OK) {
        use_builtin_fallback("esp_partition_mmap 失败");
        return;
    }

    const ui_font_blob_hdr_t *hdr = (const ui_font_blob_hdr_t *)s_base;
    if (memcmp(hdr->magic, "UIFP", 4) != 0) {
        use_builtin_fallback("fonts 分区里没有 UIFP 字体镜像（烧录的是旧数据？）");
        return;
    }
    if (hdr->version != UI_FONT_BLOB_VERSION) {
        use_builtin_fallback("字体镜像版本不匹配");
        return;
    }
    if (hdr->glyph_dsc_size != sizeof(lv_font_fmt_txt_glyph_dsc_t)) {
        ESP_LOGE(TAG, "字体镜像按 %lu 字节字形描述符打包，固件是 %u 字节 —— "
                      "请检查 CONFIG_LV_FONT_FMT_TXT_LARGE 后重新生成 fonts.bin",
                 (unsigned long)hdr->glyph_dsc_size,
                 (unsigned)sizeof(lv_font_fmt_txt_glyph_dsc_t));
        use_builtin_fallback("glyph_dsc 布局不匹配");
        return;
    }
    if (hdr->blob_size > s_part->size ||
        hdr->blob_size < sizeof(ui_font_blob_hdr_t) +
                        hdr->font_count * sizeof(ui_font_blob_ent_t) ||
        hdr->font_count == 0 || hdr->font_count > UI_FONT_MAX_FONTS) {
        use_builtin_fallback("字体镜像大小/字体数非法");
        return;
    }
    if (esp_rom_crc32_le(0, s_base + sizeof(ui_font_blob_hdr_t),
                         hdr->blob_size - sizeof(ui_font_blob_hdr_t)) != hdr->payload_crc) {
        use_builtin_fallback("字体镜像 CRC 校验失败（烧录不完整？）");
        return;
    }
    s_blob_size = hdr->blob_size;

    for (int i = 0; i < FONT_COUNT; i++) {
        const ui_font_blob_ent_t *ent = find_ent(hdr, k_font_names[i]);
        if (ent == NULL) {
            ESP_LOGE(TAG, "字体镜像里缺少 %s", k_font_names[i]);
            free_slots();
            use_builtin_fallback("字体镜像缺项");
            return;
        }
        if (!load_one(hdr, ent, &s_slots[i])) {
            free_slots();
            use_builtin_fallback("字体表校验失败");
            return;
        }
    }

    /* ---- fallback 链：sc16 → jp1 → jp2 → dict16，title22 → sc16 ---- */
    s_slots[FONT_JP2].font.fallback = &s_slots[FONT_DICT16].font;
    s_slots[FONT_JP1].font.fallback = &s_slots[FONT_JP2].font;

    ui_font_text = s_slots[FONT_SC16].font;
    ui_font_text.fallback = &s_slots[FONT_JP1].font;

    ui_font_title22 = s_slots[FONT_SC22].font;
    ui_font_title22.fallback = &ui_font_text;

    s_ready = true;

    ESP_LOGI(TAG, "fonts 分区 0x%lx +0x%lx，映射到 %p",
             (unsigned long)s_part->address, (unsigned long)s_blob_size, (void *)s_base);
    for (int i = 0; i < FONT_COUNT; i++) {
        ESP_LOGI(TAG, "  %-6s %5lu 字形 %6lu B 位图 %2lu cmap",
                 k_font_names[i], (unsigned long)s_slots[i].ent->glyph_count,
                 (unsigned long)s_slots[i].ent->bitmap_size,
                 (unsigned long)s_slots[i].ent->cmap_num);
    }

    /* 自检：走一遍完整的 cmap → glyph_dsc 查找（失败只告警，不中止启动） */
    lv_font_glyph_dsc_t gd;
    if (!lv_font_get_glyph_dsc(&ui_font_text, &gd, 0x4E2D /* 中 */, 0)) {
        ESP_LOGW(TAG, "自检失败：主字体里找不到 U+4E2D");
    }
}

bool ui_font_is_ready(void)
{
    return s_ready;
}
