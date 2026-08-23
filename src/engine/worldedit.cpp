// worldedit.cpp: streamed-world editing and scatter placement

#ifdef WORLDIO_MODULE_IMPLEMENTATION

struct worldeditcapture
{
    bool active;

    worldeditcapture() : active(false) {}

    void clear() { active = false; }
};

static worldeditcapture currentworldedit;

static ivec worldorientnormal(int orient)
{
    ivec normal(0, 0, 0);
    if(orient >= O_LEFT && orient <= O_TOP)
        normal[dimension(orient)] = dimcoord(orient) ? 1 : -1;
    return normal;
}

static bool validworldscatter(const worldchunk &chunk, const worldscatterinstance &scatter)
{
    if(scatter.type < 0 || scatter.type >= numworldscatters() ||
       scatter.x < 0 || scatter.x >= WORLD_CHUNK_SIZE ||
       scatter.y < 0 || scatter.y >= WORLD_CHUNK_SIZE ||
       scatter.z < 0 || scatter.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE ||
       scatter.orient < O_LEFT || scatter.orient > O_TOP)
        return false;

    const ivec origin = worldchunkorigin(chunk);
    const ivec center = ivec(origin).add(ivec(
        scatter.x + WORLD_BLOCK_SIZE / 2,
        scatter.y + WORLD_BLOCK_SIZE / 2,
        scatter.z + WORLD_BLOCK_SIZE / 2));
    const ivec normal = worldorientnormal(scatter.orient),
               supportcenter = ivec(center).sub(
                   ivec(normal).mul(WORLD_BLOCK_SIZE));
    if(!insideworld(center) || !insideworld(supportcenter)) return false;
    ivec cubeorigin;
    int cubesize;
    const cube occupied = sampleworldblockcube(lookupcube(center, 0, cubeorigin, cubesize), center, cubeorigin, cubesize);
    if(!isempty(occupied) || occupied.material != MAT_AIR) return false;

    const bool placeable = isworldplaceable(scatter.type);
    if((!placeable && scatter.orient != O_TOP) ||
       (placeable && scatter.orient == O_BOTTOM))
        return false;
    const cube support = sampleworldblockcube(lookupcube(supportcenter, 0, cubeorigin, cubesize), supportcenter, cubeorigin, cubesize);
    if(isempty(support) || !isentirelysolid(support) ||
       support.material != MAT_AIR)
        return false;
    return true;
}

void cancelworldedit()
{
    currentworldedit.clear();
}

void beginworldedit(int operation, const selinfo &selection, int arg1, int arg2, int arg3, int arg4)
{
    cancelworldedit();
    if(worldchunks.empty() || selection.s.iszero()) return;

    currentworldedit.active = true;
}

void commitworldedit()
{
    currentworldedit.clear();
}

bool worldtorchincell(const ivec &cell)
{
    if(cell.x < 0 || cell.y < 0 || cell.z < 0 ||
       cell.x >= worldsize || cell.y >= worldsize ||
       cell.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE)
        return false;
    const int chunkx = worldfirstchunkx + cell.x / WORLD_CHUNK_SIZE,
              chunky = worldfirstchunky + cell.y / WORLD_CHUNK_SIZE,
              chunkindex = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(chunkindex)) return false;
    const worldchunk &chunk = worldchunks[chunkindex];
    const ivec origin = worldchunkorigin(chunk);
    loopv(chunk.scatter)
    {
        const worldscatterinstance &scatter = chunk.scatter[i];
        if(isworldplaceable(scatter.type) &&
           scatter.x == cell.x - origin.x &&
           scatter.y == cell.y - origin.y &&
           scatter.z == cell.z)
            return true;
    }
    return false;
}

bool worldplaceableblockcollisionat(const ivec &cell)
{
    if(cell.x < 0 || cell.y < 0 || cell.z < 0 ||
       cell.x >= worldsize || cell.y >= worldsize ||
       cell.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE)
        return false;
    const int chunkx = worldfirstchunkx + cell.x / WORLD_CHUNK_SIZE,
              chunky = worldfirstchunky + cell.y / WORLD_CHUNK_SIZE,
              chunkindex = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(chunkindex)) return false;
    const worldchunk &chunk = worldchunks[chunkindex];
    if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) return false;
    const ivec origin = worldchunkorigin(chunk);
    loopv(chunk.scatter)
    {
        const worldscatterinstance &scatter = chunk.scatter[i];
        if(scatter.x == cell.x - origin.x && scatter.y == cell.y - origin.y && scatter.z == cell.z &&
           getworldplaceableblockcollision(scatter.type))
            return true;
    }
    return false;
}

int getworldscatterindexat(const ivec &support, int orient)
{
    const ivec target = ivec(support).add(ivec(worldorientnormal(orient)).mul(WORLD_BLOCK_SIZE));
    if(target.x < 0 || target.y < 0 || target.z < 0 || target.x >= worldsize || target.y >= worldsize ||
       target.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE)
        return -1;
    const int chunkx = worldfirstchunkx + target.x / WORLD_CHUNK_SIZE,
              chunky = worldfirstchunky + target.y / WORLD_CHUNK_SIZE,
              chunkindex = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(chunkindex)) return -1;
    const worldchunk &chunk = worldchunks[chunkindex];
    const ivec origin = worldchunkorigin(chunk);
    loopv(chunk.scatter)
    {
        const worldscatterinstance &scatter = chunk.scatter[i];
        if(scatter.x == target.x - origin.x && scatter.y == target.y - origin.y && scatter.z == target.z && scatter.orient == orient)
            return scatter.type;
    }
    return -1;
}

bool editworldscatter(int type, const ivec &support, int orient, bool place)
{
    if(!worldscatterdefinitions.inrange(type) || orient < O_LEFT || orient > O_TOP ||
       (!isworldplaceable(type) && orient != O_TOP) || (isworldplaceable(type) && orient == O_BOTTOM))
        return false;

    const ivec target = ivec(support).add(ivec(worldorientnormal(orient)).mul(WORLD_BLOCK_SIZE));

    if(target.x < 0 || target.y < 0 || target.z < 0 || target.x >= worldsize || target.y >= worldsize || target.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE)
        return false;

    selinfo occupied;
    occupied.o = target;
    occupied.s = ivec(1, 1, 1);
    occupied.grid = WORLD_BLOCK_SIZE;
    occupied.orient = orient;
    occupied.cx = occupied.cy = occupied.corner = 0;
    occupied.cxs = occupied.cys = 2;
    if(!worldselectionready(occupied)) return false;

    const int chunkx = worldfirstchunkx + target.x / WORLD_CHUNK_SIZE,
              chunky = worldfirstchunky + target.y / WORLD_CHUNK_SIZE,
              chunkindex = findworldchunk(chunkx, chunky);

    if(!worldchunks.inrange(chunkindex)) return false;
    worldchunk &chunk = worldchunks[chunkindex];
    if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) return false;
    const ivec origin = worldchunkorigin(chunk);
    worldscatterinstance scatter(target.x - origin.x, target.y - origin.y, target.z, type, orient);
    game::cacheworldscattertransform(chunk.x, chunk.y, game::getworldscattermaxoffset(), scatter);

    int existing = -1;
    loopv(chunk.scatter)
    {
        if(chunk.scatter[i].x == scatter.x && chunk.scatter[i].y == scatter.y && chunk.scatter[i].z == scatter.z)
        {
            existing = i;
            break;
        }
    }

    if(place)
    {
        if(existing >= 0 || !validworldscatter(chunk, scatter)) return false;
        chunk.scatter.add(scatter);
    }
    else
    {
        if(existing < 0 || chunk.scatter[existing].type != type || chunk.scatter[existing].orient != orient)
            return false;

        scatter = chunk.scatter[existing];
        chunk.scatter.removeunordered(existing);
    }
    dirtyworldscattermesh(chunk, scatter);
    updateworldscatterers();
    return true;
}

bool worldselectionready(const selinfo &selection)
{
    if(!worldroot || selection.grid <= 0 || selection.s.x <= 0 ||
       selection.s.y <= 0 || selection.s.z <= 0)
        return false;

    int minx = worldfirstchunkx + int(floor(double(selection.o.x) / WORLD_CHUNK_SIZE)),
        miny = worldfirstchunky + int(floor(double(selection.o.y) / WORLD_CHUNK_SIZE)),
        maxx = worldfirstchunkx + int(floor(double(selection.o.x +
                    selection.s.x * selection.grid - 1) / WORLD_CHUNK_SIZE)),
        maxy = worldfirstchunky + int(floor(double(selection.o.y +
                    selection.s.y * selection.grid - 1) / WORLD_CHUNK_SIZE));
    for(int y = miny; y <= maxy; ++y) for(int x = minx; x <= maxx; ++x)
    {
        int index = findworldchunk(x, y);
        if(!worldchunks.inrange(index) || worldchunks[index].loading ||
           !worldchunks[index].root || !worldchunkmounted(worldchunks[index]))
            return false;
        const worldchunk &chunk = worldchunks[index];
        ivec origin = worldchunkorigin(chunk);
        int localminx = clamp(selection.o.x - origin.x, 0, int(WORLD_CHUNK_SIZE) - 1),
            localminy = clamp(selection.o.y - origin.y, 0, int(WORLD_CHUNK_SIZE) - 1),
            localmaxx = clamp(selection.o.x + selection.s.x * selection.grid - 1 - origin.x,
                              0, int(WORLD_CHUNK_SIZE) - 1),
            localmaxy = clamp(selection.o.y + selection.s.y * selection.grid - 1 - origin.y,
                              0, int(WORLD_CHUNK_SIZE) - 1),
            minsection = clamp(selection.o.z / WORLD_SECTION_SIZE, 0,
                               int(WORLD_SECTION_LAYERS) - 1),
            maxsection = clamp((selection.o.z + selection.s.z * selection.grid - 1) /
                               WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1);
        for(int section = minsection; section <= maxsection; ++section)
            for(int tiley = localminy / WORLD_SECTION_SIZE;
                tiley <= localmaxy / WORLD_SECTION_SIZE; ++tiley)
                for(int tilex = localminx / WORLD_SECTION_SIZE;
                    tilex <= localmaxx / WORLD_SECTION_SIZE; ++tilex)
                {
                    int tile = tiley * WORLD_SECTION_COLUMNS + tilex;
                    if(!(chunk.mountedtiles[section] & (1U << tile))) return false;
                }
    }
    return true;
}

#endif
