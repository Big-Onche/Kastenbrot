#ifndef __GAME_WORLD_H__
#define __GAME_WORLD_H__

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
        worldwatersample(int height = 0, int water = 0) : height(height), water(water), freshwater(false), bank(false) {}
    };

    extern int getworldseed();
    extern int getconfiguredworldseed();
    extern void loadworldseed(int seed);
    extern void activateworldseed();
}

#endif
