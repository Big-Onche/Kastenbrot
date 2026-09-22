#include "game.h"

namespace game
{
    // Beach sand is the intersection of a shoreline band and an inclusive integer height band.
    // Half-block thresholds put the tint boundary between the last sand terrace and the first grass terrace.
    float getbeachtintdistance(float shore, float width, float height, float low, float high, float slope)
    {
        const float elevation = max(low - 0.5f - height, height - high - 0.5f) / max(slope, 0.125f);
        return max(shore - width, elevation);
    }

    // Celsius (-10, 10, 30) and humidity percent (0, 50, 100). Only coloration clamps here.
    vec interpolatevegetationcolor(const vec (&colors)[3][3], float temperatureC, float humidityPercent)
    {
        const float t = clamp((temperatureC + 10.0f) / 20.0f, 0.0f, 2.0f), h = clamp(humidityPercent / 50.0f, 0.0f, 2.0f);
        const int ti = min(int(t), 1), hi = min(int(h), 1);
        vec dry, wet;
        dry.lerp(colors[ti][hi], colors[ti + 1][hi], t - ti);
        wet.lerp(colors[ti][hi + 1], colors[ti + 1][hi + 1], t - ti);
        return vec().lerp(dry, wet, h - hi).div(255.0f);
    }

    vec getgrassclimatecolor(float temperatureC, float humidityPercent)
    {
        static const vec colors[3][3] = {// Dry, medium, humid: cold adds blue; moisture deepens and saturates green.
                                         {vec(126, 148, 100), vec(94, 160, 100), vec(54, 142, 78)},
                                         {vec(151, 175, 55), vec(108, 190, 49), vec(58, 158, 38)},
                                         {vec(194, 157, 58), vec(174, 176, 51), vec(89, 154, 36)}};
        return interpolatevegetationcolor(colors, temperatureC, humidityPercent);
    }

    vec getweedclimatecolor(float temperatureC, float humidityPercent)
    {
        static const vec colors[3][3] = {// Dry, medium, humid: cold stays pale blue-green; heat dries foliage to bright straw yellow.
                                         // Moisture retains saturated greens with enough brightness for the grayscale texture's shading.
                                         {vec(139, 202, 155), vec(116, 195, 144), vec(87, 181, 124)},
                                         {vec(204, 199, 57), vec(99, 190, 38), vec(43, 163, 27)},
                                         {vec(240, 211, 69), vec(186, 195, 43), vec(53, 160, 25)}};
        return interpolatevegetationcolor(colors, temperatureC, humidityPercent);
    }

    worldclimate::worldclimate(int seed, float sealevel, float temperaturelapserate) : sealevel(sealevel), temperaturelapserate(temperaturelapserate)
    {
        regionaltemperature.SetSeed(seed ^ 0x37A91D25);
        regionalhumidity.SetSeed(seed ^ 0x62B4E713);
        regionaltemperature.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        regionalhumidity.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        regionaltemperature.SetFrequency(0.00032f);
        regionalhumidity.SetFrequency(0.00011f);
    }

    float worldclimate::getaltitude(const vec &worldpos) const
    {
        return (worldpos.z - GROUND_UNITS) / BLOCK_UNITS - sealevel;
    }

    float worldclimate::getregionaltemperature(const vec &worldpos) const
    {
        return 10.0f + 35.0f * regionaltemperature.GetNoise(worldpos.x / BLOCK_UNITS + 10000.5f, worldpos.y / BLOCK_UNITS - 10000.5f);
    }

    float worldclimate::gettemperature(const vec &worldpos) const
    {
        const float altitude = max(getaltitude(worldpos), 0.0f);

        const float normalized = clamp(altitude / 255.0f, 0.0f, 1.0f);
        // Default cooling: about 30 degrees at 200 blocks and 40 degrees at 255 blocks.
        // Keep the low foothill climate while bringing the full mountain curve in above it.
        const float curvature = powf(normalized, 1.7f), regional = getregionaltemperature(worldpos),
                    foothillcooling = altitude * temperaturelapserate * (0.65f + 0.85f * powf(normalized, 1.5f)),
                    mountaincooling = altitude * (temperaturelapserate + 0.01f) + 12.0f * curvature, mountain = transition(55.0f, 100.0f, altitude),
                    temperature = regional - (foothillcooling + (mountaincooling - foothillcooling) * mountain),
                    // Even the warmest regional extremes converge below freezing at the world ceiling.
            summit = transition(220.0f, 255.0f, altitude);
        return temperature - max(temperature + 2.0f, 0.0f) * summit;
    }

    float worldclimate::transition(float low, float high, float value)
    {
        const float t = clamp((value - low) / (high - low), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float worldclimate::gethumidity(const vec &worldpos, float coast, float freshwater) const
    {
        const float altitude = getaltitude(worldpos),
                    regional = 50.0f + 50.0f * regionalhumidity.GetNoise(worldpos.x / BLOCK_UNITS + 10000.5f, worldpos.y / BLOCK_UNITS - 10000.5f),
                    upland = transition(40.0f, 100.0f, altitude) * (1.0f - transition(200.0f, 260.0f, altitude)),
                    highmountain = transition(260.0f, 400.0f, altitude), marine = coast * (1.0f - transition(40.0f, 240.0f, fabsf(altitude)));
        return clamp(regional + 18.0f * upland - 10.0f * highmountain + 15.0f * marine + 20.0f * freshwater, 0.0f, 100.0f);
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
            ~cache()
            {
                delete generator;
            }
        } state;
        const worldsettings settings;
        if(!state.generator || state.generator->seed != getworldseed() || memcmp(&state.generator->settings, &settings, sizeof(settings)))
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
                              sandcoverage(samplesoil(vec(absolute).sub(vec(step, 0, 0))))) /
                             8.0f,
                        dy = (sandcoverage(samplesoil(vec(absolute).add(vec(0, step, 0)))) -
                              sandcoverage(samplesoil(vec(absolute).sub(vec(0, step, 0))))) /
                             8.0f;
            distance = -soil / max(sqrtf(dx * dx + dy * dy), 0.0001f);
            const float x = absolute.x / worldclimate::BLOCK_UNITS, y = absolute.y / worldclimate::BLOCK_UNITS;
            if(settings.coastwidth > 0)
            {
                const int bx = int(floorf(x)), by = int(floorf(y));
                const float continental = samplecontinental(*this, x + 10000.5f, y - 10000.5f),
                            shore = samplecoastdistance(*this, x + 10000.5f, y - 10000.5f, continental), width = beachtransitionwidth(bx, by);
                if(shore - width < 64.0f)
                {
                    // The visible beach often ends at its height cap, well before the shoreline-distance cap.
                    // Sample terrain height, not vertex height: the same edge must extend down exposed block sides.
                    const int ground = height(bx, by);
                    const float gx = (height(bx + 4, by) - height(bx - 4, by)) / 8.0f, gy = (height(bx, by + 4) - height(bx, by - 4)) / 8.0f;
                    if(!cliff(bx, by, ground))
                        distance =
                            min(distance, getbeachtintdistance(shore, width, float(ground),
                                                               float(settings.sealevel + min(settings.beachminheight, settings.beachmaxheight)),
                                                               float(settings.sealevel + max(settings.beachminheight, settings.beachmaxheight)),
                                                               sqrtf(gx * gx + gy * gy)));
                }
            }
        }
        // Byte 255 disables blending for LOD2; 0..254 represent -64..64 blocks.
        return vec(clamp((environmentclimate.gettemperature(absolute) + 10.0f) / 40.0f, 0.0f, 1.0f), gethumidity(absolute) * 0.01f,
                   transition ? (clamp(distance / 128.0f + 0.5f, 0.0f, 1.0f) * 254.0f / 255.0f) : 1.0f);
    }

    enum
    {
        CLIMATE_GRASS = 0,
        CLIMATE_TERRAIN,
        CLIMATE_WEEDS,
        NUM_CLIMATE_CACHES
    };

    static vec cachedworldclimate(const vec &absolute, int kind)
    {
        // A bounded, disposable cache of the existing climate, independent of terrain residency and LOD.
        // Fixed four-metre nodes give continuous trilinear color and avoid repeated hydrology/noise queries.
        enum
        {
            STEP = 64,
            CACHE_SIZE = 8192
        };
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
            const float weight =
                (i & 1 ? fraction.x : 1 - fraction.x) * (i & 2 ? fraction.y : 1 - fraction.y) * (i & 4 ? fraction.z : 1 - fraction.z);
            if(weight <= 0) continue;
            const uint hash = uint(key.x) * 0x8DA6B343U ^ uint(key.y) * 0xD8163841U ^ uint(key.z) * 0xCB1AB31FU;
            entry &sample = cache[hash & (CACHE_SIZE - 1)];
            if(sample.revision != grassclimaterevision || sample.key != key)
            {
                const vec position = vec(key).mul(float(STEP));
                if(kind == CLIMATE_TERRAIN)
                    sample.color = generator.terrainclimate(position);
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

} // namespace game
