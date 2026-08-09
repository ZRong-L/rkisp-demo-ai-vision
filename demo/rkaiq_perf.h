/* SPDX-License-Identifier: Apache-2.0
 * rkaiq_perf.h — 性能测试工具（参考 slice_capture_test.c 的 get_timestamp / DEBUG_USE_TIME）
 */
#ifndef RKAIQ_PERF_H
#define RKAIQ_PERF_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>

/* 性能埋点总开关：注释掉则关闭所有性能统计 */
#define DEBUG_USE_TIME

#ifndef PERF_WINDOW_FRAMES
#define PERF_WINDOW_FRAMES 100
#endif

static inline uint64_t get_timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

typedef struct {
    const char *name;
    double     sum_us;
    uint64_t   win_start;
    int        cnt;
} perf_counter_t;

static inline void perf_init(perf_counter_t *c, const char *name)
{
    c->name = name;
    c->sum_us = 0;
    c->win_start = 0;
    c->cnt = 0;
}

static inline void perf_sample(perf_counter_t *c, double us)
{
    if (c->win_start == 0)
        c->win_start = get_timestamp();
    c->sum_us += us;
    c->cnt++;

    if (c->cnt >= PERF_WINDOW_FRAMES) {
        uint64_t now = get_timestamp();
        double win_ms = (double)(now - c->win_start) / 1000.0;
        double fps = (win_ms > 0) ? (c->cnt * 1000.0 / win_ms) : 0.0;
        double avg_us = c->sum_us / c->cnt;

        printf("[perf] %-24s avg=%.1fus (%.2fms)  fps=%.1f  over %d frames\n",
               c->name, avg_us, avg_us / 1000.0, fps, c->cnt);

        c->sum_us = 0;
        c->win_start = 0;
        c->cnt = 0;
    }
}

/* AI 检测流共享时间戳：定义在 rkaiq_infer.cpp */
typedef struct {
    uint64_t ai_t1;
    uint64_t ai_t2;
    uint64_t ai_t3;
    uint64_t ai_t4;
} rkaiq_perf_ai_t;

extern rkaiq_perf_ai_t g_perf_ai;

#ifdef __cplusplus
}
#endif

#endif /* RKAIQ_PERF_H */
