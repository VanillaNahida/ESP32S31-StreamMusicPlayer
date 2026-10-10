# 对 vendored 库 libs/espaudiocore 的本地补丁

⚠ **`libs/` 在外层仓库的 `.gitignore` 里**，所以 `libs/espaudiocore/` 里的一切改动
（包括这里的 bug 修复）**外层 git 不跟踪**。espaudiocore 自带一个独立的 `.git`，
补丁目前只以"未提交的工作区改动"形式存在于本机：

```
$ git -C libs/espaudiocore status --porcelain
 M CMakeLists.txt
 M lib/avpack/base/mp4.h
 M src/audio_player.cpp
 M src/decoder_mp3.cpp
```

`espaudiocore-local.patch` 就是这四份改动的快照，用来在**重新克隆 / 更新该库**之后还原。

## 应用方法

```powershell
# 在仓库根目录执行（-p1 对应 a/ b/ 前缀；--directory 指向嵌套仓库根）
git apply -p1 --directory=libs/espaudiocore docs/patches/espaudiocore-local.patch
```

或者进入嵌套仓库再 `git apply`：

```powershell
cd libs\espaudiocore
git apply ..\..\docs\patches\espaudiocore-local.patch
```

## 四份改动分别是什么（按重要性）

| 文件 | 改动 | 为什么必须保留 |
| --- | --- | --- |
| `src/decoder_mp3.cpp` | `audio_free(xing_);` 后补 `xing_ = nullptr;` | ★★ **真机崩溃根因**：不置空的话 `close()` 里那次 `if (xing_) audio_free(xing_);` 就是**对同一块内存的第二次 free** → `assert failed: tlsf_free ... "block already marked as free"`。只在**可 seek 的源（本地文件）**上触发，所以库作者以流式为主的验证路径没暴露 |
| `CMakeLists.txt` | ① include 路径去掉 `lib/ffbase`；② 全组件定义 `FF_UNIX / FF_LITTLE_ENDIAN / FFBASE_OPT_SIZE`；③ `decoder_m4a.cpp` 加 `-fpermissive` | ESP-IDF 6.1 + riscv32-esp-elf(GCC 15) 上才编得过：ffbase 自带的 `string.h`/`time.h` 会顶替 C 库同名头；平台探测不认识 newlib/RISC-V；`FFBASE_OPT_SIZE` 是"允许单独编译 sort.c/str-format.c"的开关 |
| `lib/avpack/base/mp4.h` | 新增 `MP4_BBOX_TBL` 宏（C 下 `static const …`，C++ 下 `extern const …`） | avpack 的 box 表用 C 的 tentative definition 分两步写，C++ 里是"大小未知的定义"，连 `-fpermissive` 都不降级 |
| `src/audio_player.cpp` | `ctx_destroy()` 里加逐段日志（`teardown: …`） | **仅诊断用**：那次 double-free 的 panic backtrace 拿不到调用栈（`frame did not save the PC`），只能靠"崩溃前最后一行日志"缩小范围。定位结束后可以删掉（删了就少几行刷屏） |

详细背景见 [`../audio-stack.md`](../audio-stack.md) §6 / §7.1。
