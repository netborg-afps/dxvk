#pragma once

#include <stdint.h>

// Check windows
#if _WIN32 || _WIN64
   #if _WIN64
     #define ENV64BIT
  #else
    #define ENV32BIT
  #endif
#endif

// Check GCC
#if __GNUC__
  #if __x86_64__ || __ppc64__
    #define ENV64BIT
  #else
    #define ENV32BIT
  #endif
#endif


#if defined(ENV64BIT)

  typedef uint64_t bitset_t;

  static constexpr uint8_t platform_bits            = 64;
  static constexpr uint8_t platform_bits_minus_one  = 63;

  static constexpr bitset_t all_bits_set            = 0xFFFFFFFFFFFFFFFF;
  static constexpr bitset_t one                     = 1;

  constexpr uint8_t getLeadingZeros( uint64_t bitset ) {
    return __builtin_clzll( bitset );
  }


#elif defined (ENV32BIT)

  typedef uint32_t bitset_t;

  static constexpr uint8_t platform_bits            = 32;
  static constexpr uint8_t platform_bits_minus_one  = 31;

  static constexpr bitset_t all_bits_set            = 0xFFFFFFFF;
  static constexpr bitset_t one                     = 1;

  constexpr uint8_t getLeadingZeros( uint32_t bitset ) {
    return __builtin_clz( bitset );
  }

#endif

