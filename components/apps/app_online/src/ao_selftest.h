/*
 * ao_selftest.h —— 在线链路自检（默认不编译，见 ao_common.h 的 ONLINE_SELFTEST）
 */
#pragma once

/* 起自检任务（栈 WORK_TASK_STACK，优先级 3）。只在 ONLINE_SELFTEST=1 时真的做事情。 */
void ao_selftest_start(void);
