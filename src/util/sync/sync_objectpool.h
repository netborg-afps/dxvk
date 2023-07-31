#pragma once

#include <stdint.h>
#include <array>

#include "sync_spinlock.h"

namespace dxvk::sync {

  /**
   * \brief Generic spin function
   *
   * Performs low-power spinning in a loop without
   * giving up thread scheduling.
   * \param [in] spinCount Number of loop-iterations until return
   */
  inline void spin(uint32_t spinCount) {
    for (uint32_t i = 0; i < spinCount; i++) {
      #if defined(DXVK_ARCH_X86)
      _mm_pause();
      #elif defined(DXVK_ARCH_ARM64)
      __asm__ __volatile__ ("yield");
      #else
      #error "Pause/Yield not implemented for this architecture."
      #endif
    }
  }

  /**
   * \brief Object pool with lockfree properties
   *
   * Objects are stored redundantly such they can be accessed
   * via spinlock-like functionality.
   * Is guaranteed to be lockfree if the number of simultaneous
   * threads accessing the pool is at most equal to \c size.
   * If more threads are accessing the pool, there is still a
   * great chance for quick lock acquiring.
   *
   * This pool potentially trades memory for speed and can
   * only be used if it doesn't matter which sub-pool is accessed.
   */
  template<typename T, uint16_t size>
  class ObjectPool {

  public:

    ObjectPool()  { }
    ~ObjectPool() { }

    ObjectPool             ( const ObjectPool& ) = delete;
    ObjectPool& operator = ( const ObjectPool& ) = delete;

    struct Object {
      Spinlock  spinMutex;
      T         data;
    };

    Object& getObjectLocked() {
      while( true ) {
        for (size_t i = 0; i < m_objects.size(); i++ ) {
          if( m_objects[i].spinMutex.try_lock() )
            return m_objects[i];
        }
        spin(200);
      }
    }

    T& getDataUnsafe( uint16_t index ) {
      return m_objects[index].data;
    }

    uint16_t getSize() { return size; }

  private:

    std::array<Object, size> m_objects;

  };

}