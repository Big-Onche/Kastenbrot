#include "game.h"

VARP(worldseed, 0, 1337, INT_MAX);

FVAR(worldgeologyfrequency, 0.00001f, 0.0009f, 0.1f);
FVAR(worldmaxcontinentheight, 1.0f, 96.0f, 255.0f);
FVAR(worldmaxoceandepth, 1.0f, 32.0f, 255.0f);
FVAR(worldmegacontinentfrequency, 0.00001f, 0.00016f, 0.01f);
FVAR(worldmacrocontinentfrequency, 0.00001f, 0.00055f, 0.02f);
FVAR(worldcoastdetailfrequency, 0.0001f, 0.0025f, 0.25f);
FVAR(worldcoastdetailstrength, 0.0f, 0.075f, 0.25f);
FVAR(worldoceanregionalfrequency, 0.000001f, 0.000055f, 0.001f);
FVAR(worldoceanregionalbias, 0.0f, 0.22f, 0.75f);
FVAR(worldoceancoverage, 0.0f, 55.0f, 100.0f);
FVAR(worldterraincoverage, 0.0f, 45.0f, 100.0f);
FVAR(worldplainscoverage, 0.0f, 45.0f, 100.0f);
FVAR(worldhillscoverage, 0.0f, 32.0f, 100.0f);
FVAR(worldmountainscoverage, 0.0f, 18.0f, 100.0f);
FVAR(worldhighsummitscoverage, 0.0f, 5.0f, 100.0f);
FVAR(worldterrainmicrofrequency, 0.0001f, 0.035f, 0.5f);
FVAR(worldplainsmicrovariation, 0.0f, 1.0f, 8.0f);
FVAR(worldreliefmicrovariation, 0.0f, 6.0f, 32.0f);
FVAR(worldsecondarysummitheight, 0.0f, 14.0f, 64.0f);
FVAR(worldrockyledgeheight, 0.0f, 5.0f, 24.0f);
FVAR(worldclusedepth, 0.0f, 9.0f, 48.0f);
FVAR(worldmountainchainfrequency, 0.00005f, 0.0005f, 0.01f);
FVAR(worldmountainlocalfrequency, 0.0002f, 0.003f, 0.05f);
FVAR(worldmountainmaxamplitude, 0.0f, 250.0f, 255.0f);
FVAR(worldmountainthreshold, 0.0f, 0.52f, 1.0f);
FVAR(worldmountainwidth, 0.01f, 0.16f, 0.5f);
FVAR(worldmountainspacing, 1.0f, 1.25f, 4.0f);

FVAR(worldtectonicfrequency, 0.0001f, 0.0014f, 0.01f);
FVAR(worldtectonicwarpamplitude, 0.0f, 64.0f, 512.0f);
FVAR(worldtectonicridgepower, 0.1f, 2.2f, 8.0f);
FVAR(worldtectonicactivitythreshold, 0.0f, 0.35f, 0.95f);
FVAR(worldmaxlanduplift, 0.0f, 250.0f, 255.0f);
FVAR(worldmaxoceansubsidence, 0.0f, 100.0f, 255.0f);
FVAR(worldtectoniccavestrength, 0.0f, 0.35f, 1.0f);
FVAR(worldtectonicfracturestrength, 0.0f, 0.40f, 1.0f);
FVAR(worldcoastprotectionwidth, 0.0f, 32.0f, 256.0f);
FVAR(worldcliffchance, 0.0f, 24.0f, 100.0f);
FVAR(worldcliffmaxheight, 2.0f, 21.0f, 255.0f);

FVAR(worldrockfrequency, 0.000001f, 0.08f, 1.0f);

VAR(worldsealevel, -255, 0, 255);
VAR(worldsoildepth, 2, 5, 6);
// Scaled-world cooling in Celsius per metre (one block), independent of regional temperature.
FVAR(worldtemperaturelapserate, 0.0f, 0.065f, 1.0f);
VAR(worldstonelow, -255, 120, 255);
VAR(worldstonehigh, -255, 200, 255);
VAR(worldbiomeblend, 0, 16, 64);
VAR(worldcoastwidth, 0, 8, 32);
VAR(worldcoastvariation, 0, 3, 16);
VAR(worldbeachminheight, -32, -2, 32);
VAR(worldbeachmaxheight, -32, 1, 32);

FVAR(worldbasetreedensity, 0.0f, 0.018f, 0.25f);
FVAR(worldgrassfrequency, 0.00001f, 0.02f, 1.0f);
FVAR(worldgrassdensity, 0.0f, 0.35f, 1.0f);
FVAR(worldgrassmaxoffset, 0.0f, 0.18f, 0.45f);
FVAR(worldflowerchance, 0.0f, 0.18f, 1.0f);
FVAR(worldroseweight, 0.0f, 1.0f, 100.0f);
FVAR(worldtulipweight, 0.0f, 1.0f, 100.0f);
FVAR(worlddandelionweight, 0.0f, 1.0f, 100.0f);
VAR(worldpinestartheight, -255, 80, 255);
VAR(worldpinefullheight, -255, 160, 255);

FVAR(worldcavefrequency, 0.0001f, 0.045f, 0.25f);
FVAR(worldcavethreshold, -1.0f, 0.58f, 1.0f);
FVAR(worldlargecavefrequency, 0.0001f, 0.018f, 0.25f);
FVAR(worldlargecavethreshold, -1.0f, 0.76f, 1.0f);
FVAR(worldlargecavedeepthreshold, -1.0f, 0.58f, 1.0f);
FVAR(worldtunnelfrequency, 0.0001f, 0.025f, 0.25f);
FVAR(worldtunnelwidth, 0.001f, 0.075f, 0.3f);
FVAR(worldcaveentrancewidth, 0.001f, 0.05f, 0.3f);
VAR(worldcavemindepth, 1, 12, 64);
VAR(worldcavefulldepth, 1, 32, 128);
VAR(worldcavedeepheight, -255, -64, 255);

VAR(worldbottomlavalayers, 0, 3, 16);
VAR(worldlavalakestartheight, -255, -16, 255);
VAR(worldlavalakedeepheight, -255, -64, 255);
FVAR(worldlavalakeshallowchance, 0.0f, 0.03f, 1.0f);
FVAR(worldlavalakedeepchance, 0.0f, 0.22f, 1.0f);
VAR(worldlavalakeminsize, 1, 4, 32);
VAR(worldlavalakemaxsize, 1, 14, 32);
VAR(worldlavalakespacing, 8, 24, 64);
FVAR(worldlavalakeshapefrequency, 0.001f, 0.08f, 1.0f);
FVAR(worldlavalakeshapevariation, 0.0f, 0.35f, 0.75f);

namespace game
{
    static int activeworldseed = 1337;

    float smoothstep(float low, float high, float value)
    {
        if(high <= low) return value >= high ? 1.0f : 0.0f;
        const float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    void setupnoise(FastNoiseLite &noise, int seed, float frequency, int octaves, float gain)
    {
        noise.SetSeed(seed);
        noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        noise.SetFrequency(frequency);
        noise.SetFractalType(octaves > 1 ? FastNoiseLite::FractalType_FBm : FastNoiseLite::FractalType_None);
        noise.SetFractalOctaves(octaves);
        noise.SetFractalLacunarity(1.8f);
        noise.SetFractalGain(gain);
    }

    static void setupwarp(FastNoiseLite &warp, int seed, float frequency, float amplitude)
    {
        warp.SetSeed(seed);
        warp.SetDomainWarpType(FastNoiseLite::DomainWarpType_OpenSimplex2);
        warp.SetFrequency(frequency);
        warp.SetDomainWarpAmp(amplitude);
    }

    worldtectonicsample::worldtectonicsample()
        : activity(0), landuplift(0), oceantrench(0), caveexpansion(0), terrainroughness(0), terrainstructure(0), rockyledge(0), grassplateau(0),
          grassplateaudetail(0), hillrock(0)
    {
    }
    worldwatersample::worldwatersample(int height, int water) : height(height), water(water), freshwater(false), bank(false) {}
    worldgenerator::treequery::treequery() : base(0), height(0), species(TREE_REGULAR), shape(0), priority(0), valid(false) {}

    worldgenerator::seaicequery::seaicequery() : bottom(0), top(0), covered(false) {}

    uint worldspatialhash(uint seed, int x, int y, uint salt)
    {
        uint hash = seed ^ salt;
        hash ^= uint(x) * 0x9E3779B9U;
        hash ^= uint(y) * 0x85EBCA6BU;
        hash ^= hash >> 16;
        hash *= 0x7FEB352DU;
        hash ^= hash >> 15;
        hash *= 0x846CA68BU;
        hash ^= hash >> 16;
        return hash;
    }

    float worldspatialunit(uint seed, int x, int y, uint salt)
    {
        return float(worldspatialhash(seed, x, y, salt) & 0x00FFFFFFU) / float(0x01000000U);
    }

    worldsettings::worldsettings()
        : geologyfrequency(worldgeologyfrequency), maxcontinentheight(worldmaxcontinentheight), maxoceandepth(worldmaxoceandepth),
          megacontinentfrequency(worldmegacontinentfrequency), macrocontinentfrequency(worldmacrocontinentfrequency),
          coastdetailfrequency(worldcoastdetailfrequency), coastdetailstrength(worldcoastdetailstrength),
          oceanregionalfrequency(worldoceanregionalfrequency), oceanregionalbias(worldoceanregionalbias), oceancoverage(worldoceancoverage),
          terraincoverage(worldterraincoverage), plainscoverage(worldplainscoverage), hillscoverage(worldhillscoverage),
          mountainscoverage(worldmountainscoverage), highsummitscoverage(worldhighsummitscoverage), terrainmicrofrequency(worldterrainmicrofrequency),
          plainsmicrovariation(worldplainsmicrovariation), reliefmicrovariation(worldreliefmicrovariation),
          secondarysummitheight(worldsecondarysummitheight), rockyledgeheight(worldrockyledgeheight), clusedepth(worldclusedepth),
          mountainchainfrequency(worldmountainchainfrequency), mountainlocalfrequency(worldmountainlocalfrequency),
          mountainmaxamplitude(worldmountainmaxamplitude), mountainthreshold(worldmountainthreshold), mountainwidth(worldmountainwidth),
          mountainspacing(worldmountainspacing), tectonicfrequency(worldtectonicfrequency), tectonicwarpamplitude(worldtectonicwarpamplitude),
          tectonicridgepower(worldtectonicridgepower), tectonicactivitythreshold(worldtectonicactivitythreshold), maxlanduplift(worldmaxlanduplift),
          maxoceansubsidence(worldmaxoceansubsidence), tectoniccavestrength(worldtectoniccavestrength),
          tectonicfracturestrength(worldtectonicfracturestrength), coastprotectionwidth(worldcoastprotectionwidth), cliffchance(worldcliffchance),
          cliffmaxheight(worldcliffmaxheight), rockfrequency(worldrockfrequency), temperaturelapserate(worldtemperaturelapserate),
          basetreedensity(worldbasetreedensity), grassfrequency(worldgrassfrequency), grassdensity(worldgrassdensity),
          grassmaxoffset(worldgrassmaxoffset), flowerchance(worldflowerchance), roseweight(worldroseweight), tulipweight(worldtulipweight),
          dandelionweight(worlddandelionweight), cavefrequency(worldcavefrequency), cavethreshold(worldcavethreshold),
          largecavefrequency(worldlargecavefrequency), largecavethreshold(worldlargecavethreshold),
          largecavedeepthreshold(worldlargecavedeepthreshold), tunnelfrequency(worldtunnelfrequency), tunnelwidth(worldtunnelwidth),
          caveentrancewidth(worldcaveentrancewidth), lavalakeshallowchance(worldlavalakeshallowchance), lavalakedeepchance(worldlavalakedeepchance),
          lavalakeshapefrequency(worldlavalakeshapefrequency), lavalakeshapevariation(worldlavalakeshapevariation), sealevel(worldsealevel),
          soildepth(worldsoildepth), stonelow(worldstonelow), stonehigh(worldstonehigh), biomeblend(worldbiomeblend), coastwidth(worldcoastwidth),
          coastvariation(worldcoastvariation), beachminheight(worldbeachminheight), beachmaxheight(worldbeachmaxheight),
          pinestartheight(worldpinestartheight), pinefullheight(worldpinefullheight), cavemindepth(worldcavemindepth),
          cavefulldepth(worldcavefulldepth), cavedeepheight(worldcavedeepheight), bottomlavalayers(worldbottomlavalayers),
          lavalakestartheight(worldlavalakestartheight), lavalakedeepheight(worldlavalakedeepheight), lavalakeminsize(worldlavalakeminsize),
          lavalakemaxsize(worldlavalakemaxsize), lavalakespacing(worldlavalakespacing)
    {
    }

    worldgenerator::worldgenerator(int seed, const worldsettings &settings)
        : environmentclimate(seed, settings.sealevel, settings.temperaturelapserate), settings(settings), seed(seed), treeblockcache(1 << 12),
          hydrology(NULL)
    {
        setupnoise(biomeedgewarp, seed ^ 0x4D71C923, 0.05f, 1);
        setupnoise(coldbroad, seed ^ 0x35DAA821, 0.0008f, 3);
        setupnoise(coldroll, seed ^ 0x45E19327, 0.0018f, 4);
        setupnoise(coldmicro, seed ^ 0x196AB391, 0.0060f, 2);
        setupnoise(coldregions, seed ^ 0x2F59C741, 0.0009f, 3);
        setupnoise(snowpatches, seed ^ 0x71D49A23, 0.035f, 2, 0.35f);
        setupnoise(vegetationvariation, seed ^ 0x4C87A219, 0.006f, 2, 0.35f);
        // Isotropic, unwarped mega noise owns the continental topology. Macro
        // noise adds lobes and inland seas without bending the whole landmass.
        setupnoise(geology, seed, settings.megacontinentfrequency, 1);
        setupnoise(covenoise, seed ^ 0x2B61D4A7, settings.macrocontinentfrequency, 2, 0.32f);
        setupnoise(oceanregional, seed ^ 0x0D84A91F, settings.oceanregionalfrequency, 1);
        setupnoise(hills, seed ^ 0x4A39B70D, settings.macrocontinentfrequency * 1.8f, 2, 0.30f);
        setupnoise(coastshape, seed ^ 0x57C8E219, settings.macrocontinentfrequency * 3.0f, 1);
        setupnoise(coastdetail, seed ^ 0x1F6D38A5, settings.coastdetailfrequency, 3, 0.48f);
        setupnoise(beachnoise, seed ^ 0x73A9C52D, settings.macrocontinentfrequency * 2.0f, 1);
        setupnoise(cliffnoise, seed ^ 0x4E91A73B, settings.macrocontinentfrequency * 2.5f, 1);
        // Long ridged chain envelopes gate all local mountain relief. Independent
        // local fields provide multiple peaks, valleys, saddles, and foothills.
        setupnoise(mountainrange, seed ^ 0x18F47C53, settings.mountainchainfrequency * 0.55f / settings.mountainspacing, 1);
        setupnoise(mountainnoise, seed ^ 0x3D72A95B, settings.mountainlocalfrequency, 3, 0.42f);
        setupnoise(mountainpeaks, seed ^ 0x25B46D81, settings.mountainlocalfrequency * 2.1f, 2, 0.36f);
        // Reuse one independent octave at two scales for patchy foothills, smaller summits and rolling mountain meadows.
        setupnoise(foothillgeology, seed ^ 0x53C91B27, settings.mountainlocalfrequency * 0.65f, 1);
        setupnoise(secondarysummita, seed ^ 0x41D7A2C9, settings.mountainlocalfrequency * 1.45f, 2, 0.40f);
        setupnoise(secondarysummitb, seed ^ 0x6B2E935D, settings.mountainlocalfrequency * 1.85f, 2, 0.40f);
        setupnoise(hollowshape, seed ^ 0x2C85F1B7, settings.mountainlocalfrequency * 0.75f, 2, 0.35f);
        setupnoise(foldnoise, seed ^ 0x59A34E21, settings.mountainlocalfrequency * 0.60f, 2, 0.35f);
        setupnoise(clusenoise, seed ^ 0x17C6B8F3, settings.mountainlocalfrequency * 0.45f, 2, 0.35f);
        foldnoise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
        clusenoise.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
        setupnoise(terrainmicro, seed ^ 0x34A72C91, settings.terrainmicrofrequency, 4, 0.48f);
        setupnoise(terrainmicromask, seed ^ 0x62E9B4D7, settings.terrainmicrofrequency * 0.25f, 2, 0.45f);
        setupnoise(plainsroll, seed ^ 0x39C6A17D, settings.terrainmicrofrequency * 0.35f, 2, 0.35f);
        setupnoise(deeprock, seed ^ 0x53B8D291, 0.015f, 1);
        setupnoise(tectonicnoise, seed ^ 0x68E31DA4, settings.mountainchainfrequency / settings.mountainspacing, 1);
        setupwarp(tectonicwarp, seed ^ 0x6C8E9CF5, settings.tectonicfrequency * 0.8f, min(settings.tectonicwarpamplitude, 36.0f));
        setupnoise(biomeblend, seed ^ 0x13C6E91F, settings.biomeblend > 0 ? 1.0f / settings.biomeblend : 1.0f, 1);
        setupnoise(rockiness, seed ^ 0x5E4A19C3, settings.rockfrequency, 2);
        setupnoise(caves, seed ^ 0x7A84F12D, settings.cavefrequency, 2);
        setupnoise(largecaves, seed ^ 0x36B9C7E5, settings.largecavefrequency, 2);
        setupnoise(tunnela, seed ^ 0x19F3A6C7, settings.tunnelfrequency, 2);
        setupnoise(tunnelb, seed ^ 0x5C2D8E91, settings.tunnelfrequency, 2);
        setupnoise(lakeshape, seed ^ 0x43E7B5D9, settings.lavalakeshapefrequency, 2);
        setupnoise(fracturecorridors, seed ^ 0x278D4A6B, settings.tunnelfrequency * 0.35f, 1);
        setupnoise(fracturevertical, seed ^ 0x71B5C3D9, settings.tunnelfrequency, 1);

        const unsigned int anglehash = unsigned(seed) * 0x9E3779B9U + 0x7F4A7C15U;
        const float foldangle = float(anglehash & 0xFFFFU) * (6.28318530718f / 65536.0f);
        foldcos = cosf(foldangle);
        foldsin = sinf(foldangle);
    }

    int getworldseed()
    {
        return activeworldseed;
    }

    int getconfiguredworldseed()
    {
        return worldseed;
    }

    void loadworldseed(int seed)
    {
        worldseed = max(seed, 0);
        activeworldseed = worldseed;
    }

    void activateworldseed()
    {
        loadworldseed(worldseed);
    }

} // namespace game

#ifndef STANDALONE
ICOMMAND(worldloadseed, "i", (int *seed), {
    if(game::waitforserveredit())
    {
        conoutf(CON_ERROR, "the multiplayer server owns the world seed");
        return;
    }
    game::loadworldseed(*seed);
});
#endif
