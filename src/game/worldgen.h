#ifndef __GAME_WORLDGEN_H__
#define __GAME_WORLDGEN_H__

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
    WORLD_SURFACE_DIRT
};

struct worldsurfacesample
{
    int height, waterheight, material;
    bool water;

    worldsurfacesample() : height(0), waterheight(0), material(WORLD_SURFACE_GRASS), water(false) {}
};

namespace game
{
    extern int getworldseed();
    extern int getconfiguredworldseed();
    extern void loadworldseed(int seed);
    extern void activateworldseed();
    extern worldgencontext *createworldgeneration(bool prepared, bool remip, SDL_atomic_t *cancelled = NULL);
    extern void destroyworldgeneration(worldgencontext *generation);
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
