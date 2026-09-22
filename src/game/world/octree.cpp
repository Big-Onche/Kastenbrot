#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

static void resetworldgencube(cube &c)
{
    c.playeredited = false;
    c.children = NULL;
    c.ext = NULL;
    c.visible = 0;
    c.merged = 0;
    c.material = MAT_AIR;
    emptyfaces(c);
    loopi(6) c.texture[i] = DEFAULT_GEOM;
}

ivec worldgenorientnormal(int orient)
{
    ivec normal(0, 0, 0);
    normal[dimension(orient)] = dimcoord(orient) ? 1 : -1;
    return normal;
}

cube *allocworldgenfamily(worldgencontext &ctx)
{
#ifdef STANDALONE
    cube *c = new cube[8];
    loopi(8) resetworldgencube(c[i]);
    ctx.families++;
    return c;
#else
    if(!ctx.prepared) return newcubes(F_EMPTY);
    cube *c = new cube[8];
    loopi(8) resetworldgencube(c[i]);
    ctx.families++;
    return c;
#endif
}

void freepreparedworldchunk(cube *root)
{
    if(!root) return;
    loopi(8)
        if(root[i].children) freepreparedworldchunk(root[i].children);
    delete[] root;
}

#ifndef STANDALONE
static void setworldcubetexture(cube &c, int texture, int toptexture = -1, int bottomtexture = -1, int material = MAT_AIR)
{
    solidfaces(c);
    c.material = material;
    loopi(6) c.texture[i] = texture;
    if(toptexture >= 0) c.texture[O_TOP] = toptexture;
    if(bottomtexture >= 0) c.texture[O_BOTTOM] = bottomtexture;
}
#endif

bool setworldcubetype(cube &c, const worldgencontext &ctx, int index, int material)
{
    if(!ctx.cubetextures.inrange(index)) return false;
    if(index == ctx.cubetype("ice")) material |= MAT_ALPHA;
#ifdef STANDALONE
    solidfaces(c);
    c.material = material;
    loopi(6) c.texture[i] = ushort(index);
#else
    if(ctx.indexedtextures)
    {
        solidfaces(c);
        c.material = material;
        loopi(6) c.texture[i] = ushort(index);
    }
    else
    {
        const worldgencubetextures &textures = ctx.cubetextures[index];
        setworldcubetexture(c, textures.side, textures.top, textures.bottom, material);
    }
#endif
    return true;
}

void setworldcubematerial(cube &c, int material)
{
    emptyfaces(c);
    c.material = material;
}

uchar &worldgensectionflags(worldgencontext &ctx, int blockx, int blocky, int blockz)
{
    const int tile = blocky / WORLD_SECTION_BLOCKS * WORLD_SECTION_COLUMNS + blockx / WORLD_SECTION_BLOCKS,
              section = clamp(blockz / WORLD_SECTION_BLOCKS, 0, int(WORLD_SECTION_LAYERS) - 1);
    return ctx.renderdata.flags[section][tile];
}

void markworldgencarvedsection(worldgencontext &ctx, int blockx, int blocky, int blockz, bool entrance)
{
    static const int directions[][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    uchar &flags = worldgensectionflags(ctx, blockx, blocky, blockz);
    flags = (flags | SECTION_INTERIOR) & ~SECTION_FULLY_SOLID;

    if(entrance) flags |= SECTION_CAVE_ENTRANCE;

    loopi(6)
    {
        const int coordinate = i < 2 ? blockx : i < 4 ? blocky : blockz, direction = directions[i][i / 2];
        if((direction < 0 && coordinate % WORLD_SECTION_BLOCKS) || (direction > 0 && coordinate % WORLD_SECTION_BLOCKS != WORLD_SECTION_BLOCKS - 1))
            continue;

        const int neighborx = blockx + directions[i][0], neighbory = blocky + directions[i][1], neighborz = blockz + directions[i][2];

        if(neighborx < 0 || neighborx >= WORLD_CHUNK_BLOCKS || neighbory < 0 || neighbory >= WORLD_CHUNK_BLOCKS || neighborz < 0 ||
           neighborz >= WORLD_HEIGHT_BLOCKS)
            continue;

        uchar &neighborflags = worldgensectionflags(ctx, neighborx, neighbory, neighborz);
        neighborflags |= SECTION_INTERIOR;

        if(entrance) neighborflags |= SECTION_CAVE_ENTRANCE;
    }
}

void markworldgenexteriorshell(worldgencontext &ctx, int chunkx, int chunky)
{
    static const int directions[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    ctx.renderdata.clear();
    loopi(WORLD_SECTION_LAYERS)
        loopj(WORLD_SECTION_TILES) ctx.renderdata.flags[i][j] = SECTION_FULLY_SOLID;

    loop(y, WORLD_CHUNK_BLOCKS)
        loop(x, WORLD_CHUNK_BLOCKS)
        {
            const int sealevel = clamp(ctx.watermap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE - WORLD_MIN_HEIGHT, 0, int(WORLD_HEIGHT_BLOCKS));
            const int surface = clamp(ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE - WORLD_MIN_HEIGHT, 0, int(WORLD_HEIGHT_BLOCKS));
            if(surface > 0) worldgensectionflags(ctx, x, y, surface - 1) |= SECTION_EXTERIOR;

            const int tile = y / WORLD_SECTION_BLOCKS * WORLD_SECTION_COLUMNS + x / WORLD_SECTION_BLOCKS;
            loop(section, WORLD_SECTION_LAYERS)
            {
                const int sectiontop = (section + 1) * WORLD_SECTION_BLOCKS;
                if(surface < sectiontop) ctx.renderdata.flags[section][tile] &= ~SECTION_FULLY_SOLID;
            }

            if(surface < sealevel)
            {
                const int first = surface / WORLD_SECTION_BLOCKS, last = (sealevel - 1) / WORLD_SECTION_BLOCKS;

                for(int section = first; section <= last; ++section) ctx.renderdata.flags[section][tile] |= SECTION_WATER;
            }

            loopi(4)
            {
                const int neighborx = x + directions[i][0], neighbory = y + directions[i][1];
                int neighborheight;
                if(neighborx >= 0 && neighborx < WORLD_CHUNK_BLOCKS && neighbory >= 0 && neighbory < WORLD_CHUNK_BLOCKS)
                {
                    neighborheight = ctx.heightmap[neighbory * WORLD_CHUNK_BLOCKS + neighborx];
                }
                else
                    neighborheight = generateworldheight(ctx, chunkx, chunky, neighborx, neighbory);

                const int neighborsurface = clamp(neighborheight / WORLD_BLOCK_SIZE - WORLD_MIN_HEIGHT, 0, int(WORLD_HEIGHT_BLOCKS));

                if(surface <= neighborsurface) continue;

                for(int section = neighborsurface / WORLD_SECTION_BLOCKS; section <= (surface - 1) / WORLD_SECTION_BLOCKS; ++section)
                    ctx.renderdata.flags[section][tile] |= SECTION_EXTERIOR;
            }
        }
}

bool generateworldcube(worldgencontext &ctx, cube &c, const ivec &o, int size, int mingridsize)
{
    if(ctx.iscanceled()) return false;
    int type = worldcubetype(ctx, o, size);
    if(type == WORLD_TERRAIN_MIXED && size <= mingridsize) type = worldrepresentativecubetype(ctx, o, size);
    if(type == WORLD_TERRAIN_EMPTY)
    {
        setworldcubematerial(c, MAT_AIR);
        return true;
    }
    if(type == WORLD_TERRAIN_WATER)
    {
        setworldcubematerial(c, MAT_WATER);
        return true;
    }
    if(type >= 0 && ctx.cubetextures.inrange(type))
    {
        setworldcubetype(c, ctx, type);
        return true;
    }

    if(size <= mingridsize)
    {
        setworldcubematerial(c, MAT_AIR);
        return true;
    }

    c.children = allocworldgenfamily(ctx);
    const int childsize = size >> 1;
    loopi(8)
    {
        if(!generateworldcube(ctx, c.children[i], ivec(i, o, childsize), childsize, mingridsize)) return false;
    }

    return true;
}

static void subdivideworldgencube(worldgencontext &ctx, cube &c)
{
    if(c.children) return;
    cube parent = c;
    c.children = allocworldgenfamily(ctx);
    loopi(8)
    {
        c.children[i] = parent;
        c.children[i].children = NULL;
        c.children[i].ext = NULL;
        c.children[i].visible = 0;
        c.children[i].merged = 0;
    }
}

cube &lookupworldgenblock(worldgencontext &ctx, cube *root, const ivec &position)
{
    cube *family = root;
    ivec origin(0, 0, 0);
    int size = WORLD_CHUNK_ROOT_SIZE;
    for(;;)
    {
        const int index = (position.x >= origin.x + size ? 1 : 0) | (position.y >= origin.y + size ? 2 : 0) | (position.z >= origin.z + size ? 4 : 0);
        cube &c = family[index];
        if(size == WORLD_BLOCK_SIZE) return c;
        subdivideworldgencube(ctx, c);
        origin = ivec(index, origin, size);
        family = c.children;
        size >>= 1;
    }
}
