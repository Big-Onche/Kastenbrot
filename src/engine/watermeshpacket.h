#ifndef ENGINE_WATERMESHPACKET_H
#define ENGINE_WATERMESHPACKET_H
#include "watergeometry.h"
struct watermeshvertex
{
    vec4 position;
    vec normal;
};

struct watermeshpatch
{
    waterfacepatch face;
    int material, first;
};

struct watermeshstate
{
    vec4 endpoints; // min(height0 + wave * weight0, height1 + wave * weight1)
    vec2 surface; // spatial wave multiplier, undisplaced surface for pass selection
};

struct watermeshpacket
{
    vector<watermeshvertex> vertices;
    vector<uint> indices;
    vector<watermeshpatch> patches;
    vector<watermeshstate> states; // Immutable world-water attributes, built with the topology.
    int first[8], count[8];

    watermeshpacket()
    {
        memset(first, 0, sizeof(first));
        memset(count, 0, sizeof(count));
    }
};
#endif
