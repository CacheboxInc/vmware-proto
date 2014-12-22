/*
 * Copyright(2013) Cachebox Inc.
 *
 * except.h
 * 
 */

#ifndef EXCEPT_H
#define EXCEPT_H
#include <assert.h>
#include <stdlib.h>

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#if TEST_EV
#define ASSERT(a) assert(a)

#define ASSERT_FMT(expr, fmt, s...)                                            \
    do {                                                                       \
        if (unlikely(!(expr))) {                                               \
            fprintf(stderr, "Assertion failed: \t");                           \
            fprintf(stderr, "FILE: %s, FUNC: %s, LINE: %d, --> ", __FILE__,    \
                __func__, __LINE__);                                           \
            fprintf(stderr, fmt, s);                                           \
            taskexitall(1);                                                    \
        }                                                                      \
    } while (0)

#define ASSERT_TYPE(l, o, r, type)                                             \
    do {                                                                       \
        type _l = (type) (l);                                                  \
        type _r = (type) (r);                                                  \
        if (unlikely(!(_l o _r))) {                                            \
            fprintf(stderr, "Assertion failed: \t");                           \
            fprintf(stderr, "FILE: %s, FUNC: %s, LINE: %d, --> (%s (0x%"PRIx64") "   \
                "%s %s (0x%"PRIx64"))\n", __FILE__, __func__, __LINE__, #l,          \
		(uint64_t) _l, #o, #r, (uint64_t) _r);                         \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

#define ASSERT_SIGN(l, o, r)    ASSERT_TYPE(l, o, r, int64_t)
#define ASSERT_UNSIGN(l, o, r)  ASSERT_TYPE(l, o, r, uint64_t)

#ifdef __x86_64__
#define ASSERT_PTR(l, o, r)     ASSERT_TYPE(l, o, r, uint64_t)
#else
#define ASSERT_PTR(l, o, r)     ASSERT_TYPE(l, o, r, uint32_t)
#endif

#else
/* Each task would have a exeception handler associated with the taskdata.
 */
#error "assert in production should"
#define ASSERT(a) unlikely(!a) && ("throw exception")
#endif

#endif /* end of EXCEPT_H */
