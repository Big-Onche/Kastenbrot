// worldstream.cpp: generic infinite-world streaming and chunk residency

#ifdef WORLDIO_MODULE_IMPLEMENTATION

static void invalidateworldsectionvisibility();
static void addworldsectionvisibilitychunk(int x, int y);
static ivec worldorientnormal(int orient);
static int processworldchunkchanges(int chunkx, int chunky);
static void mountworldchunksafetyregion(int chunkx, int chunky, bool updategeometry = true);
static int worldcubesectionstate(const cube &c);
static void setworldchunksectioncontent(worldchunk &chunk, int tile, int section, bool content);
static void setworldchunksectionopaque(worldchunk &chunk, int tile, int section, bool opaque);
static void queueworldchunksectionupdates(const worldchunk &chunk, int tile, const int *sections, int numsections);
static int processworldchunkvaupdates();
static bool queueworldchunksave(worldchunk &chunk);
static int processworldchunksaveresults();
static bool flushworldchunksaves();
static int queueworldchunk(int x, int y);
static int processworldchunkresults();
static int acquireworldchunkblocking(int x, int y, int &generated);

static bool worldchunkmounted(const worldchunk &chunk);
static int worldchunkvaupdatekey(const ivec &origin);

VARP(chunkremip, 0, 1, 1); // optional CPU-for-memory octree collapse on generation/load

worldchunkjob::worldchunkjob(int x, int y, uint epoch, uint request, const char *folder)
    : x(x), y(y), families(0), optimized(0), epoch(epoch), request(request), snapshotrevision(0), remip(chunkremip != 0),
      leavesalpha(::leavesalpha != 0), sectionstatesready(false), checksnapshot(folder && folder[0]), snapshotplayeredited(false),
      snapshotresult(WORLD_SNAPSHOT_MISSING), root(NULL), generation(NULL)
{
    memclear(contenttiles);
    memclear(opaquetiles);
    memclear(portals);
    memclear(portalcellmasks);
    SDL_AtomicSet(&cancelled, 0);
    copystring(this->folder, folder ? folder : "");
    snapshoterror[0] = '\0';
    generation = game::createworldgeneration(true, remip, &cancelled);
}

worldchunkjob::~worldchunkjob()
{
    game::destroyworldgeneration(generation);
}

static vector<worldchunk> worldchunks;
static vector<worldscatterinstance> reconstructedworldscatter;
static worldsectionrenderdata reconstructedworldrenderdata;
static vector<worldchunkjob *> worldchunkjobs, worldchunkactivejobs, worldchunkresults;
static vector<worldchunksavejob *> worldchunksavejobs, worldchunksaveactivejobs, worldchunksaveresults;
static string worldfolder = "";

static bool saveworldchunksnapshots()
{
    if(!game::islocalworld() || !worldfolder[0])
    {
        conoutf(CON_ERROR, "authoritative chunk saving is only available for a loaded local world");
        return false;
    }
    int queued = 0;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root) continue;
        if(chunk.saving) continue;
        if(!queueworldchunksave(chunk)) return false;
        ++queued;
    }
    conoutf("queued %d authoritative chunk snapshots for %s", queued, worldfolder);
    return true;
}
static int activeworldchunk = -1;
static int worldfirstchunkx = 0, worldfirstchunky = 0;
static int lastplayerchunkx = INT_MIN, lastplayerchunky = INT_MIN, lastchunkdist = -1;
static bool rebuildingworldchunks = false;
static bool suppressworldchunkdirty = false;
static vector<SDL_Thread *> worldchunkworkers;
static SDL_mutex *worldchunkmutex = NULL;
static SDL_cond *worldchunkcond = NULL;
static bool stopworldchunkthread = false;
static bool flushingworldchunksaves = false, worldchunksavefailure = false;
static uint worldchunkepoch = 1;
static uint worldchunkrequest = 1;
static int lastworldchunkpublish = -1;
static bool stopworldchunkgeneration = false;
static int worldchunkfocusx = 0, worldchunkfocusy = 0;
static int worldchunkaheadx = 0, worldchunkaheady = 0;
static int worldchunkviewx = 0, worldchunkviewy = 0;
static double lastworldchunkposx = 0, lastworldchunkposy = 0;
static float worldchunkvelocityx = 0, worldchunkvelocityy = 0;
static int lastworldchunkmotion = -1;
static vector<int> worldchunkvaupdates;
static hashset<int> worldchunkvaupdateset(1<<14);
static vector<ivec> worldchunkvapendingbuilds;
static hashset<int> worldchunkvapendingbuildset(1<<14);
static hashtable<int, worldsectionowner> worldsectionowners(1<<15);
static hashtable<ivec, int> worldchunkindices(1<<14);
static vector<ivec> worldsectionvisibilityadditions;
static bool worldsectionvisibilitydirty = true;
static int worldsectionvisibilitychunkx = INT_MIN, worldsectionvisibilitychunky = INT_MIN,
           worldsectionvisibilitymaxdist = -1;
static ivec worldsectionvisibilityfocus(INT_MIN, INT_MIN, INT_MIN);
static float worldchunkvasectionmillis = 2.0f;
static int worldvaevictionsframe = 0, worldvanorenderskipsframe = 0;
static int worldvaresidentcounts[WORLD_VA_GEOMETRY_COUNT], worldvapendingbuildcount = 0, worldvapendinguploadcount = 0;

static ivec worldchunkindexkey(int x, int y)
{
    uint hash = uint(x) * 0x9E3779B1U ^ uint(y) * 0x85EBCA77U ^ uint(x) ^ uint(y);
    return ivec(x, y, int(hash));
}

void worldpositiontoabsolute(vec &position)
{
    position.x += float(double(worldfirstchunkx) * WORLD_CHUNK_SIZE);
    position.y += float(double(worldfirstchunky) * WORLD_CHUNK_SIZE);
}

void worldpositiontolocal(vec &position)
{
    position.x -= float(double(worldfirstchunkx) * WORLD_CHUNK_SIZE);
    position.y -= float(double(worldfirstchunky) * WORLD_CHUNK_SIZE);
}

float worldpositionheight(float z)
{
    return z / float(WORLD_BLOCK_SIZE) + WORLD_MIN_HEIGHT;
}

void worldselectiontoabsolute(selinfo &selection)
{
    selection.o.x += worldfirstchunkx * WORLD_CHUNK_SIZE;
    selection.o.y += worldfirstchunky * WORLD_CHUNK_SIZE;
}

void worldselectiontolocal(selinfo &selection)
{
    selection.o.x -= worldfirstchunkx * WORLD_CHUNK_SIZE;
    selection.o.y -= worldfirstchunky * WORLD_CHUNK_SIZE;
}

static void setworldleavesalpha(cube *root, bool enabled, int leaveslot, int needlesslot)
{
    if(!root) return;
    loopi(8)
    {
        cube &c = root[i];
        if(c.children) setworldleavesalpha(c.children, enabled, leaveslot, needlesslot);
        else if(!isempty(c) && (c.texture[0] == leaveslot || c.texture[0] == needlesslot))
        {
            bool foliage = true;
            loopj(6) if(c.texture[j] != c.texture[0]) { foliage = false; break; }
            if(!foliage) continue;
            if(enabled) c.material |= MAT_ALPHA;
            else c.material &= ~MAT_ALPHA;
            c.visible = c.merged = 0;
        }
    }
}

static void setworldleavesalpha(cube *root, bool enabled)
{
    worlddefinition *leaves = findworldcube("leaves"), *needles = findworldcube("needles");
    if(!root || (!leaves && !needles)) return;
    setworldleavesalpha(root, enabled, leaves ? leaves->slot : -1, needles ? needles->slot : -1);
}

static void updateleavesalpha()
{
    setworldleavesalpha(worldroot, leavesalpha != 0);
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.root && chunk.root != worldroot) setworldleavesalpha(chunk.root, leavesalpha != 0);
        memclear(chunk.opaqueknown);
        memclear(chunk.portalsknown);
    }
    invalidateworldsectionvisibility();
    if(worldroot) allchanged();
}

VARP(asyncchunkloads, 2, 4, 4);
VARP(chunkthreads, 0, 0, 16);
VAR(chunkcachedist, 0, 0, 0);
VAR(chunkpendinglimit, 4, 8, 16);
VAR(chunklookahead, 0, 2, 8);
VAR(chunkpublishbudget, 2, 3, 33);
VAR(chunkcleanupbudget, 1, 3, 33);
VAR(chunksectionbatch, 1, 1, WORLD_MAX_SECTION_BATCH);
VAR(chunkvastagelimit, 1, 3, 16);
VAR(chunkinteriorradius, 1, 2, 8);
VAR(drawfullchunk, 0, 0, 1);

static cube *prepareworldchunk(worldchunkjob &job);
static int worldchunkloader(void *);
static bool startworldchunkloader();
static void shutdownworldchunkloader();
static void updateworldscatterers();
static void clearworldscattererentities();
static void clearworldscattermeshes();
static void updateworldlods(int chunkx, int chunky, bool force = false);
static void clearworldlods();
static int findworldchunk(int x, int y);
int remipworldchunk(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled = NULL);
static int remipworldchunkbounded(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled,
                                  const worldchunkdirtybounds *dirty);
static bool subdivideworldmip(const cube &c, cube *children);
static bool prepareworldchunksectionstates(worldchunkjob &job);
static int pruneworldchunkresidency(int chunkx, int chunky, int limit);
static void worldchunkname(char *name, size_t len, const worldchunk &chunk);
void setmapfilenames(const char *fname, const char *cname);

int getworldsectionsize()
{
    return worldchunks.empty() ? 0 : WORLD_SECTION_SIZE;
}

static cube sampleworldblockcube(const cube &source, const ivec &position, ivec &origin, int &size)
{
    cube sampled = source;
    sampled.children = NULL;
    sampled.ext = NULL;
    while(size > WORLD_BLOCK_SIZE && !isempty(sampled) && !isentirelysolid(sampled))
    {
        cube children[8];
        subdivideworldmip(sampled, children);
        size >>= 1;
        const int child = (position.x >= origin.x + size ? 1 : 0) |
                          (position.y >= origin.y + size ? 2 : 0) |
                          (position.z >= origin.z + size ? 4 : 0);
        origin = ivec(child, origin, size);
        sampled = children[child];
    }
    return sampled;
}

struct worlddebugstats
{
    int chunkx, chunky;
    double absolutex, absolutey, absolutez;
    int rendered;
    int loadingqueue, generationqueue;
    float tectonicactivity, tectonicuplift, tectonictrench, tectoniccaveexpansion;
};

static void getworlddebugstats(const vec &position, worlddebugstats &stats)
{
    stats.rendered = 0;
    stats.loadingqueue = stats.generationqueue = 0;
    stats.tectonicactivity = stats.tectonicuplift = stats.tectonictrench = stats.tectoniccaveexpansion = 0;

    if(!worldchunks.empty())
    {
        const int localchunkx = int(floor(position.x / WORLD_CHUNK_SIZE)),
                  localchunky = int(floor(position.y / WORLD_CHUNK_SIZE));
        stats.chunkx = worldfirstchunkx + localchunkx;
        stats.chunky = worldfirstchunky + localchunky;
        stats.absolutex = double(worldfirstchunkx) * WORLD_CHUNK_SIZE + position.x;
        stats.absolutey = double(worldfirstchunky) * WORLD_CHUNK_SIZE + position.y;

        loopv(worldchunks)
        {
            const worldchunk &chunk = worldchunks[i];
            if(worldchunkmounted(chunk)) stats.rendered++;
            if(chunk.loading)
            {
                if(chunk.generating) stats.generationqueue++;
                else stats.loadingqueue++;
            }
        }
    }
    else
    {
        stats.chunkx = int(floor(position.x / WORLD_CHUNK_SIZE));
        stats.chunky = int(floor(position.y / WORLD_CHUNK_SIZE));
        stats.absolutex = position.x;
        stats.absolutey = position.y;
    }
    stats.absolutez = position.z;
    if(!worldchunks.empty())
    {
        const int blockx = int(floor(stats.absolutex / WORLD_BLOCK_SIZE)),
                  blocky = int(floor(stats.absolutey / WORLD_BLOCK_SIZE)),
                  logicalz = int(floor(position.z / WORLD_BLOCK_SIZE)) + WORLD_MIN_HEIGHT;
        game::sampleworldgenerationdebug(blockx, blocky, logicalz, stats.tectonicactivity, stats.tectonicuplift, stats.tectonictrench,
                                         stats.tectoniccaveexpansion);
    }
}

static worlddebugstats worlddebugcache;
static int worlddebugcachemillis = -1;

static const worlddebugstats &currentworlddebugstats()
{
    if(worlddebugcachemillis != totalmillis)
    {
        getworlddebugstats(camera1->o, worlddebugcache);
        worlddebugcachemillis = totalmillis;
    }
    return worlddebugcache;
}

static void debugcoordinateresult(double coordinate)
{
    defformatstring(value, "%.2f", coordinate);
    result(value);
}

static void debugworldvalueresult(float value)
{
    defformatstring(formatted, "%.3f", clamp(value, 0.0f, 1.0f));
    result(formatted);
}

ICOMMAND(getdebugcamx, "", (), debugcoordinateresult(currentworlddebugstats().absolutex));
ICOMMAND(getdebugcamy, "", (), debugcoordinateresult(currentworlddebugstats().absolutey));
ICOMMAND(getdebugcamz, "", (), debugcoordinateresult(currentworlddebugstats().absolutez));
ICOMMAND(getdebugchunkx, "", (), intret(currentworlddebugstats().chunkx));
ICOMMAND(getdebugchunky, "", (), intret(currentworlddebugstats().chunky));
ICOMMAND(getdebugrenderedfull, "", (), intret(currentworlddebugstats().rendered));
ICOMMAND(getdebugtargetchunks, "", (), intret((2 * maxchunkdist + 1) * (2 * maxchunkdist + 1)));
ICOMMAND(getdebugloadingqueue, "", (), intret(currentworlddebugstats().loadingqueue));
ICOMMAND(getdebuggenerationqueue, "", (), intret(currentworlddebugstats().generationqueue));
ICOMMAND(getdebugtectonicactivity, "", (), debugworldvalueresult(currentworlddebugstats().tectonicactivity));
ICOMMAND(getdebugtectonicuplift, "", (), debugworldvalueresult(currentworlddebugstats().tectonicuplift));
ICOMMAND(getdebugtectonictrench, "", (), debugworldvalueresult(currentworlddebugstats().tectonictrench));
ICOMMAND(getdebugtectoniccave, "", (), debugworldvalueresult(currentworlddebugstats().tectoniccaveexpansion));

void clearworldchunks()
{
    ZoneScopedN("Chunks/Clear all chunks");
    cancelworldedit();
    clearworldscattererentities();
    clearworldscattermeshes();
    clearworldlods();
    shutdownworldchunkloader();
    worldchunkvaupdates.setsize(0);
    worldchunkvaupdateset.clear();
    worldchunkvapendingbuilds.setsize(0);
    worldchunkvapendingbuildset.clear();
    memclear(worldvaresidentcounts);
    worldvapendingbuildcount = worldvapendinguploadcount = 0;
    worldsectionowners.clear();
    worldchunkindices.clear();
    invalidateworldsectionvisibility();
    worldsectionvisibilitychunkx = worldsectionvisibilitychunky = INT_MIN;
    worldsectionvisibilitymaxdist = -1;
    worldsectionvisibilityfocus = ivec(INT_MIN, INT_MIN, INT_MIN);
    loopv(worldchunks) if(worldchunks[i].root && worldchunks[i].root != worldroot)
    {
        ZoneScopedN("Chunks/Free chunk during clear");
        ZoneTextF("%d_%d", worldchunks[i].x, worldchunks[i].y);
        freeocta(worldchunks[i].root);
    }
    worldchunks.setsize(0);
    reconstructedworldscatter.setsize(0);
    reconstructedworldrenderdata.clear();
    worldfolder[0] = '\0';
    activeworldchunk = -1;
    worldfirstchunkx = worldfirstchunky = 0;
    lastplayerchunkx = lastplayerchunky = INT_MIN;
    lastchunkdist = -1;
    stopworldchunkgeneration = false;
    rebuildingworldchunks = false;
    lastworldchunkpublish = -1;
    lastworldchunkmotion = -1;
    worldchunkvelocityx = worldchunkvelocityy = 0;
    worldchunkfocusx = worldchunkfocusy = worldchunkaheadx = worldchunkaheady = worldchunkviewx = worldchunkviewy = 0;
    worldchunkvasectionmillis = 2.0f;
    worlddebugcachemillis = -1;
    ++worldchunkepoch;
}

static void copyworldcube(const cube &src, cube &dst)
{
    dst = src;
    dst.visible = 0;
    dst.merged = 0;
    dst.ext = NULL;
    if(src.children)
    {
        dst.children = newcubes(F_EMPTY);
        loopi(8) copyworldcube(src.children[i], dst.children[i]);
    }
}

static const cube &lookupworldchunkrootcube(const cube *root, const ivec &pos, int size)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    const cube *c = &root[octastep(pos.x, pos.y, pos.z, scale)];
    while(!(size >> scale) && c->children)
    {
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    return *c;
}

static void resetworldcube(cube &c)
{
    c.children = NULL;
    c.ext = NULL;
    c.visible = 0;
    c.merged = 0;
    c.material = MAT_AIR;
    emptyfaces(c);
    loopi(6) c.texture[i] = DEFAULT_GEOM;
}

static void moveworldcube(cube &src, cube &dst)
{
    discardchildren(dst);
    dst = src;
    resetworldcube(src);
}

static void detachworldcubegeometry(cube &c)
{
    c.visible = 0;
    c.merged = 0;
    if(c.ext)
    {
        if(c.ext->va)
        {
            destroyva(c.ext->va);
            c.ext->va = NULL;
        }
        c.ext->tjoints = -1;
        freeoctaentities(c);
    }
    if(c.children) loopi(8) detachworldcubegeometry(c.children[i]);
}

static ivec worldchunkorigin(const worldchunk &chunk, int z = 0)
{
    return ivec((chunk.x - worldfirstchunkx) * WORLD_CHUNK_SIZE,
                (chunk.y - worldfirstchunky) * WORLD_CHUNK_SIZE, z);
}

static cube &lookupworldsnapshotrootcube(cube *root, const ivec &pos, int size)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    cube *c = &root[octastep(pos.x, pos.y, pos.z, scale)];
    while(!(size >> scale))
    {
        if(!c->children) subdividecube(*c);
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    return *c;
}

static void mergeworldsnapshotmountedsections(const worldchunk &chunk, cube *snapshotroot)
{
    const ivec runtimeorigin = worldchunkorigin(chunk);
    loop(section, WORLD_SECTION_LAYERS) loop(tile, WORLD_SECTION_TILES)
    {
        if(!(chunk.mountedtiles[section] & (1U << tile))) continue;
        const ivec position((tile % WORLD_SECTION_COLUMNS) * WORLD_SECTION_SIZE,
                            (tile / WORLD_SECTION_COLUMNS) * WORLD_SECTION_SIZE,
                            section * WORLD_SECTION_SIZE);
        cube &destination = lookupworldsnapshotrootcube(snapshotroot, position, WORLD_SECTION_SIZE);
        discardchildren(destination);
        copyworldsnapshotcube(lookupcube(ivec(runtimeorigin).add(position), WORLD_SECTION_SIZE), destination);
    }
}

static bool queueworldchunksave(worldchunk &chunk)
{
    if(chunk.saving) return true;
    if(!chunk.root || chunk.loading || chunk.corrupted || !worldfolder[0]) return false;
    if(!startworldchunkloader()) return false;

    worldchunksavejob *job = new worldchunksavejob(chunk.x, chunk.y, worldchunkepoch, chunk.revision, worldfolder);
    worldsnapshotsamplingtree sampling;
    if(!sampling.build(chunk) || !game::capturelocalchunkdata(chunk.x, chunk.y, job->gameplay))
    {
        if(!job->error[0]) copystring(job->error, "could not capture sparse gameplay state");
        conoutf(CON_ERROR, "could not capture chunk %d_%d for asynchronous saving: %s", chunk.x, chunk.y, job->error);
        delete job;
        return false;
    }
    job->root = sampling.root;
    sampling.root = NULL;
    job->renderdata = chunk.renderdata;
    job->playeredited = chunk.playeredited;
    if(!chunk.scatter.empty()) job->scatter.put(chunk.scatter.getbuf(), chunk.scatter.length());

    chunk.saving = true;
    chunk.savingrevision = chunk.revision;
    SDL_LockMutex(worldchunkmutex);
    worldchunksavejobs.add(job);
    TracyPlot("Chunks/Queued saves", int64_t(worldchunksavejobs.length()));
    SDL_CondSignal(worldchunkcond);
    SDL_UnlockMutex(worldchunkmutex);
    return true;
}

static void collectworldsupportnode(const cube &c, const ivec &origin, int size, const ivec &minimum, const ivec &maximum,
                                    const worldchunk &chunk, vector<ivec> &cells)
{
    if(origin.x >= maximum.x || origin.y >= maximum.y || origin.z >= maximum.z || origin.x + size <= minimum.x ||
       origin.y + size <= minimum.y || origin.z + size <= minimum.z)
        return;
    if(c.children)
    {
        const int childsize = size >> 1;
        loopi(8) collectworldsupportnode(c.children[i], ivec(i, origin, childsize), childsize, minimum, maximum, chunk, cells);
        return;
    }
    const int worldindex = getworldcubebytextures(c.texture);
    if(size < WORLD_BLOCK_SIZE || isempty(c) || !isentirelysolid(c) || getworldcubesupportdistance(worldindex) <= 0)
        return;
    const int startx = max(origin.x, minimum.x), starty = max(origin.y, minimum.y), startz = max(origin.z, minimum.z),
              endx = min(origin.x + size, maximum.x), endy = min(origin.y + size, maximum.y), endz = min(origin.z + size, maximum.z),
              absolutex = chunk.x * WORLD_CHUNK_SIZE, absolutey = chunk.y * WORLD_CHUNK_SIZE;
    for(int z = startz; z < endz; z += WORLD_BLOCK_SIZE)
        for(int y = starty; y < endy; y += WORLD_BLOCK_SIZE)
            for(int x = startx; x < endx; x += WORLD_BLOCK_SIZE)
                cells.add(ivec(absolutex + x, absolutey + y, z));
}

bool collectworldsupportcells(const ivec &absoluteorigin, int size, vector<ivec> &cells)
{
    cells.setsize(0);
    if(size <= 0 || absoluteorigin.z < 0 || absoluteorigin.z + size > WORLD_MAP_SIZE) return false;
    const int chunkx = int(floor(double(absoluteorigin.x) / WORLD_CHUNK_SIZE)),
              chunky = int(floor(double(absoluteorigin.y) / WORLD_CHUNK_SIZE));
    const int index = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(index)) return false;
    const worldchunk &chunk = worldchunks[index];
    if(chunk.loading || chunk.corrupted || !chunk.root) return false;
    const ivec minimum(absoluteorigin.x - chunkx * WORLD_CHUNK_SIZE, absoluteorigin.y - chunky * WORLD_CHUNK_SIZE, absoluteorigin.z),
               maximum = ivec(minimum).add(size);
    if(size == WORLD_SECTION_SIZE && !(minimum.x % WORLD_SECTION_SIZE) && !(minimum.y % WORLD_SECTION_SIZE) &&
       !(minimum.z % WORLD_SECTION_SIZE))
    {
        const int section = minimum.z / WORLD_SECTION_SIZE,
                  tile = minimum.y / WORLD_SECTION_SIZE * WORLD_SECTION_COLUMNS + minimum.x / WORLD_SECTION_SIZE;
        if(section >= 0 && section < WORLD_SECTION_LAYERS && tile >= 0 && tile < WORLD_SECTION_TILES &&
           (chunk.mountedtiles[section] & (1U << tile)))
        {
            const ivec runtimeorigin = ivec(worldchunkorigin(chunk)).add(minimum);
            collectworldsupportnode(lookupcube(runtimeorigin, WORLD_SECTION_SIZE), minimum, WORLD_SECTION_SIZE,
                                    minimum, maximum, chunk, cells);
            return true;
        }
    }
    loopi(8)
        collectworldsupportnode(chunk.root[i], ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE), WORLD_CHUNK_ROOT_SIZE,
                                minimum, maximum, chunk, cells);
    return true;
}

static cube &lookupworldchunkcube(worldchunk &chunk, const ivec &pos, int size)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    cube *c = &chunk.root[octastep(pos.x, pos.y, pos.z, scale)];
    while(!(size >> scale))
    {
        if(!c->children) subdividecube(*c);
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    return *c;
}

static const cube &lookupworldchunkcube(const worldchunk &chunk, const ivec &pos, int size)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    const cube *c = &chunk.root[octastep(pos.x, pos.y, pos.z, scale)];
    while(!(size >> scale) && c->children)
    {
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    return *c;
}

static bool worldchunkmounted(const worldchunk &chunk)
{
    loopi(WORLD_SECTION_LAYERS) if(chunk.mountedtiles[i]) return true;
    return false;
}

static void restoreworldwatersources(const cube &c, const ivec &origin, int size)
{
    if(c.children)
    {
        const int childsize = size >> 1;
        loopi(8) restoreworldwatersources(c.children[i], ivec(i, origin, childsize), childsize);
        return;
    }
    if((c.material&MATF_VOLUME) != MAT_WATER ||
       !(c.material&(MAT_WATER_SOURCE_MANUAL|MAT_WATER_SOURCE_NATURAL_ACTIVE)))
        return;
    const ivec end = ivec(origin).add(size);
    for(int z = origin.z; z < end.z; z += WORLD_BLOCK_SIZE)
    for(int y = origin.y; y < end.y; y += WORLD_BLOCK_SIZE)
    for(int x = origin.x; x < end.x; x += WORLD_BLOCK_SIZE)
        watermaterialloaded(ivec(x, y, z), c.material);
}

static void restoreworldwatersources()
{
    if(!worldroot) return;
    const int rootsize = worldsize >> 1;
    loopi(8) restoreworldwatersources(worldroot[i], ivec(i, ivec(0, 0, 0), rootsize), rootsize);
}

static bool worldsectionfullysolid(const cube &c)
{
    if(c.children)
    {
        loopi(8) if(!worldsectionfullysolid(c.children[i])) return false;
        return true;
    }
    return isentirelysolid(c) && !isliquid(c.material&MATF_VOLUME);
}

static bool worldsectionhaswater(const cube &c)
{
    if(c.children)
    {
        loopi(8) if(worldsectionhaswater(c.children[i])) return true;
        return false;
    }
    return (c.material&MATF_VOLUME) == MAT_WATER;
}

static const cube &worldsectioncube(const worldchunk *chunk, const cube *root, int tile, int section)
{
    const int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    const ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE);
    if(chunk && (chunk->mountedtiles[section] & (1U << tile)))
        return lookupcube(ivec(worldchunkorigin(*chunk)).add(pos), WORLD_SECTION_SIZE);
    return lookupworldchunkrootcube(root, pos, WORLD_SECTION_SIZE);
}

static void reclassifyworldchunkrenderdata(worldchunk *chunk, cube *root, worldsectionrenderdata &renderdata,
                                           const worldchunkdirtybounds &dirty)
{
    if(!root || !dirty.valid) return;
    static const int directions[][3] =
    {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };
    bool affected[WORLD_SECTION_LAYERS][WORLD_SECTION_TILES], changed[WORLD_SECTION_LAYERS][WORLD_SECTION_TILES];
    memclear(affected);
    memclear(changed);
    const int minx = clamp(dirty.minimum.x / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1),
              maxx = clamp((dirty.maximum.x - 1) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1),
              miny = clamp(dirty.minimum.y / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1),
              maxy = clamp((dirty.maximum.y - 1) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1),
              minz = clamp(dirty.minimum.z / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1),
              maxz = clamp((dirty.maximum.z - 1) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1);
    for(int z = minz; z <= maxz; ++z) for(int y = miny; y <= maxy; ++y) for(int x = minx; x <= maxx; ++x)
    {
        const int tile = y * WORLD_SECTION_COLUMNS + x;
        affected[z][tile] = changed[z][tile] = true;
        loopi(6)
        {
            const int nx = x + directions[i][0], ny = y + directions[i][1], nz = z + directions[i][2];
            if(nx < 0 || nx >= WORLD_SECTION_COLUMNS || ny < 0 || ny >= WORLD_SECTION_COLUMNS || nz < 0 || nz >= WORLD_SECTION_LAYERS)
                continue;
            affected[nz][ny * WORLD_SECTION_COLUMNS + nx] = true;
        }
    }

    loop(section, WORLD_SECTION_LAYERS) loop(tile, WORLD_SECTION_TILES) if(affected[section][tile])
    {
        uchar &flags = renderdata.flags[section][tile];
        const cube &sectioncube = worldsectioncube(chunk, root, tile, section);
        const bool fullysolid = worldsectionfullysolid(sectioncube);
        flags &= ~(SECTION_WATER | SECTION_FULLY_SOLID | SECTION_NO_RENDER);
        if(worldsectionhaswater(sectioncube)) flags |= SECTION_WATER;
        if(fullysolid)
        {
            flags |= SECTION_FULLY_SOLID;
            flags &= ~(SECTION_INTERIOR | SECTION_CAVE_ENTRANCE);
        }
        else if(changed[section][tile]) flags |= SECTION_INTERIOR;
    }

    loop(section, WORLD_SECTION_LAYERS) loop(tile, WORLD_SECTION_TILES) if(affected[section][tile])
    {
        uchar &flags = renderdata.flags[section][tile];
        if(!(flags&SECTION_FULLY_SOLID)) continue;
        const int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
        bool exposed = false;
        loopi(6)
        {
            const int nx = x + directions[i][0], ny = y + directions[i][1], nz = section + directions[i][2];
            if(nx < 0 || nx >= WORLD_SECTION_COLUMNS || ny < 0 || ny >= WORLD_SECTION_COLUMNS || nz < 0 || nz >= WORLD_SECTION_LAYERS)
            {
                exposed = true;
                break;
            }
            const int neighbortile = ny * WORLD_SECTION_COLUMNS + nx;
            if(!worldsectionfullysolid(worldsectioncube(chunk, root, neighbortile, nz)))
            {
                exposed = true;
                break;
            }
        }
        if(exposed && !(flags&SECTION_EXTERIOR)) flags |= SECTION_INTERIOR;
        if(!(flags&(SECTION_EXTERIOR | SECTION_INTERIOR | SECTION_WATER))) flags |= SECTION_NO_RENDER;
    }
}

int getworldsectionrenderflags(int chunkx, int chunky, int tile, int section)
{
    const int index = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(index) || tile < 0 || tile >= WORLD_SECTION_TILES || section < 0 || section >= WORLD_SECTION_LAYERS) return 0;
    return worldchunks[index].renderdata.flags[section][tile];
}

static bool worldsectionvaactive(const worldsectionvaresidency &residency)
{
    loopi(WORLD_VA_GEOMETRY_COUNT) if(residency.state[i] >= PENDING_UPLOAD) return true;
    return false;
}

static bool worldsectionvaphysicalresident(const worldsectionvaresidency &residency)
{
    loopi(WORLD_VA_GEOMETRY_COUNT) if(residency.state[i] == RESIDENT || residency.state[i] == EVICTABLE) return true;
    return false;
}

static void setworldsectionvaresidencystate(worldsectionvaresidency &residency, int geometry, int state)
{
    const int oldstate = residency.state[geometry];
    if(oldstate == state) return;
    if(oldstate == PENDING_BUILD) worldvapendingbuildcount--;
    else if(oldstate == PENDING_UPLOAD) worldvapendinguploadcount--;
    else if(oldstate == RESIDENT || oldstate == EVICTABLE) worldvaresidentcounts[geometry]--;
    residency.state[geometry] = state;
    if(state == PENDING_BUILD) worldvapendingbuildcount++;
    else if(state == PENDING_UPLOAD) worldvapendinguploadcount++;
    else if(state == RESIDENT || state == EVICTABLE) worldvaresidentcounts[geometry]++;
}

static void resetworldsectionvaresidency(worldsectionvaresidency &residency)
{
    loopi(WORLD_VA_GEOMETRY_COUNT) setworldsectionvaresidencystate(residency, i, NOT_RESIDENT);
}

static void dirtyworldchunkvaresidency(worldchunk &chunk, int tile, int section)
{
    if(tile < 0 || tile >= WORLD_SECTION_TILES || section < 0 || section >= WORLD_SECTION_LAYERS) return;
    chunk.varesidencydirtytiles[section] |= 1U << tile;
    chunk.varesidencydirty = true;
}

static void dirtyallworldchunkvaresidency(worldchunk &chunk)
{
    const uint alltiles = (1U << WORLD_SECTION_TILES) - 1;
    loopi(WORLD_SECTION_LAYERS) chunk.varesidencydirtytiles[i] = alltiles;
    chunk.varesidencydirty = true;
}

static int worldchunksectionvakey(int chunkx, int chunky, int tile, int section)
{
    const int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    return worldchunkvaupdatekey(ivec((chunkx - worldfirstchunkx) * WORLD_CHUNK_SIZE + x * WORLD_SECTION_SIZE,
                                     (chunky - worldfirstchunky) * WORLD_CHUNK_SIZE + y * WORLD_SECTION_SIZE,
                                     section * WORLD_SECTION_SIZE));
}

static int worldchunksectionvakey(const worldchunk &chunk, int tile, int section)
{
    return worldchunksectionvakey(chunk.x, chunk.y, tile, section);
}

static void queueworldchunkvapendingbuild(const worldchunk &chunk, int tile, int section)
{
    const int key = worldchunksectionvakey(chunk, tile, section);
    if(worldchunkvapendingbuildset.access(key)) return;
    worldchunkvapendingbuildset.add(key);
    worldchunkvapendingbuilds.add(ivec(chunk.x, chunk.y, section * WORLD_SECTION_TILES + tile));
}

static void removeworldchunkvapendingbuild(const worldchunk &chunk, int tile, int section)
{
    const int key = worldchunksectionvakey(chunk, tile, section);
    if(!worldchunkvapendingbuildset.access(key)) return;
    worldchunkvapendingbuildset.remove(key);
    loopv(worldchunkvapendingbuilds) if(worldchunkvapendingbuilds[i] == ivec(chunk.x, chunk.y, section * WORLD_SECTION_TILES + tile))
    {
        worldchunkvapendingbuilds.removeunordered(i);
        break;
    }
}

static void clearworldsectionvaresidency(worldchunk &chunk, int tile, int section)
{
    removeworldchunkvapendingbuild(chunk, tile, section);
    resetworldsectionvaresidency(chunk.varesidency[section][tile]);
    dirtyworldchunkvaresidency(chunk, tile, section);
}

bool worldsectionvaenabled(const ivec &origin, int size)
{
    if(size != WORLD_SECTION_SIZE || worldchunks.empty()) return true;
    worldsectionowner *owner = worldsectionowners.access(worldchunkvaupdatekey(origin));
    if(!owner) return true;
    const int index = findworldchunk(owner->chunkx, owner->chunky);
    if(!worldchunks.inrange(index)) return false;
    const worldchunk &chunk = worldchunks[index];
    if(chunk.renderdata.flags[owner->section][owner->tile]&SECTION_NO_RENDER)
    {
        worldvanorenderskipsframe++;
        return false;
    }
    return worldsectionvaactive(chunk.varesidency[owner->section][owner->tile]);
}

static void invalidateworldchunksectionstate(worldchunk &chunk, int x, int y, int section)
{
    if(x < 0 || x >= WORLD_SECTION_COLUMNS || y < 0 || y >= WORLD_SECTION_COLUMNS || section < 0 || section >= WORLD_SECTION_LAYERS) return;
    const int tile = y * WORLD_SECTION_COLUMNS + x;
    const uint tilebit = 1U << tile;
    chunk.contentknown[section] &= ~tilebit;
    chunk.opaqueknown[section] &= ~tilebit;
    chunk.portalsknown[section] &= ~tilebit;
    dirtyworldchunkvaresidency(chunk, tile, section);
}

void markworldchunksdirty(const ivec &bbmin, const ivec &bbmax)
{
    if(suppressworldchunkdirty || worldchunks.empty()) return;
    bool visibilitychanged = false;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(!worldchunkmounted(chunk)) continue;
        ivec origin = worldchunkorigin(chunk);
        if(bbmax.x <= origin.x || bbmin.x >= origin.x + WORLD_CHUNK_SIZE ||
           bbmax.y <= origin.y || bbmin.y >= origin.y + WORLD_CHUNK_SIZE ||
           bbmax.z <= 0 || bbmin.z >= WORLD_MAP_SIZE)
            continue;
        int minx = clamp((bbmin.x - origin.x) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1),
            maxx = clamp((bbmax.x - 1 - origin.x) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1),
            miny = clamp((bbmin.y - origin.y) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1),
            maxy = clamp((bbmax.y - 1 - origin.y) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1),
            minz = clamp(bbmin.z / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1),
            maxz = clamp((bbmax.z - 1) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1);
        for(int z = minz; z <= maxz; ++z) for(int y = miny; y <= maxy; ++y) for(int x = minx; x <= maxx; ++x)
        {
            invalidateworldchunksectionstate(chunk, x, y, z);
            invalidateworldchunksectionstate(chunk, x - 1, y, z);
            invalidateworldchunksectionstate(chunk, x + 1, y, z);
            invalidateworldchunksectionstate(chunk, x, y - 1, z);
            invalidateworldchunksectionstate(chunk, x, y + 1, z);
            invalidateworldchunksectionstate(chunk, x, y, z - 1);
            invalidateworldchunksectionstate(chunk, x, y, z + 1);
        }
        worldchunkdirtybounds renderdirty;
        renderdirty.minimum = ivec(bbmin).sub(origin).max(0);
        renderdirty.maximum = ivec(bbmax).sub(origin).min(ivec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, WORLD_MAP_SIZE));
        renderdirty.valid = renderdirty.minimum.x < renderdirty.maximum.x && renderdirty.minimum.y < renderdirty.maximum.y &&
                            renderdirty.minimum.z < renderdirty.maximum.z;
        reclassifyworldchunkrenderdata(&chunk, chunk.root, chunk.renderdata, renderdirty);
        if(++chunk.revision == 0) chunk.revision = 1;
        chunk.playeredited = true;
        visibilitychanged = true;
    }
    if(visibilitychanged) invalidateworldsectionvisibility();
}

static bool mountworldchunktile(worldchunk &chunk, int section, int tile)
{
    const uint tilebit = 1U << tile;
    if(chunk.mountedtiles[section] & tilebit) return false;
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE);
    ivec runtimepos = ivec(worldchunkorigin(chunk)).add(pos);
    int key = worldchunkvaupdatekey(runtimepos);
    worldsectionowner *owner = worldsectionowners.access(key);
    if(owner)
    {
        if(owner->matches(chunk, section, tile))
        {
            chunk.mountedtiles[section] |= tilebit;
            return false;
        }
        conoutf(CON_ERROR,
                "refusing to mount chunk %d_%d section %d:%d over chunk %d_%d section %d:%d",
                chunk.x, chunk.y, section, tile, owner->chunkx, owner->chunky,
                int(owner->section), int(owner->tile));
        chunk.corrupted = true;
        invalidateworldsectionvisibility();
        return false;
    }
    moveworldcube(lookupworldchunkcube(chunk, pos, WORLD_SECTION_SIZE),
                  lookupcube(runtimepos, WORLD_SECTION_SIZE));
    restoreworldwatersources(lookupcube(runtimepos, WORLD_SECTION_SIZE), runtimepos, WORLD_SECTION_SIZE);
    worldsectionowners[key] = worldsectionowner(chunk.x, chunk.y, section, tile);
    chunk.mountedtiles[section] |= tilebit;
    return true;
}

static bool unmountworldchunktile(worldchunk &chunk, int section, int tile)
{
    const uint tilebit = 1U << tile;
    if(!(chunk.mountedtiles[section] & tilebit)) return false;
    clearworldsectionvaresidency(chunk, tile, section);
    int x = tile % WORLD_SECTION_COLUMNS, y = tile / WORLD_SECTION_COLUMNS;
    ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, section * WORLD_SECTION_SIZE);
    ivec runtimepos = ivec(worldchunkorigin(chunk)).add(pos);
    int key = worldchunkvaupdatekey(runtimepos);
    worldsectionowner *owner = worldsectionowners.access(key);
    if(!owner || !owner->matches(chunk, section, tile))
    {
        conoutf(CON_ERROR, "refusing to unmount chunk %d_%d section %d:%d: runtime ownership mismatch",
                chunk.x, chunk.y, section, tile);
        chunk.corrupted = true;
        invalidateworldsectionvisibility();
        chunk.mountedtiles[section] &= ~tilebit;
        return false;
    }
    cube &c = lookupcube(runtimepos, WORLD_SECTION_SIZE);
    int sectionstate = worldcubesectionstate(c);
    setworldchunksectioncontent(chunk, tile, section, (sectionstate&WORLD_SECTION_CONTENT) != 0);
    setworldchunksectionopaque(chunk, tile, section, (sectionstate&WORLD_SECTION_OPAQUE) != 0);
    chunk.portalsknown[section] &= ~tilebit;
    detachworldcubegeometry(c);
    moveworldcube(c, lookupworldchunkcube(chunk, pos, WORLD_SECTION_SIZE));
    worldsectionowners.remove(key);
    chunk.mountedtiles[section] &= ~tilebit;
    return true;
}

static void unmountworldchunk(worldchunk &chunk)
{
    if(!worldchunkmounted(chunk)) return;
    ZoneScopedN("Chunks/Unmount");
    ZoneTextF("%d_%d", chunk.x, chunk.y);
    loopi(WORLD_SECTION_LAYERS) loopj(WORLD_SECTION_TILES)
        unmountworldchunktile(chunk, i, j);
}

static int unmountworldchunkcolumnbatch(worldchunk &chunk, int tile, int *sections, int maxsections)
{
    ZoneScopedN("Chunks/Unmount column sections");
    ZoneTextF("%d_%d tile %d", chunk.x, chunk.y, tile);
    const uint tilebit = 1U << tile;
    const int playersection = clamp(player ? int(player->o.z) / WORLD_SECTION_SIZE
                                           : WORLD_SECTION_LAYERS / 2,
                                    0, int(WORLD_SECTION_LAYERS) - 1);
    int unmounted = 0;
    while(unmounted < maxsections)
    {
        int best = -1, bestdist = -1;
        loopi(WORLD_SECTION_LAYERS)
        {
            if(!(chunk.mountedtiles[i] & tilebit)) continue;
            int dist = abs(i - playersection);
            if(dist <= bestdist) continue;
            best = i;
            bestdist = dist;
        }
        if(best < 0) break;
        const bool hadresidentva = worldsectionvaactive(chunk.varesidency[best][tile]);
        if(!unmountworldchunktile(chunk, best, tile)) break;
        if(hadresidentva) worldvaevictionsframe++;
        sections[unmounted++] = best;
    }
    ZoneValue(unmounted);
    return unmounted;
}

static int findworldchunk(int x, int y)
{
    ivec key = worldchunkindexkey(x, y);
    int *cached = worldchunkindices.access(key);
    if(cached && *cached < 0) return -1;
    if(cached && worldchunks.inrange(*cached) && worldchunks[*cached].x == x && worldchunks[*cached].y == y) return *cached;
    loopv(worldchunks) if(worldchunks[i].x == x && worldchunks[i].y == y)
    {
        worldchunkindices[key] = i;
        return i;
    }
    worldchunkindices[key] = -1;
    return -1;
}

bool sampleworldsolid(const ivec &position, int &leafbottom)
{
    if(worldchunks.empty())
    {
        ivec origin;
        int size;
        const cube &c = lookupcube(position, -1, origin, size);
        leafbottom = origin.z;
        return !isempty(c);
    }
    if(position.x < 0 || position.y < 0 || position.z < 0 || position.x >= worldsize || position.y >= worldsize ||
       position.z >= WORLD_MAP_SIZE)
    {
        leafbottom = position.z;
        return position.z < WORLD_MAP_SIZE;
    }
    const int localchunkx = position.x / WORLD_CHUNK_SIZE, localchunky = position.y / WORLD_CHUNK_SIZE,
              chunkx = worldfirstchunkx + localchunkx, chunky = worldfirstchunky + localchunky,
              index = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(index) || worldchunks[index].loading || !worldchunks[index].root)
    {
        leafbottom = 0;
        return true;
    }
    const worldchunk &chunk = worldchunks[index];
    const ivec local(position.x - localchunkx * WORLD_CHUNK_SIZE, position.y - localchunky * WORLD_CHUNK_SIZE, position.z);
    const int section = local.z / WORLD_SECTION_SIZE,
              tile = (local.y / WORLD_SECTION_SIZE) * WORLD_SECTION_COLUMNS + local.x / WORLD_SECTION_SIZE;
    if(chunk.mountedtiles[section] & (1U << tile))
    {
        ivec origin;
        int size;
        const cube &c = lookupcube(position, -1, origin, size);
        leafbottom = origin.z;
        return !isempty(c);
    }

    int scale = WORLD_CHUNK_SCALE - 1;
    const cube *c = &chunk.root[octastep(local.x, local.y, local.z, scale)];
    while(c->children)
    {
        --scale;
        c = &c->children[octastep(local.x, local.y, local.z, scale)];
    }
    leafbottom = local.z & (~0U << scale);
    return !isempty(*c);
}

void captureworldlocalambient(const ivec &origin, const ivec &dimensions, int resolution, uchar *solid, bvec4 *albedo)
{
    if(!solid || !albedo || dimensions.x <= 0 || dimensions.y <= 0 || dimensions.z <= 0 || resolution <= 0) return;
    const int halfresolution = resolution / 2;
    int cachedlocalchunkx = INT_MIN, cachedlocalchunky = INT_MIN, cachedchunkindex = -1;
    loop(z, dimensions.z) loop(y, dimensions.y) loop(x, dimensions.x)
    {
        const ivec position(origin.x + x * resolution + halfresolution, origin.y + y * resolution + halfresolution,
                            origin.z + z * resolution + halfresolution);
        bool occupied = true;
        bvec color(0, 0, 0);
        if(worldchunks.empty())
        {
            ivec cubeorigin;
            int size;
            const cube &c = lookupcube(position, -1, cubeorigin, size);
            occupied = !isempty(c);
            if(occupied) color = getworldcubegialbedo(c);
        }
        else if(position.x >= 0 && position.y >= 0 && position.z >= 0 && position.x < worldsize && position.y < worldsize &&
                position.z < WORLD_MAP_SIZE)
        {
            const int localchunkx = position.x / WORLD_CHUNK_SIZE, localchunky = position.y / WORLD_CHUNK_SIZE;
            if(localchunkx != cachedlocalchunkx || localchunky != cachedlocalchunky)
            {
                cachedlocalchunkx = localchunkx;
                cachedlocalchunky = localchunky;
                cachedchunkindex = findworldchunk(worldfirstchunkx + localchunkx, worldfirstchunky + localchunky);
            }
            if(worldchunks.inrange(cachedchunkindex) && !worldchunks[cachedchunkindex].loading && worldchunks[cachedchunkindex].root)
            {
                const worldchunk &chunk = worldchunks[cachedchunkindex];
                const ivec local(position.x - localchunkx * WORLD_CHUNK_SIZE, position.y - localchunky * WORLD_CHUNK_SIZE, position.z);
                const int section = local.z / WORLD_SECTION_SIZE,
                          tile = (local.y / WORLD_SECTION_SIZE) * WORLD_SECTION_COLUMNS + local.x / WORLD_SECTION_SIZE;
                const uint tilebit = 1U << tile;
                if((chunk.contentknown[section] & tilebit) && !(chunk.contenttiles[section] & tilebit)) occupied = false;
                else if(chunk.mountedtiles[section] & tilebit)
                {
                    ivec cubeorigin;
                    int size;
                    const cube &c = lookupcube(position, -1, cubeorigin, size);
                    occupied = !isempty(c);
                    if(occupied) color = getworldcubegialbedo(c);
                }
                else
                {
                    int scale = WORLD_CHUNK_SCALE - 1;
                    const cube *c = &chunk.root[octastep(local.x, local.y, local.z, scale)];
                    while(c->children)
                    {
                        --scale;
                        c = &c->children[octastep(local.x, local.y, local.z, scale)];
                    }
                    occupied = !isempty(*c);
                    if(occupied) color = getworldcubegialbedo(*c);
                }
            }
        }
        const int destination = (z * dimensions.y + y) * dimensions.x + x;
        solid[destination] = occupied ? 255 : 0;
        albedo[destination] = bvec4(color, occupied ? 255 : 0);
    }
}

static void indexworldchunk(int index)
{
    if(!worldchunks.inrange(index)) return;
    const worldchunk &chunk = worldchunks[index];
    worldchunkindices[worldchunkindexkey(chunk.x, chunk.y)] = index;
}

static void rebuildworldchunkindices()
{
    worldchunkindices.clear();
    loopv(worldchunks) indexworldchunk(i);
}

bool receivenetworkworldchunk(int chunkx, int chunky, uint revision, const uchar *voxdata, int voxlength, const uchar *datdata, int datlength)
{
    if(!voxdata || voxlength <= 0 || !datdata || datlength <= 0 || !revision) return false;
    vector<uchar> vox, dat;
    vox.put(voxdata, voxlength);
    dat.put(datdata, datlength);
    worldchunksnapshot snapshot(chunkx, chunky);
    vector<worldsnapshotvoxel> voxels;
    string error;
    if(!deserializeworldsnapshotvox(vox, chunkx, chunky, snapshot, voxels, error) || snapshot.revision != revision)
        return false;
    const uint voxchecksum = uint(vox[vox.length() - 4]) | uint(vox[vox.length() - 3]) << 8 | uint(vox[vox.length() - 2]) << 16 |
                             uint(vox[vox.length() - 1]) << 24;
    if(!deserializeworldsnapshotdat(dat, chunkx, chunky, voxchecksum, snapshot, error)) return false;
    const int oldindex = findworldchunk(chunkx, chunky);
    if(worldchunks.inrange(oldindex) && worldchunks[oldindex].playeredited && worldchunks[oldindex].revision >= revision) return true;

    cube *root = allocworldsnapshotfamily();
    loopi(8) buildworldsnapshotcube(root[i], ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE), WORLD_CHUNK_ROOT_SIZE, snapshot, voxels);
    if(!game::restorelocalchunkdata(chunkx, chunky, snapshot.gameplay.getbuf(), snapshot.gameplay.length()))
    {
        freeworldsnapshotfamily(root);
        return false;
    }
    if(worldchunks.inrange(oldindex))
    {
        worldchunk &old = worldchunks[oldindex];
        if(worldchunkmounted(old)) unmountworldchunk(old);
        if(old.root && old.root != worldroot) freeocta(old.root);
        worldchunks.removeunordered(oldindex);
        rebuildworldchunkindices();
    }
    worldchunk &chunk = worldchunks.add(worldchunk(chunkx, chunky, root));
    indexworldchunk(worldchunks.length() - 1);
    chunk.revision = chunk.savedrevision = revision;
    chunk.playeredited = snapshot.playeredited;
    chunk.renderdata = snapshot.renderdata;
    worldchunkdirtybounds renderdirty;
    renderdirty.minimum = ivec(0, 0, 0);
    renderdirty.maximum = ivec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, WORLD_MAP_SIZE);
    renderdirty.valid = true;
    reclassifyworldchunkrenderdata(&chunk, chunk.root, chunk.renderdata, renderdirty);
    loopv(snapshot.scatter)
        chunk.scatter.add(worldscatterinstance(snapshot.scatter[i].x, snapshot.scatter[i].y, snapshot.scatter[i].z, snapshot.scatter[i].type,
                                               snapshot.scatter[i].orientation));
    game::cacheworldscattertransforms(chunkx, chunky, game::getworldscattermaxoffset(), chunk.scatter);
    addworldsectionvisibilitychunk(chunkx, chunky);
    invalidateworldsectionvisibility();
    const ivec localorigin = worldchunkorigin(chunk);
    invalidatelocalambient(localorigin, ivec(localorigin).add(ivec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, WORLD_MAP_SIZE)));
    lastplayerchunkx = INT_MIN;
    return true;
}

static int worldchunkdistance(int x, int y, int focusx, int focusy)
{
    long long dx = (long long)x - focusx, dy = (long long)y - focusy;
    if(dx < 0) dx = -dx;
    if(dy < 0) dy = -dy;
    return int(min(max(dx, dy), (long long)INT_MAX));
}

static bool worldchunkinview(const worldchunk &chunk, int chunkx, int chunky)
{
    return worldchunkdistance(chunk.x, chunk.y, chunkx, chunky) <= maxchunkdist;
}

static bool worldchunkjobwanted(int x, int y, int chunkx, int chunky, int aheadx, int aheady)
{
    (void)aheadx;
    (void)aheady;
    return worldchunkdistance(x, y, chunkx, chunky) <= maxchunkdist;
}

static void worldchunkviewfocus(int chunkx, int chunky, int &viewx, int &viewy)
{
    float dominant = max(fabsf(camdir.x), fabsf(camdir.y));
    if(!camera1 || dominant < 0.05f)
    {
        viewx = chunkx;
        viewy = chunky;
        return;
    }
    int reach = min(maxchunkdist, 6);
    viewx = chunkx + int(roundf(camdir.x / dominant * reach));
    viewy = chunky + int(roundf(camdir.y / dominant * reach));
}

static int worldchunkcoordinatescore(int x, int y)
{
    int currentdist = worldchunkdistance(x, y, worldchunkfocusx, worldchunkfocusy),
        aheaddist = worldchunkdistance(x, y, worldchunkaheadx, worldchunkaheady),
        viewdist = worldchunkdistance(x, y, worldchunkviewx, worldchunkviewy);
    long long dx = (long long)x - worldchunkaheadx,
              dy = (long long)y - worldchunkaheady,
              urgent = currentdist <= 1 ? 0 : 0x10000000LL,
              score = urgent + (long long)currentdist * 0x200000
                    + (long long)viewdist * 0x20000
                    + (long long)aheaddist * 0x1000 + dx * dx + dy * dy;
    return int(min(score, (long long)INT_MAX));
}

static int worldchunkjobscore(const worldchunkjob &job)
{
    return worldchunkcoordinatescore(job.x, job.y);
}

static int worldchunkloader(void *)
{
#ifdef TRACY_ENABLE
    tracy::SetThreadName("World chunk loader");
#endif
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
    for(;;)
    {
        SDL_LockMutex(worldchunkmutex);
        while(worldchunkjobs.empty() && worldchunksavejobs.empty() && !stopworldchunkthread)
            SDL_CondWait(worldchunkcond, worldchunkmutex);
        if(stopworldchunkthread)
        {
            SDL_UnlockMutex(worldchunkmutex);
            return 0;
        }
        worldchunkjob *job = NULL;
        worldchunksavejob *savejob = NULL;
        {
            ZoneScopedN("Chunks/Worker select job");
            if(!worldchunkjobs.empty() && (!flushingworldchunksaves || worldchunksavejobs.empty()))
            {
                int best = 0, bestscore = worldchunkjobscore(*worldchunkjobs[0]);
                loopv(worldchunkjobs) if(i)
                {
                    int score = worldchunkjobscore(*worldchunkjobs[i]);
                    if(score < bestscore) { best = i; bestscore = score; }
                }
                job = worldchunkjobs.remove(best);
                worldchunkactivejobs.add(job);
                TracyPlot("Chunks/Queued jobs", int64_t(worldchunkjobs.length()));
                TracyPlot("Chunks/Active workers", int64_t(worldchunkactivejobs.length()));
            }
            else if(!worldchunksavejobs.empty())
            {
                savejob = worldchunksavejobs.remove(0);
                worldchunksaveactivejobs.add(savejob);
                TracyPlot("Chunks/Queued saves", int64_t(worldchunksavejobs.length()));
                TracyPlot("Chunks/Active saves", int64_t(worldchunksaveactivejobs.length()));
            }
        }
        SDL_UnlockMutex(worldchunkmutex);

        if(savejob)
        {
            {
                ZoneScopedN("Chunks/Worker save snapshot");
                ZoneTextF("%d_%d", savejob->x, savejob->y);
                savejob->success = writeworldchunksnapshot(savejob->folder, savejob->x, savejob->y, savejob->revision, savejob->playeredited,
                                                           savejob->compress, savejob->root, savejob->renderdata, savejob->scatter, savejob->gameplay,
                                                           savejob->error);
                freeocta(savejob->root);
                savejob->root = NULL;
            }
            SDL_LockMutex(worldchunkmutex);
            worldchunksaveactivejobs.removeobj(savejob);
            worldchunksaveresults.add(savejob);
            TracyPlot("Chunks/Active saves", int64_t(worldchunksaveactivejobs.length()));
            TracyPlot("Chunks/Ready saves", int64_t(worldchunksaveresults.length()));
            SDL_CondBroadcast(worldchunkcond);
            SDL_UnlockMutex(worldchunkmutex);
            continue;
        }

        {
            ZoneScopedN("Chunks/Worker job");
            ZoneTextF("%d_%d", job->x, job->y);
            if(job->checksnapshot && !SDL_AtomicGet(&job->cancelled))
            {
                job->snapshotresult = loadworldchunksnapshotdata(job->folder, job->x, job->y, job->root, job->scatter, job->renderdata,
                                                                  job->gameplay, job->snapshoterror, &job->snapshotrevision,
                                                                  &job->snapshotplayeredited);
                job->checksnapshot = false;
            }
            if(!job->root && !SDL_AtomicGet(&job->cancelled)) job->root = prepareworldchunk(*job);
            if(job->root && !SDL_AtomicGet(&job->cancelled))
            {
                setworldleavesalpha(job->root, job->leavesalpha);
                job->sectionstatesready = prepareworldchunksectionstates(*job);
            }
        }
        if(SDL_AtomicGet(&job->cancelled) && job->root)
        {
            ZoneScopedN("Chunks/Worker discard cancelled");
            game::freeworldchunk(job->root);
            job->root = NULL;
        }

        SDL_LockMutex(worldchunkmutex);
        worldchunkactivejobs.removeobj(job);
        TracyPlot("Chunks/Active workers", int64_t(worldchunkactivejobs.length()));
        while(worldchunkresults.length() >= WORLD_MAX_PREPARED_CHUNKS && !stopworldchunkthread)
            SDL_CondWait(worldchunkcond, worldchunkmutex);
        if(stopworldchunkthread)
        {
            SDL_UnlockMutex(worldchunkmutex);
            {
                ZoneScopedN("Chunks/Worker discard on shutdown");
                game::freeworldchunk(job->root);
            }
            delete job;
            return 0;
        }
        {
            ZoneScopedN("Chunks/Worker enqueue result");
            worldchunkresults.add(job);
            TracyPlot("Chunks/Ready results", int64_t(worldchunkresults.length()));
        }
        SDL_UnlockMutex(worldchunkmutex);
    }
}

static bool startworldchunkloader()
{
    if(!worldchunkworkers.empty()) return true;
    ZoneScopedN("Chunks/Start worker pool");
    worldchunkmutex = SDL_CreateMutex();
    worldchunkcond = SDL_CreateCond();
    stopworldchunkthread = false;
    if(!worldchunkmutex || !worldchunkcond)
    {
        if(worldchunkcond) SDL_DestroyCond(worldchunkcond);
        if(worldchunkmutex) SDL_DestroyMutex(worldchunkmutex);
        worldchunkcond = NULL;
        worldchunkmutex = NULL;
        return false;
    }

    // Keep roughly one third of the logical CPUs available to the render thread
    // and OS. The old hard cap of four left modern six- and eight-core CPUs
    // underused even though these workers run at low scheduler priority.
    int workers = chunkthreads > 0 ? chunkthreads : min(max(numcpus - max(numcpus / 3, 1), 1), 8);
    loopi(workers)
    {
        SDL_Thread *worker = SDL_CreateThread(worldchunkloader, "world chunk loader", NULL);
        if(!worker) break;
        worldchunkworkers.add(worker);
    }
    if(worldchunkworkers.empty())
    {
        SDL_DestroyCond(worldchunkcond);
        SDL_DestroyMutex(worldchunkmutex);
        worldchunkcond = NULL;
        worldchunkmutex = NULL;
        return false;
    }
    conoutf(CON_DEBUG, "started %d low-priority world chunk workers (numcpus %d)",
            worldchunkworkers.length(), numcpus);
    return true;
}

static void shutdownworldchunkloader()
{
    ZoneScopedN("Chunks/Shutdown worker pool");
    if(!worldchunkworkers.empty()) flushworldchunksaves();
    if(!worldchunkworkers.empty())
    {
        SDL_LockMutex(worldchunkmutex);
        stopworldchunkthread = true;
        loopv(worldchunkactivejobs) SDL_AtomicSet(&worldchunkactivejobs[i]->cancelled, 1);
        SDL_CondBroadcast(worldchunkcond);
        SDL_UnlockMutex(worldchunkmutex);
        {
            ZoneScopedN("Chunks/Join worker threads");
            loopv(worldchunkworkers) SDL_WaitThread(worldchunkworkers[i], NULL);
        }
        worldchunkworkers.setsize(0);
    }

    loopv(worldchunkjobs) delete worldchunkjobs[i];
    worldchunkjobs.setsize(0);
    ASSERT(worldchunkactivejobs.empty());
    loopv(worldchunkresults)
    {
        {
            ZoneScopedN("Chunks/Free queued result");
            ZoneTextF("%d_%d", worldchunkresults[i]->x, worldchunkresults[i]->y);
            game::freeworldchunk(worldchunkresults[i]->root);
        }
        delete worldchunkresults[i];
    }
    worldchunkresults.setsize(0);
    loopv(worldchunksavejobs) delete worldchunksavejobs[i];
    worldchunksavejobs.setsize(0);
    ASSERT(worldchunksaveactivejobs.empty());
    loopv(worldchunksaveresults) delete worldchunksaveresults[i];
    worldchunksaveresults.setsize(0);

    if(worldchunkcond) SDL_DestroyCond(worldchunkcond);
    if(worldchunkmutex) SDL_DestroyMutex(worldchunkmutex);
    worldchunkcond = NULL;
    worldchunkmutex = NULL;
    stopworldchunkthread = false;
    flushingworldchunksaves = false;
}

static void setworldchunkgenerationstopped(bool stopped)
{
    if(stopworldchunkgeneration == stopped) return;
    stopworldchunkgeneration = stopped;
    if(stopped)
    {
        // Keep completed chunks and their mounted geometry, but discard all
        // work that has not been published yet so no new terrain appears
        // after the pause command.
        shutdownworldchunkloader();
        for(int i = worldchunks.length() - 1; i >= 0; --i)
            if(worldchunks[i].loading) worldchunks.removeunordered(i);
        conoutf("procedural chunk loading and generation stopped");
    }
    else
    {
        // Force the next streaming update to rebuild the queue even if the
        // player has stayed in the same chunk while generation was paused.
        lastplayerchunkx = lastplayerchunky = INT_MIN;
        lastchunkdist = -1;
        lastworldchunkpublish = -1;
        conoutf("procedural chunk loading and generation resumed");
    }
}

ICOMMAND(stopchunkgen, "", (), setworldchunkgenerationstopped(!stopworldchunkgeneration));

static void worldsectionrenderstats()
{
    int exterior = 0, interior = 0, mixed = 0, interioronly = 0, entrances = 0, fullysolid = 0, norender = 0;
    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root) continue;
        loop(section, WORLD_SECTION_LAYERS) loop(tile, WORLD_SECTION_TILES)
        {
            const uchar flags = chunk.renderdata.flags[section][tile];
            if(flags&SECTION_EXTERIOR) exterior++;
            if(flags&SECTION_INTERIOR) interior++;
            if((flags&(SECTION_EXTERIOR | SECTION_INTERIOR)) == (SECTION_EXTERIOR | SECTION_INTERIOR)) mixed++;
            if((flags&SECTION_INTERIOR) && !(flags&SECTION_EXTERIOR)) interioronly++;
            if(flags&SECTION_CAVE_ENTRANCE) entrances++;
            if(flags&SECTION_FULLY_SOLID) fullysolid++;
            if(flags&SECTION_NO_RENDER) norender++;
        }
    }
    conoutf(CON_DEBUG,
            "section render classes: exterior %d, interior %d, mixed %d, interior-only %d, cave entrances %d, fully-solid %d, no-render %d",
            exterior, interior, mixed, interioronly, entrances, fullysolid, norender);
}

COMMAND(worldsectionrenderstats, "");

static void worldvaresidencystats()
{
    int states[WORLD_VA_GEOMETRY_COUNT][EVICTABLE + 1];
    memclear(states);
    loopv(worldchunks) loop(section, WORLD_SECTION_LAYERS) loop(tile, WORLD_SECTION_TILES) loop(geometry, WORLD_VA_GEOMETRY_COUNT)
        states[geometry][worldchunks[i].varesidency[section][tile].state[geometry]]++;
    int uploadedbytes = 0, uploadedvertices = 0;
    getworldvauploadstats(uploadedbytes, uploadedvertices);
    conoutf(CON_DEBUG,
            "VA exterior not-resident %d, build %d, upload %d, resident %d, evictable %d; "
            "interior not-resident %d, build %d, upload %d, resident %d, evictable %d",
            states[WORLD_VA_EXTERIOR][NOT_RESIDENT], states[WORLD_VA_EXTERIOR][PENDING_BUILD],
            states[WORLD_VA_EXTERIOR][PENDING_UPLOAD], states[WORLD_VA_EXTERIOR][RESIDENT], states[WORLD_VA_EXTERIOR][EVICTABLE],
            states[WORLD_VA_INTERIOR][NOT_RESIDENT], states[WORLD_VA_INTERIOR][PENDING_BUILD],
            states[WORLD_VA_INTERIOR][PENDING_UPLOAD], states[WORLD_VA_INTERIOR][RESIDENT], states[WORLD_VA_INTERIOR][EVICTABLE]);
    conoutf(CON_DEBUG, "last frame: evictions %d, no-render skips %d, uploaded %d bytes / %d vertices", worldvaevictionsframe,
            worldvanorenderskipsframe, uploadedbytes, uploadedvertices);
}

COMMAND(worldvaresidencystats, "");

static int acquireworldchunksync(int x, int y, int &generated)
{
    int index = findworldchunk(x, y);
    if(index >= 0) return index;

    ZoneScopedN("Chunks/Load synchronous");
    ZoneTextF("%d_%d", x, y);
    vector<worldscatterinstance> scatter;
    worldsectionrenderdata renderdata;
    cube *root = NULL;
    string snapshoterror;
    uint snapshotrevision = 0;
    bool snapshotplayeredited = false;
    const worldsnapshotloadresult snapshotresult = worldfolder[0]
                                                   ? loadworldchunksnapshot(worldfolder, x, y, root, scatter, renderdata, snapshoterror,
                                                                            &snapshotrevision, &snapshotplayeredited, false)
                                                   : WORLD_SNAPSHOT_MISSING;
    if(snapshotresult == WORLD_SNAPSHOT_INVALID)
        conoutf(CON_ERROR, "authoritative chunk %d_%d could not be loaded: %s; regenerating it", x, y, snapshoterror);
    if(!root)
    {
        ZoneScopedN("Chunks/Generate unsaved");
        root = game::generateworldchunk(x, y, &renderdata);
        if(root) game::generateworldscatter(root, x, y, scatter);
        generated++;
    }
    if(root) setworldleavesalpha(root, leavesalpha != 0);
    worldchunk &chunk = worldchunks.add(worldchunk(x, y, root));
    indexworldchunk(worldchunks.length() - 1);
    chunk.scatter.move(scatter);
    chunk.renderdata = renderdata;
    if(snapshotresult == WORLD_SNAPSHOT_LOADED)
    {
        chunk.revision = chunk.savedrevision = snapshotrevision;
        chunk.playeredited = snapshotplayeredited;
    }
    addworldsectionvisibilitychunk(x, y);
    const ivec localorigin = worldchunkorigin(chunk);
    invalidatelocalambient(localorigin, ivec(localorigin).add(ivec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, WORLD_MAP_SIZE)));
    if(snapshotresult != WORLD_SNAPSHOT_LOADED && root && worldfolder[0] && game::islocalworld() && !queueworldchunksave(chunk))
        conoutf(CON_ERROR, "generated chunk %d_%d remains unsaved and is not authoritative", x, y);
    return worldchunks.length() - 1;
}

static void loadinitialworldchunks(int chunkx, int chunky)
{
    ZoneScopedN("Chunks/Load initial chunks");
    ZoneTextF("%d_%d", chunkx, chunky);
    static const int offsets[][2] =
    {
        { 0, 0 }, { -1, 0 }, { 1, 0 }, { 0, -1 },
        { 0, 1 }, { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 }
    };
    // Only the entry chunk blocks map startup. Everything around it uses the
    // same asynchronous path as runtime streaming.
    int target = 1,
        ready = findworldchunk(chunkx, chunky) >= 0 ? 1 : 0, generated = 0;
    renderprogress(target > 0 ? ready / float(target) : 1, "loading nearby chunks...");
    loopi(sizeof(offsets) / sizeof(offsets[0]))
    {
        if(ready >= target) break;
        int x = chunkx + offsets[i][0], y = chunky + offsets[i][1];
        if(abs(offsets[i][0]) > maxchunkdist || abs(offsets[i][1]) > maxchunkdist ||
           findworldchunk(x, y) >= 0)
            continue;
        acquireworldchunkblocking(x, y, generated);
        ready++;
        renderprogress(ready / float(target), "loading nearby chunks...");
    }
}

static int queueworldchunk(int x, int y)
{
    ZoneScopedN("Chunks/Queue chunk");
    ZoneTextF("%d_%d", x, y);
    if(stopworldchunkgeneration) return -1;
    int index = findworldchunk(x, y);
    if(index >= 0) return index;

    if(!startworldchunkloader())
    {
        int generated = 0;
        return acquireworldchunksync(x, y, generated);
    }

    uint request = ++worldchunkrequest;
    if(!request) request = ++worldchunkrequest;
    worldchunkjob *job = new worldchunkjob(x, y, worldchunkepoch, request, worldfolder);

    worldchunk &chunk = worldchunks.add(worldchunk(x, y, NULL, true));
    indexworldchunk(worldchunks.length() - 1);
    chunk.request = request;
    chunk.generating = true;
    SDL_LockMutex(worldchunkmutex);
    worldchunkjobs.add(job);
    TracyPlot("Chunks/Queued jobs", int64_t(worldchunkjobs.length()));
    SDL_CondSignal(worldchunkcond);
    SDL_UnlockMutex(worldchunkmutex);
    return worldchunks.length() - 1;
}

static int acquireworldchunkblocking(int x, int y, int &generated)
{
    int index = findworldchunk(x, y);
    if(index < 0) index = queueworldchunk(x, y);
    if(index < 0) return index;
    while(worldchunks.inrange(index) && worldchunks[index].loading)
    {
        processworldchunkresults();
        index = findworldchunk(x, y);
        if(index < 0 || !worldchunks[index].loading) break;
        SDL_LockMutex(worldchunkmutex);
        SDL_CondWaitTimeout(worldchunkcond, worldchunkmutex, 5);
        SDL_UnlockMutex(worldchunkmutex);
    }
    if(worldchunks.inrange(index) && worldchunks[index].root && !worldchunks[index].savedrevision) generated++;
    return index;
}

static int worldchunkoutstandingjobs()
{
    if(!worldchunkmutex) return 0;
    SDL_LockMutex(worldchunkmutex);
    int outstanding = worldchunkjobs.length() + worldchunkactivejobs.length() + worldchunkresults.length();
    SDL_UnlockMutex(worldchunkmutex);
    return outstanding;
}

static int queueworldchunkview(int chunkx, int chunky, int aheadx, int aheady)
{
    ZoneScopedN("Chunks/Fill load queue");
    ZoneTextF("focus %d_%d ahead %d_%d", chunkx, chunky, aheadx, aheady);
    if(stopworldchunkgeneration) return 0;
    if(!startworldchunkloader()) return 0;
    int viewx, viewy;
    worldchunkviewfocus(chunkx, chunky, viewx, viewy);
    SDL_LockMutex(worldchunkmutex);
    worldchunkfocusx = chunkx;
    worldchunkfocusy = chunky;
    worldchunkaheadx = aheadx;
    worldchunkaheady = aheady;
    worldchunkviewx = viewx;
    worldchunkviewy = viewy;
    SDL_UnlockMutex(worldchunkmutex);

    int queued = 0, outstanding = worldchunkoutstandingjobs(),
        minx = chunkx - maxchunkdist,
        maxx = chunkx + maxchunkdist,
        miny = chunky - maxchunkdist,
        maxy = chunky + maxchunkdist;
    while(outstanding < chunkpendinglimit)
    {
        int bestx = 0, besty = 0, bestscore = INT_MAX;
        bool found = false;
        for(int y = miny; y <= maxy; ++y) for(int x = minx; x <= maxx; ++x)
        {
            if(!worldchunkjobwanted(x, y, chunkx, chunky, aheadx, aheady) ||
               findworldchunk(x, y) >= 0)
                continue;
            int score = worldchunkcoordinatescore(x, y);
            if(score >= bestscore) continue;
            bestx = x;
            besty = y;
            bestscore = score;
            found = true;
        }
        if(!found || queueworldchunk(bestx, besty) < 0) break;
        queued++;
        outstanding++;
    }
    return queued;
}

static int reprioritizeworldchunkqueue(int chunkx, int chunky, int aheadx, int aheady)
{
    ZoneScopedN("Chunks/Reprioritize queue");
    ZoneTextF("focus %d_%d ahead %d_%d", chunkx, chunky, aheadx, aheady);
    int viewx, viewy;
    worldchunkviewfocus(chunkx, chunky, viewx, viewy);
    if(!worldchunkmutex)
    {
        worldchunkfocusx = chunkx;
        worldchunkfocusy = chunky;
        worldchunkaheadx = aheadx;
        worldchunkaheady = aheady;
        worldchunkviewx = viewx;
        worldchunkviewy = viewy;
        return 0;
    }

    int cancelled = 0;
    vector<worldchunkjob *> stale;
    SDL_LockMutex(worldchunkmutex);
    worldchunkfocusx = chunkx;
    worldchunkfocusy = chunky;
    worldchunkaheadx = aheadx;
    worldchunkaheady = aheady;
    worldchunkviewx = viewx;
    worldchunkviewy = viewy;
    for(int i = worldchunkjobs.length() - 1; i >= 0; --i)
    {
        worldchunkjob *job = worldchunkjobs[i];
        if(worldchunkjobwanted(job->x, job->y, chunkx, chunky, aheadx, aheady)) continue;
        delete worldchunkjobs.remove(i);
        cancelled++;
    }
    loopv(worldchunkactivejobs)
    {
        worldchunkjob *job = worldchunkactivejobs[i];
        if(worldchunkjobwanted(job->x, job->y, chunkx, chunky, aheadx, aheady)) continue;
        if(!SDL_AtomicGet(&job->cancelled))
        {
            SDL_AtomicSet(&job->cancelled, 1);
            cancelled++;
        }
    }
    for(int i = worldchunkresults.length() - 1; i >= 0; --i)
    {
        worldchunkjob *job = worldchunkresults[i];
        if(worldchunkjobwanted(job->x, job->y, chunkx, chunky, aheadx, aheady)) continue;
        SDL_AtomicSet(&job->cancelled, 1);
        stale.add(worldchunkresults.remove(i));
        cancelled++;
    }
    if(!stale.empty()) SDL_CondBroadcast(worldchunkcond);
    SDL_UnlockMutex(worldchunkmutex);

    loopv(stale)
    {
        {
            ZoneScopedN("Chunks/Free stale result");
            ZoneTextF("%d_%d", stale[i]->x, stale[i]->y);
            game::freeworldchunk(stale[i]->root);
        }
        delete stale[i];
    }

    // A job already owned by a worker cannot be cancelled safely. Removing
    // its placeholder makes its eventual result self-discard instead of
    // publishing terrain that the camera has already outrun.
    for(int i = worldchunks.length() - 1; i >= 0; --i)
    {
        worldchunk &chunk = worldchunks[i];
        if(!chunk.loading ||
           worldchunkjobwanted(chunk.x, chunk.y, chunkx, chunky, aheadx, aheady))
            continue;
        worldchunks.removeunordered(i);
    }
    return cancelled;
}

static int processworldchunkresults()
{
    if(worldchunkworkers.empty()) return 0;
    ZoneScopedN("Chunks/Process worker results");

    int handled = 0, published = 0, generated = 0, optimized = 0;
    while(handled < asyncchunkloads)
    {
        worldchunkjob *job = NULL;
        {
            ZoneScopedN("Chunks/Dequeue worker result");
            SDL_LockMutex(worldchunkmutex);
            ZoneValue(worldchunkresults.length());
            if(!worldchunkresults.empty())
            {
                int best = -1, bestscore = INT_MAX;
                loopv(worldchunkresults)
                {
                    if(SDL_AtomicGet(&worldchunkresults[i]->cancelled) || !worldchunkresults[i]->root)
                    {
                        best = i;
                        break;
                    }
                    int score = worldchunkjobscore(*worldchunkresults[i]);
                    if(score < bestscore) { best = i; bestscore = score; }
                }
                if(best >= 0) job = worldchunkresults.remove(best);
            }
            TracyPlot("Chunks/Ready results", int64_t(worldchunkresults.length()));
            if(job) SDL_CondSignal(worldchunkcond);
            SDL_UnlockMutex(worldchunkmutex);
        }
        if(!job) break;

        int index = findworldchunk(job->x, job->y);
        bool current = index >= 0 && worldchunks[index].loading &&
                       worldchunks[index].request == job->request;
        if(job->epoch != worldchunkepoch || SDL_AtomicGet(&job->cancelled) ||
           !job->root || !current)
        {
            ZoneScopedN("Chunks/Discard worker result");
            ZoneTextF("%d_%d", job->x, job->y);
            if(current)
                worldchunks.removeunordered(index);
            game::freeworldchunk(job->root);
            delete job;
            continue;
        }
        if(job->snapshotresult == WORLD_SNAPSHOT_LOADED &&
           !game::restorelocalchunkdata(job->x, job->y, job->gameplay.getbuf(), job->gameplay.length()))
        {
            conoutf(CON_ERROR, "authoritative chunk %d_%d contains invalid sparse gameplay data; regenerating it", job->x, job->y);
            game::freeworldchunk(job->root);
            job->root = NULL;
            job->scatter.setsize(0);
            job->gameplay.setsize(0);
            job->snapshotresult = WORLD_SNAPSHOT_INVALID;
            job->sectionstatesready = false;
            SDL_LockMutex(worldchunkmutex);
            worldchunkjobs.add(job);
            SDL_CondSignal(worldchunkcond);
            SDL_UnlockMutex(worldchunkmutex);
            continue;
        }
        handled++;

        {
            ZoneScopedN("Chunks/Publish worker result");
            ZoneTextF("%d_%d generated families %d", job->x, job->y, job->families);
            ZoneValue(job->families);
            worldchunk &chunk = worldchunks[index];
            chunk.root = job->root;
            chunk.scatter.move(job->scatter);
            chunk.renderdata = job->renderdata;
            bool leavesalphamatches = job->leavesalpha == (leavesalpha != 0);
            if(!leavesalphamatches) setworldleavesalpha(chunk.root, leavesalpha != 0);
            if(job->sectionstatesready)
            {
                const uint alltiles = (1U << WORLD_SECTION_TILES) - 1;
                loopi(WORLD_SECTION_LAYERS)
                {
                    chunk.contentknown[i] = alltiles;
                    chunk.contenttiles[i] = job->contenttiles[i];
                    chunk.opaqueknown[i] = leavesalphamatches ? alltiles : 0;
                    chunk.opaquetiles[i] = job->opaquetiles[i];
                    chunk.portalsknown[i] = leavesalphamatches ? alltiles : 0;
                    memcpy(chunk.portals[i], job->portals[i], sizeof(chunk.portals[i]));
                    memcpy(chunk.portalcellmasks[i], job->portalcellmasks[i], sizeof(chunk.portalcellmasks[i]));
                }
            }
            chunk.loading = false;
            chunk.generating = false;
            chunk.revision = job->snapshotresult == WORLD_SNAPSHOT_LOADED ? job->snapshotrevision : 1;
            chunk.savedrevision = job->snapshotresult == WORLD_SNAPSHOT_LOADED ? job->snapshotrevision : 0;
            chunk.playeredited = job->snapshotresult == WORLD_SNAPSHOT_LOADED && job->snapshotplayeredited;
            allocnodes += job->families;
            if(job->snapshotresult != WORLD_SNAPSHOT_LOADED) generated++;
            optimized += job->optimized;
            addworldsectionvisibilitychunk(chunk.x, chunk.y);
            const ivec localorigin = worldchunkorigin(chunk);
            invalidatelocalambient(localorigin, ivec(localorigin).add(ivec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, WORLD_MAP_SIZE)));
            if(job->snapshotresult == WORLD_SNAPSHOT_INVALID)
                conoutf(CON_ERROR, "authoritative chunk %d_%d could not be loaded: %s; regenerated it", chunk.x, chunk.y,
                        job->snapshoterror[0] ? job->snapshoterror : "invalid snapshot");
            if(job->snapshotresult != WORLD_SNAPSHOT_LOADED && worldfolder[0] && game::islocalworld() && !queueworldchunksave(chunk))
                conoutf(CON_ERROR, "generated chunk %d_%d remains unsaved and is not authoritative", chunk.x, chunk.y);
            published++;
            if(!game::islocalworld()) game::requestworldchunk(chunk.x, chunk.y);
        }
        delete job;
    }

    if(published)
        conoutf(CON_DEBUG, "prepared %d chunks asynchronously (%d generated, %d octree families remipped)", published, generated, optimized);
    return published;
}

static int processworldchunksaveresults()
{
    if(!worldchunkmutex) return 0;
    int handled = 0;
    for(;;)
    {
        SDL_LockMutex(worldchunkmutex);
        worldchunksavejob *job = worldchunksaveresults.empty() ? NULL : worldchunksaveresults.remove(0);
        TracyPlot("Chunks/Ready saves", int64_t(worldchunksaveresults.length()));
        SDL_UnlockMutex(worldchunkmutex);
        if(!job) break;

        int index = findworldchunk(job->x, job->y);
        bool resave = false;
        if(job->epoch == worldchunkepoch && worldchunks.inrange(index))
        {
            worldchunk &chunk = worldchunks[index];
            if(chunk.saving && chunk.savingrevision == job->revision)
            {
                chunk.saving = false;
                if(job->success) chunk.savedrevision = job->revision;
                resave = job->success && chunk.savedrevision != chunk.revision;
            }
        }
        if(!job->success)
        {
            worldchunksavefailure = true;
            conoutf(CON_ERROR, "could not save authoritative chunk %d_%d asynchronously: %s", job->x, job->y, job->error[0] ? job->error : "unknown error");
        }
        else if(job->error[0]) conoutf(CON_WARN, "saved chunk %d_%d with warning: %s", job->x, job->y, job->error);
        delete job;
        if(resave && worldchunks.inrange(index) && !queueworldchunksave(worldchunks[index]))
            conoutf(CON_ERROR, "could not queue updated authoritative chunk %d_%d after an older save completed", worldchunks[index].x, worldchunks[index].y);
        handled++;
    }
    return handled;
}

static bool flushworldchunksaves()
{
    if(!worldchunkmutex) return true;
    SDL_LockMutex(worldchunkmutex);
    flushingworldchunksaves = true;
    SDL_CondBroadcast(worldchunkcond);
    SDL_UnlockMutex(worldchunkmutex);

    for(;;)
    {
        processworldchunksaveresults();
        SDL_LockMutex(worldchunkmutex);
        const bool pending = !worldchunksavejobs.empty() || !worldchunksaveactivejobs.empty() || !worldchunksaveresults.empty();
        if(pending) SDL_CondWaitTimeout(worldchunkcond, worldchunkmutex, 10);
        SDL_UnlockMutex(worldchunkmutex);
        if(!pending) break;
    }
    processworldchunksaveresults();
    SDL_LockMutex(worldchunkmutex);
    flushingworldchunksaves = false;
    SDL_UnlockMutex(worldchunkmutex);
    const bool success = !worldchunksavefailure;
    worldchunksavefailure = false;
    return success;
}

static void processworldchunkupdates(int chunkx, int chunky, int aheadx, int aheady)
{
    if(stopworldchunkgeneration || lastworldchunkpublish == totalmillis) return;
    ZoneScopedN("Chunks/Streaming update");
    ZoneTextF("focus %d_%d ahead %d_%d", chunkx, chunky, aheadx, aheady);
    lastworldchunkpublish = totalmillis;
    processworldchunksaveresults();
    reprioritizeworldchunkqueue(chunkx, chunky, aheadx, aheady);
    processworldchunkresults();
    queueworldchunkview(chunkx, chunky, aheadx, aheady);
    mountworldchunksafetyregion(chunkx, chunky);
    processworldchunkchanges(chunkx, chunky);
    pruneworldchunkresidency(chunkx, chunky, INT_MAX);
    activeworldchunk = findworldchunk(chunkx, chunky);
}

static void rebaseworldchunks(int chunkx, int chunky, bool translateplayer = true)
{
    ZoneScopedN("Chunks/Rebase runtime world");
    ZoneTextF("%d_%d", chunkx, chunky);
    invalidateworldsectionvisibility();
    worldchunkvaupdates.setsize(0);
    worldchunkvaupdateset.clear();
    worldchunkvapendingbuilds.setsize(0);
    worldchunkvapendingbuildset.clear();
    clearworldscattererentities();
    loopv(worldchunks) if(worldchunkmounted(worldchunks[i])) unmountworldchunk(worldchunks[i]);
    if(worldsectionowners.numelems)
    {
        conoutf(CON_ERROR, "discarding %d stale runtime section owners during chunk rebase",
                worldsectionowners.numelems);
        worldsectionowners.clear();
    }

    int newfirstx = chunkx - WORLD_RUNTIME_CENTER,
        newfirsty = chunky - WORLD_RUNTIME_CENTER;
    long long shiftx = ((long long)newfirstx - worldfirstchunkx) * WORLD_CHUNK_SIZE,
              shifty = ((long long)newfirsty - worldfirstchunky) * WORLD_CHUNK_SIZE;
    {
        ZoneScopedN("Chunks/Rebase free old octree");
        freeocta(worldroot);
    }
    worldroot = newcubes(F_EMPTY);
    worldfirstchunkx = newfirstx;
    worldfirstchunky = newfirsty;
    if(player && translateplayer)
    {
        player->o.x -= float(shiftx);
        player->o.y -= float(shifty);
    }
    if(shiftx || shifty) game::rebasenpcs(float(shiftx), float(shifty));
    conoutf(CON_DEBUG, "rebased chunk window around %d_%d", chunkx, chunky);
}

static bool worldchunksafetysectionbounds(int &minx, int &maxx, int &miny, int &maxy, int &minz, int &maxz)
{
    if(!player) return false;
    minx = int(floorf((player->o.x - WORLD_SECTION_SIZE * 0.5f) / WORLD_SECTION_SIZE));
    miny = int(floorf((player->o.y - WORLD_SECTION_SIZE * 0.5f) / WORLD_SECTION_SIZE));
    maxx = minx + 1;
    maxy = miny + 1;
    int playersection = clamp(int(floorf(player->o.z / WORLD_SECTION_SIZE)), 0, int(WORLD_SECTION_LAYERS) - 1);
    minz = max(playersection - 1, 0);
    maxz = min(playersection + 1, int(WORLD_SECTION_LAYERS) - 1);
    return true;
}

static void mountworldchunksafetyregion(int chunkx, int chunky, bool updategeometry)
{
    int minx, maxx, miny, maxy, minz, maxz;
    if(!worldchunksafetysectionbounds(minx, maxx, miny, maxy, minz, maxz)) return;
    ZoneScopedN("Chunks/Mount safety region");
    ZoneTextF("%d_%d", chunkx, chunky);
    int changedsections = 0;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || chunk.corrupted || !chunk.root ||
           !worldchunkinview(chunk, chunkx, chunky))
            continue;
        loopj(WORLD_SECTION_TILES)
        {
            int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS,
                worldtilex = (chunk.x - worldfirstchunkx) * WORLD_SECTION_COLUMNS + x,
                worldtiley = (chunk.y - worldfirstchunky) * WORLD_SECTION_COLUMNS + y;
            if(worldtilex < minx || worldtilex > maxx || worldtiley < miny || worldtiley > maxy) continue;
            int sections[3], numsections = 0;
            for(int section = minz; section <= maxz; ++section)
            {
                if(!mountworldchunktile(chunk, section, j)) continue;
                sections[numsections++] = section;
            }
            if(!numsections) continue;
            if(updategeometry) queueworldchunksectionupdates(chunk, j, sections, numsections);
            changedsections += numsections;
        }
    }
    if(changedsections && updategeometry)
    {
        ZoneScopedN("Chunks/Queue safety region geometry");
        ZoneValue(changedsections);
        processworldchunkvaupdates();
    }
}

static int pruneworldchunkresidency(int chunkx, int chunky, int limit)
{
    ZoneScopedN("Chunks/Prune cache");
    ZoneTextF("focus %d_%d limit %d", chunkx, chunky, limit);
    Uint64 start = SDL_GetPerformanceCounter();
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    int released = 0, cachedist = maxchunkdist + chunkcachedist;
    for(int i = worldchunks.length() - 1; i >= 0; --i)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || worldchunkmounted(chunk) || !chunk.root ||
           worldchunkdistance(chunk.x, chunk.y, chunkx, chunky) <= cachedist || game::haslocalchunkdynamicstate(chunk.x, chunk.y))
            continue;
        {
            ZoneScopedN("Chunks/Release cache");
            ZoneTextF("%d_%d", chunk.x, chunk.y);
            if(worldfolder[0] && game::islocalworld() && chunk.savedrevision != chunk.revision && !queueworldchunksave(chunk))
            {
                conoutf(CON_ERROR, "refusing to release unsaved authoritative chunk %d_%d", chunk.x, chunk.y);
                break;
            }
            if(chunk.saving) continue;
            if(game::islocalworld()) game::unloadlocalchunknpcs(chunk.x, chunk.y);
            freeocta(chunk.root);
        }
        worldchunks.removeunordered(i);
        released++;
        double elapsed = (SDL_GetPerformanceCounter() - start) * 1000.0 / frequency;
        if(released >= limit || elapsed >= chunkcleanupbudget) break;
    }
    if(released) invalidateworldsectionvisibility();
    return released;
}

static void rebuildworldchunks(int chunkx, int chunky, int aheadx, int aheady, bool load, bool updategeometry)
{
    if(stopworldchunkgeneration) return;
    ZoneScopedN("Chunks/Rebuild view");
    ZoneTextF("focus %d_%d ahead %d_%d", chunkx, chunky, aheadx, aheady);
    rebuildingworldchunks = true;
    int cancelled = reprioritizeworldchunkqueue(chunkx, chunky, aheadx, aheady),
        queued = load ? 0 : queueworldchunkview(chunkx, chunky, aheadx, aheady);

    vector<int> entering, leaving;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        bool shouldmount = worldchunkinview(chunk, chunkx, chunky);
        if(worldchunkmounted(chunk) && !shouldmount) leaving.add(i);
        else if(!worldchunkmounted(chunk) && !chunk.loading && !chunk.corrupted &&
                chunk.root && shouldmount)
            entering.add(i);
    }

    lastplayerchunkx = chunkx;
    lastplayerchunky = chunky;
    lastchunkdist = maxchunkdist;
    if(load)
    {
        // Bootstrap collision only. Surface and camera-visible sections are
        // published by the normal priority scheduler instead of mounting the
        // entire entry chunk before the first frame.
        mountworldchunksafetyregion(chunkx, chunky, false);
        ZoneScopedN("Chunks/Validate runtime octree");
        validatec(worldroot, worldsize >> 1);
    }
    if(updategeometry)
    {
        if(load)
        {
            ZoneScopedN("Chunks/Rebuild all geometry");
            calcmerges();
            allchanged(worldfolder[0] != '\0');
        }
    }
    // Keep CPU-heavy generation workers out of the synchronous bootstrap.
    // In optimized builds their startup used to overlap VA/material creation.
    if(load) queued = queueworldchunkview(chunkx, chunky, aheadx, aheady);

    int released = pruneworldchunkresidency(chunkx, chunky, 1);
    activeworldchunk = findworldchunk(chunkx, chunky);
    if(worldchunks.inrange(activeworldchunk))
    {
        string name;
        worldchunkname(name, sizeof(name), worldchunks[activeworldchunk]);
        setmapfilenames(name, NULL);
    }

    int mounted = 0;
    loopv(worldchunks) if(worldchunkmounted(worldchunks[i])) mounted++;
    rebuildingworldchunks = false;
    conoutf(CON_DEBUG, "chunk view %d_%d: +%d -%d, %d queued, %d cancelled, %d cached released, %d/%d mounted",
            chunkx, chunky, entering.length(), leaving.length(), queued, cancelled, released,
            mounted, (2 * maxchunkdist + 1) * (2 * maxchunkdist + 1));
}

static void updateworldchunkprediction(int chunkx, int chunky, double absolutex, double absolutey)
{
    if(lastworldchunkmotion < 0)
    {
        lastworldchunkposx = absolutex;
        lastworldchunkposy = absolutey;
        lastworldchunkmotion = totalmillis;
        worldchunkvelocityx = worldchunkvelocityy = 0;
        worldchunkaheadx = chunkx;
        worldchunkaheady = chunky;
        return;
    }

    if(totalmillis > lastworldchunkmotion)
    {
        int elapsed = totalmillis - lastworldchunkmotion;
        if(elapsed > 500)
        {
            worldchunkvelocityx = worldchunkvelocityy = 0;
        }
        else
        {
            float samplex = float((absolutex - lastworldchunkposx) / elapsed),
                  sampley = float((absolutey - lastworldchunkposy) / elapsed);
            worldchunkvelocityx = worldchunkvelocityx * 0.65f + samplex * 0.35f;
            worldchunkvelocityy = worldchunkvelocityy * 0.65f + sampley * 0.35f;
        }
        lastworldchunkposx = absolutex;
        lastworldchunkposy = absolutey;
        lastworldchunkmotion = totalmillis;
    }

    if(chunklookahead <= 0)
    {
        worldchunkaheadx = chunkx;
        worldchunkaheady = chunky;
        return;
    }

    const float horizon = 750.0f;
    int predictedx = int(floor((absolutex + worldchunkvelocityx * horizon) / WORLD_CHUNK_SIZE)),
        predictedy = int(floor((absolutey + worldchunkvelocityy * horizon) / WORLD_CHUNK_SIZE));
    worldchunkaheadx = chunkx + clamp(predictedx - chunkx, -chunklookahead, chunklookahead);
    worldchunkaheady = chunky + clamp(predictedy - chunky, -chunklookahead, chunklookahead);
}

void updateworldchunks(bool force)
{
    if(worldchunks.empty() || rebuildingworldchunks || !worldroot) return;
    ZoneScopedN("Chunks/Update world chunks");
    if(stopworldchunkgeneration) return;

    int localchunkx = 0, localchunky = 0;
    if(player)
    {
        localchunkx = int(floor(player->o.x / WORLD_CHUNK_SIZE));
        localchunky = int(floor(player->o.y / WORLD_CHUNK_SIZE));
    }
    int chunkx = worldfirstchunkx + localchunkx,
        chunky = worldfirstchunky + localchunky;
    double absolutex = double(worldfirstchunkx) * WORLD_CHUNK_SIZE + (player ? player->o.x : 0),
           absolutey = double(worldfirstchunky) * WORLD_CHUNK_SIZE + (player ? player->o.y : 0);
    updateworldchunkprediction(chunkx, chunky, absolutex, absolutey);
    if(!force) processworldchunkupdates(chunkx, chunky, worldchunkaheadx, worldchunkaheady);
    updateworldlods(chunkx, chunky, force);
    if(!force && chunkx == lastplayerchunkx && chunky == lastplayerchunky &&
       maxchunkdist == lastchunkdist)
    {
        updateworldscatterers();
        return;
    }

    int viewdist = maxchunkdist;
    bool rebase = localchunkx - viewdist <= 0 || localchunkx + viewdist >= WORLD_RUNTIME_CHUNKS - 1 ||
                  localchunky - viewdist <= 0 || localchunky + viewdist >= WORLD_RUNTIME_CHUNKS - 1;
    if(rebase)
    {
        rebaseworldchunks(chunkx, chunky);
        mountworldchunksafetyregion(chunkx, chunky);
    }
    rebuildworldchunks(chunkx, chunky, worldchunkaheadx, worldchunkaheady, force && !rebase, true);
    updateworldscatterers();
}

static bool parseworldcoordinate(const char *text, double &coordinate)
{
    if(!text || !*text) return false;
    const char *number = text;
    if(*number == '+' || *number == '-') ++number;
    if(!isdigit(*number) && *number != '.') return false;

    char *end = NULL;
    errno = 0;
    coordinate = strtod(text, &end);
    return errno != ERANGE && end != text && !*end &&
           coordinate >= -DBL_MAX && coordinate <= DBL_MAX;
}

static void teleportplayer(char *xtext, char *ytext, char *ztext)
{
    if(!player)
    {
        conoutf(CON_ERROR, "teleport: no player is available");
        return;
    }

    double x, y, z;
    if(!parseworldcoordinate(xtext, x) || !parseworldcoordinate(ytext, y) ||
       !parseworldcoordinate(ztext, z))
    {
        conoutf(CON_ERROR, "usage: /teleport <absolute x> <absolute y> <absolute z>");
        return;
    }

    if(worldchunks.empty())
    {
        if(x < 0 || x >= worldsize || y < 0 || y >= worldsize ||
           z < 0 || z >= worldsize)
        {
            conoutf(CON_ERROR, "teleport: coordinates must be inside this map (0 <= x, y, z < %d)",
                    worldsize);
            return;
        }

        player->o = vec(float(x), float(y), float(z));
        player->reset();
        player->resetinterp();
        conoutf("teleported to %.2f %.2f %.2f", x, y, z);
        return;
    }

    if(z < 0 || z >= WORLD_MAP_SIZE)
    {
        conoutf(CON_ERROR, "teleport: z must be in the generated world band (0 <= z < %d)",
                WORLD_MAP_SIZE);
        return;
    }

    double chunkxd = floor(x / WORLD_CHUNK_SIZE),
           chunkyd = floor(y / WORLD_CHUNK_SIZE);
    const int chunkmargin = max(int(WORLD_RUNTIME_CENTER), maxchunkdist) + 1,
              minchunk = INT_MIN + chunkmargin,
              maxchunk = INT_MAX - chunkmargin;
    if(chunkxd < minchunk || chunkxd > maxchunk ||
       chunkyd < minchunk || chunkyd > maxchunk)
    {
        double mincoordinate = double(minchunk) * WORLD_CHUNK_SIZE,
               maxcoordinate = double(maxchunk + 1LL) * WORLD_CHUNK_SIZE;
        conoutf(CON_ERROR,
                "teleport: x and y must be in the safe streamed range [%.0f, %.0f)",
                mincoordinate, maxcoordinate);
        return;
    }

    int chunkx = int(chunkxd), chunky = int(chunkyd);

    // A teleport can invalidate every queued streaming request. Stop the
    // workers and remove their placeholders before preparing the destination
    // synchronously, ensuring collision exists as soon as the player arrives.
    shutdownworldchunkloader();
    for(int i = worldchunks.length() - 1; i >= 0; --i)
        if(worldchunks[i].loading) worldchunks.removeunordered(i);

    int generated = 0;
    int destination = acquireworldchunkblocking(chunkx, chunky, generated);
    if(!worldchunks.inrange(destination) || !worldchunks[destination].root)
    {
        conoutf(CON_ERROR, "teleport: could not prepare destination chunk %d_%d",
                chunkx, chunky);
        return;
    }

    rebaseworldchunks(chunkx, chunky, false);
    player->o = vec(float(x - double(worldfirstchunkx) * WORLD_CHUNK_SIZE),
                    float(y - double(worldfirstchunky) * WORLD_CHUNK_SIZE),
                    float(z));
    player->reset();
    player->resetinterp();

    lastworldchunkmotion = -1;
    worldchunkaheadx = chunkx;
    worldchunkaheady = chunky;
    worlddebugcachemillis = -1;
    rebuildworldchunks(chunkx, chunky, chunkx, chunky, true, true);

    conoutf("teleported to absolute %.2f %.2f %.2f (chunk %d_%d%s)",
            x, y, z, chunkx, chunky, generated ? ", generated" : "");
}

COMMANDN(teleport, teleportplayer, "sss");

static int worldmidedge(const ivec &a, const ivec &b, int xd, int yd, bool &perfect)
{
    int ax = a[xd], ay = a[yd], bx = b[xd], by = b[yd];
    if(ay == by) return ay;
    if(ax == bx) { perfect = false; return ay; }
    bool crossx = (ax < 8 && bx > 8) || (ax > 8 && bx < 8),
         crossy = (ay < 8 && by > 8) || (ay > 8 && by < 8);
    if(crossy && !crossx) { worldmidedge(a, b, yd, xd, perfect); return 8; }
    if(ax <= 8 && bx <= 8) return ax > bx ? ay : by;
    if(ax >= 8 && bx >= 8) return ax < bx ? ay : by;
    int risex = (by - ay) * (8 - ax) * 256,
        s = risex / (bx - ax),
        y = s / 256 + ay;
    if((abs(s) & 0xFF) || (crossy && y != 8) || y < 0 || y > 16) perfect = false;
    return crossy ? 8 : clamp(y, 0, 16);
}

static inline bool worldcrosscenter(const ivec &a, const ivec &b, int xd, int yd)
{
    int ax = a[xd], ay = a[yd], bx = b[xd], by = b[yd];
    return (((ax <= 8 && bx <= 8) || (ax >= 8 && bx >= 8)) &&
            ((ay <= 8 && by <= 8) || (ay >= 8 && by >= 8))) ||
           (ax + bx == 16 && ay + by == 16);
}

// Worker-safe counterpart of subdividecube(). Temporary candidate children
// are deliberately detached from allocnodes and renderer-owned cubeext state.
static bool subdivideworldmip(const cube &c, cube *children)
{
    if(isempty(c) || isentirelysolid(c))
    {
        loopi(8)
        {
            resetworldcube(children[i]);
            if(isentirelysolid(c)) solidfaces(children[i]);
            children[i].material = c.material;
            loopj(6) children[i].texture[j] = c.texture[j];
        }
        return true;
    }

    loopi(8)
    {
        resetworldcube(children[i]);
        solidfaces(children[i]);
        children[i].material = c.material;
    }
    bool perfect = true;
    ivec v[8];
    loopi(8)
    {
        cube &source = const_cast<cube &>(c);
        v[i].x = edgeget(cubeedge(source, 0, (i >> R[0]) & 1, (i >> C[0]) & 1), (i >> D[0]) & 1);
        v[i].y = edgeget(cubeedge(source, 1, (i >> R[1]) & 1, (i >> C[1]) & 1), (i >> D[1]) & 1);
        v[i].z = edgeget(cubeedge(source, 2, (i >> R[2]) & 1, (i >> C[2]) & 1), (i >> D[2]) & 1);
        v[i].mul(2);
    }

    loopj(6)
    {
        int d = dimension(j), z = dimcoord(j);
        const ivec &v00 = v[octaindex(d, 0, 0, z)],
                   &v10 = v[octaindex(d, 1, 0, z)],
                   &v01 = v[octaindex(d, 0, 1, z)],
                   &v11 = v[octaindex(d, 1, 1, z)];
        int e[3][3];
        e[0][0] = v00[d];
        e[0][2] = v01[d];
        e[2][0] = v10[d];
        e[2][2] = v11[d];
        e[0][1] = worldmidedge(v00, v01, C[d], d, perfect);
        e[1][0] = worldmidedge(v00, v10, R[d], d, perfect);
        e[1][2] = worldmidedge(v11, v01, R[d], d, perfect);
        e[2][1] = worldmidedge(v11, v10, C[d], d, perfect);
        bool p1 = perfect, p2 = perfect;
        int c1 = worldmidedge(v00, v11, R[d], d, p1),
            c2 = worldmidedge(v01, v10, R[d], d, p2);
        if(z ? c1 > c2 : c1 < c2)
        {
            e[1][1] = c1;
            perfect = p1 && (c1 == c2 || worldcrosscenter(v00, v11, C[d], R[d]));
        }
        else
        {
            e[1][1] = c2;
            perfect = p2 && (c1 == c2 || worldcrosscenter(v01, v10, C[d], R[d]));
        }

        loopi(8)
        {
            children[i].texture[j] = c.texture[j];
            int rd = (i >> R[d]) & 1, cd = (i >> C[d]) & 1, dd = (i >> D[d]) & 1;
            edgeset(cubeedge(children[i], d, 0, 0), z, clamp(e[rd][cd] - dd * 8, 0, 8));
            edgeset(cubeedge(children[i], d, 1, 0), z, clamp(e[1 + rd][cd] - dd * 8, 0, 8));
            edgeset(cubeedge(children[i], d, 0, 1), z, clamp(e[rd][1 + cd] - dd * 8, 0, 8));
            edgeset(cubeedge(children[i], d, 1, 1), z, clamp(e[1 + rd][1 + cd] - dd * 8, 0, 8));
        }
    }

    // validatec() normally performs this leaf validation, but it may touch the
    // global allocator. Candidate children never contain descendants.
    loopi(8) loopj(3)
    {
        uint f = children[i].faces[j], e0 = f & 0x0F0F0F0FU, e1 = (f >> 4) & 0x0F0F0F0FU;
        if(e0 == e1 || ((e1 + 0x07070707U) | (e1 - e0)) & 0xF0F0F0F0U)
        {
            emptyfaces(children[i]);
            break;
        }
    }
    return perfect;
}

static const cube *lookupworldmipneighbour(cube *root, int orient, const ivec &co, int size,
                                          ivec &origin, int &neighboursize)
{
    ivec position = co;
    int dim = dimension(orient);
    if(dimcoord(orient)) position[dim] += size;
    else position[dim] -= size;
    if(position[dim] < 0 || position[dim] >= WORLD_CHUNK_MAP_SIZE) return NULL;

    int scale = WORLD_CHUNK_SCALE - 1;
    const cube *neighbour = &root[octastep(position.x, position.y, position.z, scale)];
    while(!(size >> scale) && neighbour->children)
    {
        --scale;
        neighbour = &neighbour->children[octastep(position.x, position.y, position.z, scale)];
    }
    origin = ivec(position).mask(~0U << scale);
    neighboursize = 1 << scale;
    return neighbour;
}

static bool getworldmipcubeindex(const cube &c, int &worldindex)
{
    worldindex = -1;
    loopi(8)
    {
        const cube &child = c.children[i];
        if(isempty(child)) continue;
        const int index = getworldcubebytextures(child.texture);
        if(index < 0 || (worldindex >= 0 && index != worldindex)) return false;
        worldindex = index;
    }
    return true;
}

static bool remipworldchunk(cube &c, const ivec &co, int size, cube *root,
                            bool prepared, int &families, int &merged, const worldchunkdirtybounds *dirty)
{
    cube *children = c.children;
    if(!children) return true;

    bool perfect = true;
    loopi(8)
    {
        const ivec childorigin(i, co, size), childend = ivec(childorigin).add(size);
        if(dirty && (childend.x <= dirty->minimum.x || childend.y <= dirty->minimum.y || childend.z <= dirty->minimum.z ||
                     childorigin.x >= dirty->maximum.x || childorigin.y >= dirty->maximum.y || childorigin.z >= dirty->maximum.z))
        {
            perfect = false;
            continue;
        }
        if(!remipworldchunk(children[i], childorigin, size >> 1, root, prepared, families, merged, dirty)) perfect = false;
    }

    // A mip leaf is one gameplay block. Grass, dirt, sand, stone, and every other type must remain separate even while their shared faces are buried.
    int worldindex;
    const bool sameworldblock = getworldmipcubeindex(c, worldindex);
    solidfaces(c);
    loopi(6) c.texture[i] = getmippedtexture(c, i);
    if(sameworldblock && worldindex >= 0) loopi(6) c.texture[i] = getworldcubefaceslot(worldindex, i);
    if(!perfect || !sameworldblock || (size << 1) > 0x1000) return false;

    ushort material = MAT_AIR;
    loopi(8)
    {
        material = children[i].material;
        if((material & MATF_CLIP) == MAT_NOCLIP || material & MAT_ALPHA)
        {
            if(i > 0) return false;
            while(++i < 8) if(children[i].material != material) return false;
            break;
        }
        else if(!isentirelysolid(children[i]))
        {
            while(++i < 8)
            {
                int othermaterial = children[i].material;
                if(isentirelysolid(children[i])
                    ? (othermaterial & MATF_CLIP) == MAT_NOCLIP || othermaterial & MAT_ALPHA
                    : material != othermaterial)
                    return false;
            }
            break;
        }
    }

    cube candidate = c;
    candidate.ext = NULL;
    forcemip(candidate);
    candidate.children = NULL;
    cube reconstructed[8];
    if(!subdivideworldmip(candidate, reconstructed)) return false;

    uchar visible[6] = { 0, 0, 0, 0, 0, 0 };
    loopi(8)
    {
        if(children[i].faces[0] != reconstructed[i].faces[0] ||
           children[i].faces[1] != reconstructed[i].faces[1] ||
           children[i].faces[2] != reconstructed[i].faces[2])
            return false;
        if(isempty(children[i]) && isempty(reconstructed[i])) continue;

        ivec childorigin(i, co, size);
        loopj(6)
        {
            ivec neighbourorigin;
            int neighboursize;
            const cube *neighbour = lookupworldmipneighbour(root, j, childorigin, size,
                                                            neighbourorigin, neighboursize);
            if(neighbour && !visiblefaceagainst(children[i], j, childorigin, size,
                                                *neighbour, neighbourorigin, neighboursize,
                                                MAT_AIR, (material & MAT_ALPHA) ^ MAT_ALPHA, MAT_ALPHA))
                continue;
            if(children[i].texture[j] != candidate.texture[j]) return false;
            visible[j] |= 1 << i;
        }
    }

    delete[] children;
    if(prepared) families--;
    else allocnodes--;
    c.children = NULL;
    loopi(3) c.faces[i] = candidate.faces[i];
    c.material = material;
    c.visible = 0;
    loopi(6) if(visible[i]) c.visible |= 1 << i;
    if(c.visible) c.visible |= 0x40;
    c.merged = 0;
    merged++;
    return true;
}

static int remipworldchunkbounded(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled, const worldchunkdirtybounds *dirty)
{
    int merged = 0;
    loopi(8)
    {
        if(cancelled && SDL_AtomicGet(cancelled)) break;
        const ivec origin(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE), end = ivec(origin).add(WORLD_CHUNK_ROOT_SIZE);
        if(dirty && (end.x <= dirty->minimum.x || end.y <= dirty->minimum.y || end.z <= dirty->minimum.z || origin.x >= dirty->maximum.x ||
                     origin.y >= dirty->maximum.y || origin.z >= dirty->maximum.z))
            continue;
        remipworldchunk(root[i], origin, WORLD_CHUNK_ROOT_SIZE >> 1, root, prepared, families, merged, dirty);
    }
    return merged;
}

int remipworldchunk(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled)
{
    return remipworldchunkbounded(root, prepared, families, cancelled, NULL);
}

static cube *prepareworldchunk(worldchunkjob &job)
{
    ZoneScopedN("Chunks/Prepare");
    ZoneTextF("%d_%d", job.x, job.y);
    if(SDL_AtomicGet(&job.cancelled)) return NULL;
    {ZoneScopedN("Chunks/Generate unsaved");}
    cube *root = game::generateworldchunk(job.generation, job.x, job.y, job.families, job.optimized, &job.renderdata);
    if(!root) return NULL;
    game::generateworldscatter(job.generation, root, job.x, job.y, job.scatter);
    setworldleavesalpha(root, job.leavesalpha);
    return root;
}


#endif
