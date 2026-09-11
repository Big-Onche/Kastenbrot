#ifndef __GAME_WORLD_H__
#define __GAME_WORLD_H__

#include "worldclimate.h"

struct stream;

namespace game
{
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

    enum worldtreeblock
    {
        WORLD_TREE_AIR = 0,
        WORLD_TREE_WOOD,
        WORLD_TREE_DARK_WOOD,
        WORLD_TREE_LEAVES,
        WORLD_TREE_NEEDLES
    };

    struct worldtectonicsample
    {
        float activity, landuplift, oceantrench, caveexpansion;
        float terrainroughness, terrainstructure, rockyledge;

        worldtectonicsample()
            : activity(0), landuplift(0), oceantrench(0), caveexpansion(0),
              terrainroughness(0), terrainstructure(0), rockyledge(0)
        {
        }
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
        float mountainthreshold, mountainwidth;
        float tectonicfrequency, tectonicwarpamplitude, tectonicridgepower;
        float tectonicactivitythreshold, maxlanduplift, maxoceansubsidence;
        float tectoniccavestrength, tectonicfracturestrength, coastprotectionwidth;
        float cliffchance, cliffmaxheight;
        float rockfrequency;
        float basetreedensity;
        float grassfrequency, grassdensity, grassmaxoffset;
        float flowerchance, roseweight, tulipweight, dandelionweight;
        float cavefrequency, cavethreshold, largecavefrequency;
        float largecavethreshold, largecavedeepthreshold;
        float tunnelfrequency, tunnelwidth, caveentrancewidth;
        float lavalakeshallowchance, lavalakedeepchance;
        float lavalakeshapefrequency, lavalakeshapevariation;
        int sealevel, soildepth, snowheight, stonelow, stonehigh;
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
        worldwatersample(int height = 0, int water = 0) : height(height), water(water), freshwater(false), bank(false) {}
    };

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
        FastNoiseLite vegetationvariation;
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
        // Absolute engine coordinates, using the same physical climate as vegetation and F2.
        BiomeSample sampleBiome(const vec &worldpos) const;
        int biome(int x, int y, int height) const;
        // Legacy material codes only: snow is temperature driven, independent of ecosystem identity.
        int surfacematerial(int x, int y, int height) const;
        bool cliff(int x, int y, int height, bool *face = NULL) const;
        bool rock(int x, int y, int height) const;
        bool tree(int x, int y, int &base, int &height, uint &shape, bool &pine) const;
        int treeblock(int x, int y, int z) const;
        float treedensity(int x, int y, int height) const;
    };

    // Shared main-thread sampler; generation jobs keep their own instances.
    extern worldgenerator &getenvironmentgenerator();
    extern float treesuitability(float temperature, float humidity);
    extern int getworldseed();
    extern int getconfiguredworldseed();
    extern void loadworldseed(int seed);
    extern void activateworldseed();
}

#endif
