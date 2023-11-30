#pragma once

#ifdef __GNUC__
#define likely(x)   __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)
#define force_inline inline __attribute__((always_inline))
#define no_inline    __attribute__ ((noinline))
#else
#define likely(x)   (x)
#define unlikely(x) (x)
#define force_inline inline
#define no_inline
#endif
