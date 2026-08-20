// worldgen.cpp: Kastenbrot procedural world generation

#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#include <SDL_atomic.h>
#endif
#include "worldruntime.h"
#include "world.h"
#include "worldgen.h"

extern vector<worldgencubetextures> worldgentextures;
extern int worldgrassscatter, worldrosescatter, worldtulipscatter, worlddandelionscatter;
extern int chunkremip, leavesalpha;
extern int remipworldchunk(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled);

static void resetworldgencube(cube &c)
{
    c.children = NULL;
    c.ext = NULL;
    c.visible = 0;
    c.merged = 0;
    c.material = MAT_AIR;
    emptyfaces(c);
    loopi(6) c.texture[i] = DEFAULT_GEOM;
}

static ivec worldgenorientnormal(int orient)
{
    ivec normal(0, 0, 0);
    normal[dimension(orient)] = dimcoord(orient) ? 1 : -1;
    return normal;
}

struct worldcavesegment
{
    vec start, end;
    float startradius, endradius, verticalscale;
    uint roughness;
    bool entrance;

    worldcavesegment(const vec &start, const vec &end, float startradius, float endradius, float verticalscale, uint roughness, bool entrance = false)
        : start(start), end(end), startradius(startradius), endradius(endradius), verticalscale(verticalscale), roughness(roughness),
          entrance(entrance)
    {
    }
};

struct worldcavechamber
{
    vec center;
    float radiusx, radiusy, radiusz, anglecos, anglesin;
    uint roughness;

    worldcavechamber(const vec &center, float radiusx, float radiusy, float radiusz, float angle, uint roughness)
        : center(center), radiusx(radiusx), radiusy(radiusy), radiusz(radiusz), anglecos(cosf(angle)), anglesin(sinf(angle)), roughness(roughness)
    {
    }
};

struct worldgencontext
{
    game::worldgenerator generator;
    game::worldsettings settings;
    int heightmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar biomemap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar beachmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar cliffmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar reliefcliffmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar rockmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    worldsectionrenderdata renderdata;
    vector<worldcavesegment> cavesegments;
    vector<worldcavechamber> cavechambers;
    int seed;
    vector<worldgencubetextures> cubetextures;
    mutable hashtable<const char *, int> cubeids;
    hashtable<ivec, int> surfaceheightcache;
    int errorcube;
    bool prepared, remip, indexedtextures;
    int families, optimized;
    SDL_atomic_t *cancelled;

    worldgencontext(int seed, const vector<worldgencubetextures> &cubetextures, bool prepared, bool remip,
                    const game::worldsettings &settings, SDL_atomic_t *cancelled = NULL, bool indexedtextures = false)
        : generator(seed, settings), settings(settings), seed(seed), cubetextures(cubetextures), cubeids(64), surfaceheightcache(1 << 12),
          errorcube(-1),
          prepared(prepared), remip(remip), indexedtextures(indexedtextures), families(0), optimized(0), cancelled(cancelled)
    {
        loopv(this->cubetextures) cubeids[this->cubetextures[i].id] = i;
        int *error = cubeids.access("error");
        errorcube = error ? *error : -1;
    }

    bool iscanceled() const { return cancelled && SDL_AtomicGet(cancelled); }

    int cubetype(const char *id) const
    {
        return cubeids.access(id, errorcube);
    }
};

static cube *allocworldgenfamily(worldgencontext &ctx)
{
#ifdef STANDALONE
    cube *c = new cube[8];
    loopi(8) resetworldgencube(c[i]);
    ctx.families++;
    return c;
#else
    if(!ctx.prepared) return newcubes(F_EMPTY);
    cube *c = new cube[8];
    loopi(8) resetworldgencube(c[i]);
    ctx.families++;
    return c;
#endif
}

static void freepreparedworldchunk(cube *root)
{
    if(!root) return;
    loopi(8) if(root[i].children) freepreparedworldchunk(root[i].children);
    delete[] root;
}

#ifndef STANDALONE
static void setworldcubetexture(cube &c, int texture, int toptexture = -1, int bottomtexture = -1, int material = MAT_AIR)
{
    solidfaces(c);
    c.material = material;
    loopi(6) c.texture[i] = texture;
    if(toptexture >= 0) c.texture[O_TOP] = toptexture;
    if(bottomtexture >= 0) c.texture[O_BOTTOM] = bottomtexture;
}
#endif

static bool setworldcubetype(cube &c, const worldgencontext &ctx, int index, int material = MAT_AIR)
{
    if(!ctx.cubetextures.inrange(index)) return false;
#ifdef STANDALONE
    solidfaces(c);
    c.material = material;
    loopi(6) c.texture[i] = ushort(index);
#else
    if(ctx.indexedtextures)
    {
        solidfaces(c);
        c.material = material;
        loopi(6) c.texture[i] = ushort(index);
    }
    else
    {
        const worldgencubetextures &textures = ctx.cubetextures[index];
        setworldcubetexture(c, textures.side, textures.top, textures.bottom, material);
    }
#endif
    return true;
}

static void setworldcubematerial(cube &c, int material)
{
    emptyfaces(c);
    c.material = material;
}

static const int WORLD_TERRAIN_EMPTY = -1, WORLD_TERRAIN_WATER = -2, WORLD_TERRAIN_MIXED = -3, WORLD_TERRAIN_UNSET = -4;

static float worldsmoothstep(float low, float high, float value)
{
    if(high <= low) return value >= high ? 1.0f : 0.0f;
    float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static int generateworldheight(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky, game::worldtectonicsample *tectonics = NULL)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx,
              y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.height(x, y, tectonics) * WORLD_BLOCK_SIZE;
}

static void generateworldbeachmap(worldgencontext &ctx, int chunkx, int chunky)
{
    memset(ctx.beachmap, 0, sizeof(ctx.beachmap));
    if(ctx.settings.coastwidth <= 0) return;

    const int maxbeachwidth = int(ceil(ctx.generator.maxbeachtransitionwidth())),
              halo = maxbeachwidth + 1,
              mapsize = WORLD_CHUNK_BLOCKS + 2 * halo,
              maparea = mapsize * mapsize,
              fardistance = INT_MAX / 8,
              seaheight = ctx.settings.sealevel * WORLD_BLOCK_SIZE;
    vector<uchar> water;
    vector<int> distance;
    water.pad(maparea);
    distance.pad(maparea);

    loop(y, mapsize) loop(x, mapsize)
    {
        const int blockx = x - halo, blocky = y - halo,
                  height = blockx >= 0 && blockx < WORLD_CHUNK_BLOCKS &&
                           blocky >= 0 && blocky < WORLD_CHUNK_BLOCKS
                         ? ctx.heightmap[blocky * WORLD_CHUNK_BLOCKS + blockx]
                         : generateworldheight(ctx, chunkx, chunky, blockx, blocky);
        water[y * mapsize + x] = height < seaheight;
        distance[y * mapsize + x] = fardistance;
    }

    for(int y = 1; y < mapsize - 1; ++y) for(int x = 1; x < mapsize - 1; ++x)
    {
        const int index = y * mapsize + x;
        const uchar iswater = water[index];
        if(water[index - 1] != iswater || water[index + 1] != iswater || water[index - mapsize] != iswater || water[index + mapsize] != iswater)
            distance[index] = 0;
    }

    for(int y = 1; y < mapsize - 1; ++y) for(int x = 1; x < mapsize - 1; ++x)
    {
        const int index = y * mapsize + x;
        distance[index] = min(distance[index], distance[index - 1] + 3);
        distance[index] = min(distance[index], distance[index - mapsize] + 3);
        distance[index] = min(distance[index], distance[index - mapsize - 1] + 4);
        distance[index] = min(distance[index], distance[index - mapsize + 1] + 4);
    }
    for(int y = mapsize - 2; y >= 1; --y) for(int x = mapsize - 2; x >= 1; --x)
    {
        const int index = y * mapsize + x;
        distance[index] = min(distance[index], distance[index + 1] + 3);
        distance[index] = min(distance[index], distance[index + mapsize] + 3);
        distance[index] = min(distance[index], distance[index + mapsize + 1] + 4);
        distance[index] = min(distance[index], distance[index + mapsize - 1] + 4);
    }

    loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
    {
        const int index = y * WORLD_CHUNK_BLOCKS + x,
                  coastdistance = distance[(y + halo) * mapsize + x + halo];
        ctx.beachmap[index] = coastdistance <= int(floor(ctx.generator.beachtransitionwidth(chunkx * WORLD_CHUNK_BLOCKS + x,
                                                                                           chunky * WORLD_CHUNK_BLOCKS + y) * 3.0f + 0.5f));
    }
}

static int generateworldbiome(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky, int height)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx,
              y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.biome(x, y, height / WORLD_BLOCK_SIZE);
}

static bool generateworldrock(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky, int height)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx,
              y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.rock(x, y, height / WORLD_BLOCK_SIZE);
}

static bool generateworldcliff(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky, int height)
{
    const int x = chunkx * WORLD_CHUNK_BLOCKS + blockx,
              y = chunky * WORLD_CHUNK_BLOCKS + blocky;
    return ctx.generator.cliff(x, y, height / WORLD_BLOCK_SIZE);
}

static bool generateworldheightmap(worldgencontext &ctx, int chunkx, int chunky)
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
                ctx.biomemap[index] = generateworldbiome(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
                ctx.cliffmap[index] = ctx.reliefcliffmap[index] || generateworldcliff(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
                ctx.rockmap[index] = generateworldrock(ctx, chunkx, chunky, x, y, ctx.heightmap[index]);
            }
        }
    }
    return !ctx.iscanceled();
}

static uchar &worldgensectionflags(worldgencontext &ctx, int blockx, int blocky, int blockz)
{
    const int tile = blocky / WORLD_SECTION_BLOCKS * WORLD_SECTION_COLUMNS + blockx / WORLD_SECTION_BLOCKS,
              section = clamp(blockz / WORLD_SECTION_BLOCKS, 0, int(WORLD_SECTION_LAYERS) - 1);
    return ctx.renderdata.flags[section][tile];
}

static void markworldgencarvedsection(worldgencontext &ctx, int blockx, int blocky, int blockz, bool entrance)
{
    static const int directions[][3] =
    {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };
    uchar &flags = worldgensectionflags(ctx, blockx, blocky, blockz);
    flags = (flags | SECTION_INTERIOR) & ~SECTION_FULLY_SOLID;
    if(entrance) flags |= SECTION_CAVE_ENTRANCE;
    loopi(6)
    {
        const int coordinate = i < 2 ? blockx : i < 4 ? blocky : blockz,
                  direction = directions[i][i / 2];
        if((direction < 0 && coordinate % WORLD_SECTION_BLOCKS) ||
           (direction > 0 && coordinate % WORLD_SECTION_BLOCKS != WORLD_SECTION_BLOCKS - 1))
            continue;
        const int neighborx = blockx + directions[i][0], neighbory = blocky + directions[i][1], neighborz = blockz + directions[i][2];
        if(neighborx < 0 || neighborx >= WORLD_CHUNK_BLOCKS || neighbory < 0 || neighbory >= WORLD_CHUNK_BLOCKS ||
           neighborz < 0 || neighborz >= WORLD_HEIGHT_BLOCKS)
            continue;
        uchar &neighborflags = worldgensectionflags(ctx, neighborx, neighbory, neighborz);
        neighborflags |= SECTION_INTERIOR;
        if(entrance) neighborflags |= SECTION_CAVE_ENTRANCE;
    }
}

static void markworldgenexteriorshell(worldgencontext &ctx, int chunkx, int chunky)
{
    static const int directions[][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    const int sealevel = clamp(ctx.settings.sealevel - WORLD_MIN_HEIGHT, 0, int(WORLD_HEIGHT_BLOCKS));
    ctx.renderdata.clear();
    loopi(WORLD_SECTION_LAYERS) loopj(WORLD_SECTION_TILES) ctx.renderdata.flags[i][j] = SECTION_FULLY_SOLID;

    loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
    {
        const int surface = clamp(ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE - WORLD_MIN_HEIGHT, 0,
                                  int(WORLD_HEIGHT_BLOCKS));
        if(surface > 0) worldgensectionflags(ctx, x, y, surface - 1) |= SECTION_EXTERIOR;

        const int tile = y / WORLD_SECTION_BLOCKS * WORLD_SECTION_COLUMNS + x / WORLD_SECTION_BLOCKS;
        loop(section, WORLD_SECTION_LAYERS)
        {
            const int sectiontop = (section + 1) * WORLD_SECTION_BLOCKS;
            if(surface < sectiontop) ctx.renderdata.flags[section][tile] &= ~SECTION_FULLY_SOLID;
        }

        if(surface < sealevel)
        {
            const int first = surface / WORLD_SECTION_BLOCKS,
                      last = (sealevel - 1) / WORLD_SECTION_BLOCKS;
            for(int section = first; section <= last; ++section) ctx.renderdata.flags[section][tile] |= SECTION_WATER;
        }

        loopi(4)
        {
            const int neighborx = x + directions[i][0], neighbory = y + directions[i][1];
            int neighborheight;
            if(neighborx >= 0 && neighborx < WORLD_CHUNK_BLOCKS && neighbory >= 0 && neighbory < WORLD_CHUNK_BLOCKS)
                neighborheight = ctx.heightmap[neighbory * WORLD_CHUNK_BLOCKS + neighborx];
            else neighborheight = generateworldheight(ctx, chunkx, chunky, neighborx, neighbory);
            const int neighborsurface = clamp(neighborheight / WORLD_BLOCK_SIZE - WORLD_MIN_HEIGHT, 0, int(WORLD_HEIGHT_BLOCKS));
            if(surface <= neighborsurface) continue;
            for(int section = neighborsurface / WORLD_SECTION_BLOCKS; section <= (surface - 1) / WORLD_SECTION_BLOCKS; ++section)
                ctx.renderdata.flags[section][tile] |= SECTION_EXTERIOR;
        }
    }
}

static int worldheight(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.heightmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static int worldbiome(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.biomemap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE];
}

static bool worldbeach(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.beachmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static bool worldrock(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.rockmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static bool worldcliff(const worldgencontext &ctx, int localx, int localy)
{
    return ctx.cliffmap[localy / WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS + localx / WORLD_BLOCK_SIZE] != 0;
}

static int worldcolumncubetype(const worldgencontext &ctx, int z, int size, int height, int biome, bool beachprofile, bool cliff, bool rock)
{
    const int surface = WORLD_GROUND_HEIGHT + height,
              watertop = WORLD_GROUND_HEIGHT + ctx.settings.sealevel * WORLD_BLOCK_SIZE,
              dirtbottom = surface - ctx.settings.soildepth * WORLD_BLOCK_SIZE,
              grassbottom = surface - WORLD_BLOCK_SIZE,
              beachmin = (ctx.settings.sealevel + min(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE,
              beachmax = (ctx.settings.sealevel + max(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE;
    const bool beach = beachprofile && height >= beachmin && height <= beachmax;

    if(z >= max(surface, watertop)) return WORLD_TERRAIN_EMPTY;
    if(surface < watertop && z >= surface && z + size <= watertop) return WORLD_TERRAIN_WATER;
    if(z + size <= dirtbottom) return ctx.cubetype("stone");
    if(cliff)
    {
        // Every exposed stair of the cliff belongs to the rock face. Normal
        // surface rules resume immediately behind this band, producing a grassy
        // plateau without grass caps scattered down the vertical wall.
        if(z >= dirtbottom && z + size <= surface) return ctx.cubetype("stone");
        return WORLD_TERRAIN_MIXED;
    }
    if(rock)
    {
        if(biome == game::WORLD_BIOME_SNOW && z >= grassbottom && z + size <= surface) return ctx.cubetype("snow");
        if(z >= dirtbottom && z + size <= surface) return ctx.cubetype("stone");
        return WORLD_TERRAIN_MIXED;
    }
    if(beach || biome == game::WORLD_BIOME_DESERT)
    {
        if(z >= dirtbottom && z + size <= surface) return ctx.cubetype("sand");
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

static bool worldtreegrowablesurface(const worldgencontext &ctx, int blockx, int blocky, int height, int biome)
{
    const int localx = blockx * WORLD_BLOCK_SIZE,
              localy = blocky * WORLD_BLOCK_SIZE,
              surfacez = WORLD_GROUND_HEIGHT + height - WORLD_BLOCK_SIZE,
              type = worldcolumncubetype(ctx, surfacez, WORLD_BLOCK_SIZE, height, biome,
                                         worldbeach(ctx, localx, localy),
                                         worldcliff(ctx, localx, localy),
                                         worldrock(ctx, localx, localy));
    return type == ctx.cubetype("grass") || type == ctx.cubetype("dirt");
}

static int worldcubetype(const worldgencontext &ctx, const ivec &o, int size)
{
    if(o.x >= WORLD_CHUNK_SIZE || o.y >= WORLD_CHUNK_SIZE || o.z >= WORLD_MAP_SIZE)
        return WORLD_TERRAIN_EMPTY;
    if(o.x + size > WORLD_CHUNK_SIZE || o.y + size > WORLD_CHUNK_SIZE || o.z + size > WORLD_MAP_SIZE)
        return WORLD_TERRAIN_MIXED;

    int type = WORLD_TERRAIN_UNSET;
    for(int y = o.y; y < o.y + size; y += WORLD_BLOCK_SIZE)
    for(int x = o.x; x < o.x + size; x += WORLD_BLOCK_SIZE)
    {
        int columntype = worldcolumncubetype(ctx, o.z, size, worldheight(ctx, x, y), worldbiome(ctx, x, y), worldbeach(ctx, x, y), worldcliff(ctx, x, y),
                                             worldrock(ctx, x, y));
        if(columntype == WORLD_TERRAIN_MIXED || (type != WORLD_TERRAIN_UNSET && type != columntype)) return WORLD_TERRAIN_MIXED;
        type = columntype;
    }
    return type;
}

static int worldrepresentativecubetype(const worldgencontext &ctx, const ivec &o, int size)
{
    const int x = clamp(o.x + size / 2, 0, WORLD_CHUNK_SIZE - 1),
              y = clamp(o.y + size / 2, 0, WORLD_CHUNK_SIZE - 1),
              height = worldheight(ctx, x, y),
              biome = worldbiome(ctx, x, y),
              surface = WORLD_GROUND_HEIGHT + height,
              watertop = WORLD_GROUND_HEIGHT + ctx.settings.sealevel * WORLD_BLOCK_SIZE,
              visibletop = max(surface, watertop);
    int z = clamp(o.z + size / 2, 0, WORLD_MAP_SIZE - 1);

    // A coarse cube intersecting the visible column top represents its
    // surface, not the greater volume underneath it. Sample immediately below
    // that top so grass/sand/snow/stone wins over dirt, and water wins for a
    // submerged terrain column. Cubes wholly underground retain the centre
    // sample used for their dominant interior material.
    if(visibletop > o.z && visibletop <= o.z + size)
        z = clamp(visibletop - 1, 0, WORLD_MAP_SIZE - 1);

    return worldcolumncubetype(ctx, z, 1, height, biome, worldbeach(ctx, x, y), worldcliff(ctx, x, y), worldrock(ctx, x, y));
}

static bool generateworldcube(worldgencontext &ctx, cube &c, const ivec &o, int size, int mingridsize)
{
    if(ctx.iscanceled()) return false;
    int type = worldcubetype(ctx, o, size);
    if(type == WORLD_TERRAIN_MIXED && size <= mingridsize) type = worldrepresentativecubetype(ctx, o, size);
    if(type == WORLD_TERRAIN_EMPTY)
    {
        setworldcubematerial(c, MAT_AIR);
        return true;
    }
    if(type == WORLD_TERRAIN_WATER)
    {
        setworldcubematerial(c, MAT_WATER);
        return true;
    }
    if(type >= 0 && ctx.cubetextures.inrange(type))
    {
        setworldcubetype(c, ctx, type);
        return true;
    }

    if(size <= mingridsize)
    {
        setworldcubematerial(c, MAT_AIR);
        return true;
    }

    c.children = allocworldgenfamily(ctx);
    const int childsize = size >> 1;
    loopi(8) if(!generateworldcube(ctx, c.children[i], ivec(i, o, childsize), childsize, mingridsize))
        return false;
    return true;
}

static uint hashworldtree(uint seed, int chunkx, int chunky, int blockx, int blocky, uint salt)
{
    const uint worldx = uint(chunkx) * uint(WORLD_CHUNK_BLOCKS) + uint(blockx),
               worldy = uint(chunky) * uint(WORLD_CHUNK_BLOCKS) + uint(blocky);
    uint hash = seed ^ salt;
    hash ^= worldx * 0x9E3779B9U;
    hash ^= worldy * 0x85EBCA6BU;
    hash ^= hash >> 16;
    hash *= 0x7FEB352DU;
    hash ^= hash >> 15;
    hash *= 0x846CA68BU;
    hash ^= hash >> 16;
    return hash;
}

static float worldtreeunit(uint hash)
{
    return float(hash & 0x00FFFFFFU) / float(0x01000000U);
}

static uint hashworldgrass(uint seed, uint worldx, uint worldy, uint salt)
{
    uint hash = seed ^ salt;
    hash ^= worldx * 0x9E3779B9U;
    hash ^= worldy * 0x85EBCA6BU;
    hash ^= hash >> 16;
    hash *= 0x7FEB352DU;
    hash ^= hash >> 15;
    hash *= 0x846CA68BU;
    hash ^= hash >> 16;
    return hash;
}

static void cacheworldscattertransform(int chunkx, int chunky, float maxoffset, const worldscatterinstance &scatter)
{
    if(scatter.rendertransformvalid && scatter.rendermaxoffset == maxoffset) return;

    const uint worldx = uint(chunkx * WORLD_CHUNK_BLOCKS + scatter.x / WORLD_BLOCK_SIZE),
               worldy = uint(chunky * WORLD_CHUNK_BLOCKS + scatter.y / WORLD_BLOCK_SIZE),
               seed = uint(game::getworldseed());

    scatter.renderyaw = int(worldtreeunit(hashworldgrass(seed, worldx, worldy, 0x63D83595U)) * 360.0f);

    const float angle = worldtreeunit(hashworldgrass(seed, worldx, worldy, 0xC2B2AE35U)) * 2.0f * M_PI,
                offsetunit = worldtreeunit(hashworldgrass(seed, worldx, worldy, 0x27D4EB2FU)),
                offset = maxoffset * WORLD_BLOCK_SIZE * offsetunit * offsetunit;

    scatter.renderoffsetx = cosf(angle) * offset;
    scatter.renderoffsety = sinf(angle) * offset;
    scatter.rendermaxoffset = maxoffset;
    scatter.rendertransformvalid = true;
}

static void cacheworldscattertransforms(int chunkx, int chunky, float maxoffset, const vector<worldscatterinstance> &scatter)
{
    loopv(scatter) cacheworldscattertransform(chunkx, chunky, maxoffset, scatter[i]);
}

struct worldgrasscollectcontext
{
    FastNoiseLite distribution, flowerdistribution[3];
    game::worldsettings settings;
    vector<worldscatterinstance> &scatter;
    int chunkx, chunky;
    uint seed;

    worldgrasscollectcontext(int chunkx, int chunky, const game::worldsettings &settings, vector<worldscatterinstance> &scatter)
        : settings(settings), scatter(scatter), chunkx(chunkx), chunky(chunky), seed(uint(game::getworldseed()))
    {
        distribution.SetSeed(game::getworldseed() ^ 0x6E624EB7);
        distribution.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        distribution.SetFrequency(settings.grassfrequency);
        distribution.SetFractalType(FastNoiseLite::FractalType_FBm);
        distribution.SetFractalOctaves(2);
        distribution.SetFractalLacunarity(1.8f);
        distribution.SetFractalGain(0.5f);

        static const uint flowersalts[3] =
        {
            0x9E21F4A7U, 0xC13FA9A9U, 0x91E10DA5U
        };
        loopi(3)
        {
            flowerdistribution[i].SetSeed(game::getworldseed() ^ flowersalts[i]);
            flowerdistribution[i].SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
            flowerdistribution[i].SetFrequency(settings.grassfrequency * 0.35f);
            flowerdistribution[i].SetFractalType(FastNoiseLite::FractalType_FBm);
            flowerdistribution[i].SetFractalOctaves(2);
            flowerdistribution[i].SetFractalLacunarity(1.8f);
            flowerdistribution[i].SetFractalGain(0.5f);
        }
    }
};

static const cube &lookupgeneratedworldcube(const cube *root, const ivec &pos)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    const cube *c = &root[octastep(pos.x, pos.y, pos.z, scale)];
    while(c->children)
    {
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    return *c;
}

static bool validgeneratedworldscatter(const cube *root, const worldscatterinstance &scatter)
{
    if(!root || scatter.x < 0 || scatter.x >= WORLD_CHUNK_SIZE ||
       scatter.y < 0 || scatter.y >= WORLD_CHUNK_SIZE ||
       scatter.z < 0 || scatter.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE ||
       scatter.type < 0 || scatter.type >= numworldscatters() ||
       scatter.orient < O_LEFT || scatter.orient > O_TOP)
        return false;

    const ivec center(scatter.x + WORLD_BLOCK_SIZE / 2, scatter.y + WORLD_BLOCK_SIZE / 2, scatter.z + WORLD_BLOCK_SIZE / 2);
    const cube &occupied = lookupgeneratedworldcube(root, center);
    if(!isempty(occupied) || occupied.material != MAT_AIR) return false;

    const bool placeable = isworldplaceable(scatter.type);

    if((!placeable && scatter.orient != O_TOP) || (placeable && scatter.orient == O_BOTTOM))
        return false;

    const ivec supportcenter = ivec(center).sub(ivec(worldgenorientnormal(scatter.orient)).mul(WORLD_BLOCK_SIZE));

    // An edge-mounted torch can be owned by the neighboring chunk. Its support
    // is checked once both chunks are mounted in the runtime world.
    if(supportcenter.x < 0 || supportcenter.x >= WORLD_CHUNK_SIZE || supportcenter.y < 0 || supportcenter.y >= WORLD_CHUNK_SIZE)
        return placeable;

    const cube &support = lookupgeneratedworldcube(root, supportcenter);
    if(isempty(support) || !isentirelysolid(support) || support.material != MAT_AIR) return false;
    return true;
}

static bool worldflowerspaced(const worldgrasscollectcontext &ctx, uint worldx, uint worldy, int flower)
{
    static const uint spacingsalts[3] =
    {
        0xD1B54A35U, 0x94D049BBU, 0x369DEA0FU
    };
    const uint priority = hashworldgrass(ctx.seed, worldx, worldy, spacingsalts[flower]);
    for(int oy = -1; oy <= 1; ++oy) for(int ox = -1; ox <= 1; ++ox)
    {
        if(!ox && !oy) continue;
        const uint other = hashworldgrass(ctx.seed, worldx + ox, worldy + oy, spacingsalts[flower]);
        if(other < priority || (other == priority && (oy < 0 || (!oy && ox < 0)))) return false;
    }
    return true;
}

static int chooseworldflower(worldgrasscollectcontext &ctx, float noisex, float noisey, uint worldx, uint worldy)
{
    const float weights[3] =
    {
        worldrosescatter >= 0 ? max(ctx.settings.roseweight, 0.0f) : 0.0f,
        worldtulipscatter >= 0 ? max(ctx.settings.tulipweight, 0.0f) : 0.0f,
        worlddandelionscatter >= 0 ? max(ctx.settings.dandelionweight, 0.0f) : 0.0f
    };
    const int types[3] =
    {
        worldrosescatter, worldtulipscatter, worlddandelionscatter
    };
    const float weightsum = weights[0] + weights[1] + weights[2];
    if(ctx.settings.flowerchance <= 0 || weightsum <= 0) return -1;

    static const uint chancesalts[3] =
    {
        0xDB4F0B91U, 0xBBE05633U, 0xA0F2EC75U
    };
    static const uint choicesalts[3] =
    {
        0x89E18285U, 0xC6BC2796U, 0xCA01F9DDU
    };
    int selected = -1;
    float selectedscore = -1;
    loopi(3)
    {
        if(weights[i] <= 0) continue;

        const float noise = clamp(ctx.flowerdistribution[i].GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    patch = worldsmoothstep(0.48f, 0.72f, noise),
                    chance = clamp(ctx.settings.flowerchance * (weights[i] / weightsum) * (0.05f + 4.95f * patch * patch), 0.0f, 1.0f);

        if(worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy, chancesalts[i])) >= chance || !worldflowerspaced(ctx, worldx, worldy, i))
            continue;

        const float score = patch + worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy, choicesalts[i])) * 0.05f;

        if(score > selectedscore)
        {
            selected = types[i];
            selectedscore = score;
        }
    }
    return selected;
}

static void collectworldgrassnode(worldgrasscollectcontext &ctx, const cube &c, const cube *root, const ivec &o, int size, int surfacetexture)
{
    if(o.z >= WORLD_MAP_SIZE || o.x >= WORLD_CHUNK_SIZE || o.y >= WORLD_CHUNK_SIZE)
        return;

    if(c.children)
    {
        const int childsize = size >> 1;
        loopi(8)
            collectworldgrassnode(ctx, c.children[i], root, ivec(i, o, childsize), childsize, surfacetexture);
        return;
    }

    if(size < WORLD_BLOCK_SIZE || isempty(c) || !isentirelysolid(c) || c.material != MAT_AIR || c.texture[O_TOP] != surfacetexture)
        return;

    const int top = o.z + size;
    if(top >= WORLD_MAP_SIZE) return;

    const int startx = max(o.x, 0), starty = max(o.y, 0),
              endx = min(o.x + size, int(WORLD_CHUNK_SIZE)),
              endy = min(o.y + size, int(WORLD_CHUNK_SIZE));

    for(int y = starty; y < endy; y += WORLD_BLOCK_SIZE)
    for(int x = startx; x < endx; x += WORLD_BLOCK_SIZE)
    {
        const cube &above = lookupgeneratedworldcube(root, ivec(x + WORLD_BLOCK_SIZE / 2, y + WORLD_BLOCK_SIZE / 2, top));
        if(!isempty(above) || above.material != MAT_AIR) continue;

        const int blockx = ctx.chunkx * WORLD_CHUNK_BLOCKS + x / WORLD_BLOCK_SIZE,
                  blocky = ctx.chunky * WORLD_CHUNK_BLOCKS + y / WORLD_BLOCK_SIZE;
        const uint worldx = uint(blockx), worldy = uint(blocky);
        const float noisex = float(blockx) + 0.5f,
                    noisey = float(blocky) + 0.5f;
        int type = chooseworldflower(ctx, noisex, noisey, worldx, worldy);

        if(type < 0)
        {
            if(worldgrassscatter < 0) continue;
            const float noise = clamp(ctx.distribution.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                        patch = worldsmoothstep(0.2f, 0.8f, noise),
                        density = clamp(ctx.settings.grassdensity * (0.12f + 1.88f * patch * patch), 0.0f, 1.0f);
            if(worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy, 0xA511E9B3U)) >= density) continue;
            type = worldgrassscatter;
        }

        worldscatterinstance &scatter = ctx.scatter.add(worldscatterinstance(x, y, top, type));
        cacheworldscattertransform(ctx.chunkx, ctx.chunky, ctx.settings.grassmaxoffset, scatter);
    }
}

static void generateworldscatter(cube *root, int chunkx, int chunky, const game::worldsettings &settings, vector<worldscatterinstance> &scatter)
{
    scatter.setsize(0);

    if(!root || (worldgrassscatter < 0 && worldrosescatter < 0 && worldtulipscatter < 0 && worlddandelionscatter < 0))
        return;

    const bool grass = worldgrassscatter >= 0 && settings.grassdensity > 0,
               flowers = settings.flowerchance > 0 && ((worldrosescatter >= 0 && settings.roseweight > 0) || (worldtulipscatter >= 0 && settings.tulipweight > 0) || (worlddandelionscatter >= 0 && settings.dandelionweight > 0));

    if(!grass && !flowers) return;

    worlddefinition *surface = findworldcube("grass");
    if(!surface && worldcubedefinitions.inrange(worlderrorcube)) surface = worldcubedefinitions[worlderrorcube];
    worldgrasscollectcontext ctx(chunkx, chunky, settings, scatter);

    loopi(8) collectworldgrassnode(ctx, root[i], root, ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE), WORLD_CHUNK_ROOT_SIZE, surface->slot);
}

static void addworldtreeblock(vector<ivec> &blocks, int blockx, int blocky, int blockz)
{
    if(blockx < 0 || blockx >= WORLD_CHUNK_BLOCKS || blocky < 0 || blocky >= WORLD_CHUNK_BLOCKS || blockz < 0 || blockz >= WORLD_HEIGHT_BLOCKS)
        return;

    blocks.add(ivec(blockx * WORLD_BLOCK_SIZE, blocky * WORLD_BLOCK_SIZE, blockz * WORLD_BLOCK_SIZE));
}

static void addworldregulartree(vector<ivec> &wood, vector<ivec> &leaves, int blockx, int blocky, int basez, int height, uint shapehash)
{
    loop(z, height) addworldtreeblock(wood, blockx, blocky, basez + z);

    for(int z = height - 2; z <= height; ++z)
    {
        const int radius = z == height ? 1 : 2;
        for(int y = -radius; y <= radius; ++y) for(int x = -radius; x <= radius; ++x)
        {
            if(radius == 2 && abs(x) == 2 && abs(y) == 2 && (hashworldtree(shapehash, x, y, z, height, 0xA511E9B3U) & 1U)) continue;
            addworldtreeblock(leaves, blockx + x, blocky + y, basez + z);
        }
    }
}

static void addworldpinetree(vector<ivec> &pinewood, vector<ivec> &needles, int blockx, int blocky, int basez, int height)
{
    loop(z, height) addworldtreeblock(pinewood, blockx, blocky, basez + z);
    addworldtreeblock(needles, blockx, blocky, basez + height);

    for(int z = 2; z < height; ++z)
    {
        const int fromtop = height - z, radius = min(3, 1 + fromtop / 3);
        for(int y = -radius; y <= radius; ++y) for(int x = -radius; x <= radius; ++x)
        {
            if(abs(x) + abs(y) > radius + 1) continue;
            addworldtreeblock(needles, blockx + x, blocky + y, basez + z);
        }
    }
}

struct worldtreecandidate
{
    int blockx, blocky, worldx, worldy, basez, height;
    uint priority, shape;
    bool pine;

    worldtreecandidate(int blockx, int blocky, int worldx, int worldy, int basez, int height, uint priority, uint shape, bool pine)
        : blockx(blockx), blocky(blocky), worldx(worldx), worldy(worldy), basez(basez), height(height),
          priority(priority), shape(shape), pine(pine)
    {
    }
};

static bool worldtreecandidateallowed(const vector<worldtreecandidate> &candidates, const worldtreecandidate &candidate)
{
    loopv(candidates)
    {
        const worldtreecandidate &other = candidates[i];
        if(other.blockx == candidate.blockx && other.blocky == candidate.blocky) continue;
        if(abs(other.worldx - candidate.worldx) > 1 || abs(other.worldy - candidate.worldy) > 1) continue;
        const bool lowerpriority = other.priority < candidate.priority;
        const bool samepriority = other.priority == candidate.priority;
        const bool lowery = other.worldy < candidate.worldy;
        const bool samey = other.worldy == candidate.worldy;
        const bool lowerx = other.worldx < candidate.worldx;

        if(lowerpriority || (samepriority && (lowery || (samey && lowerx)))) return false;
    }
    return true;
}

static void subdivideworldgencube(worldgencontext &ctx, cube &c)
{
    if(c.children) return;
    cube parent = c;
    c.children = allocworldgenfamily(ctx);
    loopi(8)
    {
        c.children[i] = parent;
        c.children[i].children = NULL;
        c.children[i].ext = NULL;
        c.children[i].visible = 0;
        c.children[i].merged = 0;
    }
}

static cube &lookupworldgenblock(worldgencontext &ctx, cube *root, const ivec &position)
{
    cube *family = root;
    ivec origin(0, 0, 0);
    int size = WORLD_CHUNK_ROOT_SIZE;
    for(;;)
    {
        const int index = (position.x >= origin.x + size ? 1 : 0) | (position.y >= origin.y + size ? 2 : 0) | (position.z >= origin.z + size ? 4 : 0);
        cube &c = family[index];
        if(size == WORLD_BLOCK_SIZE) return c;
        subdivideworldgencube(ctx, c);
        origin = ivec(index, origin, size);
        family = c.children;
        size >>= 1;
    }
}

static void markworldgentreeblock(worldgencontext &ctx, const ivec &position)
{
    uchar &flags = worldgensectionflags(ctx, position.x / WORLD_BLOCK_SIZE, position.y / WORLD_BLOCK_SIZE, position.z / WORLD_BLOCK_SIZE);
    flags = (flags | SECTION_EXTERIOR) & ~(SECTION_FULLY_SOLID | SECTION_NO_RENDER);
}

enum
{
    WORLD_CARVE_NONE = 0,
    WORLD_CARVE_AIR = 1 << 0,
    WORLD_CARVE_LAVA = 1 << 1,
    WORLD_CARVE_ENTRANCE = 1 << 2,
    WORLD_CARVE_TYPE = WORLD_CARVE_AIR | WORLD_CARVE_LAVA
};

static int worldcarveindex(int x, int y, int blockz)
{
    return (blockz * WORLD_CHUNK_BLOCKS + y) * WORLD_CHUNK_BLOCKS + x;
}

static uint mixworldfeaturehash(uint hash, uint value)
{
    hash ^= value + 0x9E3779B9U + (hash << 6) + (hash >> 2);
    hash ^= hash >> 16;
    hash *= 0x7FEB352DU;
    hash ^= hash >> 15;
    return hash;
}

static uint hashworldfeature(uint seed, long long x, long long y, int z, uint salt)
{
    const unsigned long long ux = (unsigned long long)x,
                             uy = (unsigned long long)y;
    uint hash = seed ^ salt;
    hash = mixworldfeaturehash(hash, uint(ux));
    hash = mixworldfeaturehash(hash, uint(ux >> 32));
    hash = mixworldfeaturehash(hash, uint(uy));
    hash = mixworldfeaturehash(hash, uint(uy >> 32));
    return mixworldfeaturehash(hash, uint(z));
}

struct worldoredefinition
{
    const char *id;
    int minheight, maxheight, optimalminheight, optimalmaxheight;
    int mindepth, maxdepth, minvein, maxvein, rareminvein, raremaxvein, cellsize;
    float chance, geologicalbonus;
    uint salt;
    bool uniformdistribution;
};

static const worldoredefinition worldores[] =
{
    // Elevation and depth values are in world blocks relative to sea level.
    { "coal_ore",      -112,  200,  -32,   64,   4, 110,  8, 28, 40, 80, 12, 1.05f, 1.6f, 0x4A1D3B27U, false },
    { "copper_ore",    -128,  128,  -48,   32,  10, 130,  5, 16,  0,  0, 14, 0.90f, 1.5f, 0x7C3E91A5U, false },
    { "iron_ore",      -192,  160,  -96,   16,  12, 180,  6, 20,  0,  0, 14, 0.85f, 1.7f, 0xB6A54D19U, false },
    { "tin_ore",       -176,   64, -112,  -48,  25, 170,  3,  9,  0,  0, 16, 0.80f, 1.4f, 0xD82F6043U, false },
    { "gold_ore",      -224,  -32, -168, -112,  60, WORLD_HEIGHT_BLOCKS, 2,  7,  0,  0, 20, 0.70f, 1.8f, 0xE91B72C5U, false },
    { "diamond_ore",   -248, -136, -232, -200, 120, WORLD_HEIGHT_BLOCKS, 1,  4,  0,  0, 24, 0.55f, 1.25f, 0xF05A8C31U, false },
    { "moon_dust_ore", WORLD_MIN_HEIGHT, WORLD_MAX_HEIGHT - 1, WORLD_MIN_HEIGHT, WORLD_MAX_HEIGHT - 1, 0, WORLD_HEIGHT_BLOCKS, 1, 2, 1, 3, 8, 1.2f,
      1.0f, 0x2C7E4B91U, true }
};

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
    const float relief = clamp(tectonics.terrainroughness, 0.0f, 1.0f),
                mountain = worldsmoothstep(0.45f, 0.75f, relief),
                hill = worldsmoothstep(0.12f, 0.45f, relief),
                deep = 1.0f - worldsmoothstep(-180.0f, -80.0f, float(elevation)),
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

static long long worldfloordiv(long long value, int divisor)
{
    long long quotient = value / divisor;
    if(value < 0 && value % divisor) --quotient;
    return quotient;
}

enum
{
    // A halo wider than the longest worm plus its largest chamber makes clipping seamless at chunk edges.
    WORLD_CAVE_REGION_SIZE = 144,
    WORLD_CAVE_REGION_HALO = 400
};

struct worldcaverandom
{
    uint state;

    worldcaverandom(uint seed) : state(seed ? seed : 0xA341316CU) {}

    uint next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float unit() { return float(next() & 0x00FFFFFFU) / float(0x01000000U); }
    int range(int low, int high) { return low + int(next() % uint(max(high - low + 1, 1))); }
};

struct worldcaveanchor
{
    vec position;
    int worm;

    worldcaveanchor(const vec &position, int worm) : position(position), worm(worm) {}
};

static float worldcaveradius(worldcaverandom &random)
{
    // Target durations are independent of category, so these are also the long-run passage proportions.
    const float category = random.unit();
    if(category < 0.20f) return 2.0f + random.unit();
    if(category < 0.75f) return 3.0f + random.unit() * 3.0f;
    if(category < 0.95f) return 6.0f + random.unit() * 4.0f;
    return 10.0f + random.unit() * 10.0f;
}

static void addworldcaveworm(const worldgencontext &ctx, worldcaverandom &random, vector<worldcavesegment> &segments,
                             vector<worldcaveanchor> &anchors, const vec &origin, float yaw, float pitch, int steps, int worm)
{
    const int mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth),
              bottom = WORLD_MIN_HEIGHT + clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)) + 3;

    vec position(origin);

    const float initialdeepness = worldsmoothstep(180.0f, 228.0f, -origin.z);
    float radius = min(3.0f + random.unit() * 3.0f, 4.25f - initialdeepness * 1.75f), targetradius = radius,
          turnrate = (random.unit() < 0.5f ? -1.0f : 1.0f) * (0.14f + random.unit() * 0.25f);
    int radiussteps = 1, turnsteps = random.range(1, 3);

    anchors.add(worldcaveanchor(position, worm));

    loopi(steps)
    {
        const float deepness = worldsmoothstep(180.0f, 228.0f, -position.z);
        if(--turnsteps <= 0)
        {
            turnrate = (random.unit() < 0.5f ? -1.0f : 1.0f) * (0.14f + random.unit() * 0.29f);
            turnsteps = random.range(1, 3);
        }
        yaw += turnrate * (1.0f + deepness * 0.90f) + (random.unit() - 0.5f) * (0.24f + deepness * 0.22f);
        if(random.unit() < 0.17f + deepness * 0.15f)
        {
            yaw += (random.unit() < 0.5f ? -1.0f : 1.0f) * (0.48f + random.unit() * 0.82f);
            turnrate = -turnrate * (0.55f + random.unit() * 0.35f);
            turnsteps = random.range(1, 3);
        }
        pitch = pitch * (0.68f - deepness * 0.12f) + (random.unit() - 0.5f) * (0.32f + deepness * 0.24f);
        if(random.unit() < 0.10f + deepness * 0.08f) pitch += (random.unit() < 0.72f ? -1.0f : 1.0f) * (0.38f + random.unit() * (0.44f + deepness * 0.20f));
        pitch = clamp(pitch, -0.85f, 0.70f);

        if(--radiussteps <= 0)
        {
            targetradius = worldcaveradius(random);
            radiussteps = random.range(2, 6);
        }
        const float unrestrictedradius = radius + clamp(targetradius - radius, -3.0f, 3.0f),
                    deepcap = 4.25f - deepness * 1.75f,
                    nextradius = min(unrestrictedradius, deepcap),
                    steplength = (3.5f + random.unit() * 3.0f) * (1.0f - deepness * 0.34f),
                    horizontal = cosf(pitch) * steplength;
        vec next(position.x + cosf(yaw) * horizontal, position.y + sinf(yaw) * horizontal, position.z + sinf(pitch) * steplength);
        const int surface = ctx.generator.height(int(floorf(next.x)), int(floorf(next.y))),
                  ceiling = surface - max(mindepth, int(ceilf(nextradius))) - 1;
        next.z = clamp(next.z, float(bottom), float(max(ceiling, bottom)));

        const float verticalscale = 0.52f - deepness * 0.09f + random.unit() * (0.83f - deepness * 0.19f);
        segments.add(worldcavesegment(position, next, radius, nextradius, verticalscale, random.next()));
        position = next;
        radius = nextradius;
        if(i % 3 == 2 || i == steps - 1) anchors.add(worldcaveanchor(position, worm));
    }
}

static void addworldcaveconnection(worldcaverandom &random, vector<worldcavesegment> &segments, const vec &start, const vec &end, float startradius, float endradius, bool entrance = false)
{
    const float deepness = worldsmoothstep(180.0f, 228.0f, -min(start.z, end.z));
    const float dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z,
                distance = sqrtf(dx * dx + dy * dy + dz * dz), horizontal = max(sqrtf(dx * dx + dy * dy), 0.001f),
                direction = random.unit() < 0.5f ? -1.0f : 1.0f,
                curve = direction * min(distance * (0.15f + random.unit() * 0.13f + deepness * 0.05f), 28.0f + deepness * 8.0f),
                wiggle = -direction * min(distance * (0.05f + random.unit() * 0.08f + deepness * 0.04f), 12.0f + deepness * 6.0f),
                verticalcurve = (random.unit() - 0.62f) * min(distance * 0.14f, 15.0f);
    const float smallwiggle = direction * min(distance * (0.025f + random.unit() * 0.045f + deepness * 0.035f), 7.0f + deepness * 5.0f);
    const int steps = max(int(ceilf(distance / (5.5f - deepness * 1.8f))), 3);
    vec previous(start);
    float previousradius = startradius;
    for(int i = 1; i <= steps; ++i)
    {
        const float amount = i / float(steps),
                    lateral = sinf(amount * M_PI) * curve + sinf(amount * 2.0f * M_PI) * wiggle +
                              sinf(amount * 3.0f * M_PI) * smallwiggle,
                    vertical = sinf(amount * M_PI) * verticalcurve,
                    radius = startradius + (endradius - startradius) * amount;
        const vec next(start.x + dx * amount - dy / horizontal * lateral,
                       start.y + dy * amount + dx / horizontal * lateral,
                       start.z + dz * amount + vertical);
        segments.add(worldcavesegment(previous, next, previousradius, radius, 0.58f + random.unit() * 0.70f, random.next(), entrance));
        previous = next;
        previousradius = radius;
    }
}

static void addworldcavedeepdescent(const worldgencontext &ctx, worldcaverandom &random, vector<worldcavesegment> &segments, vector<worldcaveanchor> &anchors, const vec &start, int worm)
{
    const int bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS));
    const float lavaceiling = WORLD_MIN_HEIGHT + bottomlayers + 0.5f,
                verticaldistance = max(start.z - lavaceiling, 1.0f);
    const int levels = clamp(int(ceilf(verticaldistance / 44.0f)), 3, 9);
    vec previous(start);
    float previousradius = min(3.2f + random.unit() * 1.4f, 4.2f), angle = random.unit() * 2.0f * M_PI;

    anchors.add(worldcaveanchor(previous, worm));

    for(int i = 1; i <= levels; ++i)
    {
        const float amount = i / float(levels),
                    deepness = worldsmoothstep(0.45f, 1.0f, amount),
                    horizontal = i == levels ? random.unit() * 10.0f : 13.0f + random.unit() * (25.0f - deepness * 8.0f),
                    radius = 3.2f + (1.65f - 3.2f) * deepness + random.unit() * (0.65f - deepness * 0.25f);

        angle += (random.unit() < 0.5f ? -1.0f : 1.0f) * (0.55f + random.unit() * 1.35f);

        const vec next(start.x + cosf(angle) * horizontal,
                       start.y + sinf(angle) * horizontal,
                       start.z + (lavaceiling - start.z) * amount);

        addworldcaveconnection(random, segments, previous, next, previousradius, radius);
        previous = next;
        previousradius = radius;
        anchors.add(worldcaveanchor(previous, worm));
    }
}

static void addworldcavechamber(worldcaverandom &random, vector<worldcavechamber> &chambers, const vec &attachment)
{
    const float category = random.unit(),
                generatedradius = category < 0.78f ? 5.0f + random.unit() * 5.0f
                                : category < 0.97f ? 10.0f + random.unit() * 10.0f
                                                   : 20.0f + random.unit() * 15.0f,
                deepness = worldsmoothstep(180.0f, 228.0f, -attachment.z),
                baseradius = min(generatedradius, 6.0f - deepness * 2.5f);
    const int lobes = random.range(3, category >= 0.97f ? 7 : 5);
    const float mainangle = random.unit() * 2.0f * M_PI;

    loopi(lobes)
    {
        const float offsetangle = mainangle + (random.unit() - 0.5f) * M_PI,
                    offsetdistance = i ? baseradius * random.unit() * 0.32f : 0.0f,
                    verticaloffset = i ? (random.unit() - 0.5f) * baseradius * 0.30f : 0.0f,
                    radiusx = baseradius * (0.66f + random.unit() * 0.34f),
                    radiusy = baseradius * (0.55f + random.unit() * 0.43f),
                    radiusz = baseradius * (0.35f + random.unit() * 0.34f),
                    angle = mainangle + (random.unit() - 0.5f) * 0.8f;
        const vec center(attachment.x + cosf(offsetangle) * offsetdistance,
                         attachment.y + sinf(offsetangle) * offsetdistance,
                         attachment.z + verticaloffset);

        chambers.add(worldcavechamber(center, radiusx, radiusy, radiusz, angle, random.next()));
    }
}

static bool generateworldcavesystem(const worldgencontext &ctx, long long regionx, long long regiony, vector<worldcavesegment> &segments, vector<worldcavechamber> &chambers)
{
    // Neighboring regions share a density class, producing compact quiet areas and dense labyrinth clusters.
    const long long clusterx = worldfloordiv(regionx, 2), clustery = worldfloordiv(regiony, 2);
    const float cluster = worldtreeunit(hashworldfeature(uint(ctx.seed), clusterx, clustery, 0, 0x6E624EB7U));
    const int density = cluster < 0.14f ? 0 : cluster < 0.68f ? 1 : 2;
    const float systemchance = density == 0 ? 0.56f : density == 1 ? 0.84f : 0.99f;
    const uint systemhash = hashworldfeature(uint(ctx.seed), regionx, regiony, density, 0xB5297A4DU);

    if(worldtreeunit(systemhash) >= systemchance) return false;

    worldcaverandom random(systemhash ^ 0x68E31DA4U);
    const float originx = float(regionx * WORLD_CAVE_REGION_SIZE + 24) + random.unit() * (WORLD_CAVE_REGION_SIZE - 48),
                originy = float(regiony * WORLD_CAVE_REGION_SIZE + 24) + random.unit() * (WORLD_CAVE_REGION_SIZE - 48);
    const int surface = ctx.generator.height(int(floorf(originx)), int(floorf(originy))),
              bottom = WORLD_MIN_HEIGHT + clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)) + 8,
              mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth),
              depth = random.range(28, density == 2 ? 105 : 82) + (random.unit() < 0.18f ? random.range(35, 90) : 0),
              originz = clamp(surface - depth, bottom, surface - mindepth - 8);

    if(originz <= bottom && surface - bottom < mindepth + 12) return false;

    vector<worldcaveanchor> anchors;
    const vec origin(originx, originy, float(originz));
    const int primaryworms = random.range(density == 0 ? 3 : 4, density == 2 ? 9 : density == 1 ? 7 : 5);
    int worm = 0;

    loopi(primaryworms)
    {
        const float yaw = random.unit() * 2.0f * M_PI,
                    pitch = (random.unit() - 0.58f) * (random.unit() < 0.16f ? 0.85f : 0.25f);
        const int steps = random.range(density == 0 ? 18 : density == 1 ? 21 : 24, density == 0 ? 26 : density == 1 ? 30 : 34);
        addworldcaveworm(ctx, random, segments, anchors, origin, yaw, pitch, steps, worm++);
        if(random.unit() < 0.24f) addworldcavechamber(random, chambers, anchors.last().position);
    }
    if(random.unit() < 0.52f) addworldcavechamber(random, chambers, origin);

    const int junctions = random.range(density == 0 ? 1 : 3, density == 2 ? 8 : density == 1 ? 5 : 3);
    loopi(junctions)
    {
        if(!anchors.length()) break;
        vec attachment(origin);
        loopj(8)
        {
            const vec &candidate = anchors[random.next() % uint(anchors.length())].position;
            const float dx = candidate.x - origin.x, dy = candidate.y - origin.y, dz = candidate.z - origin.z;
            if(dx * dx + dy * dy + dz * dz <= 145.0f * 145.0f)
            {
                attachment = candidate;
                break;
            }
        }
        const float baseyaw = random.unit() * 2.0f * M_PI;
        int forks = random.unit() < 0.30f ? 2 : 1;
        if(density == 2 && random.unit() < 0.10f) forks = 3;
        loopj(forks)
        {
            const float yaw = baseyaw + j * 2.0f * M_PI / forks + (random.unit() - 0.5f) * 0.45f,
                        pitch = (random.unit() - 0.62f) * 0.62f;
            addworldcaveworm(ctx, random, segments, anchors, attachment, yaw, pitch, random.range(9, 18), worm++);
            if(random.unit() < 0.18f) addworldcavechamber(random, chambers, anchors.last().position);
        }
    }

    int connections = density == 0 ? 2 : density == 1 ? 4 : 6;
    // These cross-worm links turn the origin's branching tree into a graph while leaving unselected ends intact.
    for(int attempt = 0; attempt < 64 && connections > 0 && anchors.length() >= 2; ++attempt)
    {
        const worldcaveanchor &from = anchors[random.next() % uint(anchors.length())],
                              &to = anchors[random.next() % uint(anchors.length())];
        if(from.worm == to.worm) continue;
        const float dx = to.position.x - from.position.x,
                    dy = to.position.y - from.position.y,
                    dz = to.position.z - from.position.z,
                    distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if(distance < 28.0f || distance > 155.0f || fabsf(dz) > 72.0f) continue;

        const float radius = random.unit() < 0.18f ? 7.0f + random.unit() * 6.0f : 3.5f + random.unit() * 3.5f;
        addworldcaveconnection(random, segments, from.position, to.position, radius * 0.85f, radius * 0.85f);
        --connections;
    }

    const game::worldtectonicsample terrain = ctx.generator.tectonics(int(floorf(originx)), int(floorf(originy)));
    const float hillweight = worldsmoothstep(0.12f, 0.68f, terrain.terrainroughness),
                entrancechance = 0.48f + hillweight * 0.49f;
    int entrances = random.unit() < entrancechance ? 1 : 0;
    if(entrances && density == 2 && random.unit() < hillweight * 0.42f) ++entrances;
    loopi(entrances)
    {
        const worldcaveanchor &attachment = anchors[random.next() % uint(anchors.length())];
        const float angle = random.unit() * 2.0f * M_PI, distance = 10.0f + random.unit() * 34.0f,
                    entrancex = originx + cosf(angle) * distance, entrancey = originy + sinf(angle) * distance;
        const int entranceheight = ctx.generator.height(int(floorf(entrancex)), int(floorf(entrancey)));
        if(entranceheight <= ctx.settings.sealevel + 2) continue;

        const vec entrance(entrancex, entrancey, entranceheight - 0.25f);
        const float radius = 3.0f + random.unit() * 2.5f;
        addworldcaveconnection(random, segments, attachment.position, entrance, radius * 1.25f, max(radius * 0.72f, 2.2f), true);
    }

    const int descents = 1 + (density == 2 && random.unit() < 0.42f ? 1 : 0);
    loopi(descents)
    {
        vec descentstart(origin);
        if(i == 0)
        {
            loopj(anchors.length()) if(anchors[j].position.z < descentstart.z) descentstart = anchors[j].position;
        }
        else loopj(12)
        {
            const vec &candidate = anchors[random.next() % uint(anchors.length())].position;
            if(candidate.z < descentstart.z) descentstart = candidate;
        }

        vector<worldcaveanchor> deepanchors;
        addworldcavedeepdescent(ctx, random, segments, deepanchors, descentstart, worm++);
        const int descentanchors = deepanchors.length();
        const vec descentend = deepanchors.last().position;
        const int deepbranches = random.range(density == 0 ? 2 : 3, density == 2 ? 5 : density == 1 ? 4 : 3);
        loopj(deepbranches)
        {
            vec attachment(descentend);
            loopk(8)
            {
                const vec &candidate = deepanchors[random.next() % uint(descentanchors)].position;
                if(candidate.z <= -190.0f)
                {
                    attachment = candidate;
                    break;
                }
            }
            const float yaw = random.unit() * 2.0f * M_PI, pitch = (random.unit() - 0.58f) * 0.48f;
            addworldcaveworm(ctx, random, segments, deepanchors, attachment, yaw, pitch, random.range(10, 16), worm++);
            if(random.unit() < 0.12f) addworldcavechamber(random, chambers, deepanchors.last().position);
        }
    }
    return true;
}

static float worldcavesegmentradius(const worldgencontext &ctx, const worldcavesegment &segment, float x, float y, float z, float radius)
{
    // Cheese fields may perturb an existing wall, but can never establish the cave's primary route.
    const float offsetx = float(segment.roughness & 0xFFU) * 7.0f,
                offsety = float((segment.roughness >> 8) & 0xFFU) * 7.0f,
                offsetz = float((segment.roughness >> 16) & 0xFFU) * 3.0f,
                coarse = ctx.generator.caves.GetNoise(x + offsetx, y - offsety, z + offsetz),
                fine = ctx.generator.caves.GetNoise(x * 1.85f - offsety, y * 1.85f + offsetx, z * 1.55f - offsetz),
                roughness = coarse * 0.62f + fine * 0.38f,
                secondary = ctx.generator.largecaves.GetNoise(x - offsety, y + offsetx, z - offsetz),
                widening = max(secondary - max(ctx.settings.largecavethreshold, 0.72f), 0.0f) * 7.0f;
    float carvedradius = max(radius + roughness * min(radius * 0.28f, 2.20f) + widening, 1.35f);
    if(z < -196.0f) carvedradius = min(carvedradius, max(4.0f - (-z - 196.0f) * 0.075f, 2.35f));
    return carvedradius;
}

static bool worldcavesegmentcontains(const worldgencontext &ctx, const worldcavesegment &segment, float x, float y, float z)
{
    const float scale = max(segment.verticalscale, 0.1f),
                ax = segment.start.x, ay = segment.start.y, az = segment.start.z / scale,
                bx = segment.end.x, by = segment.end.y, bz = segment.end.z / scale,
                px = x, py = y, pz = z / scale,
                dx = bx - ax, dy = by - ay, dz = bz - az,
                length2 = dx * dx + dy * dy + dz * dz;
    const float amount = length2 > 0.0001f ? clamp(((px - ax) * dx + (py - ay) * dy + (pz - az) * dz) / length2, 0.0f, 1.0f) : 0.0f,
                centerx = ax + dx * amount, centery = ay + dy * amount, centerz = az + dz * amount,
                baseradius = segment.startradius + (segment.endradius - segment.startradius) * amount,
                distancex = px - centerx, distancey = py - centery, distancez = pz - centerz,
                distance2 = distancex * distancex + distancey * distancey + distancez * distancez,
                maximumradius = baseradius + 4.2f;
    if(distance2 > maximumradius * maximumradius) return false;
    const float radius = worldcavesegmentradius(ctx, segment, x, y, z, baseradius);
    return distance2 <= radius * radius;
}

static bool worldcavechambercontains(const worldgencontext &ctx, const worldcavechamber &chamber, float x, float y, float z)
{
    const float dx = x - chamber.center.x, dy = y - chamber.center.y,
                rotatedx = dx * chamber.anglecos + dy * chamber.anglesin,
                rotatedy = -dx * chamber.anglesin + dy * chamber.anglecos,
                dz = z - chamber.center.z,
                ellipsoiddistance = rotatedx * rotatedx / (chamber.radiusx * chamber.radiusx) +
                                    rotatedy * rotatedy / (chamber.radiusy * chamber.radiusy) + dz * dz / (chamber.radiusz * chamber.radiusz);
    if(ellipsoiddistance > 1.24f) return false;
    const float offsetx = float(chamber.roughness & 0xFFU) * 5.0f,
                offsety = float((chamber.roughness >> 8) & 0xFFU) * 5.0f,
                roughness = ctx.generator.caves.GetNoise(x + offsetx, y - offsety, z + 4700.5f),
                secondary = ctx.generator.largecaves.GetNoise(x - offsety, y + offsetx, z - 4700.5f),
                boundary = 1.0f + roughness * 0.12f + max(secondary - max(ctx.settings.largecavethreshold, 0.74f), 0.0f) * 0.45f;
    return ellipsoiddistance <= boundary;
}

static void carveworldcavesegment(const worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky, const worldcavesegment &segment)
{
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS, chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS;
    const float radius = max(segment.startradius, segment.endradius) + 4.25f,
                verticalradius = radius * max(segment.verticalscale, 1.0f);
    const int xmin = max(int(floorf(min(segment.start.x, segment.end.x) - radius - chunkstartx)), 0),
              xmax = min(int(ceilf(max(segment.start.x, segment.end.x) + radius - chunkstartx)), WORLD_CHUNK_BLOCKS - 1),
              ymin = max(int(floorf(min(segment.start.y, segment.end.y) - radius - chunkstarty)), 0),
              ymax = min(int(ceilf(max(segment.start.y, segment.end.y) + radius - chunkstarty)), WORLD_CHUNK_BLOCKS - 1),
              zmin = max(int(floorf(min(segment.start.z, segment.end.z) - verticalradius)), int(WORLD_MIN_HEIGHT)),
              zmax = min(int(ceilf(max(segment.start.z, segment.end.z) + verticalradius)), int(WORLD_MAX_HEIGHT) - 1),
              mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth);
    if(xmin > xmax || ymin > ymax || zmin > zmax) return;

    for(int y = ymin; y <= ymax; ++y) for(int x = xmin; x <= xmax; ++x)
    {
        const int surface = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE,
                  ceiling = surface - 1,
                  localzmax = min(zmax, ceiling);
        for(int z = zmin; z <= localzmax; ++z)
        {
            if(!segment.entrance && surface - z < mindepth) continue;
            if(worldcavesegmentcontains(ctx, segment, float(chunkstartx + x) + 0.5f, float(chunkstarty + y) + 0.5f, z + 0.5f))
            {
                uchar &carve = carvemap[worldcarveindex(x, y, z - WORLD_MIN_HEIGHT)];
                carve = WORLD_CARVE_AIR | (carve&WORLD_CARVE_ENTRANCE) | (segment.entrance ? WORLD_CARVE_ENTRANCE : 0);
            }
        }
    }
}

static void carveworldcavechamber(const worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky, const worldcavechamber &chamber)
{
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS, chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS;
    const float horizontalradius = max(chamber.radiusx, chamber.radiusy) * 1.12f,
                verticalradius = chamber.radiusz * 1.12f;
    const int xmin = max(int(floorf(chamber.center.x - horizontalradius - chunkstartx)), 0),
              xmax = min(int(ceilf(chamber.center.x + horizontalradius - chunkstartx)), WORLD_CHUNK_BLOCKS - 1),
              ymin = max(int(floorf(chamber.center.y - horizontalradius - chunkstarty)), 0),
              ymax = min(int(ceilf(chamber.center.y + horizontalradius - chunkstarty)), WORLD_CHUNK_BLOCKS - 1),
              zmin = max(int(floorf(chamber.center.z - verticalradius)), int(WORLD_MIN_HEIGHT)),
              zmax = min(int(ceilf(chamber.center.z + verticalradius)), int(WORLD_MAX_HEIGHT) - 1),
              mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth);
    if(xmin > xmax || ymin > ymax || zmin > zmax) return;

    for(int y = ymin; y <= ymax; ++y) for(int x = xmin; x <= xmax; ++x)
    {
        const int surface = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE,
                  localzmax = min(zmax, surface - mindepth);
        for(int z = zmin; z <= localzmax; ++z)
            if(worldcavechambercontains(ctx, chamber, float(chunkstartx + x) + 0.5f, float(chunkstarty + y) + 0.5f, z + 0.5f))
            {
                uchar &carve = carvemap[worldcarveindex(x, y, z - WORLD_MIN_HEIGHT)];
                carve = WORLD_CARVE_AIR | (carve&WORLD_CARVE_ENTRANCE);
            }
    }
}

static bool generateworldcavenetworks(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
{
    ctx.cavesegments.setsize(0);
    ctx.cavechambers.setsize(0);
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS,
                    chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS,
                    minregionx = worldfloordiv(chunkstartx - WORLD_CAVE_REGION_HALO, WORLD_CAVE_REGION_SIZE),
                    maxregionx = worldfloordiv(chunkstartx + WORLD_CHUNK_BLOCKS - 1 + WORLD_CAVE_REGION_HALO, WORLD_CAVE_REGION_SIZE),
                    minregiony = worldfloordiv(chunkstarty - WORLD_CAVE_REGION_HALO, WORLD_CAVE_REGION_SIZE),
                    maxregiony = worldfloordiv(chunkstarty + WORLD_CHUNK_BLOCKS - 1 + WORLD_CAVE_REGION_HALO, WORLD_CAVE_REGION_SIZE);
    vector<worldcavesegment> systemsegments;
    vector<worldcavechamber> systemchambers;
    const float cacheminx = float(chunkstartx) - 6.0f, cachemaxx = float(chunkstartx + WORLD_CHUNK_BLOCKS) + 6.0f,
                cacheminy = float(chunkstarty) - 6.0f, cachemaxy = float(chunkstarty + WORLD_CHUNK_BLOCKS) + 6.0f;
    for(long long regiony = minregiony; regiony <= maxregiony; ++regiony)
    for(long long regionx = minregionx; regionx <= maxregionx; ++regionx)
    {
        if(ctx.iscanceled()) return false;
        systemsegments.setsize(0);
        systemchambers.setsize(0);
        if(!generateworldcavesystem(ctx, regionx, regiony, systemsegments, systemchambers)) continue;
        loopv(systemsegments)
        {
            const worldcavesegment &segment = systemsegments[i];
            const float radius = max(segment.startradius, segment.endradius) + 4.25f;
            if(max(segment.start.x, segment.end.x) + radius < cacheminx || min(segment.start.x, segment.end.x) - radius > cachemaxx ||
               max(segment.start.y, segment.end.y) + radius < cacheminy || min(segment.start.y, segment.end.y) - radius > cachemaxy) continue;
            ctx.cavesegments.add(segment);
        }
        loopv(systemchambers)
        {
            const worldcavechamber &chamber = systemchambers[i];
            const float radius = max(chamber.radiusx, chamber.radiusy) * 1.12f;
            if(chamber.center.x + radius < cacheminx || chamber.center.x - radius > cachemaxx || chamber.center.y + radius < cacheminy ||
               chamber.center.y - radius > cachemaxy) continue;
            ctx.cavechambers.add(chamber);
        }
    }

    loopv(ctx.cavesegments)
    {
        if((i & 31) == 0 && ctx.iscanceled()) return false;
        carveworldcavesegment(ctx, carvemap, chunkx, chunky, ctx.cavesegments[i]);
    }
    loopv(ctx.cavechambers)
    {
        if((i & 15) == 0 && ctx.iscanceled()) return false;
        carveworldcavechamber(ctx, carvemap, chunkx, chunky, ctx.cavechambers[i]);
    }
    return true;
}

static bool generateworldlavalakes(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
{
    const int spacing = max(ctx.settings.lavalakespacing, 1),
              verticalspacing = max(spacing / 2, 8),
              minradius = min(ctx.settings.lavalakeminsize, ctx.settings.lavalakemaxsize),
              maxradius = max(ctx.settings.lavalakeminsize, ctx.settings.lavalakemaxsize),
              bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)),
              minimumheight = WORLD_MIN_HEIGHT + bottomlayers,
              startheight = max(ctx.settings.lavalakestartheight, ctx.settings.lavalakedeepheight),
              deepheight = min(ctx.settings.lavalakestartheight, ctx.settings.lavalakedeepheight);
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS,
                    chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS,
                    mincellx = worldfloordiv(chunkstartx - maxradius, spacing),
                    maxcellx = worldfloordiv(chunkstartx + WORLD_CHUNK_BLOCKS - 1 + maxradius, spacing),
                    mincelly = worldfloordiv(chunkstarty - maxradius, spacing),
                    maxcelly = worldfloordiv(chunkstarty + WORLD_CHUNK_BLOCKS - 1 + maxradius, spacing);
    const int mincellz = int(worldfloordiv(minimumheight - maxradius, verticalspacing)),
              maxcellz = int(worldfloordiv(startheight, verticalspacing));

    for(long long celly = mincelly; celly <= maxcelly; ++celly)
    for(long long cellx = mincellx; cellx <= maxcellx; ++cellx)
    for(int cellz = mincellz; cellz <= maxcellz; ++cellz)
    {
        if(ctx.iscanceled()) return false;
        const uint positionhash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0xC13FA9A9U),
                   chancehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0x91E10DA5U),
                   sizehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, 0xD192ED03U);
        const long long centerx = cellx * spacing + int(positionhash % uint(spacing)),
                        centery = celly * spacing + int((positionhash >> 8) % uint(spacing));
        const int centerz = cellz * verticalspacing + int((positionhash >> 16) % uint(verticalspacing));
        if(centerz < minimumheight || centerz > startheight) continue;

        const float approachweight = deepheight < startheight ? clamp((startheight - centerz) / float(startheight - deepheight), 0.0f, 1.0f) : 1.0f,
                    deepweight = deepheight > minimumheight
                                     ? clamp((deepheight - centerz) / float(deepheight - minimumheight), 0.0f, 1.0f)
                                     : centerz <= deepheight ? 1.0f : 0.0f,
                    lakechance = ctx.settings.lavalakeshallowchance * approachweight +
                                 (ctx.settings.lavalakedeepchance - ctx.settings.lavalakeshallowchance) * deepweight;
        if(worldtreeunit(chancehash) >= clamp(lakechance, 0.0f, 1.0f)) continue;

        const int depthmaxradius = clamp(int(floor(minradius + (maxradius - minradius) * (0.25f + deepweight * 0.75f) + 0.5f)), minradius, maxradius),
                  radiusrange = max(depthmaxradius - minradius + 1, 1),
                  radius = minradius + int(sizehash % uint(radiusrange)),
                  minorradius = max(2, int(floor(radius * (0.55f + ((sizehash >> 8) & 0xFFU) / 637.5f) + 0.5f))),
                  verticalradius = max(2, (radius + minorradius) / 4),
                  lavalevel = centerz - int((sizehash >> 28) % uint(max(verticalradius / 2, 1)));
        const float angle = ((sizehash >> 16) & 0x0FFFU) / 4096.0f * 2.0f * M_PI,
                    anglecos = cosf(angle), anglesin = sinf(angle),
                    lobeangle = angle + (((positionhash >> 24) & 0xFFU) / 255.0f - 0.5f) * M_PI,
                    lobedistance = radius * (0.15f + ((chancehash >> 24) & 0xFFU) / 1275.0f),
                    lobecenterx = cosf(lobeangle) * lobedistance,
                    lobecentery = sinf(lobeangle) * lobedistance,
                    loberadius = max(radius * 0.62f, 1.0f),
                    lobeminorradius = max(minorradius * 0.7f, 1.0f),
                    shapevariation = clamp(ctx.settings.lavalakeshapevariation, 0.0f, 0.75f);
        const int centerlocalx = int(centerx - chunkstartx),
                  centerlocaly = int(centery - chunkstarty);
        if(centerlocalx + radius < 0 || centerlocalx - radius >= WORLD_CHUNK_BLOCKS ||
           centerlocaly + radius < 0 || centerlocaly - radius >= WORLD_CHUNK_BLOCKS) continue;

        const int centerblockx = int(centerx - (long long)chunkx * WORLD_CHUNK_BLOCKS),
                  centerblocky = int(centery - (long long)chunky * WORLD_CHUNK_BLOCKS),
                  centerheight = generateworldheight(ctx, chunkx, chunky, centerblockx, centerblocky) / WORLD_BLOCK_SIZE;
        if(centerz + verticalradius > centerheight - ctx.settings.cavemindepth) continue;

        const int xmin = max(centerlocalx - radius, 0),
                  xmax = min(centerlocalx + radius, WORLD_CHUNK_BLOCKS - 1),
                  ymin = max(centerlocaly - radius, 0),
                  ymax = min(centerlocaly + radius, WORLD_CHUNK_BLOCKS - 1),
                  zmin = max(centerz - verticalradius, minimumheight),
                  zmax = min(centerz + verticalradius, WORLD_MAX_HEIGHT - 1);
        for(int y = ymin; y <= ymax; ++y) for(int x = xmin; x <= xmax; ++x)
        {
            const float localx = float(x - centerlocalx),
                        localy = float(y - centerlocaly),
                        rotatedx = localx * anglecos + localy * anglesin,
                        rotatedy = -localx * anglesin + localy * anglecos,
                        primary = rotatedx * rotatedx / float(radius * radius) + rotatedy * rotatedy / float(minorradius * minorradius),
                        lobex = rotatedx - lobecenterx,
                        lobey = rotatedy - lobecentery,
                        lobe = lobex * lobex / (loberadius * loberadius) + lobey * lobey / (lobeminorradius * lobeminorradius),
                        horizontal = min(primary, lobe),
                        shapenoise = ctx.generator.lakeshape.GetNoise(float(chunkx) * WORLD_CHUNK_BLOCKS + x + 9200.5f,
                                                                     float(chunky) * WORLD_CHUNK_BLOCKS + y - 9200.5f),
                        boundary = 1.0f - shapevariation * 0.5f + shapenoise * shapevariation * 0.5f;
            if(horizontal > boundary) continue;

            const int surfaceheight = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE;
            for(int logicalz = zmin; logicalz <= zmax; ++logicalz)
            {
                if(surfaceheight - logicalz < ctx.settings.cavemindepth) continue;
                const float dz = (logicalz - centerz) / float(verticalradius);
                if(horizontal + dz * dz > boundary) continue;

                uchar &carve = carvemap[worldcarveindex(x, y, logicalz - WORLD_MIN_HEIGHT)];
                if(logicalz <= lavalevel) carve = WORLD_CARVE_LAVA | (carve&WORLD_CARVE_ENTRANCE);
                else if(!(carve&WORLD_CARVE_TYPE)) carve = WORLD_CARVE_AIR | (carve&WORLD_CARVE_ENTRANCE);
            }
        }
    }
    return true;
}

static bool placeworldcaves(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    const int mapblocks = WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS * WORLD_HEIGHT_BLOCKS;
    vector<uchar> carvemap;
    uchar *carve;
    {
        ZoneScopedN("Chunks/Allocate cave map");
        carve = carvemap.pad(mapblocks);
        memset(carve, WORLD_CARVE_NONE, mapblocks * sizeof(uchar));
    }

    {
        ZoneScopedN("Chunks/Generate cave networks");
        if(!generateworldcavenetworks(ctx, carve, chunkx, chunky)) return false;
    }
    {
        ZoneScopedN("Chunks/Generate lava lakes");
        if(!generateworldlavalakes(ctx, carve, chunkx, chunky)) return false;
    }

    {
        ZoneScopedN("Chunks/Apply cave map");
        const int bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS));
        loop(z, bottomlayers) loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
        {
            uchar &carveflags = carve[worldcarveindex(x, y, z)];
            carveflags = WORLD_CARVE_LAVA | (carveflags&WORLD_CARVE_ENTRANCE);
        }

        loop(z, WORLD_HEIGHT_BLOCKS)
        {
            if(ctx.iscanceled()) return false;
            loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
            {
                const uchar carveflags = carve[worldcarveindex(x, y, z)], type = carveflags&WORLD_CARVE_TYPE;
                if(type == WORLD_CARVE_NONE) continue;
                cube &c = lookupworldgenblock(ctx, root, ivec(x * WORLD_BLOCK_SIZE, y * WORLD_BLOCK_SIZE, z * WORLD_BLOCK_SIZE));
                bool affected = false;
                if(type == WORLD_CARVE_LAVA)
                {
                    setworldcubematerial(c, MAT_LAVA);
                    affected = true;
                }
                else if(!isempty(c) && c.material == MAT_AIR)
                {
                    setworldcubematerial(c, MAT_AIR);
                    affected = true;
                }
                if(!affected) continue;
                markworldgencarvedsection(ctx, x, y, z, (carveflags&WORLD_CARVE_ENTRANCE) != 0);
            }
        }
    }
    return true;
}

static bool worldcaveairat(const worldgencontext &ctx, int worldx, int worldy, int elevation)
{
    const int bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)),
              minheight = WORLD_MIN_HEIGHT + bottomlayers,
              mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth),
              surfaceheight = ctx.generator.height(worldx, worldy),
              caveceiling = min(surfaceheight - 1, WORLD_MAX_HEIGHT - 1);
    if(elevation < minheight || elevation > caveceiling) return false;
    const float x = worldx + 0.5f, y = worldy + 0.5f, z = elevation + 0.5f;
    loopv(ctx.cavesegments)
    {
        const worldcavesegment &segment = ctx.cavesegments[i];
        if(!segment.entrance && surfaceheight - elevation < mindepth) continue;
        if(worldcavesegmentcontains(ctx, segment, x, y, z)) return true;
    }
    if(surfaceheight - elevation < mindepth) return false;
    loopv(ctx.cavechambers) if(worldcavechambercontains(ctx, ctx.cavechambers[i], x, y, z)) return true;
    return false;
}

static bool worldorecaveedge(const worldgencontext &ctx, int worldx, int worldy, int elevation)
{
    static const int directions[6][3] =
    {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 },
        { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };
    if(worldcaveairat(ctx, worldx, worldy, elevation)) return false;
    loopi(6) if(worldcaveairat(ctx, worldx + directions[i][0], worldy + directions[i][1], elevation + directions[i][2])) return true;
    return false;
}

static void placeworldoreblock(worldgencontext &ctx, cube *root, const worldoredefinition &ore, int chunkx, int chunky, int worldx, int worldy,
                               int elevation, int stonetexture, int orecube)
{
    if(elevation < WORLD_MIN_HEIGHT || elevation >= WORLD_MAX_HEIGHT) return;
    const int localx = worldx - chunkx * WORLD_CHUNK_BLOCKS,
              localy = worldy - chunky * WORLD_CHUNK_BLOCKS;

    if(localx < 0 || localx >= WORLD_CHUNK_BLOCKS || localy < 0 || localy >= WORLD_CHUNK_BLOCKS) return;

    const int surfaceheight = ctx.heightmap[localy * WORLD_CHUNK_BLOCKS + localx] / WORLD_BLOCK_SIZE,
              depth = surfaceheight - elevation;

    if(elevation < ore.minheight || elevation > ore.maxheight || depth < ore.mindepth || depth > ore.maxdepth) return;

    cube &c = lookupworldgenblock(
        ctx, root, ivec(localx * WORLD_BLOCK_SIZE, localy * WORLD_BLOCK_SIZE, (elevation - WORLD_MIN_HEIGHT) * WORLD_BLOCK_SIZE));
    if(!isempty(c) && c.texture[0] == stonetexture) setworldcubetype(c, ctx, orecube);
}

static void placeworldorevein(worldgencontext &ctx, cube *root, const worldoredefinition &ore, int chunkx, int chunky, long long cellx, long long celly, int cellz, int centerx, int centery, int centerz, int stonetexture, int orecube)
{
    const uint sizehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, ore.salt ^ 0xA511E9B3U),
               shapehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, ore.salt ^ 0x63D83595U);
    const bool rare = ore.rareminvein > 0 && (sizehash & 0xFFU) < 8U;
    const int minvein = rare ? ore.rareminvein : ore.minvein,
              maxvein = rare ? ore.raremaxvein : ore.maxvein,
              veinrange = max(maxvein - minvein + 1, 1),
              veinsize = minvein + int((sizehash >> 8) % uint(veinrange)),
              radius = max(2, int(ceilf(powf(max(float(veinsize), 1.0f), 1.0f / 3.0f) * 2.0f))),
              verticalradius = max(1, radius * 3 / 4);
    static const int directions[6][3] =
    {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 },
        { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };
    vector<ivec> blocks, frontier;
    blocks.add(ivec(centerx, centery, centerz));
    frontier.add(ivec(centerx, centery, centerz));

    loop(step, veinsize - 1)
    {
        bool added = false;
        loop(attempt, 24)
        {
            const uint offsethash = hashworldfeature(uint(ctx.seed), cellx + step * 17 + attempt, celly - step * 31 - attempt, cellz + step,
                                                     ore.salt ^ shapehash);
            const ivec &parent = frontier[offsethash % uint(frontier.length())];
            const int *direction = directions[(offsethash >> 8) % 6];
            const ivec block(parent.x + direction[0], parent.y + direction[1], parent.z + direction[2]);
            const int offsetx = block.x - centerx, offsety = block.y - centery, offsetz = block.z - centerz;
            const float horizontal = (offsetx * offsetx + offsety * offsety) / float(radius * radius),
                        vertical = offsetz * offsetz / float(verticalradius * verticalradius);
            if(horizontal + vertical > 1.0f) continue;

            bool duplicate = false;
            loopv(blocks) if(blocks[i] == block)
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

    loopv(blocks) placeworldoreblock(ctx, root, ore, chunkx, chunky, blocks[i].x, blocks[i].y, blocks[i].z, stonetexture, orecube);
}

static bool placeworldores(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS,
                    chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS;
    const int stonecube = ctx.cubetype("stone"),
              stonetexture = ctx.cubetextures.inrange(stonecube) ? ctx.indexedtextures ? stonecube : ctx.cubetextures[stonecube].side : -1;

    if(stonetexture < 0) return true;

    loopi(int(sizeof(worldores) / sizeof(worldores[0])))
    {
        const worldoredefinition &ore = worldores[i];
        const int orecube = ctx.cubetype(ore.id),
                  radius = worldoreveinradius(ore), cellsize = max(ore.cellsize, 1);
        if(!ctx.cubetextures.inrange(orecube)) continue;

        const long long mincellx = worldfloordiv(chunkstartx - radius, cellsize),
                        maxcellx = worldfloordiv(chunkstartx + WORLD_CHUNK_BLOCKS - 1 + radius, cellsize),
                        mincelly = worldfloordiv(chunkstarty - radius, cellsize),
                        maxcelly = worldfloordiv(chunkstarty + WORLD_CHUNK_BLOCKS - 1 + radius, cellsize);
        const int mincellz = int(worldfloordiv(ore.minheight - radius, cellsize)),
                  maxcellz = int(worldfloordiv(ore.maxheight + radius, cellsize));

        for(long long celly = mincelly; celly <= maxcelly; ++celly)
        for(long long cellx = mincellx; cellx <= maxcellx; ++cellx)
        for(int cellz = mincellz; cellz <= maxcellz; ++cellz)
        {
            if(ctx.iscanceled()) return false;
            const uint positionhash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, ore.salt),
                       chancehash = hashworldfeature(uint(ctx.seed), cellx, celly, cellz, ore.salt ^ 0xC13FA9A9U);
            const int centerpadding = ore.uniformdistribution ? radius : 0,
                      centerspan = max(cellsize - centerpadding * 2, 1),
                      centerx = int(cellx * cellsize + centerpadding + positionhash % uint(centerspan)),
                      centery = int(celly * cellsize + centerpadding + (positionhash >> 8) % uint(centerspan)),
                      centerz = int(cellz * cellsize + centerpadding + (positionhash >> 16) % uint(centerspan));

            if(centerz < ore.minheight || centerz > ore.maxheight) continue;

            const int surfaceheight = ctx.generator.height(centerx, centery);
            const int depth = surfaceheight - centerz;
            if(depth < ore.mindepth || depth > ore.maxdepth) continue;

            const game::worldtectonicsample tectonics = ctx.generator.tectonics(centerx, centery);
            const float caveweight = ore.uniformdistribution ? 1.0f : worldorecaveedge(ctx, centerx, centery, centerz) ? 2.5f : 0.75f;
            const float chance = clamp(ore.chance * worldoreelevationweight(ore, centerz) * worldoregeologicalweight(ore, tectonics, centerz) * caveweight, 0.0f, 1.0f);
            if(worldtreeunit(chancehash) >= chance) continue;

            placeworldorevein(ctx, root, ore, chunkx, chunky, cellx, celly, cellz, centerx, centery, centerz, stonetexture, orecube);
        }
    }
    return !ctx.iscanceled();
}

static bool placeworldtrees(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    vector<ivec> wood, pinewood, leaves, needles;
    vector<worldtreecandidate> candidates;
    const int halo = 4,
              beachmin = (ctx.settings.sealevel + min(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE,
              beachmax = (ctx.settings.sealevel + max(ctx.settings.beachminheight, ctx.settings.beachmaxheight)) * WORLD_BLOCK_SIZE,
              coasttreemax = (ctx.settings.sealevel + 2) * WORLD_BLOCK_SIZE;

    {
        ZoneScopedN("Chunks/Select tree blocks");
        for(int y = -halo; y < WORLD_CHUNK_BLOCKS + halo; ++y)
        for(int x = -halo; x < WORLD_CHUNK_BLOCKS + halo; ++x)
        {
            if(x == -halo && ctx.iscanceled()) return false;
            const bool inside = x >= 0 && x < WORLD_CHUNK_BLOCKS && y >= 0 && y < WORLD_CHUNK_BLOCKS;
            const int index = inside ? y * WORLD_CHUNK_BLOCKS + x : 0;
            game::worldtectonicsample terrain;
            const int height = inside ? ctx.heightmap[index] : generateworldheight(ctx, chunkx, chunky, x, y, &terrain),
                      biome = inside ? ctx.biomemap[index] : generateworldbiome(ctx, chunkx, chunky, x, y, height);
            if(biome != game::WORLD_BIOME_FOREST && biome != game::WORLD_BIOME_PLAINS) continue;
            if(ctx.settings.coastwidth > 0 && height >= beachmin && height <= max(beachmax, coasttreemax)) continue;
            if(inside)
            {
                if(!worldtreegrowablesurface(ctx, x, y, height, biome)) continue;
            }
            else if(terrain.rockyledge > 0.22f
                 || generateworldcliff(ctx, chunkx, chunky, x, y, height)
                 || generateworldrock(ctx, chunkx, chunky, x, y, height)) continue;
            const float density = biome == game::WORLD_BIOME_FOREST ? ctx.settings.foresttreedensity : ctx.settings.plainstreedensity;
            const uint spawn = hashworldtree(uint(ctx.seed), chunkx, chunky, x, y, 0xD1B54A35U);
            if(worldtreeunit(spawn) >= density) continue;

            const float heightblocks = height / float(WORLD_BLOCK_SIZE),
                        pinelow = float(min(ctx.settings.pinestartheight, ctx.settings.pinefullheight)),
                        pinehigh = float(max(ctx.settings.pinestartheight, ctx.settings.pinefullheight)),
                        pinechance = worldsmoothstep(pinelow, pinehigh, heightblocks);
            const uint shape = hashworldtree(uint(ctx.seed), chunkx, chunky, x, y, 0x94D049BBU);
            const bool pine = worldtreeunit(shape) < pinechance;
            const int treeheight = pine ? 6 + int((shape >> 24) & 3U) : 4 + int((shape >> 24) % 3U),
                      basez = WORLD_GROUND_HEIGHT / WORLD_BLOCK_SIZE + height / WORLD_BLOCK_SIZE;

            if(basez + treeheight >= WORLD_HEIGHT_BLOCKS) continue;

            candidates.add(worldtreecandidate(x, y, chunkx * WORLD_CHUNK_BLOCKS + x, chunky * WORLD_CHUNK_BLOCKS + y, basez, treeheight, spawn, shape, pine));
        }
    }

    {
        ZoneScopedN("Chunks/Apply tree blocks");
        loopv(candidates)
        {
            if(!worldtreecandidateallowed(candidates, candidates[i])) continue;
            if(candidates[i].pine) addworldpinetree(pinewood, needles, candidates[i].blockx, candidates[i].blocky, candidates[i].basez, candidates[i].height);
            else addworldregulartree(wood, leaves, candidates[i].blockx, candidates[i].blocky, candidates[i].basez, candidates[i].height, candidates[i].shape);
        }
        ZoneValue(wood.length() + pinewood.length() + leaves.length() + needles.length());
        const int leafcube = ctx.cubetype("leaves"), needlescube = ctx.cubetype("needles"),
                  woodcube = ctx.cubetype("wood"), pinewoodcube = ctx.cubetype("dark_wood"),
                  leaftexture = ctx.cubetextures.inrange(leafcube) ? ctx.indexedtextures ? leafcube : ctx.cubetextures[leafcube].top : -1,
                  needlestexture = ctx.cubetextures.inrange(needlescube) ? ctx.indexedtextures ? needlescube : ctx.cubetextures[needlescube].top : -1;
        loopv(leaves)
        {
            cube &c = lookupworldgenblock(ctx, root, leaves[i]);
            if(isempty(c) && c.material == MAT_AIR)
            {
                if(setworldcubetype(c, ctx, leafcube, leavesalpha ? MAT_ALPHA : MAT_AIR)) markworldgentreeblock(ctx, leaves[i]);
            }
        }
        loopv(needles)
        {
            cube &c = lookupworldgenblock(ctx, root, needles[i]);
            if(isempty(c) && c.material == MAT_AIR)
            {
                if(setworldcubetype(c, ctx, needlescube, leavesalpha ? MAT_ALPHA : MAT_AIR)) markworldgentreeblock(ctx, needles[i]);
            }
        }
        loopv(wood)
        {
            cube &c = lookupworldgenblock(ctx, root, wood[i]);
            if((isempty(c) && c.material == MAT_AIR) || c.texture[0] == leaftexture || c.texture[0] == needlestexture)
            {
                if(setworldcubetype(c, ctx, woodcube)) markworldgentreeblock(ctx, wood[i]);
            }
        }
        loopv(pinewood)
        {
            cube &c = lookupworldgenblock(ctx, root, pinewood[i]);
            if((isempty(c) && c.material == MAT_AIR) || c.texture[0] == leaftexture || c.texture[0] == needlestexture)
            {
                if(setworldcubetype(c, ctx, pinewoodcube)) markworldgentreeblock(ctx, pinewood[i]);
            }
        }
    }
    return !ctx.iscanceled();
}

static cube *generateworldchunk(int chunkx, int chunky, worldgencontext &ctx)
{
    ZoneScopedN("Chunks/Generate");
    ZoneTextF("%d_%d", chunkx, chunky);
    {
        ZoneScopedN("Chunks/Generate height and biomes");
        if(!generateworldheightmap(ctx, chunkx, chunky)) return NULL;
        markworldgenexteriorshell(ctx, chunkx, chunky);
    }
    cube *root;
    {
        ZoneScopedN("Chunks/Generate base octree");
        root = allocworldgenfamily(ctx);
        const int rootsize = WORLD_CHUNK_ROOT_SIZE;
        loopi(8) if(!generateworldcube(ctx, root[i], ivec(i, ivec(0, 0, 0), rootsize), rootsize, WORLD_BLOCK_SIZE))
        {
            ZoneScopedN("Chunks/Free failed generation");
            freepreparedworldchunk(root);
            return NULL;
        }
    }
    {
        ZoneScopedN("Chunks/Generate caves");
        if(!placeworldcaves(ctx, root, chunkx, chunky))
        {
            ZoneScopedN("Chunks/Free failed generation");
            freepreparedworldchunk(root);
            return NULL;
        }
    }
    loopi(WORLD_SECTION_LAYERS) loopj(WORLD_SECTION_TILES)
    {
        uchar &flags = ctx.renderdata.flags[i][j];
        if((flags&SECTION_FULLY_SOLID) && !(flags&(SECTION_EXTERIOR | SECTION_INTERIOR | SECTION_WATER))) flags |= SECTION_NO_RENDER;
        else flags &= ~SECTION_NO_RENDER;
    }
    {
        ZoneScopedN("Chunks/Generate ores");
        if(!placeworldores(ctx, root, chunkx, chunky))
        {
            ZoneScopedN("Chunks/Free failed generation");
            freepreparedworldchunk(root);
            return NULL;
        }
    }
    {
        ZoneScopedN("Chunks/Generate trees");
        if(!placeworldtrees(ctx, root, chunkx, chunky))
        {
            ZoneScopedN("Chunks/Free failed generation");
            freepreparedworldchunk(root);
            return NULL;
        }
    }
    if(ctx.remip)
    {
#ifndef STANDALONE
        ZoneScopedN("Chunks/Remip generated octree");
        ctx.optimized = remipworldchunk(root, ctx.prepared, ctx.families, ctx.cancelled);
        ZoneValue(ctx.optimized);
#else
        ctx.optimized = 0;
#endif
    }
    else ctx.optimized = 0;
    if(ctx.iscanceled())
    {
        ZoneScopedN("Chunks/Free cancelled generation");
        freepreparedworldchunk(root);
        return NULL;
    }
    ZoneValue(ctx.families);
    return root;
}

static cube *generateworldchunk(int chunkx, int chunky, worldsectionrenderdata *renderdata)
{
    ZoneScopedN("Chunks/Generate synchronous");
    ZoneTextF("%d_%d", chunkx, chunky);
    const game::worldsettings settings;
    worldgencontext ctx(game::getworldseed(), worldgentextures, false, chunkremip != 0, settings);
    cube *root = generateworldchunk(chunkx, chunky, ctx);
    if(root && renderdata) *renderdata = ctx.renderdata;
    return root;
}

static bool dryworldspawnblock(const game::worldgenerator &generator, const game::worldsettings &settings, int x, int y)
{
    const int height = generator.height(x, y);
    return height >= settings.sealevel && height <= WORLD_MAX_HEIGHT - 3;
}

bool game::chooseworldspawn(double originx, double originy, double &spawnx, double &spawny)
{
    const int originblockx = int(floor(originx / WORLD_BLOCK_SIZE)),
              originblocky = int(floor(originy / WORLD_BLOCK_SIZE));

    game::worldsettings settings;
    game::worldgenerator generator(game::getworldseed(), settings);

    if(dryworldspawnblock(generator, settings, originblockx, originblocky))
    {
        spawnx = (double(originblockx) + 0.5) * WORLD_BLOCK_SIZE;
        spawny = (double(originblocky) + 0.5) * WORLD_BLOCK_SIZE;
        return true;
    }

#ifndef STANDALONE
    renderprogress(0.82f, "choosing a better spawn point because you had no chance...");
#endif

    int bestx = originblockx, besty = originblocky;
    long long bestdist = LLONG_MAX;

    // Search every nearby block first, then cover a continent-scale area on a
    // coarse grid. A final local pass turns the best coarse hit into a block-
    // precise dry spawn without evaluating millions of noise samples.
    const int exactradius = 64;
    for(int y = originblocky - exactradius; y <= originblocky + exactradius; ++y)
    for(int x = originblockx - exactradius; x <= originblockx + exactradius; ++x)
    {
        if(!dryworldspawnblock(generator, settings, x, y)) continue;
        const long long dx = x - originblockx, dy = y - originblocky,
                        dist = dx * dx + dy * dy;
        if(dist >= bestdist) continue;
        bestx = x;
        besty = y;
        bestdist = dist;
    }

    if(bestdist == LLONG_MAX)
    {
        const int searchradius = 8192, searchstep = 64;
        for(int y = originblocky - searchradius; y <= originblocky + searchradius; y += searchstep)
        for(int x = originblockx - searchradius; x <= originblockx + searchradius; x += searchstep)
        {
            if(!dryworldspawnblock(generator, settings, x, y)) continue;
            const long long dx = x - originblockx, dy = y - originblocky,
                            dist = dx * dx + dy * dy;
            if(dist >= bestdist) continue;
            bestx = x;
            besty = y;
            bestdist = dist;
        }
    }

    if(bestdist == LLONG_MAX) return false;

    {
        const int refine = 64;
        int refinedx = bestx, refinedy = besty;
        long long refineddist = bestdist;
        for(int y = besty - refine; y <= besty + refine; ++y)
        for(int x = bestx - refine; x <= bestx + refine; ++x)
        {
            if(!dryworldspawnblock(generator, settings, x, y)) continue;
            const long long dx = x - originblockx, dy = y - originblocky,
                            dist = dx * dx + dy * dy;
            if(dist >= refineddist) continue;
            refinedx = x;
            refinedy = y;
            refineddist = dist;
        }
        bestx = refinedx;
        besty = refinedy;
    }

    spawnx = (double(bestx) + 0.5) * WORLD_BLOCK_SIZE;
    spawny = (double(besty) + 0.5) * WORLD_BLOCK_SIZE;
    return true;
}


namespace game
{
    float getworldscattermaxoffset()
    {
        return worldsettings().grassmaxoffset;
    }

    ullong worldgenerationparameterhash()
    {
        const worldsettings settings;
        const uchar *bytes = (const uchar *)&settings;
        ullong hash = 1469598103934665603ULL;
        loopi(sizeof(settings))
        {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    void sampleworldgenerationdebug(int blockx, int blocky, int logicalz, float &activity, float &uplift, float &trench, float &caveexpansion)
    {
        worldgenerator generator(getworldseed());
        const int surfaceheight = generator.height(blockx, blocky);
        const worldtectonicsample tectonics = generator.tectonics(blockx, blocky, max(surfaceheight - logicalz, 0));
        activity = tectonics.activity;
        uplift = tectonics.landuplift;
        trench = tectonics.oceantrench;
        caveexpansion = tectonics.caveexpansion;
    }

    worldgencontext *createworldgeneration(bool prepared, bool remip, SDL_atomic_t *cancelled, bool indexedtextures)
    {
        const worldsettings settings;
#ifdef STANDALONE
        indexedtextures = true;
#endif
        return new worldgencontext(getworldseed(), worldgentextures, prepared, remip, settings, cancelled, indexedtextures);
    }

    void destroyworldgeneration(worldgencontext *generation)
    {
        delete generation;
    }

    static bool sampleterrainheightcached(worldgencontext *generation, int blockx, int blocky, int &height)
    {
        if(!generation || generation->iscanceled()) return false;
        const ivec position(blockx, blocky, 0);
        int *cached = generation->surfaceheightcache.access(position);
        if(cached)
        {
            height = *cached;
            return true;
        }
        height = generation->generator.height(blockx, blocky);
        if(generation->iscanceled()) return false;
        generation->surfaceheightcache.access(position, height);
        return true;
    }

    // Match worldgenerator::beach while sharing expensive height samples and remaining responsive to cancelled LOD jobs.
    static bool sampleterrainbeach(worldgencontext *generation, int blockx, int blocky, bool &beach)
    {
        beach = false;
        if(generation->settings.coastwidth <= 0) return true;

        const float width = generation->generator.beachtransitionwidth(blockx, blocky);
        const int maximumcost = int(floorf(width * 3.0f + 0.5f)),
                  searchradius = int(ceilf(generation->generator.maxbeachtransitionwidth())) + 1,
                  sealevel = generation->settings.sealevel;
        for(int dy = -searchradius; dy <= searchradius; ++dy)
        {
            if(generation->iscanceled()) return false;
            for(int dx = -searchradius; dx <= searchradius; ++dx)
            {
                const int diagonal = min(abs(dx), abs(dy)), straight = max(abs(dx), abs(dy)) - diagonal,
                          cost = diagonal * 4 + straight * 3;
                if(cost > maximumcost) continue;
                const int samplex = blockx + dx, sampley = blocky + dy;
                int center, neighbor;
                if(!sampleterrainheightcached(generation, samplex, sampley, center)) return false;
                const bool water = center < sealevel;
                static const int directions[][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
                loopi(4)
                {
                    if(!sampleterrainheightcached(generation, samplex + directions[i][0], sampley + directions[i][1], neighbor)) return false;
                    if((neighbor < sealevel) != water)
                    {
                        beach = true;
                        return true;
                    }
                }
            }
        }
        return !generation->iscanceled();
    }

    bool sampleterrainheight(worldgencontext *generation, int blockx, int blocky, int &height)
    {
        return sampleterrainheightcached(generation, blockx, blocky, height);
    }

    bool sampleterrainsurface(worldgencontext *generation, int blockx, int blocky, worldsurfacesample &surface)
    {
        if(!generation || generation->iscanceled()) return false;
        int height;
        if(!sampleterrainheightcached(generation, blockx, blocky, height)) return false;
        const int biome = generation->generator.biome(blockx, blocky, height);
        const int beachminimum = generation->settings.sealevel + min(generation->settings.beachminheight, generation->settings.beachmaxheight),
                  beachmaximum = generation->settings.sealevel + max(generation->settings.beachminheight, generation->settings.beachmaxheight);
        const bool cliff = generation->generator.cliff(blockx, blocky, height),
                   rock = generation->generator.rock(blockx, blocky, height);
        bool beach = false;
        if(height >= beachminimum && height <= beachmaximum && !sampleterrainbeach(generation, blockx, blocky, beach)) return false;

        surface.height = height;
        surface.waterheight = generation->settings.sealevel;
        surface.water = height < surface.waterheight;
        if(cliff) surface.material = WORLD_SURFACE_STONE;
        else if(rock) surface.material = biome == WORLD_BIOME_SNOW ? WORLD_SURFACE_SNOW : WORLD_SURFACE_STONE;
        else if(beach || biome == WORLD_BIOME_DESERT) surface.material = WORLD_SURFACE_SAND;
        else if(biome == WORLD_BIOME_OCEAN) surface.material = WORLD_SURFACE_DIRT;
        else if(biome == WORLD_BIOME_SNOW) surface.material = WORLD_SURFACE_SNOW;
        else surface.material = WORLD_SURFACE_GRASS;
        return !generation->iscanceled();
    }

    bool sampleworldtree(worldgencontext *generation, int blockx, int blocky, int &base, int &height, uint &shape, bool &pine)
    {
        if(!generation || generation->iscanceled()) return false;
        return generation->generator.tree(blockx, blocky, base, height, shape, pine) && !generation->iscanceled();
    }

    cube *generateworldchunk(worldgencontext *generation, int chunkx, int chunky, int &families, int &optimized, worldsectionrenderdata *renderdata)
    {
        if(!generation) return NULL;
        cube *root = ::generateworldchunk(chunkx, chunky, *generation);
        families = generation->families;
        optimized = generation->optimized;
        if(root && renderdata) *renderdata = generation->renderdata;
        return root;
    }

    void generateworldscatter(worldgencontext *generation, cube *root, int chunkx, int chunky, vector<worldscatterinstance> &scatter)
    {
        if(!generation)
        {
            scatter.setsize(0);
            return;
        }
        ::generateworldscatter(root, chunkx, chunky, generation->settings, scatter);
    }

    void generateworldscatter(cube *root, int chunkx, int chunky, vector<worldscatterinstance> &scatter)
    {
        const worldsettings settings;
        ::generateworldscatter(root, chunkx, chunky, settings, scatter);
    }

    cube *generateworldchunk(int chunkx, int chunky, worldsectionrenderdata *renderdata)
    {
        return ::generateworldchunk(chunkx, chunky, renderdata);
    }

    void freeworldchunk(cube *root)
    {
        freepreparedworldchunk(root);
    }

    bool validgeneratedworldscatter(const cube *root, const worldscatterinstance &scatter)
    {
        return ::validgeneratedworldscatter(root, scatter);
    }

    void cacheworldscattertransform(int chunkx, int chunky, float maxoffset, const worldscatterinstance &scatter)
    {
        ::cacheworldscattertransform(chunkx, chunky, maxoffset, scatter);
    }

    void cacheworldscattertransforms(int chunkx, int chunky, float maxoffset, const vector<worldscatterinstance> &scatter)
    {
        ::cacheworldscattertransforms(chunkx, chunky, maxoffset, scatter);
    }
}

