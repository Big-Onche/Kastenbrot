// Compile from src with the normal include paths and -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections.
// Include production sampling code, replacing only console variable registration.
#define STANDALONE
#include <map>
#include <vector>
#include <set>
#include "game.h"
#undef VAR
#undef VARP
#undef FVAR
#define VAR(name, low, current, high) int name = current
#define VARP(name, low, current, high) int name = current
#define FVAR(name, low, current, high) float name = current
#include "../game/world.cpp"
#include <cassert>
int main()
{
    // Every definition owns its center. Distances remain normalized and continuous across climate-space boundaries.
    for(int i = 0; i < game::climateBiomeCount; ++i)
    {
        const game::ClimateBiome &definition = game::climateBiomes[i];
        assert(definition.temperatureRange > 0 && definition.humidityRange > 0);
        assert(game::sampleClimateBiome(definition.temperatureCenter, definition.humidityCenter).primary == definition.type);
    }
    for(int t = -100; t <= 100; ++t) for(int h = 0; h <= 100; ++h)
    {
        const game::BiomeSample a = game::sampleClimateBiome(t, h), b = game::sampleClimateBiome(t + 0.001f, h + 0.001f);
        float total = 0;
        assert(a.primary != a.secondary && a.secondary != game::WORLD_BIOME_OCEAN);
        assert(a.primaryWeight >= a.secondaryWeight);
        for(int i = 0; i < game::WORLD_BIOME_COUNT; ++i)
        {
            assert(std::isfinite(a.weights[i]) && a.weights[i] >= 0 && a.weights[i] <= a.primaryWeight);
            assert(fabsf(a.weights[i] - b.weights[i]) < 0.001f);
            total += a.weights[i];
        }
        assert(fabsf(total - 1) < 0.00001f);
    }
    // Regional heat must support sandy desert through moderate humidity, while cold/dry and hot/wet remain distinct.
    assert(game::sampleClimateBiome(5, 15).primary == game::WORLD_BIOME_COLD_DESERT);
    assert(game::sampleClimateBiome(40, 10).primary == game::WORLD_BIOME_DESERT);
    assert(game::sampleClimateBiome(40, 40).primary == game::WORLD_BIOME_DESERT);
    assert(game::sampleClimateBiome(40, 85).primary == game::WORLD_BIOME_RAINFOREST);
    game::worldsettings settings;
    settings.sealevel = 23;
    game::worldgenerator generator(1337, settings);
    const game::worldclimate &climate = generator.environmentclimate;
    float maxdt = 0, maxdh = 0;
    const int coordinates[][2] = {{0, 0}, {-64, 64}, {10000, -20000}, {-40000, 50000}, {80000, 90000}};
    for(const auto &xy : coordinates)
    {
        const vec sea(xy[0] * 16.0f, xy[1] * 16.0f, 4096.0f + settings.sealevel * 16.0f);
        const vec summit = vec(sea).add(vec(0, 0, 160 * 16)), lowland = vec(sea).sub(vec(0, 0, 800));
        assert(fabsf(climate.getaltitude(sea)) < 0.0001f);
        assert(climate.gettemperature(sea) == climate.getregionaltemperature(sea));
        assert(fabsf(climate.gettemperature(summit) - (climate.gettemperature(sea) - 16.0f)) < 0.0001f);
        assert(climate.gettemperature(lowland) > climate.gettemperature(sea));
        // Freezing elevation follows regional temperature, rather than a shared configured height.
        const vec freezing = vec(sea).add(vec(0, 0, climate.gettemperature(sea) / settings.temperaturelapserate * 16));
        assert(fabsf(climate.gettemperature(freezing)) < 0.0001f);
        for(float lapse : {0.0f, 0.05f, 0.2f})
        {
            const game::worldclimate adjusted(1337, settings.sealevel, lapse);
            assert(fabsf(adjusted.gettemperature(summit) - (adjusted.gettemperature(sea) - lapse * 160)) < 0.0001f);
        }
    }
    game::loadworldseed(1337);
    game::worldgenerator restored(game::getworldseed(), settings), other(4567, settings);
    bool changed = false;
    for(int x = -20000; x < 20000; ++x)
    {
        const vec a(x * 16.0f, 128, 4464), b = vec(a).add(vec(16, 0, 0));
        const float t = climate.gettemperature(a), h = climate.gethumidity(a);
        maxdt = max(maxdt, fabsf(t - climate.gettemperature(b)));
        maxdh = max(maxdh, fabsf(h - climate.gethumidity(b)));
        assert(h >= 0 && h <= 100);
        climate.gettemperature(vec(-x * 16.0f, 200000, 8000));
        assert(t == restored.environmentclimate.gettemperature(a));
        assert(h == restored.environmentclimate.gethumidity(a));
        changed |= t != other.environmentclimate.gettemperature(a);
        if(x % 64 == 0)
        {
            const vec left = vec(a).sub(vec(0.01f, 0, 0)), right = vec(a).add(vec(0.01f, 0, 0));
            assert(fabsf(climate.gettemperature(left) - climate.gettemperature(right)) < 0.001f);
            assert(fabsf(climate.gethumidity(left) - climate.gethumidity(right)) < 0.001f);
        }
    }
    assert(changed && maxdt < 0.06f && maxdh < 0.06f);
    assert(game::treesuitability(20, 0) == 0 && game::treesuitability(-20, 90) == 0);
    assert(game::treesuitability(20, 90) == 1 && game::treesuitability(50, 90) == 0);
    float previous = 0;
    for(int i = 0; i <= 10000; ++i)
    {
        const float suitability = game::treesuitability(20, i * 0.01f);
        assert(suitability >= previous && suitability - previous < 0.001f);
        previous = suitability;
    }
    // Exercise the real proximity sampler with known drainage geometry, without routing an entire world.
    generator.hydrology = new game::worldhydrology(generator);
    restored.hydrology = new game::worldhydrology(restored);
    game::worldhydrology::channel river = {};
    river.a.x = river.b.x = 64;
    river.a.y = -64;
    river.b.y = 128;
    river.a.head = river.b.head = 50;
    river.flow = 4;
    for(int ty = -2; ty <= 2; ++ty) for(int tx = -9; tx <= 9; ++tx)
    {
        const game::worldhydrology::key key(tx, ty);
        generator.hydrology->tiles[key].channels.push_back(river);
        restored.hydrology->tiles[key].channels.push_back(river);
    }
    const vec riverpos(64 * 16, 32 * 16, 4096 + 50 * 16);
    assert(generator.hydrology->moisture(64, 32, 50) == 1);
    assert(generator.hydrology->moisture(100, 32, 50) == 0);
    assert(generator.hydrology->moisture(64, 32, 120) == 0);
    assert(climate.gethumidity(riverpos, 0, 1) > climate.gethumidity(riverpos));
    assert(climate.gethumidity(riverpos, 1, 0) > climate.gethumidity(riverpos));
    const float coastal = generator.gethumidity(vec(100 * 16, 32 * 16, riverpos.z));
    assert(coastal >= climate.gethumidity(vec(100 * 16, 32 * 16, riverpos.z)));
    const vec low(0, 0, 4096 + settings.sealevel * 16);
    for(int altitude : {100, 150, 200})
        assert(climate.gethumidity(vec(low).add(vec(0, 0, altitude * 16))) > climate.gethumidity(low));
    assert(climate.gethumidity(vec(low).add(vec(0, 0, 350 * 16))) < climate.gethumidity(low));
    float maxdensitydelta = 0;
    for(int i = -640; i < 640; ++i)
    {
        const vec a(i * 1.6f, 32 * 16, riverpos.z), b = vec(a).add(vec(0.01f, 0, 0));
        const float h = generator.gethumidity(a);
        assert(h == restored.gethumidity(a));
        assert(fabsf(h - generator.gethumidity(b)) < 0.01f);
        assert(h >= 0 && h <= 100);
    }
    for(int x = -64; x < 64; ++x)
    {
        const vec pos(x * 16, 32 * 16, riverpos.z);
        const float density = generator.treedensity(x, 32, 50),
                    suitability = game::treesuitability(climate.gettemperature(pos), generator.gethumidity(pos));
        assert(density >= 0 && density <= 2.5f * settings.basetreedensity * suitability + 0.000001f);
        assert(density == restored.treedensity(x, 32, 50));
        maxdensitydelta = max(maxdensitydelta, fabsf(density - generator.treedensity(x + 1, 32, 50)));
    }
    assert(maxdensitydelta < 0.004f);
    // Local vegetation should leave substantial open areas independently of humidity boosts.
    int clear = 0;
    for(int x = -20000; x < 20000; ++x)
        if(generator.vegetationvariation.GetNoise(x + 10000.5f, -10000.5f) <= -0.10f) ++clear;
    assert(clear > 8000);
    // Mountain vegetation is bounded by local climate; cold slopes may be sparser than warm plains.
    double plainsdensity = 0, mountaindensity = 0;
    int openplains = 0, openmountains = 0;
    const int mountainheight = settings.sealevel + 90;
    for(int x = -512; x < 512; ++x)
    {
        const float plains = generator.treedensity(x, 32, settings.sealevel),
                    mountain = generator.treedensity(x, 32, mountainheight);
        plainsdensity += plains;
        mountaindensity += mountain;
        openplains += plains == 0;
        openmountains += mountain == 0;
        const vec pos(x * 16, 32 * 16, 4096 + mountainheight * 16);
        const float suitable = game::treesuitability(climate.gettemperature(pos), generator.gethumidity(pos));
        assert(mountain <= 2.5f * settings.basetreedensity * suitable + 0.000001f);
        assert(mountain == restored.treedensity(x, 32, mountainheight));
        assert(fabsf(generator.treedensity(x, 32, mountainheight - 1) -
                     generator.treedensity(x, 32, mountainheight)) < 0.002f);
    }
    assert(openplains > 0);
    printf("PASS: mountain/plains density ratio %.2f; open samples %d mountains, %d plains\n",
           mountaindensity / plainsdensity, openmountains, openplains);
    // Also check actual drainage planning across a generated tile edge, in opposite query orders.
    game::worldgenerator planned(1337, settings), reverse(1337, settings);
    const vec edge(64 * 16, 32 * 16, 4096 + 50 * 16),
              left = vec(edge).sub(vec(0.01f, 0, 0)), right = vec(edge).add(vec(0.01f, 0, 0));
    const float lefthumidity = planned.gethumidity(left), righthumidity = planned.gethumidity(right);
    assert(righthumidity == reverse.gethumidity(right));
    assert(lefthumidity == reverse.gethumidity(left));
    assert(fabsf(lefthumidity - righthumidity) < 0.01f);
    for(int x : {-65, -64, -1, 0, 63, 64, 10000})
    {
        const vec position(x * 16, 32 * 16, 4096 + 50 * 16);
        const game::BiomeSample sample = planned.sampleBiome(position), again = reverse.sampleBiome(position),
                                adjacent = planned.sampleBiome(vec(position).add(vec(0.001f, 0, 0)));
        assert(sample.temperature == planned.environmentclimate.gettemperature(position));
        assert(sample.humidity == planned.gethumidity(position));
        assert(planned.biome(x, 32, 50) == sample.primary);
        assert(planned.biome(x, 32, settings.sealevel - 1) == game::WORLD_BIOME_OCEAN);
        for(int i = 0; i < game::WORLD_BIOME_COUNT; ++i)
        {
            assert(sample.weights[i] == again.weights[i]);
            assert(fabsf(sample.weights[i] - adjacent.weights[i]) < 0.001f);
        }
        const int height = 50;
        const int material = planned.surfacematerial(x, 32, height);
        assert((material == game::WORLD_BIOME_SNOW) == planned.snowcovered(x, 32, sample.temperature));
    }
    // Terrain only modifies the ecotone, preserving unequivocal desert and non-desert interiors.
    assert(game::blenddesertcoverage(0, 8, 1, 0) == 0);
    assert(game::blenddesertcoverage(1, -8, 0, 1) == 1);
    assert(game::blenddesertcoverage(0.5f, -8, 0, 0) < game::blenddesertcoverage(0.5f, 0, 0, 0));
    assert(game::blenddesertcoverage(0.5f, 8, 0, 0) > game::blenddesertcoverage(0.5f, 0, 0, 0));
    assert(game::blenddesertcoverage(0.5f, 0, 0, 1) < game::blenddesertcoverage(0.5f, 0, 0, 0));
    int sandpatches = 0, edgechanges = 0;
    bool differentseed = false;
    for(int y = -128; y < 128; ++y) for(int x = -128; x < 128; ++x)
    {
        const float threshold = planned.sandthreshold(x, y);
        assert(threshold == reverse.sandthreshold(x, y));
        assert(threshold >= 0.05f && threshold <= 0.95f);
        assert(fabsf(threshold - planned.sandthreshold(x + 0.001f, y)) < 0.001f);
        differentseed |= threshold != other.sandthreshold(x, y);
        sandpatches += threshold < 0.5f;
        edgechanges += (threshold < 0.5f) != (planned.sandthreshold(x + 1, y) < 0.5f);
    }
    assert(differentseed && sandpatches > 4096 && sandpatches < 61440);
    // Connected patches, not independent per-block speckling.
    assert(edgechanges > 0 && edgechanges < 4096);
    printf("PASS: coherent biome intrusions, seed/edge continuity, river and relief influence (%d edges)\n", edgechanges);
    int coverage[5] = {};
    const float temperatures[] = {-3, 0, 2, 3.5f, 5};
    for(int y = -128; y < 128; ++y) for(int x = -128; x < 128; ++x)
    {
        bool previous = true;
        for(int i = 0; i < 5; ++i)
        {
            const bool snow = planned.snowcovered(x, y, temperatures[i]);
            assert(!snow || previous);
            assert(snow == reverse.snowcovered(x, y, temperatures[i]));
            previous = snow;
            coverage[i] += snow;
        }
        // Warm drifts cannot join across cells and cannot become wide sheets of snow.
        if(planned.snowcovered(x, y, 2))
        {
            assert(!planned.snowcovered(x + 6, y, 2) || int(floorf(x / 12.0f)) != int(floorf((x + 6) / 12.0f)));
        }
    }
    assert(coverage[0] == 256 * 256 && coverage[4] == 0);
    assert(coverage[1] > coverage[2] * 3 && coverage[1] < coverage[0]);
    assert(coverage[2] > coverage[3] && coverage[3] > 0 && coverage[2] < coverage[0] / 10);
    printf("PASS: snow coverage at -3/0/2/3.5/5 C: %d/%d/%d/%d/%d of 65536 columns\n",
           coverage[0], coverage[1], coverage[2], coverage[3], coverage[4]);
    printf("PASS: climate centers, normalized weights, continuous blending, shared climate and geography override\n");
    printf("PASS: coast/river/altitude boosts, smooth boundaries, density bounds; %d/40000 open samples\n", clear);
    printf("PASS: max adjacent-metre delta: %.6f C, %.6f %%\n", maxdt, maxdh);
}
