#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

namespace game
{
    void snapshotworldnpcdefinitions(worldgencontext *generation)
    {
        // Called on the submitting thread, never from terrain/LOD workers.
        generation->npcdefinitions.setsize(0);
        loopi(numnpcdefinitions()) generation->npcdefinitions.add(*getnpcdefinition(i));
    }

    static bool generatednpcair(const cube &c, const ivec &origin, int size, const vec &minimum, const vec &maximum)
    {
        loopi(3)
            if(origin[i] >= maximum[i] || origin[i] + size <= minimum[i]) return true;
        if(!c.children) return isempty(c) && (c.material & MATF_VOLUME) == MAT_AIR;
        loopi(8)
            if(!generatednpcair(c.children[i], ivec(i, origin, size / 2), size / 2, minimum, maximum)) return false;
        return true;
    }

    void generateworldnpcs(worldgencontext *generation, const cube *root, int chunkx, int chunky, vector<uchar> &data, bool generated)
    {
        ZoneScopedN("Chunks/Generate NPC positions");
        data.setsize(0);
        if(!generation || !root) return;
        worldgencontext &ctx = *generation;
        // The existing chunk NPC format also stores dormant, never-activated inhabitants.
        const auto putuint = [&data](uint value)
        {
            loopi(4) data.add(uchar(value >> (8 * i)));
        };
        const auto putfloat = [&putuint](float value)
        {
            union
            {
                float f;
                uint u;
            } bits;
            bits.f = value;
            putuint(bits.u);
        };
        putuint(2);
        putuint(0);
        uint count = 0;
        vector<vec> occupied;
        vector<float> radii;
        cavenpcpool pool;
        loopv(ctx.npcdefinitions) pool.definitions.add(&ctx.npcdefinitions[i]);
        pool.sort();
        loopv(pool.definitions)
        {
            const npcdefinition &definition = *pool.definitions[i];
            const bool cave = definition.attitude == NPC_AGGRESSIVE;
            if(!cave && definition.naturalbiome < 0) continue;
            const int cells = WORLD_CHUNK_BLOCKS / PASSIVE_NPC_CELL_BLOCKS;
            for(int x = 0; x < cells; ++x)
                for(int y = 0; y < cells; ++y)
                    for(int band = cave ? 0 : -1; band < (cave ? definition.cavebands : 0); ++band)
                    {
                        if(ctx.iscanceled())
                        {
                            data.setsize(0);
                            return;
                        }
                        if(cave && pool.choose(ctx.seed, chunkx * cells + x, chunky * cells + y, band, definition.cavebands) != &definition) continue;
                        passivenpcspawn spawns[16];
                        const int members = generatepassivenpcgroup(definition, ctx.seed, chunkx * cells + x, chunky * cells + y, spawns, 16, band);
                        loopj(members)
                        {
                            const passivenpcspawn &spawn = spawns[j];
                            const int bx = spawn.blockx - chunkx * WORLD_CHUNK_BLOCKS, by = spawn.blocky - chunky * WORLD_CHUNK_BLOCKS;
                            if(bx < 0 || by < 0 || bx >= WORLD_CHUNK_BLOCKS || by >= WORLD_CHUNK_BLOCKS) continue;
                            const int column = by * WORLD_CHUNK_BLOCKS + bx,
                                      height =
                                          generated ? ctx.heightmap[column] / WORLD_BLOCK_SIZE : ctx.generator.height(spawn.blockx, spawn.blocky),
                                      surface = height - WORLD_MIN_HEIGHT;
                            if(!cave && (generated ? int(ctx.biomemap[column]) : ctx.generator.biome(spawn.blockx, spawn.blocky, height)) !=
                                            definition.naturalbiome)
                                continue;
                            vec position;
                            const auto probe = [&](int wx, int wy, int z)
                            {
                                const int lx = wx - chunkx * WORLD_CHUNK_BLOCKS, ly = wy - chunky * WORLD_CHUNK_BLOCKS;
                                const vec feet((lx + 0.5f) * WORLD_BLOCK_SIZE, (ly + 0.5f) * WORLD_BLOCK_SIZE, z * WORLD_BLOCK_SIZE + 0.1f);
                                const vec minimum(feet.x - definition.radius, feet.y - definition.radius, feet.z),
                                    maximum(feet.x + definition.radius, feet.y + definition.radius,
                                            feet.z + definition.height + max(definition.height * 0.08f, 2.0f));
                                if(minimum.x < 0 || minimum.y < 0 || maximum.x >= WORLD_CHUNK_SIZE || maximum.y >= WORLD_CHUNK_SIZE || z < 1 ||
                                   maximum.z >= WORLD_MAP_SIZE)
                                    return false;
                                const cube &support = lookupgeneratedworldcube(root, ivec(vec(feet).subz(1)));
                                if(isempty(support) || (support.material & MATF_VOLUME) != MAT_AIR) return false;
                                loopk(8)
                                    if(!generatednpcair(root[k], ivec(k, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE), WORLD_CHUNK_ROOT_SIZE, minimum,
                                                        maximum))
                                        return false;
                                bool roof = false;
                                for(int rz = int(ceilf(maximum.z)); rz < WORLD_MAP_SIZE; rz += WORLD_BLOCK_SIZE)
                                    if(!isempty(lookupgeneratedworldcube(root, ivec(int(feet.x), int(feet.y), rz))))
                                    {
                                        roof = true;
                                        break;
                                    }
                                if(roof != cave) return false;
                                position = vec(feet).addz(definition.height);
                                loopvk(occupied)
                                {
                                    const float separation = definition.radius + radii[k] + 1;
                                    if(occupied[k].squaredist(position) < separation * separation) return false;
                                }
                                return true;
                            };
                            if(cave ? !findcavenpcfloor(spawn, surface, 256 + ctx.settings.sealevel, probe)
                                    : !probe(spawn.blockx, spawn.blocky, surface))
                                continue;
                            occupied.add(position);
                            radii.add(definition.radius);
                            putuint(uint(strlen(definition.id)));
                            data.put((const uchar *)definition.id, int(strlen(definition.id)));
                            putfloat(position.x + chunkx * float(WORLD_CHUNK_SIZE));
                            putfloat(position.y + chunky * float(WORLD_CHUNK_SIZE));
                            putfloat(position.z);
                            putfloat(spawn.yaw);
                            putfloat(0);
                            putfloat(float(definition.health));
                            putuint(uint(definition.attitude));
                            putuint(uint(definition.behavior));
                            putuint(0);
                            putuint(uint(spawn.key));
                            putuint(uint(spawn.key >> 32));
                            ++count;
                        }
                    }
        }
        loopi(4) data[4 + i] = uchar(count >> (8 * i));
    }

} // namespace game
