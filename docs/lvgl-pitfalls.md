# LVGL 9 踩坑与铁律（真机崩溃换来的）

本文件只记**已经用真机日志定死根因**的坑。改 UI 代码前扫一眼，尤其是"全局复用的控件"
和"挂在 `lv_layer_top()` 上的浮层 / 键盘"。

---

## 1. ★★ 键盘持有 textarea 指针：浮层删了它不清（2026-10 真机崩溃）

### 现场

```
Load address misaligned                                  ← RISC-V 非对齐访问异常
MEPC: lv_color_over32 (managed_components/lvgl__lvgl/src/misc/lv_color_op.c:109)
MTVAL: 0x00000009                                        ← 故障地址是 0x9，典型垃圾指针
```

触发条件（用户原话）：**"连续点击和关闭在线音乐右上角的添加歌单组件"**。

### 根因

LVGL 的 `lv_keyboard_set_textarea()` **只存指针**，而且**下一次设置时会先去动旧对象**：

```c
/* managed_components/lvgl__lvgl/src/widgets/keyboard/lv_keyboard.c */
void lv_keyboard_set_textarea(lv_obj_t * obj, lv_obj_t * ta)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_keyboard_t * keyboard = (lv_keyboard_t *)obj;

    /*Hide the cursor of the old Text area if cursor management is enabled*/
    if(keyboard->ta) {
        lv_obj_remove_state(keyboard->ta, LV_STATE_FOCUSED);   /* ← 写旧 textarea！ */
    }

    keyboard->ta = ta;
    ...
}
```

它**没有**注册 textarea 的 `LV_EVENT_DELETE`（整个文件里搜不到 `LV_EVENT_DELETE`），
所以 textarea 被删掉之后 `keyboard->ta` 就是**悬垂指针**。而本工程的键盘是**全局复用**的
（`ao_overlay.cc` 的 `s_kb`）：

1. 开浮层 → `lv_keyboard_set_textarea(s_kb, ta1)`；
2. 关浮层 → 只 `lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN)`，**指针没清**；
3. 浮层渐出动画结束后 scrim + ta1 被真删；
4. 再开浮层 → `lv_keyboard_set_textarea(s_kb, ta2)` → 第 3 行对 **已释放的 ta1**
   写状态位 → 踩坏 LVGL 对象树；
5. 下一次刷新绘制到被踩坏的对象 → `lv_color_over32` 读到垃圾指针 → 崩。

### 修法（`components/apps/app_online/src/ao_overlay.cc`）

**在浮层真的被删掉之前解绑**，趁 textarea 还活着：

```c
void ao_modal_close(void)
{
    if (s_kb) {
        lv_keyboard_set_textarea(s_kb, NULL);   /* ★ 必须在 textarea 还活着时做 */
        lv_obj_add_flag(s_kb, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ime) {
        lv_pinyin_ime_attach(s_ime, NULL);      /* 拼音组件同理（它也不会自己清） */
        lv_pinyin_ime_hide(s_ime);
    }
    ...
}
```

`set_textarea(kb, NULL)` 里的 `lv_obj_remove_state(keyboard->ta, ...)` 此刻操作的是
**还合法**的对象，所以安全 —— 顺序绝不能颠倒（先删浮层再解绑 = 照样踩空）。

`modal_on_delete`（scrim 的 DELETE 回调）里也补了一道，**那里做同样安全**，因为
LVGL 的 `lv_obj_delete()` 是"**先给对象自己发 LV_EVENT_DELETE、之后才删子对象**"
（`lv_obj_tree.c` 的 `obj_delete_core()`），回调触发时 textarea 还活着。

### 同款排查结论（全仓库）

| 位置 | 是否踩坑 | 说明 |
| --- | --- | --- |
| `app_online/ao_overlay.cc` 的 `s_kb` / `s_ime`（全局复用） | ❌ **踩了**（本次崩溃） | 已修：close + delete 两处都解绑 |
| `app_momotalk/app_momotalk.cc` 的 `s_ime`（全局复用） | ⚠ 潜在 | 页面销毁时只 hide 没解绑 → 已补 `lv_pinyin_ime_attach(s_ime, NULL)`（页面 DELETE 回调里，textarea 仍活着） |
| `app_settings/as_page_wifi.cc` 的 `kb` | ✅ 安全 | 键盘是**浮层的子对象**、每次开浮层新建，跟 textarea 同生共死，且不会再 `set_textarea` |

> 拼音组件（`D:\Codes\lvgl_pinyin_ime`）比 LVGL 键盘规矩一点：`lv_pinyin_ime_attach()`
> 只做 `ctx->ta = ta`，重绑时**不碰旧对象**；而且它的 dismiss 回调有
> `if (lv_obj_has_flag(ime, LV_OBJ_FLAG_HIDDEN)) return;` 的门禁，所以"隐藏着"时
> 不会去读悬垂的 `ctx->ta`。但**它自己在 textarea 删除时也不清理** —— 照样要显式解绑。

### 铁律

> **凡是"比 textarea 活得久"的控件（全局键盘、常驻浮层、跨页复用的对象），
> 在 textarea 被销毁前必须显式解绑（置 NULL）。**
> 判断方法：搜 `lv_keyboard_set_textarea` / `lv_pinyin_ime_attach` / `lv_obj_set_user_data`
> 之类"存指针"的调用，问一句"**这个指针的 owner 会不会先死**"。
> 顺带记住 LVGL 的删除顺序（父对象先收 DELETE、再删子对象），它决定了
> "在 DELETE 回调里还能不能安全地碰子对象"。

---

## 2. 浮层/弹窗一律挂 `lv_layer_top()`，且必须异步删

（历史记录，2026-10 之前就定下的）弹窗若挂在当前页 screen 上，页面被 pop 时回调会指空；
挂在 `lv_layer_top()` 上则要显式管理生命周期（页面切走时主动 close）。

## 3. 长按菜单要等手指抬起来再建

`lv_async_call` + 轮询手指状态（`MENU_GRACE_MS`），否则"按下即建菜单 + 抬手触发原控件
CLICKED"会连环误触。

---

## 4. ★ 可滚动容器的**裁剪不算 padding**：滚动内容会盖住同级的标题（2026-10 真机照片）

### 现场（用户照片）

文件管理页往下滑一点，"文件管理 /sdcard · 共17项"这行标题被列表行**压掉一半**；
再滑一点，标题整个消失。返回钮没事（它在 `lv_layer_top` 上，z 序永远最高）。

### 根因

页面的排法是"标题 label 和列表容器都是 `scr` 的**同级**子对象、都铺满整屏，
靠列表自己的 `pad_top = 40` 把行**错开**到标题下面"：

```c
lv_obj_set_style_pad_all(scr, UI_PAGE_PAD, ...);
lv_obj_set_style_pad_top(scr, UI_STATUSBAR_H + UI_PAGE_PAD, ...);
lv_obj_t *head = lv_label_create(scr);            /* 标题：align 到 (68, 10) */
...
s_list_col = lv_obj_create(scr);
lv_obj_set_size(s_list_col, LV_PCT(100), LV_PCT(100));   /* 铺满整屏！ */
lv_obj_set_style_pad_top(s_list_col, UI_PX_H(40), ...);  /* ← 指望它把行推下去 */
```

**LVGL 裁剪一个对象的子对象时用的是这个对象自己的盒子（`obj->coords`），
padding 只影响"子对象摆在哪"，不影响"能画到哪"**（要 `LV_OBJ_FLAG_OVERFLOW_VISIBLE`
才反过来）。所以列表往上滚时，行可以一路画到容器盒子顶端 —— 而那个顶端在
`pad_top` 之上、正好压在标题上。**"用 padding 把两个铺满屏的对象错开"这个写法本身就是错的。**

### 铁律

**标题栏和滚动区必须是两个盒子，父容器用 flex 列排：**

```c
lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);      /* 父容器：纵向 flex */
lv_obj_set_style_pad_row(scr, 0, LV_PART_MAIN);
lv_obj_t *bar = lv_obj_create(scr);                  /* ① 标题栏：固定高 UI_TITLEBAR_H */
lv_obj_set_size(bar, LV_PCT(100), UI_TITLEBAR_H);
lv_obj_t *list = lv_obj_create(scr);                 /* ② 滚动区：吃掉剩余高度 */
lv_obj_set_width(list, LV_PCT(100));
lv_obj_set_flex_grow(list, 1);
lv_obj_set_scroll_dir(list, LV_DIR_VER);
```

这样滚动区的盒子**物理上**就在标题栏下面，行再滚也够不着标题。
（`app_momotalk` 页从一开始就是这么排的；`music_view_local` 与 `as_page_shell`
2026-10 按这条改过来。）

⚠ 父容器一旦是 flex，**子对象的 `lv_obj_align()` 会被布局覆盖**。要绝对定位就必须
给它加 **`LV_OBJ_FLAG_FLOATING`**（LVGL 的 flex 会跳过带这个 flag 的子对象，见
`lv_flex.c` 里 `flex_item_is_ignored` 的那个位掩码），否则它会被当成一个普通 flex item
排到别处去（设置页右上角那个动作按钮就是这么处理的）。

⚠ 反过来说：**这个坑只在"滚动容器和标题是同级且都铺满"时出现**。播放器页那种
"信息在右栏、列表在子盒子里"的结构不受影响；`app_online` 的"标题行 / tab 行 / 页面宿主"
本来就是三个 flex 兄弟，也没问题。

---

## 5. ★ 悬在导航栏上的返回钮：三条一起犯才会正常（2026-10 定版版面）

版面口径（`ui_metrics.h` 的 `UI_TITLEBAR_*` / `UI_BACK_BTN_*`）：**返回钮的圆心落在
导航栏中线上**（与标题、页签、右上角按钮同一条线），按钮 64 px 比栏高 48 大，
所以上下各悬出 `UI_BACK_BTN_DIP`(8 px)——上边正好贴状态栏下沿、下边正好压到内容区第一行。
这么一摆就同时踩到 LVGL 的三个默认行为，缺一条就出错：

| 坑 | 现象 | 正确做法 |
| --- | --- | --- |
| ① **父容器会裁剪子对象**（裁剪用的是父对象的盒子） | 按钮放进高度只有 `UI_TITLEBAR_H`(48) 的导航栏行里 → **悬出来的部分被切掉** | 按钮**挂到 screen** 上（不是导航栏行里），父容器是 flex 就加 `LV_OBJ_FLAG_FLOATING`，再用 `ui_back_button_place()` 摆位（它自己扣父容器 padding，落点全局一致） |
| ② **后建的画在上面**（同级按创建顺序叠） | 按钮先建、滚动 body 后建 → body 里往上滚的行**盖住按钮** | 页面自带的返回钮**最后建**（全局返回键挂在 `lv_layer_top()` 上，天然最高，不受影响） |
| ③ **按钮 z 序在页面内容之上**，会吃掉下面的点击 | 悬出来的下半截压住第一行列表/第一个页签，既看不见也点不到 | 栏下**第一块控件**让开 `UI_BACK_BTN_DIP`(8 px)：列表/body 的 `pad_top`、`app_online` 的 tabs `pad_top`。**背景类内容（封面/壁纸）不用让** —— 压在背景上才是那个味道 |

①的一个连带结论：**"用 padding 把按钮挤出去"是不行的** —— padding 只影响子对象摆在哪，
不影响能画到哪（同 §4）。悬出去的东西必须挂在**不会被裁到的父容器**上。

另：`lv_obj_align()` 的偏移是相对**父容器内容区**（扣掉 padding）算的。本工程页面 screen
来自 `as_make_screen()`（`pad_all = UI_PAGE_PAD`、`pad_top = UI_TITLEBAR_Y`），而全局返回键的
父容器是无 padding 的 `lv_layer_top()` —— 各页自己写 `lv_obj_align(btn, ..., UI_BACK_BTN_X,
UI_BACK_BTN_Y)` 的话，两边**绝对**位置会差一个 padding，返回钮对不齐。
统一走 `ui_back_button_place()`（源码见 `components/ui_core/ui_core.cc`）。

### 5.1 ⚠ 返回钮的按下反馈：拿 lv_button 去调 `lv_image_set_scale()` = 越界写堆 + 毫无反应

2026-10 的真 bug，值得单独记一条：返回钮原来自己写了一套缩放动画，`exec_cb` 里是

```c
lv_image_set_scale((lv_obj_t *)var, (uint32_t)v);   /* var 其实是 lv_button，不是 lv_image */
```

`lv_image_set_scale()` 的实现是 `lv_image_t * img = (lv_image_t *)obj;` 然后直接写
`img->align / img->scale_x / img->scale_y`（`lv_image.c:386`）。而 `lv_button_t` 里
**根本没有这些成员**（`lv_button.h` 连结构体都没定义，实例就是 `lv_obj_t`），
本工程又关掉了 `CONFIG_LV_USE_ASSERT_OBJ`（`LV_ASSERT_OBJ` 退化成只判空），
于是这句按 `lv_image_t` 的偏移去写一块 `lv_obj_t` 大小的内存 —— **越界写堆**；
而按钮的绘制路径根本不读那些字段，所以**按住毫无反馈**（把
`BACK_PRESS_SCALE` 从 192 调到 150、时长调到 300 ms 都没有任何变化）。

现在统一走 `ui_ba.cc` 的 `ui_ba_press_feedback()`（弹窗按钮 / 选项行 / ✕ / 返回钮
共用一份）：`transform_scale_x/y`（绘制期变换，不参与布局，256 = 100%）——
按住 ≈92%、松手 150 ms 缓出复原、**复原动画跑完**才派发回调。教训一句话：
**给对象改属性之前先确认它真的是那个类**（`lv_obj_check_type()`），
或者干脆用不吃类型的通用属性。

