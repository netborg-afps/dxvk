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
    using microseconds = std::chrono::microseconds;
    using time_point = high_resolution_clock::time_point;
  public:

    LowLatencyMode(Mode mode, LatencyMarkersStorage* storage, const DxvkOptions& options)
    : FramePacerMode(mode, storage)
    , m_lowLatencyOffset(getLowLatencyOffset(options)) {
      Logger::info( str::format("Using lowLatencyOffset: ", m_lowLatencyOffset) );
    }

    ~LowLatencyMode() {}


    void startFrame( uint64_t frameId ) override {

      using std::chrono::duration_cast;

      m_fenceGpuStart.wait( frameId-1 );

      // we wanted this in there (not sure if needed), but we got some
      // hard to reproduce spurious deadlocks, so disabled for now
      //   m_fenceCpuFinished.wait( frameId-1 );

      time_point now = high_resolution_clock::now();

      uint64_t finishedId = m_latencyMarkersStorage->getTimeline()->gpuFinished.load();
      if (finishedId <= DXGI_MAX_SWAP_CHAIN_BUFFERS+1ull)
        return;

      // we are the only in-flight frame, nothing to do other then to apply fps-limiter if needed

      if (finishedId == frameId-1) {
        m_lastStart = sleepFor( now, 0 );
        return;
      }

      assert(finishedId == frameId-2);
      const LatencyMarkers* m2 = m_latencyMarkersStorage->getConstMarkers(finishedId);
      const LatencyMarkers* m1 = m_latencyMarkersStorage->getConstMarkers(frameId-1);
      const SyncProps props = getPrediction();

      size_t numLoop = std::min( std::min(m2->gpuReady.size(), m2->gpuSubmit.size()), m2->gpuRun.size() );
      if (numLoop == 0 || m1->gpuRun.empty()) {
        m_lastStart = sleepFor( now, 0 );
        return;
      }

      // estimate the target gpu sync point for this frame
      // and calculate backwards when we want to start this frame

      int32_t gpuReadyPrediction = duration_cast<microseconds>(
        m1->start + microseconds(getGpuFinishedPrediction()) - now).count();

      int32_t targetGpuSync = gpuReadyPrediction
        + props.gpuSync;

      int32_t delay = targetGpuSync
        - props.cpuUntilGpuSync
        + m_lowLatencyOffset;

      m_lastStart = sleepFor( now, delay );

    }


    void finishRender( uint64_t frameId ) override {

      using std::chrono::duration_cast;
      const LatencyMarkers* m = m_latencyMarkersStorage->getConstMarkers(frameId);

      int32_t numLoop = std::min((int32_t)(m->gpuReady.size())-1, (int32_t)m->gpuRun.size() );
      assert( m->gpuRun.size() == m->gpuSubmit.size() );
      if (numLoop <= 0) {
        m_props[frameId % m_props.size()] = SyncProps();
        m_props[frameId % m_props.size()].isOutlier = true;
        m_propsFinished.store( frameId );
        return;
      }

      // estimates the optimal overlap for cpu/gpu work by optimizing gpu scheduling first
      // such that gpu doesn't go into idle for this frame, and then aligning cpu submits
      // where gpuSubmit[i] <= gpuRun[i] for all i

      std::vector<int32_t>& gpuRun = m_tempGpuRun;
      gpuRun.clear();
      int32_t optimizedGpuTime = 0;
      for (int i=0; i<numLoop; ++i) {
        gpuRun.push_back(optimizedGpuTime);
        optimizedGpuTime += duration_cast<microseconds>( m->gpuReady[i+1] - m->gpuRun[i] ).count();
      }

      int32_t offset = 0;
      for (int i=1; i<numLoop; ++i) {
        int32_t curSubmit = duration_cast<microseconds>( m->gpuSubmit[i] - m->gpuSubmit[0] ).count();
        int32_t diff = curSubmit - gpuRun[i];
        offset = std::max( offset, diff );
      }

      SyncProps& props = m_props[frameId % m_props.size()];
      props.gpuSync = 0;
      props.cpuUntilGpuSync = offset + duration_cast<microseconds>( m->gpuSubmit[0] - m->start ).count();
      props.optimizedGpuTime = optimizedGpuTime;
      props.isOutlier = isOutlier(frameId);

      m_propsFinished.store( frameId );

    }


    Sleep::TimePoint sleepFor( const Sleep::TimePoint t, int32_t delay ) {

      // account for the fps limit and ensure we won't sleep too long, just in case
      int32_t frametime = std::chrono::duration_cast<microseconds>( t - m_lastStart ).count();
      int32_t frametimeDiff = std::max( 0, m_fpsLimitFrametime.load() - frametime );
      delay = std::max( delay, frametimeDiff );
      delay = std::max( 0, std::min( delay, 20000 ) );

      Sleep::TimePoint nextStart = t + microseconds(delay);
      Sleep::sleepUntil( t, nextStart );
      return nextStart;

    }


  private:

    struct SyncProps {

      int32_t optimizedGpuTime;   // gpu time minus idle
      int32_t gpuSync;            // us after gpuStart
      int32_t cpuUntilGpuSync;
      bool    isOutlier;

    };


    SyncProps getPrediction() {

      SyncProps res = {};
      uint64_t id = m_propsFinished;
      if (id < DXGI_MAX_SWAP_CHAIN_BUFFERS+7)
        return res;

      for (size_t i=0; i<7; ++i) {
        const SyncProps& props = m_props[ (id-i) % m_props.size() ];
        if (!props.isOutlier) {
          id = id-i;
          break;
        }
      }

      return m_props[ id % m_props.size() ];

    };


    int32_t getGpuFinishedPrediction() {

      uint64_t id = m_propsFinished;
      if (id < DXGI_MAX_SWAP_CHAIN_BUFFERS+7)
        return 0;

      for (size_t i=0; i<7; ++i) {
        const SyncProps& props = m_props[ (id-i) % m_props.size() ];
        if (!props.isOutlier) {
          const LatencyMarkers* m = m_latencyMarkersStorage->getConstMarkers(id-i);
          return m->gpuFinished;
        }
      }

      const LatencyMarkers* m = m_latencyMarkersStorage->getConstMarkers(id);
      return m->gpuFinished;

    };


    bool isOutlier( uint64_t frameId ) {

      constexpr size_t numLoop = 7;
      int32_t totalCpuTime = 0;
      for (size_t i=0; i<numLoop; ++i) {
        const LatencyMarkers* m = m_latencyMarkersStorage->getConstMarkers(frameId-i);
        totalCpuTime += m->cpuFinished;
      }

      int32_t avgCpuTime = totalCpuTime / numLoop;
      if (m_latencyMarkersStorage->getConstMarkers(frameId)->cpuFinished > 1.7*avgCpuTime)
        return true;

      return false;

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
    std::array<SyncProps, 16> m_props;
    std::atomic<uint64_t> m_propsFinished = { 0 };
    std::vector<int32_t>  m_tempGpuRun;

  };

}
