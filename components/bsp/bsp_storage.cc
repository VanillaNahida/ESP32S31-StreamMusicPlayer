/*
 * bsp_storage.cc —— 板载 microSD（4-bit SDMMC + SD_CTRL）
 *
 * 引脚、总线宽度、频率、挂载点与供电控制全部由板级配置 fs_sdcard 提供。
 * 注意：SD_CTRL(GPIO39) 是**低电平使能**卡供电，由板级的 sdcard_power_ctrl
 * 设备负责，本文件不直接拉 GPIO（走 esp_board_device_power_ctrl()）。
 *
 * 除了"挂 / 卸 / 探"三件事，本文件还提供热插拔真正需要的那两件：
 *   - bsp_sdcard_power_cycle()：给卡做一次**断电重启**（见 bsp.h 的说明）；
 *   - bsp_sdcard_presence_probe()：不挂 FATFS，纯协议层判"卡到底在不在"
 *     —— 板子没有卡检测脚，这是"没插卡"与"插着卡但读不出来"唯一的分界。
 */
#include "bsp.h"
#include "bsp_internal.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>          /* stat()：挂载后体检"真的挂上了吗" */
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"          /* sdmmc_card_t（bsp_sdcard_card_info 用） */
#include "sd_protocol_defs.h"   /* SD_OCR_SDHC_CAP / SD_OCR_S18_RA */
#include "esp_board_manager_includes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_sd";

#define SD_DEV_NAME       ESP_BOARD_DEVICE_NAME_FS_SDCARD   /* "fs_sdcard" */
#define SD_DEFAULT_MOUNT  "/sdcard"

/*
 * 断电重启的两个时延。
 *
 * SD 规范只规定"上电后 1 ms 内进入 idle、之后至少要 74 个时钟才认命令"，
 * 但真正的问题在**掉电**：卡里的电荷泵/电容要放到足够低才会重新走一遍上电流程，
 * 实测几十毫秒的断电才稳。上电后同样要给它一点时间跑完内部初始化，
 * 否则紧接着的 ACMD41 就是超时 —— 那正好会被误判成"没插卡"。
 * 两个值都取 60 ms：比规范保守，但相对"每次挂载失败几秒"的开销可以忽略。
 */
#define SD_POWER_OFF_MS      60
#define SD_POWER_SETTLE_MS   60

static bool s_mounted;
static char s_mount_point[32] = SD_DEFAULT_MOUNT;
static bool s_mount_fail_logged;   /* 没插卡时"挂载失败"只打一次，别每次重试都刷 */
/*
 * 挂载期间有效的卡句柄（= fat->card）。
 *
 * 缓存它是为了**不再每次探测都去问板级管理器要 handle**：
 * esp_board_manager_get_device_handle() 每次都会打一行 INFO
 * （"Device handle fs_sdcard found, ..."），而热插拔探测是每秒一次
 * —— 日志会被这一个轮询刷满。挂载时记下、卸载时清掉，生命周期和 s_mounted 一致。
 */
static sdmmc_card_t *s_card;

/*
 * 板级 fs_sdcard 的配置（引脚/总线/挂载点）。
 *
 * 同样必须缓存：esp_board_manager_get_device_config() 每次都会打一行 INFO
 * （"Device fs_sdcard config found: ..."），而探卡是失败路径上的常态动作。
 * 板级配置在运行期不变，所以缓存是安全的。
 */
static const dev_fs_fat_config_t *s_cfg;

static const dev_fs_fat_config_t *sd_config(void)
{
    if (s_cfg) {
        return s_cfg;
    }
    void *raw = nullptr;
    if (esp_board_manager_get_device_config(SD_DEV_NAME, &raw) != ESP_OK || raw == nullptr) {
        return nullptr;
    }
    s_cfg = (const dev_fs_fat_config_t *)raw;
    return s_cfg;
}

esp_err_t bsp_sdcard_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    if (!esp_board_manager_check_name(SD_DEV_NAME)) {
        ESP_LOGW(TAG, "board has no %s device", SD_DEV_NAME);
        return ESP_ERR_NOT_FOUND;
    }

    /* 挂载点以**板级配置**为准，不从 handle 里读：handle 有可能已经陈旧
     * （见下面那段"假成功"的说明），读它的字段等于读已释放的内存 */
    const dev_fs_fat_config_t *cfg = sd_config();
    if (cfg && cfg->mount_point) {
        /* 工程规范 F9：IDF 开了 -Werror，字符串拷贝一律用 snprintf，不用 strncpy */
        snprintf(s_mount_point, sizeof(s_mount_point), "%s", cfg->mount_point);
    }

    esp_err_t ret = bsp_device_ensure(SD_DEV_NAME);
    if (ret != ESP_OK) {
        /* 没插卡是正常情况：不崩，本地音乐入口显示"未检测到 SD 卡"。
         * 只打一次：热插拔轮询会反复重试（而且调用方在后台重试时已经把日志压掉了）。 */
        if (!s_mount_fail_logged) {
            s_mount_fail_logged = true;
            ESP_LOGW(TAG, "sdcard mount failed: %s (未插卡或卡异常)", esp_err_to_name(ret));
        }
        return ret;
    }
    s_mount_fail_logged = false;

    /*
     * ★ "板级 init 返回 OK" ≠ "真的挂上了"（2026-10 补的兜底）。
     *
     * `esp_board_device_init()` 在 `handle->device_handle != NULL` 时走的是
     * "already initialized" 分支 —— **直接返回 ESP_OK，什么都不做**。
     * 而 device_handle 没被清掉只有一种可能：上一次 deinit 失败了（它会把
     * ref_count 加回去、handle 留着）。那种情况下这里会一路"成功"下去，
     * 我们拿到的 `fat->card` 还是上一次卸载时已经 free 掉的那块内存（悬垂指针，
     * 接下来 CMD13 探的就是它），而监控任务会一直以为"已挂载"，再也不会重试
     * —— 表现就是"插了新卡没反应、还得重启"。
     *
     * 一次 stat(挂载点) 就能把这种假成功打成失败，让上层继续重试。
     */
    struct stat st;
    if (stat(s_mount_point, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ESP_LOGE(TAG, "mount 声称成功但 '%s' 不是目录（板级 handle 陈旧？）",
                 s_mount_point);
        esp_board_manager_deinit_device_by_name(SD_DEV_NAME);
        return ESP_ERR_INVALID_STATE;
    }

    void *handle = NULL;
    ret = esp_board_manager_get_device_handle(SD_DEV_NAME, &handle);
    if (ret != ESP_OK || !handle) {
        ESP_LOGE(TAG, "get %s handle failed: %s", SD_DEV_NAME, esp_err_to_name(ret));
        return (ret == ESP_OK) ? ESP_FAIL : ret;
    }

    dev_fs_fat_handle_t *fat = (dev_fs_fat_handle_t *)handle;
    s_card = fat->card;

    s_mounted = true;
    ESP_LOGI(TAG, "sdcard mounted at %s", s_mount_point);
    return ESP_OK;
}

/*
 * 卡还在不在（见 bsp.h 的说明）。
 *
 * 只发一条 CMD13，不发数据、不碰文件系统，所以可以在轮询里每秒调一次。
 * 卡被拔掉时 CMD13 拿不到响应（硬件超时），返回 ESP_ERR_TIMEOUT / ESP_FAIL。
 *
 * ⚠ 用挂载时缓存的卡句柄，**不要**在这里调 esp_board_manager_get_device_handle()：
 *   那个函数每次都会打一行 INFO（"Device handle fs_sdcard found, ..."），而本函数
 *   是每秒一次的轮询，日志会被它刷满。
 */
esp_err_t bsp_sdcard_probe(void)
{
    if (!s_mounted || s_card == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return sdmmc_get_status(s_card);
}

void bsp_sdcard_unmount(void)
{
    if (!s_mounted) {
        return;
    }
    ESP_LOGI(TAG, "unmounting sdcard");
    /* 先清缓存：deinit 会把板级 handle（含 card）释放掉，留着就是悬垂指针 */
    s_card = nullptr;
    /* 状态先落：不管下面成败，我们都不再认为它挂着 */
    s_mounted = false;
    /*
     * 这里是**必须**走板级 deinit 的：fs_fat 设备的 deinit 会
     * esp_vfs_fat_sdcard_unmount() → 内部 call_host_deinit()，把 SDMMC 也一起
     * 拆干净（S31 上走 sdmmc_host_deinit_slot()，内部同样会把 s_ctlr 置 NULL）。
     * 不拆干净的话，之后再挂载会在 sdmmc_host_init_slot() 上拿到
     * "slot is not available"（ESP_ERR_INVALID_STATE），也就是"换卡必须重启"的根因。
     */
    const esp_err_t ret = esp_board_manager_deinit_device_by_name(SD_DEV_NAME);
    if (ret != ESP_OK) {
        /*
         * ★ 卸载失败必须喊出来（2026-10 补）。
         *
         * 板级 deinit 失败时它会把 ref_count 加回去、`device_handle` 留着不清 ——
         * 于是**下一次** `esp_board_device_init()` 会说 "already initialized"
         * 直接返回 ESP_OK，什么都没挂，而我们会以为挂上了；更糟的是那块
         * fat handle（含 card）已经被 free，成了悬垂指针。
         * 这行不会被后台那个压日志的窗口吞掉（sd_drop_mount 不在窗口里）：
         * 看到它就说明要往板级/驱动层查，而不是继续怀疑这里的状态机。
         */
        ESP_LOGE(TAG, "deinit fs_sdcard failed: %s —— FATFS/VFS 可能没释放，"
                      "下一次挂载会拿到陈旧的 device handle",
                 esp_err_to_name(ret));
    }
}

bool bsp_sdcard_is_mounted(void)
{
    return s_mounted;
}

/* ---------------------------------------------------------------- 供电控制 */

esp_err_t bsp_sdcard_power(bool on)
{
    if (!esp_board_manager_check_name(SD_DEV_NAME)) {
        return ESP_ERR_NOT_FOUND;
    }
    /*
     * 挂载期间只允许"开"：断电会把文件系统从底下抽走（FATFS 的目录项、甚至
     * 正在写的 FAT 表都还在内存里），下一次写卡就会写到空气上。
     * 需要断电重启的路径（热插拔）本来就只在**未挂载**时跑。
     */
    if (!on && s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on && s_mounted) {
        return ESP_OK;   /* 已经上着电了，别再走一遍电源设备（它会重设引脚） */
    }
    return esp_board_device_power_ctrl(SD_DEV_NAME, on);
}

esp_err_t bsp_sdcard_power_cycle(void)
{
    if (s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = bsp_sdcard_power(false);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(SD_POWER_OFF_MS));
    ret = bsp_sdcard_power(true);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(SD_POWER_SETTLE_MS));
    return ESP_OK;
}

/* ---------------------------------------------------------------- 纯协议层探卡 */

bsp_sd_presence_t bsp_sdcard_presence_probe(void)
{
    if (s_mounted) {
        return BSP_SD_PRESENT;   /* 都挂上了，当然在 */
    }
    if (!esp_board_manager_check_name(SD_DEV_NAME)) {
        return BSP_SD_UNKNOWN;
    }
    const dev_fs_fat_config_t *cfg = sd_config();
    if (!cfg || !cfg->sub_type || strcmp(cfg->sub_type, "sdmmc") != 0) {
        return BSP_SD_UNKNOWN;
    }
    const dev_fs_fat_sdmmc_sub_config_t *sub = &cfg->sub_cfg.sdmmc;

    /*
     * 照抄板级 fs_fat/sdmmc 子设备的初始化口径（dev_fs_fat_sub_sdmmc.c）——
     * 引脚、位宽、slot 全来自板级 YAML，本文件一个都不硬编码。
     *
     * 频率取 SDMMC_FREQ_PROBING（400 kHz）：探卡只要"卡在不在"，不需要带宽，
     * 低速对信号质量最宽容（劣质卡/长走线在高速下可能直接握手失败，
     * 那会被误判成"卡有问题"）。
     */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_PROBING;
    host.slot = sub->slot;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    /* 这几个字段在 IDF 里是 gpio_num_t（枚举），而板级配置给的是 int ——
     * 板级管理器自己是 C 文件所以能隐式转换，C++ 必须显式转。 */
    slot.cd    = (gpio_num_t)sub->pins.cd;
    slot.wp    = (gpio_num_t)sub->pins.wp;
    slot.clk   = (gpio_num_t)sub->pins.clk;
    slot.cmd   = (gpio_num_t)sub->pins.cmd;
    slot.d0    = (gpio_num_t)sub->pins.d0;
    slot.d1    = (gpio_num_t)sub->pins.d1;
    slot.d2    = (gpio_num_t)sub->pins.d2;
    slot.d3    = (gpio_num_t)sub->pins.d3;
    slot.d4    = (gpio_num_t)sub->pins.d4;
    slot.d5    = (gpio_num_t)sub->pins.d5;
    slot.d6    = (gpio_num_t)sub->pins.d6;
    slot.d7    = (gpio_num_t)sub->pins.d7;
    slot.width = sub->bus_width;
    slot.flags = sub->slot_flags;

    /* sdmmc_card_t 有一千多字节（含 ext_csd 512 B），别放栈上 ——
     * 调用方（存储服务的监控任务）栈只有 4 KB */
    sdmmc_card_t *card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
    if (!card) {
        return BSP_SD_UNKNOWN;
    }

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        free(card);
        return BSP_SD_UNKNOWN;
    }
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK) {
        err = sdmmc_card_init(&host, card);
    }
    /* 不管成没成都要把主机拆干净：下一次真正挂载（板级管理器）要在同一个 slot
     * 上重新 init_slot()，留着就是 "slot already initialized"。 */
    sdmmc_host_deinit();
    free(card);

    if (err == ESP_OK) {
        return BSP_SD_PRESENT;
    }
    /*
     * 判定口径（`send_op_cond returned 0x107` 那类日志就是这个码）：
     *   ESP_ERR_TIMEOUT / ESP_ERR_NOT_FOUND  → 卡根本没应答 = 没插卡；
     *   其它（CRC 错、不支持的电压/版本、CMD 响应非法…）→ 卡有应答但初始化不过，
     *                                         那就是"卡在，但读不出来"。
     */
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND) {
        return BSP_SD_ABSENT;
    }
    ESP_LOGD(TAG, "presence probe: card answered but init failed (%s)", esp_err_to_name(err));
    return BSP_SD_PRESENT;
}

const char *bsp_sdcard_mount_point(void)
{
    return s_mount_point;
}

esp_err_t bsp_sdcard_capacity_kb(uint32_t *total_kb, uint32_t *free_kb)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t total = 0, free_bytes = 0;
    esp_err_t ret = esp_vfs_fat_info(s_mount_point, &total, &free_bytes);
    if (ret != ESP_OK) {
        return ret;
    }
    if (total_kb) {
        *total_kb = (uint32_t)(total / 1024);
    }
    if (free_kb) {
        *free_kb = (uint32_t)(free_bytes / 1024);
    }
    return ESP_OK;
}

/*
 * 卡的物理信息（"设置 → 存储"详情页要显示的额外几行）。
 *
 * 解码口径**照抄 IDF 的 sdmmc_card_print_info()**（sdmmc/sdmmc_common.c:314-361），
 * 别自己发明：类型靠 ocr 的 CCS/S18R 位判，数据线宽度取 ssr.cur_bus_width
 * （那是**当前**宽度，不是卡支持的最大值）。
 */
esp_err_t bsp_sdcard_card_info(bsp_sd_card_info_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_mounted || s_card == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 同 bsp_sdcard_probe()：用挂载时缓存的句柄，别每次去问板级管理器（它会打 INFO） */
    const sdmmc_card_t *card = s_card;

    out->card_bytes   = (uint64_t)card->csd.capacity * (uint64_t)card->csd.sector_size;
    out->freq_khz     = card->real_freq_khz;
    out->max_freq_khz = (int)card->max_freq_khz;
    out->bus_width    = card->ssr.cur_bus_width ? 4 : 1;
    out->ddr          = card->is_ddr != 0;

    if (card->is_mmc) {
        snprintf(out->type, sizeof(out->type), "MMC");
    } else if (card->is_sdio) {
        snprintf(out->type, sizeof(out->type), "SDIO");
    } else if ((card->ocr & SD_OCR_SDHC_CAP) == 0) {
        snprintf(out->type, sizeof(out->type), "SDSC");
    } else if (card->ocr & SD_OCR_S18_RA) {
        snprintf(out->type, sizeof(out->type), "SDHC/SDXC (UHS-I)");
    } else {
        snprintf(out->type, sizeof(out->type), "SDHC");
    }

    /* CID 里的产品名是 8 字节、**空格填充**的（不是 NUL 结尾），要自己截 + 去尾空格 */
    snprintf(out->name, sizeof(out->name), "%.8s", card->cid.name);
    for (int i = (int)strlen(out->name) - 1; i >= 0 && out->name[i] == ' '; i--) {
        out->name[i] = '\0';
    }
    out->serial = (uint32_t)card->cid.serial;
    return ESP_OK;
}