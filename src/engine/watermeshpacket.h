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

struct watermeshpacket
{
    vector<watermeshvertex> vertices;
    vector<uint> indices;
    vector<watermeshpatch> patches;
    int first[8], count[8];

    watermeshpacket()
    {
        memset(first, 0, sizeof(first));
        memset(count, 0, sizeof(count));
    }
};
#endif