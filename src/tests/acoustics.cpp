// Build from src with the usual includes and -O2 -flto -static -ffunction-sections -fdata-sections -Wl,--gc-sections.
#include "engine.h"
#undef VAR
#undef VARP
#undef FVAR
#undef FVARP
#define VAR(name, low, current, high) int name = current
#define VARP(name, low, current, high) int name = current
#define FVAR(name, low, current, high) float name = current
#define FVARP(name, low, current, high) float name = current
#include "../engine/acoustics.cpp"
#include <cassert>
#undef main

int totalmillis = 0, worldsize = 4096;
static physent listener;
physent *camera1 = &listener;
static int casts = 0;
static bool enclosed = true;
static vector<vec> directions;

float raycube(const vec &origin, const vec &direction, float radius, int mode, int size, extentity *target)
{
    assert(origin == camera1->o);
    assert(fabsf(direction.magnitude() - 1) < 1e-5f);
    ++casts;
    directions.add(direction);
    return enclosed ? 10.0f : radius;
}

void conoutf(int type, const char *fmt, ...) {}

namespace sound
{
    void updateAcousticReverb(const EFXEAXREVERBPROPERTIES *shape, float gain, float decay, float reflection)
    {
        assert(gain >= 0 && gain <= 2);
        assert(decay > 0);
    }
} // namespace sound

static void start(int rays = 128, int interval = 250)
{
    acoustics::resetAcoustics();
    acoustics::soundacoustics = 1;
    acoustics::soundacousticrays = rays;
    acoustics::soundacousticinterval = interval;
    acoustics::soundacousticsmooth = 1000;
    listener.o = vec(100, 100, 100);
    totalmillis = casts = 0;
    directions.setsize(0);
    acoustics::updateAcoustics();
    assert(casts == 0);
}

static void advance(int elapsed)
{
    totalmillis += elapsed;
    acoustics::updateAcoustics();
}

int main()
{
    using namespace acoustics;
    // Every preset can win; selection must not remain at the indoor/outdoor fallback.
    loopi(AP_NUM)
    {
        float scores[AP_NUM] = {};
        scores[i] = 0.75f;
        scores[(i + 1) % AP_NUM] = 0.25f;
        AcousticChoice choice = chooseTopAcousticPresets(scores, AP_HALL);
        assert(choice.first == i && choice.second == (i + 1) % AP_NUM && choice.secondWeight == 0.25f);
    }
    // Exact budgets at multiple frame cadences, including odd ray counts and fractional rays per frame.
    const int counts[] = {16, 127, 128, 256}, intervals[] = {137, 250, 1000}, frames[] = {1, 7, 16, 33};
    for(int count : counts)
        for(int interval : intervals)
            for(int frame : frames)
            {
                start(count, interval);
                while(totalmillis < interval * 3)
                {
                    int step = min(frame, interval * 3 - totalmillis), before = casts;
                    listener.o.x += 0.125f; // Every cast must use the current camera, including during an unfinished sweep.
                    advance(step);
                    assert(casts == totalmillis * count / interval);
                    assert(casts - before <= (step * count + interval - 1) / interval);
                }
                assert(acousticReady);
                loopi(count)
                    loopj(i)
                        assert(directions[i].dist(directions[j]) > 1e-4f);
            }

    // A static room reaches its target exactly one second after the first completed evaluation.
    start();
    loopi(25)
        advance(10);
    assert(acousticReady && acousticEvaluation.skyOpenness == 0 && acousticEvaluation.outdoorRatio == 0);
    assert(acousticProbe.reverbGain == 0);
    AcousticReverb room = acousticTarget;
    loopi(50)
        advance(10);
    assert(fabsf(acousticProbe.reverbGain - room.reverbGain * 0.5f) < 1e-5f);
    assert(fabsf(acousticProbe.reverbShape.flGain - (acousticTransitionStart.reverbShape.flGain + room.reverbShape.flGain) * 0.5f) < 1e-5f);
    loopi(50)
        advance(10);
    assert(fabsf(acousticProbe.reverbGain - room.reverbGain) < 1e-5f);
    assert(fabsf(acousticProbe.reverbShape.flDecayTime - room.reverbShape.flDecayTime) < 1e-5f);

    // Destruction changes the classification without moving the listener or invalidating a cache.
    enclosed = false;
    loopi(25)
        advance(10);
    assert(acousticEvaluation.skyOpenness == 1 && acousticEvaluation.outdoorRatio > 0.9f);
    assert(acousticEvaluation.primaryPreset == AP_OPENOUTDOOR);
    assert(fabsf(acousticProbe.reverbGain - room.reverbGain) < 1e-5f);
    AcousticReverb outdoors = acousticTarget;
    loopi(100)
        advance(10);
    assert(fabsf(acousticProbe.reverbGain - outdoors.reverbGain) < 1e-5f);
    assert(fabsf(acousticProbe.reverbShape.flGain - outdoors.reverbShape.flGain) < 1e-5f);

    // Retargeting mid-transition starts at the current output, with no jump.
    enclosed = true;
    loopi(25)
        advance(10);
    loopi(50)
        advance(10);
    AcousticReverb middle = acousticProbe;
    assert(middle.reverbGain > outdoors.reverbGain && middle.reverbGain < room.reverbGain);
    soundacousticsmooth = 0;
    advance(0);
    assert(acousticProbe.reverbGain == acousticTarget.reverbGain);

    // Configuration changes discard a partial sweep and distribute the new budget from that point.
    advance(13);
    soundacousticrays = 16;
    soundacousticinterval = 100;
    soundacousticrange = 256;
    int before = casts;
    advance(0);
    assert(acousticSamples.empty() && casts == before);
    loopi(10)
        advance(10);
    assert(casts - before == 16 && acousticSampleRange == 256);

    // A stall never triggers multiple catch-up sweeps. A repeated timestamp never casts again.
    before = casts;
    advance(10000);
    assert(casts - before == 16);
    before = casts;
    advance(0);
    assert(casts == before);

    advance(20);
    assert(!acousticSamples.empty());
    float previousGain = acousticProbe.reverbGain;
    vec origin = acousticSamples[0].origin;
    rebaseAcoustics(32, 64);
    assert(acousticSamples[0].origin == vec(origin).sub(vec(32, 64, 0)));
    assert(acousticReady && acousticProbe.reverbGain == previousGain);

    soundacoustics = 0;
    before = casts;
    advance(1000);
    assert(!acousticReady && acousticSamples.empty() && casts == before);
    soundacoustics = 1;
    advance(0);
    assert(casts == before);
    camera1 = NULL;
    advance(100);
    assert(!acousticReady && casts == before);
    camera1 = &listener;
    listener.o.x = -1;
    advance(100);
    assert(!acousticReady && casts == before);
    puts("Camera acoustics tests passed");
    return 0;
}
