/*
 * ao_overlay.h —— **所有 ui_ba 浮层 + 两个键盘 + 账号状态行**
 *
 * 包含两块（原蓝图里的 `ao_qr_login.cc` 与 `ao_modal.cc` —— 落地后两份都只有 ~240 行，
 * 远不到"每个 .cc 300~600 行"的下限，且同属"浮层/键盘"一族，故合并成一份；职责没变）：
 *   1) 二维码登录浮层 + 「我添加的歌单」页顶部的账号状态行；
 *   2) 添加歌单 / 重命名 / 搜索 浮层 + 拼音键盘（懒建，全局复用）+ LVGL 自带键盘。
 *
 * ★★ 内存与生命周期归属（别搞混）：
 *   - 浮层的 scrim（z 浮层根）随 ui_ba 自己销毁；s_qr_dsc / s_qr_px 在 ao_qr_close()
 *     集中释放。
 *   - **拼音键盘（s_ime + s_ime_box）与自带键盘（s_kb）挂在 lv_layer_top 上、
 *     不归任何浮层所有**：只建一次、全局复用，页面销毁时只 ao_ime_hide()，
 *     **绝不能 delete**（删了下次搜索要重新加载词库，卡一秒）。
 */
#pragma once

#include "lvgl.h"

#include "ao_common.h"

/* ================================================================ 二维码登录 */

/* 建二维码浮层（防重入）+ 下单取码 */
void ao_qr_open(void);
/* 停轮询、渐出浮层、释放二维码内存、清指针 */
void ao_qr_close(void);
/* 改二维码浮层状态行文字 */
void ao_qr_set_status(const char *txt);
/* 删轮询定时器（二维码失效时用） */
void ao_qr_stop_poll(void);
/*
 * OREQ_QR_KEY 的结果：记 unikey + 贴二维码（**接管并 free png**）+ 改状态行 + 起轮询。
 * 失败分支（png 为空/err 非 OK）由调用方处理。
 */
void ao_qr_show_key(const char *key, uint8_t *png, size_t png_len);

/* 账号状态行：未登录 = 「扫码登录」入口；已登录 = 昵称 + 「退出登录」 */
void ao_acct_row_build(lv_obj_t *col);

/* ================================================================ 浮层与键盘 */

#define MODAL_RENAME   (-100)    /* 只在本文件用：重命名浮层（不是网络请求） */

/* 建浮层：BA 白卡片（标题 + 输入框 + 提示 + 取消/确定）+（可选拼音键盘） */
void ao_modal_open(int kind);
/* 关浮层（渐出）+ 收起两个键盘 */
void ao_modal_close(void);
/* 收起拼音键盘与自带键盘（搜索页失焦、搜索开始时都要收） */
void ao_ime_hide(void);
/* 给某个输入框挂上拼音键盘并显示（键盘只建一次，挂在 lv_layer_top 上） */
void ao_ime_show_for(lv_obj_t *ta);
/* 查**键盘对象**（不是容器）的隐藏位判断键盘是否打开 */
bool ao_ime_open(void);

/* ================================================================ 收藏到歌单（QQ）
 *
 * 长按歌曲行 → 弹出账号里的歌单（"我喜欢的音乐" + 自建歌单），点一个就收藏进去。
 * 网易云侧没有这个入口（那边只用播放器页的红心）。
 */
/*
 * song_idx   当前队列里的下标
 * cur_dirid  当前列表所属歌单的 dirid（"删除"要用；不知道传 NULL/""）
 */
void ao_collect_open(int song_idx, const char *cur_dirid);
void ao_collect_close(void);
