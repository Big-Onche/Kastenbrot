#ifndef ENGINE_ACOUSTICS_H
#define ENGINE_ACOUSTICS_H

#include "AL/efx-presets.h"
#include "worldruntime.h"

namespace acoustics
{
    struct AcousticSourceInfo
    {
        vec apparent;
        float occlusion, virtualGain, virtualGainHF;
        bool path;

        AcousticSourceInfo() : apparent(0, 0, 0), occlusion(0), virtualGain(0), virtualGainHF(1), path(false) {}
    };

    void updateAcoustics();
    void acousticSource(const vec &loc, float dist, float &volf, float &gainhf, float &reverbSend, AcousticSourceInfo *info = NULL);
    void acousticHudSource(float &reverbSend);
    void drawAcousticsDebug();

    void clearAcousticGrid();
    void bakeAcousticGrid(int cellsize, int rays);
    bool loadAcousticGrid(const char *mname = NULL);
    bool saveAcousticGrid(const char *mname = NULL);
    void setAcousticBakeCorner(int corner, const vec &pos);
    int numAcousticCells();
    int numAcousticRegions();
    int numAcousticPortals();

    bool bakeChunkAcoustics(const worldsectionrenderdata &renderdata, int chunkx, int chunky, vector<uchar> &data,
                            SDL_atomic_t *cancelled = NULL);
    bool installChunkAcoustics(int chunkx, int chunky, const ivec &runtimeorigin, const vector<uchar> &data);
    void unloadChunkAcoustics(int chunkx, int chunky);
    void rebaseChunkAcoustics(float shiftx, float shifty);
    void clearChunkAcoustics();
}

namespace sound
{
    void updateAcousticReverb(const EFXEAXREVERBPROPERTIES *acousticShape, float reverbGain, float reverbDecay, float reflectionAmount);
}

#endif
