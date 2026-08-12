// worldstream.cpp: generic infinite-world streaming and chunk residency

#ifdef WORLDIO_MODULE_IMPLEMENTATION

static void invalidateworldsectionvisibility();
static void addworldsectionvisibilitychunk(int x, int y);
static ivec worldorientnormal(int orient);
static int processworldchunkchanges(int chunkx, int chunky);
static int worldcubesectionstate(const cube &c);
static void setworldchunksectioncontent(worldchunk &chunk, int tile, int section, bool content);
static void setworldchunksectionopaque(worldchunk &chunk, int tile, int section, bool opaque);
static void queueworldchunksectionupdates(const worldchunk &chunk, int tile, const int *sections, int numsections);
static int processworldchunkvaupdates();
static void applyworlddiffnode(cube *root, const worlddiffnode &node, bool prepared, int &families);
static void applyworldscatterchange(vector<worldscatterinstance> &scatter, const vector<worldscatterinstance> &before,
                                    const vector<worldscatterinstance> &after);

static bool worldchunkmounted(const worldchunk &chunk);
static int worldchunkvaupdatekey(const ivec &origin);

VARP(chunkremip, 0, 1, 1); // optional CPU-for-memory octree collapse on generation/load

worldchunkjob::worldchunkjob(int x, int y, uint epoch, uint request)
    : x(x), y(y), families(0), optimized(0), loaderror(0), revision(0), canonicalhash(0), epoch(epoch), request(request), loaded(false),
      remip(chunkremip != 0), leavesalpha(::leavesalpha != 0), sectionstatesready(false), root(NULL), generation(NULL)
{
    memclear(contenttiles);
    memclear(opaquetiles);
    memclear(portals);
    memclear(portalcellmasks);
    SDL_AtomicSet(&cancelled, 0);
    generation = game::createworldgeneration(true, remip, &cancelled);
    filename[0] = '\0';
}

worldchunkjob::~worldchunkjob()
{
    game::destroyworldgeneration(generation);
}

static vector<worldchunk> worldchunks;
static vector<worldscatterinstance> reconstructedworldscatter;
static bool reconstructedworldscatterready = false;
static vector<worldchunkjob *> worldchunkjobs, worldchunkactivejobs, worldchunkresults;
static vector<worldchunkdiffstate *> worldchunkdiffstates;
static string worldfolder = "";
static bool applyloadworlddefaults = false;
static int activeworldchunk = -1;
static int worldfirstchunkx = 0, worldfirstchunky = 0;
static int lastplayerchunkx = INT_MIN, lastplayerchunky = INT_MIN, lastchunkdist = -1;
static bool rebuildingworldchunks = false;
static bool suppressworldchunkdirty = false;
static vector<SDL_Thread *> worldchunkworkers;
static SDL_mutex *worldchunkmutex = NULL;
static SDL_cond *worldchunkcond = NULL;
static bool stopworldchunkthread = false;
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
static hashtable<int, worldsectionowner> worldsectionowners(1<<15);
static hashtable<ivec, int> worldchunkindices(1<<14);
static vector<ivec> worldsectionvisibilityadditions;
static bool worldsectionvisibilitydirty = true;
static int worldsectionvisibilitychunkx = INT_MIN, worldsectionvisibilitychunky = INT_MIN,
           worldsectionvisibilitymaxdist = -1;
static ivec worldsectionvisibilityfocus(INT_MIN, INT_MIN, INT_MIN);
static float worldchunkvasectionmillis = 2.0f;
static worlddiffmetadata activeworldmetadata;
static int worldeditauthor = -1, lastworlddiffflush = 0;
static ullong worldeditrevision = 0, incomingworldeditrevision = 0;
static vector<worldeditrecord *> worldredostack;

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
VARP(chunkcachedist, 0, 0, 0);
VARP(chunkpendinglimit, 4, 8, 16);
VARP(chunklookahead, 0, 2, 8);
VARP(chunkpublishbudget, 2, 6, 33);
VARP(chunkcleanupbudget, 1, 6, 33);
VARP(chunksectionbatch, 1, 1, WORLD_MAX_SECTION_BATCH);
VARP(chunkvastagelimit, 1, 6, 16);
VARP(drawfullchunk, 0, 0, 1);

static cube *prepareworldchunk(worldchunkjob &job);
static cube *newpreparedfamily(int &families);
static int worldchunkloader(void *);
static void shutdownworldchunkloader();
static void updateworldscatterers();
static void clearworldscattererentities();
static int findworldchunk(int x, int y);
int remipworldchunk(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled = NULL);
static bool subdivideworldmip(const cube &c, cube *children);
static bool prepareworldchunksectionstates(worldchunkjob &job);
static int pruneworldchunkcache(int chunkx, int chunky, int limit);
static bool saveworldconfig();
static void worldchunkname(char *name, size_t len, const worldchunk &chunk);
static worldchunkdiffstate *findworldchunkdiffstate(int x, int y, bool create = false);
static bool applyworldchunkdiff(cube *root, int x, int y, const char *filename, vector<worldscatterinstance> &scatter, bool prepared, int &families,
                                ullong &revision, ullong &canonicalhash);
static ullong hashworldchunk(cube *root);
static void flushworlddiffjournals(bool force = false);
static bool compactworldchunkdiff(worldchunk &chunk);
static void shutdownworlddiffwriter();
static ullong currentworldparameterhash();
static bool loadworldmetadata(const char *folder, int &chunkx, int &chunky, worldspawnmetadata &spawn, worlddiffmetadata &metadata);
void setmapfilenames(const char *fname, const char *cname);

int getworldsectionsize()
{
    return worldchunks.empty() ? 0 : WORLD_SECTION_SIZE;
}

static vector<uchar> worldskytransparent, worldskylight;
static vector<int> worldskyqueue;
static ivec worldskyorigin(0, 0, 0);
static int worldskydiameter = 0;

static void clearworldskyexposure()
{
    worldskytransparent.setsize(0);
    worldskylight.setsize(0);
    worldskyqueue.setsize(0);
    worldskydiameter = 0;
}

VARFP(skyexposureradius, 4, 16, 64, clearworldskyexposure());
VARFP(skyexposureattenuation, 1, 16, 255, clearworldskyexposure());

static bool worldskylighttransparent(const cube &c)
{
    return isempty(c) || (c.material&MATF_VOLUME) == MAT_GLASS || isworldleaftexture(c);
}

static cube sampleworldskylightcube(const cube &source, const ivec &position, ivec &origin, int &size)
{
    cube sampled = source;
    sampled.children = NULL;
    sampled.ext = NULL;
    // Remipping may collapse partial terrain across several blocks. Rebuild only
    // the sampled branch so the vertical scan cannot skip the whole coarse leaf.
    while(size > WORLD_BLOCK_SIZE && !worldskylighttransparent(sampled) && !isentirelysolid(sampled))
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

static bool worldskyfieldcontains(int blockx, int blocky)
{
    if(!worldskydiameter) return false;
    const int margin = max((worldskydiameter - 1) / 4, 1),
              worldblocks = worldsize / WORLD_BLOCK_SIZE;
    const bool insideleft = worldskyorigin.x == 0 ? blockx >= 0 : blockx >= worldskyorigin.x + margin,
               insideright = worldskyorigin.x + worldskydiameter >= worldblocks ? blockx < worldblocks
                                                                                : blockx < worldskyorigin.x + worldskydiameter - margin,
               insidefront = worldskyorigin.y == 0 ? blocky >= 0 : blocky >= worldskyorigin.y + margin,
               insideback = worldskyorigin.y + worldskydiameter >= worldblocks ? blocky < worldblocks
                                                                               : blocky < worldskyorigin.y + worldskydiameter - margin;
    return insideleft && insideright && insidefront && insideback;
}

static void invalidateworldskyexposure(const ivec &bbmin, const ivec &bbmax)
{
    if(!worldskydiameter) return;
    const ivec fieldmin(worldskyorigin.x * WORLD_BLOCK_SIZE, worldskyorigin.y * WORLD_BLOCK_SIZE, 0);
    const ivec fieldmax((worldskyorigin.x + worldskydiameter) * WORLD_BLOCK_SIZE, (worldskyorigin.y + worldskydiameter) * WORLD_BLOCK_SIZE,
                        WORLD_MAP_SIZE);
    if(bbmax.x > fieldmin.x && bbmin.x < fieldmax.x && bbmax.y > fieldmin.y && bbmin.y < fieldmax.y && bbmax.z > fieldmin.z && bbmin.z < fieldmax.z)
        clearworldskyexposure();
}

static void buildworldskyexposure(int blockx, int blocky)
{
    ZoneScopedN("World/Six-direction skylight");
    const int worldblocks = worldsize / WORLD_BLOCK_SIZE,
              radius = min(skyexposureradius, max((worldblocks - 1) / 2, 0)),
              diameter = 2 * radius + 1,
              plane = diameter * diameter,
              cellcount = plane * WORLD_HEIGHT_BLOCKS;
    if(diameter <= 0 || cellcount <= 0)
    {
        clearworldskyexposure();
        return;
    }

    worldskyorigin.x = clamp(blockx - radius, 0, max(worldblocks - diameter, 0));
    worldskyorigin.y = clamp(blocky - radius, 0, max(worldblocks - diameter, 0));
    worldskyorigin.z = 0;
    worldskydiameter = diameter;
    worldskytransparent.setsize(0);
    worldskylight.setsize(0);
    worldskytransparent.pad(cellcount);
    worldskylight.pad(cellcount);
    worldskyqueue.setsize(0);
    worldskyqueue.reserve(cellcount);
    memset(worldskytransparent.getbuf(), 0, cellcount);
    memset(worldskylight.getbuf(), 0, cellcount);

    loop(y, diameter) loop(x, diameter)
    {
        bool directsky = true;
        for(int z = WORLD_HEIGHT_BLOCKS - 1; z >= 0;)
        {
            const ivec center((worldskyorigin.x + x) * WORLD_BLOCK_SIZE + WORLD_BLOCK_SIZE / 2,
                              (worldskyorigin.y + y) * WORLD_BLOCK_SIZE + WORLD_BLOCK_SIZE / 2,
                              z * WORLD_BLOCK_SIZE + WORLD_BLOCK_SIZE / 2);
            ivec cubeorigin;
            int cubesize;
            const cube c = sampleworldskylightcube(lookupcube(center, 0, cubeorigin, cubesize), center, cubeorigin, cubesize);
            const bool transparent = worldskylighttransparent(c);
            int bottom = cubesize >= WORLD_BLOCK_SIZE ? cubeorigin.z / WORLD_BLOCK_SIZE : z;
            bottom = clamp(bottom, 0, z);
            if(!transparent) directsky = false;

            for(; z >= bottom; --z)
            {
                if(!transparent) continue;
                const int index = (z * diameter + y) * diameter + x;

                worldskytransparent[index] = 1;
                if(directsky)
                {
                    worldskylight[index] = 255;
                    worldskyqueue.add(index);
                }
            }
        }
    }

    for(int cursor = 0; cursor < worldskyqueue.length(); ++cursor)
    {
        const int index = worldskyqueue[cursor],
                  light = worldskylight[index];
        if(light <= skyexposureattenuation) continue;
        const int propagated = light - skyexposureattenuation,
                  z = index / plane,
                  offset = index - z * plane,
                  y = offset / diameter,
                  x = offset - y * diameter;

        #define PROPAGATESKYLIGHT(neighbor) do { \
            const int next = (neighbor); \
            if(worldskytransparent[next] && worldskylight[next] < propagated) \
            { \
                worldskylight[next] = propagated; \
                worldskyqueue.add(next); \
            } \
        } while(0)

        if(x > 0) PROPAGATESKYLIGHT(index - 1);
        if(x + 1 < diameter) PROPAGATESKYLIGHT(index + 1);
        if(y > 0) PROPAGATESKYLIGHT(index - diameter);
        if(y + 1 < diameter) PROPAGATESKYLIGHT(index + diameter);
        if(z > 0) PROPAGATESKYLIGHT(index - plane);
        if(z + 1 < WORLD_HEIGHT_BLOCKS) PROPAGATESKYLIGHT(index + plane);

        #undef PROPAGATESKYLIGHT
    }
    worldskyqueue.setsize(0);
}

static float sampleworldskylight(float x, float y, float z)
{
    const int diameter = worldskydiameter, plane = diameter * diameter;
    float positions[3] =
    {
        x / WORLD_BLOCK_SIZE - worldskyorigin.x - 0.5f,
        y / WORLD_BLOCK_SIZE - worldskyorigin.y - 0.5f,
        z / WORLD_BLOCK_SIZE - 0.5f
    };
    int lower[3], upper[3];
    float blend[3];
    const int limits[3] = { diameter, diameter, WORLD_HEIGHT_BLOCKS };
    loopi(3)
    {
        positions[i] = clamp(positions[i], 0.0f, float(limits[i] - 1));
        lower[i] = int(floorf(positions[i]));
        upper[i] = min(lower[i] + 1, limits[i] - 1);
        blend[i] = positions[i] - lower[i];
    }

    float samples[2][2][2];
    loop(zindex, 2) loop(yindex, 2) loop(xindex, 2)
    {
        const int sx = xindex ? upper[0] : lower[0],
                  sy = yindex ? upper[1] : lower[1],
                  sz = zindex ? upper[2] : lower[2];
        samples[zindex][yindex][xindex] = worldskylight[sz * plane + sy * diameter + sx] / 255.0f;
    }

    float layers[2];
    loop(zindex, 2)
    {
        const float low = samples[zindex][0][0] + (samples[zindex][0][1] - samples[zindex][0][0]) * blend[0],
                    high = samples[zindex][1][0] + (samples[zindex][1][1] - samples[zindex][1][0]) * blend[0];
        layers[zindex] = low + (high - low) * blend[1];
    }
    return layers[0] + (layers[1] - layers[0]) * blend[2];
}

float getworldskyexposure(const vec &position)
{
    if(worldchunks.empty() || !worldroot || !insideworld(position) || position.z < 0 || position.z >= WORLD_MAP_SIZE)
        return 1.0f;

    const int blockx = int(floorf(position.x / WORLD_BLOCK_SIZE)),
              blocky = int(floorf(position.y / WORLD_BLOCK_SIZE));
    if(!worldskyfieldcontains(blockx, blocky)) buildworldskyexposure(blockx, blocky);
    return worldskydiameter ? sampleworldskylight(position.x, position.y, position.z) : 1.0f;
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
    flushworlddiffjournals(true);
    shutdownworlddiffwriter();
    cancelworldedit();
    clearworldscattererentities();
    shutdownworldchunkloader();
    worldchunkvaupdates.setsize(0);
    worldchunkvaupdateset.clear();
    worldsectionowners.clear();
    worldchunkindices.clear();
    invalidateworldsectionvisibility();
    worldsectionvisibilitychunkx = worldsectionvisibilitychunky = INT_MIN;
    worldsectionvisibilitymaxdist = -1;
    worldsectionvisibilityfocus = ivec(INT_MIN, INT_MIN, INT_MIN);
    clearworldskyexposure();
    loopv(worldchunks) if(worldchunks[i].root && worldchunks[i].root != worldroot)
    {
        ZoneScopedN("Chunks/Free chunk during clear");
        ZoneTextF("%d_%d", worldchunks[i].x, worldchunks[i].y);
        freeocta(worldchunks[i].root);
    }
    worldchunks.setsize(0);
    reconstructedworldscatter.setsize(0);
    reconstructedworldscatterready = false;
    worldchunkdiffstates.deletecontents();
    worldredostack.deletecontents();
    worldfolder[0] = '\0';
    activeworldmetadata = worlddiffmetadata();
    worldeditrevision = incomingworldeditrevision = 0;
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

static cube &lookupworldchunkrootcube(cube *root, const ivec &pos, int size)
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

static void setworldcubetreematerial(cube &c, int material)
{
    if(c.children)
    {
        loopi(8) setworldcubetreematerial(c.children[i], material);
        return;
    }
    emptyfaces(c);
    c.material = material;
}

static cube *copyworldchunkforsave(const worldchunk &chunk)
{
    cube *root = newcubes(F_EMPTY);
    loopi(8) copyworldcube(chunk.root[i], root[i]);

    vector<ivec> flowing;
    getflowingwatercells(flowing);
    const int absolutex = chunk.x * WORLD_CHUNK_SIZE,
              absolutey = chunk.y * WORLD_CHUNK_SIZE;
    loopv(flowing)
    {
        const ivec &position = flowing[i];
        if(position.x < absolutex || position.x >= absolutex + WORLD_CHUNK_SIZE ||
           position.y < absolutey || position.y >= absolutey + WORLD_CHUNK_SIZE ||
           position.z < 0 || position.z >= WORLD_MAP_SIZE)
            continue;
        const ivec local(position.x - absolutex, position.y - absolutey, position.z);
        cube &cell = lookupworldchunkrootcube(root, local, WORLD_BLOCK_SIZE);
        setworldcubetreematerial(cell, MAT_AIR);
    }
    return root;
}

static void pasteworldcube(const cube &src, cube &dst)
{
    discardchildren(dst);
    copyworldcube(src, dst);
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

static bool worldchunkfullymounted(const worldchunk &chunk)
{
    const uint alltiles = (1U << WORLD_SECTION_TILES) - 1;
    loopi(WORLD_SECTION_LAYERS) if(chunk.mountedtiles[i] != alltiles) return false;
    return true;
}

void markworldchunksdirty(const ivec &bbmin, const ivec &bbmax)
{
    if(suppressworldchunkdirty || worldchunks.empty()) return;
    invalidateworldskyexposure(bbmin, bbmax);
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
            uint tilebit = 1U << (y * WORLD_SECTION_COLUMNS + x);
            chunk.contentknown[z] &= ~tilebit;
            chunk.opaqueknown[z] &= ~tilebit;
            chunk.portalsknown[z] &= ~tilebit;
        }
        chunk.dirty = true;
        visibilitychanged = true;
    }
    if(visibilitychanged) invalidateworldsectionvisibility();
}

static bool syncmountedworldchunk(worldchunk &chunk)
{
    if(!worldchunkmounted(chunk) || !chunk.root || !worldroot) return !chunk.corrupted;
    ZoneScopedN("Chunks/Sync mounted chunk");
    ZoneTextF("%d_%d", chunk.x, chunk.y);
    bool valid = !chunk.corrupted;
    loopi(WORLD_SECTION_LAYERS) if(chunk.mountedtiles[i]) loopj(WORLD_SECTION_TILES)
    {
        if(!(chunk.mountedtiles[i] & (1U << j))) continue;
        int x = j % WORLD_SECTION_COLUMNS, y = j / WORLD_SECTION_COLUMNS;
        ivec pos(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, i * WORLD_SECTION_SIZE);
        ivec runtimepos = ivec(worldchunkorigin(chunk)).add(pos);
        int key = worldchunkvaupdatekey(runtimepos);
        worldsectionowner *owner = worldsectionowners.access(key);
        if(!owner || !owner->matches(chunk, i, j))
        {
            conoutf(CON_ERROR, "refusing to sync chunk %d_%d section %d:%d: runtime ownership mismatch",
                    chunk.x, chunk.y, i, j);
            chunk.corrupted = true;
            invalidateworldsectionvisibility();
            valid = false;
            continue;
        }
        pasteworldcube(lookupcube(runtimepos, WORLD_SECTION_SIZE),
                       lookupworldchunkcube(chunk, pos, WORLD_SECTION_SIZE));
    }
    return valid;
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
    invalidateworldskyexposure(runtimepos, ivec(runtimepos).add(WORLD_SECTION_SIZE));
    worldsectionowners[key] = worldsectionowner(chunk.x, chunk.y, section, tile);
    chunk.mountedtiles[section] |= tilebit;
    return true;
}

static bool unmountworldchunktile(worldchunk &chunk, int section, int tile)
{
    const uint tilebit = 1U << tile;
    if(!(chunk.mountedtiles[section] & tilebit)) return false;
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
    invalidateworldskyexposure(runtimepos, ivec(runtimepos).add(WORLD_SECTION_SIZE));
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
        if(best < 0 || !unmountworldchunktile(chunk, best, tile)) break;
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
    long long score = worldchunkcoordinatescore(job.x, job.y);
    // Disk hits are normally much faster than generation. Prefer one only
    // within the same spatial band so nearby collision terrain still wins.
    if(job.filename[0]) score = max(score - 0x1000, 0LL);
    return int(min(score, (long long)INT_MAX));
}

static int worldchunkloader(void *)
{
#ifdef TRACY_ENABLE
    tracy::SetThreadName("World chunk loader");
#endif
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for(;;)
    {
        SDL_LockMutex(worldchunkmutex);
        while(worldchunkjobs.empty() && !stopworldchunkthread)
            SDL_CondWait(worldchunkcond, worldchunkmutex);
        if(stopworldchunkthread)
        {
            SDL_UnlockMutex(worldchunkmutex);
            return 0;
        }
        worldchunkjob *job = NULL;
        {
            ZoneScopedN("Chunks/Worker select job");
            ZoneValue(worldchunkjobs.length());
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
        SDL_UnlockMutex(worldchunkmutex);

        {
            ZoneScopedN("Chunks/Worker job");
            ZoneTextF("%d_%d", job->x, job->y);
            if(!SDL_AtomicGet(&job->cancelled)) job->root = prepareworldchunk(*job);
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

    if(worldchunkcond) SDL_DestroyCond(worldchunkcond);
    if(worldchunkmutex) SDL_DestroyMutex(worldchunkmutex);
    worldchunkcond = NULL;
    worldchunkmutex = NULL;
    stopworldchunkthread = false;
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

static int acquireworldchunksync(int x, int y, int &generated)
{
    int index = findworldchunk(x, y);
    if(index >= 0) return index;

    ZoneScopedN("Chunks/Load synchronous");
    ZoneTextF("%d_%d", x, y);
    defformatstring(diffname, "media/map/%s/chunks/%d_%d_%d.diff",
                    worldfolder, x, y, WORLD_DIFF_Z);
    path(diffname);
    const char *found = findfile(diffname, "rb");
    string diffpath;
    diffpath[0] = '\0';
    if(found && fileexists(found, "r")) copystring(diffpath, diffname);
    cube *root = game::generateworldchunk(x, y);
    vector<worldscatterinstance> scatter;
    game::generateworldscatter(root, x, y, scatter);
    bool loaded = diffpath[0] != '\0';
    if(root && loaded)
    {
        int families = 0;
        ullong revision = 0, canonicalhash = 0;
        applyworldchunkdiff(root, x, y, diffpath, scatter, false, families,
                            revision, canonicalhash);
        if(chunkremip) remipworldchunk(root, false, families);
        worldchunkdiffstate *state = findworldchunkdiffstate(x, y, true);
        state->revision = revision;
        worldeditrevision = max(worldeditrevision, revision);
        state->canonicalhash = hashworldchunk(root);
    }
    else generated++;
    worldchunk &chunk = worldchunks.add(worldchunk(x, y, root, false, loaded));
    indexworldchunk(worldchunks.length() - 1);
    chunk.scatter.move(scatter);
    addworldsectionvisibilitychunk(x, y);
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
        acquireworldchunksync(x, y, generated);
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
    worldchunkjob *job = new worldchunkjob(x, y, worldchunkepoch, request);
    defformatstring(chunkfile, "media/map/%s/chunks/%d_%d_%d.diff",
                    worldfolder, x, y, WORLD_DIFF_Z);
    path(chunkfile);
    const char *found = findfile(chunkfile, "rb");
    if(found && fileexists(found, "r"))
        copystring(job->filename, chunkfile);

    worldchunk &chunk = worldchunks.add(worldchunk(x, y, NULL, true));
    indexworldchunk(worldchunks.length() - 1);
    chunk.request = request;
    chunk.generating = !job->filename[0];
    SDL_LockMutex(worldchunkmutex);
    worldchunkjobs.add(job);
    TracyPlot("Chunks/Queued jobs", int64_t(worldchunkjobs.length()));
    SDL_CondSignal(worldchunkcond);
    SDL_UnlockMutex(worldchunkmutex);
    return worldchunks.length() - 1;
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

    int handled = 0, published = 0, loaded = 0, generated = 0, optimized = 0;
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
        handled++;

        {
            ZoneScopedN("Chunks/Publish worker result");
            ZoneTextF("%d_%d %s families %d", job->x, job->y,
                      job->loaded ? "disk" : "generated", job->families);
            ZoneValue(job->families);
            worldchunk &chunk = worldchunks[index];
            chunk.root = job->root;
            chunk.scatter.move(job->scatter);
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
            chunk.saved = job->loaded;
            chunk.dirty = false;
            worldchunkdiffstate *diffstate = findworldchunkdiffstate(chunk.x, chunk.y, true);
            diffstate->revision = max(diffstate->revision, job->revision);
            worldeditrevision = max(worldeditrevision, job->revision);
            diffstate->canonicalhash = job->canonicalhash;
            allocnodes += job->families;
            if(!job->loaded && job->filename[0])
                conoutf(CON_WARN, "asynchronous load of chunk %d_%d failed at stage %d; regenerated it",
                        job->x, job->y, job->loaderror);
            if(job->loaded) loaded++; else generated++;
            optimized += job->optimized;
            addworldsectionvisibilitychunk(chunk.x, chunk.y);
            published++;
        }
        delete job;
    }

    if(published)
        conoutf(CON_DEBUG, "prepared %d chunks asynchronously (%d loaded, %d generated, %d octree families remipped)",
                published, loaded, generated, optimized);
    return published;
}

static void processworldchunkupdates(int chunkx, int chunky, int aheadx, int aheady)
{
    if(stopworldchunkgeneration || lastworldchunkpublish == totalmillis) return;
    ZoneScopedN("Chunks/Streaming update");
    ZoneTextF("focus %d_%d ahead %d_%d", chunkx, chunky, aheadx, aheady);
    lastworldchunkpublish = totalmillis;
    reprioritizeworldchunkqueue(chunkx, chunky, aheadx, aheady);
    processworldchunkresults();
    queueworldchunkview(chunkx, chunky, aheadx, aheady);
    processworldchunkchanges(chunkx, chunky);
    pruneworldchunkcache(chunkx, chunky, INT_MAX);
    activeworldchunk = findworldchunk(chunkx, chunky);
}

static void rebaseworldchunks(int chunkx, int chunky, bool translateplayer = true)
{
    ZoneScopedN("Chunks/Rebase runtime world");
    ZoneTextF("%d_%d", chunkx, chunky);
    invalidateworldsectionvisibility();
    worldchunkvaupdates.setsize(0);
    worldchunkvaupdateset.clear();
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
        game::rebasenpcs(float(shiftx), float(shifty));
    }
    conoutf(CON_DEBUG, "rebased chunk window around %d_%d", chunkx, chunky);
}

static void mountworldchunksafetyregion(int chunkx, int chunky, bool updategeometry = true)
{
    if(!player) return;
    ZoneScopedN("Chunks/Mount safety region");
    ZoneTextF("%d_%d", chunkx, chunky);
    int playertilex = int(player->o.x) / WORLD_SECTION_SIZE,
        playertiley = int(player->o.y) / WORLD_SECTION_SIZE,
        playersection = clamp(int(player->o.z) / WORLD_SECTION_SIZE,
                              0, int(WORLD_SECTION_LAYERS) - 1),
        changedsections = 0;
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
            if(worldtilex != playertilex || worldtiley != playertiley) continue;
            int sections[3], numsections = 0;
            for(int section = max(playersection - 1, 0);
                section <= min(playersection + 1, int(WORLD_SECTION_LAYERS) - 1);
                ++section)
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

static int pruneworldchunkcache(int chunkx, int chunky, int limit)
{
    ZoneScopedN("Chunks/Prune cache");
    ZoneTextF("focus %d_%d limit %d", chunkx, chunky, limit);
    Uint64 start = SDL_GetPerformanceCounter();
    const Uint64 frequency = SDL_GetPerformanceFrequency();
    int released = 0, cachedist = maxchunkdist + chunkcachedist;
    for(int i = worldchunks.length() - 1; i >= 0; --i)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || worldchunkmounted(chunk) || chunk.dirty || !chunk.root ||
           worldchunkdistance(chunk.x, chunk.y, chunkx, chunky) <= cachedist)
            continue;
        {
            ZoneScopedN("Chunks/Release cache");
            ZoneTextF("%d_%d", chunk.x, chunk.y);
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

    int released = pruneworldchunkcache(chunkx, chunky, 1);
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
    flushworlddiffjournals(false);
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
    int destination = acquireworldchunksync(chunkx, chunky, generated);
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

static bool remipworldchunk(cube &c, const ivec &co, int size, cube *root,
                            bool prepared, int &families, int &merged)
{
    cube *children = c.children;
    if(!children) return true;

    bool perfect = true;
    loopi(8) if(!remipworldchunk(children[i], ivec(i, co, size), size >> 1, root,
                                 prepared, families, merged))
        perfect = false;

    solidfaces(c);
    loopi(6) c.texture[i] = getmippedtexture(c, i);
    if(!perfect || (size << 1) > 0x1000) return false;

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

int remipworldchunk(cube *root, bool prepared, int &families, SDL_atomic_t *cancelled)
{
    int merged = 0;
    loopi(8)
    {
        if(cancelled && SDL_AtomicGet(cancelled)) break;
        remipworldchunk(root[i], ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE),
                        WORLD_CHUNK_ROOT_SIZE >> 1, root, prepared, families, merged);
    }
    return merged;
}

static cube *prepareworldchunk(worldchunkjob &job)
{
    ZoneScopedN("Chunks/Prepare");
    ZoneTextF("%d_%d", job.x, job.y);
    if(SDL_AtomicGet(&job.cancelled)) return NULL;
    {
        ZoneScopedN("Chunks/Generate base and apply diff");
        cube *root = game::generateworldchunk(job.generation, job.x, job.y, job.families, job.optimized);
        if(!root) return NULL;
        game::generateworldscatter(job.generation, root, job.x, job.y, job.scatter);
        if(job.filename[0])
        {
            applyworldchunkdiff(root, job.x, job.y, job.filename, job.scatter,
                                true, job.families,
                                job.revision, job.canonicalhash);
            if(job.remip)
                job.optimized += remipworldchunk(root, true, job.families);
            job.canonicalhash = hashworldchunk(root);
            job.loaded = true;
        }
        else
        {
            job.loaded = false;
            job.canonicalhash = hashworldchunk(root);
        }
        return root;
    }
}


#endif
