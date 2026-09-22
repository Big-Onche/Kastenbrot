#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

int generateworldheight(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky, game::worldtectonicsample *tectonics)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx, y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.height(x, y, tectonics) * WORLD_BLOCK_SIZE;
}

static void generateworldbeachmap(worldgencontext &ctx, int chunkx, int chunky)
{
    memset(ctx.beachmap, 0, sizeof(ctx.beachmap));
    if(ctx.settings.coastwidth <= 0) return;

    const int maxbeachwidth = int(ceil(ctx.generator.maxbeachtransitionwidth())), halo = maxbeachwidth + 1, mapsize = WORLD_CHUNK_BLOCKS + 2 * halo,
              maparea = mapsize * mapsize, fardistance = INT_MAX / 8, seaheight = ctx.settings.sealevel * WORLD_BLOCK_SIZE;
    vector<uchar> water;
    vector<int> distance;
    water.pad(maparea);
    distance.pad(maparea);

    loop(y, mapsize)
        loop(x, mapsize)
        {
            const int blockx = x - halo, blocky = y - halo,
                      height = blockx >= 0 && blockx < WORLD_CHUNK_BLOCKS && blocky >= 0 && blocky < WORLD_CHUNK_BLOCKS
                                   ? ctx.heightmap[blocky * WORLD_CHUNK_BLOCKS + blockx]
                                   : generateworldheight(ctx, chunkx, chunky, blockx, blocky);
            water[y * mapsize + x] = height < seaheight;
            distance[y * mapsize + x] = fardistance;
        }

    for(int y = 1; y < mapsize - 1; ++y)
        for(int x = 1; x < mapsize - 1; ++x)
        {
            const int index = y * mapsize + x;
            const uchar iswater = water[index];
            if(water[index - 1] != iswater || water[index + 1] != iswater || water[index - mapsize] != iswater || water[index + mapsize] != iswater)
                distance[index] = 0;
        }

    for(int y = 1; y < mapsize - 1; ++y)
        for(int x = 1; x < mapsize - 1; ++x)
        {
            const int index = y * mapsize + x;
            distance[index] = min(distance[index], distance[index - 1] + 3);
            distance[index] = min(distance[index], distance[index - mapsize] + 3);
            distance[index] = min(distance[index], distance[index - mapsize - 1] + 4);
            distance[index] = min(distance[index], distance[index - mapsize + 1] + 4);
        }
    for(int y = mapsize - 2; y >= 1; --y)
        for(int x = mapsize - 2; x >= 1; --x)
        {
            const int index = y * mapsize + x;
            distance[index] = min(distance[index], distance[index + 1] + 3);
            distance[index] = min(distance[index], distance[index + mapsize] + 3);
            distance[index] = min(distance[index], distance[index + mapsize + 1] + 4);
            distance[index] = min(distance[index], distance[index + mapsize - 1] + 4);
        }

    loop(y, WORLD_CHUNK_BLOCKS)
        loop(x, WORLD_CHUNK_BLOCKS)
        {
            const int index = y * WORLD_CHUNK_BLOCKS + x, coastdistance = distance[(y + halo) * mapsize + x + halo];
            ctx.beachmap[index] =
                coastdistance <=
                int(floor(ctx.generator.beachtransitionwidth(chunkx * WORLD_CHUNK_BLOCKS + x, chunky * WORLD_CHUNK_BLOCKS + y) * 3.0f + 0.5f));
        }
}

static int generateworldmaterial(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky, int height)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx, y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.biome(x, y, height / WORLD_BLOCK_SIZE);
}

static bool generateworldrock(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky, int height)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx, y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.rock(x, y, height / WORLD_BLOCK_SIZE);
}

static int generateworldcliff(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky, int height)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx, y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    bool face = false;
    const bool coast = ctx.generator.cliff(x, y, height / WORLD_BLOCK_SIZE, &face);
    return (coast ? WORLD_CLIFF_COAST : 0) | (face ? WORLD_CLIFF_ROCK : 0);
}

bool generateworldheightmap(worldgencontext &ctx, int chunkx, int chunky)
{
    {
        ZoneScopedN("Chunks/Generate terrain heights");
        loop(y, WORLD_CHUNK_BLOCKS)
        {
            if(ctx.iscanceled()) return false;
            loop(x, WORLD_CHUNK_BLOCKS)
            {
                const int index = y * WORLD_CHUNK_BLOCKS + x;
                game::worldtectonicsample tectonics;
                ctx.heightmap[index] = generateworldheight(ctx, chunkx, chunky, x, y, &tectonics);
                ctx.watermap[index] =
                    ctx.generator.surface(chunkx * WORLD_CHUNK_BLOCKS + x, chunky * WORLD_CHUNK_BLOCKS + y).water * WORLD_BLOCK_SIZE;
                ctx.reliefcliffmap[index] = tectonics.rockyledge > 0.22f;
            }
        }
    }
    {
        ZoneScopedN("Chunks/Generate beach map");
        generateworldbeachmap(ctx, chunkx, chunky);
    }
    {
        ZoneScopedN("Chunks/Generate biome maps");
        loop(y, WORLD_CHUNK_BLOCKS)
        {
            if(ctx.iscanceled()) return false;
            loop(x, WORLD_CHUNK_BLOCKS)
            {
                const int index = y * WORLD_CHUNK_BLOCKS + x;
                ctx.biomemap[index] = generateworldmaterial(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);

                const float strata = ctx.generator.rockiness.GetNoise(float(chunkx * WORLD_CHUNK_BLOCKS + x), float(chunky * WORLD_CHUNK_BLOCKS + y));
                ctx.sandstonedepthmap[index] = clamp(int(floorf(3.5f + 2.0f * strata)), 2, 4);
                const vec beachposition(float(chunkx * WORLD_CHUNK_BLOCKS + x) * WORLD_BLOCK_SIZE,
                                        float(chunky * WORLD_CHUNK_BLOCKS + y) * WORLD_BLOCK_SIZE, float(WORLD_GROUND_HEIGHT + ctx.heightmap[index]));
                ctx.snowbeachmap[index] = ctx.generator.environmentclimate.gettemperature(beachposition) < -5.0f;
                ctx.materialmap[index] = ctx.generator.surfacematerial(chunkx * WORLD_CHUNK_BLOCKS + x, chunky * WORLD_CHUNK_BLOCKS + y,
                                                                       ctx.heightmap[index] / WORLD_BLOCK_SIZE);
                ctx.cliffmap[index] =
                    (ctx.reliefcliffmap[index] ? WORLD_CLIFF_ROCK : 0) | generateworldcliff(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
                ctx.rockmap[index] = generateworldrock(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
            }
        }
    }
    return !ctx.iscanceled();
}

static int worldheight(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.heightmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static int worldmaterial(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.materialmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static bool worldbeach(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.beachmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static bool worldrock(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.rockmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static int worldcliff(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.cliffmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static int worldwaterheight(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.watermap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static const char *coldsurfacename(int material)
{
    switch(material)
    {
    case game::WORLD_SNOWY_GRASS:
        return "snowy_grass";
    case game::WORLD_FROZEN_DIRT:
        return "frozen_dirt";
    case game::WORLD_MOSS:
        return "moss";
    case game::WORLD_FROZEN_MOSS:
        return "frozen_moss";
    case game::WORLD_FROZEN_GRAVEL:
        return "frozen_gravel";
    case game::WORLD_COLD_ROCK:
        return "stone";
    case game::WORLD_SNOW_CRUST:
        return "snow_crust";
    case game::WORLD_DEEP_SNOW:
        return "deep_snow";
    case game::WORLD_ICE:
        return "ice";
    default:
        return NULL;
    }
}

static int worldcolumncubetype(const worldgencontext &ctx, int z, int size, int height, int biome, bool beachprofile, int cliff, bool rock,
                               int waterheight = INT_MIN, int sandstonedepth = 3, bool snowbeach = false)
{
    const int surface = WORLD_GROUND_HEIGHT + height,
              watertop = WORLD_GROUND_HEIGHT + (waterheight == INT_MIN ? ctx.settings.sealevel * WORLD_BLOCK_SIZE : waterheight),
              dirtbottom = surface - ctx.settings.soildepth * WORLD_BLOCK_SIZE, grassbottom = surface - WORLD_BLOCK_SIZE,
              beachmin = (ctx.settings.sealevel + min(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE,
              beachmax = (ctx.settings.sealevel + max(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE;

    const bool beach = beachprofile && height >= beachmin && height <= beachmax;

    if(z >= max(surface, watertop)) return WORLD_TERRAIN_EMPTY;
    if(surface < watertop && z >= surface && z + size <= watertop)
    {
        if(biome == game::WORLD_FROZEN_WATER)
        {
            if(z >= watertop - WORLD_BLOCK_SIZE) return ctx.cubetype("ice");
            if(z + size > watertop - WORLD_BLOCK_SIZE) return WORLD_TERRAIN_MIXED;
        }
        return WORLD_TERRAIN_WATER;
    }
    const bool freshwaterbed = waterheight > ctx.settings.sealevel * WORLD_BLOCK_SIZE && surface < watertop,
               sandy = !freshwaterbed && !rock && !(cliff & WORLD_CLIFF_ROCK) &&
                       ((cliff & WORLD_CLIFF_COAST) ? biome == game::WORLD_BIOME_DESERT : beach || biome == game::WORLD_BIOME_DESERT);

    if(sandy)
    {
        if(beach && snowbeach && surface >= watertop)
        {
            if(z >= grassbottom && z + size <= surface) return ctx.cubetype("snow");
            if(z < surface && z + size > grassbottom) return WORLD_TERRAIN_MIXED;
        }
        // Keep the existing sand thickness and insert sandstone between it and the underlying geology.
        const int sandbottom = cliff & WORLD_CLIFF_COAST ? grassbottom : dirtbottom,
                  stonebottom = sandbottom - clamp(sandstonedepth, 2, 4) * WORLD_BLOCK_SIZE;

        if(z + size <= stonebottom) return ctx.cubetype("stone");
        if(z >= stonebottom && z + size <= sandbottom) return ctx.cubetype("sandstone");
        if(z >= sandbottom && z + size <= surface) return ctx.cubetype("sand");
        // Force subdivision when a cube straddles either stratum boundary.
        return WORLD_TERRAIN_MIXED;
    }
    if(z + size <= dirtbottom) return ctx.cubetype("stone");
    if(waterheight > ctx.settings.sealevel * WORLD_BLOCK_SIZE && surface < watertop)
    {
        if(z >= dirtbottom && z + size <= surface) return ctx.cubetype("dirt");
        return WORLD_TERRAIN_MIXED;
    }
    if(cliff & WORLD_CLIFF_ROCK)
    {
        // Every exposed stair of the cliff belongs to the rock face. Normal surface rules resume immediately behind this band, producing a grassy
        // plateau without grass caps scattered down the vertical wall.
        if(z >= dirtbottom && z + size <= surface) return ctx.cubetype("stone");
        return WORLD_TERRAIN_MIXED;
    }
    const char *coldsurface = coldsurfacename(biome);
    if(coldsurface && z >= grassbottom && z + size <= surface) return ctx.cubetype(coldsurface);
    if(rock)
    {
        if(biome == game::WORLD_BIOME_SNOW && z >= grassbottom && z + size <= surface) return ctx.cubetype("snow");
        if(z >= dirtbottom && z + size <= surface) return ctx.cubetype("stone");
        return WORLD_TERRAIN_MIXED;
    }
    if(cliff & WORLD_CLIFF_COAST)
    {
        // Sea cliffs have one surface block over solid stone, never a deep exposed soil layer.
        if(z + size <= grassbottom) return ctx.cubetype("stone");
        if(z >= grassbottom && z + size <= surface) return ctx.cubetype(biome == game::WORLD_BIOME_SNOW ? "snow" : "grass");
        return WORLD_TERRAIN_MIXED;
    }
    if(biome == game::WORLD_BIOME_OCEAN)
    {
        if(z >= dirtbottom && z + size <= surface) return ctx.cubetype("dirt");
        return WORLD_TERRAIN_MIXED;
    }
    if(z >= dirtbottom && z + size <= grassbottom) return ctx.cubetype("dirt");
    if(biome == game::WORLD_BIOME_SNOW && z >= grassbottom && z + size <= surface) return ctx.cubetype("snow");
    if(z >= grassbottom && z + size <= surface) return ctx.cubetype("grass");
    return WORLD_TERRAIN_MIXED;
}

int worldcubetype(const worldgencontext &ctx, const ivec &o, int size)
{
    if(o.x >= WORLD_CHUNK_SIZE || o.y >= WORLD_CHUNK_SIZE || o.z >= WORLD_MAP_SIZE) return WORLD_TERRAIN_EMPTY;
    if(o.x + size > WORLD_CHUNK_SIZE || o.y + size > WORLD_CHUNK_SIZE || o.z + size > WORLD_MAP_SIZE) return WORLD_TERRAIN_MIXED;

    int type = WORLD_TERRAIN_UNSET;
    for(int y = o.y; y < o.y + size; y += WORLD_BLOCK_SIZE)
        for(int x = o.x; x < o.x + size; x += WORLD_BLOCK_SIZE)
        {
            int columntype = worldcolumncubetype(ctx, o.z, size, worldheight(ctx, x, y), worldmaterial(ctx, x, y), worldbeach(ctx, x, y),
                                                 worldcliff(ctx, x, y), worldrock(ctx, x, y), worldwaterheight(ctx, x, y),
                                                 ctx.sandstonedepthmap[y / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + x / WORLD_BLOCK_SIZE],
                                                 ctx.snowbeachmap[y / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + x / WORLD_BLOCK_SIZE]);

            if(columntype == WORLD_TERRAIN_MIXED || (type != WORLD_TERRAIN_UNSET && type != columntype)) return WORLD_TERRAIN_MIXED;
            type = columntype;
        }
    return type == ctx.geologymaterials[0] ? worldgeologicalcubetype(ctx, o, size) : type;
}

int worldrepresentativecubetype(const worldgencontext &ctx, const ivec &o, int size)
{
    const int x = clamp(o.x + size / 2, 0, WORLD_CHUNK_SIZE - 1), y = clamp(o.y + size / 2, 0, WORLD_CHUNK_SIZE - 1), height = worldheight(ctx, x, y),
              biome = worldmaterial(ctx, x, y), surface = WORLD_GROUND_HEIGHT + height, watertop = WORLD_GROUND_HEIGHT + worldwaterheight(ctx, x, y),
              visibletop = max(surface, watertop);
    int z = clamp(o.z + size / 2, 0, WORLD_MAP_SIZE - 1);

    // A coarse cube intersecting the visible column top represents its
    // surface, not the greater volume underneath it. Sample immediately below
    // that top so grass/sand/snow/stone wins over dirt, and water wins for a
    // submerged terrain column. Cubes wholly underground retain the centre
    // sample used for their dominant interior material.
    if(visibletop > o.z && visibletop <= o.z + size) z = clamp(visibletop - 1, 0, WORLD_MAP_SIZE - 1);

    const int type =
        worldcolumncubetype(ctx, z, 1, height, biome, worldbeach(ctx, x, y), worldcliff(ctx, x, y), worldrock(ctx, x, y), worldwaterheight(ctx, x, y),
                            ctx.sandstonedepthmap[y / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + x / WORLD_BLOCK_SIZE],
                            ctx.snowbeachmap[y / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + x / WORLD_BLOCK_SIZE]);

    return type == ctx.geologymaterials[0] ? worldgeologicalcubetype(ctx, ivec(x, y, z), 1) : type;
}
