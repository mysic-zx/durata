/* 零依赖测试框架。计数器定义在 main.c，各测试文件 extern 共享，
 * 否则 static 在每个翻译单元各一份，main 拿不到真实失败数 */
#ifndef DU_TEST_H
#define DU_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int du_check_fail;
extern int du_total_fail;

#define DU_CHECK(cond) do { \
    if (!(cond)) { \
        printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        du_check_fail++; \
        du_total_fail++; \
    } \
} while (0)

#define DU_CHECK_EQ(a, b) do { \
    long long _x = (long long)(a), _y = (long long)(b); \
    if (_x != _y) { \
        printf("  FAIL %s:%d  %s == %s  (%lld != %lld)\n", \
               __FILE__, __LINE__, #a, #b, _x, _y); \
        du_check_fail++; \
    } \
} while (0)

#define DU_CHECK_NEAR(a, b, eps) do { \
    double _x = (double)(a), _y = (double)(b); \
    if (!(_y - (eps) <= _x && _x <= _y + (eps))) { \
        printf("  FAIL %s:%d  %s ~= %s  (%f vs %f)\n", \
               __FILE__, __LINE__, #a, #b, _x, _y); \
        du_check_fail++; \
    } \
} while (0)

#define DU_TEST_RUN(name, fn) do { \
    du_check_fail = 0; \
    printf("== %s ==\n", name); \
    fn(); \
    printf("  %s\n", du_check_fail == 0 ? "ok" : "FAILED"); \
} while (0)

#endif
