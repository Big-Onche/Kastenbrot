// worldvisibility.cpp: streamed section visibility, portals, and VA scheduling

#ifdef WORLDIO_MODULE_IMPLEMENTATION

static void invalidateworldsectionvisibility()
{
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
            int state = classifyworldsection(lookupworldchunkrootcube(static_cast<const cube *>(job.root), pos, WORLD_SECTION_SIZE),
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
        state = classifyworldsection(lookupcube(runtimepos, -WORLD_SECTION_SIZE, actualorigin, actualsize), portals, facemasks, focuscell,
                                     &focusfaces);
    }
    else state = classifyworldsection(lookupworldchunkcube(static_cast<const worldchunk &>(chunk), pos, WORLD_SECTION_SIZE), portals,
                                      facemasks, focuscell, &focusfaces);
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

static ivec worldchunkvaupdateorigin(int key)
{
    const int rowsize = WORLD_RUNTIME_SIZE / WORLD_SECTION_SIZE;
    int x = key % rowsize;
    key /= rowsize;
    int y = key % rowsize, z = key / rowsize;
    return ivec(x, y, z).mul(WORLD_SECTION_SIZE);
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
        if(!queueworldchunkvaupdate(center)) continue;

        // Faces can only change inside the moved section or immediately across
        // its boundary. Invalidating all six neighboring sections rebuilt up to
        // seven times the required render data for every streaming operation.
        bbmins[numregions] = ivec(center).sub(1).max(0);
        bbmaxs[numregions] = ivec(center).add(WORLD_SECTION_SIZE + 1).min(
            ivec(worldsize, worldsize, WORLD_MAP_SIZE));
        numregions++;
    }
    if(numregions)
    {
        // Runtime cubes have already moved. Their old parent VAs must be
        // destroyed before another frame can draw them at stale coordinates.
        // Building replacement VAs remains grouped in processworldchunkvaupdates().
        bool oldsuppress = suppressworldchunkdirty;
        suppressworldchunkdirty = true;
        changedstreaming(bbmins, bbmaxs, numregions, false);
        suppressworldchunkdirty = oldsuppress;
    }
    ZoneValue(numsections);
}

static bool worldchunksectionnearplayer(const worldchunk &chunk, int tile, int section, int radius)
{
    if(!player && !camera1) return false;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec origin = ivec(worldchunkorigin(chunk)).add(
        ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE,
             section * WORLD_SECTION_SIZE));
    const vec &focus = player ? player->o : camera1->o;
    int sectionx = origin.x / WORLD_SECTION_SIZE,
        sectiony = origin.y / WORLD_SECTION_SIZE,
        focusx = int(floorf(focus.x / WORLD_SECTION_SIZE)),
        focusy = int(floorf(focus.y / WORLD_SECTION_SIZE)),
        focusz = clamp(int(floorf(focus.z / WORLD_SECTION_SIZE)),
                       0, int(WORLD_SECTION_LAYERS) - 1);
    return abs(sectionx - focusx) <= radius && abs(sectiony - focusy) <= radius &&
           abs(section - focusz) <= radius;
}

struct worldsectionnode
{
    int chunkindex, tile, section;
    uchar exits;

    worldsectionnode(int chunkindex, int tile, int section, uchar exits)
        : chunkindex(chunkindex), tile(tile), section(section), exits(exits) {}
};

static bool findworldsectionneighbor(int chunkindex, int tile, int section, int dx, int dy, int dz, int focusx, int focusy,
                                     int &neighborindex, int &neighbortile, int &neighborsection)
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

static bool worldsectionfacesoverlap(int chunkindex, int tile, int section, int face, int neighborindex, int neighbortile,
                                     int neighborsection)
{
    const uint *facemask = worldchunksectionfacemask(worldchunks[chunkindex], tile, section, face),
               *neighbormask = worldchunksectionfacemask(worldchunks[neighborindex], neighbortile, neighborsection, face^1);
    loopi(WORLD_SECTION_FACE_WORDS) if(facemask[i] & neighbormask[i]) return true;
    return false;
}

static void markworldsectionvisible(worldchunk &chunk, int tile, int section)
{
    const uint tilebit = 1U << tile;
    if(worldchunksectionhascontent(chunk, tile, section)) chunk.visibletiles[section] |= tilebit;
}

static void revealworldsection(vector<worldsectionnode> &queue, int chunkindex, int tile, int section, uchar entrances)
{
    worldchunk &chunk = worldchunks[chunkindex];
    markworldsectionvisible(chunk, tile, section);
    const uchar *portals = worldchunksectionportals(chunk, tile, section);
    uchar exits = 0;
    loopi(WORLD_SECTION_FACE_COUNT) if(entrances & (1<<i)) exits |= portals[i];
    exits &= ~chunk.reachablefaces[section][tile];
    if(!exits) return;
    chunk.reachablefaces[section][tile] |= exits;
    queue.add(worldsectionnode(chunkindex, tile, section, exits));
}

static void revealworldsectionfromfocus(vector<worldsectionnode> &queue, int chunkindex, int tile, int section, uchar exits)
{
    worldchunk &chunk = worldchunks[chunkindex];
    markworldsectionvisible(chunk, tile, section);
    exits &= ~chunk.reachablefaces[section][tile];
    if(!exits) return;
    chunk.reachablefaces[section][tile] |= exits;
    queue.add(worldsectionnode(chunkindex, tile, section, exits));
}

static void updateworldsectionvisibility(int chunkx, int chunky)
{
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
    ivec focussection(INT_MIN, INT_MIN, INT_MIN);
    if(focus)
        focussection = ivec(int(floorf(focus->x / WORLD_SECTION_SIZE)), int(floorf(focus->y / WORLD_SECTION_SIZE)),
                            clamp(int(floorf(focus->z / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_LAYERS) - 1));
    bool focuschanged = focussection != worldsectionvisibilityfocus,
         rebuild = worldsectionvisibilitydirty || chunkx != worldsectionvisibilitychunkx || chunky != worldsectionvisibilitychunky ||
                   maxchunkdist != worldsectionvisibilitymaxdist || focuschanged;
    if(!rebuild && worldsectionvisibilityadditions.empty()) return;

    ZoneScopedN("Chunks/Update dirty section visibility");
    ZoneValue(worldsectionvisibilityadditions.length());
    vector<worldsectionnode> queue;
    if(rebuild)
    {
        ZoneScopedN("Chunks/Rebuild section visibility");
        rebuildworldchunkindices();
        loopv(worldchunks)
        {
            memclear(worldchunks[i].reachablefaces);
            memclear(worldchunks[i].visibletiles);
        }

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
    else
    {
        ZoneScopedN("Chunks/Extend section visibility");
        loopv(worldsectionvisibilityadditions)
        {
            const ivec &added = worldsectionvisibilityadditions[i];
            int index = findworldchunk(added.x, added.y);
            if(!worldchunks.inrange(index)) continue;
            worldchunk &chunk = worldchunks[index];
            if(chunk.loading || chunk.corrupted || !chunk.root || !worldchunkinview(chunk, chunkx, chunky)) continue;
            if(!focus) loopj(WORLD_SECTION_TILES) revealworldsection(queue, index, j, WORLD_SECTION_LAYERS - 1, 1<<5);

            // A newly published chunk may connect to an already reachable cave
            // through a side face, even when it is sealed from the sky.
            loopj(WORLD_SECTION_TILES) loopk(WORLD_SECTION_LAYERS)
            {
                loopl(6)
                {
                    int neighborindex, neighbortile, neighborsection;
                    if(!findworldsectionneighbor(index, j, k, directions[l][0], directions[l][1], directions[l][2], chunkx, chunky,
                                                 neighborindex, neighbortile, neighborsection))
                        continue;
                    const worldchunk &neighbor = worldchunks[neighborindex];
                    if(!(neighbor.reachablefaces[neighborsection][neighbortile] & (1<<(l^1)))) continue;
                    revealworldsection(queue, index, j, k,
                                       worldsectionfacesoverlap(index, j, k, l, neighborindex, neighbortile, neighborsection) ? 1<<l : 0);
                }
            }
        }
    }

    // A sealed cave is not connected to outside air, so explicitly seed the
    // camera's own section as a second visibility region.
    if(focus)
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
                revealworldsectionfromfocus(queue, cameraindex, tile, focussection.z,
                                            worldchunksectionfocusfaces(chunk, tile, focussection.z, *focus));
            }
        }
    }

    for(int pos = 0; pos < queue.length(); ++pos)
    {
        const worldsectionnode node = queue[pos];
        loopi(6)
        {
            if(!(node.exits & (1<<i))) continue;
            int neighborindex, neighbortile, neighborsection;
            if(!findworldsectionneighbor(node.chunkindex, node.tile, node.section, directions[i][0], directions[i][1], directions[i][2],
                                         chunkx, chunky, neighborindex, neighbortile, neighborsection))
                continue;
            revealworldsection(queue, neighborindex, neighbortile, neighborsection,
                               worldsectionfacesoverlap(node.chunkindex, node.tile, node.section, i, neighborindex, neighbortile,
                                                        neighborsection) ? 1<<(i^1) : 0);
        }
    }
    worldsectionvisibilitydirty = false;
    worldsectionvisibilityadditions.setsize(0);
    worldsectionvisibilitychunkx = chunkx;
    worldsectionvisibilitychunky = chunky;
    worldsectionvisibilitymaxdist = maxchunkdist;
    worldsectionvisibilityfocus = focussection;
    ZoneValue(queue.length());
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

static bool worldchunksectionrequired(worldchunk &chunk, int tile, int section, int playerradius)
{
    if(drawfullchunk || worldchunksectionnearplayer(chunk, tile, section, playerradius))
        return true;
    const uint tilebit = 1U << tile;
    return (chunk.visibletiles[section] & tilebit) && worldchunksectionviewclass(chunk, tile, section) != 0;
}

extern int csmfarplane;

static bool worldchunksectionwithinresidentrange(const worldchunk &chunk, int tile, int section)
{
    const vec *focus = camera1 ? &camera1->o : player ? &player->o : NULL;
    if(!focus) return true;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec bbmin = ivec(worldchunkorigin(chunk)).add(ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE)),
         bbmax = ivec(bbmin).add(WORLD_SECTION_SIZE);
    return focus->dist_to_bb(bbmin, bbmax) <= max(calcfogcull(), float(csmfarplane));
}

static bool worldchunksectionoccluded(const worldchunk &chunk, int tile, int section)
{
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec origin = ivec(worldchunkorigin(chunk)).add(ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE)),
         actualorigin;
    int actualsize;
    const cube &c = lookupcube(origin, -WORLD_SECTION_SIZE, actualorigin, actualsize);
    return actualorigin == origin && actualsize == WORLD_SECTION_SIZE && c.ext && isvaoccluded(c.ext->va);
}

static bool worldchunksectionresidentrequired(worldchunk &chunk, int tile, int section, int playerradius)
{
    if(drawfullchunk || worldchunksectionnearplayer(chunk, tile, section, playerradius)) return true;
    const uint tilebit = 1U << tile;
    return (chunk.visibletiles[section] & tilebit) && worldchunksectionwithinresidentrange(chunk, tile, section) &&
           !worldchunksectionoccluded(chunk, tile, section);
}

static long long worldchunksectionmountscore(const worldchunk &chunk, int tile, int section)
{
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
    return (worldchunksectionviewclass(chunk, tile, section) == 1 ? 1LL<<60 : 0) + distance;
}

struct worldsectioncandidate
{
    int chunkindex, tile, section;
    long long score;
};

static bool worldchunksectionwithinnearload(const worldchunk &chunk, int tile, int section)
{
    if(!player) return false;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec bbmin = ivec(worldchunkorigin(chunk)).add(ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE)),
         bbmax = ivec(bbmin).add(WORLD_SECTION_SIZE);
    float squareddistance = 0;
    loopi(3)
    {
        float delta = player->o[i] < bbmin[i] ? bbmin[i] - player->o[i] : player->o[i] > bbmax[i] ? player->o[i] - bbmax[i] : 0;
        squareddistance += delta * delta;
    }
    const int distance = WORLD_NEAR_RENDER_BLOCKS * WORLD_BLOCK_SIZE;
    return squareddistance < float(distance * distance);
}

static int findworldchunknearmountsections(int chunkx, int chunky, worldsectioncandidate *candidates, int maxcandidates)
{
    if(!player || maxcandidates <= 0) return 0;
    int focusx = int(floorf(player->o.x / WORLD_SECTION_SIZE)),
        focusy = int(floorf(player->o.y / WORLD_SECTION_SIZE)),
        focusz = clamp(int(floorf(player->o.z / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_LAYERS) - 1),
        numcandidates = 0;

    for(int radius = 0; radius <= WORLD_NEAR_RENDER_SECTION_RADIUS; ++radius)
    for(int dz = -radius; dz <= radius; ++dz)
    for(int dy = -radius; dy <= radius; ++dy)
    for(int dx = -radius; dx <= radius; ++dx)
    {
        if(max(max(abs(dx), abs(dy)), abs(dz)) != radius) continue;
        int sectionx = focusx + dx, sectiony = focusy + dy, section = focusz + dz;
        if(sectionx < 0 || sectionx >= WORLD_RUNTIME_SIZE / WORLD_SECTION_SIZE ||
           sectiony < 0 || sectiony >= WORLD_RUNTIME_SIZE / WORLD_SECTION_SIZE ||
           section < 0 || section >= WORLD_SECTION_LAYERS)
            continue;
        int chunkindex = findworldchunk(worldfirstchunkx + sectionx / WORLD_SECTION_COLUMNS,
                                        worldfirstchunky + sectiony / WORLD_SECTION_COLUMNS);
        if(!worldchunks.inrange(chunkindex)) continue;
        worldchunk &chunk = worldchunks[chunkindex];
        if(chunk.loading || chunk.corrupted || !chunk.root || !worldchunkinview(chunk, chunkx, chunky)) continue;
        int tile = (sectiony % WORLD_SECTION_COLUMNS) * WORLD_SECTION_COLUMNS + sectionx % WORLD_SECTION_COLUMNS;
        if((chunk.mountedtiles[section] & (1U << tile)) || !worldchunksectionwithinnearload(chunk, tile, section) ||
           !worldchunksectionrequired(chunk, tile, section, 1))
            continue;
        worldsectioncandidate &candidate = candidates[numcandidates++];
        candidate.chunkindex = chunkindex;
        candidate.tile = tile;
        candidate.section = section;
        candidate.score = 0;
        if(numcandidates >= maxcandidates) return numcandidates;
    }
    return numcandidates;
}

static int findworldchunkmountsections(int chunkx, int chunky,
                                       worldsectioncandidate *candidates, int maxcandidates)
{
    ZoneScopedN("Chunks/Select render sections");
    if(maxcandidates <= 0) return 0;
    int numcandidates = findworldchunknearmountsections(chunkx, chunky, candidates, maxcandidates);
    if(numcandidates >= maxcandidates)
    {
        ZoneValue(numcandidates);
        return numcandidates;
    }

    const vec *nearfocus = player ? &player->o : camera1 ? &camera1->o : NULL;
    int focusx = nearfocus ? int(floorf(nearfocus->x / WORLD_SECTION_SIZE)) : INT_MIN,
        focusy = nearfocus ? int(floorf(nearfocus->y / WORLD_SECTION_SIZE)) : INT_MIN,
        focusz = nearfocus ? clamp(int(floorf(nearfocus->z / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_LAYERS) - 1) : INT_MIN;
    const uint alltiles = (1U << WORLD_SECTION_TILES) - 1;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || chunk.corrupted || !chunk.root || worldchunkfullymounted(chunk) ||
           !worldchunkinview(chunk, chunkx, chunky))
            continue;
        int chunksectionx = (chunk.x - worldfirstchunkx) * WORLD_SECTION_COLUMNS,
            chunksectiony = (chunk.y - worldfirstchunky) * WORLD_SECTION_COLUMNS;
        loopk(WORLD_SECTION_LAYERS)
        {
            uint pendingtiles = (drawfullchunk ? alltiles : chunk.visibletiles[k]) & ~chunk.mountedtiles[k];
            if(nearfocus && abs(k - focusz) <= 1)
            {
                loopj(WORLD_SECTION_TILES)
                {
                    int sectionx = chunksectionx + j % WORLD_SECTION_COLUMNS,
                        sectiony = chunksectiony + j / WORLD_SECTION_COLUMNS;
                    if(abs(sectionx - focusx) <= 1 && abs(sectiony - focusy) <= 1) pendingtiles |= 1U << j;
                }
                pendingtiles &= ~chunk.mountedtiles[k];
            }
            if(!pendingtiles) continue;
            loopj(WORLD_SECTION_TILES) if(pendingtiles & (1U << j))
            {
                if(!worldchunksectionrequired(chunk, j, k, 1) || worldchunksectionwithinnearload(chunk, j, k)) continue;
                long long score = worldchunksectionmountscore(chunk, j, k);
                int insert = numcandidates;
                while(insert > 0 && score < candidates[insert - 1].score) --insert;
                if(insert >= maxcandidates) continue;
                int newcount = min(numcandidates + 1, maxcandidates);
                for(int move = newcount - 1; move > insert; --move) candidates[move] = candidates[move - 1];
                candidates[insert].chunkindex = i;
                candidates[insert].tile = j;
                candidates[insert].section = k;
                candidates[insert].score = score;
                numcandidates = newcount;
            }
        }
    }
    ZoneValue(numcandidates);
    return numcandidates;
}

static bool findworldchunkunloadcolumn(int chunkx, int chunky, int &chunkindex, int &tile)
{
    int bestdist = -1;
    chunkindex = tile = -1;
    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(!worldchunkmounted(chunk) || worldchunkinview(chunk, chunkx, chunky)) continue;
        int dist = worldchunkdistance(chunk.x, chunk.y, chunkx, chunky);
        if(chunkindex >= 0 && dist <= bestdist) continue;
        loopj(WORLD_SECTION_TILES)
        {
            uint tilebit = 1U << j;
            loopk(WORLD_SECTION_LAYERS) if(chunk.mountedtiles[k] & tilebit)
            {
                bestdist = dist;
                chunkindex = i;
                tile = j;
                break;
            }
            if(chunkindex == i) break;
        }
    }
    return chunkindex >= 0;
}

static int findworldchunkcachedsections(int chunkx, int chunky,
                                        worldsectioncandidate *candidates, int maxcandidates)
{
    if(drawfullchunk || maxcandidates <= 0) return 0;
    ZoneScopedN("Chunks/Select non-visible sections for caching");
    vec focus = camera1 ? camera1->o : player ? player->o : vec(0, 0, 0);
    int focusx = int(floorf(focus.x / WORLD_SECTION_SIZE)),
        focusy = int(floorf(focus.y / WORLD_SECTION_SIZE)),
        focusz = clamp(int(floorf(focus.z / WORLD_SECTION_SIZE)),
                       0, int(WORLD_SECTION_LAYERS) - 1),
        numcandidates = 0;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || chunk.corrupted || !chunk.root || !worldchunkmounted(chunk) ||
           !worldchunkinview(chunk, chunkx, chunky))
            continue;
        loopk(WORLD_SECTION_LAYERS)
        {
            uint mountedtiles = chunk.mountedtiles[k];
            if(!mountedtiles) continue;
            loopj(WORLD_SECTION_TILES) if(mountedtiles & (1U << j))
            {
                int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS,
                    sectionx = (chunk.x - worldfirstchunkx) * WORLD_SECTION_COLUMNS + x,
                    sectiony = (chunk.y - worldfirstchunky) * WORLD_SECTION_COLUMNS + y,
                    dx = sectionx - focusx, dy = sectiony - focusy;
                if(worldchunksectionresidentrequired(chunk, j, k, 2)) continue;
                int dz = k - focusz;
                long long distance = (long long)dx * dx + (long long)dy * dy +
                                     (long long)dz * dz,
                          score = distance;
                int insert = numcandidates;
                while(insert > 0 && score > candidates[insert - 1].score) --insert;
                if(insert >= maxcandidates) continue;
                int newcount = min(numcandidates + 1, maxcandidates);
                for(int move = newcount - 1; move > insert; --move)
                    candidates[move] = candidates[move - 1];
                candidates[insert].chunkindex = i;
                candidates[insert].tile = j;
                candidates[insert].section = k;
                candidates[insert].score = score;
                numcandidates = newcount;
            }
        }
    }
    ZoneValue(numcandidates);
    return numcandidates;
}

static int processworldchunkvaupdates()
{
    int pending = worldchunkvaupdates.length();
    if(pending <= 0) return 0;

    ZoneScopedN("Chunks/Process prioritized VA updates");
    ZoneValue(pending);

    Uint64 start = SDL_GetPerformanceCounter();
    {
        ZoneScopedN("Chunks/Greedy mesh changed sections");
        loopv(worldchunkvaupdates) calcmerges(worldchunkvaupdateorigin(worldchunkvaupdates[i]), WORLD_SECTION_SIZE);
    }
    {
        ZoneScopedN("Chunks/Commit invalidated VA updates");
        ZoneValue(pending);
        commitchanges();
    }
    worldchunkvaupdates.setsize(0);
    worldchunkvaupdateset.clear();
    TracyPlot("Chunks/Pending VA sections", int64_t(0));
    float sample = max(float((SDL_GetPerformanceCounter() - start) * 1000.0 /
                             SDL_GetPerformanceFrequency()) / pending, 0.05f);
    worldchunkvasectionmillis = worldchunkvasectionmillis * 0.75f + sample * 0.25f;
    TracyPlot("Chunks/VA section milliseconds", double(worldchunkvasectionmillis));
    return pending;
}

static int worldchunkstagelimit(int budget)
{
    int estimated = int(float(budget) / max(worldchunkvasectionmillis, 0.05f));
    return min(chunkvastagelimit, max(estimated, 1));
}

static int processworldchunkchanges(int chunkx, int chunky)
{
    ZoneScopedN("Chunks/Process geometry changes");
    ZoneTextF("focus %d_%d", chunkx, chunky);
    updateworldsectionvisibility(chunkx, chunky);
    Uint64 phasestart = SDL_GetPerformanceCounter();
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    int changedcolumns = 0, unloaded = 0, unloadedsections = 0,
        unloadtarget = WORLD_MAX_COLUMN_CHANGES,
        cleanupstagelimit = worldchunkstagelimit(chunkcleanupbudget);

    // Cleanup has its own budget and always runs before publication. This
    // prevents rapid movement from leaving a growing trail of live geometry.
    {
        ZoneScopedN("Chunks/Unload columns");
        while(unloaded < unloadtarget && unloadedsections < cleanupstagelimit)
        {
            double elapsed = (SDL_GetPerformanceCounter() - phasestart) * 1000.0 / frequency;
            if(unloaded && elapsed >= chunkcleanupbudget) break;
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
        if(unloadedsections < cleanupstagelimit)
        {
            worldsectioncandidate candidates[WORLD_MAX_SECTION_BATCH];
            int numcandidates = findworldchunkcachedsections(
                chunkx, chunky, candidates,
                min(cleanupstagelimit - unloadedsections, int(WORLD_MAX_SECTION_BATCH)));
            loopi(numcandidates)
            {
                double elapsed = (SDL_GetPerformanceCounter() - phasestart) * 1000.0 / frequency;
                if(unloaded && elapsed >= chunkcleanupbudget) break;
                worldsectioncandidate &candidate = candidates[i];
                worldchunk &chunk = worldchunks[candidate.chunkindex];
                if(!unmountworldchunktile(chunk, candidate.section, candidate.tile)) continue;
                queueworldchunksectionupdates(chunk, candidate.tile, &candidate.section, 1);
                unloadedsections++;
                unloaded++;
                changedcolumns++;
            }
        }
        ZoneValue(unloaded);
    }

    phasestart = SDL_GetPerformanceCounter();
    int mounted = 0, mountedsections = 0, mounttarget = WORLD_MAX_COLUMN_CHANGES,
        publishstagelimit = worldchunkstagelimit(chunkpublishbudget);
    {
        ZoneScopedN("Chunks/Mount render sections");
        worldsectioncandidate candidates[WORLD_MAX_SECTION_BATCH];
        int numcandidates = findworldchunkmountsections(chunkx, chunky, candidates,
                                                        min(publishstagelimit,
                                                            int(WORLD_MAX_SECTION_BATCH)));
        loopi(numcandidates)
        {
            double elapsed = (SDL_GetPerformanceCounter() - phasestart) * 1000.0 / frequency;
            if(mounted && elapsed >= chunkpublishbudget) break;
            worldsectioncandidate &candidate = candidates[i];
            worldchunk &chunk = worldchunks[candidate.chunkindex];
            if(!mountworldchunktile(chunk, candidate.section, candidate.tile)) continue;
            queueworldchunksectionupdates(chunk, candidate.tile, &candidate.section, 1);
            mountedsections++;
            mounted++;
            changedcolumns++;
            if(mounted >= mounttarget) break;
        }
        ZoneValue(mountedsections);
    }

    processworldchunkvaupdates();
    return changedcolumns;
}


#endif
