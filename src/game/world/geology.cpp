#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

bool generateworldgeology(worldgencontext &ctx, int chunkx, int chunky)
{
    // One shared 3D field bends both contacts. Sampling a world-aligned lattice keeps chunks seamless and avoids voxel noise calls.
    loop(z, WORLD_GEOLOGY_HEIGHT)
    {
        if(ctx.iscanceled()) return false;
        const int elevation = WORLD_MIN_HEIGHT + z * WORLD_GEOLOGY_STEP;
        const float depth = float(ctx.settings.sealevel - elevation);
        loop(y, WORLD_GEOLOGY_WIDTH)
            loop(x, WORLD_GEOLOGY_WIDTH)
            {
                float warpeddepth = depth;
                if((depth >= 48 && depth <= 112) || (depth >= 144 && depth <= 208))
                {
                    warpeddepth +=
                        32.0f * ctx.generator.deeprock.GetNoise(float(chunkx * WORLD_CHUNK_BLOCKS + x * WORLD_GEOLOGY_STEP),
                                                                float(chunky * WORLD_CHUNK_BLOCKS + y * WORLD_GEOLOGY_STEP), float(elevation));
                }
                ctx.geology[(z * WORLD_GEOLOGY_WIDTH + y) * WORLD_GEOLOGY_WIDTH + x] = warpeddepth;
            }
    }
    return true;
}

static float sampleworldgeology(const worldgencontext &ctx, float x, float y, float z)
{
    const float scale = float(WORLD_GEOLOGY_STEP * WORLD_BLOCK_SIZE);
    x /= scale;
    y /= scale;
    z /= scale;

    const int ix = min(int(x), WORLD_GEOLOGY_WIDTH - 2), iy = min(int(y), WORLD_GEOLOGY_WIDTH - 2), iz = min(int(z), WORLD_GEOLOGY_HEIGHT - 2);
    x -= ix;
    y -= iy;
    z -= iz;

    float depth = 0;
    loop(dz, 2)
        loop(dy, 2)
            loop(dx, 2)
                depth += ctx.geology[((iz + dz) * WORLD_GEOLOGY_WIDTH + iy + dy) * WORLD_GEOLOGY_WIDTH + ix + dx] * (dx ? x : 1 - x) *
                         (dy ? y : 1 - y) * (dz ? z : 1 - z);

    return depth;
}

static int worldgeologylayer(float depth)
{
    return depth >= 176 ? 2 : depth >= 80 ? 1 : 0;
}

int worldgeologicalcubetype(const worldgencontext &ctx, const ivec &o, int size)
{
    const float bottomdepth = ctx.settings.sealevel - WORLD_MIN_HEIGHT - o.z / float(WORLD_BLOCK_SIZE),
                topdepth = bottomdepth - size / float(WORLD_BLOCK_SIZE);

    if(bottomdepth <= 48) return ctx.geologymaterials[0];
    if(topdepth >= 208) return ctx.geologymaterials[2];
    if(topdepth >= 112 && bottomdepth <= 144) return ctx.geologymaterials[1];
    if(size > WORLD_GEOLOGY_STEP * WORLD_BLOCK_SIZE) return WORLD_TERRAIN_MIXED;
    if(size <= WORLD_BLOCK_SIZE)
        return ctx.geologymaterials[worldgeologylayer(sampleworldgeology(ctx, o.x + size * 0.5f, o.y + size * 0.5f, o.z + size * 0.5f))];

    // Octree cells fit inside one lattice cell. Trilinear values stay within their corner bounds, so uniform cells need no subdivision.
    int layer = -1;
    loopi(8)
    {
        const ivec corner(i, o, size);
        const int current = worldgeologylayer(sampleworldgeology(ctx, corner.x, corner.y, corner.z));
        if(layer >= 0 && current != layer) return WORLD_TERRAIN_MIXED;
        layer = current;
    }
    return ctx.geologymaterials[layer];
}
