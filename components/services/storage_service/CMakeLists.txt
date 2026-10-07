idf_component_register(
    SRCS "storage_service.cc"
    INCLUDE_DIRS "include"
    REQUIRES
        bsp
        fatfs
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)