#include "game.h"

namespace game
{
    ColdSample worldgenerator::samplecold(int x, int y, int h, const BiomeSample &climate) const
    {
        ColdSample c = {};
        c.coldness = clamp((2.0f - climate.temperature) / 22.0f, 0.0f, 1.0f);
        const float humidity = clamp(climate.humidity * 0.01f, 0.0f, 1.0f);
        c.desert = (1.0f - smoothstep(-16.0f, -6.0f, climate.temperature)) * (1.0f - smoothstep(30.0f, 55.0f, climate.humidity));
        // Four-metre support reduces voxel quantization bias; sample the final carved surface across chunk boundaries.
        const float left = height(x - 4, y), right = height(x + 4, y), down = height(x, y - 4), up = height(x, y + 4), dx = (right - left) / 8.0f,
                    dy = (up - down) / 8.0f, gradient = sqrtf(dx * dx + dy * dy);
        c.slope = clamp(gradient / 1.25f, 0.0f, 1.0f);
        c.basin = clamp(0.5f + (left + right + down + up - 4.0f * h) / 12.0f, 0.0f, 1.0f);
        c.exposure = clamp(0.5f + (dx * 0.9701425f + dy * 0.2425356f) * 0.5f, 0.0f, 1.0f);
        c.deposition = clamp(c.basin * 0.6f + (1.0f - c.exposure) * 0.4f, 0.0f, 1.0f);
        c.region = coldregions.GetNoise(float(x), float(y)) * 0.5f + 0.5f;
        const float large = coldroll.GetNoise(float(x) * 0.83f, float(y) * 0.83f), small = snowpatches.GetNoise(float(x) * 0.23f, float(y) * 0.23f),
                    altitude = clamp(float(h - settings.sealevel) / 80.0f, 0.0f, 1.0f);
        c.snow = clamp(c.coldness * 0.45f + humidity * 0.08f + altitude * 0.08f + c.deposition * 0.22f + large * 0.25f + small * 0.12f +
                           c.desert * 0.25f - c.slope * 0.35f - c.exposure * 0.08f,
                       0.0f, 1.0f);
        // Persistent regional wind scour opens bare shelves even on otherwise flat polar ground.
        c.snow = max(0.0f, c.snow - c.desert * smoothstep(0.55f, 0.72f, c.region) * (0.55f + c.exposure * 0.20f));
        c.snow *= 1.0f - smoothstep(0.0f, 5.0f, climate.temperature);
        // One coherent soil field establishes regional identity. Fine fields only perturb its borders.
        const float region = coldroll.GetNoise(x * 0.833333f + 3171.0f, y * 0.833333f - 951.0f),
                    detail = coldmicro.GetNoise(x * 1.166667f, y * 1.166667f), micro = snowpatches.GetNoise(x * 0.714286f, y * 0.714286f),
                    patch = clamp(0.5f + region * 0.85f + detail * 0.10f + micro * 0.025f, 0.0f, 1.0f), dry = 1.0f - humidity;
        c.severity = 1.0f - smoothstep(-15.0f, 2.0f, climate.temperature);
        c.nearwater = surface(x, y).bank ? 1.0f : 0.0f;
        c.wetness = clamp(humidity * 0.65f + c.basin * 0.25f + c.nearwater * 0.10f, 0.0f, 1.0f);
        c.grassscore = 0.54f;
        c.dirtscore = dry * 0.28f + c.exposure * 0.14f + patch * 0.52f + c.slope * 0.12f;
        c.mossscore = c.wetness * 0.42f + (1.0f - patch) * 0.48f - c.slope * 0.18f;
        c.gravelscore = c.slope * 0.50f + c.exposure * 0.20f + dry * 0.15f + patch * 0.15f;
        const bool exposed = c.slope > 0.45f || c.exposure > 0.75f || c.nearwater > 0.5f || (dry > 0.72f && patch > 0.86f);
        // The pre-existing retention mask contributes to snow; a separate broad field forms deposition regions.
        const float snowregion =
            clamp(0.5f + coldroll.GetNoise(x * 0.833333f - 1921.0f, y * 0.833333f + 7813.0f) * 0.85f + detail * 0.08f, 0.0f, 1.0f);
        c.snowscore = clamp(c.severity * 0.24f + c.basin * 0.16f + altitude * 0.08f + snowregion * 0.46f + c.snow * 0.16f - c.slope * 0.25f -
                                c.exposure * 0.10f,
                            0.0f, 1.0f);
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
        c.vegetation = clamp(humidity * 0.45f + (1.0f - c.slope) * 0.25f + (1.0f - c.snow) * 0.25f + c.basin * 0.05f +
                                 coldmicro.GetNoise(float(x) * 2.5f, float(y) * 2.5f) * 0.15f,
                             0.0f, 1.0f);
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

} // namespace game
