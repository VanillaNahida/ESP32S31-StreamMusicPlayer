可以。你这个问题的核心其实不是“LVGL 9 怎么开动画”，而是：

> **怎么在 ESP32-S31 + LVGL 9 上做类似手机 UI 那种“动画进行到一半，用户再次操作后，立即从当前状态接管并转向新的目标”，同时还要保证高 FPS、低 CPU 占用和不抖动。**

我查了 LVGL 9.3 的动画机制，以及乐鑫目前对 ESP32-S31 的 HMI/图形加速支持。**ESP32-S31 非常适合做这种方案**：320 MHz 双核 RISC-V、DDR-PSRAM，并且有 PPA 2D 图形加速和 2D-DMA。乐鑫官方已经展示了 S31 在 800×480 上 LVGL Benchmark 约 50 FPS、1024×600 约 43 FPS，以及实时叠图动画等场景。([Espressif Systems][1])

---

# 一、先说结论：不要把“可打断动画”理解成 `lv_anim` 的普通播放/反向播放

例如：

```text
A → B
```

动画跑到：

```text
A ───────●────── B
         ↑
       当前状态
```

这时候用户操作要求：

```text
A → B
     ↘
       C
```

真正想要的是：

```text
A ───────────── B
        ↑
        │
        └──────→ C
```

而不是：

```text
A → B → C
```

更不是：

```text
A → B
  ← A
  → C
```

**也就是说，新动画必须把“当前视觉状态”作为新的起点。**

这就是所谓：

> **Interruptible / Interruptible Transition / Retargetable Animation**

---

# 二、LVGL 9 本身其实已经给你了一个非常重要的能力

LVGL 9 的 `lv_anim_t` 有：

```c
lv_anim_set_get_value_cb()
```

官方文档明确说明，这个 callback 可以：

> 使用变量当前值，并让动画的 start/end value 相对于当前值计算。

([LVGL][2])

另外，LVGL 的动画本身是：

```c
start_value
      ↓
    path
      ↓
current_value
      ↓
exec_cb
```

而且 `lv_anim_start()` 对同一个：

```text
variable + exec callback
```

的动画会移除原有动画。([LVGL][3])

这正好可以拿来实现可打断动画。

---

# 三、最简单的 LVGL 9 可打断动画

例如一个 panel 左右滑动：

```c
static void anim_x_cb(void * var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}
```

正常动画：

```c
lv_anim_t a;

lv_anim_init(&a);
lv_anim_set_var(&a, panel);
lv_anim_set_exec_cb(&a, anim_x_cb);

lv_anim_set_values(&a, 0, 300);
lv_anim_set_duration(&a, 500);

lv_anim_set_path_cb(&a, lv_anim_path_ease_out);

lv_anim_start(&a);
```

问题来了。

动画进行到 250ms：

```text
x = 180
```

用户突然要求：

```text
→ x = 600
```

不要重新：

```c
lv_anim_set_values(&a, 0, 600);
```

因为这样会产生：

```text
180 → 0 → 600
```

或者视觉上的跳变。

而应该：

```c
current = lv_obj_get_x(panel);

new animation:

current → target
```

也就是：

```c
lv_anim_set_values(&a, current, target);
```

---

# 四、但我更推荐你不要完全依赖 LVGL Object 的当前值

如果你要做的是：

* 类似手机桌面
* 页面滑动
* 卡片动画
* 抽屉
* Modal
* Bottom Sheet
* Slider
* 菜单
* UI 转场
* 复杂多对象动画

我建议直接建立一个：

# **Animation Controller 层**

而不是：

```text
业务代码
   ↓
lv_anim
   ↓
lv_obj
```

改成：

```text
业务状态
   ↓
Animation Controller
   ↓
LVGL
   ↓
Display
```

这是我认为在 ESP32-S31 上最值得做的方案。

---

# 五、核心思想：永远保存“动画状态”

比如一个页面：

```c
typedef struct {
    float x;
    float y;
    float scale;
    float opacity;

    float vx;
    float vy;

    float target_x;
    float target_y;

    uint32_t start_time;
    uint32_t duration;

    bool running;
} ui_anim_state_t;
```

比如：

```text
当前：

x = 183.4
y = 20
scale = 0.87
opacity = 210
```

用户突然改变目标：

```text
target_x = 600
target_scale = 1.0
```

你直接：

```text
当前状态
   ↓
183.4
   ↓
新的目标
600
```

重新计算 duration。

---

# 六、真正高级一点：使用“速度连续”的 Retarget

这是手机 UI 感觉很顺滑的关键。

假设：

```text
A → B
```

动画已经进行了一半：

```text
position = 200
velocity = +800 px/s
```

用户此时要求：

```text
→ C
```

如果简单地：

```text
200 → C
```

速度会瞬间变成：

```text
+800 → 0 → 新速度
```

这会有明显的“顿一下”。

所以更高级的实现应该保持：

```text
position 连续
velocity 也连续
```

也就是：

```text
       原动画
A ─────────────── B
          ↗
         /
        /
       ●────────────── C
```

而不是：

```text
A ─────────●
           ↓
           ●────────── C
```

---

# 七、推荐的数学模型：Spring / Damped Spring

如果你想做出真正类似：

* iOS
* Android
* Material
* Steam Deck
* 游戏 UI
* 高级 HMI

的动画效果，我甚至建议：

> **不要大量依赖 `lv_anim_path_ease_*`，自己实现一个 Spring Animation Controller。**

例如：

```text
x'' + 2ζωx' + ω²(x-target) = 0
```

离散化以后：

```c
velocity += (target - position) * stiffness;
velocity *= damping;

position += velocity;
```

每一帧：

```c
target = desired_position;

velocity += (target - position) * stiffness;
velocity *= damping;
position += velocity;
```

这样你得到一个非常重要的特性：

### 任意时刻修改 target 都可以

例如：

```text
target = 500
```

动画：

```text
0
   100
      200
         300
```

突然：

```text
target = 100
```

马上变成：

```text
         300
        ↙
      260
     210
    160
   120
  100
```

**完全不需要停止旧动画再创建新动画。**

这才是我最推荐的“可打断动画”。

---

# 八、LVGL 9 只负责“显示”，动画控制器负责“运动”

架构可以设计成：

```text
                 ┌──────────────────┐
Touch / Input ──→│ UI State Machine │
                 └────────┬─────────┘
                          ↓
                 ┌──────────────────┐
                 │ Animation Engine │
                 │                  │
                 │ position         │
                 │ velocity         │
                 │ target           │
                 │ spring           │
                 │ easing            │
                 └────────┬─────────┘
                          ↓
                    LVGL 9 Object
                          ↓
                    Render Engine
                          ↓
                 PPA / 2D-DMA / LCD
```

这样做有个巨大好处：

**动画和 LVGL 解耦。**

---

# 九、例如做一个“可打断侧边栏”

假设：

```text
closed = 0
open = 300
```

用户打开：

```c
anim.target = 300;
```

动画：

```text
0
20
45
75
110
...
```

用户突然又按关闭：

```c
anim.target = 0;
```

不用：

```c
lv_anim_del(...)
```

不用：

```c
lv_anim_start(...)
```

甚至不需要重新创建动画。

只需要：

```c
anim.target = 0;
```

下一帧：

```text
current position
      ↓
Animation Engine
      ↓
new target
```

---

# 十、如果坚持使用 LVGL `lv_anim`，推荐这种模式

LVGL 9 本身有：

```c
lv_anim_set_get_value_cb()
```

([LVGL][2])

因此可以设计一个：

```c
static int32_t get_current_value(lv_anim_t * a)
{
    lv_obj_t * obj = lv_anim_get_var(a);

    return lv_obj_get_x(obj);
}
```

然后：

```c
lv_anim_set_get_value_cb(&a, get_current_value);
```

当重新启动动画时：

```text
old animation
       ↓
读取当前值
       ↓
current position
       ↓
new animation
       ↓
target
```

这比简单地：

```c
lv_anim_set_values(&a, 0, target);
```

高级很多。

---

# 十一、但是我不建议“每次触摸都创建一个 lv_anim”

比如触摸：

```text
16 ms
32 ms
48 ms
64 ms
80 ms
...
```

如果每次都：

```c
lv_anim_t a;
lv_anim_init(&a);
lv_anim_start(&a);
```

会造成：

```text
创建
删除
创建
删除
创建
删除
...
```

对于复杂 UI 不漂亮。

更推荐：

```text
一个长期存在的 Animation Controller
```

例如：

```c
typedef struct {

    float value;
    float velocity;
    float target;

    float stiffness;
    float damping;

    bool active;

} ui_spring_t;
```

然后：

```c
void ui_spring_set_target(ui_spring_t *s, float target)
{
    s->target = target;
}
```

触摸事件：

```c
ui_spring_set_target(&sidebar_anim, 300);
```

再次触摸：

```c
ui_spring_set_target(&sidebar_anim, 0);
```

---

# 十二、ESP32-S31 上尤其适合这么干

因为 S31 不只是 CPU 更快。

它还增加了：

```text
PPA
2D-DMA
硬件 JPEG
RGB888
DDR PSRAM
```

乐鑫官方明确把 PPA 定位为图像缩放、旋转、Alpha 混合等像素处理硬件，并使用 2D-DMA 做帧缓冲搬运。([Espressif Systems][1])

所以你应该尽可能把：

```text
CPU
```

用于：

```text
动画状态计算
UI逻辑
输入
业务逻辑
```

而不要让 CPU 疯狂做：

```text
memcpy
alpha blend
scale
rotate
大块 framebuffer 搬运
```

---

# 十三、一个非常关键的问题：不要让动画刷新整个屏幕

例如一个：

```text
100 × 100
```

的按钮移动。

不要：

```text
整个 800×480 framebuffer
```

每次都重新处理。

应该让 LVGL 的 invalidation 机制只处理变化区域。

乐鑫自己的 ESP32-S31 示例也特别强调了这一点：小范围绘制只使 stroke 的 bounding box 失效，而不是整个 800×480 屏幕。([Espressif ESP32-S31平台][4])

---

# 十四、S31 + LVGL 9 我推荐的性能架构

如果你目标是：

> **60 FPS 级别的可打断动画**

我会这样设计：

```text
                   Touch
                     │
                     ▼
              ┌──────────────┐
              │ Input System  │
              └──────┬───────┘
                     │
                     ▼
             ┌────────────────┐
             │ UI State       │
             │ Machine        │
             └───────┬────────┘
                     │ target
                     ▼
          ┌──────────────────────┐
          │ Animation Controller │
          │                      │
          │ position             │
          │ velocity             │
          │ target               │
          │ spring/easing        │
          └───────────┬──────────┘
                      │
                      ▼
                 LVGL 9
                      │
                      ▼
              Dirty Area Render
                      │
             ┌────────┴────────┐
             ▼                 ▼
           PPA              2D DMA
             │                 │
             └────────┬────────┘
                      ▼
                    LCD
```

---

# 十五、LVGL 刷新周期也非常重要

LVGL 9 本身支持：

```c
CONFIG_LV_DEF_REFR_PERIOD
```

乐鑫 FAQ 针对 LVGL 9 给出的高帧率优化建议里就包括：

```text
CONFIG_LV_OS_FREERTOS=y
CONFIG_LV_USE_CLIB_MALLOC=y
CONFIG_LV_USE_CLIB_STRING=y
CONFIG_LV_USE_CLIB_SPRINTF=y
CONFIG_LV_DEF_REFR_PERIOD=15
```

并且建议优化 CPU/PSRAM/Flash、任务优先级以及 GUI 设计。([Espressif Systems][5])

15 ms 大约就是：

```text
66.7 FPS
```

当然最终能不能跑到 60 FPS，还取决于：

```text
屏幕接口
分辨率
刷新率
buffer
渲染复杂度
alpha layer 数量
图片大小
字体
PSRAM 带宽
```

---

# 十六、S31 的 buffer 配置也很关键

如果你是 RGB 屏，建议重点研究：

```text
LVGL partial/direct/full refresh
+
RGB double buffer / triple buffer
+
DMA
+
avoid tearing
```

乐鑫的 BSP 已经提供了：

```text
FULL_REFRESH
DIRECT_MODE
double buffer
triple buffer
avoid tearing
```

等模式。([components.espressif.com][6])

不过不要看到 triple buffer 就无脑打开。

应该根据：

```text
屏幕接口
TE
VSYNC
LVGL render mode
buffer 所在内存
```

组合测试。

---

# 十七、一个特别值得注意的 S31 当前状态

现在 ESP32-S31 的 ESP-IDF 支持仍然处于比较新的阶段。

乐鑫官方开发者页面在 **2026-10-06** 更新的信息显示：

> ESP32-S31 当前最新支持在 ESP-IDF master 分支中。

也就是说，如果你现在正开始做 S31 项目，**不要直接拿很老的 ESP-IDF release 做基准**。([Developer Portal][7])

官方也已经提供：

```text
esp32_s31_korvo_1
```

的 LVGL display example。([components.espressif.com][8])

---

# 十八、我建议你直接做“两层动画系统”

这是我认为最适合你的方案：

### 第一层：LVGL Animation

用于：

```text
简单属性动画
opacity
x
y
width
height
```

例如：

```c
lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
```

LVGL 自带：

```text
linear
ease_in
ease_out
ease_in_out
overshoot
bounce
```

并且允许自定义 path。([LVGL][9])

---

### 第二层：自己的 Animation Engine

用于：

```text
页面转场
拖拽
滑动
侧边栏
Bottom Sheet
卡片
惯性滚动
手势
可打断动画
Spring
```

例如：

```c
typedef struct {
    float value;
    float velocity;
    float target;

    float stiffness;
    float damping;

    float min;
    float max;

    bool active;
} anim_spring_t;
```

核心：

```c
void anim_spring_update(anim_spring_t *a, float dt)
{
    float force =
        (a->target - a->value) * a->stiffness;

    a->velocity += force * dt;

    a->velocity *= a->damping;

    a->value += a->velocity * dt;
}
```

然后：

```c
lv_obj_set_x(panel, (int32_t)anim.value);
```

---

# 十九、进一步优化：不要用固定 `dt`

如果你真的要做高质量动画，最好：

```c
uint32_t now = lv_tick_get();
dt = now - last_tick;
```

然后：

```c
dt = clamp(dt, 0, 32);
```

这样如果某一帧因为 Wi-Fi / Flash / JPEG / 其他任务卡了：

```text
16ms
16ms
48ms
16ms
```

不会导致：

```text
动画突然飞出去
```

---

# 二十、最终推荐方案

如果这是一个正式的 ESP32-S31 产品，我会选择：

### UI 基础动画

```text
LVGL 9 lv_anim
```

### 可打断动画

```text
自定义 Animation Controller
```

### 拖拽/手势

```text
position + velocity
```

### 惯性

```text
velocity decay
```

### 高级转场

```text
Spring / Damped Spring
```

### 渲染

```text
LVGL dirty area
```

### 图像处理

```text
PPA
```

### Framebuffer 搬运

```text
2D-DMA
```

### 大图

```text
PSRAM / DDR-PSRAM
```

### RGB LCD

```text
double/triple buffer
+
VSYNC/TE
```

### RTOS

```text
Core 0 / Core 1 合理分工
```

---

## 最关键的一句话

**不要设计成“动画 A 被打断 → 删除 A → 创建动画 B”。**

而是设计成：

```text
             ┌───────────────┐
             │ Animation     │
             │ State          │
             │                │
             │ current        │
             │ velocity       │
             │ target         │
             └───────┬───────┘
                     │
                     │ 用户随时修改 target
                     ▼
              ┌──────────────┐
              │ Motion Solver│
              │ Spring/Ease  │
              └──────┬───────┘
                     ▼
                   LVGL
```

这样：

```text
A → B
```

跑到 37%：

```text
       ●
      ↗
     ↗
A ──●──────── B
```

突然：

```text
target = C
```

直接：

```text
A ─────────●
          ╲
           ╲
            ╲──────── C
```

甚至还能做到：

```text
位置连续
速度连续
加速度近似连续
```

这才是**真正高效、可打断、手感好的动画系统**。

如果你愿意，我下一步可以直接给你设计一套 **「ESP32-S31 + LVGL 9 的可打断动画框架」**，包括 `anim_controller.c/.h`、Spring、Ease、拖拽惯性、动画抢占、LVGL 9 集成，以及针对 **320 MHz 双核 + PSRAM + PPA + 2D-DMA** 的线程/刷新架构，基本可以直接放进 ESP-IDF 工程里。

[1]: https://www.espressif.com/zh-hans/news/ESP32_S31_Release?utm_source=chatgpt.com "乐鑫发布 ESP32-S31：高性能多协议双核 RISC-V，面向 AI 智能交互 | 乐鑫科技"
[2]: https://lvgl.io/docs/open/9.2/API/misc/lv_anim?utm_source=chatgpt.com "lv_anim.h — LVGL documentation"
[3]: https://lvgl.io/docs/open/9.2/overview/animations?utm_source=chatgpt.com "Animations — LVGL documentation"
[4]: https://esp32-s31.espressif.com/en/community/18?utm_source=chatgpt.com "ESP-Now You Draw I Guess | Espressif"
[5]: https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/peripherals/lcd.html?utm_source=chatgpt.com "LCD - - — ESP-FAQ latest documentation"
[6]: https://components.espressif.com/components/espressif/esp32_s3_lcd_ev_board/versions/4.0.1/readme?language=en&utm_source=chatgpt.com "espressif/esp32_s3_lcd_ev_board • v4.0.1 - ESP Component Registry"
[7]: https://developer.espressif.com/hardware/esp32s31/?utm_source=chatgpt.com "ESP32-S31 status · Developer Portal"
[8]: https://components.espressif.com/components/espressif/esp32_s31_korvo_1/versions/1.0.1/examples/display?language=en&utm_source=chatgpt.com "espressif/esp32_s31_korvo_1 • v1.0.1 • Example display - ESP Component Registry"
[9]: https://lvgl.io/docs/open/9.3/details/main-modules/animation?utm_source=chatgpt.com "Animation (lv_anim) - LVGL 9.3 documentation"
