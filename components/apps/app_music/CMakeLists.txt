idf_component_register(
    SRCS
        "music_app.cc"            # 对外 5 个 API + APP 注册 + 两个常驻定时器
        "music_state.cc"          # 共享状态（唯一实例 + 粗粒度迁移函数）
        "music_logic.cc"          # 纯逻辑（无 LVGL、无 ESP 头，可 host 单测）
        "music_player.cc"         # 播放控制：起播/换曲/播放暂停/事件/媒体键/seek 受理
        "music_tick.cc"           # 500 ms 常驻心跳：tick 主干拆成 13 段
        "music_online.cc"         # 在线队列所有权 / 异步准备 / 地址重刷 / 起播
        "music_source_local.cc"   # 本地目录数据源（异步扫描线程 + SD 代次）
        "music_cover.cc"          # 封面解码线程 + 像素/描述符产权
        "music_view_player.cc"    # 播放器页：数值刷新/来源药丸/缓冲提示/seek 手势/封面贴图
        "music_view_page.cc"      # 播放器页：布局骨架（建控件 + 注册事件）
        "music_view_lyric.cc"     # 歌词引擎：数据 + 渲染（高亮/居中不变式）
        "music_view_local.cc"     # 本地音乐列表页
        "music_view_output.cc"    # 输出设备按钮 / 选择弹窗 / 音量键
        "music_info.cc"           # 歌曲信息弹窗（ⓘ 按钮：标签/参数/路径，可滑动）
    INCLUDE_DIRS "include"
    REQUIRES
        lvgl
        ui_core
        bsp
        audio_service
        storage_service
        media_meta
        play_state
        bt_service
        online_service
        app_settings
)

# 内部头文件（music_*.h）放在组件根目录，同目录下用相对包含，不进 INCLUDE_DIRS
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
