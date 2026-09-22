#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

namespace game
{

    bool treewood(int type)
    {
        return type == WORLD_TREE_WOOD || type == WORLD_TREE_DARK_WOOD || type == WORLD_TREE_PALM_WOOD || type == WORLD_TREE_BIRCH_WOOD;
    }

    const char *treeblockname(int type)
    {
        static const char *const names[] = {"air",       "wood",       "dark_wood",   "leaves",      "needles",
                                            "palm_wood", "birch_wood", "palm_leaves", "birch_leaves"};
        return type >= 0 && type < WORLD_TREE_BLOCK_COUNT ? names[type] : "air";
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

    float worldgenerator::treedensity(int x, int y, int height) const
    {
        const vec position(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                           worldclimate::GROUND_UNITS + float(height) * worldclimate::BLOCK_UNITS);
        float weights[TREE_SPECIES_COUNT], density = 0;
        treeweights(x, y, height, sampleBiome(position), weights);
        loopi(TREE_SPECIES_COUNT) density += weights[i];
        return clamp(density, 0.0f, 1.0f);
    }

    int treefinalheight(int species, float temperature, uint shape)
    {
        if(species == TREE_PALM) return 7 + int((shape >> 24) % 4U);
        if(species == TREE_BIRCH) return 5 + int((shape >> 24) % 3U);
        if(species == TREE_POPLAR) return 15 + int((shape >> 24) & 1U); // Including the leafy tip: 11--12 blocks.
        if(species == TREE_PINE || species == TREE_ACACIA)
            return 6 + int((shape >> 24) % 3U);
        else
            return 4 + int((shape >> 24) % 3U); // regular ones
        if(temperature <= -4.0f) return 4 + int((shape >> 24) & 1U);
        if(temperature <= 1.5f) return 5 + int((shape >> 24) % 3U);
        return 6 + int((shape >> 24) & 3U);
    }

    int treeshaperadius(int species)
    {
        switch(species)
        {
        case TREE_POPLAR:
            return 1;
        case TREE_PINE:
            return 3;
        case TREE_ACACIA:
            return 6;
        case TREE_PALM:
            return 5;
        }
        return 2;
    }

    float treepinechance(const worldsettings &settings, const BiomeSample &sample, uint seed, int x, int y, int height)
    {
        const int broadx = x >= 0 ? x / 24 : (x - 23) / 24, broady = y >= 0 ? y / 24 : (y - 23) / 24;

        const float broad = worldspatialunit(seed, broadx, broady, 0xA24BAED4U), local = worldspatialunit(seed, x, y, 0x9FB21C65U),
                    patch = clamp(0.65f * broad + 0.35f * local, 0.0f, 1.0f),

                    pinelow = float(min(settings.pinestartheight, settings.pinefullheight)),
                    pinehigh = float(max(settings.pinestartheight, settings.pinefullheight)), altitude = smoothstep(pinelow, pinehigh, float(height)),

                    cooltemperate = 1.0f - smoothstep(8.0f, 14.0f, sample.temperature), humidity = smoothstep(25.0f, 70.0f, sample.humidity);

        // Species follow the actual climate and elevation, including across biome boundaries.
        const float cold = 1.0f - smoothstep(-4.0f, 8.0f, sample.temperature), warm = smoothstep(16.0f, 30.0f, sample.temperature),
                    chance = (0.10f + 0.55f * cold + 0.20f * cooltemperate + 0.30f * altitude + 0.16f * (patch - 0.5f)) * (1.0f - 0.88f * warm) *
                             (0.85f + 0.15f * humidity);

        // Broadleaf survival fades through cold taiga and reaches zero at -4 degrees
        const float broadleaf = smoothstep(-4.0f, 4.0f, sample.temperature);
        return 1.0f - (1.0f - clamp(chance, 0.02f, 0.96f)) * broadleaf;
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
        return smoothstep(-11.0f, 3.0f, temperature) * (0.65f + 0.35f * smoothstep(15.0f, 70.0f, humidity)) * (1.0f - 0.98f * hotdry);
    }

    static float woodlanddensity(const worldgenerator &generator, int x, int y, int height, const BiomeSample &sample, float freshwater)
    {
        const float suitability = treesuitability(sample.temperature, sample.humidity);
        if(suitability <= 0.0f || generator.settings.basetreedensity <= 0.0f) return 0.0f;

        const float noisex = x + 10000.5f, noisey = y - 10000.5f,
                    broad = generator.vegetationvariation.GetNoise(noisex * 0.28f + 1731.0f, noisey * 0.28f - 2917.0f),
                    local = generator.vegetationvariation.GetNoise(noisex, noisey), patch = clamp(0.5f + 0.80f * broad + 0.25f * local, 0.0f, 1.0f);

        const worldtectonicsample relief = generator.tectonics(x, y);
        const float altitude = float(height - generator.settings.sealevel),
                    foothills = smoothstep(0.10f, 0.48f, relief.terrainroughness) * smoothstep(4.0f, 24.0f, altitude),
                    mountainbelt = max(foothills, smoothstep(28.0f, 65.0f, altitude)),
                    woodland = smoothstep(0.38f - 0.22f * mountainbelt, 0.70f - 0.20f * mountainbelt, patch),
                    scattered = 0.12f + 0.20f * smoothstep(-0.5f, 0.5f, local), densityfactor = scattered + (3.0f + 2.0f * mountainbelt) * woodland,
                    waterbonus = 1.0f + 0.45f * freshwater, density = generator.settings.basetreedensity * suitability * densityfactor * waterbonus;

        // hot and relatively dry.
        const float spirouland = smoothstep(20.0f, 28.0f, sample.temperature) * (1.0f - smoothstep(35.0f, 60.0f, sample.humidity));

        // Keep hot zones mostly open, but allow denser vegetation near freshwater.
        const float spiroulandrelief = clamp(freshwater * 0.65f, 0.0f, 0.65f);
        const float spiroulandtarget = 0.18f + spiroulandrelief;
        const float spiroulandpenalty = 1.0f * (1.0f - spirouland) + spiroulandtarget * spirouland;

        return clamp(density * spiroulandpenalty, 0.0f, 1.0f);
    }

    int getacacia(int height, uint shape, int x, int y, int z)
    {
        // main growth direction
        const int dir = (shape >> 8) & 3U;
        const int dx = dir == 0 ? 1 : dir == 1 ? -1 : 0;
        const int dy = dir == 2 ? 1 : dir == 3 ? -1 : 0;

        // perpendicular direction, randomly mirrored
        const int side = ((shape >> 10) & 1U) ? 1 : -1;
        const int sx = dy * side;
        const int sy = -dx * side;

        // independent deterministic shape variation
        const uint variant = worldtreehash(shape, 0, 0, height, 0, 0xACA71A31U);

        // Radius distribution: 40% -> radius 2 | 30% -> radius 3-4 | 30% -> radius 5-6
        const uint radiusroll = variant % 100U;

        int radius;
        if(radiusroll < 40U)
            radius = 2;
        else if(radiusroll < 70U)
            radius = 3 + int((variant >> 8) & 1U);
        else
            radius = 5 + int((variant >> 9) & 1U);

        // large acacias may have a second, lower canopy
        const bool doublecanopy = radius > 3 && ((variant >> 11) & 1U) != 0;

        // small trees lean one block, very large trees may lean two blocks
        const int lean = radius >= 5 ? 2 : 1;
        const int bendstart = max(2, height / 2);
        const int trunkend = height - 1;
        const int trunkspan = max(1, trunkend - bendstart);
        // final center of the main canopy
        const int tipx = dx * lean;
        const int tipy = dy * lean;
        // Main trunk: Instead of teleporting sideways at bendstart, move progressively toward the canopy center.
        // Keep the previous position too so every step stays face-connected.
        if(z <= trunkend)
        {
            const int currentstep = z <= bendstart ? 0 : min(lean, ((z - bendstart) * lean + trunkspan - 1) / trunkspan);
            const int previousz = max(bendstart, z - 1);
            const int previousstep = previousz <= bendstart ? 0 : min(lean, ((previousz - bendstart) * lean + trunkspan - 1) / trunkspan);

            const int tx = dx * currentstep;
            const int ty = dy * currentstep;

            const int ptx = dx * previousstep;
            const int pty = dy * previousstep;

            if((x == tx && y == ty) || (z >= bendstart && x == ptx && y == pty)) return WORLD_TREE_DARK_WOOD;
        }

        // Wide canopies receive supporting branches:
        // radius 2-3 : main trunk only | radius 4   : 1 support branch | radius 5-6 : 2 support branches
        const int supports = radius >= 5 ? 2 : radius >= 4 ? 1 : 0;

        const int branchstart = max(2, height - 5);
        const int branchend = height - 2;
        const int branchspan = max(1, branchend - branchstart);

        const int rootstep = branchstart <= bendstart ? 0 : min(lean, ((branchstart - bendstart) * lean + trunkspan - 1) / trunkspan);

        const int rootx = dx * rootstep;
        const int rooty = dy * rootstep;

        const int supportreach = max(1, radius / 2);
        // first supporting branch
        const int support1x = tipx + sx * supportreach;
        const int support1y = tipy + sy * supportreach;

        if(supports >= 1 && z >= branchstart && z <= branchend)
        {
            const int step = z - branchstart;
            const int previous = max(0, step - 1);

            const float t = float(step) / float(branchspan);
            const float pt = float(previous) / float(branchspan);

            const int bx = int(roundf(rootx + (support1x - rootx) * t));
            const int by = int(roundf(rooty + (support1y - rooty) * t));

            const int pbx = int(roundf(rootx + (support1x - rootx) * pt));
            const int pby = int(roundf(rooty + (support1y - rooty) * pt));

            // third test bridges diagonal steps horizontally
            if((x == bx && y == by) || (x == pbx && y == pby) || (x == bx && y == pby)) return WORLD_TREE_DARK_WOOD;
        }

        // second support on the opposite side for the largest trees
        const int support2x = tipx - sx * supportreach;
        const int support2y = tipy - sy * supportreach;

        if(supports >= 2 && z >= branchstart && z <= branchend)
        {
            const int step = z - branchstart;
            const int previous = max(0, step - 1);

            const float t = float(step) / float(branchspan);
            const float pt = float(previous) / float(branchspan);

            const int bx = int(roundf(rootx + (support2x - rootx) * t));
            const int by = int(roundf(rooty + (support2y - rooty) * t));

            const int pbx = int(roundf(rootx + (support2x - rootx) * pt));
            const int pby = int(roundf(rooty + (support2y - rooty) * pt));

            if((x == bx && y == by) || (x == pbx && y == pby) || (x == bx && y == pby)) return WORLD_TREE_DARK_WOOD;
        }

        // Secondary canopy: only possible for radius > 3, lower than the main canopy, 2 blocks smaller, hifted sideways
        const int secondaryradius = max(2, radius - 2);
        const int secondaryz = height - 3;

        const int secondaryoffset = 2;

        const int secondaryx = tipx + sx * secondaryoffset;
        const int secondaryy = tipy + sy * secondaryoffset;

        if(doublecanopy && secondaryz > branchstart && z >= branchstart && z <= secondaryz) // give the secondary canopy its own supporting branch
        {
            const int secondaryspan = secondaryz - branchstart;
            const int step = z - branchstart;
            const int previous = max(0, step - 1);

            const float t = float(step) / float(secondaryspan);
            const float pt = float(previous) / float(secondaryspan);

            const int bx = int(roundf(rootx + (secondaryx - rootx) * t));
            const int by = int(roundf(rooty + (secondaryy - rooty) * t));

            const int pbx = int(roundf(rootx + (secondaryx - rootx) * pt));
            const int pby = int(roundf(rooty + (secondaryy - rooty) * pt));

            if((x == bx && y == by) || (x == pbx && y == pby) || (x == bx && y == pby)) return WORLD_TREE_DARK_WOOD;
        }

        // Main canopy: Don't generate a perfect disk
        // Each side gets a slightly different reach and edge cells are randomly removed.
        // This creates lobes / missing corners while preserving a broad flat acacia silhouette.

        const int mx = x - tipx;
        const int my = y - tipy;

        const int negxreach = max(1, radius - int((variant >> 13) & 1U));
        const int posxreach = max(1, radius - int((variant >> 14) & 1U));
        const int negyreach = max(1, radius - int((variant >> 15) & 1U));
        const int posyreach = max(1, radius - int((variant >> 16) & 1U));

        if(z == height - 1) // main broad leaf layer
        {
            const int xreach = mx < 0 ? negxreach : posxreach;
            const int yreach = my < 0 ? negyreach : posyreach;

            const int ax = abs(mx);
            const int ay = abs(my);

            const int diagonalreach = radius + max(1, radius / 2);

            const bool inside = ax <= xreach && ay <= yreach && ax + ay <= diagonalreach;

            if(inside)
            {
                const bool edge = ax >= xreach || ay >= yreach || ax + ay >= diagonalreach - 1;
                const bool supported = (ax <= 1 && ay <= 1) || (supports >= 1 && abs(x - support1x) + abs(y - support1y) <= 1) ||
                                       (supports >= 2 && abs(x - support2x) + abs(y - support2y) <= 1);
                const uint leafhash = worldtreehash(shape, x, y, z, height, 0xC4110F37U);

                if(!edge || supported || leafhash % 100U >= 30U) return WORLD_TREE_LEAVES;
            }
        }

        // Smaller upper layer: Slightly offset relative to the main layer so the
        // crown isn't just concentric circles stacked vertically.
        if(z == height)
        {
            const int topoffset = ((variant >> 17) & 1U) ? 1 : 0;

            const int topx = tipx + sx * topoffset;
            const int topy = tipy + sy * topoffset;

            const int tx = x - topx;
            const int ty = y - topy;

            const int topradius = max(1, radius - 2);

            const int ax = abs(tx);
            const int ay = abs(ty);

            const bool inside = ax <= topradius && ay <= topradius && ax + ay <= topradius + max(1, topradius / 2);

            if(inside)
            {
                const bool edge = ax == topradius || ay == topradius || ax + ay >= topradius + max(1, topradius / 2) - 1;
                const uint leafhash = worldtreehash(shape, x, y, z, height, 0x7EAF311DU);

                if((!edge || leafhash % 100U >= 35U) && !(ax == topradius && ay == topradius)) return WORLD_TREE_LEAVES;
            }
        }

        // Sparse underside: This gives the canopy a little thickness without turning it into a round blob.
        if(z == height - 2)
        {
            const int lowradius = max(1, radius - 3);

            const int ax = abs(mx);
            const int ay = abs(my);

            if(ax <= lowradius && ay <= lowradius && ax + ay <= lowradius + 1)
            {
                const uint leafhash = worldtreehash(shape, x, y, z, height, 0x10A3C91BU);
                if(leafhash % 100U >= 18U) return WORLD_TREE_LEAVES;
            }

            // Small bunches around branch tips make the canopy look physically supported instead of hovering.
            if(supports >= 1 && abs(x - support1x) <= 1 && abs(y - support1y) <= 1) return WORLD_TREE_LEAVES;
            if(supports >= 2 && abs(x - support2x) <= 1 && abs(y - support2y) <= 1) return WORLD_TREE_LEAVES;
        }

        if(doublecanopy) // secondary canopy
        {
            const int bx = x - secondaryx;
            const int by = y - secondaryy;

            if(z == secondaryz) // broad lower secondary crown
            {
                const int ax = abs(bx);
                const int ay = abs(by);

                const int rx = max(1, secondaryradius - int((variant >> 18) & 1U));
                const int ry = max(1, secondaryradius - int((variant >> 19) & 1U));

                const bool inside = ax <= rx && ay <= ry && ax + ay <= secondaryradius + max(1, secondaryradius / 2);

                if(inside)
                {
                    const bool edge = ax >= rx || ay >= ry || ax + ay >= secondaryradius + max(1, secondaryradius / 2) - 1;
                    const uint leafhash = worldtreehash(shape, x, y, z, height, 0x52C0A1A5U);

                    if(!edge || leafhash % 100U >= 35U) return WORLD_TREE_LEAVES;
                }
            }

            // Smaller cap one block above the secondary canopy: still below the main crown because secondaryz = height - 3
            if(z == secondaryz + 1)
            {
                const int r = max(1, secondaryradius - 1);

                const int ax = abs(bx);
                const int ay = abs(by);

                if(ax <= r && ay <= r && ax + ay <= r + 1)
                {
                    const uint leafhash = worldtreehash(shape, x, y, z, height, 0x62B59D47U);
                    const bool edge = ax == r || ay == r || ax + ay >= r + 1;

                    if(!edge || leafhash % 100U >= 40U) return WORLD_TREE_LEAVES;
                }
            }
        }

        return WORLD_TREE_AIR;
    }

    int getpalm(int height, uint shape, int x, int y, int z)
    {
        const int direction = (shape >> 8) & 3U, lean = 1 + int((shape >> 10) & 1U),
                  dx = direction == 0   ? 1
                       : direction == 1 ? -1
                                        : 0,
                  dy = direction == 2   ? 1
                       : direction == 3 ? -1
                                        : 0,
                  bend = max(0, z - height / 3) * lean / max(1, height - 1 - height / 3),
                  previous = max(0, z - 1 - height / 3) * lean / max(1, height - 1 - height / 3);

        // horizontal bridge cubes keep every stepped lean face-connected for foliage support
        if(z < height && ((x == dx * bend && y == dy * bend) || (x == dx * previous && y == dy * previous))) return WORLD_TREE_PALM_WOOD;

        x -= dx * lean;
        y -= dy * lean;

        if(z == height && abs(x) + abs(y) <= 1 + int((shape >> 12) & 1U)) return WORLD_TREE_PALM_LEAVES;

        const int reachx = 2 + int((shape >> 13) & 1U), reachy = 2 + int((shape >> 14) & 1U), distance = max(abs(x), abs(y));

        const bool frond = (y == 0 && abs(x) <= reachx) || (x == 0 && abs(y) <= reachy) || (((shape >> 15) & 1U) && abs(x) <= 2 && abs(y) <= 2);

        if(frond && ((distance <= 2 && z == height - 1) || (distance >= 2 && z == height - 2))) return WORLD_TREE_PALM_LEAVES;

        return WORLD_TREE_AIR;
    }

    int getpoplar(int height, uint shape, int x, int y, int z)
    {
        const int bottom = 2 + int((shape >> 11) & 1U), trunkheight = height - 3 + int((shape >> 12) & 1U),
                  notch = bottom + 2 + int((shape >> 13) & 1U);

        if(!x && !y && z < trunkheight) return WORLD_TREE_WOOD;
        if(z < bottom || abs(x) > 1 || abs(y) > 1) return WORLD_TREE_AIR;

        // full three-block-wide tufts alternate with narrow, connected leafy sections near the tip
        // the trunk stops inside the crown, leaving several foliage-only levels above it
        if(z == height) return !x && !y ? WORLD_TREE_LEAVES : WORLD_TREE_AIR;
        if(z == height - 2 || z == notch)
        {
            const bool shoulder = ((shape >> 14) & 1U) && (((shape >> 15) & 1U) ? !x : !y) && abs(x) + abs(y) == 1;
            return (!x && !y) || shoulder ? WORLD_TREE_LEAVES : WORLD_TREE_AIR;
        }

        // occasionally soften a lower tuft's corners while keeping the upper leafy cap full
        if(z < height - 3 && abs(x) == 1 && abs(y) == 1 && (worldtreehash(shape, x, y, z, height, 0x50F1A2B3U) & 3U) == 0) return WORLD_TREE_AIR;

        return WORLD_TREE_LEAVES;
    }

    int treeshapeblock(int species, int height, uint shape, int x, int y, int z)
    {
        if(z < 0 || z > height) return WORLD_TREE_AIR;
        if(species == TREE_ACACIA) return getacacia(height, shape, x, y, z);
        if(species == TREE_PALM) return getpalm(height, shape, x, y, z);
        if(species == TREE_POPLAR) return getpoplar(height, shape, x, y, z);
        if(!x && !y && z < height)
            return species == TREE_PINE ? WORLD_TREE_DARK_WOOD : species == TREE_BIRCH ? WORLD_TREE_BIRCH_WOOD : WORLD_TREE_WOOD;
        if(species == TREE_PINE)
        {
            if(z == height) return !x && !y ? WORLD_TREE_NEEDLES : WORLD_TREE_AIR;
            const int radius = min(3, 1 + (height - z) / 3);
            return z >= 2 && abs(x) <= radius && abs(y) <= radius && abs(x) + abs(y) <= radius + 1 ? WORLD_TREE_NEEDLES : WORLD_TREE_AIR;
        }

        const int bottom = height - 2 - (species == TREE_BIRCH ? int((shape >> 11) & 1U) : 0), radius = z == height ? 1 : 2;

        if(z < bottom || abs(x) > radius || abs(y) > radius) return WORLD_TREE_AIR;

        if(radius == 2 && abs(x) == 2 && abs(y) == 2 && (worldtreehash(shape, x, y, z, height, 0xA511E9B3U) & 1U)) return WORLD_TREE_AIR;

        return species == TREE_BIRCH ? WORLD_TREE_BIRCH_LEAVES : WORLD_TREE_LEAVES;
    }
    bool worldgenerator::treeweights(int x, int y, int height, const BiomeSample &sample, float (&weights)[TREE_SPECIES_COUNT], int material,
                                     float spawn) const
    {
        loopi(TREE_SPECIES_COUNT) weights[i] = 0;
        if(settings.basetreedensity <= 0) return false;
        const float freshwater = freshwatermoisture(float(x), float(y), float(height)), hot = smoothstep(18.0f, 30.0f, sample.temperature),
                    palmmoisture = smoothstep(25.0f, 50.0f, sample.humidity), dry = 1.0f - smoothstep(30.0f, 60.0f, sample.humidity),
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
                const float noisex = x + 10000.5f, noisey = y - 10000.5f, continental = samplecontinental(*this, noisex, noisey),
                            distance = samplecoastdistance(*this, noisex, noisey, continental),
                            configuredwidth = max(settings.coastwidth + biomeblend.GetNoise(noisex, noisey) * settings.coastvariation, 0.0f),
                            width = max(configuredwidth, coasttransitionwidth(x, y));
                coastal = 1.0f - smoothstep(width * 0.5f, max(width, 1.0f), distance);
            }
            // Local humidity includes freshwater influence; even oases must clear the survival threshold.
            palm = hot * palmmoisture * (0.004f + 0.22f * coastal * grove + 0.012f * dry * grove + (0.20f + 0.60f * dry) * freshwater * freshwater);
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

        const float savannabiome = smoothstep(0.20f, 0.55f, sample.weights[WORLD_BIOME_SAVANNA]),
                    acaciaheat = smoothstep(27.0f, 29.0f, sample.temperature), savanna = savannabiome * acaciaheat,
                    pine = treepinechance(settings, sample, uint(seed), x, y, height) * (1.0f - savanna),
                    open = 1.0f - smoothstep(0.7f, 2.4f, density / settings.basetreedensity),
                    poplarhabitat = temperate * humid * open * (1.0f - savanna), left = poplarhabitat > 0 ? this->height(x - 4, y) : height,
                    right = poplarhabitat > 0 ? this->height(x + 4, y) : height, down = poplarhabitat > 0 ? this->height(x, y - 4) : height,
                    up = poplarhabitat > 0 ? this->height(x, y + 4) : height,
                    basin = clamp((left + right + down + up - 4.0f * height) / 12.0f, 0.0f, 1.0f),
                    flat = 1.0f - smoothstep(2.0f, 10.0f, fabsf(right - left) + fabsf(up - down)),
                    exposure = clamp(0.5f + ((right - left) * 0.9701425f + (up - down) * 0.2425356f) / 16.0f, 0.0f, 1.0f),
                    wind = clamp(exposure * 0.6f + (coldregions.GetNoise(float(x), float(y)) * 0.5f + 0.5f) * 0.4f, 0.0f, 1.0f),
                    birch = (0.04f + 0.24f * humid) * temperate,
                    poplar = min(0.40f, temperate * humid * open * (0.12f + 0.28f * wind) * (0.35f + 0.65f * max(flat, max(basin, freshwater))));

        const float normal = density * (1.0f - savanna);
        weights[TREE_ACACIA] = density * savanna;
        weights[TREE_PINE] = normal * pine;
        weights[TREE_BIRCH] = normal * (1.0f - pine) * birch;
        weights[TREE_POPLAR] = normal * (1.0f - pine) * poplar;
        weights[TREE_REGULAR] = normal * (1.0f - pine) * max(0.0f, 1.0f - birch - poplar);

        return true;
    }

    static bool sampleworldtreecandidate(const worldgenerator &generator, int x, int y, queriedworldtree &tree)
    {
        const int chunkx = x >= 0 ? x / 64 : (x - 63) / 64, chunky = y >= 0 ? y / 64 : (y - 63) / 64, blockx = x - chunkx * 64,
                  blocky = y - chunky * 64;

        const uint spawn = worldtreehash(uint(generator.seed), chunkx, chunky, blockx, blocky, 0xD1B54A35U);
        // Bound woodland (5.32 * 1.45) plus palms before any height/hydrology query.
        // This only skips impossible candidates; it does not change spatial decisions.
        if(generator.settings.basetreedensity <= 0 || worldtreeunit(spawn) >= min(1.0f, generator.settings.basetreedensity * 7.715f) + 0.045f)
            return false;

        const int height = generator.height(x, y);

        if(height < generator.surface(x, y).water) return false;

        const vec position(float(x) * worldclimate::BLOCK_UNITS, float(y) * worldclimate::BLOCK_UNITS,
                           worldclimate::GROUND_UNITS + float(height) * worldclimate::BLOCK_UNITS);

        const BiomeSample sample = generator.sampleBiome(position);
        const int material = generator.surfacematerial(x, y, height, &sample);
        if(material != WORLD_BIOME_PLAINS && material != WORLD_MOSS && material != WORLD_SNOWY_GRASS && material != WORLD_FROZEN_DIRT &&
           material != WORLD_FROZEN_MOSS && material != WORLD_BIOME_SNOW && material != WORLD_BIOME_DESERT)
            return false;

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
            if(selection < 0)
            {
                tree.species = i;
                break;
            }
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
        for(int oy = -3; oy <= 3; ++oy)
            for(int ox = -3; ox <= 3; ++ox)
            {
                if(!ox && !oy) continue;
                // A neighbour that loses priority cannot suppress this tree, regardless of its species or habitat.
                const uint priority = worldtreehash(uint(generator.seed), 0, 0, x + ox, y + oy, 0xD1B54A35U);
                if(priority > tree.priority || (priority == tree.priority && (oy > 0 || (!oy && ox > 0)))) continue;
                queriedworldtree other;
                if(!queryworldtreecandidate(generator, x + ox, y + oy, other)) continue;
                const int spacing = tree.species == TREE_PALM || other.species == TREE_PALM ? 3 : 1;
                if(abs(ox) > spacing || abs(oy) > spacing) continue;
                if(other.priority < tree.priority ||
                   (other.priority == tree.priority && (other.y < tree.y || (other.y == tree.y && other.x < tree.x))))
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
        for(int ty = y - TREE_RADIUS; ty <= y + TREE_RADIUS; ++ty)
            for(int tx = x - TREE_RADIUS; tx <= x + TREE_RADIUS; ++tx)
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
        const bool edge = treecanopyheight(x - 1, y) <= ground || treecanopyheight(x + 1, y) <= ground || treecanopyheight(x, y - 1) <= ground ||
                          treecanopyheight(x, y + 1) <= ground;
        // Absolute coordinates keep the same 33% edge decisions across chunks and LOD tiers.
        if(edge && worldspatialunit(uint(seed), x, y, 0x534E4F57U) < 0.33f) return material;
        return WORLD_BIOME_PLAINS;
    }

    int worldgenerator::treeblock(int x, int y, int z) const
    {
        if(treeblockcache.numelems >= 1 << 18) treeblockcache.clear();
        const ivec key(x, y, z);
        int *cached = treeblockcache.access(key);
        if(cached) return *cached;
        int foliage = WORLD_TREE_AIR;
        for(int treeY = y - TREE_RADIUS; treeY <= y + TREE_RADIUS; ++treeY)
            for(int treeX = x - TREE_RADIUS; treeX <= x + TREE_RADIUS; ++treeX)
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

} // namespace game

static void addworldtreeblock(vector<ivec> &blocks, int blockx, int blocky, int blockz)
{
    if(blockx < 0 || blockx >= WORLD_CHUNK_BLOCKS || blocky < 0 || blocky >= WORLD_CHUNK_BLOCKS || blockz < 0 || blockz >= WORLD_HEIGHT_BLOCKS)
        return;

    blocks.add(ivec(blockx * WORLD_BLOCK_SIZE, blocky * WORLD_BLOCK_SIZE, blockz * WORLD_BLOCK_SIZE));
}

static void markworldgentreeblock(worldgencontext &ctx, const ivec &position)
{
    uchar &flags = worldgensectionflags(ctx, position.x / WORLD_BLOCK_SIZE, position.y / WORLD_BLOCK_SIZE, position.z / WORLD_BLOCK_SIZE);
    flags = (flags | SECTION_EXTERIOR) & ~(SECTION_FULLY_SOLID | SECTION_NO_RENDER);
}

bool placeworldtrees(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    vector<ivec> blocks[game::WORLD_TREE_BLOCK_COUNT];
    const int halo = game::TREE_RADIUS;
    for(int y = -halo; y < WORLD_CHUNK_BLOCKS + halo; ++y)
        for(int x = -halo; x < WORLD_CHUNK_BLOCKS + halo; ++x)
        {
            if(ctx.iscanceled()) return false;
            int base, height, species;
            uint shape;
            if(!ctx.generator.tree(chunkx * WORLD_CHUNK_BLOCKS + x, chunky * WORLD_CHUNK_BLOCKS + y, base, height, shape, species)) continue;
            const int radius = game::treeshaperadius(species);
            for(int z = 0; z <= height; ++z)
                for(int oy = -radius; oy <= radius; ++oy)
                    for(int ox = -radius; ox <= radius; ++ox)
                    {
                        const int type = game::treeshapeblock(species, height, shape, ox, oy, z);
                        if(type != game::WORLD_TREE_AIR) addworldtreeblock(blocks[type], x + ox, y + oy, base + z);
                    }
        }
    {
        int types[game::WORLD_TREE_BLOCK_COUNT], textures[game::WORLD_TREE_BLOCK_COUNT];
        for(int k = 1; k < game::WORLD_TREE_BLOCK_COUNT; ++k)
        {
            types[k] = ctx.cubetype(game::treeblockname(k));
            textures[k] = ctx.cubetextures.inrange(types[k]) ? (ctx.indexedtextures ? types[k] : ctx.cubetextures[types[k]].top) : -1;
        }
        // Foliage first, then trunks. Both LOD tiers use the same material precedence.
        loop(pass, 2)
            for(int k = 1; k < game::WORLD_TREE_BLOCK_COUNT; ++k)
            {
                const bool wood = game::treewood(k);
                if(wood != (pass != 0)) continue;
                loopv(blocks[k])
                {
                    const ivec &p = blocks[k][i];
                    cube &c = lookupworldgenblock(ctx, root, p);
                    bool replace = isempty(c) && c.material == MAT_AIR;
                    if(wood)
                        for(int leaf = 1; leaf < game::WORLD_TREE_BLOCK_COUNT; ++leaf)
                            if(!game::treewood(leaf) && c.texture[0] == textures[leaf]) replace = true;
                    if(replace && setworldcubetype(c, ctx, types[k], !wood && leavesalpha ? MAT_ALPHA : MAT_AIR)) markworldgentreeblock(ctx, p);
                }
            }
        int canopy[WORLD_CHUNK_BLOCKS][WORLD_CHUNK_BLOCKS];
        loop(y, WORLD_CHUNK_BLOCKS)
            loop(x, WORLD_CHUNK_BLOCKS) canopy[y][x] = -1;
        for(int k = 1; k < game::WORLD_TREE_BLOCK_COUNT; ++k)
            loopv(blocks[k])
            {
                const ivec &p = blocks[k][i];
                int &top = canopy[p.y / WORLD_BLOCK_SIZE][p.x / WORLD_BLOCK_SIZE];
                top = max(top, p.z);
            }
        for(int k = 1; k < game::WORLD_TREE_BLOCK_COUNT; ++k)
        {
            if(game::treewood(k) || k == game::WORLD_TREE_PALM_LEAVES) continue;
            loopv(blocks[k])
            {
                const ivec &p = blocks[k][i];
                if(p.z != canopy[p.y / WORLD_BLOCK_SIZE][p.x / WORLD_BLOCK_SIZE]) continue;
                cube &c = lookupworldgenblock(ctx, root, p);
                if(c.texture[0] != textures[k]) continue;
                const int x = chunkx * WORLD_CHUNK_BLOCKS + p.x / WORLD_BLOCK_SIZE, y = chunky * WORLD_CHUNK_BLOCKS + p.y / WORLD_BLOCK_SIZE,
                          ground = ctx.generator.height(x, y), material = ctx.generator.surfacematerial(x, y, ground);
                if(material == game::WORLD_SNOWY_GRASS || material == game::WORLD_BIOME_SNOW)
                    setworldcubetype(c, ctx,
                                     ctx.cubetype(k == game::WORLD_TREE_NEEDLES        ? "snowy_needles"
                                                  : k == game::WORLD_TREE_BIRCH_LEAVES ? "snowy_birch_leaves"
                                                                                       : "snowy_leaves"),
                                     leavesalpha ? MAT_ALPHA : MAT_AIR);
            }
        }
        const int snowcube = ctx.cubetype("snow"), snowygrasscube = ctx.cubetype("snowy_grass"), grasscube = ctx.cubetype("grass");
        loop(y, WORLD_CHUNK_BLOCKS)
            loop(x, WORLD_CHUNK_BLOCKS)
            {
                if(canopy[y][x] < 0) continue;
                const int wx = chunkx * WORLD_CHUNK_BLOCKS + x, wy = chunky * WORLD_CHUNK_BLOCKS + y,
                          height = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE,
                          material = ctx.generator.surfacematerial(wx, wy, height);
                if(ctx.generator.treegroundmaterial(wx, wy, height, material) != game::WORLD_BIOME_PLAINS) continue;
                const ivec p(x * WORLD_BLOCK_SIZE, y * WORLD_BLOCK_SIZE, WORLD_GROUND_HEIGHT + (height - 1) * WORLD_BLOCK_SIZE);
                cube &c = lookupworldgenblock(ctx, root, p);
                const int snowtexture = ctx.indexedtextures ? snowcube : ctx.cubetextures[snowcube].top,
                          grasstexture = ctx.indexedtextures ? snowygrasscube : ctx.cubetextures[snowygrasscube].top;
                if(c.texture[O_TOP] == snowtexture || c.texture[O_TOP] == grasstexture) setworldcubetype(c, ctx, grasscube);
            }
    }
    return !ctx.iscanceled();
}
