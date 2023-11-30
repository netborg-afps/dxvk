#pragma once

#include <stdint.h>
#include <atomic>

#include "../util_assert.h"
#include "../util_benchmark.h"
#include "../util_likely.h"
#include "../util_error.h"
#include "../util_platform.h"
#include "../util_math.h"


namespace dxvk::sync {

  /**
   * \brief Bitset Array Free-List
   *
   * Fulfills the objective of a free-list in the most compact form,
   * very tightly packed into a continuous memory region. It is a way
   * to minimize cache pressure onto the rest of the application.
   * It's thread-safe using lockfree atomic operations.
   *
   * The payload data it's managing is also in continuous memory,
   * given a fixed size per chunk, the position of a bit within the
   * bitset array maps to the position of its respective chunk within
   * the payload data array. Consecutive allocations are likely to be
   * grouped together, providing good cache locality.
   */
  class BitsetArrayFreeList {

  public:


    BitsetArrayFreeList( uint64_t totalMemorySize, uint32_t blockSize )
    : m_totalSize( totalMemorySize )
    , m_blockSize( blockSize )
    , m_numBlocks( m_totalSize/m_blockSize )
    , m_pBitsets( nullptr )
    , m_pBitsetsEnd( nullptr )
    , m_pData( nullptr ) {

      dxvk_assert( totalMemorySize % blockSize == 0 );
      m_initFlag.store( InitState::not_initialized );
      m_pCurBitset.store( nullptr );

    }


    ~BitsetArrayFreeList() {

      delete[] m_pData;
      delete[] m_pBitsets;

    }

    struct AllocInfo {

      AllocInfo() {};

      std::atomic<bitset_t>* pBitset;
      uint8_t leadingZeros;

    };


    bool isInit() {
      return m_initFlag.load() == InitState::initialized;
    }

    bool tryInitialize() {

      uint8_t not_initialized = InitState::not_initialized;
      uint8_t initializing    = InitState::initializing;
      if( m_initFlag.compare_exchange_strong(not_initialized, initializing) ) {

        initialize();
        m_initFlag.compare_exchange_strong(initializing, InitState::initialized);
        return true;

      }
      return false;

    }

    std::byte* alloc( AllocInfo& allocInfo ) {

      std::atomic<bitset_t>*& pBitset = allocInfo.pBitset;

      for(;;) {

        bitset_t bitsetValue;
        for(;;) {

          std::atomic<bitset_t>* pCurBitset = m_pCurBitset.load( std::memory_order_relaxed );
          pBitset = pCurBitset;

          // traverse forwards from pCurBitset towards the end

          while( !(bitsetValue = pBitset->load()) ) {
            ++pBitset;
          }

          if( pBitset != m_pBitsetsEnd ) {
            dxvk_assert( bitsetValue != 0 );
            break;
          }

          // traverse forwards from start towards pCurBitset

          pBitset = m_pBitsets;
          while( (pBitset != pCurBitset) && !(bitsetValue = pBitset->load()) ) {
            ++pBitset;
          }

          if( pBitset != pCurBitset ) {
            dxvk_assert( bitsetValue != 0 );
            break;
          }

          // we traversed all elements, no free storage found
          return nullptr;
        }

        if( allocBit( allocInfo, bitsetValue ) )
          break;

      }

      m_pCurBitset.store( pBitset, std::memory_order_relaxed );

      uint64_t blockIndex = (pBitset - m_pBitsets) * platform_bits;
      blockIndex += allocInfo.leadingZeros;

      dxvk_assert( blockIndex < m_numBlocks );

      return &m_pData[blockIndex * m_blockSize];

    }

    void free( const AllocInfo& allocInfo ) {

      dxvk_assert( isInit() );
      dxvk_assert( ((*allocInfo.pBitset) & (one << (platform_bits_minus_one - allocInfo.leadingZeros))) == 0 && "double-free?!" );
      allocInfo.pBitset->fetch_or( one << (platform_bits_minus_one - allocInfo.leadingZeros) );

    }

    void free( std::byte* pBlock ) { // in case we only have the data pointer

      uint32_t blockIndex  = (pBlock - m_pData) / m_blockSize;
      uint32_t bitsetIndex = (pBlock - m_pData) / (m_blockSize * 64);

      dxvk_assert( pBlock >= m_pData );
      dxvk_assert( pBlock < m_pData + m_numBlocks * (uint64_t) m_blockSize );
      dxvk_assert( (pBlock - m_pData) % (m_blockSize * 64) == 0 );

      freeBit( blockIndex, bitsetIndex );

    }

    alignas( CACHE_LINE_SIZE )
    const uint64_t m_totalSize;
    const uint32_t m_blockSize;
    const uint32_t m_numBlocks;


  private:

    bool allocBit( AllocInfo& allocInfo, bitset_t bitsetValue ) {

      std::atomic<bitset_t>*& pBitset = allocInfo.pBitset;
      uint8_t& leadingZeros = allocInfo.leadingZeros;

      for(;;) {

        leadingZeros = getLeadingZeros(bitsetValue);

        bitset_t fetch_result = pBitset->fetch_and( ~(one << (platform_bits_minus_one - leadingZeros)) );
        if( ((fetch_result & (one << (platform_bits_minus_one - leadingZeros))) == 0) ) { // another thread got it
          bitsetValue = pBitset->load();
          if( bitsetValue == 0 ) { // bitset exhausted, we need another one to allocate from
            return false;
          }
          continue;
        }
        return true;

      }

    }

    void freeBit( uint32_t blockIndex, uint32_t bitsetIndex ) {

      dxvk_assert( (m_pBitsets[ bitsetIndex ] & ( one << (platform_bits_minus_one - (blockIndex & platform_bits_minus_one)) )) == 0 && "double-free?!" );
      m_pBitsets[ bitsetIndex ] |= ( one << (platform_bits_minus_one - (blockIndex & platform_bits_minus_one)) );

    }

    void no_inline initialize() {

      constexpr uint64_t largePageSize =   2 << 20; // 2 MiB
      constexpr uint64_t cacheSize     = 256 << 10; // 256 KiB
      const uint64_t align = m_totalSize >= largePageSize ? largePageSize : cacheSize;
      m_pData = new (std::align_val_t(align)) std::byte[ m_totalSize ];

      uint32_t numBitsets = m_numBlocks / platform_bits;
      if( m_numBlocks % platform_bits != 0 )
        numBitsets++;

      m_pBitsets = new (std::align_val_t(platform_bits))
        std::atomic<bitset_t>[ numBitsets+1 ];

      m_pCurBitset.store( m_pBitsets );
      for( size_t i=0; i<numBitsets; ++i ) {
        m_pBitsets[i].store( all_bits_set );
      }

      // set a one here, so we don't iterate over the boundary

      m_pBitsets[ numBitsets ] = 1;
      m_pBitsetsEnd = &m_pBitsets[ numBitsets ];

      // setting up the last bitset if needed

      if( m_numBlocks % platform_bits != 0 ) {

        std::atomic<bitset_t>& block = m_pBitsets[ numBitsets-1 ];
        uint32_t numBitsToSet = numBitsets % platform_bits;

        block = 0;
        uint8_t bitPosition = platform_bits_minus_one;
        for( size_t i=0; i<numBitsToSet; ++i ) {
          block |= (one << bitPosition);
          --bitPosition;
        }

      }

    }

    enum InitState {

      initialized     = 0,
      initializing    = 1,
      not_initialized = 2

    };

    std::atomic<uint8_t> m_initFlag = { InitState::not_initialized };

    std::atomic< std::atomic<bitset_t>* >   m_pCurBitset;       // pointer to the most recent allocation location
    std::atomic<bitset_t>*                  m_pBitsets;         // points to the start of the bitset array
    std::atomic<bitset_t>*                  m_pBitsetsEnd;      // points to the bitset one after the last one
    std::byte*                              m_pData;            // payload data to allocate and free

  };

  // make sure the structure fits into one cache line
  static_assert( sizeof(BitsetArrayFreeList) == 64 );

}
