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
    // Albedo of white grass, indexed by Celsius (-10, 10, 30) and humidity percent (0, 50, 100).
    // Only coloration clamps here; the physical climate retains its full range.
    inline vec getgrassclimatecolor(float temperatureC, float humidityPercent)
    {
        static const vec colors[3][3] =
        {
            // Dry, medium, humid: cold adds blue; moisture deepens and saturates green.
            { vec(126, 148, 100), vec(94, 160, 100), vec(54, 142, 78) },
            { vec(151, 175, 55), vec(108, 190, 49), vec(58, 158, 38) },
            { vec(194, 157, 58), vec(174, 176, 51), vec(89, 154, 36) }
        };
        const float t = clamp((temperatureC + 10.0f) / 20.0f, 0.0f, 2.0f),
                    h = clamp(humidityPercent / 50.0f, 0.0f, 2.0f);
        const int ti = min(int(t), 1), hi = min(int(h), 1);
        vec dry, wet;
        dry.lerp(colors[ti][hi], colors[ti + 1][hi], t - ti);
        wet.lerp(colors[ti][hi + 1], colors[ti + 1][hi + 1], t - ti);
        return vec().lerp(dry, wet, h - hi).div(255.0f);
    }

    // Absolute engine coordinates: one 16-unit block is one metre; terrain datum is z = 4096.
    // Convert floating-origin camera coordinates to absolute coordinates before sampling.
    // Fixed regional frequencies (per metre) make baseline climate reproducible from the saved seed.
    struct worldclimate
    {
        enum { BLOCK_UNITS = 16, GROUND_UNITS = 4096 };
        FastNoiseLite regionaltemperature, regionalhumidity;
        float sealevel, temperaturelapserate;

        worldclimate(int seed, float sealevel, float temperaturelapserate)
            : sealevel(sealevel), temperaturelapserate(temperaturelapserate)
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
            // A consistent cooling rate lets each region reach freezing at its own altitude.
            return getregionaltemperature(worldpos) - getaltitude(worldpos) * temperaturelapserate;
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
