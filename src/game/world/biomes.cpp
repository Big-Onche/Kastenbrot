#include "game.h"

namespace game
{
    // Ranges normalize Celsius and percent independently; density scales existing climate suitability.
    const ClimateBiome climateBiomes[] = { // type, name, identifier, temp, humidity, temp range, humidity range, tree density
        {WORLD_BIOME_SNOW_DESERT, "Snow Desert", "snow_desert", -20, 20, 10, 25, 0.0f},
        {WORLD_BIOME_TUNDRA, "Tundra", "tundra", -10, 40, 12, 25, 0.02f},
        {WORLD_BIOME_TAIGA, "Taiga", "taiga", 3, 60, 12, 25, 0.75f},
        {WORLD_BIOME_COLD_DESERT, "Cold Desert", "cold_desert", 5, 15, 12, 25, 0.02f},
        {WORLD_BIOME_PLAINS, "Grassland", "grassland", 14, 40, 12, 25, 0.10f},
        {WORLD_BIOME_FOREST, "Temperate Forest", "forest", 15, 70, 12, 25, 1.00f},
        {WORLD_BIOME_DESERT, "Desert", "desert", 35, 20, 12, 30, 0.0f},
        {WORLD_BIOME_SAVANNA, "Savanna", "savanna", 27, 40, 12, 25, 0.06f},
        {WORLD_BIOME_RAINFOREST, "Tropical Rainforest", "rainforest", 27, 85, 12, 25, 1.0f}};
    const int climateBiomeCount = sizeof(climateBiomes) / sizeof(climateBiomes[0]);

    const char *biomeName(int biome)
    {
        loopi(climateBiomeCount)
            if(climateBiomes[i].type == biome) return climateBiomes[i].name;
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
                        dh = (humidity - biome.humidityCenter) / biome.humidityRange, distance = dt * dt + dh * dh;

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
        const float coldmix = 1.0f - smoothstep(4.0f, 6.0f, temperature), taiga = smoothstep(-1.0f, 1.0f, temperature),
                    snowdesert = 1.0f - smoothstep(-11.0f, -9.0f, temperature), tundra = clamp(1.0f - taiga - snowdesert, 0.0f, 1.0f);

        // Taiga -> Tundra -> Snow Desert
        loopi(climateBiomeCount)
        {
            const int type = climateBiomes[i].type;

            if(type == WORLD_BIOME_TAIGA || type == WORLD_BIOME_TUNDRA || type == WORLD_BIOME_SNOW_DESERT || type == WORLD_BIOME_COLD_DESERT)
            {
                sample.weights[type] = 0.0f;
            }
        }

        // Renormalize the remaining warm biomes into the non-cold share
        float warmtotal = 0.0f;
        loopi(climateBiomeCount) warmtotal += sample.weights[climateBiomes[i].type];

        if(warmtotal > 1e-20f)
        {
            const float warmmix = 1.0f - coldmix;

            loopi(climateBiomeCount) sample.weights[climateBiomes[i].type] *= warmmix / warmtotal;
        }

        sample.weights[WORLD_BIOME_TAIGA] = coldmix * taiga;
        sample.weights[WORLD_BIOME_TUNDRA] = coldmix * tundra;
        sample.weights[WORLD_BIOME_SNOW_DESERT] = coldmix * snowdesert;
        sample.weights[WORLD_BIOME_COLD_DESERT] = 0.0f;

        // final normalization
        float total = 0.0f;
        loopi(climateBiomeCount) total += sample.weights[climateBiomes[i].type];

        if(total > 1e-20f)
        {
            loopi(climateBiomeCount) sample.weights[climateBiomes[i].type] /= total;
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
                               worldclimate::GROUND_UNITS + float(height) * worldclimate::BLOCK_UNITS))
            .primary;
    }

    static float desertshare(const BiomeSample &soil)
    {
        const float sand = soil.weights[WORLD_BIOME_DESERT];
        float competitor = 0;
        loopi(climateBiomeCount)
            if(climateBiomes[i].type != WORLD_BIOME_DESERT) competitor = max(competitor, soil.weights[climateBiomes[i].type]);
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
        const vec lookup =
            vec(position).add(soilblendoffset(biomeedgewarp, position.x / worldclimate::BLOCK_UNITS, position.y / worldclimate::BLOCK_UNITS));
        // Ocean air must not create a grass belt between a desert and its beach.
        // Keep freshwater and altitude humidity in soil selection; physical climate still includes the coast.
        const float x = lookup.x / worldclimate::BLOCK_UNITS, y = lookup.y / worldclimate::BLOCK_UNITS,
                    z = (lookup.z - worldclimate::GROUND_UNITS) / worldclimate::BLOCK_UNITS;
        const float humidity = environmentclimate.gethumidity(lookup, 0.0f, freshwatermoisture(x, y, z));
        return sampleClimateBiome(environmentclimate.getregionaltemperature(lookup), humidity);
    }

    float worldgenerator::sandcoverage(const BiomeSample &soil) const
    {
        return smoothstep(0.35f, 0.65f, desertshare(soil));
    }

    int worldgenerator::surfacematerial(int x, int y, int height, const BiomeSample *climate) const
    {
        const vec position(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                           worldclimate::GROUND_UNITS + float(height) * worldclimate::BLOCK_UNITS);

        const BiomeSample sample = climate ? *climate : sampleBiome(position);
        const worldwatersample water = surface(x, y);

        // water / frozen water
        if(height < water.water)
        {
            const vec waterpos(position.x, position.y, worldclimate::GROUND_UNITS + water.water * worldclimate::BLOCK_UNITS);

            const float temperature = environmentclimate.gettemperature(waterpos);

            // Solid shore-fast ice ends at the shelf edge; offshore slabs keep their water cracks.
            if(water.freshwater ? temperature < -2.0f || (temperature < 0.0f && snowpatches.GetNoise(float(x), float(y)) > 0.0f)
                                : coastice(x, y, temperature))
            {
                return WORLD_FROZEN_WATER;
            }

            return WORLD_BIOME_OCEAN;
        }

        // Snow cover follows temperature in every biome, with coherent wind-shaped melt edges.
        if(sample.temperature < -4.0f) return WORLD_BIOME_SNOW;
        if(sample.temperature < 3.0f)
        {
            const float broad = snowpatches.GetNoise(float(x), float(y)), detail = coldmicro.GetNoise(float(x) + 731.0f, float(y) + 1913.0f),
                        drift = broad * 0.85f + detail * 0.15f, warmth = clamp((sample.temperature + 4.0f) / 6.0f, 0.0f, 1.0f);
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
            for(int cy = celly - 1; cy <= celly + 1; ++cy)
                for(int cx = cellx - 1; cx <= cellx + 1; ++cx)
                {
                    if(worldspatialunit(uint(seed), cx, cy, 0x534E4201U) >= chance) continue;
                    const float centerx = (cx + 0.15f + 0.70f * worldspatialunit(uint(seed), cx, cy, 0x534E4202U)) * 32.0f,
                                centery = (cy + 0.15f + 0.70f * worldspatialunit(uint(seed), cx, cy, 0x534E4203U)) * 32.0f,
                                radius = 5.0f + 7.0f * worldspatialunit(uint(seed), cx, cy, 0x534E4204U),
                                aspect = 0.70f + 0.50f * worldspatialunit(uint(seed), cx, cy, 0x534E4205U),
                                angle = 2.0f * M_PI * worldspatialunit(uint(seed), cx, cy, 0x534E4206U), dx = float(x) - centerx,
                                dy = float(y) - centery, u = (dx * cosf(angle) + dy * sinf(angle)) / radius,
                                v = (dy * cosf(angle) - dx * sinf(angle)) / (radius * aspect);
                    if(u * u + v * v < rim) return WORLD_SNOWY_GRASS;
                }
        }

        // warm climates retain the simple soil logic
        const BiomeSample soil = samplesoil(position);

        return sandcoverage(soil) > 0.5f ? WORLD_BIOME_DESERT : WORLD_BIOME_PLAINS;
    }

} // namespace game
