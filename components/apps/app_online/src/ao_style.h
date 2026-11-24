/*
 * ao_style.h —— 在线音乐的**设计系统**（配色 / 背景 / 页签 / 列表行 / 卡片）
 *
 * 出处：assets/UIDesign/ 里的四张图（在线音乐主界面、歌单曲目详情页、账号页面、
 * 搜索页参考）是同一个视觉体系，这里把其中**反复出现的东西**收成一份：
 *
 *   背景    #132536 → #0E1722(55%) → #0A0F16 的斜向渐变 + 右上角 #2878A8 径向光晕
 *   强调色  #63C5FF（页签下划线、序号、开关、连接状态）
 *   卡片    白 6% 底 + 16 px 圆角（账号信息卡、设置卡片）
 *   列表行  无卡片底、靠行距与 7% 白分割线区分；首行用白 6% 圆角块 + 青点高亮
 *   次级字  白 45%~50%；正文白 90%~100%
 *
 * ⚠ 尺寸一律写设计稿里的**参考像素**，再经 UI_PX_W/UI_PX_H 换算（见 ui_metrics.h），
 *   与其它 UI 代码同一条纪律：不出现 800/480 字面量。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 配色 */
/*
 * 页面底色：**#132C3F**（2026-11 用户指定）。
 *
 * 沿革：原来是设计稿那套斜向三段渐变（#132536 → #0E1722 → #0A0F16），
 * 用户看过真机后要求"在线音乐的蓝色背景改成 #132C3F"—— 于是这里改成单色铺底。
 * 层次感由右上角那团 #2878A8 径向光晕（ao_style_glow）提供，不再靠渐变。
 */
#define AO_C_BG        0x132C3F
#define AO_C_GLOW      0x2878A8
#define AO_C_ACCENT    0x63C5FF
#define AO_C_CYAN_TXT  0xA5D8FF
#define AO_C_DANGER    0xE5535B
#define AO_C_OK        0x3E9E86

/* ------------------------------------------------------------------ 背景 */

/*
 * 给 screen 铺底色：**#132C3F 单色**（2026-11 用户指定，见 AO_C_BG）。
 * ⚠ 会先把 screen 的样式全清掉（等价 ui_screen_apply_default_bg 的第一步），
 *   所以**必须在**设置 screen 的 pad/flex 之前调。
 */
void ao_style_screen_bg(lv_obj_t *scr);

/*
 * 右上角那团径向光晕（#2878A8，中心 (25%,45%) 或 (80%,15%)，半径 55%，
 * 渐隐到 0）。返回的对象已经摆满 parent 并**不参与父容器的 flex/grid 布局**，
 * 调用方建完其它子对象即可（先建它 = 画在最底层）。
 *
 * @param cx_pct 光晕圆心 x（百分比，设计稿里主界面 80%、播放器 25% 等）
 * @param cy_pct 光晕圆心 y（百分比）
 * @param opa    最亮处的透明度（设计稿 22%~38%）
 */
lv_obj_t *ao_style_glow(lv_obj_t *parent, int cx_pct, int cy_pct, lv_opa_t opa);

/* ------------------------------------------------------------------ 文本 */

/* 小标题（12 px / 白 45%）："歌单 · 14 个"、"历史搜索" 这类。 */
lv_obj_t *ao_style_caption(lv_obj_t *parent, const char *text);

/*
 * "等东西"时那句转圈文案。
 *
 * 用户要求（2026-11）：别再用干巴巴的"载入中…"，换成有意思的。
 * 这里是一组**轮换**的短句（约每 6 s 换一句，加载久一点就能看到变化），
 * 每隔一会儿取一次不会闪：throttle 之内返回同一句。
 * 想加/换句子只改 ao_style.cc 里那张表，不要在各页面写死文案。
 */
const char *ao_style_loading_text(void);

/*
 * 大标题字体（设计稿 22 px 那一档）：项目自带的 22 px 中文位图字体，
 * 生僻字自动落到 16 px 主字体（见 ui_metrics.h 的 UI_FONT_TITLE_22）。
 * 保留 text 参数是为了调用点整齐，目前不影响返回值。
 */
const lv_font_t *ao_style_title_font(const char *text);

/* ------------------------------------------------------------------ 控件 */

/* 白 6% / 圆角 16 的卡片（账号信息卡、设置卡片那种）。 */
lv_obj_t *ao_style_card(lv_obj_t *parent, int x, int y, int w, int h, int radius);

/*
 * 歌单封面卡片（歌单曲目详情页的左上角）：设计稿的斜向蓝渐变 #2FA4E7 → #174E78、
 * 圆角 18。没有真封面时它就是"占位封面"，卡片里再叠歌单名与类型小字。
 */
void ao_style_cover_card(lv_obj_t *box, int radius);

/* 只有一行居中文字的行（"上一页 / 下一页 / 重试"这种导航行，扁平风格）。 */
lv_obj_t *ao_style_text_row(lv_obj_t *parent, const char *text);

/*
 * 歌单列表的一行（在线音乐主界面 / 我的歌单 / 我添加的歌单共用）：
 *
 *   [●] 每日推荐                                  根据你的口味每天更新
 *   ────────────────────────────────────────────────────────────
 *
 * @param first    true = 首行：白 6% 圆角块 + 左侧青点（设计稿的高亮态）
 * @param divider  true = 行底画 7% 白分割线（最后一行传 false）
 */
lv_obj_t *ao_style_playlist_row(lv_obj_t *parent, const char *title, const char *sub,
                                bool first, bool divider);

/*
 * 歌单详情里的一行曲目（设计稿：序号 / 歌名 / 右侧歌手 / 分割线）：
 *
 *   01  绝对占有 相对自由                                     陈粒
 *   ────────────────────────────────────────────────────────────
 *
 * @param no      序号（1 起），高亮行（first）的序号用强调色
 * @param first   true = 高亮行（设计稿里第一首就是高亮态）
 */
lv_obj_t *ao_style_song_row(lv_obj_t *parent, int no, const char *title, const char *artist,
                            bool first, bool divider);

/* 胶囊按钮（设计稿的"全部播放"、"切换账号 / 重新登录"那一类）。
 * fill / border 传 0 表示不画；radius 传 0 表示"整圆角"(h/2)。 */
lv_obj_t *ao_style_pill(lv_obj_t *parent, int w, int h, int radius, const char *text,
                        uint32_t fill, lv_opa_t fill_opa, uint32_t border,
                        lv_opa_t border_opa, uint32_t text_color);

/* ---------------------------------------------------------------- 长歌名跑马灯
 *
 * 需求（2026-11 用户）："歌单列表里如果有歌名比较长的歌曲，当用户未手动划列表的时候，
 * 就自动滚动显示长歌曲名的歌曲列表项；手指滑动滚动列表的时候停止歌曲文字滚动动画。"
 *
 * 做法：
 *   - ao_style_song_row() 把歌名标签标成"可跑马灯"（LV_OBJ_FLAG_USER_1），
 *     文本超宽时由 LVGL 的 LV_LABEL_LONG_SCROLL_CIRCULAR 自己滚；
 *   - 本模块给清单挂一个 LV_EVENT_SCROLL 回调：列表一滚就**把长歌名切回 DOT**
 *     （= 停住动画，只显示省略号），并起一个单次 timer；
 *   - 手指停下 AO_MARQUEE_IDLE_MS 之后 timer 再把长歌名切回 SCROLL_CIRCULAR 继续滚。
 *
 * ⚠ 页面销毁时必须调 ao_style_marquee_detach_all()：它**不碰任何控件**，只把登记表清空、
 *   把 timer 删掉。为什么不能在这里碰控件：screen 的 LV_EVENT_DELETE 是在子对象**已经
 *   销毁之后**才派发的（见 app_online.cc 的 online_on_delete），那时清单指针已失效。
 */
void ao_style_marquee_attach(lv_obj_t *list);
void ao_style_marquee_detach_all(void);

#ifdef __cplusplus
}
#endif