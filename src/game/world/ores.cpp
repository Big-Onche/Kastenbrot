#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

struct worldoredefinition
{
    const char *id;
    int minheight, maxheight, optimalminheight, optimalmaxheight;
    int mindepth, maxdepth, minvein, maxvein, rareminvein, raremaxvein, cellsize;
    float chance, geologicalbonus;
    uint salt;
    bool uniformdistribution;
};

static const worldoredefinition worldores[] = {
    // Elevation and depth values are in world blocks relative to sea level.
    {"coal_ore", -112, 200, -32, 64, 4, 110, 8, 28, 40, 80, 12, 1.05f, 1.6f, 0x4A1D3B27U, false},
    {"copper_ore", -128, 128, -48, 32, 10, 130, 5, 16, 0, 0, 14, 0.90f, 1.5f, 0x7C3E91A5U, false},
    {"iron_ore", -192, 160, -96, 16, 12, 180, 6, 20, 0, 0, 14, 0.85f, 1.7f, 0xB6A54D19U, false},
    {"tin_ore", -176, 64, -112, -48, 25, 170, 3, 9, 0, 0, 16, 0.80f, 1.4f, 0xD82F6043U, false},
    {"gold_ore", -224, -32, -168, -112, 60, WORLD_HEIGHT_BLOCKS, 2, 7, 0, 0, 20, 0.75f, 1.8f, 0xE91B72C5U, false},
    {"diamond_ore", -248, -136, -232, -200, 120, WORLD_HEIGHT_BLOCKS, 1, 5, 0, 0, 24, 0.62f, 1.25f, 0xF05A8C31U, false},
    {"moon_dust_ore", 160, 255, 192, 224, 0, WORLD_HEIGHT_BLOCKS, 1, 3, 1, 3, 8, 1.2f, 1.0f, 0x2C7E4B91U, true}};

static float worldoreoptimalweight(const worldoredefinition &ore, int elevation)
{
    const int edge = elevation < ore.optimalminheight ? ore.optimalminheight - ore.minheight : ore.maxheight - ore.optimalmaxheight;
    if(elevation >= ore.optimalminheight && elevation <= ore.optimalmaxheight) return 1.0f;
    if(edge <= 0) return 0.35f;
    const int dist = elevation < ore.optimalminheight ? ore.optimalminheight - elevation : elevation - ore.optimalmaxheight;
    return 1.0f - 0.65f * clamp(dist / float(edge), 0.0f, 1.0f);
}

static float worldoreelevationweight(const worldoredefinition &ore, int elevation)
{
    if(!ore.uniformdistribution) return worldoreoptimalweight(ore, elevation);

    // Moon Dust is uncommon near the surface and reaches its full rate at
    // -196, remaining capped throughout the deepest layers.
    const float depth = clamp((float(WORLD_MAX_HEIGHT - 1) - elevation) / 451.0f, 0.0f, 1.0f);
    return 0.10f + 0.90f * depth * depth;
}

static float worldoregeologicalweight(const worldoredefinition &ore, const game::worldtectonicsample &tectonics, int elevation)
{
    if(ore.uniformdistribution) return 1.0f;
    const float relief = clamp(tectonics.terrainroughness, 0.0f, 1.0f), mountain = worldsmoothstep(0.45f, 0.75f, relief),
                hill = worldsmoothstep(0.12f, 0.45f, relief), deep = 1.0f - worldsmoothstep(-180.0f, -80.0f, float(elevation)),
                activity = clamp(tectonics.activity, 0.0f, 1.0f);
    if(!strcmp(ore.id, "coal_ore")) return 1.0f + (ore.geologicalbonus - 1.0f) * mountain;
    if(!strcmp(ore.id, "copper_ore")) return 1.0f + (ore.geologicalbonus - 1.0f) * hill;
    if(!strcmp(ore.id, "iron_ore")) return (1.0f + 0.7f * mountain) * (1.0f + 0.3f * activity);
    if(!strcmp(ore.id, "tin_ore")) return 1.0f + (ore.geologicalbonus - 1.0f) * mountain * deep;
    if(!strcmp(ore.id, "gold_ore")) return 1.0f + (ore.geologicalbonus - 1.0f) * activity;
    return 1.0f + (ore.geologicalbonus - 1.0f) * deep;
}

static int worldoreveinradius(const worldoredefinition &ore)
{
    const int largestvein = max(ore.maxvein, ore.raremaxvein), radius = int(ceilf(powf(max(float(largestvein), 1.0f), 1.0f / 3.0f) * 2.0f));
    return max(radius, 2);
}

static bool worldorecaveedge(const worldgencontext &ctx, int worldx, int worldy, int elevation)
{
    static const int directions[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    if(worldcaveairat(ctx, worldx, worldy, elevation)) return false;
    loopi(6)
        if(worldcaveairat(ctx, worldx + directions[i][0], worldy + directions[i][1], elevation + directions[i][2])) return true;
    return false;
}

static int worldorehost(const worldgencontext &ctx, const cube &c)
{
    if(isempty(c) || c.material != MAT_AIR) return -1;
    loopi(3)
        if(ctx.geologytextures[i] >= 0 && c.texture[0] == ctx.geologytextures[i]) return i;
    return -1;
}

static void placeworldoreblock(worldgencontext &ctx, cube *root, const worldoredefinition &ore, int chunkx, int chunky, int worldx, int worldy,
                               int elevation, int orecube)
{
    if(elevation < WORLD_MIN_HEIGHT || elevation >= WORLD_MAX_HEIGHT) return;
    const int localx = worldx - chunkx * WORLD_CHUNK_BLOCKS, localy = worldy - chunky * WORLD_CHUNK_BLOCKS;

    if(localx < 0 || localx >= WORLD_CHUNK_BLOCKS || localy < 0 || localy >= WORLD_CHUNK_BLOCKS) return;

    const int surfaceheight = ctx.heightmap[localy * WORLD_CHUNK_BLOCKS + localx] / WORLD_BLOCK_SIZE, depth = surfaceheight - elevation;

    if(elevation < ore.minheight || elevation > ore.maxheight || depth < ore.mindepth || depth > ore.maxdepth) return;

    cube &c =
        lookupworldgenblock(ctx, root, ivec(localx * WORLD_BLOCK_SIZE, localy * WORLD_BLOCK_SIZE, (elevation - WORLD_MIN_HEIGHT) * WORLD_BLOCK_SIZE));
    const int host = worldorehost(ctx, c);
    if(host >= 0) setworldcubetype(c, ctx, ctx.rockvariants[orecube][host]);
}

static void placeworldorevein(worldgencontext &ctx, cube *root, const worldoredefinition &ore, int chunkx, int chunky, long long cellx,
                              long long celly, int cellz, int centerx, int centery, int centerz, int orecube)
{
    const uint sizehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, ore.salt ^ 0xA511E9B3U),
               shapehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, ore.salt ^ 0x63D83595U);
    const bool rare = ore.rareminvein > 0 && (sizehash & 0xFFU) < 8U;
    const int minvein = rare ? ore.rareminvein : ore.minvein, maxvein = rare ? ore.raremaxvein : ore.maxvein,
              veinrange = max(maxvein - minvein + 1, 1), veinsize = minvein + int((sizehash >> 8) % uint(veinrange)),
              radius = max(2, int(ceilf(powf(max(float(veinsize), 1.0f), 1.0f / 3.0f) * 2.0f))), verticalradius = max(1, radius * 3 / 4);
    static const int directions[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    vector<ivec> blocks, frontier;
    blocks.add(ivec(centerx, centery, centerz));
    frontier.add(ivec(centerx, centery, centerz));

    loop(step, veinsize - 1)
    {
        bool added = false;
        loop(attempt, 24)
        {
            const uint offsethash =
                hashworldfeature(uint(ctx.seed), cellx + step * 17 + attempt, celly - step * 31 - attempt, cellz + step, ore.salt ^ shapehash);
            const ivec &parent = frontier[offsethash % uint(frontier.length())];
            const int *direction = directions[(offsethash >> 8) % 6];
            const ivec block(parent.x + direction[0], parent.y + direction[1], parent.z + direction[2]);
            const int offsetx = block.x - centerx, offsety = block.y - centery, offsetz = block.z - centerz;
            const float horizontal = (offsetx * offsetx + offsety * offsety) / float(radius * radius),
                        vertical = offsetz * offsetz / float(verticalradius * verticalradius);
            if(horizontal + vertical > 1.0f) continue;

            bool duplicate = false;
            loopv(blocks)
                if(blocks[i] == block)
                {
                    duplicate = true;
                    break;
                }
            if(duplicate) continue;
            blocks.add(block);
            frontier.add(block);
            added = true;
            break;
        }
        if(!added) break;
    }

    loopv(blocks) placeworldoreblock(ctx, root, ore, chunkx, chunky, blocks[i].x, blocks[i].y, blocks[i].z, orecube);
}

bool placeworldores(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS, chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS;
    if(ctx.geologytextures[0] < 0) return true;

    loopi(int(sizeof(worldores) / sizeof(worldores[0])))
    {
        const worldoredefinition &ore = worldores[i];
        const int orecube = ctx.cubetype(ore.id), radius = worldoreveinradius(ore), cellsize = max(ore.cellsize, 1);
        if(!ctx.cubetextures.inrange(orecube)) continue;

        const long long mincellx = worldfloordiv(chunkstartx - radius, cellsize),
                        maxcellx = worldfloordiv(chunkstartx + WORLD_CHUNK_BLOCKS - 1 + radius, cellsize),
                        mincelly = worldfloordiv(chunkstarty - radius, cellsize),
                        maxcelly = worldfloordiv(chunkstarty + WORLD_CHUNK_BLOCKS - 1 + radius, cellsize);
        const int mincellz = int(worldfloordiv(ore.minheight - radius, cellsize)), maxcellz = int(worldfloordiv(ore.maxheight + radius, cellsize));

        for(long long celly = mincelly; celly <= maxcelly; ++celly)
            for(long long cellx = mincellx; cellx <= maxcellx; ++cellx)
                for(int cellz = mincellz; cellz <= maxcellz; ++cellz)
                {
                    if(ctx.iscanceled()) return false;
                    const uint positionhash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, ore.salt),
                               chancehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, ore.salt ^ 0xC13FA9A9U);
                    const int centerpadding = ore.uniformdistribution ? radius : 0, centerspan = max(cellsize - centerpadding * 2, 1),
                              centerx = int(cellx * cellsize + centerpadding + positionhash % uint(centerspan)),
                              centery = int(celly * cellsize + centerpadding + (positionhash >> 8) % uint(centerspan)),
                              centerz = int(cellz * cellsize + centerpadding + (positionhash >> 16) % uint(centerspan));

                    if(centerz < ore.minheight || centerz > ore.maxheight) continue;

                    // Ore eligibility uses uncarved terrain; distant vein candidates must not start surface hydrology planning.
                    const int surfaceheight = ctx.generator.baseheight(centerx, centery);
                    const int depth = surfaceheight - centerz;
                    if(depth < ore.mindepth || depth > ore.maxdepth) continue;

                    const game::worldtectonicsample tectonics = ctx.generator.tectonics(centerx, centery);
                    const float caveweight = ore.uniformdistribution ? 1.0f : worldorecaveedge(ctx, centerx, centery, centerz) ? 2.5f : 0.75f;
                    const float chance =
                        clamp(ore.chance * worldoreelevationweight(ore, centerz) * worldoregeologicalweight(ore, tectonics, centerz) * caveweight,
                              0.0f, 1.0f);
                    if(worldtreeunit(chancehash) >= chance) continue;

                    placeworldorevein(ctx, root, ore, chunkx, chunky, cellx, celly, cellz, centerx, centery, centerz, orecube);
                }
    }
    return !ctx.iscanceled();
}
