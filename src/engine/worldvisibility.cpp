// worldvisibility.cpp: streamed section visibility, portals, and VA scheduling

#ifdef WORLDIO_MODULE_IMPLEMENTATION

static void resetworldsectionvisibilityqueue();

static void invalidateworldsectionvisibility()
{
    resetworldsectionvisibilityqueue();
    worldsectionvisibilitydirty = true;
    worldsectionvisibilityadditions.setsize(0);
}

static void addworldsectionvisibilitychunk(int x, int y)
{
    if(!worldsectionvisibilitydirty) worldsectionvisibilityadditions.add(ivec(x, y, 0));
}

static int worldcubesectionstate(const cube &c)
{
    if(c.children)
    {
        int state = WORLD_SECTION_OPAQUE;
        loopi(8)
        {
            int childstate = worldcubesectionstate(c.children[i]);
            state |= childstate&WORLD_SECTION_CONTENT;
            state &= childstate | ~WORLD_SECTION_OPAQUE;
        }
        return state;
    }
    return (!isempty(c) || c.material != MAT_AIR ? WORLD_SECTION_CONTENT : 0) |
           (isentirelysolid(c) && !(c.material&MAT_ALPHA) ? WORLD_SECTION_OPAQUE : 0);
}

static void fillworldsectionpassability(const cube &c, const ivec &origin, int size, uchar *passable)
{
    if(size <= WORLD_BLOCK_SIZE)
    {
        int x = origin.x / WORLD_BLOCK_SIZE, y = origin.y / WORLD_BLOCK_SIZE, z = origin.z / WORLD_BLOCK_SIZE;
        passable[(z * WORLD_SECTION_BLOCKS + y) * WORLD_SECTION_BLOCKS + x] =
            (worldcubesectionstate(c)&WORLD_SECTION_OPAQUE) == 0;
        return;
    }
    if(c.children)
    {
        const int childsize = size >> 1;
        loopi(8) fillworldsectionpassability(c.children[i], ivec(i, origin, childsize), childsize, passable);
        return;
    }
    if(worldcubesectionstate(c)&WORLD_SECTION_OPAQUE)
    {
        int minx = origin.x / WORLD_BLOCK_SIZE, miny = origin.y / WORLD_BLOCK_SIZE, minz = origin.z / WORLD_BLOCK_SIZE,
            maxx = (origin.x + size) / WORLD_BLOCK_SIZE,
            maxy = (origin.y + size) / WORLD_BLOCK_SIZE,
            maxz = (origin.z + size) / WORLD_BLOCK_SIZE;
        for(int z = minz; z < maxz; ++z) for(int y = miny; y < maxy; ++y) for(int x = minx; x < maxx; ++x)
            passable[(z * WORLD_SECTION_BLOCKS + y) * WORLD_SECTION_BLOCKS + x] = 0;
        return;
    }
    if(isempty(c) || c.material&MAT_ALPHA) return;

    // Remipping may collapse shaped terrain across multiple blocks. Rebuild its
    // temporary children so a coarse sloped leaf cannot become a fake portal.
    cube children[8];
    subdivideworldmip(c, children);
    const int childsize = size >> 1;
    loopi(8) fillworldsectionpassability(children[i], ivec(i, origin, childsize), childsize, passable);
}

static int classifyworldsection(const cube &c, uchar *portals, uint facemasks[WORLD_SECTION_FACE_COUNT][WORLD_SECTION_FACE_WORDS],
                                int focuscell = -1, uchar *focusfaces = NULL)
{
    static const int offsets[][3] =
    {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };
    uchar passable[WORLD_SECTION_CELL_COUNT], visited[WORLD_SECTION_CELL_COUNT];
    ushort queue[WORLD_SECTION_CELL_COUNT];
    memset(passable, 1, sizeof(passable));
    memclear(visited);
    memset(portals, 0, WORLD_SECTION_FACE_COUNT * sizeof(uchar));
    memset(facemasks, 0, WORLD_SECTION_FACE_COUNT * WORLD_SECTION_FACE_WORDS * sizeof(uint));
    if(focusfaces) *focusfaces = 0;
    fillworldsectionpassability(c, ivec(0, 0, 0), WORLD_SECTION_SIZE, passable);

    for(int z = 0; z < WORLD_SECTION_BLOCKS; ++z) for(int y = 0; y < WORLD_SECTION_BLOCKS; ++y)
    for(int x = 0; x < WORLD_SECTION_BLOCKS; ++x)
    {
        int cell = (z * WORLD_SECTION_BLOCKS + y) * WORLD_SECTION_BLOCKS + x;
        if(!passable[cell]) continue;
        int yz = z * WORLD_SECTION_BLOCKS + y,
            xz = z * WORLD_SECTION_BLOCKS + x,
            xy = y * WORLD_SECTION_BLOCKS + x;
        if(x == 0) facemasks[0][yz >> 5] |= 1U << (yz & 31);
        if(x == WORLD_SECTION_BLOCKS - 1) facemasks[1][yz >> 5] |= 1U << (yz & 31);
        if(y == 0) facemasks[2][xz >> 5] |= 1U << (xz & 31);
        if(y == WORLD_SECTION_BLOCKS - 1) facemasks[3][xz >> 5] |= 1U << (xz & 31);
        if(z == 0) facemasks[4][xy >> 5] |= 1U << (xy & 31);
        if(z == WORLD_SECTION_BLOCKS - 1) facemasks[5][xy >> 5] |= 1U << (xy & 31);
    }

    loopi(WORLD_SECTION_CELL_COUNT)
    {
        if(!passable[i] || visited[i]) continue;
        int head = 0, tail = 0;
        uchar faces = 0;
        bool containsfocus = false;
        visited[i] = 1;
        queue[tail++] = ushort(i);
        while(head < tail)
        {
            int cell = queue[head++],
                x = cell % WORLD_SECTION_BLOCKS,
                y = (cell / WORLD_SECTION_BLOCKS) % WORLD_SECTION_BLOCKS,
                z = cell / (WORLD_SECTION_BLOCKS * WORLD_SECTION_BLOCKS);
            if(cell == focuscell) containsfocus = true;
            if(x == 0) faces |= 1<<0;
            if(x == WORLD_SECTION_BLOCKS - 1) faces |= 1<<1;
            if(y == 0) faces |= 1<<2;
            if(y == WORLD_SECTION_BLOCKS - 1) faces |= 1<<3;
            if(z == 0) faces |= 1<<4;
            if(z == WORLD_SECTION_BLOCKS - 1) faces |= 1<<5;

            loopj(WORLD_SECTION_FACE_COUNT)
            {
                int nx = x + offsets[j][0], ny = y + offsets[j][1], nz = z + offsets[j][2];
                if(nx < 0 || nx >= WORLD_SECTION_BLOCKS || ny < 0 || ny >= WORLD_SECTION_BLOCKS ||
                   nz < 0 || nz >= WORLD_SECTION_BLOCKS)
                    continue;
                int neighbor = (nz * WORLD_SECTION_BLOCKS + ny) * WORLD_SECTION_BLOCKS + nx;
                if(!passable[neighbor] || visited[neighbor]) continue;
                visited[neighbor] = 1;
                queue[tail++] = ushort(neighbor);
            }
        }
        loopj(WORLD_SECTION_FACE_COUNT) if(faces & (1<<j)) portals[j] |= faces;
        if(containsfocus && focusfaces) *focusfaces = faces;
    }
    return worldcubesectionstate(c);
}

static bool prepareworldchunksectionstates(worldchunkjob &job)
{
    if(!job.root) return false;
    ZoneScopedN("Chunks/Worker classify sections");
    loopi(WORLD_SECTION_LAYERS)
    {
        uint content = 0, opaque = 0;
        loopj(WORLD_SECTION_TILES)
        {
            if(SDL_AtomicGet(&job.cancelled)) return false;
            int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS;
            ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, i * WORLD_SECTION_SIZE);
            int state;
            if(job.renderdata.flags[i][j]&SECTION_NO_RENDER)
            {
                state = WORLD_SECTION_CONTENT | WORLD_SECTION_OPAQUE;
                memset(job.portals[i][j], 0, sizeof(job.portals[i][j]));
                memset(job.portalcellmasks[i][j], 0, sizeof(job.portalcellmasks[i][j]));
            }
            else state = classifyworldsection(lookupworldchunkrootcube(static_cast<const cube *>(job.root), pos, WORLD_SECTION_SIZE),
                                              job.portals[i][j], job.portalcellmasks[i][j]);
            if(state&WORLD_SECTION_CONTENT) content |= 1U << j;
            if(state&WORLD_SECTION_OPAQUE) opaque |= 1U << j;
        }
        job.contenttiles[i] = content;
        job.opaquetiles[i] = opaque;
    }
    return true;
}

static int worldchunksectionstate(worldchunk &chunk, int tile, int section)
{
    const uint tilebit = 1U << tile;
    if((chunk.contentknown[section] & tilebit) && (chunk.opaqueknown[section] & tilebit))
        return (chunk.contenttiles[section] & tilebit ? WORLD_SECTION_CONTENT : 0) |
               (chunk.opaquetiles[section] & tilebit ? WORLD_SECTION_OPAQUE : 0);
    if(chunk.renderdata.flags[section][tile]&SECTION_NO_RENDER)
    {
        chunk.contentknown[section] |= tilebit;
        chunk.contenttiles[section] |= tilebit;
        chunk.opaqueknown[section] |= tilebit;
        chunk.opaquetiles[section] |= tilebit;
        return WORLD_SECTION_CONTENT | WORLD_SECTION_OPAQUE;
    }
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE,
             section * WORLD_SECTION_SIZE);
    int state;
    if(chunk.mountedtiles[section] & tilebit)
    {
        ivec actualorigin;
        int actualsize;
        state = worldcubesectionstate(
            lookupcube(ivec(worldchunkorigin(chunk)).add(pos), -WORLD_SECTION_SIZE,
                       actualorigin, actualsize));
    }
    else state = worldcubesectionstate(
        lookupworldchunkcube(static_cast<const worldchunk &>(chunk),
                             pos, WORLD_SECTION_SIZE));
    chunk.contentknown[section] |= tilebit;
    chunk.opaqueknown[section] |= tilebit;
    if(state&WORLD_SECTION_CONTENT) chunk.contenttiles[section] |= tilebit;
    else chunk.contenttiles[section] &= ~tilebit;
    if(state&WORLD_SECTION_OPAQUE) chunk.opaquetiles[section] |= tilebit;
    else chunk.opaquetiles[section] &= ~tilebit;
    return state;
}

static bool worldchunksectionhascontent(worldchunk &chunk, int tile, int section)
{
    return (worldchunksectionstate(chunk, tile, section)&WORLD_SECTION_CONTENT) != 0;
}

static void setworldchunksectioncontent(worldchunk &chunk, int tile, int section, bool content)
{
    const uint tilebit = 1U << tile;
    chunk.contentknown[section] |= tilebit;
    if(content) chunk.contenttiles[section] |= tilebit;
    else chunk.contenttiles[section] &= ~tilebit;
}

static void cacheworldchunksectionclassification(worldchunk &chunk, int tile, int section, int state, const uchar *portals,
                                                 const uint facemasks[WORLD_SECTION_FACE_COUNT][WORLD_SECTION_FACE_WORDS])
{
    const uint tilebit = 1U << tile;
    chunk.contentknown[section] |= tilebit;
    chunk.opaqueknown[section] |= tilebit;
    chunk.portalsknown[section] |= tilebit;
    if(state&WORLD_SECTION_CONTENT) chunk.contenttiles[section] |= tilebit;
    else chunk.contenttiles[section] &= ~tilebit;
    if(state&WORLD_SECTION_OPAQUE) chunk.opaquetiles[section] |= tilebit;
    else chunk.opaquetiles[section] &= ~tilebit;
    memcpy(chunk.portals[section][tile], portals, WORLD_SECTION_FACE_COUNT * sizeof(uchar));
    memcpy(chunk.portalcellmasks[section][tile], facemasks,
           WORLD_SECTION_FACE_COUNT * WORLD_SECTION_FACE_WORDS * sizeof(uint));
}

static const uchar *worldchunksectionportals(worldchunk &chunk, int tile, int section)
{
    const uint tilebit = 1U << tile;
    if(chunk.portalsknown[section] & tilebit) return chunk.portals[section][tile];

    if(chunk.renderdata.flags[section][tile]&SECTION_NO_RENDER)
    {
        uchar portals[WORLD_SECTION_FACE_COUNT] = { 0 };
        uint facemasks[WORLD_SECTION_FACE_COUNT][WORLD_SECTION_FACE_WORDS];
        memclear(facemasks);
        cacheworldchunksectionclassification(chunk, tile, section, WORLD_SECTION_CONTENT | WORLD_SECTION_OPAQUE, portals, facemasks);
        return chunk.portals[section][tile];
    }

    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE);
    uchar portals[WORLD_SECTION_FACE_COUNT];
    uint facemasks[WORLD_SECTION_FACE_COUNT][WORLD_SECTION_FACE_WORDS];
    int state;
    if(chunk.mountedtiles[section] & tilebit)
    {
        ivec actualorigin;
        int actualsize;
        state = classifyworldsection(lookupcube(ivec(worldchunkorigin(chunk)).add(pos), -WORLD_SECTION_SIZE, actualorigin, actualsize),
                                     portals, facemasks);
    }
    else state = classifyworldsection(lookupworldchunkcube(static_cast<const worldchunk &>(chunk), pos, WORLD_SECTION_SIZE), portals,
                                      facemasks);
    cacheworldchunksectionclassification(chunk, tile, section, state, portals, facemasks);
    return chunk.portals[section][tile];
}

static const uint *worldchunksectionfacemask(worldchunk &chunk, int tile, int section, int face)
{
    worldchunksectionportals(chunk, tile, section);
    return chunk.portalcellmasks[section][tile][face];
}

static uchar worldchunksectionfocusfaces(worldchunk &chunk, int tile, int section, const vec &focus)
{
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE),
         runtimepos = ivec(worldchunkorigin(chunk)).add(pos);
    int focusx = clamp(int(floorf((focus.x - runtimepos.x) / WORLD_BLOCK_SIZE)), 0, int(WORLD_SECTION_BLOCKS) - 1),
        focusy = clamp(int(floorf((focus.y - runtimepos.y) / WORLD_BLOCK_SIZE)), 0, int(WORLD_SECTION_BLOCKS) - 1),
        focusz = clamp(int(floorf((focus.z - runtimepos.z) / WORLD_BLOCK_SIZE)), 0, int(WORLD_SECTION_BLOCKS) - 1),
        focuscell = (focusz * WORLD_SECTION_BLOCKS + focusy) * WORLD_SECTION_BLOCKS + focusx;
    uchar portals[WORLD_SECTION_FACE_COUNT], focusfaces = 0;
    uint facemasks[WORLD_SECTION_FACE_COUNT][WORLD_SECTION_FACE_WORDS];
    int state;
    if(chunk.mountedtiles[section] & (1U << tile))
    {
        ivec actualorigin;
        int actualsize;
        state = classifyworldsection(lookupcube(runtimepos, -WORLD_SECTION_SIZE, actualorigin, actualsize), portals, facemasks, focuscell, &focusfaces);
    }
    else state = classifyworldsection(lookupworldchunkcube(static_cast<const worldchunk &>(chunk), pos, WORLD_SECTION_SIZE), portals, facemasks, focuscell, &focusfaces);
    cacheworldchunksectionclassification(chunk, tile, section, state, portals, facemasks);
    return focusfaces;
}

static void setworldchunksectionopaque(worldchunk &chunk, int tile, int section, bool opaque)
{
    const uint tilebit = 1U << tile;
    chunk.opaqueknown[section] |= tilebit;
    if(opaque) chunk.opaquetiles[section] |= tilebit;
    else chunk.opaquetiles[section] &= ~tilebit;
}

static int worldchunkvaupdatekey(const ivec &origin)
{
    const int rowsize = WORLD_RUNTIME_SIZE / WORLD_SECTION_SIZE;
    return ((origin.z / WORLD_SECTION_SIZE) * rowsize
          + origin.y / WORLD_SECTION_SIZE) * rowsize
          + origin.x / WORLD_SECTION_SIZE;
}

static bool queueworldchunkvaupdate(const ivec &origin)
{
    int key = worldchunkvaupdatekey(origin);
    if(worldchunkvaupdateset.access(key)) return false;
    worldchunkvaupdateset.add(key);
    worldchunkvaupdates.add(key);
    TracyPlot("Chunks/Pending VA sections", int64_t(worldchunkvaupdates.length()));
    return true;
}

static void queueworldchunksectionupdates(const worldchunk &chunk, int tile, const int *sections, int numsections)
{
    ZoneScopedN("Chunks/Queue affected VA sections");
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec bbmins[WORLD_MAX_SECTION_BATCH], bbmaxs[WORLD_MAX_SECTION_BATCH];
    int numregions = 0;
    loopi(numsections)
    {
        ivec center = worldchunkorigin(chunk, sections[i] * WORLD_SECTION_SIZE);
        center.add(ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, 0));
        queueworldchunkvaupdate(center);

        // Queue independent mesh tiles plus the adjacent border tiles.
        bbmins[numregions] = center;
        bbmaxs[numregions] = ivec(center).add(WORLD_SECTION_SIZE).min(ivec(worldsize, worldsize, WORLD_MAP_SIZE));
        numregions++;
    }
    if(numregions)
    {
        // Unmount already destroys the removed section's VAs. Surviving border
        // tiles remain drawable until their replacements can be built.
        bool oldsuppress = suppressworldchunkdirty;
        suppressworldchunkdirty = true;
        changedstreaming(bbmins, bbmaxs, numregions, false);
        suppressworldchunkdirty = oldsuppress;
    }
    ZoneValue(numsections);
}

static bool worldchunksectionnearplayer(const worldchunk &chunk, int tile, int section, int radius, const ivec &focussection)
{
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    const ivec origin = worldchunkorigin(chunk);
    const int sectionx = origin.x / WORLD_SECTION_SIZE + x,
              sectiony = origin.y / WORLD_SECTION_SIZE + y;

    return abs(sectionx - focussection.x) <= radius && abs(sectiony - focussection.y) <= radius && abs(section - focussection.z) <= radius;
}

struct worldsectionnode
{
    int chunkx, chunky, tile, section;
    uchar exits;

    worldsectionnode(const worldchunk &chunk, int tile, int section, uchar exits)
        : chunkx(chunk.x), chunky(chunk.y), tile(tile), section(section), exits(exits)
    {
    }
};

static vector<worldsectionnode> worldsectionvisibilityqueue;
static int worldsectionvisibilitycursor = 0;
static uint worldsectionvisibilityepoch = 1, worldsectionvisibilitypublication = 1;
static bool worldsectionvisibilitypublishing = false;

static void resetworldsectionvisibilityqueue()
{
    worldsectionvisibilityqueue.setsize(0);
    worldsectionvisibilitycursor = 0;
    worldsectionvisibilitypublishing = false;
}

static bool findworldsectionneighbor(int chunkindex, int tile, int section, int dx, int dy, int dz, int focusx, int focusy, int &neighborindex, int &neighbortile, int &neighborsection)
{
    worldchunk &chunk = worldchunks[chunkindex];
    int chunkx = chunk.x, chunky = chunk.y,
        x = tile % WORLD_SECTION_COLUMNS + dx,
        y = tile / WORLD_SECTION_COLUMNS + dy;
    neighborsection = section + dz;
    if(neighborsection < 0 || neighborsection >= WORLD_SECTION_LAYERS) return false;
    if(x < 0) { --chunkx; x += WORLD_SECTION_COLUMNS; }
    else if(x >= WORLD_SECTION_COLUMNS) { ++chunkx; x -= WORLD_SECTION_COLUMNS; }
    if(y < 0) { --chunky; y += WORLD_SECTION_COLUMNS; }
    else if(y >= WORLD_SECTION_COLUMNS) { ++chunky; y -= WORLD_SECTION_COLUMNS; }
    neighborindex = chunkx == chunk.x && chunky == chunk.y ? chunkindex : findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(neighborindex)) return false;
    worldchunk &neighbor = worldchunks[neighborindex];
    if(neighbor.loading || neighbor.corrupted || !neighbor.root || !worldchunkinview(neighbor, focusx, focusy)) return false;
    neighbortile = y * WORLD_SECTION_COLUMNS + x;
    return true;
}

static bool worldsectionfacesoverlap(int chunkindex, int tile, int section, int face, int neighborindex, int neighbortile, int neighborsection)
{
    const uint *facemask = worldchunksectionfacemask(worldchunks[chunkindex], tile, section, face),
               *neighbormask = worldchunksectionfacemask(worldchunks[neighborindex], neighbortile, neighborsection, face^1);
    loopi(WORLD_SECTION_FACE_WORDS) if(facemask[i] & neighbormask[i]) return true;
    return false;
}

static void markworldsectionvisible(worldchunk &chunk, int tile, int section)
{
    if(chunk.visibilityepoch != worldsectionvisibilityepoch)
    {
        memclear(chunk.reachablefaces);
        memclear(chunk.traversedtiles);
        chunk.visibilityepoch = worldsectionvisibilityepoch;
    }
    const uint tilebit = 1U << tile;
    if(!worldchunksectionhascontent(chunk, tile, section)) return;
    chunk.traversedtiles[section] |= tilebit;
    // A reachable air face can expose a formerly buried solid section, including
    // across chunk boundaries where generation/edit classification is local.
    uchar &flags = chunk.renderdata.flags[section][tile];
    if(flags&SECTION_NO_RENDER)
    {
        flags = (flags & ~SECTION_NO_RENDER) | SECTION_INTERIOR;
        dirtyworldchunkvaresidency(chunk, tile, section);
    }
    if(chunk.visibletiles[section] & tilebit) return;
    chunk.visibletiles[section] |= tilebit;
    dirtyworldchunkvaresidency(chunk, tile, section);
}

static uchar worldsectionoutwardfaces(const worldchunk &chunk, int tile, int section)
{
    if(worldsectionvisibilityfocus.x == INT_MIN) return 0x3F;
    const ivec origin = ivec(worldchunkorigin(chunk)).div(WORLD_SECTION_SIZE).add(
        ivec(tile % WORLD_SECTION_COLUMNS, tile / WORLD_SECTION_COLUMNS, section));
    uchar faces = 0x3F;
    // A straight sightline never crosses an axis back toward the camera's
    // section. Connectivity through a U-turn is not potential visibility.
    loopi(3)
    {
        if(origin[i] < worldsectionvisibilityfocus[i]) faces &= ~(1 << (2*i + 1));
        else if(origin[i] > worldsectionvisibilityfocus[i]) faces &= ~(1 << (2*i));
    }
    return faces;
}

static void revealworldsection(vector<worldsectionnode> &queue, int chunkindex, int tile, int section, uchar entrances)
{
    worldchunk &chunk = worldchunks[chunkindex];
    markworldsectionvisible(chunk, tile, section);
    const uchar *portals = worldchunksectionportals(chunk, tile, section);
    uchar exits = 0;
    loopi(WORLD_SECTION_FACE_COUNT) if(entrances & (1<<i)) exits |= portals[i];
    exits &= worldsectionoutwardfaces(chunk, tile, section);
    exits &= ~chunk.reachablefaces[section][tile];
    if(!exits) return;
    chunk.reachablefaces[section][tile] |= exits;
    queue.add(worldsectionnode(chunk, tile, section, exits));
}

static void revealworldsectionfromfocus(vector<worldsectionnode> &queue, int chunkindex, int tile, int section, uchar exits)
{
    worldchunk &chunk = worldchunks[chunkindex];
    markworldsectionvisible(chunk, tile, section);
    exits &= ~chunk.reachablefaces[section][tile];
    if(!exits) return;
    chunk.reachablefaces[section][tile] |= exits;
    queue.add(worldsectionnode(chunk, tile, section, exits));
}

static void updateworldsectionvisibility(int chunkx, int chunky)
{
    const double visibilitybudget = min(double(chunkvisibilitybudget), worldchunkstreamremaining() * 0.25);
    static int additioncursor = 0;
    static ivec additionkey(INT_MIN, INT_MIN, INT_MIN);
    static const int directions[][3] =
    {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };
    if(drawfullchunk)
    {
        invalidateworldsectionvisibility();
        return;
    }

    const vec *focus = camera1 ? &camera1->o : player ? &player->o : NULL;
    ivec focussection(INT_MIN, INT_MIN, INT_MIN), focuscell(INT_MIN, INT_MIN, INT_MIN);
    if(focus)
    {
        focussection = ivec(int(floorf(focus->x / WORLD_SECTION_SIZE)), int(floorf(focus->y / WORLD_SECTION_SIZE)),
                            clamp(int(floorf(focus->z / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_LAYERS) - 1));
        focuscell = ivec(int(floorf(focus->x / WORLD_BLOCK_SIZE)), int(floorf(focus->y / WORLD_BLOCK_SIZE)),
                         int(floorf(focus->z / WORLD_BLOCK_SIZE)));
    }
    bool focuschanged = focussection != worldsectionvisibilityfocus,
         rebuild = worldsectionvisibilitydirty || chunkx != worldsectionvisibilitychunkx || chunky != worldsectionvisibilitychunky ||
                   worldrenderdistance != worldsectionvisibilitymaxdist || focuschanged;
    const bool seed = rebuild || !worldsectionvisibilityadditions.empty() || focuscell != worldsectionvisibilitycell;
    if(!seed && !worldsectionvisibilitypublishing && worldsectionvisibilitycursor >= worldsectionvisibilityqueue.length()) return;
    if(seed) worldsectionvisibilitypublishing = false;

    ZoneScopedN("Chunks/Update dirty section visibility");
    ZoneValue(worldsectionvisibilityadditions.length());
    const Uint64 start = SDL_GetPerformanceCounter(), frequency = SDL_GetPerformanceFrequency();
    vector<worldsectionnode> &queue = worldsectionvisibilityqueue;
    if(rebuild)
    {
        additioncursor = 0;
        resetworldsectionvisibilityqueue();
        worldsectionvisibilityfocus = focussection;
        // Reset lazily when a chunk is reached. Retain its previous visible set
        // until the traversal and the incremental publication finish.
        ++worldsectionvisibilityepoch;

        // Without a camera (during bootstrap), use outside air as a conservative
        // source. Normal rendering is seeded from the camera below, so a sealed
        // cave does not keep the unrelated surface mounted.
        if(!focus) loopv(worldchunks)
        {
            worldchunk &chunk = worldchunks[i];
            if(chunk.loading || chunk.corrupted || !chunk.root || !worldchunkinview(chunk, chunkx, chunky)) continue;
            loopj(WORLD_SECTION_TILES) revealworldsection(queue, i, j, WORLD_SECTION_LAYERS - 1, 1<<5);
        }
    }
    else if(seed)
    {
        ZoneScopedN("Chunks/Extend section visibility");
        while(!worldsectionvisibilityadditions.empty())
        {
            if((SDL_GetPerformanceCounter() - start) * 1000.0 / frequency >= visibilitybudget) break;
            const ivec added = worldsectionvisibilityadditions.last();
            if(added != additionkey)
            {
                additionkey = added;
                additioncursor = 0;
            }
            int index = findworldchunk(added.x, added.y);
            if(!worldchunks.inrange(index))
            {
                worldsectionvisibilityadditions.pop();
                additioncursor = 0;
                continue;
            }
            worldchunk &chunk = worldchunks[index];
            if(chunk.loading || chunk.corrupted || !chunk.root || !worldchunkinview(chunk, chunkx, chunky))
            {
                worldsectionvisibilityadditions.pop();
                additioncursor = 0;
                continue;
            }
            if(!focus && !additioncursor)
                loopj(WORLD_SECTION_TILES) revealworldsection(queue, index, j, WORLD_SECTION_LAYERS - 1, 1<<5);

            // A newly published chunk may connect to an already reachable cave
            // through a side face, even when it is sealed from the sky.
            while(additioncursor < WORLD_SECTION_TILES * WORLD_SECTION_LAYERS)
            {
                if((SDL_GetPerformanceCounter() - start) * 1000.0 / frequency >= visibilitybudget) break;
                const int j = additioncursor / WORLD_SECTION_LAYERS, k = additioncursor % WORLD_SECTION_LAYERS;
                ++additioncursor;
                loopl(6)
                {
                    int neighborindex, neighbortile, neighborsection;
                    if(!findworldsectionneighbor(index, j, k, directions[l][0], directions[l][1], directions[l][2], chunkx, chunky,
                                                 neighborindex, neighbortile, neighborsection))
                        continue;
                    const worldchunk &neighbor = worldchunks[neighborindex];
                    if(neighbor.visibilityepoch != worldsectionvisibilityepoch) continue;
                    if(!(neighbor.reachablefaces[neighborsection][neighbortile] & (1<<(l^1)))) continue;
                    revealworldsection(queue, index, j, k, worldsectionfacesoverlap(index, j, k, l, neighborindex, neighbortile, neighborsection) ? 1<<l : 0);
                }
            }
            if(additioncursor < WORLD_SECTION_TILES * WORLD_SECTION_LAYERS) break;
            worldsectionvisibilityadditions.pop();
            additioncursor = 0;
        }
    }

    // A sealed cave is not connected to outside air, so explicitly seed the
    // camera's own section as a second visibility region.
    if(focus && seed)
    {
        int camerachunkx = worldfirstchunkx + int(floorf(focus->x / WORLD_CHUNK_SIZE)),
            camerachunky = worldfirstchunky + int(floorf(focus->y / WORLD_CHUNK_SIZE)),
            cameraindex = findworldchunk(camerachunkx, camerachunky);
        if(worldchunks.inrange(cameraindex))
        {
            worldchunk &chunk = worldchunks[cameraindex];
            if(!chunk.loading && !chunk.corrupted && chunk.root)
            {
                ivec origin = worldchunkorigin(chunk);
                int tilex = clamp(int(floorf((focus->x - origin.x) / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_COLUMNS) - 1),
                    tiley = clamp(int(floorf((focus->y - origin.y) / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_COLUMNS) - 1);
                int tile = tiley * WORLD_SECTION_COLUMNS + tilex;
                revealworldsectionfromfocus(queue, cameraindex, tile, focussection.z, worldchunksectionfocusfaces(chunk, tile, focussection.z, *focus));
            }
        }
    }

    worldsectionvisibilitydirty = false;
    if(rebuild) worldsectionvisibilityadditions.setsize(0);
    worldsectionvisibilitychunkx = chunkx;
    worldsectionvisibilitychunky = chunky;
    worldsectionvisibilitymaxdist = worldrenderdistance;
    worldsectionvisibilityfocus = focussection;
    worldsectionvisibilitycell = focuscell;

    // Store absolute chunk coordinates, never vector indices: cache pruning and
    // job cancellation can swap chunk slots while this traversal is suspended.
    int visited = 0;
    while(worldsectionvisibilitycursor < queue.length())
    {
        if((SDL_GetPerformanceCounter() - start) * 1000.0 / frequency >= visibilitybudget) break;
        const worldsectionnode node = queue[worldsectionvisibilitycursor++];
        const int index = findworldchunk(node.chunkx, node.chunky);
        if(!worldchunks.inrange(index) || worldchunks[index].loading || worldchunks[index].corrupted || !worldchunks[index].root ||
           !worldchunkinview(worldchunks[index], chunkx, chunky)) continue;
        ++visited;
        loopi(6)
        {
            if(!(node.exits & (1<<i))) continue;
            int neighborindex, neighbortile, neighborsection;
            if(!findworldsectionneighbor(index, node.tile, node.section, directions[i][0], directions[i][1], directions[i][2],
                                         chunkx, chunky, neighborindex, neighbortile, neighborsection))
                continue;
            revealworldsection(queue, neighborindex, neighbortile, neighborsection,
                               worldsectionfacesoverlap(index, node.tile, node.section, i, neighborindex, neighbortile,
                                                        neighborsection) ? 1<<(i^1) : 0);
        }
    }
    if(worldsectionvisibilitycursor == queue.length() && worldsectionvisibilityadditions.empty())
    {
        if(!worldsectionvisibilitypublishing)
        {
            ++worldsectionvisibilitypublication;
            worldsectionvisibilitypublishing = true;
        }
        loopv(worldchunks)
        {
            if((SDL_GetPerformanceCounter() - start) * 1000.0 / frequency >= visibilitybudget) return;
            worldchunk &chunk = worldchunks[i];
            if(chunk.visibilitypublication == worldsectionvisibilitypublication) continue;
            loopj(WORLD_SECTION_LAYERS)
            {
                const uint traversed = chunk.visibilityepoch == worldsectionvisibilityepoch ? chunk.traversedtiles[j] : 0;
                const uint changed = chunk.visibletiles[j] ^ traversed;
                if(!changed) continue;
                chunk.visibletiles[j] = traversed;
                chunk.varesidencydirtytiles[j] |= changed;
                chunk.varesidencydirty = true;
            }
            chunk.visibilitypublication = worldsectionvisibilitypublication;
        }
        resetworldsectionvisibilityqueue();
    }
    else if(worldsectionvisibilitycursor >= 4096 && worldsectionvisibilitycursor >= queue.length() / 2)
    {
        queue.remove(0, worldsectionvisibilitycursor);
        worldsectionvisibilitycursor = 0;
    }
    ZoneValue(visited);
    TracyPlot("Chunks/Pending visibility nodes", int64_t(queue.length() - worldsectionvisibilitycursor));
}

static int worldchunksectionviewclass(const worldchunk &chunk, int tile, int section)
{
    if(!camera1 || !viewfrustumvalid()) return 0;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec bbmin = ivec(worldchunkorigin(chunk)).add(ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE)),
         bbmax = ivec(bbmin).add(WORLD_SECTION_SIZE);
    if(isvisiblebb(bbmin, ivec(bbmax).sub(bbmin)) < VFC_FOGGED) return 2;

    // Keep one reachable section beyond the current frustum ready. This is the
    // small mining/movement margin; it must not turn the entire connected air
    // component into render data.
    const int expansion = WORLD_SECTION_PREFETCH_MARGIN * WORLD_SECTION_SIZE;
    bbmin.sub(expansion);
    bbmax.add(expansion);
    return isvisiblebb(bbmin, ivec(bbmax).sub(bbmin)) < VFC_FOGGED ? 1 : 0;
}

static bool worldsectionviewchanged()
{
    extern plane vfcP[5];
    extern float vfcDfog;
    static plane previous[5];
    static float previousfog = 0;
    static bool previousvalid = false;
    const bool valid = camera1 && viewfrustumvalid();
    const bool changed = valid != previousvalid || (valid && (memcmp(previous, vfcP, sizeof(previous)) || previousfog != vfcDfog));
    if(valid)
    {
        memcpy(previous, vfcP, sizeof(previous));
        previousfog = vfcDfog;
    }
    previousvalid = valid;
    return changed;
}

static bool worldchunksectioninteriorvisible(const worldchunk &chunk, int tile, int section, const ivec &playersection)
{
    // Keep only the immediate mining/movement neighbourhood unconditionally.
    // Distant interiors need both a portal path and a view/prefetch intersection.
    if(worldchunksectionnearplayer(chunk, tile, section, min(chunkinteriorradius, int(WORLD_SECTION_PREFETCH_MARGIN)), playersection))
        return true;
    return (chunk.visibletiles[section] & (1U << tile)) && worldchunksectionviewclass(chunk, tile, section) > 0;
}

static bool worldsectionwithinrenderdistance(const ivec &origin, const vec *focus)
{
    if(!focus) return true;
    const int x = int(floorf(focus->x / WORLD_SECTION_SIZE)) * WORLD_SECTION_SIZE,
              y = int(floorf(focus->y / WORLD_SECTION_SIZE)) * WORLD_SECTION_SIZE,
              range = worldrenderdistance * WORLD_BLOCK_SIZE;
    return origin.x < x + WORLD_SECTION_SIZE + range && origin.x + WORLD_SECTION_SIZE > x - range &&
           origin.y < y + WORLD_SECTION_SIZE + range && origin.y + WORLD_SECTION_SIZE > y - range;
}

bool worldsectionvavisible(const ivec &origin, int size)
{
    if(size != WORLD_SECTION_SIZE || worldchunks.empty() || drawfullchunk) return true;
    if(!worldsectionwithinrenderdistance(origin, camera1 ? &camera1->o : player ? &player->o : NULL)) return false;
    const worldsectionowner *owner = worldsectionowners.access(worldchunkvaupdatekey(origin));
    if(!owner) return false;
    const int index = findworldchunk(owner->chunkx, owner->chunky);
    if(!worldchunks.inrange(index)) return false;
    const worldchunk &chunk = worldchunks[index];
    if(chunk.renderdata.flags[owner->section][owner->tile] & (SECTION_EXTERIOR | SECTION_WATER)) return true;
    const vec *focus = player ? &player->o : camera1 ? &camera1->o : NULL;
    if(!focus) return true;
    const ivec playersection(int(floorf(focus->x / WORLD_SECTION_SIZE)), int(floorf(focus->y / WORLD_SECTION_SIZE)),
                             int(floorf(focus->z / WORLD_SECTION_SIZE)));
    return worldchunksectioninteriorvisible(chunk, owner->tile, owner->section, playersection);
}

extern int csmfarplane;

static bool worldchunksectionwithinresidentrange(const worldchunk &chunk, int tile, int section, const vec *focus, float residentrange)
{
    if(!focus) return true;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec bbmin = ivec(worldchunkorigin(chunk)).add(ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE)),
         bbmax = ivec(bbmin).add(WORLD_SECTION_SIZE);
    if(!worldsectionwithinrenderdistance(bbmin, focus)) return false;
    // Residency is invalidated at section crossings. Measure from the entire
    // focus section so movement within it cannot expose an unrequested border.
    const ivec focusmin(int(floorf(focus->x / WORLD_SECTION_SIZE)) * WORLD_SECTION_SIZE,
                         int(floorf(focus->y / WORLD_SECTION_SIZE)) * WORLD_SECTION_SIZE,
                         int(floorf(focus->z / WORLD_SECTION_SIZE)) * WORLD_SECTION_SIZE);
    const vec nearest(clamp(float(bbmin.x), float(focusmin.x), float(focusmin.x + WORLD_SECTION_SIZE)),
                       clamp(float(bbmin.y), float(focusmin.y), float(focusmin.y + WORLD_SECTION_SIZE)),
                       clamp(float(bbmin.z), float(focusmin.z), float(focusmin.z + WORLD_SECTION_SIZE)));
    return nearest.dist_to_bb(bbmin, bbmax) <= residentrange;
}

static int worldchunksectiongeometrymask(const worldchunk &chunk, int tile, int section)
{
    const uchar flags = chunk.renderdata.flags[section][tile];
    if(flags&SECTION_NO_RENDER) return 0;
    int mask = 0;
    if(flags&(SECTION_EXTERIOR | SECTION_WATER)) mask |= 1 << WORLD_VA_EXTERIOR;
    if(flags&SECTION_INTERIOR) mask |= 1 << WORLD_VA_INTERIOR;
    return mask;
}

static int worldsectionrenderflagsat(const vec &focus)
{
    const int chunkx = worldfirstchunkx + int(floorf(focus.x / WORLD_CHUNK_SIZE)),
              chunky = worldfirstchunky + int(floorf(focus.y / WORLD_CHUNK_SIZE)),
              chunkindex = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(chunkindex)) return 0;
    const worldchunk &chunk = worldchunks[chunkindex];
    if(chunk.loading || chunk.corrupted || !chunk.root) return 0;
    const ivec chunkorigin = worldchunkorigin(chunk);
    const int tilex = clamp(int(floorf((focus.x - chunkorigin.x) / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_COLUMNS) - 1),
              tiley = clamp(int(floorf((focus.y - chunkorigin.y) / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_COLUMNS) - 1),
              tile = tiley * WORLD_SECTION_COLUMNS + tilex,
              section = clamp(int(floorf(focus.z / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_LAYERS) - 1);
    return chunk.renderdata.flags[section][tile];
}

float worldplayercavefactor()
{
    if(!player) return 0.0f;
    const int flags = worldsectionrenderflagsat(player->o);
    if(!(flags&SECTION_INTERIOR) || flags&(SECTION_EXTERIOR | SECTION_WATER | SECTION_CAVE_ENTRANCE)) return 0.0f;

    // A sealed interior supplies half the cave confidence. Depth below sea
    // level supplies the other half, reaching full confidence at -64 blocks
    // (1,024 engine units) and remaining clamped below that elevation.
    const float depth = clamp(-worldpositionheight(player->o.z) / 64.0f, 0.0f, 1.0f);
    return 0.5f + 0.5f * depth;
}

static int worldplayersectionrenderflags(bool &nearentrance)
{
    static const int directions[][3] =
    {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };
    nearentrance = false;
    const vec *focus = camera1 ? &camera1->o : player ? &player->o : NULL;
    if(!focus) return 0;
    const int flags = worldsectionrenderflagsat(*focus);
    nearentrance = (flags&SECTION_CAVE_ENTRANCE) != 0;
    loopi(6) if(!nearentrance)
    {
        vec neighbor(*focus);
        neighbor.x += directions[i][0] * WORLD_SECTION_SIZE;
        neighbor.y += directions[i][1] * WORLD_SECTION_SIZE;
        neighbor.z += directions[i][2] * WORLD_SECTION_SIZE;
        nearentrance = (worldsectionrenderflagsat(neighbor)&SECTION_CAVE_ENTRANCE) != 0;
    }
    return flags;
}

static int worldchunksectionwantedmask(worldchunk &chunk, int tile, int section, int available, bool requiresvoxel,
                                       const ivec &playersection, const vec *viewfocus, float residentrange)
{
    if(!available) return 0;
    // Heightfield LODs replace the exterior only; they contain no cave walls.
    if(!requiresvoxel) available &= 1 << WORLD_VA_INTERIOR;
    if(drawfullchunk) return available;
    int wanted = 0;
    const uint tilebit = 1U << tile;
    if(worldchunksectionwithinresidentrange(chunk, tile, section, viewfocus, residentrange))
    {
        if(chunk.visibletiles[section] & tilebit) wanted |= available & (1 << WORLD_VA_EXTERIOR);
        if(worldchunksectioninteriorvisible(chunk, tile, section, playersection)) wanted |= available & (1 << WORLD_VA_INTERIOR);
    }
    return wanted;
}

static void updateworldsectionresidencywanted()
{
    static ivec lastviewsection(INT_MIN, INT_MIN, INT_MIN), lastplayersection(INT_MIN, INT_MIN, INT_MIN);
    static int lastworldrenderdistance = -1, lastinteriorradius = -1, lastresidentrange = -1, lastdrawfullchunk = -1;
    static bool lastcavemode = false, lastentrancemode = false, initialized = false;

    const vec *viewfocus = camera1 ? &camera1->o : player ? &player->o : NULL,
              *playerfocus = player ? &player->o : camera1 ? &camera1->o : NULL;
    ivec viewsection(INT_MIN, INT_MIN, INT_MIN), playersection(INT_MIN, INT_MIN, INT_MIN);
    if(viewfocus)
        viewsection = ivec(int(floorf(viewfocus->x / WORLD_SECTION_SIZE)), int(floorf(viewfocus->y / WORLD_SECTION_SIZE)),
                           clamp(int(floorf(viewfocus->z / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_LAYERS) - 1));
    if(playerfocus)
        playersection = ivec(int(floorf(playerfocus->x / WORLD_SECTION_SIZE)), int(floorf(playerfocus->y / WORLD_SECTION_SIZE)),
                             clamp(int(floorf(playerfocus->z / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_LAYERS) - 1));

    bool entrancemode;
    const int playerflags = worldplayersectionrenderflags(entrancemode);
    const bool cavemode = (playerflags&SECTION_INTERIOR) && !(playerflags&(SECTION_EXTERIOR | SECTION_WATER));
    const int residentrange = int(ceilf(max(calcfogcull(), float(csmfarplane))));
    const bool viewchanged = worldsectionviewchanged();
    const bool globaldirty = !initialized || viewsection != lastviewsection || playersection != lastplayersection || cavemode != lastcavemode ||
                             entrancemode != lastentrancemode || worldrenderdistance != lastworldrenderdistance ||
                             chunkinteriorradius != lastinteriorradius || residentrange != lastresidentrange || drawfullchunk != lastdrawfullchunk;
    initialized = true;
    lastviewsection = viewsection;
    lastplayersection = playersection;
    lastcavemode = cavemode;
    lastentrancemode = entrancemode;
    lastworldrenderdistance = worldrenderdistance;
    lastinteriorradius = chunkinteriorradius;
    lastresidentrange = residentrange;
    lastdrawfullchunk = drawfullchunk;

    // Epochs invalidate lazily: movement does not visit every section before
    // the timed work starts. Dirty bits survive an interrupted chunk.
    static int cursor = 0;
    if(globaldirty) ++worldchunkresidencyepoch;
    if(viewchanged) ++worldchunkresidencyviewepoch;
    const Uint64 deadline = SDL_GetPerformanceCounter() + Uint64(worldchunkstreamremaining() * 0.3 *
                                                               SDL_GetPerformanceFrequency() / 1000.0);
    ZoneScopedN("Chunks/Update wanted VA residency");
    int updatedchunks = 0, updatedsections = 0;
    loop(scanned, worldchunks.length())
    {
        if(SDL_GetPerformanceCounter() >= deadline) break;
        if(cursor >= worldchunks.length()) cursor = 0;
        worldchunk &chunk = worldchunks[cursor];
        if(chunk.loading || chunk.corrupted || !chunk.root) { ++cursor; continue; }
        if(chunk.varesidencylod < 0)
        {
            chunk.varesidencylod = worldlodrequiresvoxel(chunk) ? 1 : 0;
            dirtyallworldchunkvaresidency(chunk);
        }
        if(chunk.residencyepoch != worldchunkresidencyepoch)
        {
            dirtyallworldchunkvaresidency(chunk);
            chunk.residencyepoch = worldchunkresidencyepoch;
        }
        if(chunk.residencyviewepoch != worldchunkresidencyviewepoch)
        {
            loopj(WORLD_SECTION_LAYERS) chunk.varesidencydirtytiles[j] |= chunk.visibletiles[j];
            chunk.varesidencydirty = true;
            chunk.residencyviewepoch = worldchunkresidencyviewepoch;
        }
        if(!chunk.varesidencydirty) { ++cursor; continue; }
        loop(visited, WORLD_SECTION_LAYERS * WORLD_SECTION_TILES)
        {
            if(SDL_GetPerformanceCounter() >= deadline) { ++cursor; return; }
            const int section = chunk.residencycursor / WORLD_SECTION_TILES, tile = chunk.residencycursor % WORLD_SECTION_TILES;
            chunk.residencycursor = (chunk.residencycursor + 1) % (WORLD_SECTION_LAYERS * WORLD_SECTION_TILES);
            if(chunk.varesidencydirtytiles[section] & (1U << tile))
            {
                chunk.varesidencydirtytiles[section] &= ~(1U << tile);
                worldsectionvaresidency &residency = chunk.varesidency[section][tile];
                const uchar flags = chunk.renderdata.flags[section][tile];
                bool waspending = false;
                loopk(WORLD_VA_GEOMETRY_COUNT) if(residency.state[k] == PENDING_BUILD) { waspending = true; break; }
                if(flags&SECTION_NO_RENDER)
                {
                    if(worldsectionvaactive(residency)) worldvanorenderskipsframe++;
                    if(waspending) removeworldchunkvapendingbuild(chunk, tile, section);
                    resetworldsectionvaresidency(residency);
                    updatedsections++;
                    continue;
                }
                const int available = worldchunksectiongeometrymask(chunk, tile, section),
                          wanted = worldchunksectionwantedmask(chunk, tile, section, available, chunk.varesidencylod != 0,
                                                              playersection, viewfocus, float(residentrange));
                loopk(WORLD_VA_GEOMETRY_COUNT)
                {
                    const bool requested = (wanted & (1 << k)) != 0;
                    if(requested)
                    {
                        if(residency.state[k] == EVICTABLE) setworldsectionvaresidencystate(residency, k, RESIDENT);
                        else if(residency.state[k] == NOT_RESIDENT)
                            setworldsectionvaresidencystate(residency, k,
                                                            worldsectionvaphysicalresident(residency) ? RESIDENT : PENDING_BUILD);
                    }
                    else if(residency.state[k] == RESIDENT) setworldsectionvaresidencystate(residency, k, EVICTABLE);
                    else if(residency.state[k] == PENDING_BUILD) setworldsectionvaresidencystate(residency, k, NOT_RESIDENT);
                }
                bool pending = false;
                loopk(WORLD_VA_GEOMETRY_COUNT) if(residency.state[k] == PENDING_BUILD) { pending = true; break; }
                if(pending && !waspending) queueworldchunkvapendingbuild(chunk, tile, section);
                else if(!pending && waspending) removeworldchunkvapendingbuild(chunk, tile, section);
                updatedsections++;
            }
        }
        chunk.varesidencydirty = false;
        ++cursor;
        updatedchunks++;
    }
    ZoneValue(updatedsections);
    TracyPlot("Chunks/Cave residency mode", int64_t(cavemode ? 1 : 0));
    TracyPlot("Chunks/Cave entrance residency mode", int64_t(entrancemode ? 1 : 0));
    TracyPlot("Chunks/Residency chunks updated", int64_t(updatedchunks));
    TracyPlot("Chunks/Residency sections updated", int64_t(updatedsections));
}

static long long worldchunksectionmountscore(const worldchunk &chunk, int tile, int section)
{
    static const long long tierstride = 1LL << 60;
    const vec &focus = camera1 ? camera1->o : player ? player->o : vec(0, 0, 0);
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec bbmin = ivec(worldchunkorigin(chunk)).add(ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE)),
         bbmax = ivec(bbmin).add(WORLD_SECTION_SIZE);
    long long distance = 0;
    loopi(3)
    {
        double delta = focus[i] < bbmin[i] ? bbmin[i] - focus[i] : focus[i] > bbmax[i] ? focus[i] - bbmax[i] : 0;
        distance += static_cast<long long>(delta * delta);
    }
    const bool containscamera = focus.x >= bbmin.x && focus.x < bbmax.x && focus.y >= bbmin.y && focus.y < bbmax.y &&
                                focus.z >= bbmin.z && focus.z < bbmax.z;
    const int tier = containscamera ? 0 : worldchunksectionviewclass(chunk, tile, section) == 2 ? 1 : 2;
    return tier * tierstride + min(distance, tierstride - 1);
}

struct worldsectioncandidate
{
    int chunkindex, tile, section;
    long long score;
};

static int findworldchunkmountsections(int chunkx, int chunky, worldsectioncandidate *candidates, int maxcandidates)
{
    ZoneScopedN("Chunks/Select render sections");
    if(maxcandidates <= 0) return 0;
    int numcandidates = 0;
    static int cursor = 0;
    const Uint64 deadline = SDL_GetPerformanceCounter() + Uint64(worldchunkstreamremaining() * 0.25 *
                                                               SDL_GetPerformanceFrequency() / 1000.0);
    int scanned = 0, count = worldchunkvapendingbuilds.length();
    while(scanned++ < count && !worldchunkvapendingbuilds.empty() && SDL_GetPerformanceCounter() < deadline)
    {
        if(cursor >= worldchunkvapendingbuilds.length()) cursor = 0;
        const int i = cursor++;
        const ivec pending = worldchunkvapendingbuilds[i];
        const int section = pending.z / WORLD_SECTION_TILES, tile = pending.z % WORLD_SECTION_TILES,
                  chunkindex = findworldchunk(pending.x, pending.y);
        bool valid = worldchunks.inrange(chunkindex);
        if(valid)
        {
            const worldsectionvaresidency &residency = worldchunks[chunkindex].varesidency[section][tile];
            valid = false;
            loopj(WORLD_VA_GEOMETRY_COUNT) if(residency.state[j] == PENDING_BUILD) { valid = true; break; }
        }
        if(!valid)
        {
            worldchunkvapendingbuildset.remove(worldchunksectionvakey(pending.x, pending.y, tile, section));
            worldchunkvapendingbuilds.removeunordered(i);
            cursor = i;
            continue;
        }
        worldchunk &chunk = worldchunks[chunkindex];
        if(chunk.loading || chunk.corrupted || !chunk.root || !worldchunkinview(chunk, chunkx, chunky)) continue;
        const long long score = worldchunksectionmountscore(chunk, tile, section);
        int insert = numcandidates;
        while(insert > 0 && score < candidates[insert - 1].score) --insert;
        if(insert >= maxcandidates) continue;
        const int newcount = min(numcandidates + 1, maxcandidates);
        for(int move = newcount - 1; move > insert; --move) candidates[move] = candidates[move - 1];
        candidates[insert].chunkindex = chunkindex;
        candidates[insert].tile = tile;
        candidates[insert].section = section;
        candidates[insert].score = score;
        numcandidates = newcount;
    }
    ZoneValue(numcandidates);
    return numcandidates;
}

static bool findworldchunkunloadcolumn(int chunkx, int chunky, int &chunkindex, int &tile)
{
    static int cursor = 0;
    chunkindex = tile = -1;
    const Uint64 deadline = SDL_GetPerformanceCounter() + min(worldchunkcleanupremaining,
        Uint64(worldchunkstreamremaining() * SDL_GetPerformanceFrequency() / 1000.0));
    loop(scanned, worldchunks.length())
    {
        if(SDL_GetPerformanceCounter() >= deadline) break;
        if(cursor >= worldchunks.length()) cursor = 0;
        const int i = cursor++;
        worldchunk &chunk = worldchunks[i];
        if(worldchunkinview(chunk, chunkx, chunky) && !chunk.retiregeometry)
        {
            chunk.evictsince = -1;
            continue;
        }
        if(chunk.retiregeometry && (worldlodrequiresvoxel(chunk) || worldchunkneedsinterior(chunk)))
        {
            chunk.retiregeometry = false;
            chunk.evictsince = -1;
            continue;
        }
        if(chunk.evictsince < 0) chunk.evictsince = totalmillis;
        if(totalmillis - chunk.evictsince < chunkevictgrace || !worldchunkmounted(chunk)) continue;
        loopj(WORLD_SECTION_TILES) loopk(WORLD_SECTION_LAYERS) if(chunk.mountedtiles[k] & (1U << j))
        {
            chunkindex = i;
            tile = j;
            return true;
        }
    }
    return chunkindex >= 0;
}

static int processworldchunkvaupdates()
{
    int pending = worldchunkvaupdates.length();
    if(pending <= 0) return 0;

    int completed = 0;
    static int cursor = 0;
    int scanned = 0;
    while(scanned++ < pending && !worldchunkvaupdates.empty() && worldchunkstreamremaining() > 0)
    {
        if(cursor >= worldchunkvaupdates.length()) cursor = 0;
        const int i = cursor++;
        const int key = worldchunkvaupdates[i], rowsize = WORLD_RUNTIME_SIZE / WORLD_SECTION_SIZE;
        const ivec origin((key % rowsize) * WORLD_SECTION_SIZE, ((key / rowsize) % rowsize) * WORLD_SECTION_SIZE,
                          (key / (rowsize * rowsize)) * WORLD_SECTION_SIZE);
        if(streaminggeometrypending(origin)) continue;
        worldsectionowner *owner = worldsectionowners.access(key);
        const int chunkindex = owner ? findworldchunk(owner->chunkx, owner->chunky) : -1;
        if(worldchunks.inrange(chunkindex))
        {
            worldsectionvaresidency &residency = worldchunks[chunkindex].varesidency[owner->section][owner->tile];
            loopj(WORLD_VA_GEOMETRY_COUNT) if(residency.state[j] == PENDING_UPLOAD)
                setworldsectionvaresidencystate(residency, j, RESIDENT);
        }
        worldchunkvaupdateset.remove(key);
        worldchunkvaupdates.removeunordered(i);
        cursor = i;
        ++completed;
    }
    TracyPlot("Chunks/Pending VA sections", int64_t(worldchunkvaupdates.length()));
    return completed;
}

static int processworldchunkchanges(int chunkx, int chunky)
{
    ZoneScopedN("Chunks/Process geometry changes");
    ZoneTextF("focus %d_%d", chunkx, chunky);
    resetworldvauploadstats();
    worldvaevictionsframe = worldvanorenderskipsframe = 0;
    if(worldchunkstreamremaining() > 0) updateworldsectionvisibility(chunkx, chunky);
    if(worldchunkstreamremaining() > 0) updateworldsectionresidencywanted();
    Uint64 phasestart = SDL_GetPerformanceCounter();
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    int changedcolumns = 0, unloaded = 0, unloadedsections = 0,
        unloadtarget = WORLD_MAX_COLUMN_CHANGES,
        cleanupstagelimit = chunkvastagelimit;

    // Cleanup only unmounts chunks outside worldrenderdistance. Section VAs inside
    // that radius remain cached across cave/exterior mode changes.
    {
        ZoneScopedN("Chunks/Unload columns");
        while(unloaded < unloadtarget && unloadedsections < cleanupstagelimit)
        {
            if(SDL_GetPerformanceCounter() - phasestart >= worldchunkcleanupremaining || worldchunkstreamremaining() <= 0) break;
            int chunkindex, tile;
            if(!findworldchunkunloadcolumn(chunkx, chunky, chunkindex, tile)) break;
            worldchunk &chunk = worldchunks[chunkindex];
            int sections[WORLD_MAX_SECTION_BATCH],
                numsections = unmountworldchunkcolumnbatch(chunk, tile, sections,
                    min(chunksectionbatch, cleanupstagelimit - unloadedsections));
            if(!numsections) break;
            queueworldchunksectionupdates(chunk, tile, sections, numsections);
            unloadedsections += numsections;
            unloaded++;
            changedcolumns++;
        }
        ZoneValue(unloaded);
    }
    const Uint64 cleanuptime = SDL_GetPerformanceCounter() - phasestart;
    worldchunkcleanupremaining -= min(worldchunkcleanupremaining, cleanuptime);

    phasestart = SDL_GetPerformanceCounter();
    int mounted = 0, mountedsections = 0, mounttarget = WORLD_MAX_COLUMN_CHANGES,
        publishstagelimit = max(chunkvastagelimit - worldchunkvaupdates.length(), 0);
    {
        ZoneScopedN("Chunks/Mount render sections");
        worldsectioncandidate candidates[WORLD_MAX_SECTION_BATCH];
        int numcandidates = findworldchunkmountsections(chunkx, chunky, candidates,
                                                        min(publishstagelimit,
                                                            int(WORLD_MAX_SECTION_BATCH)));
        loopi(numcandidates)
        {
            double elapsed = (SDL_GetPerformanceCounter() - phasestart) * 1000.0 / frequency;
            int bytes = 0, vertices = 0;
            getworldvauploadstats(bytes, vertices);
            if(elapsed >= chunkpublishbudget || worldchunkstreamremaining() <= 0 || bytes >= chunkvauploadkb * 1024) break;
            worldsectioncandidate &candidate = candidates[i];
            worldchunk &chunk = worldchunks[candidate.chunkindex];
            mountworldchunktile(chunk, candidate.section, candidate.tile);
            if(!(chunk.mountedtiles[candidate.section] & (1U << candidate.tile))) continue;
            worldsectionvaresidency &residency = chunk.varesidency[candidate.section][candidate.tile];
            bool queued = false;
            loopj(WORLD_VA_GEOMETRY_COUNT) if(residency.state[j] == PENDING_BUILD)
            {
                setworldsectionvaresidencystate(residency, j, PENDING_UPLOAD);
                queued = true;
            }
            if(!queued) continue;
            removeworldchunkvapendingbuild(chunk, candidate.tile, candidate.section);
            queueworldchunksectionupdates(chunk, candidate.tile, &candidate.section, 1);
            mountedsections++;
            mounted++;
            changedcolumns++;
            if(mounted >= mounttarget) break;
        }
        ZoneValue(mountedsections);
    }

    // Safety mounts, cleanup borders, and render mounts share ONE geometry
    // slice. Admission is bounded separately so deferred work cannot grow
    // without limit when the renderer is slower than section selection.
    const double remainingbudget = max(chunkpublishbudget - (SDL_GetPerformanceCounter() - phasestart) * 1000.0 / frequency, 0.0);
    processstreaminggeometry(min(remainingbudget, worldchunkstreamremaining() * 0.9), chunkvauploadkb * 1024);
    processworldchunkvaupdates();
    int uploadedbytes = 0, uploadedvertices = 0;
    getworldvauploadstats(uploadedbytes, uploadedvertices);
    TracyPlot("Chunks/Resident exterior VAs", int64_t(worldvaresidentcounts[WORLD_VA_EXTERIOR]));
    TracyPlot("Chunks/Resident interior VAs", int64_t(worldvaresidentcounts[WORLD_VA_INTERIOR]));
    TracyPlot("Chunks/Pending VA builds", int64_t(worldvapendingbuildcount));
    TracyPlot("Chunks/Pending VA uploads", int64_t(worldvapendinguploadcount));
    TracyPlot("Chunks/VA evictions", int64_t(worldvaevictionsframe));
    TracyPlot("Chunks/NO_RENDER skips", int64_t(worldvanorenderskipsframe));
    TracyPlot("Chunks/VA uploaded bytes", int64_t(uploadedbytes));
    TracyPlot("Chunks/VA uploaded vertices", int64_t(uploadedvertices));
    return changedcolumns;
}


#endif
