#pragma once

#include "dxvk_framepacer_mode.h"
#include "dxvk_latency_markers.h"
#include "../../util/util_time.h"
#include <dxgi.h>


namespace dxvk {

  struct DxvkOptions;

  /* \brief Frame pacer interface managing the CPU - GPU synchronization.
   *
   * GPUs render frames asynchronously to the game's and dxvk's CPU-side work
   * in order to improve fps-throughput. Aligning the cpu work to chosen time-
   * points allows to tune certain characteristics of the video presentation,
   * like smoothness and latency.
   *
   * Note that the dxvk-side frameId is incremented in
   * D3D11/9SwapChain::SubmitPresent(). The pacer interprets present(frameId)
   * and preceding submits to be grouped to the same frameId.
   */

  class FramePacer {
    using microseconds = std::chrono::microseconds;
  public:

    FramePacer( const DxvkOptions& options );
    ~FramePacer();

    void startFrame( uint64_t frameId ) {
      // wait for finished rendering of a previous frame, typically the one before last
      m_mode->wait(frameId);
      // potentially wait some more if the cpu gets too much ahead
      m_mode->startFrame(frameId);
      m_latencyMarkersStorage.registerFrameStart(frameId);
    }

    void endFrame( uint64_t frameId ) {
      // the frame has been displayed to the screen
      m_latencyMarkersStorage.registerFrameEnd(frameId);
      m_mode->endFrame(frameId);
    }

    void onSubmitCmdList() {
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(m_lastSubmitFrameId+1);
      m->gpuSubmit.push_back(high_resolution_clock::now());
    }

    // dx to vk translation is finished
    void onSubmitPresent( uint64_t frameId ) {
      auto now = high_resolution_clock::now();
      m_lastSubmitFrameId = frameId;
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
      m->cpuFinished = std::chrono::duration_cast<microseconds>(now - m->start).count();
      m_latencyMarkersStorage.m_timeline.cpuFinished.store(frameId);

      LatencyMarkers* next = m_latencyMarkersStorage.getMarkers(frameId+1);
      next->gpuSubmit.clear();
    }

    void onFinishedQueueCmdList() {
      auto now = high_resolution_clock::now();
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(m_lastFinishedFrameId+1);
      m->gpuRun.push_back(now);

      if (m->gpuRun.size() == 1) {
        m->gpuStart = std::chrono::duration_cast<microseconds>(now - m->start).count();
        m_latencyMarkersStorage.m_timeline.gpuStart.store(m_lastFinishedFrameId+1);
        m_mode->signalGpuStart(m_lastFinishedFrameId+1);
      }
    }

    void onFinishedGpuActivity() {
      auto now = high_resolution_clock::now();
      LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(m_lastFinishedFrameId+1);
      m->gpuReady.push_back(now);
      m->gpuLastActive = now;
    }

    void onFinishedQueuePresent( uint64_t frameId ) {
      // we get frameId == 0 for repeated presents (SyncInterval)
      if (frameId != 0) {
        m_lastFinishedFrameId = frameId;

        LatencyMarkers* m = m_latencyMarkersStorage.getMarkers(frameId);
        LatencyMarkers* next = m_latencyMarkersStorage.getMarkers(frameId+1);
        m->gpuFinished = std::chrono::duration_cast<microseconds>(m->gpuLastActive - m->start).count();
        next->gpuRun.clear();
        next->gpuReady.clear();
        next->gpuReady.push_back(m->gpuLastActive);

        if (m->gpuRun.empty()) {
          m->gpuStart = 0;
          m->gpuFinished = 0;
          m_latencyMarkersStorage.m_timeline.gpuStart.store(frameId);
          m_mode->signalGpuStart(frameId);
        }

        m_latencyMarkersStorage.m_timeline.gpuFinished.store(frameId);
        m_mode->finishRender(frameId);
        m_mode->signal(frameId);
      }
    }

    FramePacerMode::Mode getMode() const {
      return m_mode->m_mode;
    }

    void setTargetFrameRate( double frameRate ) {
      m_mode->setTargetFrameRate(frameRate);
    }

    LatencyMarkersStorage m_latencyMarkersStorage;

  private:

    std::unique_ptr<FramePacerMode> m_mode;

    uint64_t m_lastSubmitFrameId   = { DXGI_MAX_SWAP_CHAIN_BUFFERS };
    uint64_t m_lastFinishedFrameId = { DXGI_MAX_SWAP_CHAIN_BUFFERS };

  };

}
