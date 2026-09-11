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
    game::worldsettings settings;
    settings.sealevel = 23;
    game::worldgenerator generator(1337, settings);
    const game::worldclimate &climate = generator.environmentclimate;
    float maxdt = 0, maxdh = 0;
    const int coordinates[][2] = {{0, 0}, {-64, 64}, {10000, -20000}, {-40000, 50000}, {80000, 90000}};
    for(const auto &xy : coordinates)
    {
        const vec sea(xy[0] * 16.0f, xy[1] * 16.0f, 4096.0f + settings.sealevel * 16.0f);
        const vec snow(sea.x, sea.y, 4096.0f + settings.snowheight * 16.0f),
                  summit = vec(snow).add(vec(0, 0, 50 * 16)), lowland = vec(sea).sub(vec(0, 0, 800));
        assert(fabsf(climate.getaltitude(sea)) < 0.0001f);
        assert(climate.gettemperature(sea) == climate.getregionaltemperature(sea));
        assert(climate.gettemperature(snow) <= 0.0001f);
        if(climate.gettemperature(sea) > 1) assert(fabsf(climate.gettemperature(snow)) < 0.0001f);
        assert(climate.gettemperature(summit) < climate.gettemperature(snow));
        assert(climate.gettemperature(lowland) > climate.gettemperature(sea));
        const vec below = vec(snow).sub(vec(0, 0, 0.01f)), above = vec(snow).add(vec(0, 0, 0.01f));
        assert(fabsf(climate.gettemperature(below) - climate.gettemperature(above)) < 0.001f);
        printf("(%d, %d) sea %.3f C, snow line %.3f C, summit %.3f C, humidity %.3f %%\n",
               xy[0], xy[1], climate.gettemperature(sea), climate.gettemperature(snow),
               climate.gettemperature(summit), climate.gethumidity(sea));
        for(int snowheight : {80, 160, 240})
        {
            const game::worldclimate adjusted(1337, settings.sealevel, snowheight);
            const vec snowpos(sea.x, sea.y, 4096.0f + snowheight * 16.0f);
            assert(adjusted.gettemperature(snowpos) <= 0.0001f);
            assert(adjusted.gettemperature(vec(snowpos).add(vec(0, 0, 16))) < adjusted.gettemperature(snowpos));
        }
    }
    for(int snowheight : {22, 23})
    {
        const game::worldclimate adjusted(1337, settings.sealevel, snowheight);
        assert(std::isfinite(adjusted.gettemperature(vec(0, 0, 5000))));
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
    assert(changed && maxdt < 0.03f && maxdh < 0.06f);
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
    // Compare the mountain belt with plains over the same horizontal positions and climate regions.
    double plainsdensity = 0, mountaindensity = 0;
    int openplains = 0, openmountains = 0;
    const int mountainheight = settings.sealevel + int(0.70f * (settings.snowheight - settings.sealevel));
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
        assert(fabsf(generator.treedensity(x, 32, settings.snowheight - 1) -
                     generator.treedensity(x, 32, settings.snowheight)) < 0.002f);
    }
    assert(mountaindensity > plainsdensity * 1.5);
    assert(openmountains < openplains && openplains > 0);
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
    printf("PASS: coast/river/altitude boosts, smooth boundaries, density bounds; %d/40000 open samples\n", clear);
    printf("PASS: max adjacent-metre delta: %.6f C, %.6f %%\n", maxdt, maxdh);
}
