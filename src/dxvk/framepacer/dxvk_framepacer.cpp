#include "dxvk_framepacer.h"
#include "dxvk_framepacer_mode_low_latency.h"
#include "dxvk_framepacer_mode_min_latency.h"
#include "dxvk_options.h"
#include "../../util/util_env.h"
#include "../../util/log/log.h"

namespace dxvk {


  FramePacer::FramePacer( const DxvkOptions& options ) {
    FramePacerMode::Mode mode = FramePacerMode::LOW_LATENCY;

    std::string configStr = env::getEnvVar("DXVK_FRAME_PACE");

    if (configStr.find("max-frame-latency") != std::string::npos) {
      mode = FramePacerMode::MAX_FRAME_LATENCY;
    } else if (configStr.find("low-latency") != std::string::npos) {
      mode = FramePacerMode::LOW_LATENCY;
    } else if (configStr.find("min-latency") != std::string::npos) {
      mode = FramePacerMode::MIN_LATENCY;
    } else if (options.framePace.find("max-frame-latency") != std::string::npos) {
      mode = FramePacerMode::MAX_FRAME_LATENCY;
    } else if (options.framePace.find("low-latency") != std::string::npos) {
      mode = FramePacerMode::LOW_LATENCY;
    } else if (options.framePace.find("min-latency") != std::string::npos) {
      mode = FramePacerMode::MIN_LATENCY;
    }

    switch (mode) {
      case FramePacerMode::MAX_FRAME_LATENCY:
        Logger::info( "Frame pace: max-frame-latency" );
        m_mode = std::make_unique<FramePacerMode>(FramePacerMode::MAX_FRAME_LATENCY, &m_latencyMarkersStorage);
        break;

      case FramePacerMode::LOW_LATENCY:
        Logger::info( "Frame pace: low-latency" );
        m_mode = std::make_unique<LowLatencyMode>(mode, &m_latencyMarkersStorage, options);
        break;

      case FramePacerMode::MIN_LATENCY:
        Logger::info( "Frame pace: min-latency" );
        m_mode = std::make_unique<MinLatencyMode>(mode, &m_latencyMarkersStorage);
        break;
    }
  }


  FramePacer::~FramePacer() {}

}

