idf_component_register(
    SRCS "online_service.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_http_client
        mbedtls
        cjson
        nvs_flash
        fatfs
        esp_timer
        wifi_service
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

# ---------------------------------------------------------------------------
# 内置网易云 cookie（免扫码登录）
#
# CONFIG_ONLINE_NETEASE_CK_FILE 是**相对工程根目录**的路径（见 main/Kconfig.projbuild），
# 默认 assets/ncm-ck.txt。配置为空、或文件不存在 -> 什么都不做，online_service.cc 里
# 的 ONLINE_NCM_CK_BUILTIN 也就不会定义，行为与没有这个功能时完全一致。
#
# 三种嵌入方式里选 TEXT：它会在数据末尾补一个 '\0'，于是
# `_binary_ncm_ck_builtin_start` 可以直接当 C 字符串用（BINARY 方式没有结尾 0）。
# ---------------------------------------------------------------------------
set(ONLINE_NCM_CK_REL "${CONFIG_ONLINE_NETEASE_CK_FILE}")
if(ONLINE_NCM_CK_REL)
    set(ONLINE_NCM_CK_ABS "${CMAKE_SOURCE_DIR}/${ONLINE_NCM_CK_REL}")
    if(EXISTS "${ONLINE_NCM_CK_ABS}")
        target_add_binary_data(${COMPONENT_LIB} "${ONLINE_NCM_CK_ABS}" TEXT
                               RENAME_TO "ncm_ck_builtin")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ONLINE_NCM_CK_BUILTIN=1)
        message(STATUS "online_service: 内置网易云 cookie <- ${ONLINE_NCM_CK_REL}")
    else()
        message(STATUS
            "online_service: CONFIG_ONLINE_NETEASE_CK_FILE='${ONLINE_NCM_CK_REL}' "
            "指向的文件不存在，不嵌入内置 cookie")
    endif()
endif()
