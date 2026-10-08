/*
 * cxx_heap.cc —— 让 C++ 的 new 分配失败时"返回 NULL"，而不是 abort 整机
 *
 * 为什么需要这个文件（实测崩过一次，别再删）
 * ---------------------------------------------------------------------------
 * lvgl_pinyin_ime 的拼音引擎 libgooglepinyin 是纯 C++，里面到处是 `new`，
 * 但它是**按 NULL 语义**写的 —— 每个 `new` 后面都跟着 `if (NULL == p) return false;`
 * （例如 spellingtrie.cpp:683、dicttrie.cpp:204、matrixsearch.cpp:89）。
 *
 * 问题是 ESP-IDF 的 app 是 -fno-exceptions 编的（CONFIG_COMPILER_CXX_EXCEPTIONS=n），
 * 而 `operator new` 来自 **libstdc++ 的预编译实现**（new_op.o），它按标准
 * **抛 std::bad_alloc**；ESP-IDF 又把 __cxa_allocate_exception 换成了直接 abort()
 * 的桩（components/cxx/cxx_exception_stubs.cpp:184）。于是链路变成：
 *
 *     分配失败 → operator new 抛 std::bad_alloc → __wrap___cxa_allocate_exception
 *              → abort() → 整机重启（core dump）
 *
 * 实测的触发现场（coredump 反查）：
 *     panic_abort ← __wrap___cxa_allocate_exception ← std::bad_alloc::bad_alloc
 *                 ← SpellingTrie::load_spl_trie (spellingtrie.cpp:682)
 *                 ← DictTrie::load_dict ← MatrixSearch::init ← im_open_decoder
 *                 ← ime_engine_open ← ime_session_init ← lv_pinyin_ime_create
 *                 ← app_online.cc 的 modal_build_keyboards()
 * 根因是 `dict` 分区里的内容不是词库（分区表改过之后残留的旧数据），引擎把残留
 * 字节当成 SpellingTrie 头，读出假的 spelling_size_/spelling_num_，接着
 * `spelling_buf_ = new char[spelling_size_ * spelling_num_]`（spellingtrie.cpp:682）
 * 去要一块天文数字的内存。
 *
 * 结论：任何一次内存不足都不该变成整机重启。这里用自己实现的 operator new
 * 替掉 libstdc++ 的：**失败返回 NULL、不抛异常**，与上面的 NULL 检查语义一致。
 * 之后引擎会自己退回 false → lv_pinyin_ime_create() 返回 NULL →
 * app_online 弹出"拼音输入法不可用（dict 分区未烧写）"，其余功能照常。
 *
 * 顺带修掉一个对齐坑
 * ---------------------------------------------------------------------------
 * lvgl_pinyin_ime 的 malloc 包装（ime_alloc.c）在载荷前面塞了 12 字节的记账头，
 * 所以它返回的指针只有 4 字节对齐。libgooglepinyin 会用 `new double[...]` 装 ngram
 * 码本（ngram.cpp:285），而 RISC-V 上对未 8 字节对齐的 double 做浮点访存会触发
 * "Load address misaligned" panic。这里统一用 heap_caps_aligned_alloc(8, ...)，
 * 保证 new/new[] 出来的内存一定 8 字节对齐。
 *
 * delete 一律走 heap_caps_free：它认得 heap_caps_* 系列的任何分配；反过来
 * ime_free() 遇到"不是自己的指针"也会退回 heap_caps_free()，所以
 * new/delete 与 malloc/free 交叉使用也不会踩坏堆。
 */
#include <cstddef>
#include <new>

#include "esp_heap_caps.h"
#include "esp_log.h"

namespace {

const char *TAG = "cxx_heap";

/* libgooglepinyin 用 new 出来的内存装 double，必须 8 字节对齐 */
constexpr size_t kAlign = 8;

void *cxx_alloc(size_t size)
{
    if (size == 0) {
        size = 1;   /* C++ 要求 new(0) 返回唯一的非空指针 */
    }

    /* 与 lvgl_pinyin_ime 的分配策略一致：先 PSRAM，再内部 RAM */
    void *p = heap_caps_aligned_alloc(kAlign, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        p = heap_caps_aligned_alloc(kAlign, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (p == nullptr) {
        /* 这条日志是重启现场唯一的线索：能看出是谁、要多少、还剩多少 */
        ESP_LOGE(TAG, "out of memory: new %u bytes failed (PSRAM free %u, internal free %u)",
                 (unsigned)size,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    return p;
}

void cxx_free(void *ptr)
{
    if (ptr != nullptr) {
        heap_caps_free(ptr);
    }
}

}   // namespace

void *operator new(std::size_t size)
{
    return cxx_alloc(size);
}

void *operator new[](std::size_t size)
{
    return cxx_alloc(size);
}

void operator delete(void *ptr) noexcept
{
    cxx_free(ptr);
}

void operator delete[](void *ptr) noexcept
{
    cxx_free(ptr);
}

/* sized delete：C++14 起编译器可能优先调用这一对 */
void operator delete(void *ptr, std::size_t) noexcept
{
    cxx_free(ptr);
}

void operator delete[](void *ptr, std::size_t) noexcept
{
    cxx_free(ptr);
}

/*
 * 链接锚点（必须有，否则上面这些定义进不了镜像）
 * ---------------------------------------------------------------------------
 * 静态库只按"当前还没解析的符号"收成员，而 libgooglepinyin 排在 libmain.a
 * **之后**：等轮到它引用 operator new 时，libmain.a 早就处理完了，
 * cxx_heap.cc.obj 永远不会被拉出来 —— 结果 operator new 还是 libstdc++ 那个
 * 抛 std::bad_alloc 的版本，崩溃照旧（这就是 ime_dict_stub.c / main.cc 里
 * 记录过的同一个坑）。
 *
 * 所以 main.cc 里主动引用一次这个符号，把本成员从 libmain.a 里拉出来。
 */
extern "C" void cxx_heap_anchor(void)
{
}