#ifndef __GAME_WORLD_H__
#define __GAME_WORLD_H__

#ifdef SQRT3
#pragma push_macro("SQRT3")
#undef SQRT3
#define RESTORE_WORLD_SQRT3
#endif
#include "FastNoiseLite.h"
#ifdef RESTORE_WORLD_SQRT3
#pragma pop_macro("SQRT3")
#undef RESTORE_WORLD_SQRT3
#endif

struct stream;

namespace game
{
    enum worldbiome
    {
        WORLD_BIOME_OCEAN,
        WORLD_BIOME_SNOW,
        WORLD_BIOME_DESERT,
        WORLD_BIOME_FOREST,
        WORLD_BIOME_PLAINS
    };

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
        float temperaturefrequency, moisturefrequency, biomevariationfrequency;
        float biomevariationstrength, rockfrequency;
        float deserttemperature, desertmoisture, forestmoisture;
        float foresttreedensity, plainstreedensity;
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

    struct worldgenerator
    {
        FastNoiseLite geology, hills, coastshape, coastdetail, covenoise, oceanregional, beachnoise, cliffnoise;
        FastNoiseLite mountainrange, mountainnoise, mountainpeaks;
        FastNoiseLite secondarysummita, secondarysummitb, hollowshape, foldnoise, clusenoise;
        FastNoiseLite terrainmicro, terrainmicromask;
        FastNoiseLite tectonicnoise, tectonicwarp;
        FastNoiseLite temperature, moisture, biomevariation, biomeblend, rockiness;
        FastNoiseLite caves, largecaves, tunnela, tunnelb, lakeshape;
        FastNoiseLite fracturecorridors, fracturevertical;
        worldsettings settings;
        int seed;
        float foldcos, foldsin;
        mutable hashtable<ivec, int> treeblockcache;

        worldgenerator(int seed, const worldsettings &settings = worldsettings());

        worldtectonicsample tectonics(int x, int y, float cavedepth = 0) const;
        float beachtransitionwidth(int x, int y) const;
        float maxbeachtransitionwidth() const;
        float coasttransitionwidth(int x, int y) const;
        float maxcoasttransitionwidth() const;
        bool beach(int x, int y) const;
        bool coast(int x, int y) const;
        float fracturecorridor(int x, int y) const;
        int height(int x, int y, worldtectonicsample *tectonics = NULL) const;
        int biome(int x, int y, int height) const;
        bool cliff(int x, int y, int height) const;
        bool rock(int x, int y, int height) const;
        int treeblock(int x, int y, int z) const;
    };

    extern int getworldseed();
    extern int getconfiguredworldseed();
    extern void loadworldseed(int seed);
    extern void activateworldseed();
    extern void saveworldsettings(stream *f);
}

#endif
