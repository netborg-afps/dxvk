#include "dxvk_device.h"
#include "dxvk_queue.h"
#include <boost/lockfree/queue.hpp>

namespace dxvk {

  class DxvkSubmitEntryPool {
  public:
    DxvkSubmitEntryPool( int numEntries );
    ~DxvkSubmitEntryPool();

    DxvkSubmitEntry* acquire();
    void release( DxvkSubmitEntry* entry );

  private:
    DxvkSubmissionQueue::lockfree_queue_t* m_queue;
    DxvkSubmitEntry* m_entries;
  };


  DxvkSubmitEntryPool::DxvkSubmitEntryPool(int numEntries) {
    assert( numEntries <= 32 );
    m_queue = new DxvkSubmissionQueue::lockfree_queue_t();

    m_entries = new DxvkSubmitEntry[numEntries]();
    for( int i=0; i<numEntries; ++i ) {
      m_queue->push( &m_entries[i] );
    }
  }


  DxvkSubmitEntryPool::~DxvkSubmitEntryPool() {
    delete m_queue;
    delete m_entries;
  }


  DxvkSubmitEntry* DxvkSubmitEntryPool::acquire() {
    DxvkSubmitEntry* res;

    // TODO: add atomic sync
    while( !m_queue->pop(res) ) {}

    *res = {};
    return res;
  }


  void DxvkSubmitEntryPool::release(DxvkSubmitEntry* entry) {
    while( !m_queue->push(entry) ) {}
  }


  
  DxvkSubmissionQueue::DxvkSubmissionQueue(DxvkDevice* device, const DxvkQueueCallback& callback)
  : m_device(device), m_callback(callback),
    m_submitThread([this] () { submitCmdLists(); }),
    m_finishThread([this] () { finishCmdLists(); }) {

    m_submitEntryPool = new DxvkSubmitEntryPool( MaxNumQueuedCommandBuffers );
    m_lfFinishQueue   = new lockfree_queue_t;
    m_lfSubmitQueue   = new lockfree_queue_t;
  }
  
  
  DxvkSubmissionQueue::~DxvkSubmissionQueue() {
    auto vk = m_device->vkd();

    m_stopped.store(true);
    m_finishSyncIsFilled.signal_one();
    m_finishSyncIsEmpty.signal_one();
    m_submitSyncIsEmpty.signal_all();
    m_appendSync.signal_one();
    m_submitSync.signal_one();
    m_finishSync.signal_all();

    m_submitThread.join();
    m_finishThread.join();

    delete m_lfFinishQueue;
    delete m_lfSubmitQueue;
    delete m_submitEntryPool;
  }
  
  
  void DxvkSubmissionQueue::submit(DxvkSubmitInfo submitInfo, DxvkSubmitStatus* status) {
    DxvkSubmitEntry* pEntry = m_submitEntryPool->acquire();
    pEntry->status = status;
    pEntry->submit = std::move(submitInfo);

    m_lfSubmitQueue->push( pEntry );
    m_submitSyncIsEmpty.clear();
    m_appendSync.signal_one();
  }


  void DxvkSubmissionQueue::present(DxvkPresentInfo presentInfo, DxvkSubmitStatus* status) {
    DxvkSubmitEntry* pEntry= m_submitEntryPool->acquire();
    pEntry->status = status;
    pEntry->present = std::move(presentInfo);

    m_lfSubmitQueue->push( pEntry );
    m_submitSyncIsEmpty.clear();
    m_appendSync.signal_one();
  }


  void DxvkSubmissionQueue::synchronizeSubmission(
          DxvkSubmitStatus*   status) {
    while (status->result.load() == VK_NOT_READY) { // TODO: add !m_stopped.load() here?
      m_submitSync.wait();
    }
  }


  void DxvkSubmissionQueue::synchronize() {
    while (!m_stopped.load() && !m_lfSubmitQueue->empty()) {
      m_submitSyncIsEmpty.wait();
    }
  }


  void DxvkSubmissionQueue::waitForIdle() {
    while (!m_stopped.load() && !m_lfSubmitQueue->empty()) {
      m_submitSyncIsEmpty.wait();
    }

    while (!m_stopped.load() && !m_lfFinishQueue->empty()) {
      m_finishSyncIsEmpty.wait();
    }
  }


  void DxvkSubmissionQueue::lockDeviceQueue() {
    m_mutexQueue.lock();

    if (m_callback)
      m_callback(true);
  }


  void DxvkSubmissionQueue::unlockDeviceQueue() {
    if (m_callback)
      m_callback(false);

    m_mutexQueue.unlock();
  }


  void DxvkSubmissionQueue::submitCmdLists() {
    env::setThreadName("dxvk-submit");

    while (!m_stopped.load()) {
      DxvkSubmitEntry* pEntry = nullptr;
      while (!m_stopped.load() && !m_lfSubmitQueue->pop(pEntry)) {
        m_appendSync.wait();
      }
      
      if (m_stopped.load())
        return;

      DxvkSubmitEntry& entry = *pEntry;

      // Submit command buffer to device
      if (m_lastError != VK_ERROR_DEVICE_LOST) {
        std::lock_guard<dxvk::mutex> lock(m_mutexQueue);

        if (m_callback)
          m_callback(true);

        if (entry.submit.cmdList != nullptr)
          entry.result = entry.submit.cmdList->submit();
        else if (entry.present.presenter != nullptr)
          entry.result = entry.present.presenter->presentImage(entry.present.presentMode, entry.present.frameId);

        if (m_callback)
          m_callback(false);
      } else {
        // Don't submit anything after device loss
        // so that drivers get a chance to recover
        entry.result = VK_ERROR_DEVICE_LOST;
      }

      if (entry.status)
        entry.status->result = entry.result;

      bool doForward = (entry.result == VK_SUCCESS) ||
        (entry.present.presenter != nullptr && entry.result != VK_ERROR_DEVICE_LOST);

      if (doForward) {
        m_lfFinishQueue->push( pEntry );
        m_finishSyncIsEmpty.clear();
        m_finishSyncIsFilled.signal_one();

      } else {
        Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", entry.result));
        m_lastError = entry.result;

        if (m_lastError != VK_ERROR_DEVICE_LOST)
          m_device->waitForIdle();
      }

      if( m_lfSubmitQueue->empty() ) {
        m_submitSyncIsEmpty.signal_all();
      }

      m_submitSync.signal_one();
    }
  }
  
  
  void DxvkSubmissionQueue::finishCmdLists() {
    env::setThreadName("dxvk-queue");

    while (!m_stopped.load()) {

      DxvkSubmitEntry* pEntry = nullptr;
      while (!m_stopped.load() && !m_lfFinishQueue->pop(pEntry)) {
        auto t0 = dxvk::high_resolution_clock::now();
        m_finishSyncIsFilled.wait();
        auto t1 = dxvk::high_resolution_clock::now();
        m_gpuIdle += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      }

      if (m_stopped.load())
        return;

      DxvkSubmitEntry& entry = *pEntry;
      
      if (entry.submit.cmdList != nullptr) {
        VkResult status = m_lastError.load();
        
        if (status != VK_ERROR_DEVICE_LOST)
          status = entry.submit.cmdList->synchronizeFence();
        
        if (status != VK_SUCCESS) {
          m_lastError = status;

          if (status != VK_ERROR_DEVICE_LOST)
            m_device->waitForIdle();
        }
      } else if (entry.present.presenter != nullptr) {
        // Signal the frame and then immediately destroy the reference.
        // This is necessary since the front-end may want to explicitly
        // destroy the presenter object. 
        entry.present.presenter->signalFrame(entry.result,
          entry.present.presentMode, entry.present.frameId);
        entry.present.presenter = nullptr;
      }

      // Release resources and signal events, then immediately wake
      // up any thread that's currently waiting on a resource in
      // order to reduce delays as much as possible.
      if (entry.submit.cmdList != nullptr)
        entry.submit.cmdList->notifyObjects();

      if( m_lfFinishQueue->empty() ) {
          m_finishSyncIsEmpty.signal_one();
      }

      m_finishSync.signal_all();

      // Free the command list and associated objects now
      if (entry.submit.cmdList != nullptr) {
        entry.submit.cmdList->reset();
        m_device->recycleCommandList(entry.submit.cmdList);
      }

      m_submitEntryPool->release(pEntry);
    }
  }
  
}
