#pragma once

#include <stdint.h>
#include <atomic>

#include "sync_bitset_array_freelist.h"

#include "../util_likely.h"
#include "../util_error.h"
#include "../util_assert.h"


namespace dxvk::sync {

  /**
   * \brief Allocator
   *
   * Provides thread-safe allocation and freeing of memory via BitsetArrayFreeList.
   * It's generally lockfree with the exception when the internal memory pool is expanded.
   */
  class Allocator {

  public:

    Allocator( uint32_t blockSize )
    : m_block_memory_size( blockSize ) {

      // tested only with block sizes multiple of 32 bytes,
      // so double-check if using it for other sizes
      dxvk_assert( m_block_memory_size % 32 == 0 );

      m_pBitsetArraysMemory = new ( std::align_val_t(sizeof(BitsetArrayFreeList)) )
        std::byte[ m_num_bitset_arrays*sizeof(BitsetArrayFreeList) ]();

      m_pBitsetArrays = reinterpret_cast<BitsetArrayFreeList*>( m_pBitsetArraysMemory );

      // choose to start with the largest memory size fitting into 256 KiB

      uint64_t arrayMemorySize = blockSize;
      while( arrayMemorySize * 2 < 256 << 10 ) {
        arrayMemorySize *= 2;
      }

      // setting up m_pBitsetArrays

      for( uint16_t i=0; i<m_num_bitset_arrays; ++i ) {
        new (&m_pBitsetArrays[i]) BitsetArrayFreeList( arrayMemorySize, m_block_memory_size );

        arrayMemorySize *= 2;
        dxvk_assert( !m_pBitsetArrays[i].isInit() );
      }

      while( !m_pBitsetArrays[0].tryInitialize() ) {
        dxvk_assert( false && "BitsetArrayFreeList::tryInitialize() failed" );
      }

      m_pCurBitset.store(&m_pBitsetArrays[0]);

    }

    ~Allocator() {

      for( uint16_t i=0; i<m_num_bitset_arrays; ++i ) {
        m_pBitsetArrays[i].~BitsetArrayFreeList();
      }

      // don't delete m_pBitsetArrays as it's just a reinterpreted pointer!
      delete[] m_pBitsetArraysMemory;

    }

    struct AllocData {

      AllocData() = delete;
      AllocData( std::byte* d, BitsetArrayFreeList* ptr, BitsetArrayFreeList::AllocInfo& _allocInfo )
      : data(d), freePtr(ptr), allocInfo(_allocInfo) {}

      std::byte*  data;
      BitsetArrayFreeList* freePtr;
      BitsetArrayFreeList::AllocInfo allocInfo;

    };

    AllocData alloc() {

      for(;;) {

        std::byte* res = nullptr;
        BitsetArrayFreeList* pCurBitset = m_pCurBitset.load( std::memory_order_relaxed );
        BitsetArrayFreeList* pBitset = pCurBitset;
        dxvk_assert( pCurBitset >= m_pBitsetArrays && pCurBitset < &m_pBitsetArrays[m_num_bitset_arrays] );

        BitsetArrayFreeList::AllocInfo allocInfo;

        // traverse forwards from pCurBitset towards the end

        while( pBitset->isInit() && ( !(res = pBitset->alloc(allocInfo))) ) {
          ++pBitset;
        }

        if( likely(res != nullptr) ) {
          m_pCurBitset.store( pBitset, std::memory_order_relaxed );
          return AllocData(res, pBitset, allocInfo);
        }

        // traverse backwards from pCurBitset towards the start

        BitsetArrayFreeList* pBitsetsEnd = pBitset;
        pBitset = pCurBitset;
        while( pBitset != m_pBitsetArrays && ( !(res = pBitset->alloc(allocInfo))) ) {
          --pBitset;
        }

        if( likely(res != nullptr) ) {
          m_pCurBitset.store( pBitset, std::memory_order_relaxed );
          return AllocData(res, pBitset, allocInfo);
        }

        dxvk_assert( pBitset == m_pBitsetArrays );

        // if we are arriving here, we need to expand the bitsets in a thread-safe way
        // one thread grabs the "lock" and constructs a new bitset array
        // other treads may try to look again if they can alloc from the previous bitset arrays

        expandAt( pBitsetsEnd );

      }

    }

    void no_inline expandAt( BitsetArrayFreeList* pBitset ) {

      dxvk_assert( (pBitset - m_pBitsetArrays) / sizeof(BitsetArrayFreeList) < m_num_bitset_arrays );
      if( pBitset->tryInitialize() ) {
          dxvk::Logger::debug(
            dxvk::str::format("creating a new free bitset array with memory size ",
            pBitset->m_totalSize ) );
      }

    }

    const size_t m_block_memory_size;

  private:

    // have so many bitset arrays that due to exponential growth
    // the last one will never be touched. When growing by a factor of 2
    // and starting with 128 KiB with m_num_bitset_arrays = 24,
    // the storage could hold 2 terabytes of memory.
    static constexpr uint16_t m_num_bitset_arrays = 24;

    alignas( CACHE_LINE_SIZE )
    std::byte*              m_pBitsetArraysMemory;  // those two are pointing to
    BitsetArrayFreeList*    m_pBitsetArrays;        // the same memory location

    std::atomic<BitsetArrayFreeList*> m_pCurBitset;

  };


  extern Allocator g_alloc;

}



