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
