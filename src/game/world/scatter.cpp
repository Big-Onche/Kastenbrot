#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

static uint hashworldgrass(uint seed, uint worldx, uint worldy, uint salt)
{
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

void cacheworldscattertransform(int chunkx, int chunky, float maxoffset, const worldscatterinstance &scatter)
{
    if(scatter.rendertransformvalid && scatter.rendermaxoffset == maxoffset) return;

    const uint worldx = uint(chunkx * WORLD_CHUNK_BLOCKS + scatter.x / WORLD_BLOCK_SIZE),
               worldy = uint(chunky * WORLD_CHUNK_BLOCKS + scatter.y / WORLD_BLOCK_SIZE), seed = uint(game::getworldseed());

    scatter.renderyaw = int(worldtreeunit(hashworldgrass(seed, worldx, worldy, 0x63D83595U)) * 360.0f);

    const float angle = worldtreeunit(hashworldgrass(seed, worldx, worldy, 0xC2B2AE35U)) * 2.0f * M_PI,
                offsetunit = worldtreeunit(hashworldgrass(seed, worldx, worldy, 0x27D4EB2FU)),
                offset = maxoffset * WORLD_BLOCK_SIZE * offsetunit * offsetunit;

    scatter.renderoffsetx = cosf(angle) * offset;
    scatter.renderoffsety = sinf(angle) * offset;
    scatter.rendermaxoffset = maxoffset;
    scatter.rendertransformvalid = true;
}

void cacheworldscattertransforms(int chunkx, int chunky, float maxoffset, const vector<worldscatterinstance> &scatter)
{
    loopv(scatter) cacheworldscattertransform(chunkx, chunky, maxoffset, scatter[i]);
}

struct worldgrasscollectcontext
{
    FastNoiseLite distribution, flowerdistribution[3];
    game::worldgenerator generator;
    game::worldsettings settings;
    vector<worldscatterinstance> &scatter;
    int chunkx, chunky;
    uint seed;

    worldgrasscollectcontext(int chunkx, int chunky, const game::worldsettings &settings, vector<worldscatterinstance> &scatter)
        : generator(game::getworldseed(), settings), settings(settings), scatter(scatter), chunkx(chunkx), chunky(chunky),
          seed(uint(game::getworldseed()))
    {
        distribution.SetSeed(game::getworldseed() ^ 0x6E624EB7);
        distribution.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
        distribution.SetFrequency(settings.grassfrequency);
        distribution.SetFractalType(FastNoiseLite::FractalType_FBm);
        distribution.SetFractalOctaves(2);
        distribution.SetFractalLacunarity(1.8f);
        distribution.SetFractalGain(0.5f);

        static const uint flowersalts[3] = {0x9E21F4A7U, 0xC13FA9A9U, 0x91E10DA5U};
        loopi(3)
        {
            flowerdistribution[i].SetSeed(game::getworldseed() ^ flowersalts[i]);
            flowerdistribution[i].SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2S);
            flowerdistribution[i].SetFrequency(settings.grassfrequency * 0.35f);
            flowerdistribution[i].SetFractalType(FastNoiseLite::FractalType_FBm);
            flowerdistribution[i].SetFractalOctaves(2);
            flowerdistribution[i].SetFractalLacunarity(1.8f);
            flowerdistribution[i].SetFractalGain(0.5f);
        }
    }
};

const cube &lookupgeneratedworldcube(const cube *root, const ivec &pos)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    const cube *c = &root[octastep(pos.x, pos.y, pos.z, scale)];
    while(c->children)
    {
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    return *c;
}

bool validgeneratedworldscatter(const cube *root, const worldscatterinstance &scatter)
{
    if(!root || scatter.x < 0 || scatter.x >= WORLD_CHUNK_SIZE || scatter.y < 0 || scatter.y >= WORLD_CHUNK_SIZE || scatter.z < 0 ||
       scatter.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE || scatter.type < 0 || scatter.type >= numworldscatters() || scatter.orient < O_LEFT ||
       scatter.orient > O_TOP)
        return false;

    const ivec center(scatter.x + WORLD_BLOCK_SIZE / 2, scatter.y + WORLD_BLOCK_SIZE / 2, scatter.z + WORLD_BLOCK_SIZE / 2);
    const cube &occupied = lookupgeneratedworldcube(root, center);
    if(!isempty(occupied) || occupied.material != MAT_AIR) return false;

    const bool placeable = isworldplaceable(scatter.type);

    if((!placeable && scatter.orient != O_TOP) || (placeable && scatter.orient == O_BOTTOM)) return false;

    const ivec supportcenter = ivec(center).sub(ivec(worldgenorientnormal(scatter.orient)).mul(WORLD_BLOCK_SIZE));

    // An edge-mounted torch can be owned by the neighboring chunk. Its support
    // is checked once both chunks are mounted in the runtime world.
    if(supportcenter.x < 0 || supportcenter.x >= WORLD_CHUNK_SIZE || supportcenter.y < 0 || supportcenter.y >= WORLD_CHUNK_SIZE) return placeable;

    const cube &support = lookupgeneratedworldcube(root, supportcenter);
    if(isempty(support) || !isentirelysolid(support) || support.material != MAT_AIR) return false;

    return true;
}

static bool worldflowerspaced(const worldgrasscollectcontext &ctx, uint worldx, uint worldy, int flower)
{
    static const uint spacingsalts[3] = {0xD1B54A35U, 0x94D049BBU, 0x369DEA0FU};
    const uint priority = hashworldgrass(ctx.seed, worldx, worldy, spacingsalts[flower]);
    for(int oy = -1; oy <= 1; ++oy)
        for(int ox = -1; ox <= 1; ++ox)
        {
            if(!ox && !oy) continue;
            const uint other = hashworldgrass(ctx.seed, worldx + ox, worldy + oy, spacingsalts[flower]);
            if(other < priority || (other == priority && (oy < 0 || (!oy && ox < 0)))) return false;
        }
    return true;
}

static int chooseworldflower(worldgrasscollectcontext &ctx, float noisex, float noisey, uint worldx, uint worldy)
{
    const float weights[3] = {worldrosescatter >= 0 ? max(ctx.settings.roseweight, 0.0f) : 0.0f,
                              worldtulipscatter >= 0 ? max(ctx.settings.tulipweight, 0.0f) : 0.0f,
                              worlddandelionscatter >= 0 ? max(ctx.settings.dandelionweight, 0.0f) : 0.0f};
    const int types[3] = {worldrosescatter, worldtulipscatter, worlddandelionscatter};
    const float weightsum = weights[0] + weights[1] + weights[2];
    if(ctx.settings.flowerchance <= 0 || weightsum <= 0) return -1;

    static const uint chancesalts[3] = {0xDB4F0B91U, 0xBBE05633U, 0xA0F2EC75U};
    static const uint choicesalts[3] = {0x89E18285U, 0xC6BC2796U, 0xCA01F9DDU};
    int selected = -1;
    float selectedscore = -1;
    loopi(3)
    {
        if(weights[i] <= 0) continue;

        const float noise = clamp(ctx.flowerdistribution[i].GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                    patch = worldsmoothstep(0.48f, 0.72f, noise),
                    chance = clamp(ctx.settings.flowerchance * (weights[i] / weightsum) * (0.05f + 4.95f * patch * patch), 0.0f, 1.0f);

        if(worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy, chancesalts[i])) >= chance || !worldflowerspaced(ctx, worldx, worldy, i)) continue;

        const float score = patch + worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy, choicesalts[i])) * 0.05f;

        if(score > selectedscore)
        {
            selected = types[i];
            selectedscore = score;
        }
    }
    return selected;
}

static void collectworldgrassnode(worldgrasscollectcontext &ctx, const cube &c, const cube *root, const ivec &o, int size, int surfacetexture)
{
    if(o.z >= WORLD_MAP_SIZE || o.x >= WORLD_CHUNK_SIZE || o.y >= WORLD_CHUNK_SIZE) return;

    if(c.children)
    {
        const int childsize = size >> 1;
        loopi(8) collectworldgrassnode(ctx, c.children[i], root, ivec(i, o, childsize), childsize, surfacetexture);
        return;
    }

    if(size < WORLD_BLOCK_SIZE || isempty(c) || !isentirelysolid(c) || c.material != MAT_AIR) return;
    const worlddefinition *frozen = findworldcube("snowy_grass"), *moss = findworldcube("frozen_moss"), *thawedmoss = findworldcube("moss"),
                          *dirt = findworldcube("frozen_dirt"), *gravel = findworldcube("frozen_gravel");
    if(c.texture[O_TOP] != surfacetexture && (!frozen || c.texture[O_TOP] != frozen->slot) && (!moss || c.texture[O_TOP] != moss->slot) &&
       (!thawedmoss || c.texture[O_TOP] != thawedmoss->slot) && (!dirt || c.texture[O_TOP] != dirt->slot) &&
       (!gravel || c.texture[O_TOP] != gravel->slot))
        return;

    const int top = o.z + size;
    if(top >= WORLD_MAP_SIZE) return;

    const int startx = max(o.x, 0), starty = max(o.y, 0), endx = min(o.x + size, int(WORLD_CHUNK_SIZE)),
              endy = min(o.y + size, int(WORLD_CHUNK_SIZE));

    for(int y = starty; y < endy; y += WORLD_BLOCK_SIZE)
        for(int x = startx; x < endx; x += WORLD_BLOCK_SIZE)
        {
            const cube &above = lookupgeneratedworldcube(root, ivec(x + WORLD_BLOCK_SIZE / 2, y + WORLD_BLOCK_SIZE / 2, top));
            if(!isempty(above) || above.material != MAT_AIR) continue;

            const int blockx = ctx.chunkx * WORLD_CHUNK_BLOCKS + x / WORLD_BLOCK_SIZE,
                      blocky = ctx.chunky * WORLD_CHUNK_BLOCKS + y / WORLD_BLOCK_SIZE;
            const uint worldx = uint(blockx), worldy = uint(blocky);
            const float noisex = float(blockx) + 0.5f, noisey = float(blocky) + 0.5f;
            const int ground = (top - WORLD_GROUND_HEIGHT) / WORLD_BLOCK_SIZE;
            const vec position(float(blockx) * WORLD_BLOCK_SIZE, float(blocky) * WORLD_BLOCK_SIZE, float(top));
            const game::BiomeSample climate = ctx.generator.sampleBiome(position);
            int coldtype = -1;
            bool coldprop = false;
            if(climate.temperature <= 2.0f)
            {
                const game::ColdSample cold = ctx.generator.samplecold(blockx, blocky, ground, climate);
                float vegetation = cold.vegetation * 0.4f;
                const bool gravelground = gravel && c.texture[O_TOP] == gravel->slot, dirtground = dirt && c.texture[O_TOP] == dirt->slot;
                const bool mossground = cold.material == game::WORLD_MOSS || cold.material == game::WORLD_FROZEN_MOSS;
                const char *id = mossground                                       ? (cold.region > 0.58f ? "moss_clump" : "tundra_tuft")
                                 : cold.region > 0.58f && cold.vegetation > 0.40f ? "dwarf_shrub"
                                                                                  : "tundra_tuft";
                if(gravelground || (dirtground && cold.region > 0.62f))
                {
                    id = gravelground || cold.exposure > 0.58f ? "tundra_stones" : "tundra_branch";
                    vegetation = cold.region > 0.56f && !cold.covered ? 0.012f : 0.0f;
                    coldprop = true;
                }
                coldtype = worldscatterdefinitions.find(findworldscatter(id));
                if(worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy, 0x517EE731U)) >= vegetation) continue;
            }
            int type = coldprop ? coldtype : climate.temperature > -2.0f ? chooseworldflower(ctx, noisex, noisey, worldx, worldy) : -1;

            if(type < 0)
            {
                if(worldgrassscatter < 0) continue;
                const float noise = clamp(ctx.distribution.GetNoise(noisex, noisey) * 0.5f + 0.5f, 0.0f, 1.0f),
                            patch = worldsmoothstep(0.2f, 0.8f, noise),
                            density = clamp(ctx.settings.grassdensity * (0.12f + 1.88f * patch * patch), 0.0f, 1.0f);
                if(worldtreeunit(hashworldgrass(ctx.seed, worldx, worldy, 0xA511E9B3U)) >= density) continue;
                type = coldtype >= 0 ? coldtype : worldgrassscatter;
            }

            worldscatterinstance &scatter = ctx.scatter.add(worldscatterinstance(x, y, top, type));
            cacheworldscattertransform(ctx.chunkx, ctx.chunky, ctx.settings.grassmaxoffset, scatter);
        }
}

static void generateworldcacti(cube *root, int chunkx, int chunky, const game::worldsettings &settings, vector<worldscatterinstance> &scatter,
                               bool indexedtextures)
{
    worlddefinition *cactus = findworldscatter("cactus"), *sand = findworldcube("sand");
    if(!cactus || !sand) return;
    const int type = worldscatterdefinitions.find(cactus), sandindex = worldcubedefinitions.find(sand);
    if(!worldgentextures.inrange(sandindex)) return;
    const int sandtexture = indexedtextures ? sandindex : worldgentextures[sandindex].top;
    const uint seed = uint(game::getworldseed());
    game::worldgenerator generator(seed, settings);
    for(int y = 0; y < WORLD_CHUNK_BLOCKS; ++y)
        for(int x = 0; x < WORLD_CHUNK_BLOCKS; ++x)
        {
            const int wx = chunkx * WORLD_CHUNK_BLOCKS + x, wy = chunky * WORLD_CHUNK_BLOCKS + y;
            const uint priority = hashworldgrass(seed, uint(wx), uint(wy), 0xCA671351U) & 0x00FFFFFFU;
            const float patch = clamp(generator.vegetationvariation.GetNoise(float(wx), float(wy)) * 0.5f + 0.5f, 0.0f, 1.0f);
            if(worldtreeunit(priority) >= 0.0015f + 0.0135f * patch * patch) continue;
            bool spaced = true;
            for(int oy = -2; oy <= 2 && spaced; ++oy)
                for(int ox = -2; ox <= 2; ++ox)
                {
                    if(!ox && !oy) continue;
                    const uint other = hashworldgrass(seed, uint(wx + ox), uint(wy + oy), 0xCA671351U) & 0x00FFFFFFU;
                    if(other < priority || (other == priority && (oy < 0 || (!oy && ox < 0))))
                    {
                        spaced = false;
                        break;
                    }
                }
            if(!spaced) continue;
            const game::worldwatersample surface = generator.surface(wx, wy);
            const int top = WORLD_GROUND_HEIGHT + surface.height * WORLD_BLOCK_SIZE;
            if(surface.height < surface.water || top < WORLD_BLOCK_SIZE || top >= WORLD_MAP_SIZE) continue;
            if(generator.sampleBiome(vec((wx + 0.5f) * WORLD_BLOCK_SIZE, (wy + 0.5f) * WORLD_BLOCK_SIZE, float(top))).primary !=
               game::WORLD_BIOME_DESERT)
                continue;
            const ivec base(x * WORLD_BLOCK_SIZE + WORLD_BLOCK_SIZE / 2, y * WORLD_BLOCK_SIZE + WORLD_BLOCK_SIZE / 2, top - 1);
            const cube &ground = lookupgeneratedworldcube(root, base);
            if(isempty(ground) || !isentirelysolid(ground) || ground.material != MAT_AIR || ground.texture[O_TOP] != sandtexture) continue;
            // 10% one block, 40% two, 40% three, 8% four, 2% five.
            const uint roll = hashworldgrass(seed, uint(wx), uint(wy), 0x94CA6713U) % 100;
            const int height = roll < 10 ? 1 : roll < 50 ? 2 : roll < 90 ? 3 : roll < 98 ? 4 : 5;
            if(top + height * WORLD_BLOCK_SIZE > WORLD_MAP_SIZE) continue;
            bool clear = true;
            loopi(height)
            {
                const cube &space = lookupgeneratedworldcube(root, ivec(base.x, base.y, top + i * WORLD_BLOCK_SIZE));
                if(!isempty(space) || space.material != MAT_AIR) clear = false;
            }
            if(!clear) continue;
            loopi(height) scatter.add(worldscatterinstance(x * WORLD_BLOCK_SIZE, y * WORLD_BLOCK_SIZE, top + i * WORLD_BLOCK_SIZE, type));
        }
}

void generateworldscatter(cube *root, int chunkx, int chunky, const game::worldsettings &settings, vector<worldscatterinstance> &scatter,
                          bool indexedtextures)
{
    scatter.setsize(0);
    if(!root) return;
    generateworldcacti(root, chunkx, chunky, settings, scatter, indexedtextures);

    if(!root || (worldgrassscatter < 0 && worldrosescatter < 0 && worldtulipscatter < 0 && worlddandelionscatter < 0)) return;

    const bool grass = worldgrassscatter >= 0 && settings.grassdensity > 0,
               flowers = settings.flowerchance > 0 &&
                         ((worldrosescatter >= 0 && settings.roseweight > 0) || (worldtulipscatter >= 0 && settings.tulipweight > 0) ||
                          (worlddandelionscatter >= 0 && settings.dandelionweight > 0));

    if(!grass && !flowers) return;

    worlddefinition *surface = findworldcube("grass");
    if(!surface && worldcubedefinitions.inrange(worlderrorcube)) surface = worldcubedefinitions[worlderrorcube];
    worldgrasscollectcontext ctx(chunkx, chunky, settings, scatter);

    loopi(8) collectworldgrassnode(ctx, root[i], root, ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE), WORLD_CHUNK_ROOT_SIZE, surface->slot);
}
