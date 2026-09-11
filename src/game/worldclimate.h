#ifndef __GAME_WORLDCLIMATE_H__
#define __GAME_WORLDCLIMATE_H__

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

namespace game
{
    // Absolute engine coordinates: one 16-unit block is one metre; terrain datum is z = 4096.
    // Convert floating-origin camera coordinates to absolute coordinates before sampling.
    // Fixed regional frequencies (per metre) make baseline climate reproducible from the saved seed.
    struct worldclimate
    {
        enum { BLOCK_UNITS = 16, GROUND_UNITS = 4096 };
        FastNoiseLite regionaltemperature, regionalhumidity;
        float sealevel, snowheight;

        worldclimate(int seed, float sealevel, float snowheight) : sealevel(sealevel), snowheight(snowheight)
        {
            regionaltemperature.SetSeed(seed ^ 0x37A91D25);
            regionalhumidity.SetSeed(seed ^ 0x62B4E713);
            regionaltemperature.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
            regionalhumidity.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
            regionaltemperature.SetFrequency(0.00032f);
            regionalhumidity.SetFrequency(0.00011f);
        }

        float getaltitude(const vec &worldpos) const
        {
            return (worldpos.z - GROUND_UNITS) / BLOCK_UNITS - sealevel;
        }

        float getregionaltemperature(const vec &worldpos) const
        {
            return 10.0f + 35.0f * regionaltemperature.GetNoise(worldpos.x / BLOCK_UNITS + 10000.5f,
                                                              worldpos.y / BLOCK_UNITS - 10000.5f);
        }

        float gettemperature(const vec &worldpos) const
        {
            const float regional = getregionaltemperature(worldpos),
                        snowaltitude = max(snowheight - sealevel, 1.0f),
                        lapserate = max(0.0065f, regional / snowaltitude);
            // Compress mountain cooling to the world's snow line. Warm regions reach zero there;
            // already cold regions retain at least the physical lapse rate and never warm with altitude.
            return regional - getaltitude(worldpos) * lapserate;
        }

        static float transition(float low, float high, float value)
        {
            const float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        // Geography supplies continuous proximity weights, without coupling climate to terrain generation.
        // The altitude band is measured in blocks/metres above sea level; weather remains separate.
        float gethumidity(const vec &worldpos, float coast = 0, float freshwater = 0) const
        {
            const float altitude = getaltitude(worldpos),
                        regional = 50.0f + 50.0f * regionalhumidity.GetNoise(worldpos.x / BLOCK_UNITS + 10000.5f,
                                                                           worldpos.y / BLOCK_UNITS - 10000.5f),
                        upland = transition(40.0f, 100.0f, altitude) * (1.0f - transition(200.0f, 260.0f, altitude)),
                        highmountain = transition(260.0f, 400.0f, altitude),
                        marine = coast * (1.0f - transition(40.0f, 240.0f, fabsf(altitude)));
            return clamp(regional + 18.0f * upland - 10.0f * highmountain + 15.0f * marine + 20.0f * freshwater, 0.0f, 100.0f);
        }
    };
}

#endif
