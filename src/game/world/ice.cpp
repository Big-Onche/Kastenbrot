#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

namespace game
{
    float worldgenerator::icecoastdistance(int x, int y) const
    {
        // Cache a coarse shore-distance field instead of searching thousands of neighbours for every ice block.
        const int grid = 16, gx = int(floorf(float(x) / grid)), gy = int(floorf(float(y) / grid));
        float distances[4];
        loopi(4)
        {
            const ivec key(gx + (i & 1), gy + (i >> 1), 0);
            float *cached = icecoastcache.access(key);
            if(cached)
            {
                distances[i] = *cached;
                continue;
            }
            const int sx = key.x * grid, sy = key.y * grid;
            const worldwatersample center = surface(sx, sy);
            float distance = 1024.0f;
            if(center.height >= settings.sealevel && center.height >= center.water)
                distance = 0.0f;
            else
                for(int radius = 8; radius <= 512 && distance > radius; radius += 8)
                {
                    loopj(16)
                    {
                        const float angle = j * (2.0f * M_PI / 16.0f);
                        const worldwatersample shore = surface(sx + int(roundf(cosf(angle) * radius)), sy + int(roundf(sinf(angle) * radius)));
                        if(shore.height >= settings.sealevel && shore.height >= shore.water)
                        {
                            distance = float(radius);
                            break;
                        }
                    }
                }
            icecoastcache[key] = distances[i] = distance;
        }
        const float fx = float(x - gx * grid) / grid, fy = float(y - gy * grid) / grid;
        return (distances[0] * (1.0f - fx) + distances[1] * fx) * (1.0f - fy) + (distances[2] * (1.0f - fx) + distances[3] * fx) * fy;
    }

    static float icecoastwidth(const worldgenerator &generator, int x, int y, float temperature)
    {
        // Start with one or two shore blocks, then expand smoothly to the 192-block cap (three times the original extent).
        const float growth = 1.0f - smoothstep(-22.0f, -5.0f, temperature),
                    broad = generator.snowpatches.GetNoise(float(x) * 0.20f, float(y) * 0.20f), variation = clamp(0.90f + 0.10f * broad, 0.80f, 1.0f);
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
        seaicecell cell = {0, 0, 0, 0, 0};
        for(int cy = gy - 1; cy <= gy + 1; ++cy)
            for(int cx = gx - 1; cx <= gx + 1; ++cx)
            {
                const float px = (cx + 0.20f + 0.60f * worldspatialunit(seed, cx, cy, salt)) * size,
                            py = (cy + 0.20f + 0.60f * worldspatialunit(seed, cx, cy, salt ^ 0x8913U)) * size,
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
        const float temperature = environmentclimate.gettemperature(position), cold = 1.0f - smoothstep(-22.0f, 3.0f, temperature);
        if(cold > 0.0f && !surface(x, y).freshwater)
        {
            const float shore = icecoastdistance(x, y), width = icecoastwidth(*this, x, y, temperature),
                        fringe = temperature < -5.0f ? 1.0f - smoothstep(width, width + 48.0f, shore) : 0.0f,
                        reach = 1.0f - smoothstep(width + 80.0f, width + 250.0f, shore),
                        wx = float(x) + 12.0f * coldmicro.GetNoise(float(x), float(y)),
                        wy = float(y) + 12.0f * coldmicro.GetNoise(float(x) + 713.0f, float(y) - 419.0f);
            const seaicecell parent = sampleseaicecell(uint(seed), wx, wy, 48, 0x5041434BU);
            const float division = worldspatialunit(uint(seed), parent.x, parent.y, 0x5041434CU);
            const int size = division < 0.22f * cold ? 48 : division < 0.60f + 0.20f * cold ? 18 : 8;
            const seaicecell plate = size == 48 ? parent : sampleseaicecell(uint(seed), wx, wy, size, 0x5041434BU ^ uint(size));
            const float cluster = coldroll.GetNoise(plate.centerx * 1.5f + 1731.0f, plate.centery * 1.5f - 2917.0f),
                        chance = max(fringe, clamp(cold * (0.80f + 0.35f * cluster) * reach, 0.0f, 0.96f)),
                        erosion = 0.45f + 1.5f * (1.0f - cold) + 0.35f * snowpatches.GetNoise(float(x) * 1.3f + 5713.0f, float(y) * 1.3f - 2137.0f),
                        selected = worldspatialunit(uint(seed), plate.x, plate.y, 0x504C4154U ^ uint(size));
            bool cracked = min(parent.edge, plate.edge) <= erosion;
            // Some large sheets retain a short internal fracture; its broad gate leaves the plate connected elsewhere.
            if(size == 48 && division < 0.10f)
            {
                const seaicecell fracture = sampleseaicecell(uint(seed), wx, wy, 18, 0x43524143U);
                cracked = cracked || (fracture.edge < 0.45f && coldmicro.GetNoise(float(x) * 2.0f + 3171.0f, float(y) * 2.0f - 951.0f) > 0.10f);
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

        const int spacing = ocean ? 56 : 112, cellx = int(floorf(float(x) / spacing)), celly = int(floorf(float(y) / spacing));
        const uint salt = ocean ? 0x71C8B249U : 0x3E9D5A17U;
        const float occurrence = worldspatialunit(uint(seed), cellx, celly, salt);

        if(occurrence > (ocean ? 0.82f : 0.52f)) return false;

        // centres stay inside their cells, with enough margin for the whole footprint
        const int margin = ocean ? 14 : 20,
                  cx = cellx * spacing + margin + int(worldspatialunit(uint(seed), cellx, celly, salt ^ 0x2917U) * (spacing - 2 * margin)),
                  cy = celly * spacing + margin + int(worldspatialunit(uint(seed), cellx, celly, salt ^ 0x8913U) * (spacing - 2 * margin));

        const float shape = worldspatialunit(uint(seed), cellx, celly, salt ^ 0xD71FU), radius = ocean ? 8.0f + 5.0f * shape : 5.0f + 4.0f * shape,
                    angle = worldspatialunit(uint(seed), cellx, celly, salt ^ 0xE3A9U) * 2.0f * M_PI, dx = float(x - cx), dy = float(y - cy),
                    u = (dx * cosf(angle) + dy * sinf(angle)) / radius, v = (-dx * sinf(angle) + dy * cosf(angle)) / (radius * 0.72f),
                    edge = max(fabsf(u), max(fabsf(v), fabsf(u + v) * 0.65f));

        if(edge >= 1.75f) return false;

        const worldwatersample center = surface(cx, cy);
        const int base = ocean ? settings.sealevel : center.height;
        const vec position(float(cx) * worldclimate::BLOCK_UNITS, float(cy) * worldclimate::BLOCK_UNITS,
                           worldclimate::GROUND_UNITS + base * worldclimate::BLOCK_UNITS);
        const BiomeSample climate = sampleBiome(position);
        float bergscale = 1.0f;
        int coastmaxrise = 255;

        if(ocean)
        {
            if(center.freshwater || center.height >= settings.sealevel || climate.temperature >= 3.0f) return false;

            const float shore = icecoastdistance(cx, cy), width = icecoastwidth(*this, cx, cy, climate.temperature), offshore = shore - width,
                        outer = smoothstep(35.0f, 210.0f, offshore), // beyond the frozen shelf
                coldcoast = 1.0f - smoothstep(-18.0f, -3.0f, climate.temperature),
                        beach = 1.0f - smoothstep(24.0f, 120.0f, shore), // strong boost in the first ~100 blocks of ocean from the beach
                shoregate = smoothstep(2.0f, 10.0f, shore),              // avoid centers literally touching the shoreline
                nearshore = 0.42f + 0.38f * coldcoast + 0.18f * beach,   // high density along frozen coast, progressively lower offshore

                chance = clamp(nearshore * shoregate * (1.0f - 0.90f * outer) * (1.0f - smoothstep(210.0f, 250.0f, offshore)), 0.0f, 0.95f);

            if(occurrence >= chance) return false;

            bergscale = 1.0f - 0.50f * outer; // offshore bergs survive beyond the shelf and do not collapse with its narrow onset width
            coastmaxrise = int(floorf(
                4.0f + 44.0f * smoothstep(24.0f, 220.0f, shore))); // keep coastal bergs low. Height progressively returns to normal farther offshore

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
                        offset = 0.80f + 0.25f * worldspatialunit(uint(seed), cellx, celly, salt ^ uint(0x5413 + i)),
                        su = (u / bergscale - cosf(phase) * offset) / 0.55f, sv = (v / bergscale - sinf(phase) * offset) / 0.55f,
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

} // namespace game

bool placeworldice(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    const int ice = ctx.cubetype("ice");
    for(int y = 0; y < WORLD_CHUNK_BLOCKS; ++y)
    {
        if(ctx.iscanceled()) return false;
        for(int x = 0; x < WORLD_CHUNK_BLOCKS; ++x)
        {
            const int height = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE;
            int bottom, top;
            if(!ctx.generator.icecolumn(chunkx * WORLD_CHUNK_BLOCKS + x, chunky * WORLD_CHUNK_BLOCKS + y, height, bottom, top)) continue;
            for(int z = max(bottom, int(WORLD_MIN_HEIGHT)); z < top; ++z)
            {
                const ivec position(x * WORLD_BLOCK_SIZE, y * WORLD_BLOCK_SIZE, WORLD_GROUND_HEIGHT + z * WORLD_BLOCK_SIZE);
                cube &c = lookupworldgenblock(ctx, root, position);
                if(setworldcubetype(c, ctx, ice))
                {
                    uchar &flags = worldgensectionflags(ctx, x, y, position.z / WORLD_BLOCK_SIZE);
                    flags = (flags | SECTION_EXTERIOR) & ~(SECTION_FULLY_SOLID | SECTION_NO_RENDER);
                }
            }
        }
    }
    return !ctx.iscanceled();
}
