#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

extern vector<worldgencubetextures> worldgentextures;
extern int worldgrassscatter, worldrosescatter, worldtulipscatter, worlddandelionscatter;
#ifdef STANDALONE
int chunkremip = 1, leavesalpha = 1;
#else
extern int chunkremip, leavesalpha;
#endif
extern int remipworldchunk(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled);

worldsurfacesample::worldsurfacesample() : height(0), waterheight(0), material(WORLD_SURFACE_GRASS), water(false) {}
worldgencontext::worldgencontext(int seed, const vector<worldgencubetextures> &cubetextures, bool prepared, bool remip,
                                 const game::worldsettings &settings, SDL_atomic_t *cancelled, bool indexedtextures)
    : generator(seed, settings), settings(settings), seed(seed), cubetextures(cubetextures), cubeids(64), surfaceheightcache(1 << 12), errorcube(-1),
      prepared(prepared), remip(remip), indexedtextures(indexedtextures), families(0), optimized(0), cancelled(cancelled)
{
    loopv(this->cubetextures) cubeids[this->cubetextures[i].id] = i;
    int *error = cubeids.access("error");
    errorcube = error ? *error : -1;
    const char *rockids[] = {"stone", "gabbro", "peridotitis"};
    loopi(3)
    {
        geologymaterials[i] = cubetype(rockids[i]);
        if(!cubeids.access(rockids[i]) || !this->cubetextures.inrange(geologymaterials[i])) geologymaterials[i] = geologymaterials[0];
        geologytextures[i] = this->cubetextures.inrange(geologymaterials[i])
                                 ? indexedtextures ? geologymaterials[i] : this->cubetextures[geologymaterials[i]].side
                                 : -1;
    }
    loopv(this->cubetextures) rockvariants.add(ivec(i, i, i));
    loopv(this->cubetextures)
    {
        const worldgencubetextures &variant = this->cubetextures[i];
        int *base = cubeids.access(variant.variantbase);
        if(!base || !variant.varianthost[0]) continue;
        loopj(3)
            if(!strcmp(variant.varianthost, rockids[j])) rockvariants[*base][j] = i;
    }
}

bool worldgencontext::iscanceled() const
{
    return cancelled && SDL_AtomicGet(cancelled);
}

int worldgencontext::cubetype(const char *id) const
{
    return cubeids.access(id, errorcube);
}

float worldsmoothstep(float low, float high, float value)
{
    if(high <= low) return value >= high ? 1.0f : 0.0f;
    float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float worldtreeunit(uint hash)
{
    return float(hash & 0x00FFFFFFU) / float(0x01000000U);
}

int worldcarveindex(int x, int y, int blockz)
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

uint hashworldfeature(uint seed, long long x, long long y, int z, uint salt)
{
    const unsigned long long ux = (unsigned long long)x, uy = (unsigned long long)y;
    uint hash = seed ^ salt;
    hash = mixworldfeaturehash(hash, uint(ux));
    hash = mixworldfeaturehash(hash, uint(ux >> 32));
    hash = mixworldfeaturehash(hash, uint(uy));
    hash = mixworldfeaturehash(hash, uint(uy >> 32));
    return mixworldfeaturehash(hash, uint(z));
}

long long worldfloordiv(long long value, int divisor)
{
    long long quotient = value / divisor;
    if(value < 0 && value % divisor) --quotient;
    return quotient;
}

static cube *generateworldchunk(int chunkx, int chunky, worldgencontext &ctx)
{
    ZoneScopedN("Chunks/Generate");
    ZoneTextF("%d_%d", chunkx, chunky);
    {
        ZoneScopedN("Chunks/Generate height and biomes");
        if(!generateworldheightmap(ctx, chunkx, chunky)) return NULL;
        if(!generateworldgeology(ctx, chunkx, chunky)) return NULL;
        markworldgenexteriorshell(ctx, chunkx, chunky);
    }
    cube *root;
    {
        ZoneScopedN("Chunks/Generate base octree");
        root = allocworldgenfamily(ctx);
        const int rootsize = WORLD_CHUNK_ROOT_SIZE;
        loopi(8)
            if(!generateworldcube(ctx, root[i], ivec(i, ivec(0, 0, 0), rootsize), rootsize, WORLD_BLOCK_SIZE))
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
    loopi(WORLD_SECTION_LAYERS)
        loopj(WORLD_SECTION_TILES)
        {
            uchar &flags = ctx.renderdata.flags[i][j];
            if((flags & SECTION_FULLY_SOLID) && !(flags & (SECTION_EXTERIOR | SECTION_INTERIOR | SECTION_WATER)))
                flags |= SECTION_NO_RENDER;
            else
                flags &= ~SECTION_NO_RENDER;
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
        ZoneScopedN("Chunks/Generate ice formations");
        if(!placeworldice(ctx, root, chunkx, chunky))
        {
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
    else
        ctx.optimized = 0;
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
        // Include procedural geometry revisions so cached distant surfaces match regenerated chunks.
        ullong hash = 1469598103934665603ULL ^ 0x2026091303ULL;
        loopi(sizeof(settings))
        {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    void sampleworldgenerationdebug(int blockx, int blocky, int logicalz, float &activity, float &uplift, float &trench, float &caveexpansion)
    {
        worldgenerator &generator = getenvironmentgenerator();
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
        const int maximumcost = int(floorf(width * 3.0f + 0.5f)), searchradius = int(ceilf(generation->generator.maxbeachtransitionwidth())) + 1,
                  sealevel = generation->settings.sealevel;
        for(int dy = -searchradius; dy <= searchradius; ++dy)
        {
            if(generation->iscanceled()) return false;
            for(int dx = -searchradius; dx <= searchradius; ++dx)
            {
                const int diagonal = min(abs(dx), abs(dy)), straight = max(abs(dx), abs(dy)) - diagonal, cost = diagonal * 4 + straight * 3;
                if(cost > maximumcost) continue;
                const int samplex = blockx + dx, sampley = blocky + dy;
                int center, neighbor;
                if(!sampleterrainheightcached(generation, samplex, sampley, center)) return false;
                const bool water = center < sealevel;
                static const int directions[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
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

    vec samplegrassgenerationcolor(worldgencontext *generation, const vec &absolute)
    {
        worldgenerator &generator = generation->generator;
        return getgrassclimatecolor(generator.environmentclimate.gettemperature(absolute), generator.gethumidity(absolute));
    }

    vec sampleterraingenerationclimate(worldgencontext *generation, const vec &absolute, bool transition)
    {
        return generation->generator.terrainclimate(absolute, transition);
    }

    bool sampleterrainheight(worldgencontext *generation, int blockx, int blocky, int &height)
    {
        if(!sampleterrainheightcached(generation, blockx, blocky, height)) return false;
        int icebottom, icetop;
        if(generation->generator.icecolumn(blockx, blocky, height, icebottom, icetop)) height = icetop;
        return !generation->iscanceled();
    }

    bool sampleterrainsurface(worldgencontext *generation, int blockx, int blocky, worldsurfacesample &surface)
    {
        if(!generation || generation->iscanceled()) return false;
        int height;
        if(!sampleterrainheightcached(generation, blockx, blocky, height)) return false;
        const int biome = generation->generator.surfacematerial(blockx, blocky, height);
        const int beachminimum = generation->settings.sealevel + min(generation->settings.beachminheight, generation->settings.beachmaxheight),
                  beachmaximum = generation->settings.sealevel + max(generation->settings.beachminheight, generation->settings.beachmaxheight);
        bool cliffface = false;
        const bool cliff = generation->generator.cliff(blockx, blocky, height, &cliffface), rock = generation->generator.rock(blockx, blocky, height);
        bool beach = false;
        if(height >= beachminimum && height <= beachmaximum && !sampleterrainbeach(generation, blockx, blocky, beach)) return false;

        surface.height = height;
        const worldwatersample hydro = generation->generator.surface(blockx, blocky);
        surface.waterheight = hydro.water;
        surface.water = height < surface.waterheight;
        if(cliffface)
            surface.material = WORLD_SURFACE_STONE;
        else if(rock)
            surface.material = biome == WORLD_BIOME_SNOW ? WORLD_SURFACE_SNOW : WORLD_SURFACE_STONE;
        else if((beach && !cliff) || biome == WORLD_BIOME_DESERT)
            surface.material = WORLD_SURFACE_SAND;
        else if(biome == WORLD_BIOME_OCEAN)
            surface.material = WORLD_SURFACE_DIRT;
        else if(biome == WORLD_BIOME_SNOW)
            surface.material = WORLD_SURFACE_SNOW;
        else
            surface.material = WORLD_SURFACE_GRASS;
        if(beach && !surface.water && surface.material == WORLD_SURFACE_SAND &&
           generation->generator.environmentclimate.gettemperature(vec(float(blockx) * WORLD_BLOCK_SIZE, float(blocky) * WORLD_BLOCK_SIZE,
                                                                       float(WORLD_GROUND_HEIGHT + height * WORLD_BLOCK_SIZE))) < -5.0f)
            surface.material = WORLD_SURFACE_SNOW;
        if(cliff && !cliffface && !rock) surface.material |= WORLD_SURFACE_STONE_BASE;
        if(hydro.freshwater) surface.material = WORLD_SURFACE_DIRT;
        if(!cliffface && !(beach && !cliff)) switch(biome)
            {
            case WORLD_SNOWY_GRASS:
                surface.material = WORLD_SURFACE_SNOWY_GRASS;
                break;
            case WORLD_FROZEN_DIRT:
                surface.material = WORLD_SURFACE_FROZEN_DIRT;
                break;
            case WORLD_MOSS:
                surface.material = WORLD_SURFACE_MOSS;
                break;
            case WORLD_FROZEN_MOSS:
                surface.material = WORLD_SURFACE_FROZEN_MOSS;
                break;
            case WORLD_FROZEN_GRAVEL:
                surface.material = WORLD_SURFACE_FROZEN_GRAVEL;
                break;
            case WORLD_SNOW_CRUST:
                surface.material = WORLD_SURFACE_SNOW_CRUST;
                break;
            case WORLD_DEEP_SNOW:
                surface.material = WORLD_SURFACE_DEEP_SNOW;
                break;
            }
        if(biome == WORLD_COLD_ROCK) surface.material = WORLD_SURFACE_STONE;
        if((!cliffface && !(beach && !cliff) && biome == WORLD_ICE) || biome == WORLD_FROZEN_WATER)
        {
            surface.material = WORLD_SURFACE_ICE;
            if(biome == WORLD_FROZEN_WATER) surface.height = hydro.water;
            surface.water = false;
        }
        int icebottom, icetop;
        if(generation->generator.icecolumn(blockx, blocky, height, icebottom, icetop))
        {
            surface.height = icetop;
            surface.material = WORLD_SURFACE_ICE;
            surface.water = false;
        }
        if((surface.material == WORLD_SURFACE_SNOW || surface.material == WORLD_SURFACE_SNOWY_GRASS) &&
           generation->generator.treegroundmaterial(blockx, blocky, height, biome) == WORLD_BIOME_PLAINS)
            surface.material = WORLD_SURFACE_GRASS;
        return !generation->iscanceled();
    }

    bool sampleworldsnow(worldgencontext *generation, int blockx, int blocky)
    {
        if(!generation || generation->iscanceled()) return false;
        int height;
        if(!sampleterrainheightcached(generation, blockx, blocky, height)) return false;
        const vec position(float(blockx) * WORLD_BLOCK_SIZE, float(blocky) * WORLD_BLOCK_SIZE,
                           float(WORLD_GROUND_HEIGHT + height * WORLD_BLOCK_SIZE));
        // Surface snow is impossible at 3 C and above. Avoid humidity, soil and biome work for warm canopies.
        if(generation->generator.environmentclimate.gettemperature(position) >= 3.0f) return false;
        const int material = generation->generator.surfacematerial(blockx, blocky, height);
        return material == WORLD_SNOWY_GRASS || material == WORLD_BIOME_SNOW;
    }

    bool sampleworldtree(worldgencontext *generation, int blockx, int blocky, int &base, int &height, uint &shape, int &species)
    {
        if(!generation || generation->iscanceled()) return false;
        return generation->generator.tree(blockx, blocky, base, height, shape, species) && !generation->iscanceled();
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
        ::generateworldscatter(root, chunkx, chunky, generation->settings, scatter, generation->indexedtextures);
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
} // namespace game
