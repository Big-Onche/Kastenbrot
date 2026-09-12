#ifndef __GAME_WORLDGEN_H__
#define __GAME_WORLDGEN_H__

#include <SDL_atomic.h>

#include "world/world.h"
#include "world/climate.h"

struct cube;
struct stream;
struct worldgencontext;
struct worldscatterinstance;
struct worldsectionrenderdata;
template<class T> struct vector;

enum worldsurfacematerial
{
    WORLD_SURFACE_GRASS = 0,
    WORLD_SURFACE_STONE,
    WORLD_SURFACE_SAND,
    WORLD_SURFACE_SNOW,
    WORLD_SURFACE_DIRT,
    WORLD_SURFACE_FROZEN_GRASS,
    WORLD_SURFACE_FROZEN_DIRT,
    WORLD_SURFACE_FROZEN_MOSS,
    WORLD_SURFACE_FROZEN_GRAVEL,
    WORLD_SURFACE_SNOW_CRUST,
    WORLD_SURFACE_DEEP_SNOW,
    WORLD_SURFACE_ICE,
    WORLD_SURFACE_MOSS,
    WORLD_SURFACE_STONE_BASE = 1 << 5
};

struct worldsurfacesample
{
    int height, waterheight, material;
    bool water;

    worldsurfacesample() : height(0), waterheight(0), material(WORLD_SURFACE_GRASS), water(false) {}
};

namespace game
{
    struct worldgenerator
    {
        FastNoiseLite geology, hills, coastshape, coastdetail, covenoise, oceanregional, beachnoise, cliffnoise;
        FastNoiseLite mountainrange, mountainnoise, mountainpeaks;
        FastNoiseLite secondarysummita, secondarysummitb, hollowshape, foldnoise, clusenoise;
        FastNoiseLite terrainmicro, terrainmicromask, plainsroll, deeprock;
        FastNoiseLite tectonicnoise, tectonicwarp;
        FastNoiseLite biomeblend, rockiness;
        FastNoiseLite caves, largecaves, tunnela, tunnelb, lakeshape;
        FastNoiseLite fracturecorridors, fracturevertical;
        worldclimate environmentclimate;
        FastNoiseLite vegetationvariation, snowpatches;
        FastNoiseLite coldbroad, coldroll, coldmicro, coldregions;
        FastNoiseLite biomeedgewarp;
        worldsettings settings;
        int seed;
        float foldcos, foldsin;
        mutable hashtable<ivec, int> treeblockcache;
        mutable worldhydrology *hydrology;

        worldgenerator(int seed, const worldsettings &settings = worldsettings());
        ~worldgenerator();
        worldgenerator(const worldgenerator &) = delete;
        worldgenerator &operator=(const worldgenerator &) = delete;

        worldtectonicsample tectonics(int x, int y, float cavedepth = 0) const;
        float beachtransitionwidth(int x, int y) const;
        float maxbeachtransitionwidth() const;
        float coasttransitionwidth(int x, int y) const;
        float maxcoasttransitionwidth() const;
        bool beach(int x, int y) const;
        bool coast(int x, int y) const;
        float fracturecorridor(int x, int y) const;
        int height(int x, int y, worldtectonicsample *tectonics = NULL) const;
        int baseheight(int x, int y, worldtectonicsample *tectonics = NULL) const;
        worldwatersample surface(int x, int y) const;
        // Absolute engine coordinates; includes continuous coast and freshwater influence.
        float gethumidity(const vec &worldpos) const;
        vec terrainclimate(const vec &absolute, bool transition = true) const;
        // Absolute engine coordinates, using the same physical climate as vegetation and F2.
        BiomeSample sampleBiome(const vec &worldpos) const;
        int biome(int x, int y, int height) const;
        // Surface codes include legacy soil values and coldmaterial; ecosystem identity is sampled separately.
        int surfacematerial(int x, int y, int height) const;
        // Simple bounded noisy soil edge, shared by surface and vegetation queries.
        float sandcoverage(const BiomeSample &soil) const;
        BiomeSample samplesoil(const vec &position) const;
        // Surface snow mask in absolute block coordinates; raw climate is never modified.
        bool snowcovered(int x, int y, float temperature) const;
        ColdSample samplecold(int x, int y, int height, const BiomeSample &climate) const;
        bool cliff(int x, int y, int height, bool *face = NULL) const;
        bool rock(int x, int y, int height) const;
        bool tree(int x, int y, int &base, int &height, uint &shape, bool &pine) const;
        int treeblock(int x, int y, int z) const;
        float treedensity(int x, int y, int height) const;
    };

    // Shared main-thread sampler; generation jobs keep their own instances.
    extern worldgenerator &getenvironmentgenerator();
    extern float treesuitability(float temperature, float humidity);
    extern worldgencontext *createworldgeneration(bool prepared, bool remip, SDL_atomic_t *cancelled = NULL, bool indexedtextures = false);
    extern void destroyworldgeneration(worldgencontext *generation);
    extern void snapshotworldnpcdefinitions(worldgencontext *generation);
    extern void generateworldnpcs(worldgencontext *generation, const cube *root, int chunkx, int chunky, vector<uchar> &data,
                                 bool generated = true);
    extern bool sampleterrainheight(worldgencontext *generation, int blockx, int blocky, int &height);
    extern bool sampleterrainsurface(worldgencontext *generation, int blockx, int blocky, worldsurfacesample &surface);
    extern bool sampleworldtree(worldgencontext *generation, int blockx, int blocky, int &base, int &height, uint &shape, bool &pine);
    extern cube *generateworldchunk(worldgencontext *generation, int chunkx, int chunky, int &families, int &optimized, worldsectionrenderdata *renderdata = NULL);
    extern void generateworldscatter(worldgencontext *generation, cube *root, int chunkx, int chunky, vector<worldscatterinstance> &scatter);
    extern void generateworldscatter(cube *root, int chunkx, int chunky, vector<worldscatterinstance> &scatter);
    extern cube *generateworldchunk(int chunkx, int chunky, worldsectionrenderdata *renderdata = NULL);
    extern void freeworldchunk(cube *root);
    extern bool validgeneratedworldscatter(const cube *root, const worldscatterinstance &scatter);
    extern void cacheworldscattertransform(int chunkx, int chunky, float maxoffset, const worldscatterinstance &scatter);
    extern void cacheworldscattertransforms(int chunkx, int chunky, float maxoffset, const vector<worldscatterinstance> &scatter);
    extern float getworldscattermaxoffset();
    extern ullong worldgenerationparameterhash();
    extern void sampleworldgenerationdebug(int blockx, int blocky, int logicalz, float &activity, float &uplift, float &trench, float &caveexpansion);
    extern bool chooseworldspawn(double originx, double originy, double &spawnx, double &spawny);
}

#endif
