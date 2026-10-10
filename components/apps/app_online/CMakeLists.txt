# ⚠ SRCS 必须与 src/ 下的 .cc **一一对应**：
#   漏一个 = 那个文件根本没编（增量构建还会被旧对象掩盖），多一个不存在的 = CMake 直接报错。
idf_component_register(
    SRCS
        "src/app_online.cc"      # 注册 + 页面外壳（控件树）+ 页面销毁收尾
        "src/ao_work.cc"         # 工作线程 + 请求/结果协议（唯一跨任务通道）
        "src/ao_model.cc"        # ★纯逻辑：歌单队列所有权/分页/懒加载/详情 Key（不依赖 LVGL）
        "src/ao_account.cc"      # ★纯逻辑：网易云账号与登录状态机/退避节流（不依赖 LVGL）
        "src/ao_pages.cc"        # 通用 LVGL 层：页面骨架（tab/宿主/滑动切页）+ 控件工厂
        "src/ao_page_acct.cc"    # 第 0 页 我的歌单（网易云账号 / 每日推荐 / 账号歌单懒加载）
        "src/ao_page_pl.cc"      # 第 1 页 我添加的歌单（列表 + 详情 + 长按菜单）
        "src/ao_page_srch.cc"    # 第 2 页 搜索（历史 / 结果分页 / 输入法交互）
        "src/ao_overlay.cc"      # 所有浮层（二维码/添加/重命名/搜索）+ 两个键盘 + 账号状态行
        "src/ao_result.cc"       # 结果分发（on_result）+ 200 ms 轮询
        "src/ao_selftest.cc"     # 在线链路自检（默认 ONLINE_SELFTEST=0，空壳）
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES
        lvgl
        ui_core
        app_music
        audio_service
        online_service
        media_meta
        wifi_service
        lvgl_pinyin_ime
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
