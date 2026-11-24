# ⚠ SRCS 必须与 src/ 下的 .cc **一一对应**：
#   漏一个 = 那个文件根本没编（增量构建还会被旧对象掩盖），多一个不存在的 = CMake 直接报错。
idf_component_register(
    SRCS
        "src/app_online.cc"      # 注册 + 页面外壳（控件树）+ 页面销毁收尾
        "src/ao_work.cc"         # 工作线程 + 请求/结果协议（唯一跨任务通道）
        "src/ao_provider.cc"     # 音源选择（网易云 / QQ 音乐）+ 两家的文案
        "src/ao_model.cc"        # ★纯逻辑：歌单队列所有权/分页/懒加载/详情 Key（不依赖 LVGL）
        "src/ao_account.cc"      # ★纯逻辑：网易云账号与登录状态机/退避节流（不依赖 LVGL）
        "src/ao_pages.cc"        # 通用 LVGL 层：页面骨架（tab/宿主/滑动切页）+ 控件工厂
        "src/ao_style.cc"        # 设计系统：渐变背景/光晕/页签/扁平行/歌曲行/胶囊
        "src/ao_view.cc"         # 歌单页的两种版式（列表 / 左侧封面卡片 + 右侧曲目清单）
        "src/ao_pl_cover.cc"     # 歌单详情左卡片的封面（异步抓取 + PSRAM 缓存 + 闲置释放）
        "src/ao_page_acct.cc"    # 第 0 页 我的歌单（每日推荐 / 账号歌单懒加载）
        "src/ao_page_pl.cc"      # 第 1 页 我添加的歌单（列表 + 详情 + 长按菜单）
        "src/ao_page_srch.cc"    # 第 2 页 搜索（历史 / 结果分页 / 输入法交互）
        "src/ao_page_account.cc" # 第 3 页 账号（账号信息卡 / 扫码登录 / 退出登录）
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
        voice_service
        online_service
        media_meta
        wifi_service
        lvgl_pinyin_ime
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
# ---------------------------------------------------------------------------
# 歌单详情左卡片的默认封面：assets/cover/pl_default_cover_128x128.bin
# （烘 128、显示时拉伸到 184：扁平图看不出差别，但省下 34 KB flash）
#
# 需求（2026-11 用户）："歌单的默认图片（未加载歌单第一首歌的封面时）使用
# assets/defalut_cover.png"（文件名就是用户给的，拼写照抄）。
# 原始像素由 tools/icons/gen_photo.py 按 assets/cover/photos.txt 生成（不要手改 .bin）。
# 位图 128x128，显示时由 LV_IMAGE_ALIGN_STRETCH 拉到 COVER_SIDE(184)。
# ---------------------------------------------------------------------------
set(PL_DEF_COVER "${CMAKE_SOURCE_DIR}/assets/cover/pl_default_cover_128x128.bin")
if(EXISTS "${PL_DEF_COVER}")
    target_add_binary_data(${COMPONENT_LIB} "${PL_DEF_COVER}" BINARY
                           RENAME_TO "pl_default_cover_bin")
    message(STATUS "app_online: 歌单默认封面 <- assets/cover/pl_default_cover_128x128.bin")
else()
    message(WARNING "app_online: 找不到歌单默认封面 .bin（先跑 tools/icons/gen_photo.py）")
endif()
