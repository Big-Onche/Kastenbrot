// Build with -O2 -flto -static -ffunction-sections -fdata-sections -Wl,--gc-sections.
#include "game.h"
#undef VAR
#undef VARP
#undef FVARP
#define VAR(name, low, current, high) int name = current
#define VARP(name, low, current, high) int name = current
#define FVARP(name, low, current, high) float name = current
#include "../game/ambient.cpp"
#include "soundocclusion.h"
#include <cassert>
#undef main

int totalmillis = 1000;
static int markers = 0, labels = 0;
namespace sound { int soundchans = 16; }
namespace game
{
    namespace weather
    {
        float getcloudspeed(float fallback)
        {
            return 0;
        }
        float samplecurrentrain(float x, float y, float height)
        {
            return 0;
        }
    }
    namespace environment
    {
        float getdayprogress()
        {
            return 0.5f;
        }
    }
}
float cloudwindspeed = 0;
float worldpositionheight(float z)
{
    return z / 16 - 256;
}
void stopambientloop(uint handle)
{
}
bool ambientloopocclusion(uint handle, float &occlusion, float &gain)
{
    return false;
}
void worldpositiontolocal(vec &position)
{
}
void particle_splash(int type, int num, int fade, const vec &position, int color, float size, int radius, int gravity)
{
    assert(radius > 0); // splash() performs randomMT() % (radius * 2).
    ++markers;
}
void particle_textcopy(const vec &position, const char *text, int type, int fade, int color, float size, int gravity)
{
    assert(text && text[0]);
    ++labels;
}

int main()
{
    using namespace game::ambience;
    float coarse = 0, fine = 0;
    loopi(10) approach(coarse, 1, 1, 5);
    loopi(600) approach(fine, 1, 1.0f / 60, 5);
    assert(fabsf(coarse - fine) < 0.00001f);
    assert(coarse > 0 && coarse < 1);
    AmbientManager manager;
    Site site = {};
    site.placement.cave = true;
    float previous[11] = {};
    loopi(1001)
    {
        site.depth = i / 1000.0f;
        float sum = 0;
        loop(t, 11)
        {
            float value = manager.weight(site, t, 1, 1, 1);
            assert(value >= 0 && value <= 1);
            if(t < 7) assert(value == 0);
            if(i) assert(fabsf(value - previous[t]) < 0.002f);
            previous[t] = value;
            sum += value;
        }
        assert(sum >= 0.99999f && sum <= 1.12501f);
    }
    site.placement.cave = false;
    site.altitude = 1;
    site.temperature = 1;
    assert(manager.weight(site, 2, 1, 0, 0) == ambientaltitudewind);
    site.water = true;
    assert(manager.weight(site, 6, 1, 0, 0) == 1);
    assert(manager.weight(site, 4, 1, 0, 0) == 0);
    site.placement.cave = true;
    assert(manager.weight(site, 2, 1, 0, 0) == 0);
    assert(manager.weight(site, 6, 1, 0, 0) == 0);
    assert(placementseed(42, ivec(-12, 38, 7)) == placementseed(42, ivec(-12, 38, 7)));
    assert(placementseed(42, ivec(-12, 38, 7)) != placementseed(42, ivec(-11, 38, 7)));
    assert(placementseed(42, ivec(-12, 38, 7)) != placementseed(43, ivec(-12, 38, 7)));
    // The minimum radius covers the farthest corner of a 16-cube section plus 5 cubes of clearance.
    assert(640 > sqrtf(256 * 256 * 3.0f) + 80);
    site.placement.position = vec(0, 0, 0);
    manager.sites.add(site);
    debugambient = 0;
    manager.debugparticles(vec(0, 0, 0));
    assert(markers == 0 && labels == 0);
    debugambient = 1;
    manager.debugparticles(vec(0, 0, 0));
    assert(markers == 1 && labels == 1);
    manager.debugparticles(vec(0, 0, 0));
    assert(markers == 1 && labels == 1); // Rate limited within the frame.
    totalmillis += 250;
    manager.debugparticles(vec(0, 0, 0));
    assert(markers == 2 && labels == 2);
    AmbientManager streaming;
    loopi(16)
    {
        Site next = {};
        next.placement.key = ivec(i, 0, 10);
        next.placement.position = vec(10 + i * 10, 0, 0);
        next.temperature = 1;
        streaming.sites.add(next);
    }
    loopi(8)
    {
        Voice &old = streaming.voices[i];
        old.key = streaming.sites[i + 8].placement.key;
        old.position = streaming.sites[i + 8].placement.position;
        old.type = 0;
        old.current = old.target = 0.1f;
        old.selected = true;
    }
    streaming.select(vec(0, 0, 0));
    assert(streaming.waiting == 6);
    loopi(8) assert(!streaming.voices[i].selected && streaming.voices[i].target == 0);
    loopi(8) streaming.voices[i].current = 0;
    streaming.select(vec(0, 0, 0));
    assert(streaming.waiting == 0);
    loopi(6) assert(streaming.voices[i].selected && streaming.voices[i].key.x < 6);
    streaming.select(vec(10000, 0, 0));
    loopi(8) assert(streaming.voices[i].handle == 0 && !streaming.voices[i].selected);
    const vec source(0, 0, 10), listener(100, 0, 10);
    bool wall = false;
    auto trace = [&wall](const vec &, const vec &, float length) { return wall ? 50.0f : length; };
    assert(soundpointocclusion(source, listener, trace) == 0);
    wall = true; // Geometry changes while both endpoints remain stationary.
    assert(soundpointocclusion(source, listener, trace) == 1);
    wall = false;
    assert(soundpointocclusion(source, listener, trace) == 0);
    assert(soundpointocclusion(source, listener, [](const vec &, const vec &, float) { return -1.0f; }) == 0);
    assert(soundpointocclusion(source, listener, [](const vec &, const vec &, float length) { return length - 0.5f; }) == 0);
    int probe = 0;
    assert(soundpointocclusion(source, listener, [&probe](const vec &, const vec &, float length)
    {
        return probe++ < 2 ? 50.0f : length;
    }) > 0);
    auto solidwall = [](const ivec &point) { return point.x >= 32 && point.x < 48; };
    assert(soundworldhit(vec(0, 0, 0), vec(1, 0, 0), 100, 16, solidwall) < 48);
    assert(soundworldhit(vec(100, 0, 0), vec(-1, 0, 0), 100, 16, solidwall) < 69);
    assert(soundworldhit(vec(0, 0, 0), vec(0, 0, 1), 100, 16, solidwall) == 100);
    puts("Ambient tests passed: debug safety, streamed voice handoff, direct occlusion and streamed geometry traversal.");
    return 0;
}
