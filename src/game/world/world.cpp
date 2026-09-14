#include <map>
#include <vector>
#include <set>
#include "game.h"
#include "world/grasscolor.h"
#include "world/generation.h"

VARP(worldseed, 0, 1337, INT_MAX);

FVAR(worldgeologyfrequency, 0.00001f, 0.0012f, 0.1f);
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
FVAR(worldmountainchainfrequency, 0.00005f, 0.00058f, 0.01f);
FVAR(worldmountainlocalfrequency, 0.0002f, 0.0035f, 0.05f);
FVAR(worldmountainmaxamplitude, 0.0f, 250.0f, 255.0f);
FVAR(worldmountainthreshold, 0.0f, 0.52f, 1.0f);
FVAR(worldmountainwidth, 0.01f, 0.16f, 0.5f);

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

    static float smoothstep(float low, float high, float value)
    {
        if(high <= low) return value >= high ? 1.0f : 0.0f;
        const float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    static uint treespatialhash(uint seed, int x, int y, uint salt)
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

    static float treespatialunit(uint seed, int x, int y, uint salt)
    {
        return float(treespatialhash(seed, x, y, salt) & 0x00FFFFFFU) / float(0x01000000U);
    }

    float treepinechance(const worldsettings &settings, const BiomeSample &sample, uint seed, int x, int y, int height)
    {
        const int broadx = x >= 0 ? x / 24 : (x - 23) / 24,
                  broady = y >= 0 ? y / 24 : (y - 23) / 24;

        const float broad = treespatialunit(seed, broadx, broady, 0xA24BAED4U),
                    local = treespatialunit(seed, x, y, 0x9FB21C65U),
                    patch = clamp(0.65f * broad + 0.35f * local, 0.0f, 1.0f),

                    pinelow = float(min(settings.pinestartheight, settings.pinefullheight)),
                    pinehigh = float(max(settings.pinestartheight, settings.pinefullheight)),
                    altitude = smoothstep(pinelow, pinehigh, float(height)),

                    cooltemperate = 1.0f - smoothstep(8.0f, 14.0f, sample.temperature),
                    humidity = smoothstep(25.0f, 70.0f, sample.humidity);

        // Species follow the actual climate and elevation, including across biome boundaries.
        const float cold = 1.0f - smoothstep(-4.0f, 8.0f, sample.temperature),
                    warm = smoothstep(16.0f, 30.0f, sample.temperature),
                    chance = (0.10f + 0.55f * cold + 0.20f * cooltemperate + 0.30f * altitude +
                              0.16f * (patch - 0.5f)) * (1.0f - 0.88f * warm) * (0.85f + 0.15f * humidity);

        // Broadleaf survival fades through cold taiga and reaches zero at -4 degrees
        const float broadleaf = smoothstep(-4.0f, 4.0f, sample.temperature);
        return 1.0f - (1.0f - clamp(chance, 0.02f, 0.96f)) * broadleaf;
    }

    int treefinalheight(int species, float temperature, uint shape)
    {
        if(species == TREE_PALM) return 7 + int((shape >> 24) % 4U);
        if(species == TREE_BIRCH) return 5 + int((shape >> 24) % 3U);
        if(species == TREE_POPLAR) return 15 + int((shape >> 24) & 1U); // Including the leafy tip: 11--12 blocks.
        if(species == TREE_PINE) return 6 + int((shape >> 24) % 3U);
        else return 4 + int((shape >> 24) % 3U); // regular ones
        if(temperature <= -4.0f) return 4 + int((shape >> 24) & 1U);
        if(temperature <= 1.5f) return 5 + int((shape >> 24) % 3U);
        return 6 + int((shape >> 24) & 3U);
    }

    static void setupnoise(FastNoiseLite &noise, int seed, float frequency, int octaves, float gain = 0.5f)
    {
        noise.SetSeed(seed);
        noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        noise.SetFrequency(frequency);
        noise.SetFractalType(octaves > 1 ? FastNoiseLite::FractalType_FBm
                                         : FastNoiseLite::FractalType_None);
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

    worldsettings::worldsettings()
        : geologyfrequency(worldgeologyfrequency),
          maxcontinentheight(worldmaxcontinentheight), maxoceandepth(worldmaxoceandepth),
          megacontinentfrequency(worldmegacontinentfrequency), macrocontinentfrequency(worldmacrocontinentfrequency),
          coastdetailfrequency(worldcoastdetailfrequency),
          coastdetailstrength(worldcoastdetailstrength),
          oceanregionalfrequency(worldoceanregionalfrequency), oceanregionalbias(worldoceanregionalbias),
          oceancoverage(worldoceancoverage), terraincoverage(worldterraincoverage),
          plainscoverage(worldplainscoverage), hillscoverage(worldhillscoverage),
          mountainscoverage(worldmountainscoverage),
          highsummitscoverage(worldhighsummitscoverage),
          terrainmicrofrequency(worldterrainmicrofrequency),
          plainsmicrovariation(worldplainsmicrovariation),
          reliefmicrovariation(worldreliefmicrovariation),
          secondarysummitheight(worldsecondarysummitheight),
          rockyledgeheight(worldrockyledgeheight),
          clusedepth(worldclusedepth),
          mountainchainfrequency(worldmountainchainfrequency), mountainlocalfrequency(worldmountainlocalfrequency),
          mountainmaxamplitude(worldmountainmaxamplitude), mountainthreshold(worldmountainthreshold), mountainwidth(worldmountainwidth),
          tectonicfrequency(worldtectonicfrequency),
          tectonicwarpamplitude(worldtectonicwarpamplitude),
          tectonicridgepower(worldtectonicridgepower),
          tectonicactivitythreshold(worldtectonicactivitythreshold),
          maxlanduplift(worldmaxlanduplift), maxoceansubsidence(worldmaxoceansubsidence),
          tectoniccavestrength(worldtectoniccavestrength),
          tectonicfracturestrength(worldtectonicfracturestrength),
          coastprotectionwidth(worldcoastprotectionwidth),
          cliffchance(worldcliffchance), cliffmaxheight(worldcliffmaxheight),
          rockfrequency(worldrockfrequency), temperaturelapserate(worldtemperaturelapserate),
          basetreedensity(worldbasetreedensity),
          grassfrequency(worldgrassfrequency), grassdensity(worldgrassdensity),
          grassmaxoffset(worldgrassmaxoffset),
          flowerchance(worldflowerchance), roseweight(worldroseweight),
          tulipweight(worldtulipweight), dandelionweight(worlddandelionweight),
          cavefrequency(worldcavefrequency), cavethreshold(worldcavethreshold),
          largecavefrequency(worldlargecavefrequency),
          largecavethreshold(worldlargecavethreshold),
          largecavedeepthreshold(worldlargecavedeepthreshold),
          tunnelfrequency(worldtunnelfrequency), tunnelwidth(worldtunnelwidth),
          caveentrancewidth(worldcaveentrancewidth),
          lavalakeshallowchance(worldlavalakeshallowchance),
          lavalakedeepchance(worldlavalakedeepchance),
          lavalakeshapefrequency(worldlavalakeshapefrequency),
          lavalakeshapevariation(worldlavalakeshapevariation),
          sealevel(worldsealevel), soildepth(worldsoildepth),
          stonelow(worldstonelow), stonehigh(worldstonehigh),
          biomeblend(worldbiomeblend), coastwidth(worldcoastwidth),
          coastvariation(worldcoastvariation),
          beachminheight(worldbeachminheight), beachmaxheight(worldbeachmaxheight),
          pinestartheight(worldpinestartheight), pinefullheight(worldpinefullheight),
          cavemindepth(worldcavemindepth), cavefulldepth(worldcavefulldepth),
          cavedeepheight(worldcavedeepheight), bottomlavalayers(worldbottomlavalayers),
          lavalakestartheight(worldlavalakestartheight),
          lavalakedeepheight(worldlavalakedeepheight),
          lavalakeminsize(worldlavalakeminsize), lavalakemaxsize(worldlavalakemaxsize),
          lavalakespacing(worldlavalakespacing)
    {
    }

    worldgenerator::worldgenerator(int seed, const worldsettings &settings)
        : environmentclimate(seed, settings.sealevel, settings.temperaturelapserate), settings(settings), seed(seed), treeblockcache(1 << 12), hydrology(NULL)
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
        setupnoise(mountainrange, seed ^ 0x18F47C53, settings.mountainchainfrequency * 0.55f, 1);
        setupnoise(mountainnoise, seed ^ 0x3D72A95B, settings.mountainlocalfrequency, 3, 0.42f);
        setupnoise(mountainpeaks, seed ^ 0x25B46D81, settings.mountainlocalfrequency * 2.1f, 2, 0.36f);
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
        setupnoise(tectonicnoise, seed ^ 0x68E31DA4, settings.mountainchainfrequency, 1);
        setupwarp(tectonicwarp, seed ^ 0x6C8E9CF5, settings.tectonicfrequency * 0.8f, min(settings.tectonicwarpamplitude, 36.0f));
        setupnoise(biomeblend, seed ^ 0x13C6E91F, settings.biomeblend > 0 ? 1.0f / settings.biomeblend : 1.0f, 1);
        setupnoise(rockiness, seed ^ 0x5E4A19C3, settings.rockfrequency, 2);
        setupnoise(caves, seed ^ 0x7A84F12D, settings.cavefrequency, 2);
        setupnoise(largecaves, seed ^ 0x36B9C7E5, settings.largecavefrequency, 2);
        setupnoise(tunnela, seed ^ 0x19F3A6C7, settings.tunnelfrequency, 2);
        setupnoise(tunnelb, seed ^ 0x5C2D8E91, settings.tunnelfrequency, 2);
        setupnoise(lakeshape, seed ^ 0x43E7B5D9, settings.lavalakeshapefrequency, 2);
        setupnoise(fracturecorridors, seed ^ 0x278D4A6B,settings.tunnelfrequency * 0.35f, 1);
        setupnoise(fracturevertical, seed ^ 0x71B5C3D9, settings.tunnelfrequency, 1);

        const unsigned int anglehash = unsigned(seed) * 0x9E3779B9U + 0x7F4A7C15U;
        const float foldangle = float(anglehash & 0xFFFFU) * (6.28318530718f / 65536.0f);
        foldcos = cosf(foldangle);
        foldsin = sinf(foldangle);
    }

    static float landthreshold(const worldsettings &settings)
    {
        const float coverage = settings.oceancoverage + settings.terraincoverage;
        const float oceanratio = coverage > 0.0f ? settings.oceancoverage / coverage : 0.5f;

        return oceanratio <= 0.0f ? -0.98f : oceanratio >= 1.0f ? 0.98f : oceanratio - 0.5f;
    }

    static float samplecontinental(const worldgenerator &generator, float noisex, float noisey)
    {
        const float threshold = landthreshold(generator.settings),
                    mega = generator.geology.GetNoise(noisex, noisey),
                    macro = generator.covenoise.GetNoise(noisex, noisey),
                    regionalbias = generator.oceanregional.GetNoise(noisex, noisey) * generator.settings.oceanregionalbias,
                    broad = 0.82f * mega + 0.24f * macro - regionalbias,
                    detailstrength = generator.settings.coastdetailstrength,
                    detailband = max(detailstrength * 3.5f, 0.10f),
                    detailweight = 1.0f - smoothstep(detailstrength * 0.35f, detailband, fabs(broad - threshold));

        if(detailstrength <= 0.0f || detailweight <= 0.0f) return broad;
        return broad + generator.coastdetail.GetNoise(noisex, noisey) * detailstrength * detailweight;
    }

    static float sampleterrainmicrovariation(const worldgenerator &generator, float noisex, float noisey)
    {
        const float detail = generator.terrainmicro.GetNoise(noisex, noisey),
                    masknoise = clamp(generator.terrainmicromask.GetNoise(noisex, noisey) * 0.5f + 0.5f,
                                      0.0f, 1.0f),
                    mask = 0.45f + 0.55f * smoothstep(0.25f, 0.75f, masknoise);

        return detail * mask;
    }

    static void samplecoastprofile(const worldgenerator &generator, float noisex, float noisey, float &beachspan, float &plainrun, float &plainlevel)
    {
        const float beachshape = clamp(generator.beachnoise.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    grassshape = clamp(generator.coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    configuredspan = max(float(generator.settings.coastwidth), 1.0f);

        // Each of the two sand terraces spans roughly 40-120% of the configured
        // coast width. This produces broad natural beaches without changing
        // their fixed vertical sequence at sea level and sea level +1.
        beachspan = configuredspan * (0.40f + 0.80f * powf(beachshape, 2.2f));
        plainrun = 14.0f + 16.0f * (1.0f - powf(beachshape, 1.5f));
        plainlevel = 2.0f + grassshape;
    }

    static float samplecliffstrength(const worldgenerator &generator, float noisex, float noisey)
    {
        const float selector = clamp(generator.cliffnoise.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    chance = clamp(generator.settings.cliffchance * 0.01f, 0.0f, 1.0f),
                    center = 1.0f - chance;

        if(chance <= 0.0f) return 0.0f;
        if(chance >= 1.0f) return 1.0f;

        // Cliffs occur in coherent coastal sections, not as per-column accidents.
        // An eight-percent feather on each side keeps their boundaries gradual.
        return smoothstep(center - 0.08f, center + 0.08f, selector);
    }

    static float samplecoastdistance(const worldgenerator &generator, float noisex, float noisey, float continental)
    {
        const float gradientstep = 24.0f,
                    gradientx = (samplecontinental(generator, noisex + gradientstep, noisey)
                               - samplecontinental(generator, noisex - gradientstep, noisey))
                              / (2.0f * gradientstep),
                    gradienty = (samplecontinental(generator, noisex, noisey + gradientstep)
                               - samplecontinental(generator, noisex, noisey - gradientstep))
                              / (2.0f * gradientstep),
                    minimumgradient = generator.settings.macrocontinentfrequency * 0.35f,
                    maximumgradient = generator.settings.macrocontinentfrequency * 0.85f,
                    gradient = clamp(sqrtf(gradientx * gradientx + gradienty * gradienty), minimumgradient, maximumgradient);

        // A broad derivative follows the coast's overall normal without letting
        // individual coast-detail octaves reset an already-inland point back
        // into either sea-level sand terrace.
        return max((continental - landthreshold(generator.settings)) / max(gradient, 0.000001f), 0.0f);
    }

    static worldtectonicsample sampletectonics(const worldgenerator &generator, int x, int y, float continental, float cavedepth)
    {
        const worldsettings &settings = generator.settings;
        const float threshold = landthreshold(settings),
                    landdensity = continental - threshold,
                    protection = max(0.02f, settings.coastprotectionwidth * settings.macrocontinentfrequency * 0.75f);

        float tectonicx = x + 10000.5f, tectonicy = y - 10000.5f;
        generator.tectonicwarp.DomainWarp(tectonicx, tectonicy);

        const float ridge = powf(clamp(1.0f - fabs(generator.tectonicnoise.GetNoise(tectonicx, tectonicy)), 0.0f, 1.0f),
                                 max(settings.tectonicridgepower * 0.65f, 0.1f)),
                    noisex = x + 10000.5f, noisey = y - 10000.5f,
                    broadchain = clamp(generator.mountainrange.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    chainstrength = clamp(0.78f * ridge + 0.22f * broadchain, 0.0f, 1.0f);

        worldtectonicsample sample;
        sample.activity = smoothstep(settings.tectonicactivitythreshold, min(settings.tectonicactivitythreshold + 0.35f, 1.0f),
                                     max(ridge, chainstrength));

        const float oceandistance = clamp(-landdensity / max(threshold + 1.0f, 0.001f), 0.0f, 1.0f),
                    oceanshelf = smoothstep(0.0f, 0.25f, oceandistance),
                    deepocean = smoothstep(0.15f, 0.85f, oceandistance),
                    normaloceandepth = settings.maxoceandepth * (0.25f * oceanshelf + 0.75f * deepocean),

                    // Give tall relief enough inland distance to fade before the
                    // protected coast instead of clipping a mountain into a wall.
                    landmask = smoothstep(protection, protection + 0.40f, landdensity),
                    oceandensitymask = smoothstep(protection, protection + 0.16f, -landdensity),
                    deepoceanmask = oceandensitymask * smoothstep(40.0f, 100.0f, normaloceandepth),
                    hill = clamp(generator.hills.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    reliefcoverage = max(settings.plainscoverage + settings.hillscoverage + settings.mountainscoverage + settings.highsummitscoverage,
                                         0.001f),
                    mountainshare = (settings.mountainscoverage + settings.highsummitscoverage) / reliefcoverage,
                    configuredthreshold = clamp(settings.mountainthreshold + (0.23f - mountainshare) * 0.45f, 0.08f, 0.92f),
                    envelopewidth = max(settings.mountainwidth, 0.01f),
                    // Expand the foot of the existing chains without moving their crests or changing the noise frequencies.
                    reliefscale = max(settings.mountainmaxamplitude / 160.0f, 1.0f),
                    footwidth = envelopewidth * sqrtf(reliefscale),
                    hillregion = smoothstep(configuredthreshold - footwidth * 2.4f, configuredthreshold - envelopewidth * 0.25f, chainstrength),
                    mountainregion = smoothstep(configuredthreshold - footwidth, configuredthreshold + envelopewidth, chainstrength),
                    summitregion = smoothstep(configuredthreshold + envelopewidth * 0.55f,
                                              configuredthreshold + envelopewidth * 1.75f, chainstrength),
                    // Broader shoulders compensate for the extra height instead of stretching narrow peaks vertically.
                    primaryridge = powf(clamp(1.0f - fabs(generator.mountainnoise.GetNoise(noisex, noisey)), 0.0f, 1.0f), 1.55f / reliefscale),
                    secondaryridge = powf(clamp(1.0f - fabs(generator.mountainpeaks.GetNoise(noisex, noisey)), 0.0f, 1.0f), 1.85f / reliefscale),
                    plainhillshape = smoothstep(0.48f, 0.78f, hill),
                    backgroundrelief = 0.025f * plainhillshape * (1.0f - 0.75f * hillregion),
                    foothills = 0.14f * hillregion * (0.45f + 0.55f * hill),
                    mainridges = 0.52f * powf(mountainregion, 1.55f) * (0.24f + 0.76f * primaryridge),
                    surroundingpeaks = 0.24f * powf(mountainregion, 2.0f) * secondaryridge * (0.30f + 0.70f * primaryridge),
                    localsummits = 0.19f * powf(summitregion, 2.4f) * powf(primaryridge * secondaryridge, 1.15f),
                    mountainrelief = foothills + mainridges + surroundingpeaks + localsummits,
                    amplitudeconversion = settings.maxlanduplift > 0.0f ? settings.mountainmaxamplitude / settings.maxlanduplift : 0.0f,
                    trenchpotential = sample.activity * deepoceanmask;

        // Overlapping ridges can exceed the nominal uplift. Preserve their contours here;
        // the final surface approaches the world ceiling smoothly after all relief is added.
        sample.landuplift = max(landmask * (backgroundrelief + amplitudeconversion * mountainrelief), 0.0f);
        sample.terrainroughness = clamp(landmask * (0.22f * hillregion + 0.52f * mountainregion + 0.26f * summitregion)
                                            * (0.72f + 0.28f * max(primaryridge, secondaryridge)),
                                        0.0f, 1.0f);

        const float structuralzone = landmask * hillregion * (0.25f + 0.75f * mountainregion);
        if(structuralzone > 0.001f)
        {
            const float secondarya = smoothstep(0.76f, 0.96f,1.0f - fabs(generator.secondarysummita.GetNoise(noisex, noisey))),
                        secondaryb = smoothstep(0.76f, 0.96f,1.0f - fabs(generator.secondarysummitb.GetNoise(noisex, noisey))),
                        secondarysummit = landmask * mountainregion * (0.40f + 0.60f * primaryridge) * secondarya * secondaryb,
                        hollowvalue = -generator.hollowshape.GetNoise(noisex, noisey),
                        hollowcore = smoothstep(0.25f, 0.65f, hollowvalue),
                        hollowedge = smoothstep(0.20f, 0.27f, hollowvalue) * (1.0f - smoothstep(0.30f, 0.36f, hollowvalue)),
                        primaryflank = 4.0f * primaryridge * (1.0f - primaryridge),
                        secondaryflank = 4.0f * secondaryridge * (1.0f - secondaryridge),
                        steepregion = smoothstep(0.60f, 0.84f, sample.terrainroughness),
                        steepflank = smoothstep(0.60f, 0.88f, max(primaryflank, secondaryflank)),
                        ledgeselector = clamp(generator.terrainmicromask.GetNoise(noisex + 7300.0f, noisey - 7300.0f) * 0.5f + 0.5f, 0.0f, 1.0f),
                        ledgepresence = smoothstep(0.56f, 0.76f, ledgeselector),
                        ledgebump = 0.65f + 0.55f * clamp(generator.terrainmicro.GetNoise(noisex - 4100.0f, noisey + 4100.0f) * 0.5f + 0.5f,
                                                         0.0f, 1.0f);

            // Stretched fields share the tectonically warped frame. Fold ridges
            // run along local Y; the sparse zero contours sampled along local X
            // form transverse cluses that notch through those anticlines.
            const float foldx = tectonicx * generator.foldcos - tectonicy * generator.foldsin,
                        foldy = tectonicx * generator.foldsin + tectonicy * generator.foldcos,
                        foldridge = powf(clamp(1.0f - fabs(generator.foldnoise.GetNoise(foldx, foldy * 0.22f)), 0.0f, 1.0f), 3.0f),
                        foldshoulder = smoothstep(0.38f, 0.72f, foldridge),
                        foldcrest = smoothstep(0.75f, 0.92f, foldridge),
                        crossridge = powf(clamp(1.0f - fabs(generator.clusenoise.GetNoise(foldx * 0.18f, foldy)), 0.0f, 1.0f), 5.0f),
                        crosscut = smoothstep(0.72f, 0.93f, crossridge),
                        cluse = structuralzone * crosscut * (0.35f + 0.65f * foldshoulder),
                        ledge = structuralzone * max(hollowedge, 0.70f * foldcrest) * steepregion * steepflank * ledgepresence
                              * (1.0f - 0.85f * crosscut);

            sample.terrainstructure = settings.secondarysummitheight * secondarysummit
                                    + settings.rockyledgeheight * ledge * ledgebump
                                    - settings.rockyledgeheight * 0.35f * structuralzone * hollowcore
                                    - settings.clusedepth * cluse;
            sample.rockyledge = clamp(ledge, 0.0f, 1.0f);
        }
        sample.oceantrench = clamp(trenchpotential * powf(sample.activity, 0.35f), 0.0f, 1.0f);

        const float protecteddepth = max(float(settings.cavemindepth), 12.0f),
                    fulldepth = max(float(settings.cavefulldepth), 20.0f),
                    depthmask = smoothstep(protecteddepth, max(fulldepth, protecteddepth + 1.0f), cavedepth),
                    foundationprotection = 1.0f - min(sample.landuplift, 1.0f) * 0.70f;

        sample.caveexpansion = clamp(sample.activity * depthmask * foundationprotection * settings.tectoniccavestrength, 0.0f, 1.0f);

        return sample;
    }

    worldtectonicsample worldgenerator::tectonics(int x, int y, float cavedepth) const
    {
        const float continental = samplecontinental(*this, x + 10000.5f, y - 10000.5f);
        return sampletectonics(*this, x, y, continental, cavedepth);
    }

    float worldgenerator::beachtransitionwidth(int x, int y) const
    {
        const float noisex = x + 10000.5f, noisey = y - 10000.5f,
                    cliffstrength = samplecliffstrength(*this, noisex, noisey);
        float beachspan, plainrun, plainlevel;
        samplecoastprofile(*this, noisex, noisey, beachspan, plainrun, plainlevel);
        return 2.0f * beachspan * powf(1.0f - cliffstrength, 4.0f);
    }

    float worldgenerator::maxbeachtransitionwidth() const
    {
        return 2.40f * max(float(settings.coastwidth), 1.0f);
    }

    float worldgenerator::coasttransitionwidth(int x, int y) const
    {
        float beachspan, plainrun, plainlevel;
        samplecoastprofile(*this, x + 10000.5f, y - 10000.5f, beachspan, plainrun, plainlevel);

        return 2.0f * beachspan + plainrun + 14.0f;
    }

    float worldgenerator::maxcoasttransitionwidth() const
    {
        // Two maximum-width sand terraces, the longest low grass run, and the
        // final inland blend. Keep the coast-map halo large enough for all of it.
        return maxbeachtransitionwidth() + 44.0f;
    }

    bool worldgenerator::beach(int x, int y) const
    {
        if(settings.coastwidth <= 0) return false;
        const float width = beachtransitionwidth(x, y);
        const int maximumcost = int(floorf(width * 3.0f + 0.5f)),
                  searchradius = int(ceilf(maxbeachtransitionwidth())) + 1;
        for(int dy = -searchradius; dy <= searchradius; ++dy) for(int dx = -searchradius; dx <= searchradius; ++dx)
        {
            const int diagonal = min(abs(dx), abs(dy)), straight = max(abs(dx), abs(dy)) - diagonal,
                      cost = diagonal * 4 + straight * 3;
            if(cost > maximumcost) continue;
            const int samplex = x + dx, sampley = y + dy;
            const bool water = height(samplex, sampley) < settings.sealevel;
            if((height(samplex - 1, sampley) < settings.sealevel) != water ||
               (height(samplex + 1, sampley) < settings.sealevel) != water ||
               (height(samplex, sampley - 1) < settings.sealevel) != water ||
               (height(samplex, sampley + 1) < settings.sealevel) != water)
                return true;
        }
        return false;
    }

    bool worldgenerator::coast(int x, int y) const
    {
        if(settings.coastwidth <= 0) return false;
        const float noisex = x + 10000.5f, noisey = y - 10000.5f,
                    configuredwidth = max(settings.coastwidth + biomeblend.GetNoise(noisex, noisey) * settings.coastvariation, 0.0f),
                    width = max(configuredwidth, coasttransitionwidth(x, y));
        const int maximumcost = int(floorf(width * 3.0f + 0.5f)),
                  searchradius = max(settings.coastwidth + settings.coastvariation, int(ceilf(maxcoasttransitionwidth()))) + 1;
        for(int dy = -searchradius; dy <= searchradius; ++dy) for(int dx = -searchradius; dx <= searchradius; ++dx)
        {
            const int diagonal = min(abs(dx), abs(dy)), straight = max(abs(dx), abs(dy)) - diagonal,
                      cost = diagonal * 4 + straight * 3;
            if(cost > maximumcost) continue;
            const int samplex = x + dx, sampley = y + dy;
            const bool water = height(samplex, sampley) < settings.sealevel;
            if((height(samplex - 1, sampley) < settings.sealevel) != water ||
               (height(samplex + 1, sampley) < settings.sealevel) != water ||
               (height(samplex, sampley - 1) < settings.sealevel) != water ||
               (height(samplex, sampley + 1) < settings.sealevel) != water)
                return true;
        }
        return false;
    }

    float worldgenerator::fracturecorridor(int x, int y) const
    {
        return fabs(fracturecorridors.GetNoise(x + 24500.5f, y - 24500.5f));
    }

    int worldgenerator::baseheight(int x, int y, worldtectonicsample *tectonics) const
    {
        const float noisex = x + 10000.5f, noisey = y - 10000.5f;
        const float continental = samplecontinental(*this, noisex, noisey);
        const float threshold = landthreshold(settings);
        const worldtectonicsample tectonicsample = sampletectonics(*this, x, y, continental, 0);
        if(tectonics) *tectonics = tectonicsample;
        float elevation, plaindetailmask = 1.0f, plainrollmask = 1.0f, cliffdetailmask = 0.0f;
        if(continental >= threshold)
        {
            float minimumelevation = 2.0f;
            const float distance = clamp((continental - threshold) / max(1.0f - threshold, 0.001f), 0.0f, 1.0f);
            const float coastrise = smoothstep(0.0f, 0.28f, distance);
            const float inland = smoothstep(0.0f, 0.72f, distance);
            const float hill = clamp(hills.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f);
            elevation = settings.maxcontinentheight * coastrise * (0.55f + 0.30f * inland + 0.15f * hill);
            const float continentalelevation = max(elevation, minimumelevation);

            // Build a deliberate beach cross-section near the continental edge.
            // A local gradient converts continental density into approximate metres
            // inland, keeping the profile deterministic and continuous across chunks.
            const float coastprofilelimit = max(16.0f, min(settings.cliffmaxheight, settings.maxcontinentheight));
            if(elevation < coastprofilelimit)
            {
                const float shoredistance = samplecoastdistance(*this, noisex, noisey, continental);

                float beachspan, plainrun, plainlevel;
                samplecoastprofile(*this, noisex, noisey, beachspan, plainrun, plainlevel);

                const float cliffstrength = samplecliffstrength(*this, noisex, noisey),
                            // Cliff sections progressively consume the beach. At full
                            // strength the first land column can already be exposed rock.
                            effectivebeachspan = beachspan * powf(1.0f - cliffstrength, 4.0f),
                            beachend = 2.0f * effectivebeachspan,
                            sandstepratio = 0.5f + clamp(coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f) / 6.0f,
                            sandstepstart = beachend * sandstepratio,
                            grassriseend = beachend + min(8.0f, plainrun * 0.5f),
                            plainend = beachend + plainrun,
                            blendend = plainend + 14.0f;

                float normalelevation;
                if(effectivebeachspan > 0.01f && shoredistance < beachend) minimumelevation = 0.0f;
                if(effectivebeachspan > 0.01f && shoredistance < sandstepstart) normalelevation = 0.0f;
                else if(effectivebeachspan > 0.01f && shoredistance < beachend) normalelevation = 1.0f;
                // The first grass column is always level 2 on ordinary coasts.
                // Its slow rise prevents a rounded level-3 plain from skipping
                // an entire vertical cube immediately after the sand.
                else if(shoredistance < grassriseend) normalelevation = 2.0f + (plainlevel - 2.0f) * smoothstep(beachend, grassriseend, shoredistance);
                else if(shoredistance < plainend) normalelevation = plainlevel;
                else normalelevation = plainlevel + (continentalelevation - plainlevel) * smoothstep(plainend, blendend, shoredistance);

                // High original relief is allowed to return sooner, while ordinary
                // shores retain the deliberately broad 2–3 metre grass plain.
                const float reliefpermission = smoothstep(9.0f, 16.0f, continentalelevation);
                normalelevation += (max(continentalelevation, normalelevation) - normalelevation) * reliefpermission;

                const float cliffshape = clamp(coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                            // Keep the current 7–16 metre distribution at the
                            // default while making the configured value a hard cap.
                            cliffheight = settings.cliffmaxheight * (0.4375f + 0.5625f * cliffshape),
                            cliffrise = smoothstep(-0.75f, 2.0f, shoredistance),
                            // Carry the cliff top inland before easing into the continental surface.
                            // Taller cliffs need broader shoulders and longer slopes, rather than a narrow coastal ridge.
                            cliffplateauend = max(plainend, max(64.0f, cliffheight * 4.0f)),
                            cliffblendend = cliffplateauend + max(128.0f, cliffheight * 10.0f),
                            inlandtarget = max(continentalelevation, normalelevation),
                            cliffblend = smoothstep(cliffplateauend, cliffblendend, shoredistance),
                            cliffplateau = cliffheight + (inlandtarget - cliffheight) * cliffblend,
                            cliffelevation = max(inlandtarget, cliffplateau * cliffrise);

                elevation = normalelevation + (cliffelevation - normalelevation) * cliffstrength;
                plaindetailmask = smoothstep(plainend, blendend, shoredistance);
                plainrollmask = smoothstep(grassriseend, blendend, shoredistance);
                cliffdetailmask = cliffstrength * (1.0f - smoothstep(cliffplateauend, cliffblendend, shoredistance));
            }
            elevation = clamp(elevation, 0.0f, settings.maxcontinentheight)
                      + settings.maxlanduplift * tectonicsample.landuplift
                      + tectonicsample.terrainstructure;
            elevation = max(elevation, 0.0f);

            const float roughness = max(tectonicsample.terrainroughness, cliffdetailmask),
                        detailstrength = settings.plainsmicrovariation * plaindetailmask + settings.reliefmicrovariation * roughness;
            const float rollstrength = 6.0f * settings.plainsmicrovariation * plainrollmask * (1.0f - smoothstep(0.1f, 0.65f, roughness));
            if(rollstrength > 0.0f)
            {
                // Lift broad, smooth rolls above the land floor instead of clipping signed noise into flat lowlands.
                // Fade them beyond the beach terraces and out again where mountain or cliff relief takes over.
                const float roll = clamp(plainsroll.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f);
                elevation = max(elevation, minimumelevation) + roll * rollstrength;
            }
            if(detailstrength > 0.0f) elevation = max(elevation + sampleterrainmicrovariation(*this, noisex, noisey) * detailstrength, 0.0f);
            // Evaluate climate at the continental datum, never through hydrology (which calls baseheight).
            // A stable datum avoids a temperature/height feedback loop at the cold boundary.
            const vec coldpos(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                              worldclimate::GROUND_UNITS + (settings.sealevel + continentalelevation) * worldclimate::BLOCK_UNITS);
            const float temperature = environmentclimate.gettemperature(coldpos),
                        //humidity = environmentclimate.gethumidity(coldpos),
                        cold = (1.0f - smoothstep(-4.0f, 2.0f, temperature)) * smoothstep(2.0f, 18.0f, continentalelevation),
                        polar = 1.0f - smoothstep(-11.0f, -9.0f, temperature),
                        broad = coldbroad.GetNoise(float(x), float(y)), roll = coldroll.GetNoise(float(x), float(y)),
                        micro = coldmicro.GetNoise(float(x), float(y)), region = coldregions.GetNoise(float(x), float(y)),
                        ridge = powf(1.0f - fabsf(coldroll.GetNoise(x * 1.5f + y * 0.3f, y * 0.45f)), 3.0f),
                        tundra = 10.0f * broad + 18.0f * roll + 2.5f * micro,
                        barren = 12.0f * broad + 8.0f * ridge + 1.5f * micro,
                        channel = smoothstep(0.1f, 0.4f, region) * (1.0f - smoothstep(0.02f, 0.10f, fabsf(micro))),
                        boulders = smoothstep(0.20f, 0.40f, region) * smoothstep(0.40f, 0.72f,
                            coldmicro.GetNoise(float(x) * 8.0f, float(y) * 8.0f)) * (4.0f + polar * 3.0f),
                        target = boulders + continentalelevation * 0.65f + 8.0f + tundra * (1.0f - polar) + barren * polar - 3.0f * channel;
            elevation += cold * (max(target, minimumelevation) - elevation);
            // Only the beach terraces may fall below sea level +2, including after relief and microvariation.
            elevation = max(elevation, minimumelevation);
        }
        else
        {
            const float distance = clamp((threshold - continental) / max(threshold + 1.0f, 0.001f), 0.0f, 1.0f);
            const float shelf = smoothstep(0.0f, 0.25f, distance);
            const float deepocean = smoothstep(0.15f, 0.85f, distance);
            elevation = -settings.maxoceandepth * (0.25f * shelf + 0.75f * deepocean);
            elevation = clamp(elevation, -settings.maxoceandepth, 0.0f) - settings.maxoceansubsidence * tectonicsample.oceantrench;

            // Continental fields vary slowly enough that a fractional ocean
            // shelf can otherwise round back to a dry sea-level column. Keep
            // every ocean-side sample submerged by at least one whole block so
            // the coast mask reaches the level-0/+1 sand terraces and grass
            // starts cleanly at level +2.
            elevation = min(elevation, -1.0f);
        }
        // Leave lower slopes intact, then approach 255 monotonically instead of slicing off summits.
        // The shoulder has matching first and second derivatives at 200, so it introduces no ledge.
        // Work in absolute height, including sea level, and apply this after every terrain contribution.
        float surfaceheight = settings.sealevel + elevation;
        if(surfaceheight > 200.0f)
        {
            const float excess = surfaceheight - 200.0f, headroom = 55.0f;
            surfaceheight = 200.0f + headroom * excess / sqrtf(headroom * headroom + excess * excess);
        }
        return clamp(int(floor(surfaceheight + 0.5f)), -255, 255);
    }

    #include "world/hydrology.h"

    worldgenerator::~worldgenerator()
    {
        delete hydrology;
    }

    worldwatersample worldgenerator::surface(int x, int y) const
    {
        if(!hydrology) hydrology = new worldhydrology(*this);
        return hydrology->sample(x, y);
    }

    float worldgenerator::gethumidity(const vec &worldpos) const
    {
        const float x = worldpos.x / worldclimate::BLOCK_UNITS, y = worldpos.y / worldclimate::BLOCK_UNITS,
                    z = (worldpos.z - worldclimate::GROUND_UNITS) / worldclimate::BLOCK_UNITS,
                    noisex = x + 10000.5f, noisey = y - 10000.5f,
                    continental = samplecontinental(*this, noisex, noisey),
                    coastdistance = samplecoastdistance(*this, noisex, noisey, continental),
                    coast = 1.0f - smoothstep(0.0f, 160.0f, coastdistance);
        if(!hydrology) hydrology = new worldhydrology(*this);
        return environmentclimate.gethumidity(worldpos, coast, hydrology->moisture(x, y, z));
    }

    int worldgenerator::height(int x, int y, worldtectonicsample *tectonics) const
    {
        if(tectonics) *tectonics = this->tectonics(x, y);
        return surface(x, y).height;
    }

    // Ranges normalize Celsius and percent independently; density scales existing climate suitability.
    const ClimateBiome climateBiomes[] =
    {   //type, name, identifier, temp, humidity, temp range, humidity range, tree density
        { WORLD_BIOME_SNOW_DESERT, "Snow Desert", "snow_desert", -20, 20, 10, 25, 0.0f },
        { WORLD_BIOME_TUNDRA, "Tundra", "tundra", -10, 40, 12, 25, 0.02f },
        { WORLD_BIOME_TAIGA, "Taiga", "taiga", 3, 60, 12, 25, 0.75f },
        { WORLD_BIOME_COLD_DESERT, "Cold Desert", "cold_desert", 5, 15, 12, 25, 0.02f },
        { WORLD_BIOME_PLAINS, "Grassland", "grassland", 14, 40, 12, 25, 0.10f },
        { WORLD_BIOME_FOREST, "Temperate Forest", "forest", 15, 70, 12, 25, 1.00f },
        { WORLD_BIOME_DESERT, "Desert", "desert", 35, 20, 12, 30, 0.0f },
        { WORLD_BIOME_SAVANNA, "Savanna", "savanna", 27, 40, 12, 25, 0.06f },
        { WORLD_BIOME_RAINFOREST, "Tropical Rainforest", "rainforest", 27, 85, 12, 25, 1.0f }
    };
    const int climateBiomeCount = sizeof(climateBiomes) / sizeof(climateBiomes[0]);

    const char *biomeName(int biome)
    {
        loopi(climateBiomeCount) if(climateBiomes[i].type == biome) return climateBiomes[i].name;
        return biome == WORLD_BIOME_OCEAN ? "Ocean" : "Unknown";
    }

    BiomeSample sampleClimateBiome(float temperature, float humidity)
    {
        BiomeSample sample = {};
        sample.temperature = temperature;
        sample.humidity = humidity;

        // evaluate the normal climate biomes
        float nearest = FLT_MAX;
        loopi(climateBiomeCount)
        {
            const ClimateBiome &biome = climateBiomes[i];
            const float dt = (temperature - biome.temperatureCenter) / biome.temperatureRange,
                        dh = (humidity - biome.humidityCenter) / biome.humidityRange,
                        distance = dt * dt + dh * dh;

            sample.weights[biome.type] = distance;
            nearest = min(nearest, distance);
        }

        loopi(climateBiomeCount)
        {
            float &weight = sample.weights[climateBiomes[i].type];

            // subtracting nearest keeps the exponent numerically stable
            weight = expf(-2.0f * (weight - nearest));
        }

        // Cold climates are explicitly temperature-driven
        // ~5°C       : temperate <-> taiga
        // ~0°C       : taiga <-> tundra
        // ~-10°C     : tundra <-> snow desert
        const float coldmix = 1.0f - smoothstep(4.0f, 6.0f, temperature),
                    taiga = smoothstep(-1.0f, 1.0f, temperature),
                    snowdesert = 1.0f - smoothstep(-11.0f, -9.0f, temperature),
                    tundra = clamp(1.0f - taiga - snowdesert, 0.0f, 1.0f);

        // Taiga -> Tundra -> Snow Desert
        loopi(climateBiomeCount)
        {
            const int type = climateBiomes[i].type;

            if(type == WORLD_BIOME_TAIGA ||
               type == WORLD_BIOME_TUNDRA ||
               type == WORLD_BIOME_SNOW_DESERT ||
               type == WORLD_BIOME_COLD_DESERT)
            {
                sample.weights[type] = 0.0f;
            }
        }

        // Renormalize the remaining warm biomes into the non-cold share
        float warmtotal = 0.0f;
        loopi(climateBiomeCount)
            warmtotal += sample.weights[climateBiomes[i].type];

        if(warmtotal > 1e-20f)
        {
            const float warmmix = 1.0f - coldmix;

            loopi(climateBiomeCount)
                sample.weights[climateBiomes[i].type] *= warmmix / warmtotal;
        }

        sample.weights[WORLD_BIOME_TAIGA] = coldmix * taiga;
        sample.weights[WORLD_BIOME_TUNDRA] = coldmix * tundra;
        sample.weights[WORLD_BIOME_SNOW_DESERT] = coldmix * snowdesert;
        sample.weights[WORLD_BIOME_COLD_DESERT] = 0.0f;

        // final normalization
        float total = 0.0f;
        loopi(climateBiomeCount)
            total += sample.weights[climateBiomes[i].type];

        if(total > 1e-20f)
        {
            loopi(climateBiomeCount)
                sample.weights[climateBiomes[i].type] /= total;
        }

        // determine primary and secondary biomes
        loopi(climateBiomeCount)
        {
            const worldbiome type = climateBiomes[i].type;
            const float weight = sample.weights[type];

            if(weight > sample.primaryWeight)
            {
                sample.secondary = sample.primary;
                sample.secondaryWeight = sample.primaryWeight;
                sample.primary = type;
                sample.primaryWeight = weight;
            }
            else if(weight > sample.secondaryWeight || sample.secondary == WORLD_BIOME_OCEAN)
            {
                sample.secondary = type;
                sample.secondaryWeight = weight;
            }
        }

        return sample;
    }

    BiomeSample worldgenerator::sampleBiome(const vec &worldpos) const
    {
        return sampleClimateBiome(environmentclimate.gettemperature(worldpos), gethumidity(worldpos));
    }

    int worldgenerator::biome(int x, int y, int height) const
    {
        if(height < settings.sealevel) return WORLD_BIOME_OCEAN;
        // Rivers, lakes, beaches and rock faces retain their independent generation masks.
        return sampleBiome(vec(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                               worldclimate::GROUND_UNITS + float(height) * worldclimate::BLOCK_UNITS)).primary;
    }

    ColdSample worldgenerator::samplecold(int x, int y, int h, const BiomeSample &climate) const
    {
        ColdSample c = {};
        c.coldness = clamp((2.0f - climate.temperature) / 22.0f, 0.0f, 1.0f);
        const float humidity = clamp(climate.humidity * 0.01f, 0.0f, 1.0f);
        c.desert = (1.0f - smoothstep(-16.0f, -6.0f, climate.temperature)) *
                   (1.0f - smoothstep(30.0f, 55.0f, climate.humidity));
        // Four-metre support reduces voxel quantization bias; sample the final carved surface across chunk boundaries.
        const float left = height(x - 4, y), right = height(x + 4, y), down = height(x, y - 4), up = height(x, y + 4),
                    dx = (right - left) / 8.0f, dy = (up - down) / 8.0f, gradient = sqrtf(dx * dx + dy * dy);
        c.slope = clamp(gradient / 1.25f, 0.0f, 1.0f);
        c.basin = clamp(0.5f + (left + right + down + up - 4.0f * h) / 12.0f, 0.0f, 1.0f);
        c.exposure = clamp(0.5f + (dx * 0.9701425f + dy * 0.2425356f) * 0.5f, 0.0f, 1.0f);
        c.deposition = clamp(c.basin * 0.6f + (1.0f - c.exposure) * 0.4f, 0.0f, 1.0f);
        c.region = coldregions.GetNoise(float(x), float(y)) * 0.5f + 0.5f;
        const float large = coldroll.GetNoise(float(x) * 0.83f, float(y) * 0.83f),
                    small = snowpatches.GetNoise(float(x) * 0.23f, float(y) * 0.23f),
                    altitude = clamp(float(h - settings.sealevel) / 80.0f, 0.0f, 1.0f);
        c.snow = clamp(c.coldness * 0.45f + humidity * 0.08f + altitude * 0.08f + c.deposition * 0.22f +
                       large * 0.25f + small * 0.12f + c.desert * 0.25f - c.slope * 0.35f - c.exposure * 0.08f, 0.0f, 1.0f);
        // Persistent regional wind scour opens bare shelves even on otherwise flat polar ground.
        c.snow = max(0.0f, c.snow - c.desert * smoothstep(0.55f, 0.72f, c.region) * (0.55f + c.exposure * 0.20f));
        c.snow *= 1.0f - smoothstep(0.0f, 5.0f, climate.temperature);
        // One coherent soil field establishes regional identity. Fine fields only perturb its borders.
        const float region = coldroll.GetNoise(x * 0.833333f + 3171.0f, y * 0.833333f - 951.0f),
                    detail = coldmicro.GetNoise(x * 1.166667f, y * 1.166667f),
                    micro = snowpatches.GetNoise(x * 0.714286f, y * 0.714286f),
                    patch = clamp(0.5f + region * 0.85f + detail * 0.10f + micro * 0.025f, 0.0f, 1.0f),
                    dry = 1.0f - humidity;
        c.severity = 1.0f - smoothstep(-15.0f, 2.0f, climate.temperature);
        c.nearwater = surface(x, y).bank ? 1.0f : 0.0f;
        c.wetness = clamp(humidity * 0.65f + c.basin * 0.25f + c.nearwater * 0.10f, 0.0f, 1.0f);
        c.grassscore = 0.54f;
        c.dirtscore = dry * 0.28f + c.exposure * 0.14f + patch * 0.52f + c.slope * 0.12f;
        c.mossscore = c.wetness * 0.42f + (1.0f - patch) * 0.48f - c.slope * 0.18f;
        c.gravelscore = c.slope * 0.50f + c.exposure * 0.20f + dry * 0.15f + patch * 0.15f;
        const bool exposed = c.slope > 0.45f || c.exposure > 0.75f || c.nearwater > 0.5f ||
                             (dry > 0.72f && patch > 0.86f);
        // The pre-existing retention mask contributes to snow; a separate broad field forms deposition regions.
        const float snowregion = clamp(0.5f + coldroll.GetNoise(x * 0.833333f - 1921.0f, y * 0.833333f + 7813.0f) * 0.85f +
                                       detail * 0.08f, 0.0f, 1.0f);
        c.snowscore = clamp(c.severity * 0.24f + c.basin * 0.16f + altitude * 0.08f + snowregion * 0.46f +
                            c.snow * 0.16f - c.slope * 0.25f - c.exposure * 0.10f, 0.0f, 1.0f);
        c.material = WORLD_SNOWY_GRASS;
        if(c.dirtscore > c.grassscore && patch > 0.55f) c.material = WORLD_FROZEN_DIRT;
        if(c.mossscore > c.grassscore && c.wetness > 0.42f && c.slope < 0.40f)
        {
            const float thaw = -2.0f + detail * 1.5f + (patch - 0.5f) * 3.0f;
            c.material = climate.temperature > thaw ? WORLD_MOSS : WORLD_FROZEN_MOSS;
        }
        if(exposed && c.gravelscore > 0.58f) c.material = WORLD_FROZEN_GRAVEL;
        c.covered = c.snowscore > 0.55f - c.desert * 0.18f;
        if(c.covered) c.material = WORLD_SNOW_CRUST;
        if(c.slope > 0.72f) c.material = WORLD_COLD_ROCK;
        const float pineRegion = clamp(0.5f + coldroll.GetNoise(float(x) - 5193.0f, float(y) + 2197.0f), 0.0f, 1.0f),
                    pineLocal = clamp(0.5f + coldmicro.GetNoise(x * 3.0f, y * 3.0f), 0.0f, 1.0f);
        c.pinemask = smoothstep(0.65f, 0.85f, pineRegion) * smoothstep(0.58f, 0.78f, pineLocal);
        c.vegetation = clamp(humidity * 0.45f + (1.0f - c.slope) * 0.25f + (1.0f - c.snow) * 0.25f +
                             c.basin * 0.05f + coldmicro.GetNoise(float(x) * 2.5f, float(y) * 2.5f) * 0.15f, 0.0f, 1.0f);
        c.vegetation *= (1.0f - c.desert) * (1.0f - c.coldness * 0.5f);
        if(c.covered || c.slope > 0.72f) c.vegetation = 0;
        return c;
    }

    bool worldgenerator::snowcovered(int x, int y, float temperature) const
    {
        const int h = height(x, y);
        const vec position(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                           worldclimate::GROUND_UNITS + h * worldclimate::BLOCK_UNITS);
        BiomeSample climate = sampleBiome(position);
        climate.temperature = temperature;
        return samplecold(x, y, h, climate).covered;
    }

    static float desertshare(const BiomeSample &soil)
    {
        const float sand = soil.weights[WORLD_BIOME_DESERT];
        float competitor = 0;
        loopi(climateBiomeCount) if(climateBiomes[i].type != WORLD_BIOME_DESERT)
            competitor = max(competitor, soil.weights[climateBiomes[i].type]);
        return sand / max(sand + competitor, 0.000001f);
    }

    static vec soilblendoffset(const FastNoiseLite &noise, float x, float y)
    {
        const float dx = noise.GetNoise(x, y), dy = noise.GetNoise(x + 713.0f, y - 419.0f),
                    scale = 8.0f * worldclimate::BLOCK_UNITS / max(1.0f, sqrtf(dx * dx + dy * dy));
        return vec(dx * scale, dy * scale, 0);
    }

    BiomeSample worldgenerator::samplesoil(const vec &position) const
    {
        // Simple noisy edge: at most eight blocks of horizontal displacement, including diagonals.
        // No terrace, slope, boundary projection or additional terrain influence.
        const vec lookup = vec(position).add(soilblendoffset(biomeedgewarp, position.x / worldclimate::BLOCK_UNITS,
                                                            position.y / worldclimate::BLOCK_UNITS));
        // Ocean air must not create a grass belt between a desert and its beach.
        // Keep freshwater and altitude humidity in soil selection; physical climate still includes the coast.
        const float x = lookup.x / worldclimate::BLOCK_UNITS, y = lookup.y / worldclimate::BLOCK_UNITS,
                    z = (lookup.z - worldclimate::GROUND_UNITS) / worldclimate::BLOCK_UNITS;
        if(!hydrology) hydrology = new worldhydrology(*this);
        const float humidity = environmentclimate.gethumidity(lookup, 0.0f, hydrology->moisture(x, y, z));
        return sampleClimateBiome(environmentclimate.getregionaltemperature(lookup), humidity);
    }

    float worldgenerator::sandcoverage(const BiomeSample &soil) const
    {
        return smoothstep(0.35f, 0.65f, desertshare(soil));
    }

    float worldgenerator::icecoastdistance(int x, int y) const
    {
        // Cache a coarse shore-distance field instead of searching thousands of neighbours for every ice block.
        const int grid = 16, gx = int(floorf(float(x) / grid)), gy = int(floorf(float(y) / grid));
        float distances[4];
        loopi(4)
        {
            const ivec key(gx + (i & 1), gy + (i >> 1), 0);
            float *cached = icecoastcache.access(key);
            if(cached) { distances[i] = *cached; continue; }
            const int sx = key.x * grid, sy = key.y * grid;
            const worldwatersample center = surface(sx, sy);
            float distance = 1024.0f;
            if(center.height >= settings.sealevel && center.height >= center.water) distance = 0.0f;
            else for(int radius = 8; radius <= 512 && distance > radius; radius += 8)
            {
                loopj(16)
                {
                    const float angle = j * (2.0f * M_PI / 16.0f);
                    const worldwatersample shore = surface(sx + int(roundf(cosf(angle) * radius)), sy + int(roundf(sinf(angle) * radius)));
                    if(shore.height >= settings.sealevel && shore.height >= shore.water) { distance = float(radius); break; }
                }
            }
            icecoastcache[key] = distances[i] = distance;
        }
        const float fx = float(x - gx * grid) / grid, fy = float(y - gy * grid) / grid;
        return (distances[0] * (1.0f - fx) + distances[1] * fx) * (1.0f - fy) +
               (distances[2] * (1.0f - fx) + distances[3] * fx) * fy;
    }

    static float icecoastwidth(const worldgenerator &generator, int x, int y, float temperature)
    {
        // Start with one or two shore blocks, then expand smoothly to the 192-block cap (three times the original extent).
        const float growth = 1.0f - smoothstep(-22.0f, -5.0f, temperature),
                    broad = generator.snowpatches.GetNoise(float(x) * 0.20f, float(y) * 0.20f),
                    variation = clamp(0.90f + 0.10f * broad, 0.80f, 1.0f);
        return temperature >= -5.0f ? 0.0f : 1.5f + 190.5f * growth * variation;
    }

    bool worldgenerator::coastice(int x, int y, float temperature, float margin) const
    {
        if(temperature >= -5.0f) return false;
        return icecoastdistance(x, y) <= icecoastwidth(*this, x, y, temperature) + margin;
    }

    struct seaicecell
    {
        int x, y;
        float edge, centerx, centery;
    };

    static seaicecell sampleseaicecell(uint seed, float x, float y, int size, uint salt)
    {
        const int gx = int(floorf(x / size)), gy = int(floorf(y / size));
        float first = 1e20f, second = 1e20f, secondx = 0, secondy = 0;
        seaicecell cell = { 0, 0, 0, 0, 0 };
        for(int cy = gy - 1; cy <= gy + 1; ++cy) for(int cx = gx - 1; cx <= gx + 1; ++cx)
        {
            const float px = (cx + 0.20f + 0.60f * treespatialunit(seed, cx, cy, salt)) * size,
                        py = (cy + 0.20f + 0.60f * treespatialunit(seed, cx, cy, salt ^ 0x8913U)) * size,
                        distance = (x - px) * (x - px) + (y - py) * (y - py);
            if(distance < first)
            {
                second = first;
                secondx = cell.centerx;
                secondy = cell.centery;
                first = distance;
                cell.x = cx;
                cell.y = cy;
                cell.centerx = px;
                cell.centery = py;
            }
            else if(distance < second)
            {
                second = distance;
                secondx = px;
                secondy = py;
            }
        }
        const float dx = cell.centerx - secondx, dy = cell.centery - secondy;
        cell.edge = (second - first) / max(2.0f * sqrtf(dx * dx + dy * dy), 0.001f);
        return cell;
    }

    bool worldgenerator::seaice(int x, int y, int height, float &bottom, float &top) const
    {
        if(height >= settings.sealevel) return false;
        const ivec key(x, y, height);
        if(seaicequery *cached = seaicecache.access(key))
        {
            bottom = cached->bottom;
            top = cached->top;
            return cached->covered;
        }
        seaicequery result;
        const vec position(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                           worldclimate::GROUND_UNITS + settings.sealevel * worldclimate::BLOCK_UNITS);
        const float temperature = environmentclimate.gettemperature(position),
                    cold = 1.0f - smoothstep(-22.0f, 3.0f, temperature);
        if(cold > 0.0f && !surface(x, y).freshwater)
        {
            const float shore = icecoastdistance(x, y), width = icecoastwidth(*this, x, y, temperature),
                        fringe = temperature < -5.0f ? 1.0f - smoothstep(width, width + 48.0f, shore) : 0.0f,
                        reach = 1.0f - smoothstep(width + 80.0f, width + 250.0f, shore),
                        wx = float(x) + 12.0f * coldmicro.GetNoise(float(x), float(y)),
                        wy = float(y) + 12.0f * coldmicro.GetNoise(float(x) + 713.0f, float(y) - 419.0f);
            const seaicecell parent = sampleseaicecell(uint(seed), wx, wy, 48, 0x5041434BU);
            const float division = treespatialunit(uint(seed), parent.x, parent.y, 0x5041434CU);
            const int size = division < 0.22f * cold ? 48 : division < 0.60f + 0.20f * cold ? 18 : 8;
            const seaicecell plate = size == 48 ? parent : sampleseaicecell(uint(seed), wx, wy, size, 0x5041434BU ^ uint(size));
            const float cluster = coldroll.GetNoise(plate.centerx * 1.5f + 1731.0f, plate.centery * 1.5f - 2917.0f),
                        chance = max(fringe, clamp(cold * (0.80f + 0.35f * cluster) * reach, 0.0f, 0.96f)),
                        erosion = 0.45f + 1.5f * (1.0f - cold) +
                                  0.35f * snowpatches.GetNoise(float(x) * 1.3f + 5713.0f, float(y) * 1.3f - 2137.0f),
                        selected = treespatialunit(uint(seed), plate.x, plate.y, 0x504C4154U ^ uint(size));
            bool cracked = min(parent.edge, plate.edge) <= erosion;
            // Some large sheets retain a short internal fracture; its broad gate leaves the plate connected elsewhere.
            if(size == 48 && division < 0.10f)
            {
                const seaicecell fracture = sampleseaicecell(uint(seed), wx, wy, 18, 0x43524143U);
                cracked = cracked || (fracture.edge < 0.45f &&
                    coldmicro.GetNoise(float(x) * 2.0f + 3171.0f, float(y) * 2.0f - 951.0f) > 0.10f);
            }
            if(temperature < -5.0f && shore <= width)
            {
                // Continuous shore-fast ice remains intact underneath the broken offshore field.
                result.bottom = max(float(height), float(settings.sealevel - 1));
                result.top = float(settings.sealevel);
                result.covered = true;
            }
            else if(!cracked && selected < chance)
            {
                // Full voxels share the same sea-level top as the continuous frozen coast.
                result.top = float(settings.sealevel);
                result.bottom = float(settings.sealevel - 1);
                result.covered = true;
            }
        }
        if(seaicecache.numelems >= 1 << 16) seaicecache.clear();
        seaicecache.access(key, result);
        bottom = result.bottom;
        top = result.top;
        return result.covered;
    }

    bool worldgenerator::iceformation(int x, int y, int height, int &bottom, int &top) const
    {
        bottom = top = height;
        const worldwatersample water = surface(x, y);
        const bool ocean = height < settings.sealevel && !water.freshwater;

        if(!ocean && height < water.water) return false;

        const int spacing = ocean ? 56 : 112,
                  cellx = int(floorf(float(x) / spacing)), celly = int(floorf(float(y) / spacing));
        const uint salt = ocean ? 0x71C8B249U : 0x3E9D5A17U;
        const float occurrence = treespatialunit(uint(seed), cellx, celly, salt);

        if(occurrence > (ocean ? 0.82f : 0.52f)) return false;

        // centres stay inside their cells, with enough margin for the whole footprint
        const int margin = ocean ? 14 : 20,
                  cx = cellx * spacing + margin + int(treespatialunit(uint(seed), cellx, celly, salt ^ 0x2917U) * (spacing - 2 * margin)),
                  cy = celly * spacing + margin + int(treespatialunit(uint(seed), cellx, celly, salt ^ 0x8913U) * (spacing - 2 * margin));

        const float shape = treespatialunit(uint(seed), cellx, celly, salt ^ 0xD71FU),
                    radius = ocean ? 8.0f + 5.0f * shape : 5.0f + 4.0f * shape,
                    angle = treespatialunit(uint(seed), cellx, celly, salt ^ 0xE3A9U) * 2.0f * M_PI,
                    dx = float(x - cx), dy = float(y - cy),
                    u = (dx * cosf(angle) + dy * sinf(angle)) / radius,
                    v = (-dx * sinf(angle) + dy * cosf(angle)) / (radius * 0.72f),
                    edge = max(fabsf(u), max(fabsf(v), fabsf(u + v) * 0.65f));

        if(edge >= 1.75f) return false;

        const worldwatersample center = surface(cx, cy);
        const int base = ocean ? settings.sealevel : center.height;
        const vec position(float(cx) * worldclimate::BLOCK_UNITS, float(cy) * worldclimate::BLOCK_UNITS, worldclimate::GROUND_UNITS + base * worldclimate::BLOCK_UNITS);
        const BiomeSample climate = sampleBiome(position);
        float bergscale = 1.0f;
        int coastmaxrise = 255;

        if(ocean)
        {
            if(center.freshwater || center.height >= settings.sealevel || climate.temperature >= 3.0f) return false;

            const float shore = icecoastdistance(cx, cy),
                        width = icecoastwidth(*this, cx, cy, climate.temperature),
                        offshore = shore - width,
                        outer = smoothstep(35.0f, 210.0f, offshore), // beyond the frozen shelf
                        coldcoast = 1.0f - smoothstep(-18.0f, -3.0f, climate.temperature),
                        beach = 1.0f - smoothstep(24.0f, 120.0f, shore), // strong boost in the first ~100 blocks of ocean from the beach
                        shoregate = smoothstep(2.0f, 10.0f, shore), // avoid centers literally touching the shoreline
                        nearshore = 0.42f + 0.38f * coldcoast + 0.18f * beach, // high density along frozen coast, progressively lower offshore

                        chance = clamp(
                            nearshore *
                            shoregate *
                            (1.0f - 0.90f * outer) *
                            (1.0f - smoothstep(210.0f, 250.0f, offshore)),
                            0.0f,
                            0.95f
                        );

            if(occurrence >= chance) return false;

            bergscale = 1.0f - 0.50f * outer; // offshore bergs survive beyond the shelf and do not collapse with its narrow onset width
            coastmaxrise = int(floorf(4.0f + 44.0f * smoothstep(24.0f, 220.0f, shore))); // keep coastal bergs low. Height progressively returns to normal farther offshore

            if(edge >= 1.75f * bergscale) return false;

            bergscale = 1.0f - 0.50f * outer; // offshore bergs survive beyond the shelf and do not collapse with its narrow onset width
            coastmaxrise = int(floorf(4.0f + 44.0f * smoothstep(24.0f, 220.0f, shore)));

            if(edge >= 1.75f * bergscale) return false;
        }
        else
        {
            if(center.height < center.water || climate.weights[WORLD_BIOME_SNOW_DESERT] < 0.5f) return false;
            if(abs(height - base) > 5) return false; // Ice spires belong on snowfields, not perched across steep slopes or cliffs
        }

        const vec localposition(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                                worldclimate::GROUND_UNITS + max(height, base) * worldclimate::BLOCK_UNITS);

        const float temperature = environmentclimate.gettemperature(localposition),
                    cold = 1.0f - smoothstep(ocean ? -16.0f : -18.0f, ocean ? 5.0f : -8.0f, temperature),
                    summit = ocean ? (18.0f + 25.0f * shape) * bergscale : 14.0f + 23.0f * shape,
                    ridge = 0.85f + 0.15f * snowpatches.GetNoise(float(x) * (ocean ? 0.8f : 9.0f), float(y) * (ocean ? 0.8f : 9.0f));

        if(ocean && temperature >= 3.5f) return false;

        float profile = powf(max(1.0f - edge / bergscale, 0.0f), 1.05f);

        loopi(3)
        {
            // Rotated companion spires share the main mass but keep distinct, lower summits.
            const float phase = i * 2.0943951f + shape * 1.7f,
                        offset = 0.80f + 0.25f * treespatialunit(uint(seed), cellx, celly, salt ^ uint(0x5413 + i)),
                        su = (u / bergscale - cosf(phase) * offset) / 0.55f,
                        sv = (v / bergscale - sinf(phase) * offset) / 0.55f,
                        subedge = max(fabsf(su), max(fabsf(sv), fabsf(su + sv) * 0.65f)),
                        subprofile = (0.38f + 0.18f * shape) * powf(max(1.0f - subedge, 0.0f), 1.10f);

            profile = max(profile, subprofile);
        }
        int rise = int(floorf(summit * profile * ridge * cold));
        if(ocean) rise = min(rise, coastmaxrise);
        if(rise < 1) return false;
        top = min(255, base + rise);
        // Most of a berg is submerged. Stop above the seabed, preserving water underneath
        bottom = ocean ? max(height + 1, base - max(2, rise * 3)) : height - 1;
        return top > bottom && top > height;
    }

    bool worldgenerator::icecolumn(int x, int y, int height, int &bottom, int &top) const
    {
        const worldwatersample water = surface(x, y);
        if(height < settings.sealevel && !water.freshwater)
        {
            // Sparse large spires rise through the dominant flat pack ice and extend below sea level.
            if(iceformation(x, y, height, bottom, top)) return true;
            float low, high;
            if(!seaice(x, y, height, low, high)) return false;
            bottom = int(low);
            top = int(high);
            return true;
        }
        return iceformation(x, y, height, bottom, top);
    }

    int worldgenerator::surfacematerial(int x, int y, int height, const BiomeSample *climate) const
    {
        const vec position(
            float(x) * worldclimate::BLOCK_UNITS,
            float(y) * worldclimate::BLOCK_UNITS,
            worldclimate::GROUND_UNITS + float(height) * worldclimate::BLOCK_UNITS
        );

        const BiomeSample sample = climate ? *climate : sampleBiome(position);
        const worldwatersample water = surface(x, y);

        // water / frozen water
        if(height < water.water)
        {
            const vec waterpos(
                position.x,
                position.y,
                worldclimate::GROUND_UNITS + water.water * worldclimate::BLOCK_UNITS
            );

            const float temperature = environmentclimate.gettemperature(waterpos);

            // Solid shore-fast ice ends at the shelf edge; offshore slabs keep their water cracks.
            if(water.freshwater ? temperature < -2.0f ||
               (temperature < 0.0f && snowpatches.GetNoise(float(x), float(y)) > 0.0f) : coastice(x, y, temperature))
            {
                return WORLD_FROZEN_WATER;
            }

            return WORLD_BIOME_OCEAN;
        }

        // Snow cover follows temperature in every biome, with coherent wind-shaped melt edges.
        if(sample.temperature < -4.0f) return WORLD_BIOME_SNOW;
        if(sample.temperature < 3.0f)
        {
            const float broad = snowpatches.GetNoise(float(x), float(y)),
                        detail = coldmicro.GetNoise(float(x) + 731.0f, float(y) + 1913.0f),
                        drift = broad * 0.85f + detail * 0.15f,
                        warmth = clamp((sample.temperature + 4.0f) / 6.0f, 0.0f, 1.0f);
            // Raising the threshold continuously makes warm snowdrifts progressively rarer.
            if(sample.temperature <= 2.0f && drift > -0.03f + 0.56f * warmth) return WORLD_BIOME_SNOW;
            // Warp a broad channel field: snow fingers and grass clearings share winding boundaries.
            // The shortest noise wavelength is about 74 blocks, never individual-voxel speckle.
            const float warpx = float(x) + 80.0f * coldroll.GetNoise(float(x) + 3171.0f, float(y) - 951.0f),
                        warpy = float(y) + 80.0f * coldroll.GetNoise(float(x) - 1921.0f, float(y) + 7813.0f),
                        channels = fabsf(coldmicro.GetNoise(warpx * 1.25f, warpy * 1.25f)),
                        width = 0.70f * (1.0f - smoothstep(-2.0f, 3.0f, sample.temperature));
            // Let grass paths enter the cold side too, without a solid-cover discontinuity at zero degrees.
            if(sample.temperature <= -2.0f || channels < width) return WORLD_SNOWY_GRASS;

            // Residual snow collects in scattered, multi-block pockets around the broad melt paths.
            // Jittered, rotated ellipses give each spot a minimum size instead of thresholding voxel noise.
            const int cellx = int(floorf(float(x) / 32.0f)), celly = int(floorf(float(y) / 32.0f));
            const float proximity = 1.0f - smoothstep(0.0f, 0.35f, channels - width),
                        chance = (0.30f + 0.45f * proximity) * (1.0f - smoothstep(1.0f, 3.0f, sample.temperature)),
                        rim = 1.0f + 0.18f * coldmicro.GetNoise(float(x) * 3.0f + 5713.0f, float(y) * 3.0f - 2137.0f);
            for(int cy = celly - 1; cy <= celly + 1; ++cy) for(int cx = cellx - 1; cx <= cellx + 1; ++cx)
            {
                if(treespatialunit(uint(seed), cx, cy, 0x534E4201U) >= chance) continue;
                const float centerx = (cx + 0.15f + 0.70f * treespatialunit(uint(seed), cx, cy, 0x534E4202U)) * 32.0f,
                            centery = (cy + 0.15f + 0.70f * treespatialunit(uint(seed), cx, cy, 0x534E4203U)) * 32.0f,
                            radius = 5.0f + 7.0f * treespatialunit(uint(seed), cx, cy, 0x534E4204U),
                            aspect = 0.70f + 0.50f * treespatialunit(uint(seed), cx, cy, 0x534E4205U),
                            angle = 2.0f * M_PI * treespatialunit(uint(seed), cx, cy, 0x534E4206U),
                            dx = float(x) - centerx, dy = float(y) - centery,
                            u = (dx * cosf(angle) + dy * sinf(angle)) / radius,
                            v = (dy * cosf(angle) - dx * sinf(angle)) / (radius * aspect);
                if(u * u + v * v < rim) return WORLD_SNOWY_GRASS;
            }
        }

        // warm climates retain the simple soil logic
        const BiomeSample soil = samplesoil(position);

        return sandcoverage(soil) > 0.5f ? WORLD_BIOME_DESERT : WORLD_BIOME_PLAINS;
    }

    bool worldgenerator::cliff(int x, int y, int height, bool *face) const
    {
        if(face) *face = false;
        const float noisex = x + 10000.5f, noisey = y - 10000.5f,
                    continental = samplecontinental(*this, noisex, noisey), threshold = landthreshold(settings),
                    cliffstrength = samplecliffstrength(*this, noisex, noisey);
        if(continental >= threshold && height >= settings.sealevel + 2 && cliffstrength > 0.25f)
        {
            const float shoredistance = samplecoastdistance(*this, noisex, noisey, continental),
                        cliffshape = clamp(coastshape.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                        cliffheight = settings.cliffmaxheight * (0.4375f + 0.5625f * cliffshape);
            float beachspan, plainrun, plainlevel;
            samplecoastprofile(*this, noisex, noisey, beachspan, plainrun, plainlevel);
            const float plainend = 2.0f * beachspan * powf(1.0f - cliffstrength, 4.0f) + plainrun,
                        plateauend = max(plainend, max(64.0f, cliffheight * 4.0f)),
                        blendend = plateauend + max(128.0f, cliffheight * 10.0f);

            // Back the entire raised coast with stone, including recessed columns along a jagged shoreline.
            if(shoredistance <= blendend)
            {
                if(face && shoredistance < 8.0f)
                {
                    // The rising sea face is bare rock. At its crest, only cap columns that are not below a higher ledge.
                    *face = shoredistance < 2.0f;
                    for(int dy = -1; dy <= 1 && !*face; ++dy) for(int dx = -1; dx <= 1 && !*face; ++dx)
                        if((dx || dy) && this->height(x + dx, y + dy) > height + 1) *face = true;
                }
                return true;
            }
        }
        return false;
    }

    bool worldgenerator::rock(int x, int y, int height) const
    {
        const float low = min(settings.stonelow, settings.stonehigh);
        const float high = max(settings.stonelow, settings.stonehigh);
        if(height <= low) return false;
        if(height >= high) return true;

        const float rockweight = smoothstep(low, high, height);
        const float selector = clamp(rockiness.GetNoise(x + 10000.5f, y - 10000.5f) * 1.25f + 0.5f, 0.0f, 1.0f);
        return rockweight > selector;
    }

    static uint worldtreehash(uint seed, int chunkx, int chunky, int blockx, int blocky, uint salt)
    {
        const uint worldx = uint(chunkx) * 64U + uint(blockx), worldy = uint(chunky) * 64U + uint(blocky);
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

    struct queriedworldtree
    {
        int x, y, base, height;
        uint priority, shape;
        int species;

        queriedworldtree() : x(0), y(0), base(0), height(0), priority(0), shape(0), species(TREE_REGULAR) {}
    };

    float treesuitability(float temperature, float humidity)
    {
        // Dryness alone does not erase cool woodland, and heat alone does not erase rainforest.
        const float hotdry = smoothstep(22.0f, 34.0f, temperature) * (1.0f - smoothstep(15.0f, 45.0f, humidity));
        return smoothstep(-11.0f, 3.0f, temperature) * (0.65f + 0.35f * smoothstep(15.0f, 70.0f, humidity)) *
               (1.0f - 0.98f * hotdry);
    }

    static float woodlanddensity(const worldgenerator &generator, int x, int y, int height, const BiomeSample &sample, float freshwater)
    {
        const float suitability = treesuitability(sample.temperature, sample.humidity);
        if(suitability <= 0.0f || generator.settings.basetreedensity <= 0.0f) return 0.0f;

        const float noisex = x + 10000.5f, noisey = y - 10000.5f,
                    broad = generator.vegetationvariation.GetNoise(noisex * 0.28f + 1731.0f, noisey * 0.28f - 2917.0f),
                    local = generator.vegetationvariation.GetNoise(noisex, noisey),
                    patch = clamp(0.5f + 0.80f * broad + 0.25f * local, 0.0f, 1.0f);

        const worldtectonicsample relief = generator.tectonics(x, y);
        const float altitude = float(height - generator.settings.sealevel),
                    foothills = smoothstep(0.10f, 0.48f, relief.terrainroughness) * smoothstep(4.0f, 24.0f, altitude),

                    mountainbelt = max(foothills, smoothstep(28.0f, 65.0f, altitude)),
                    woodland = smoothstep(0.38f - 0.22f * mountainbelt, 0.70f - 0.20f * mountainbelt, patch),
                    scattered = 0.12f + 0.20f * smoothstep(-0.5f, 0.5f, local),
                    densityfactor = scattered + (3.0f + 2.0f * mountainbelt) * woodland,

                    waterbonus = 1.0f + 0.45f * freshwater,

                    density = generator.settings.basetreedensity * suitability * densityfactor * waterbonus;

        // hot and relatively dry.
        const float spirouland = smoothstep(20.0f, 28.0f, sample.temperature) * (1.0f - smoothstep(35.0f, 60.0f, sample.humidity));

        // Keep hot zones mostly open, but allow denser vegetation near freshwater.
        const float spiroulandrelief = clamp(freshwater * 0.65f, 0.0f, 0.65f);
        const float spiroulandtarget = 0.18f + spiroulandrelief;
        const float spiroulandpenalty = 1.0f * (1.0f - spirouland) + spiroulandtarget * spirouland;

        return clamp(density * spiroulandpenalty, 0.0f, 1.0f);
    }

    bool worldgenerator::treeweights(int x, int y, int height, const BiomeSample &sample, float (&weights)[TREE_SPECIES_COUNT], int material, float spawn) const
    {
        loopi(TREE_SPECIES_COUNT) weights[i] = 0;
        if(settings.basetreedensity <= 0) return false;
        const float freshwater = hydrology->moisture(float(x), float(y), float(height)),
                    hot = smoothstep(18.0f, 30.0f, sample.temperature),
                    palmmoisture = smoothstep(25.0f, 50.0f, sample.humidity),
                    dry = 1.0f - smoothstep(30.0f, 60.0f, sample.humidity),
                    humid = smoothstep(35.0f, 80.0f, sample.humidity),
                    temperate = smoothstep(-2.0f, 6.0f, sample.temperature) * (1.0f - smoothstep(18.0f, 28.0f, sample.temperature));
        float palm = 0;
        if(hot > 0 && palmmoisture > 0)
        {
            ZoneScopedN("Trees/Palm suitability");
            const float patch = clamp(0.5f + vegetationvariation.GetNoise(x * 0.28f + 4531.0f, y * 0.28f - 713.0f), 0.0f, 1.0f),
                        grove = smoothstep(0.48f, 0.78f, patch);
            float coastal = 0;
            if(settings.coastwidth > 0)
            {
                // coast() raster-searches a large neighbourhood and invokes height() at every point.
                // Species suitability uses the same fixed-cost distance field as terrain and climate instead.
                const float noisex = x + 10000.5f, noisey = y - 10000.5f,
                            continental = samplecontinental(*this, noisex, noisey),
                            distance = samplecoastdistance(*this, noisex, noisey, continental),
                            configuredwidth = max(settings.coastwidth + biomeblend.GetNoise(noisex, noisey) * settings.coastvariation, 0.0f),
                            width = max(configuredwidth, coasttransitionwidth(x, y));
                coastal = 1.0f - smoothstep(width * 0.5f, max(width, 1.0f), distance);
            }
            // Local humidity includes freshwater influence; even oases must clear the survival threshold.
            palm = hot * palmmoisture * (0.004f + 0.22f * coastal * grove + 0.012f * dry * grove +
                                        (0.20f + 0.60f * dry) * freshwater * freshwater);
        }
        if(material < 0) material = surfacematerial(x, y, height, &sample);
        const bool sand = material == WORLD_BIOME_DESERT;
        const int beachmin = settings.sealevel + min(settings.beachminheight, settings.beachmaxheight),
                  beachmax = settings.sealevel + max(settings.beachminheight, settings.beachmaxheight);
        const bool shoreband = settings.coastwidth > 0 && height >= beachmin && height <= max(beachmax, settings.sealevel + 2);
        weights[TREE_PALM] = min(settings.basetreedensity * palm * (sand ? 0.15f : 1.0f), sand ? 0.012f : 0.035f);
        if(sand || shoreband) return spawn < weights[TREE_PALM];

        const float density = woodlanddensity(*this, x, y, height, sample, freshwater);
        // Species partition the woodland density without changing its total. Reject before the four
        // neighbouring height samples and pine/species calculations. Leave a margin for float summation.
        if(spawn >= density + weights[TREE_PALM] + 0.000001f) return false;

        const float savanna = smoothstep(20.0f, 28.0f, sample.temperature) * (1.0f - smoothstep(35.0f, 55.0f, sample.humidity));
        const float pine = treepinechance(settings, sample, uint(seed), x, y, height) * (1.0f - savanna),

                    open = 1.0f - smoothstep(0.7f, 2.4f, density / settings.basetreedensity),
                    poplarhabitat = temperate * humid * open,
                    left = poplarhabitat > 0 ? this->height(x - 4, y) : height,
                    right = poplarhabitat > 0 ? this->height(x + 4, y) : height,
                    down = poplarhabitat > 0 ? this->height(x, y - 4) : height,
                    up = poplarhabitat > 0 ? this->height(x, y + 4) : height,
                    basin = clamp((left + right + down + up - 4.0f * height) / 12.0f, 0.0f, 1.0f),
                    flat = 1.0f - smoothstep(2.0f, 10.0f, fabsf(right - left) + fabsf(up - down)),
                    exposure = clamp(0.5f + ((right - left) * 0.9701425f + (up - down) * 0.2425356f) / 16.0f, 0.0f, 1.0f),
                    wind = clamp(exposure * 0.6f + (coldregions.GetNoise(float(x), float(y)) * 0.5f + 0.5f) * 0.4f, 0.0f, 1.0f),
                    birch = (0.04f + 0.24f * humid) * temperate,
                    poplar = min(0.40f, temperate * humid * open * (0.12f + 0.28f * wind) * (0.35f + 0.65f * max(flat, max(basin, freshwater))));

        // Birch is at most 28% of the non-pine population, even in the wettest suitable forest.
        weights[TREE_PINE] = density * pine;
        weights[TREE_BIRCH] = density * (1.0f - pine) * birch;
        weights[TREE_POPLAR] = density * (1.0f - pine) * poplar;
        weights[TREE_REGULAR] = density * (1.0f - pine) * (1.0f - birch - poplar);
        return true;
    }

    float worldgenerator::treedensity(int x, int y, int height) const
    {
        const vec position(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS, worldclimate::GROUND_UNITS + float(height) * worldclimate::BLOCK_UNITS);
        float weights[TREE_SPECIES_COUNT], density = 0;
        treeweights(x, y, height, sampleBiome(position), weights);
        loopi(TREE_SPECIES_COUNT) density += weights[i];
        return clamp(density, 0.0f, 1.0f);
    }

    bool treewood(int type)
    {
        return type == WORLD_TREE_WOOD || type == WORLD_TREE_DARK_WOOD || type == WORLD_TREE_PALM_WOOD || type == WORLD_TREE_BIRCH_WOOD;
    }

    const char *treeblockname(int type)
    {
        static const char * const names[] =
        {
            "air", "wood", "dark_wood", "leaves", "needles", "palm_wood", "birch_wood", "palm_leaves", "birch_leaves"
        };
        return type >= 0 && type < WORLD_TREE_BLOCK_COUNT ? names[type] : "air";
    }

    int treeshaperadius(int species)
    {
        return species == TREE_PALM ? TREE_RADIUS : species == TREE_PINE ? 3 : species == TREE_POPLAR ? 1 : 2;
    }

    int treeshapeblock(int species, int height, uint shape, int x, int y, int z)
    {
        if(z < 0 || z > height) return WORLD_TREE_AIR;
        if(species == TREE_PALM)
        {
            const int direction = (shape >> 8) & 3U, lean = 1 + int((shape >> 10) & 1U),
                      dx = direction == 0 ? 1 : direction == 1 ? -1 : 0,
                      dy = direction == 2 ? 1 : direction == 3 ? -1 : 0,
                      bend = max(0, z - height / 3) * lean / max(1, height - 1 - height / 3),
                      previous = max(0, z - 1 - height / 3) * lean / max(1, height - 1 - height / 3);
            // Horizontal bridge cubes keep every stepped lean face-connected for foliage support.
            if(z < height && ((x == dx * bend && y == dy * bend) || (x == dx * previous && y == dy * previous)))
                return WORLD_TREE_PALM_WOOD;
            x -= dx * lean;
            y -= dy * lean;
            if(z == height && abs(x) + abs(y) <= 1 + int((shape >> 12) & 1U)) return WORLD_TREE_PALM_LEAVES;
            const int reachx = 2 + int((shape >> 13) & 1U), reachy = 2 + int((shape >> 14) & 1U),
                      distance = max(abs(x), abs(y));
            const bool frond = (y == 0 && abs(x) <= reachx) || (x == 0 && abs(y) <= reachy) ||
                               (((shape >> 15) & 1U) && abs(x) <= 2 && abs(y) <= 2);
            if(frond && ((distance <= 2 && z == height - 1) || (distance >= 2 && z == height - 2)))
                return WORLD_TREE_PALM_LEAVES;
            return WORLD_TREE_AIR;
        }
        if(species == TREE_POPLAR)
        {
            const int bottom = 2 + int((shape >> 11) & 1U),
                      trunkheight = height - 3 + int((shape >> 12) & 1U),
                      notch = bottom + 2 + int((shape >> 13) & 1U);
            if(!x && !y && z < trunkheight) return WORLD_TREE_WOOD;
            if(z < bottom || abs(x) > 1 || abs(y) > 1) return WORLD_TREE_AIR;
            // Full three-block-wide tufts alternate with narrow, connected leafy sections near the tip.
            // The trunk stops inside the crown, leaving several foliage-only levels above it.
            if(z == height) return !x && !y ? WORLD_TREE_LEAVES : WORLD_TREE_AIR;
            if(z == height - 2 || z == notch)
            {
                const bool shoulder = ((shape >> 14) & 1U) &&
                                      (((shape >> 15) & 1U) ? !x : !y) && abs(x) + abs(y) == 1;
                return (!x && !y) || shoulder ? WORLD_TREE_LEAVES : WORLD_TREE_AIR;
            }
            // Occasionally soften a lower tuft's corners while keeping the upper leafy cap full.
            if(z < height - 3 && abs(x) == 1 && abs(y) == 1 &&
               (worldtreehash(shape, x, y, z, height, 0x50F1A2B3U) & 3U) == 0) return WORLD_TREE_AIR;
            return WORLD_TREE_LEAVES;
        }
        if(!x && !y && z < height) return species == TREE_PINE ? WORLD_TREE_DARK_WOOD :
                                                         species == TREE_BIRCH ? WORLD_TREE_BIRCH_WOOD : WORLD_TREE_WOOD;
        if(species == TREE_PINE)
        {
            if(z == height) return !x && !y ? WORLD_TREE_NEEDLES : WORLD_TREE_AIR;
            const int radius = min(3, 1 + (height - z) / 3);
            return z >= 2 && abs(x) <= radius && abs(y) <= radius && abs(x) + abs(y) <= radius + 1 ?
                   WORLD_TREE_NEEDLES : WORLD_TREE_AIR;
        }
        const int bottom = height - 2 - (species == TREE_BIRCH ? int((shape >> 11) & 1U) : 0),
                  radius = z == height ? 1 : 2;
        if(z < bottom || abs(x) > radius || abs(y) > radius) return WORLD_TREE_AIR;
        if(radius == 2 && abs(x) == 2 && abs(y) == 2 && (worldtreehash(shape, x, y, z, height, 0xA511E9B3U) & 1U))
            return WORLD_TREE_AIR;
        return species == TREE_BIRCH ? WORLD_TREE_BIRCH_LEAVES : WORLD_TREE_LEAVES;
    }

    static bool sampleworldtreecandidate(const worldgenerator &generator, int x, int y, queriedworldtree &tree)
    {
        const int chunkx = x >= 0 ? x / 64 : (x - 63) / 64,
                  chunky = y >= 0 ? y / 64 : (y - 63) / 64,
                  blockx = x - chunkx * 64, blocky = y - chunky * 64;
        const uint spawn = worldtreehash(uint(generator.seed), chunkx, chunky, blockx, blocky, 0xD1B54A35U);
        // Bound woodland (5.32 * 1.45) plus palms before any height/hydrology query.
        // This only skips impossible candidates; it does not change spatial decisions.
        if(generator.settings.basetreedensity <= 0 ||
           worldtreeunit(spawn) >= min(1.0f, generator.settings.basetreedensity * 7.715f) + 0.045f) return false;
        const int height = generator.height(x, y);
        if(height < generator.surface(x, y).water) return false;
        const vec position(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                           worldclimate::GROUND_UNITS + float(height) * worldclimate::BLOCK_UNITS);
        const BiomeSample sample = generator.sampleBiome(position);
        const int material = generator.surfacematerial(x, y, height, &sample);
        if(material != WORLD_BIOME_PLAINS && material != WORLD_MOSS && material != WORLD_SNOWY_GRASS &&
           material != WORLD_FROZEN_DIRT && material != WORLD_FROZEN_MOSS && material != WORLD_BIOME_SNOW &&
           material != WORLD_BIOME_DESERT) return false;

        float weights[TREE_SPECIES_COUNT], density = 0;
        if(!generator.treeweights(x, y, height, sample, weights, material, worldtreeunit(spawn))) return false;
        loopi(TREE_SPECIES_COUNT) density += weights[i];

        if(worldtreeunit(spawn) >= density) return false;

        // Surface masks are independent of density. Evaluate them only for surviving candidates.
        bool cliffface = false;
        generator.cliff(x, y, height, &cliffface);
        if(generator.tectonics(x, y).rockyledge > 0.22f || cliffface || generator.rock(x, y, height)) return false;

        const uint shape = worldtreehash(uint(generator.seed), chunkx, chunky, blockx, blocky, 0x94D049BBU);

        float selection = worldtreeunit(shape) * density;
        tree.species = TREE_REGULAR;
        loopi(TREE_SPECIES_COUNT)
        {
            selection -= weights[i];
            if(selection < 0) { tree.species = i; break; }
        }

        tree.x = x;
        tree.y = y;
        tree.base = 256 + height;
        tree.height = treefinalheight(tree.species, sample.temperature, shape);
        tree.priority = spawn;
        tree.shape = shape;

        return tree.base + tree.height < 512;
    }

    static bool queryworldtreecandidate(const worldgenerator &generator, int x, int y, queriedworldtree &tree)
    {
        const ivec key(x, y, 0);
        const worldgenerator::treequery *cached = generator.treecandidatecache.access(key);
        if(!cached)
        {
            if(generator.treecandidatecache.numelems >= 1 << 16) generator.treecandidatecache.clear();
            worldgenerator::treequery result;
            result.valid = sampleworldtreecandidate(generator, x, y, tree);
            if(result.valid)
            {
                result.base = tree.base;
                result.height = tree.height;
                result.species = tree.species;
                result.shape = tree.shape;
                result.priority = tree.priority;
            }
            cached = &generator.treecandidatecache.access(key, result);
        }
        if(!cached->valid) return false;
        tree.x = x;
        tree.y = y;
        tree.base = cached->base;
        tree.height = cached->height;
        tree.species = cached->species;
        tree.shape = cached->shape;
        tree.priority = cached->priority;
        return true;
    }

    static bool queryworldtree(const worldgenerator &generator, int x, int y, queriedworldtree &tree)
    {
        if(!queryworldtreecandidate(generator, x, y, tree)) return false;
        for(int oy = -3; oy <= 3; ++oy) for(int ox = -3; ox <= 3; ++ox)
        {
            if(!ox && !oy) continue;
            // A neighbour that loses priority cannot suppress this tree, regardless of its species or habitat.
            const uint priority = worldtreehash(uint(generator.seed), 0, 0, x + ox, y + oy, 0xD1B54A35U);
            if(priority > tree.priority || (priority == tree.priority && (oy > 0 || (!oy && ox > 0)))) continue;
            queriedworldtree other;
            if(!queryworldtreecandidate(generator, x + ox, y + oy, other)) continue;
            const int spacing = tree.species == TREE_PALM || other.species == TREE_PALM ? 3 : 1;
            if(abs(ox) > spacing || abs(oy) > spacing) continue;
            if(other.priority < tree.priority || (other.priority == tree.priority &&
               (other.y < tree.y || (other.y == tree.y && other.x < tree.x))))
                return false;
        }
        return true;
    }

    bool worldgenerator::tree(int x, int y, int &base, int &height, uint &shape, int &species) const
    {
        const ivec key(x, y, 0);
        treequery *cached = treequerycache.access(key);
        if(!cached)
        {
            if(treequerycache.numelems >= 1 << 16) treequerycache.clear();
            treequery result;
            queriedworldtree tree;
            result.valid = queryworldtree(*this, x, y, tree);
            if(result.valid)
            {
                result.base = tree.base;
                result.height = tree.height;
                result.shape = tree.shape;
                result.species = tree.species;
            }
            cached = &treequerycache.access(key, result);
        }
        if(!cached->valid) return false;
        base = cached->base;
        height = cached->height;
        shape = cached->shape;
        species = cached->species;
        return true;
    }

    int worldgenerator::treecanopyheight(int x, int y) const
    {
        const ivec key(x, y, 0);
        if(int *cached = canopyheightcache.access(key)) return *cached;
        int top = -1;
        for(int ty = y - TREE_RADIUS; ty <= y + TREE_RADIUS; ++ty) for(int tx = x - TREE_RADIUS; tx <= x + TREE_RADIUS; ++tx)
        {
            queriedworldtree tree;
            tree.x = tx;
            tree.y = ty;
            if(!this->tree(tx, ty, tree.base, tree.height, tree.shape, tree.species)) continue;
            for(int z = tree.base + tree.height; z >= tree.base + 2 && z > top; --z)
                if(treeshapeblock(tree.species, tree.height, tree.shape, x - tx, y - ty, z - tree.base) != WORLD_TREE_AIR)
                {
                    top = z;
                    break;
                }
        }
        if(canopyheightcache.numelems >= 1 << 16) canopyheightcache.clear();
        canopyheightcache.access(key, top);
        return top;
    }

    int worldgenerator::treegroundmaterial(int x, int y, int height, int material) const
    {
        if(material != WORLD_SNOWY_GRASS && material != WORLD_BIOME_SNOW) return material;
        const int ground = int(worldclimate::GROUND_UNITS / worldclimate::BLOCK_UNITS) + height - 1;
        if(treecanopyheight(x, y) <= ground) return material;
        const bool edge = treecanopyheight(x - 1, y) <= ground || treecanopyheight(x + 1, y) <= ground ||
                          treecanopyheight(x, y - 1) <= ground || treecanopyheight(x, y + 1) <= ground;
        // Absolute coordinates keep the same 33% edge decisions across chunks and LOD tiers.
        if(edge && treespatialunit(uint(seed), x, y, 0x534E4F57U) < 0.33f) return material;
        return WORLD_BIOME_PLAINS;
    }

    int worldgenerator::treeblock(int x, int y, int z) const
    {
        if(treeblockcache.numelems >= 1 << 18) treeblockcache.clear();
        const ivec key(x, y, z);
        int *cached = treeblockcache.access(key);
        if(cached) return *cached;
        int foliage = WORLD_TREE_AIR;
        for(int treeY = y - TREE_RADIUS; treeY <= y + TREE_RADIUS; ++treeY) for(int treeX = x - TREE_RADIUS; treeX <= x + TREE_RADIUS; ++treeX)
        {
            queriedworldtree tree;
            if(!this->tree(treeX, treeY, tree.base, tree.height, tree.shape, tree.species)) continue;
            const int type = treeshapeblock(tree.species, tree.height, tree.shape, x - treeX, y - treeY, z - tree.base);
            if(treewood(type))
            {
                treeblockcache.access(key, type);
                return type;
            }
            if(foliage == WORLD_TREE_AIR) foliage = type;
        }
        treeblockcache.access(key, foliage);
        return foliage;
    }

    static uint grassclimaterevision = 0;

    worldgenerator &getenvironmentgenerator()
    {
        // Main-thread environment queries retain drainage plans as the camera moves.
        // Generation workers always use their own generator and never touch this cache.
        static struct cache
        {
            worldgenerator *generator;
            cache() : generator(NULL) {}
            ~cache() { delete generator; }
        } state;
        const worldsettings settings;
        if(!state.generator || state.generator->seed != getworldseed() ||
           memcmp(&state.generator->settings, &settings, sizeof(settings)))
        {
            delete state.generator;
            state.generator = new worldgenerator(getworldseed(), settings);
            ++grassclimaterevision;
        }
        return *state.generator;
    }

    vec worldgenerator::terrainclimate(const vec &absolute, bool transition) const
    {
        float distance = 64.0f;
        if(transition)
        {
            // Estimate distance to the existing warped desert threshold on the climate lattice, never from voxel neighbours.
            const float step = 4.0f * worldclimate::BLOCK_UNITS;
            const float soil = sandcoverage(samplesoil(absolute)) - 0.5f,
                        dx = (sandcoverage(samplesoil(vec(absolute).add(vec(step, 0, 0)))) -
                              sandcoverage(samplesoil(vec(absolute).sub(vec(step, 0, 0))))) / 8.0f,
                        dy = (sandcoverage(samplesoil(vec(absolute).add(vec(0, step, 0)))) -
                              sandcoverage(samplesoil(vec(absolute).sub(vec(0, step, 0))))) / 8.0f;
            distance = -soil / max(sqrtf(dx * dx + dy * dy), 0.0001f);
            const float x = absolute.x / worldclimate::BLOCK_UNITS, y = absolute.y / worldclimate::BLOCK_UNITS;
            if(settings.coastwidth > 0)
            {
                const int bx = int(floorf(x)), by = int(floorf(y));
                const float continental = samplecontinental(*this, x + 10000.5f, y - 10000.5f),
                            shore = samplecoastdistance(*this, x + 10000.5f, y - 10000.5f, continental),
                            width = beachtransitionwidth(bx, by);
                if(shore - width < 64.0f)
                {
                    // The visible beach often ends at its height cap, well before the shoreline-distance cap.
                    // Sample terrain height, not vertex height: the same edge must extend down exposed block sides.
                    const int ground = height(bx, by);
                    const float gx = (height(bx + 4, by) - height(bx - 4, by)) / 8.0f,
                                gy = (height(bx, by + 4) - height(bx, by - 4)) / 8.0f;
                    if(!cliff(bx, by, ground))
                        distance = min(distance, getbeachtintdistance(shore, width, float(ground),
                            float(settings.sealevel + min(settings.beachminheight, settings.beachmaxheight)),
                            float(settings.sealevel + max(settings.beachminheight, settings.beachmaxheight)), sqrtf(gx * gx + gy * gy)));
                }
            }
        }
        // Byte 255 disables blending for LOD2; 0..254 represent -64..64 blocks.
        return vec(clamp((environmentclimate.gettemperature(absolute) + 10.0f) / 40.0f, 0.0f, 1.0f),
                   gethumidity(absolute) * 0.01f, transition ? (clamp(distance / 128.0f + 0.5f, 0.0f, 1.0f) * 254.0f / 255.0f) : 1.0f);
    }

    enum { CLIMATE_GRASS = 0, CLIMATE_TERRAIN, CLIMATE_WEEDS, NUM_CLIMATE_CACHES };

    static vec cachedworldclimate(const vec &absolute, int kind)
    {
        // A bounded, disposable cache of the existing climate, independent of terrain residency and LOD.
        // Fixed four-metre nodes give continuous trilinear color and avoid repeated hydrology/noise queries.
        enum { STEP = 64, CACHE_SIZE = 8192 };
        struct entry
        {
            ivec key;
            vec color;
            uint revision;
            entry() : key(0, 0, 0), color(0, 0, 0), revision(0) {}
        };
        static entry caches[NUM_CLIMATE_CACHES][CACHE_SIZE];
        entry *cache = caches[kind];
        worldgenerator &generator = getenvironmentgenerator();
        const vec grid = vec(absolute).div(float(STEP));
        const ivec base(int(floorf(grid.x)), int(floorf(grid.y)), int(floorf(grid.z)));
        const vec fraction = vec(grid).sub(vec(base));
        vec result(0, 0, 0);
        loopi(8)
        {
            const ivec key(base.x + (i & 1), base.y + ((i >> 1) & 1), base.z + ((i >> 2) & 1));
            const float weight = (i & 1 ? fraction.x : 1 - fraction.x) * (i & 2 ? fraction.y : 1 - fraction.y) *
                                 (i & 4 ? fraction.z : 1 - fraction.z);
            if(weight <= 0) continue;
            const uint hash = uint(key.x) * 0x8DA6B343U ^ uint(key.y) * 0xD8163841U ^ uint(key.z) * 0xCB1AB31FU;
            entry &sample = cache[hash & (CACHE_SIZE - 1)];
            if(sample.revision != grassclimaterevision || sample.key != key)
            {
                const vec position = vec(key).mul(float(STEP));
                if(kind == CLIMATE_TERRAIN) sample.color = generator.terrainclimate(position);
                else
                {
                    const float temperature = generator.environmentclimate.gettemperature(position), humidity = generator.gethumidity(position);
                    sample.color = kind == CLIMATE_WEEDS ? getweedclimatecolor(temperature, humidity) : getgrassclimatecolor(temperature, humidity);
                }
                sample.key = key;
                sample.revision = grassclimaterevision;
            }
            result.add(vec(sample.color).mul(weight));
        }
        return result;
    }

    vec getgrassworldcolor(const vec &absolute)
    {
        return cachedworldclimate(absolute, CLIMATE_GRASS);
    }

    vec getweedworldcolor(const vec &absolute)
    {
        return cachedworldclimate(absolute, CLIMATE_WEEDS);
    }

    vec getterrainworldclimate(const vec &absolute)
    {
        return cachedworldclimate(absolute, CLIMATE_TERRAIN);
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

}

#ifndef STANDALONE
ICOMMAND(worldloadseed, "i", (int *seed),
{
    if(game::waitforserveredit())
    {
        conoutf(CON_ERROR, "the multiplayer server owns the world seed");
        return;
    }
    game::loadworldseed(*seed);
});
#endif
