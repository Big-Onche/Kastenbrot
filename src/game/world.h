#ifndef __GAME_WORLD_H__
#define __GAME_WORLD_H__

#include <SDL_atomic.h>
#ifdef SQRT3
#pragma push_macro("SQRT3")
#undef SQRT3
#define RESTORE_CLIMATE_SQRT3
#endif
#include "FastNoiseLite.h"
#ifdef RESTORE_CLIMATE_SQRT3
#pragma pop_macro("SQRT3")
#undef RESTORE_CLIMATE_SQRT3
#endif

struct npcdefinition;
#include "../engine/world.h"
#include "../engine/worldcube.h"
#include "../engine/worldruntime.h"

namespace game
{
    // Absolute engine coordinates: one 16-unit block is one metre; terrain datum is z = 4096.
    // Convert floating-origin camera coordinates to absolute coordinates before sampling.
    // Fixed regional frequencies (per metre) make baseline climate reproducible from the saved seed.
    struct worldclimate
    {
        enum
        {
            BLOCK_UNITS = 16,
            GROUND_UNITS = 4096
        };
        FastNoiseLite regionaltemperature, regionalhumidity;
        float sealevel, temperaturelapserate;

        worldclimate(int seed, float sealevel, float temperaturelapserate);

        float getaltitude(const vec &worldpos) const;

        float getregionaltemperature(const vec &worldpos) const;

        float gettemperature(const vec &worldpos) const;

        static float transition(float low, float high, float value);

        // Geography supplies continuous proximity weights, without coupling climate to terrain generation.
        // The altitude band is measured in blocks/metres above sea level; weather remains separate.
        float gethumidity(const vec &worldpos, float coast = 0, float freshwater = 0) const;
    };
    extern float getbeachtintdistance(float shore, float width, float height, float low, float high, float slope);
    extern vec interpolatevegetationcolor(const vec (&colors)[3][3], float temperatureC, float humidityPercent);
    extern vec getgrassclimatecolor(float temperatureC, float humidityPercent);
    extern vec getweedclimatecolor(float temperatureC, float humidityPercent);
    enum worldbiome
    {
        WORLD_BIOME_OCEAN,
        WORLD_BIOME_SNOW,
        WORLD_BIOME_DESERT,
        WORLD_BIOME_FOREST,
        WORLD_BIOME_PLAINS,
        WORLD_BIOME_TAIGA,
        WORLD_BIOME_COLD_DESERT,
        WORLD_BIOME_SAVANNA,
        WORLD_BIOME_RAINFOREST,
        WORLD_BIOME_SNOW_DESERT,
        WORLD_BIOME_COUNT,
        WORLD_BIOME_TUNDRA = WORLD_BIOME_SNOW
    };

    struct ClimateBiome
    {
        worldbiome type;
        const char *name, *identifier;
        float temperatureCenter, humidityCenter, temperatureRange, humidityRange, treeDensity;
    };

    extern const ClimateBiome climateBiomes[];
    extern const int climateBiomeCount;
    extern const char *biomeName(int biome);

    struct BiomeSample
    {
        worldbiome primary, secondary;
        float primaryWeight, secondaryWeight, temperature, humidity;
        // Indexed by worldbiome; normalized over ALL climate candidates, not just the strongest two.
        float weights[WORLD_BIOME_COUNT];
    };

    extern BiomeSample sampleClimateBiome(float temperature, float humidity);

    // Surface codes are independent of ecosystem IDs; old biome-valued soil codes remain compatible.
    enum coldmaterial
    {
        WORLD_SNOWY_GRASS = WORLD_BIOME_COUNT,
        WORLD_FROZEN_DIRT,
        WORLD_FROZEN_MOSS,
        WORLD_FROZEN_GRAVEL,
        WORLD_COLD_ROCK,
        WORLD_SNOW_CRUST,
        WORLD_DEEP_SNOW,
        WORLD_ICE,
        WORLD_FROZEN_WATER,
        WORLD_MOSS
    };

    struct ColdSample
    {
        float coldness, desert, slope, basin, exposure, deposition, snow, vegetation, region;
        float severity, wetness, nearwater, grassscore, dirtscore, mossscore, gravelscore, snowscore, pinemask;
        int material;
        bool covered;
    };

    enum worldtreeblock
    {
        WORLD_TREE_AIR = 0,
        WORLD_TREE_WOOD,
        WORLD_TREE_DARK_WOOD,
        WORLD_TREE_LEAVES,
        WORLD_TREE_NEEDLES,
        WORLD_TREE_PALM_WOOD,
        WORLD_TREE_BIRCH_WOOD,
        WORLD_TREE_PALM_LEAVES,
        WORLD_TREE_BIRCH_LEAVES,
        WORLD_TREE_BLOCK_COUNT
    };

    struct worldtectonicsample
    {
        float activity, landuplift, oceantrench, caveexpansion;
        float terrainroughness, terrainstructure, rockyledge, grassplateau, grassplateaudetail, hillrock;

        worldtectonicsample();
    };

    struct worldsettings
    {
        float geologyfrequency, maxcontinentheight, maxoceandepth;
        float megacontinentfrequency, macrocontinentfrequency;
        float coastdetailfrequency, coastdetailstrength;
        float oceanregionalfrequency, oceanregionalbias;
        float oceancoverage, terraincoverage;
        float plainscoverage, hillscoverage, mountainscoverage, highsummitscoverage;
        float terrainmicrofrequency, plainsmicrovariation, reliefmicrovariation;
        float secondarysummitheight, rockyledgeheight, clusedepth;
        float mountainchainfrequency, mountainlocalfrequency, mountainmaxamplitude;
        float mountainthreshold, mountainwidth, mountainspacing;
        float tectonicfrequency, tectonicwarpamplitude, tectonicridgepower;
        float tectonicactivitythreshold, maxlanduplift, maxoceansubsidence;
        float tectoniccavestrength, tectonicfracturestrength, coastprotectionwidth;
        float cliffchance, cliffmaxheight;
        float rockfrequency, temperaturelapserate;
        float basetreedensity;
        float grassfrequency, grassdensity, grassmaxoffset;
        float flowerchance, roseweight, tulipweight, dandelionweight;
        float cavefrequency, cavethreshold, largecavefrequency;
        float largecavethreshold, largecavedeepthreshold;
        float tunnelfrequency, tunnelwidth, caveentrancewidth;
        float lavalakeshallowchance, lavalakedeepchance;
        float lavalakeshapefrequency, lavalakeshapevariation;
        int sealevel, soildepth, stonelow, stonehigh;
        int biomeblend, coastwidth, coastvariation;
        int beachminheight, beachmaxheight;
        int pinestartheight, pinefullheight;
        int cavemindepth, cavefulldepth, cavedeepheight;
        int bottomlavalayers, lavalakestartheight, lavalakedeepheight;
        int lavalakeminsize, lavalakemaxsize, lavalakespacing;

        worldsettings();
    };

    struct worldhydrology;
    struct worldwatersample
    {
        int height, water;
        bool freshwater, bank;
        worldwatersample(int height = 0, int water = 0);
    };

    extern int getworldseed();
    extern int getconfiguredworldseed();
    extern void loadworldseed(int seed);
    extern void activateworldseed();

} // namespace game

struct cube;
struct stream;
struct worldgencontext;
struct worldscatterinstance;
struct worldsectionrenderdata;
template <class T> struct vector;

enum worldsurfacematerial
{
    WORLD_SURFACE_GRASS = 0,
    WORLD_SURFACE_STONE,
    WORLD_SURFACE_SAND,
    WORLD_SURFACE_SNOW,
    WORLD_SURFACE_DIRT,
    WORLD_SURFACE_SNOWY_GRASS,
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

    worldsurfacesample();
};

namespace game
{
    float treepinechance(const worldsettings &settings, const BiomeSample &sample, uint seed, int x, int y, int height);
    enum worldtreespecies
    {
        TREE_REGULAR,
        TREE_PINE,
        TREE_PALM,
        TREE_BIRCH,
        TREE_POPLAR,
        TREE_ACACIA,
        TREE_SPECIES_COUNT
    };
    enum
    {
        TREE_RADIUS = 5
    };
    int treefinalheight(int species, float temperature, uint shape);
    // Relative block coordinates; shared by chunk generation, snow queries and both LOD tiers.
    int treeshapeblock(int species, int height, uint shape, int x, int y, int z);
    int treeshaperadius(int species);
    bool treewood(int type);
    const char *treeblockname(int type);

    struct worldgenerator
    {
        FastNoiseLite geology, hills, coastshape, coastdetail, covenoise, oceanregional, beachnoise, cliffnoise;
        FastNoiseLite mountainrange, mountainnoise, mountainpeaks, foothillgeology;
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
        struct treequery
        {
            int base, height, species;
            uint shape, priority;
            bool valid;
            treequery();
        };
        mutable hashtable<ivec, treequery> treequerycache;
        mutable hashtable<ivec, treequery> treecandidatecache;
        mutable hashtable<ivec, int> treeblockcache, canopyheightcache;
        struct seaicequery
        {
            float bottom, top;
            bool covered;
            seaicequery();
        };
        mutable hashtable<ivec, seaicequery> seaicecache;
        mutable hashtable<ivec, float> icecoastcache;
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
        float freshwatermoisture(float x, float y, float z) const;
        vec terrainclimate(const vec &absolute, bool transition = true) const;
        // Absolute engine coordinates, using the same physical climate as vegetation and F2.
        BiomeSample sampleBiome(const vec &worldpos) const;
        int biome(int x, int y, int height) const;
        // Surface codes include legacy soil values and coldmaterial; ecosystem identity is sampled separately.
        int surfacematerial(int x, int y, int height, const BiomeSample *climate = NULL) const;
        // Simple bounded noisy soil edge, shared by surface and vegetation queries.
        float sandcoverage(const BiomeSample &soil) const;
        BiomeSample samplesoil(const vec &position) const;
        // Surface snow mask in absolute block coordinates; raw climate is never modified.
        bool snowcovered(int x, int y, float temperature) const;
        ColdSample samplecold(int x, int y, int height, const BiomeSample &climate) const;
        bool cliff(int x, int y, int height, bool *face = NULL) const;
        bool rock(int x, int y, int height) const;
        bool tree(int x, int y, int &base, int &height, uint &shape, int &species) const;
        int treeblock(int x, int y, int z) const;
        int treecanopyheight(int x, int y) const;
        int treegroundmaterial(int x, int y, int height, int material) const;
        float treedensity(int x, int y, int height) const;
        bool treeweights(int x, int y, int height, const BiomeSample &sample, float (&weights)[TREE_SPECIES_COUNT], int material = -1,
                         float spawn = -1.0f) const;
        float icecoastdistance(int x, int y) const;
        bool iceformation(int x, int y, int height, int &bottom, int &top) const;
        bool coastice(int x, int y, float temperature, float margin = 0.0f) const;
        bool seaice(int x, int y, int height, float &bottom, float &top) const;
        // Solid ice interval in absolute terrain blocks; top is exclusive.
        bool icecolumn(int x, int y, int height, int &bottom, int &top) const;
    };

    // Shared main-thread sampler; generation jobs keep their own instances.
    extern worldgenerator &getenvironmentgenerator();
    extern float treesuitability(float temperature, float humidity);
    extern worldgencontext *createworldgeneration(bool prepared, bool remip, SDL_atomic_t *cancelled = NULL, bool indexedtextures = false);
    extern void destroyworldgeneration(worldgencontext *generation);
    extern void snapshotworldnpcdefinitions(worldgencontext *generation);
    extern void generateworldnpcs(worldgencontext *generation, const cube *root, int chunkx, int chunky, vector<uchar> &data, bool generated = true);
    extern bool sampleterrainheight(worldgencontext *generation, int blockx, int blocky, int &height);
    extern bool sampleterrainsurface(worldgencontext *generation, int blockx, int blocky, worldsurfacesample &surface);
    extern bool sampleworldsnow(worldgencontext *generation, int blockx, int blocky);
    extern bool sampleworldtree(worldgencontext *generation, int blockx, int blocky, int &base, int &height, uint &shape, int &species);
    extern cube *generateworldchunk(worldgencontext *generation, int chunkx, int chunky, int &families, int &optimized,
                                    worldsectionrenderdata *renderdata = NULL);
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
} // namespace game

namespace game
{
    // Common deterministic sampling primitives used by independent world features.
    extern float smoothstep(float low, float high, float value);
    extern void setupnoise(FastNoiseLite &noise, int seed, float frequency, int octaves, float gain = 0.5f);
    extern float samplecontinental(const worldgenerator &generator, float noisex, float noisey);
    extern float samplecoastdistance(const worldgenerator &generator, float noisex, float noisey, float continental);
    extern uint worldspatialhash(uint seed, int x, int y, uint salt);
    extern float worldspatialunit(uint seed, int x, int y, uint salt);

} // namespace game

struct vec;
struct worldgencontext;

namespace game
{
    // Packed terrain shader inputs: normalized temperature, humidity, signed soil-boundary distance.
    extern vec getterrainworldclimate(const vec &absolute);
    extern vec sampleterraingenerationclimate(worldgencontext *generation, const vec &absolute, bool transition = true);
    // Main-thread rendering query, in absolute engine coordinates. RGB only; never modifies asset alpha.
    extern vec getgrassworldcolor(const vec &absolute);
    extern vec getweedworldcolor(const vec &absolute);
    // Worker-local climate query; never accesses the main-thread environment cache.
    extern vec samplegrassgenerationcolor(worldgencontext *generation, const vec &absolute);
} // namespace game

#ifndef STANDALONE
namespace game
{
    namespace environment
    {
        extern void reset();
        extern void update();
        extern void synctime(int millis, bool frozen);
        extern int gettimemillis();
        extern float getdayprogress();
        extern float gethourafter(int millis);
        extern bool istimefrozen();
        extern float getambientlightlevel();
    } // namespace environment
} // namespace game
#endif

namespace game
{
    namespace weather
    {
        void update(int seed);
        bool preparemap(const char *folder, int seed);
        void reset();
        void clearsync();
        void synctime(int seed, uint millis, float cloudspeed, float windangle);
        int getseed(int fallback);
        double gettimemillis();
        float getcloudspeed(float fallback);
        float getwindangle(float fallback);
        int getsettingsversion();
        int getcoverageversion();
        float samplecoverage(float x, float y);
        float samplecurrentovercast(float x, float y);
        float samplecurrentrain(float x, float y, float height);
        void addparticles();
    } // namespace weather
} // namespace game

// Per-job generation data shared by the feature passes below.
struct worldcavesegment
{
    vec start, end;
    float startradius, endradius, verticalscale;
    uint roughness;
    bool entrance;

    worldcavesegment(const vec &start, const vec &end, float startradius, float endradius, float verticalscale, uint roughness,
                     bool entrance = false);
};

struct worldcavechamber
{
    vec center;
    float radiusx, radiusy, radiusz, anglecos, anglesin;
    uint roughness;

    worldcavechamber(const vec &center, float radiusx, float radiusy, float radiusz, float angle, uint roughness);
};

enum
{
    WORLD_GEOLOGY_STEP = 8,
    WORLD_GEOLOGY_WIDTH = WORLD_CHUNK_BLOCKS / WORLD_GEOLOGY_STEP + 1,
    WORLD_GEOLOGY_HEIGHT = WORLD_HEIGHT_BLOCKS / WORLD_GEOLOGY_STEP + 1
};

struct worldgencontext
{
    game::worldgenerator generator;
    game::worldsettings settings;
    int heightmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    int watermap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar biomemap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar materialmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    bool snowbeachmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar sandstonedepthmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar beachmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar cliffmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar reliefcliffmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    uchar rockmap[WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS];
    float geology[WORLD_GEOLOGY_WIDTH * WORLD_GEOLOGY_WIDTH * WORLD_GEOLOGY_HEIGHT];
    int geologymaterials[3], geologytextures[3];
    vector<ivec> rockvariants;
    worldsectionrenderdata renderdata;
    vector<worldcavesegment> cavesegments;
    vector<worldcavechamber> cavechambers;
    vector<npcdefinition> npcdefinitions;
    int seed;
    vector<worldgencubetextures> cubetextures;
    mutable hashtable<const char *, int> cubeids;
    hashtable<ivec, int> surfaceheightcache;
    int errorcube;
    bool prepared, remip, indexedtextures;
    int families, optimized;
    SDL_atomic_t *cancelled;

    worldgencontext(int seed, const vector<worldgencubetextures> &cubetextures, bool prepared, bool remip, const game::worldsettings &settings,
                    SDL_atomic_t *cancelled = NULL, bool indexedtextures = false);

    bool iscanceled() const;

    int cubetype(const char *id) const;
};

enum
{
    WORLD_CARVE_NONE = 0,
    WORLD_CARVE_AIR = 1 << 0,
    WORLD_CARVE_LAVA = 1 << 1,
    WORLD_CARVE_ENTRANCE = 1 << 2,
    WORLD_CARVE_TYPE = WORLD_CARVE_AIR | WORLD_CARVE_LAVA
};

extern vector<worldgencubetextures> worldgentextures;
extern int worldgrassscatter, worldrosescatter, worldtulipscatter, worlddandelionscatter;
extern int chunkremip, leavesalpha;
extern const cube &lookupgeneratedworldcube(const cube *root, const ivec &pos);

enum
{
    WORLD_TERRAIN_EMPTY = -1,
    WORLD_TERRAIN_WATER = -2,
    WORLD_TERRAIN_MIXED = -3,
    WORLD_TERRAIN_UNSET = -4
};

enum
{
    WORLD_CLIFF_ROCK = 1,
    WORLD_CLIFF_COAST = 2
};

// Internal feature passes and shared octree operations.
extern void generateworldscatter(cube *root, int chunkx, int chunky, const game::worldsettings &settings, vector<worldscatterinstance> &scatter,
                                 bool indexedtextures = false);
extern bool validgeneratedworldscatter(const cube *root, const worldscatterinstance &scatter);
extern void cacheworldscattertransforms(int chunkx, int chunky, float maxoffset, const vector<worldscatterinstance> &scatter);
extern void cacheworldscattertransform(int chunkx, int chunky, float maxoffset, const worldscatterinstance &scatter);
extern bool placeworldores(worldgencontext &ctx, cube *root, int chunkx, int chunky);
extern bool worldcaveairat(const worldgencontext &ctx, int worldx, int worldy, int elevation);
extern bool placeworldcaves(worldgencontext &ctx, cube *root, int chunkx, int chunky);
extern bool generateworldlavalakes(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky);
extern bool placeworldice(worldgencontext &ctx, cube *root, int chunkx, int chunky);
extern bool placeworldtrees(worldgencontext &ctx, cube *root, int chunkx, int chunky);
extern long long worldfloordiv(long long value, int divisor);
extern uint hashworldfeature(uint seed, long long x, long long y, int z, uint salt);
extern int worldcarveindex(int x, int y, int blockz);
extern cube &lookupworldgenblock(worldgencontext &ctx, cube *root, const ivec &position);
extern float worldtreeunit(uint hash);
extern void markworldgencarvedsection(worldgencontext &ctx, int blockx, int blocky, int blockz, bool entrance);
extern uchar &worldgensectionflags(worldgencontext &ctx, int blockx, int blocky, int blockz);
extern int generateworldheight(const worldgencontext &ctx, int chunkx, int chunky, int blockx, int blocky,
                               game::worldtectonicsample *tectonics = NULL);
extern float worldsmoothstep(float low, float high, float value);
extern void setworldcubematerial(cube &c, int material);
extern bool setworldcubetype(cube &c, const worldgencontext &ctx, int index, int material = MAT_AIR);
extern ivec worldgenorientnormal(int orient);

extern int worldgeologicalcubetype(const worldgencontext &ctx, const ivec &o, int size);
extern bool generateworldgeology(worldgencontext &ctx, int chunkx, int chunky);
extern int worldrepresentativecubetype(const worldgencontext &ctx, const ivec &o, int size);
extern int worldcubetype(const worldgencontext &ctx, const ivec &o, int size);
extern bool generateworldheightmap(worldgencontext &ctx, int chunkx, int chunky);
extern bool generateworldcube(worldgencontext &ctx, cube &c, const ivec &o, int size, int mingridsize);
extern void markworldgenexteriorshell(worldgencontext &ctx, int chunkx, int chunky);
extern void freepreparedworldchunk(cube *root);
extern cube *allocworldgenfamily(worldgencontext &ctx);

#endif
