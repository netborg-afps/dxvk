#pragma once

#include <atomic>

#include "sync_allocator.h"

#include "../util_assert.h"
#include "../util_platform.h"


namespace dxvk::sync {

  template< typename T, size_t size >
  class BitsetStorage;


  /**
   * \brief Bitset Storage Node
   *
   * A storage for objects where access order is not important.
   * The storage is thread-safe and lockfree (other than the allocator
   * calling malloc a few times per application lifetime).
   *
   * One node stores #platform_bits elements.
   *
   * These nodes are chained as a list in BitsetStorage, and it must
   * be noted that one reason for the great performance of this structure
   * seems to be that data locality for a typical series of pushes
   * and pops is excellent. In the best case, all data access is happening
   * at the root node, and possibly at the first indices within the array,
   * which means most data accesses will be in CPU cache, - which is in
   * contrast to storing data as a stack (vector, deque, etc) where data
   * access location always changes with the size of the structure.
   */
  template< typename T, size_t size >
  class BitsetStorageNode {

    // could support other sizes as well, but this way
    // we don't need to check for the leading zeros result
    static_assert( size == platform_bits );
    static constexpr uint8_t num_elements = size;

    friend class BitsetStorage<T, size>;

  public:

    BitsetStorageNode() {

      m_freeBitset.store( all_bits_set );
      m_lockBitset.store( all_bits_set );
      m_pNext.store( nullptr );
      m_pFree = nullptr;

    }

    ~BitsetStorageNode() = default;

    bool tryPush( const T& element ) {

      bitset_t targetBit;
      uint8_t leadingZeros;

      for(;;) {
        bitset_t bitset = m_freeBitset.load() & m_lockBitset.load();

        if( bitset == 0 )
          return false;

        leadingZeros = getLeadingZeros( bitset );

        targetBit = one << ( platform_bits_minus_one - leadingZeros );
        if( tryLock(targetBit) )
          break;
      }

      setStored( targetBit );
      m_data[ leadingZeros ] = element;
      unlock( targetBit );

      return true;

    }

    bool tryPop( T& element ) {

      bitset_t targetBit;
      uint8_t leadingOnes;

      for(;;) {
        bitset_t bitset = ~m_freeBitset.load() & m_lockBitset.load();

        if( bitset == 0 )
          return false;

        leadingOnes = getLeadingZeros( bitset );

        targetBit = one << (platform_bits_minus_one - leadingOnes);
        if( tryLock(targetBit) )
          break;
      }

      setFree( targetBit );
      element = m_data[leadingOnes];
      unlock( targetBit );

      return true;

    }

  private:

    bool tryLock( bitset_t targetBit ) {

      bitset_t fetchResult = m_lockBitset.fetch_and( ~targetBit );
      if( (fetchResult & targetBit) != 0 ) {
        return true;
      }
      return false;

    }

    void unlock( bitset_t targetBit ) {
      m_lockBitset.fetch_or( targetBit );
    }

    void setFree( bitset_t targetBit ) {
      m_freeBitset.fetch_or( targetBit );
    }

    void setStored( bitset_t targetBit ) {
      m_freeBitset.fetch_and( ~targetBit );
    }

    std::atomic<bitset_t> m_freeBitset;
    std::atomic<bitset_t> m_lockBitset;
    std::atomic<BitsetStorageNode*> m_pNext;
    BitsetArrayFreeList* m_pFree;

    alignas( sizeof(T) )
    T m_data[num_elements];

    BitsetArrayFreeList::AllocInfo m_allocInfo;

  };




  template< typename T, size_t size >
  class BitsetStorage {

  public:

    BitsetStorage() { }

    ~BitsetStorage() {

      Node* pNode = m_rootNode.m_pNext;
      while( pNode != nullptr ) {
        Node* pDeleteNode = pNode;
        pNode = pDeleteNode->m_pNext;
        pDeleteNode->m_pFree->free( pDeleteNode->m_allocInfo );
      }

    }

    void push( const T& element ) {

      for(;;) {
        Node* pNode = &m_rootNode;
        for(;;) {
          if( pNode->tryPush( element ) ) {
            return;
          }

          Node* pNext = pNode->m_pNext.load();
          if( pNext == nullptr )
            break;
          pNode = pNext;
        }

        expand();

      }
    }

    bool tryPop( T& element ) {

      Node* pNode = &m_rootNode;
      while( pNode != nullptr ) {
        if( pNode->tryPop( element ) ) {
          return true;
        }
        pNode = pNode->m_pNext.load();
      }

      return false;

    }

  private:

    typedef BitsetStorageNode<T, size> Node;

    // put the new node between the root node and the next node
    // to prevent repeated pushes to become a performance issue.
    // todo: move empty nodes aside to prevent repeated popping
    //  after repeated pushes to become an issue (always traversing whole list).
    // note: this is the only practical way to handle these situations
    //  efficiently since adding things like bookkeeping is very quickly
    //  increasing execution times dramatically.

    bool expand() {

      Node* expected = m_rootNode.m_pNext.load();
      if( expected && expected->m_freeBitset.load() != 0 )
        return false;

      Allocator::AllocData allocData = g_alloc.alloc();
      Node* newStorageNode = reinterpret_cast<Node*>( allocData.data );

      new( newStorageNode ) Node;
      newStorageNode->m_pNext = expected;

      if( m_rootNode.m_pNext.compare_exchange_strong( expected, newStorageNode )) {
        newStorageNode->m_pFree = allocData.freePtr;
        newStorageNode->m_allocInfo = allocData.allocInfo;
        return true;
      }

      allocData.freePtr->free( allocData.allocInfo );
      return false;

    }

    Node m_rootNode;

  };


}

