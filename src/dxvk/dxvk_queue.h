#pragma once

#include <mutex>
#include <queue>
#include <boost/lockfree/lockfree_forward.hpp>

#include "../util/sync/sync_atomic_signal.h"
#include "../util/thread.h"

#include "dxvk_cmdlist.h"
#include "dxvk_presenter.h"

namespace dxvk {
  
  class DxvkDevice;
  class DxvkSubmitEntryPool;

  /**
   * \brief Submission status
   * 
   * Stores the result of a queue
   * submission or a present call.
   */
  struct DxvkSubmitStatus {
    std::atomic<VkResult> result = { VK_SUCCESS };
  };


  /**
   * \brief Queue submission info
   * 
   * Stores parameters used to submit
   * a command buffer to the device.
   */
  struct DxvkSubmitInfo {
    Rc<DxvkCommandList> cmdList;
  };
  
  
  /**
   * \brief Present info
   *
   * Stores parameters used to present
   * a swap chain image on the device.
   */
  struct DxvkPresentInfo {
    Rc<Presenter>       presenter;
    VkPresentModeKHR    presentMode;
    uint64_t            frameId;
  };


  /**
   * \brief Submission queue entry
   */
  struct DxvkSubmitEntry {
    VkResult            result;
    DxvkSubmitStatus*   status;
    DxvkSubmitInfo      submit;
    DxvkPresentInfo     present;
  };


  /**
   * \brief Submission queue
   */
  class DxvkSubmissionQueue {

  public:
    
    DxvkSubmissionQueue(
            DxvkDevice*         device,
      const DxvkQueueCallback&  callback);

    ~DxvkSubmissionQueue();

    /**
     * \brief Retrieves estimated GPU idle time
     *
     * This is a monotonically increasing counter
     * which can be evaluated periodically in order
     * to calculate the GPU load.
     * \returns Accumulated GPU idle time, in us
     */
    uint64_t gpuIdleTicks() const {
      return m_gpuIdle.load();
    }

    /**
     * \brief Retrieves last submission error
     * 
     * In case an error occured during asynchronous command
     * submission, it will be returned by this function.
     * \returns Last error from command submission
     */
    VkResult getLastError() const {
      return m_lastError.load();
    }
    
    /**
     * \brief Submits a command list asynchronously
     * 
     * Queues a command list for submission on the
     * dedicated submission thread. Use this to take
     * the submission overhead off the calling thread.
     * \param [in] submitInfo Submission parameters
     * \param [out] status Submission feedback
     */
    void submit(
            DxvkSubmitInfo      submitInfo,
            DxvkSubmitStatus*   status);
    
    /**
     * \brief Presents an image synchronously
     *
     * Waits for queued command lists to be submitted
     * and then presents the current swap chain image
     * of the presenter. May stall the calling thread.
     * \param [in] present Present parameters
     * \param [out] status Submission feedback
     */
    void present(
            DxvkPresentInfo     presentInfo,
            DxvkSubmitStatus*   status);
    
    /**
     * \brief Synchronizes with one queue submission
     * 
     * Waits for the result of the given submission
     * or present operation to become available.
     * \param [in,out] status Submission status
     */
    void synchronizeSubmission(
            DxvkSubmitStatus*   status);
    
    /**
     * \brief Synchronizes with queue submissions
     * 
     * Waits for all pending command lists to be
     * submitted to the GPU before returning.
     */
    void synchronize();

    /**
     * \brief Synchronizes until a given condition becomes true
     *
     * Useful to wait for the GPU without busy-waiting.
     * \param [in] pred Predicate to check
     */
    template<typename Pred>
    void synchronizeUntil(const Pred& pred) {
      while( !m_stopped && !pred() ) {
        m_finishSync.wait();
      }
    }

    /**
     * \brief Waits for all submissions to complete
     */
    void waitForIdle();

    /**
     * \brief Locks device queue
     *
     * Locks the mutex that protects the Vulkan queue
     * that DXVK uses for command buffer submission.
     * This is needed when the app submits its own
     * command buffers to the queue.
     */
    void lockDeviceQueue();

    /**
     * \brief Unlocks device queue
     *
     * Unlocks the mutex that protects the Vulkan
     * queue used for command buffer submission.
     */
    void unlockDeviceQueue();

    typedef boost::lockfree::queue<DxvkSubmitEntry*, boost::lockfree::capacity<32>, boost::lockfree::fixed_sized<true> > lockfree_queue_t;

  private:

    DxvkDevice*                 m_device;
    DxvkQueueCallback           m_callback;

    std::atomic<VkResult>       m_lastError = { VK_SUCCESS };
    
    std::atomic<bool>           m_stopped = { false };
    std::atomic<uint64_t>       m_gpuIdle = { 0ull };

    dxvk::mutex                 m_mutexQueue;

    dxvk::sync::AtomicSignal    m_finishSync          = { "finish_sync", false };
    dxvk::sync::AtomicSignal    m_finishSyncIsFilled  = { "finish_sync_is_filled", false };
    dxvk::sync::AtomicSignal    m_finishSyncIsEmpty   = { "finish_sync_is_empty", true };
    dxvk::sync::AtomicSignal    m_submitSyncIsEmpty   = { "submit_sync_is_empty", true };
    dxvk::sync::AtomicSignal    m_submitSync          = { "submit_sync", false };
    dxvk::sync::AtomicSignal    m_appendSync          = { "append_sync", false };

    alignas(64)
    lockfree_queue_t*           m_lfFinishQueue;
    lockfree_queue_t*           m_lfSubmitQueue;

    DxvkSubmitEntryPool*        m_submitEntryPool;

    dxvk::thread                m_submitThread;
    dxvk::thread                m_finishThread;

    void submitCmdLists();

    void finishCmdLists();
    
  };
  
}
