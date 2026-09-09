#ifndef ENGINE_ACOUSTICS_H
#define ENGINE_ACOUSTICS_H

#include "AL/efx-presets.h"

namespace acoustics
{
    struct AcousticSourceInfo
    {
        vec apparent;
        float occlusion, virtualGain, virtualGainHF;
        bool path;

        AcousticSourceInfo() : apparent(0, 0, 0), occlusion(0), virtualGain(0), virtualGainHF(1), path(false) {}
    };

    void resetAcoustics();
    void rebaseAcoustics(float shiftx, float shifty);
    void updateAcoustics();
    void acousticSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend, AcousticSourceInfo *info = NULL);
    void acousticAmbientSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend, AcousticSourceInfo &info);
    void acousticHudSource(float &reverbSend);
    void drawAcousticsDebug();

} // namespace acoustics

namespace sound
{
    void updateAcousticReverb(const EFXEAXREVERBPROPERTIES *acousticShape, float reverbGain, float reverbDecay, float reflectionAmount);
}

#endif
