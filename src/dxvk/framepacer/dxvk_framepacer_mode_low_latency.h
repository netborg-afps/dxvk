#pragma once

#include "dxvk_framepacer_mode.h"
#include "../dxvk_options.h"
#include "../../util/log/log.h"
#include "../../util/util_string.h"
#include <assert.h>

namespace dxvk {

  /*
   * This low-latency mode aims to reduce latency with minimal impact in fps.
   * Effective when operating in the GPU-limit. Efficient to be used in the CPU-limit as well.
   *
   * Greatly reduces input lag variations when switching between CPU- and GPU-limit, and
   * compared to the max-frame-latency approach, it has a much more stable input lag when
   * GPU running times change dramatically, which can happen for example when rotating within a scene.
   *
   * The current implementation rather generates fluctuations alternating frame-by-frame
   * depending on the game's and dxvk's CPU-time variations. This might be visible as a loss
   * in smoothness, which is an area this implementation can be further improved.
   *
   * An interesting observation while playtesting was that not only the input lag was affected,
   * but the video generated did progress more cleanly in time as well with regards to the
   * wow and flutter effect.
   *
   * Optimized for VRR and VK_PRESENT_MODE_IMMEDIATE_KHR. It also comes with its own fps-limiter
   * which is typically used to prevent the game's fps exceeding the monitor's refresh rate.
   *
   * Can be fine-tuned via the dxvk.lowLatencyOffset (or env) variable.
   * Compared to maxFrameLatency = 3, render-latency reductions of up to 67% are achieved.
   */

  class LowLatencyMode : public FramePacerMode {

  public:

    LowLatencyMode(Mode mode, LatencyMarkersStorage* storage, const DxvkOptions& options)
    : FramePacerMode(mode, storage)
    , m_lowLatencyOffset(getLowLatencyOffset(options)) {
      Logger::info( str::format("Using lowLatencyOffset: ", m_lowLatencyOffset) );
    }

    ~LowLatencyMode() {}

    void startFrame( uint64_t frameId ) override {

      using std::chrono::microseconds;
      using std::chrono::duration_cast;
      using time_point = high_resolution_clock::time_point;

      m_fenceGpuStart.wait( frameId-1 );
      time_point now = high_resolution_clock::now();

      // estimates the optimal overlap for cpu/gpu work via min( gpuReady - gpuSubmit )
      // note that gpuReady - gpuSubmit may be negative

      uint64_t id = m_latencyMarkersStorage->getTimeline()->gpuFinished.load();
      if (id <= DXGI_MAX_SWAP_CHAIN_BUFFERS+1ull)
        return;

      const LatencyMarkers* markers = m_latencyMarkersStorage->getConstMarkers(id);
      size_t bestIndex = 0;
      int32_t bestDiff = std::numeric_limits<int32_t>::max();
      size_t numLoop = std::min( std::min(markers->gpuReady.size(), markers->gpuSubmit.size()), markers->gpuRun.size() );
      if (numLoop == 0)
        return;

      for (size_t i=0; i<numLoop; ++i) {
        int32_t diff = duration_cast<microseconds>( markers->gpuReady[i] - markers->gpuSubmit[i] ).count();
        if (diff < bestDiff) {
          bestDiff = diff;
          bestIndex = i;
        }
      }

      // estimate the target gpu finishing time for this frame
      // and calculate backwards when we want to start this frame

      int32_t gpuTime = getGpuTimePrediction();
      const LatencyMarkers* markersPrev = m_latencyMarkersStorage->getConstMarkers(frameId-1);
      time_point targetGpuFinish = markersPrev->start + microseconds( markersPrev->gpuStart + 2*gpuTime );

      if (id == frameId-1)
        targetGpuFinish = markers->start + microseconds( markers->gpuFinished + gpuTime );

      time_point targetGpuSync2 = targetGpuFinish
        - microseconds( duration_cast<microseconds>( markers->gpuLastActive - markers->gpuRun[bestIndex] ).count() );

      int32_t targetGpuSync = duration_cast<microseconds>( targetGpuSync2 - now ).count();
      int32_t delay = targetGpuSync
        - duration_cast<microseconds>( markers->gpuSubmit[bestIndex] - markers->start ).count()
        + m_lowLatencyOffset;

      // account for the fps limit and ensure we won't sleep too long, just in case

      int32_t frametime = duration_cast<microseconds>( now - m_lastStart ).count();
      int32_t frametimeDiff = std::max( 0, m_fpsLimitFrametime.load() - frametime );
      delay = std::max( delay, frametimeDiff );
      delay = std::max( 0, std::min( delay, 20000 ) );

      Sleep::TimePoint nextStart = now + microseconds(delay);
      Sleep::sleepUntil( now, nextStart );

      m_lastStart = nextStart;

    }

    void finishRender( uint64_t frameId ) override { }

  private:

    int32_t getGpuTimePrediction() {
      // we smooth out gpu running times which are pretty steady to begin with.
      // we don't do that (yet?) for cpu running times, cause basing it on the
      // last frame only gave us the best results for now. However using proper
      // smoothing and outlier rejection is a promising way to further improve
      // this pacing method.
      constexpr int32_t numLoop = 7;
      int32_t i = numLoop;
      int32_t totalGpuTime = 0;
      uint64_t id = m_latencyMarkersStorage->getTimeline()->gpuFinished.load();
      if (id < DXGI_MAX_SWAP_CHAIN_BUFFERS+numLoop)
        return 0;

      while (i>0) {
        const LatencyMarkers* markers = m_latencyMarkersStorage->getConstMarkers(id);
        int32_t gpuTime = markers->gpuFinished - markers->gpuStart;
        totalGpuTime += gpuTime;
        --id;
        --i;
      }

      return totalGpuTime/numLoop;
    }

    bool getLowLatencyOffsetFromEnv( int32_t& offset ) {
      if (!getIntFromEnv("DXVK_LOW_LATENCY_OFFSET", &offset))
        return false;
      return offset;
    }

    int32_t getLowLatencyOffset( const DxvkOptions& options ) {
      int32_t offset = options.lowLatencyOffset;
      int32_t o;
      if (getLowLatencyOffsetFromEnv(o))
        offset = o;

      offset = std::max( -10000, offset );
      offset = std::min(  10000, offset );
      return offset;
    }

    const int32_t m_lowLatencyOffset;
    Sleep::TimePoint m_lastStart = { high_resolution_clock::now() };

  };

}
