// worldio.cpp: loading & saving of maps and savegames

#include "engine.h"
#include "worlddef.h"
#ifndef STANDALONE
#include "../game/worldgen.h"
#include "../game/weather.h"
#include "worldruntime.h"
#endif
#include <errno.h>

#ifdef WIN32
#define WORLD_ULL_FORMAT "%I64u"
#else
#define WORLD_ULL_FORMAT "%llu"
#endif

void validmapname(char *dst, const char *src, const char *prefix = NULL, const char *alt = "untitled", size_t maxlen = 100)
{
    if(prefix) while(*prefix) *dst++ = *prefix++;
    const char *start = dst;
    if(src) loopi(maxlen)
    {
        char c = *src++;
        if(iscubealnum(c) || c == '_' || c == '-' || c == '/' || c == '\\') *dst++ = c;
        else break;
    }
    if(dst > start) *dst = '\0';
    else if(dst != alt) copystring(dst, alt, maxlen);
}

void fixmapname(char *name)
{
    validmapname(name, name, NULL, "");
}

static bool loadmapheader(stream *f, const char *mapname, mapheader &hdr)
{
    if(f->read(&hdr, sizeof(hdr)) != sizeof(hdr))
    {
        conoutf(CON_ERROR, "map %s has a malformed lightweight header", mapname);
        return false;
    }
    lilswap(&hdr.version, 4);
    if(memcmp(hdr.magic, "TMAP", 4) || hdr.version != MAPVERSION)
    {
        conoutf(CON_ERROR, "map %s is not a version %d lightweight octree", mapname, MAPVERSION);
        return false;
    }
    if(hdr.worldsize < (1 << 9) || hdr.worldsize > (1 << 16) ||
       (hdr.worldsize & (hdr.worldsize - 1)))
    {
        conoutf(CON_ERROR, "map %s has an invalid world size", mapname);
        return false;
    }
    return true;
}

bool loadents(const char *fname, vector<entity> &ents, uint *crc)
{
    string name;
    validmapname(name, fname);
    defformatstring(ogzname, "media/map/%s.ogz", name);
    path(ogzname);
    stream *f = openrawfile(ogzname, "rb");
    if(!f) return false;

    mapheader hdr;
    bool loaded = loadmapheader(f, ogzname, hdr);
    if(crc) *crc = 0;

    delete f;
    return loaded;
}

#ifndef STANDALONE
string ogzname, bakname, cfgname, picname;

VARP(savebak, 0, 2, 2);

struct worldspawnmetadata
{
    bool valid;
    double x, y;
    float z, yaw, pitch;

    worldspawnmetadata() : valid(false), x(0), y(0), z(0), yaw(0), pitch(0) {}
};

VARP(maxchunkdist, 2, 3, WORLD_MAX_CHUNK_DIST);

#define WORLDIO_MODULE_IMPLEMENTATION
#include "../game/worldcontent.cpp"
#include "worldcache.cpp"
#include "worldstream.cpp"
#include "worldlod.cpp"
#include "../game/worldrender.cpp"
#include "worldvisibility.cpp"
#include "worldedit.cpp"
#undef WORLDIO_MODULE_IMPLEMENTATION


static bool chunkcoords(const char *name, int &x, int &y)
{
    if(!name || !*name) return false;
    char *end = NULL;
    long parsedx = strtol(name, &end, 10);
    if(end == name || *end != '_') return false;
    const char *second = end + 1;
    long parsedy = strtol(second, &end, 10);
    if(end == second || *end || parsedx < INT_MIN || parsedx > INT_MAX || parsedy < INT_MIN || parsedy > INT_MAX)
        return false;
    x = int(parsedx);
    y = int(parsedy);
    return true;
}

static bool chunkbasename(const char *name)
{
    int x, y;
    return chunkcoords(name, x, y);
}

static void normalizeworldfolder(char *folder, size_t len, const char *requested)
{
    string name;
    validmapname(name, requested && *requested ? requested : game::getclientmap(), NULL, "untitled");
    loopi(strlen(name)) if(name[i] == '\\') name[i] = '/';

    char *slash = strrchr(name, '/');
    if(slash && chunkbasename(slash + 1)) *slash = '\0';
    copystring(folder, name[0] ? name : "untitled", len);
}

static void chooseworldfolder(const char *requested)
{
    normalizeworldfolder(worldfolder, sizeof(worldfolder), requested);
}

static void worldchunkname(char *name, size_t len, const worldchunk &chunk)
{
    snprintf(name, len, "%s/%d_%d", worldfolder, chunk.x, chunk.y);
}

void setmapfilenames(const char *fname, const char *cname = NULL)
{
    string name;
    validmapname(name, fname);
    formatstring(ogzname, "media/map/%s.ogz", name);
    formatstring(picname, "media/map/%s.png", name);
    if(savebak==1) formatstring(bakname, "media/map/%s.BAK", name);
    else
    {
        string baktime;
        time_t t = time(NULL);
        size_t len = strftime(baktime, sizeof(baktime), "%Y-%m-%d_%H.%M.%S", localtime(&t));
        baktime[min(len, sizeof(baktime)-1)] = '\0';
        formatstring(bakname, "media/map/%s_%s.BAK", name, baktime);
    }

    validmapname(name, cname ? cname : fname);
    formatstring(cfgname, "media/map/%s.cfg", name);

    path(ogzname);
    path(bakname);
    path(cfgname);
    path(picname);
}

void mapcfgname()
{
    const char *mname = game::getclientmap();
    string name;
    validmapname(name, mname);
    defformatstring(cfgname, "media/map/%s.cfg", name);
    path(cfgname);
    result(cfgname);
}

COMMAND(mapcfgname, "");

void backup(const char *name, const char *backupname)
{
    string backupfile;
    copystring(backupfile, findfile(backupname, "wb"));
    remove(backupfile);
    rename(findfile(name, "wb"), backupfile);
}

// Leaf payloads contain only shape, face type IDs, and an optional material ID.
enum { OCTSAV_CHILDREN = 0, OCTSAV_EMPTY, OCTSAV_SOLID, OCTSAV_NORMAL };

static int savemapprogress = 0;

void savec(cube *c, const ivec &o, int size, stream *f)
{
    if((savemapprogress++&0xFFF)==0) renderprogress(float(savemapprogress)/allocnodes, "saving octree...");

    loopi(8)
    {
        ivec co(i, o, size);
        if(c[i].children)
        {
            f->putchar(OCTSAV_CHILDREN);
            savec(c[i].children, co, size>>1, f);
        }
        else
        {
            ushort material = c[i].material;
            if((material&MATF_VOLUME) == MAT_WATER)
            {
                bool falling = false;
                const int level = getwatercelllevel(co, falling);
                if(level > 0 || falling) material = MAT_AIR;
            }
            int octsav = isempty(c[i]) ? OCTSAV_EMPTY :
                         isentirelysolid(c[i]) ? OCTSAV_SOLID : OCTSAV_NORMAL;
            if(material != MAT_AIR) octsav |= 0x40;
            f->putchar(octsav);
            if((octsav & 0x7) == OCTSAV_NORMAL) f->write(c[i].edges, sizeof(c[i].edges));
            if((octsav & 0x7) != OCTSAV_EMPTY)
            {
                loopj(6) f->putlil<ushort>(c[i].texture[j]);
            }
            if(octsav & 0x40) f->putlil<ushort>(material);
        }
    }
}

cube *loadchildren(stream *f, const ivec &co, int size, bool &failed);

void loadc(stream *f, cube &c, const ivec &co, int size, bool &failed)
{
    int octsav = f->getchar();
    if(octsav < 0 || octsav & ~0x47) { failed = true; return; }
    switch(octsav&0x7)
    {
        case OCTSAV_CHILDREN:
            if(octsav != OCTSAV_CHILDREN) { failed = true; return; }
            c.children = loadchildren(f, co, size>>1, failed);
            return;

        case OCTSAV_EMPTY:  emptyfaces(c);        break;
        case OCTSAV_SOLID:  solidfaces(c);        break;
        case OCTSAV_NORMAL:
            if(f->read(c.edges, sizeof(c.edges)) != sizeof(c.edges)) { failed = true; return; }
            break;
        default: failed = true; return;
    }
    if((octsav & 0x7) != OCTSAV_EMPTY) loopi(6) c.texture[i] = f->getlil<ushort>();
    if(octsav&0x40) c.material = f->getlil<ushort>();
}

cube *loadchildren(stream *f, const ivec &co, int size, bool &failed)
{
    cube *c = newcubes();
    loopi(8)
    {
        loadc(f, c[i], ivec(i, co, size), size, failed);
        if(failed) break;
    }
    return c;
}

bool save_world(const char *mname)
{
    if(!*mname) mname = game::getclientmap();
    setmapfilenames(*mname ? mname : "untitled");
    if(savebak) backup(ogzname, bakname);
    stream *f = openrawfile(ogzname, "wb");
    if(!f) { conoutf(CON_WARN, "could not write map to %s", ogzname); return false; }

    savemapprogress = 0;
    renderprogress(0, "saving lightweight octree...");

    mapheader hdr;
    memcpy(hdr.magic, "TMAP", 4);
    hdr.version = MAPVERSION;
    hdr.worldsize = worldsize;
    hdr.chunkx = hdr.chunky = INT_MIN;
    const char *forwardslash = strrchr(mname, '/'), *backslash = strrchr(mname, '\\'), *basename = !forwardslash ? backslash : !backslash || forwardslash > backslash ? forwardslash : backslash;
    basename = basename ? basename + 1 : mname;
    chunkcoords(basename, hdr.chunkx, hdr.chunky);
    lilswap(&hdr.version, 4);
    f->write(&hdr, sizeof(hdr));

    savec(worldroot, ivec(0, 0, 0), worldsize>>1, f);

    delete f;
    conoutf("wrote lightweight octree %s", ogzname);
    return true;
}

static bool preparedworldspawn = false;
static vec preparedworldspawnposition;
static vec preparedworldspawnabsolute;
static float preparedworldspawnyaw = 0, preparedworldspawnpitch = 0;

static bool mountworldspawncolumn(worldchunk &chunk, double absolutex, double absolutey)
{
    if(!chunk.root || chunk.loading || chunk.corrupted) return false;
    int localx = int(floor(absolutex - double(chunk.x) * WORLD_CHUNK_SIZE)),
        localy = int(floor(absolutey - double(chunk.y) * WORLD_CHUNK_SIZE));
    if(localx < 0 || localx >= WORLD_CHUNK_SIZE ||
       localy < 0 || localy >= WORLD_CHUNK_SIZE)
        return false;

    int tilex = localx / WORLD_SECTION_SIZE,
        tiley = localy / WORLD_SECTION_SIZE,
        tile = tiley * WORLD_SECTION_COLUMNS + tilex;
    loopi(WORLD_SECTION_LAYERS) mountworldchunktile(chunk, i, tile);
    return !chunk.corrupted;
}

static bool prepareworldspawn(const worldspawnmetadata &saved)
{
    if(!player || worldchunks.empty() || !worldroot) return false;

    renderprogress(0.78f, "waiting for ground...");

    double absolutex, absolutey;
    if(saved.valid)
    {
        absolutex = saved.x;
        absolutey = saved.y;
    }
    else
    {
        const worldchunk &entry = worldchunks.inrange(activeworldchunk)
                                ? worldchunks[activeworldchunk] : worldchunks[0];
        absolutex = double(entry.x) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2;
        absolutey = double(entry.y) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2;
        if(!game::chooseworldspawn(absolutex, absolutey, absolutex, absolutey))
        {
            conoutf(CON_ERROR, "could not find dry ground for the player spawn");
            return false;
        }
    }

    int chunkx = int(floor(absolutex / WORLD_CHUNK_SIZE)),
        chunky = int(floor(absolutey / WORLD_CHUNK_SIZE)),
        generated = 0,
        destination = acquireworldchunksync(chunkx, chunky, generated);
    if(!worldchunks.inrange(destination) || !worldchunks[destination].root)
    {
        conoutf(CON_ERROR, "could not prepare spawn chunk %d_%d", chunkx, chunky);
        return false;
    }

    rebaseworldchunks(chunkx, chunky, false);
    const float runtimex = float(absolutex - double(worldfirstchunkx) * WORLD_CHUNK_SIZE),
                runtimey = float(absolutey - double(worldfirstchunky) * WORLD_CHUNK_SIZE);
    player->o = vec(runtimex, runtimey, WORLD_MAP_SIZE - 1.0f);
    if(!mountworldspawncolumn(worldchunks[destination], absolutex, absolutey))
    {
        conoutf(CON_ERROR, "could not mount the geometry beneath the spawn point");
        return false;
    }

    vec sky(runtimex, runtimey, WORLD_MAP_SIZE - 1.0f);
    float grounddist = raycube(sky, vec(0, 0, -1), WORLD_MAP_SIZE, RAY_CLIPMAT);
    if(grounddist >= WORLD_MAP_SIZE)
    {
        conoutf(CON_ERROR, "could not find solid ground beneath the spawn point");
        return false;
    }
    const float groundz = sky.z - grounddist;

    if(saved.valid)
    {
        player->o = vec(runtimex, runtimey, clamp(saved.z, 0.0f, float(WORLD_MAP_SIZE - 1)));
        player->yaw = saved.yaw;
        player->pitch = saved.pitch;
        player->reset();
        player->resetinterp();
    }
    else
    {
        player->o = vec(runtimex, runtimey, groundz + player->eyeheight + 0.1f);
        player->yaw = player->pitch = 0;
        player->reset();
        player->resetinterp();
    }

    const int material = lookupmaterial(vec(player->o.x, player->o.y, max(player->o.z - player->eyeheight + 1, 0.0f)));
    if(!saved.valid && isliquid(material & MATF_VOLUME))
    {
        conoutf(CON_ERROR, "refusing to finish loading with the player spawn in water");
        return false;
    }

    lastworldchunkmotion = -1;
    worldchunkaheadx = chunkx;
    worldchunkaheady = chunky;
    worlddebugcachemillis = -1;
    rebuildworldchunks(chunkx, chunky, chunkx, chunky, true, false);

    preparedworldspawnposition = player->o;
    preparedworldspawnabsolute = player->o;
    worldpositiontoabsolute(preparedworldspawnabsolute);
    preparedworldspawnyaw = player->yaw;
    preparedworldspawnpitch = player->pitch;
    preparedworldspawn = true;
    renderprogress(0.9f, "ground found - putting your boots on...");
    return true;
}

bool getpreparedworldspawn(vec &position, float &yaw, float &pitch)
{
    if(!preparedworldspawn) return false;
    position = preparedworldspawnabsolute;
    yaw = preparedworldspawnyaw;
    pitch = preparedworldspawnpitch;
    return true;
}

static void applypreparedworldspawn()
{
    if(!preparedworldspawn || !player) return;
    player->o = preparedworldspawnposition;
    player->yaw = preparedworldspawnyaw;
    player->pitch = preparedworldspawnpitch;
    player->reset();
    player->resetinterp();
}

static void createworld(const char *requestedname)
{
    chooseworldfolder(requestedname);
    string chosenfolder, activechunkname;
    copystring(chosenfolder, worldfolder);
    formatstring(activechunkname, "%s/0_0", chosenfolder);

    UI::hideui("new_world");

    // Snapshot the menu/console seed before loading or resetting anything.
    // The active seed belongs to the currently loaded world and must not be
    // reused implicitly when creating a differently named world.
    const int chosenworldseed = game::getconfiguredworldseed();
    game::resetsurvivalinventory();
    game::resetfurnaces();
    game::resetchests();
    game::beginlocalworld();
    if(!emptymap(WORLD_RUNTIME_SCALE, true, activechunkname)) return;
    copystring(worldfolder, chosenfolder);
    worldfirstchunkx = worldfirstchunky = -WORLD_RUNTIME_CENTER;
    if(!loadworlddefinitions()) return;
    game::loadworldseed(chosenworldseed);
    game::weather::preparemap(worldfolder, chosenworldseed);

    freeocta(worldroot);
    worldroot = NULL;
    int generated = 0;
    activeworldchunk = acquireworldchunksync(0, 0, generated);
    loadinitialworldchunks(0, 0);

    setvar("mapscale", WORLD_RUNTIME_SCALE, true, false);
    setvar("mapsize", WORLD_RUNTIME_SIZE, true, false);
    worldroot = newcubes(F_EMPTY);
    if(player)
    {
        player->o = vec((0 - worldfirstchunkx) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        (0 - worldfirstchunky) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        WORLD_GROUND_HEIGHT + player->eyeheight + 1);
    }
    preparedworldspawn = false;
    worldspawnmetadata newspawn;
    if(!prepareworldspawn(newspawn)) return;
    updateworldchunks(true);
    applypreparedworldspawn();

    int mounted = 0;
    loopv(worldchunks) if(worldchunkmounted(worldchunks[i])) mounted++;
    conoutf("generated infinite world %s with seed %d and %d initial chunks; %d chunks queued asynchronously",
            worldfolder, game::getworldseed(), mounted, worldchunks.length() - mounted);
    conoutf("new chunks are prepared on demand");
}

ICOMMAND(newworld, "ssN", (char *arg1, char *arg2, int *numargs),
{
    const char *name = NULL;
    if(*numargs > 0)
    {
        char *end = NULL;
        strtol(arg1, &end, 10);
        bool legacysize = end != arg1 && !*end;
        if(legacysize)
        {
            conoutf(CON_WARN, "newworld size is no longer used; the world expands on demand");
            if(*numargs > 1) name = arg2;
        }
        else name = arg1;
    }
    createworld(name);
});

void startnetworkworld(int seed)
{
    game::resetfurnaces();
    game::resetchests();
    game::loadworldseed(seed);
    if(!emptymap(WORLD_RUNTIME_SCALE, true, "network/0_0", true, false)) return;
    worldfolder[0] = '\0';
    worldfirstchunkx = worldfirstchunky = -WORLD_RUNTIME_CENTER;
    if(!loadworlddefinitions()) return;
    game::weather::update(game::weather::getseed(seed));

    freeocta(worldroot);
    worldroot = NULL;
    activeworldchunk = worldchunks.length();
    {
        worldsectionrenderdata renderdata;
        worldchunk &chunk = worldchunks.add(worldchunk(0, 0, game::generateworldchunk(0, 0, &renderdata)));
        indexworldchunk(worldchunks.length() - 1);
        chunk.renderdata = renderdata;
        game::generateworldscatter(chunk.root, 0, 0, chunk.scatter);
    }
    loadinitialworldchunks(0, 0);

    setvar("mapscale", WORLD_RUNTIME_SCALE, true, false);
    setvar("mapsize", WORLD_RUNTIME_SIZE, true, false);
    worldroot = newcubes(F_EMPTY);
    if(player)
    {
        player->o = vec(WORLD_RUNTIME_CENTER * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        WORLD_RUNTIME_CENTER * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        WORLD_GROUND_HEIGHT + player->eyeheight + 1);
    }
    preparedworldspawn = false;
    worldspawnmetadata spawn;
    if(!prepareworldspawn(spawn)) return;
    updateworldchunks(true);
    applypreparedworldspawn();
    conoutf("joined authoritative world with seed %d", seed);
}

void closeproceduralworld()
{
    game::resetfurnaces();
    game::resetchests();
    clearworldchunks();
    resetmap();
    freeocta(worldroot);
    worldroot = newcubes(F_SOLID);
}

static uint mapcrc = 0;

uint getmapcrc() { return mapcrc; }
void clearmapcrc() { mapcrc = 0; }

bool load_world(const char *mname, const char *cname)
{
    ZoneScopedN("Chunks/Load entry map");
    ZoneText(mname, strlen(mname));
    int loadingstart = SDL_GetTicks();
    setmapfilenames(mname, cname);
    stream *f;
    {
        ZoneScopedN("Chunks/Open entry map");
        f = openrawfile(ogzname, "rb");
    }
    if(!f)
    {
        conoutf(CON_ERROR, "could not read map %s", ogzname);
        return false;
    }

    mapheader hdr;
    {
        ZoneScopedN("Chunks/Read entry header");
        if(!loadmapheader(f, ogzname, hdr)) { delete f; return false; }
        const char *forwardslash = strrchr(mname, '/'), *backslash = strrchr(mname, '\\'),
                   *basename = !forwardslash ? backslash :
                               !backslash || forwardslash > backslash ? forwardslash : backslash;
        basename = basename ? basename + 1 : mname;
        int expectedx, expectedy;
        if(chunkcoords(basename, expectedx, expectedy) &&
           (hdr.chunkx != expectedx || hdr.chunky != expectedy))
        {
            conoutf(CON_ERROR, "map %s identifies itself as chunk %d_%d, expected %d_%d",
                    ogzname, hdr.chunkx, hdr.chunky, expectedx, expectedy);
            delete f;
            return false;
        }
    }

    {
        ZoneScopedN("Chunks/Reset previous world");
        clearworldchunks();
        resetmap();
    }

    Texture *mapshot = textureload(picname, 3, true, false);
    renderbackground("loading...", mapshot, mname, game::getmapinfo());

    setvar("mapversion", hdr.version, true, false);

    renderprogress(0, "clearing world...");

    freeocta(worldroot);
    worldroot = NULL;

    int worldscale = 0;
    while(1<<worldscale < hdr.worldsize) worldscale++;
    setvar("mapsize", 1<<worldscale, true, false);
    setvar("mapscale", worldscale, true, false);

    texmru.shrink(0);

    renderprogress(0, "loading lightweight octree...");
    bool failed = false;
    {
        ZoneScopedN("Chunks/Decode entry octree");
        worldroot = loadchildren(f, ivec(0, 0, 0), hdr.worldsize>>1, failed);
    }
    {
        ZoneScopedN("Chunks/Close entry map");
        delete f;
    }
    mapcrc = 0;
    if(failed)
    {
        conoutf(CON_ERROR, "map %s contains a malformed octree", ogzname);
        freeocta(worldroot);
        worldroot = newcubes(F_EMPTY);
        return false;
    }

    conoutf("read map %s (%.1f seconds)", ogzname, (SDL_GetTicks()-loadingstart)/1000.0f);

    clearmainmenu();

    {
        ZoneScopedN("Chunks/Load world configuration");
        identflags |= IDF_OVERRIDDEN;
        execfile("config/default_map_settings.cfg", false);
        execfile(cfgname, false);
        identflags &= ~IDF_OVERRIDDEN;
    }

    preparedworldspawn = false;

    {
        ZoneScopedN("Chunks/Build entry geometry");
        allchanged(true);
    }

    renderbackground("loading...", mapshot, mname, game::getmapinfo());

    if(maptitle[0] && strcmp(maptitle, "Untitled Map by Unknown")) conoutf(CON_ECHO, "%s", maptitle);

    startmap(cname ? cname : mname);
    restoreworldwatersources();

    return true;
}

void savecurrentmap() { save_world(game::getclientmap()); }
void savemap(char *mname) { save_world(mname); }

COMMAND(savemap, "s");
COMMAND(savecurrentmap, "");

void writeobj(char *name)
{
    defformatstring(fname, "%s.obj", name);
    stream *f = openfile(path(fname), "w");
    if(!f) return;
    f->printf("# obj file of Cube 2 level\n\n");
    defformatstring(mtlname, "%s.mtl", name);
    path(mtlname);
    f->printf("mtllib %s\n\n", mtlname);
    vector<vec> verts, texcoords;
    hashtable<vec, int> shareverts(1<<16), sharetc(1<<16);
    hashtable<int, vector<ivec2> > mtls(1<<8);
    vector<int> usedmtl;
    vec bbmin(1e16f, 1e16f, 1e16f), bbmax(-1e16f, -1e16f, -1e16f);
    loopv(valist)
    {
        vtxarray &va = *valist[i];
        if(!va.edata || !va.vdata) continue;
        ushort *edata = va.edata + va.eoffset;
        vertex *vdata = va.vdata;
        ushort *idx = edata;
        loopj(va.texs)
        {
            elementset &es = va.texelems[j];
            if(usedmtl.find(es.texture) < 0) usedmtl.add(es.texture);
            vector<ivec2> &keys = mtls[es.texture];
            loopk(es.length)
            {
                const vertex &v = vdata[idx[k]];
                const vec &pos = v.pos;
                const vec &tc = v.tc;
                ivec2 &key = keys.add();
                key.x = shareverts.access(pos, verts.length());
                if(key.x == verts.length())
                {
                    verts.add(pos);
                    bbmin.min(pos);
                    bbmax.max(pos);
                }
                key.y = sharetc.access(tc, texcoords.length());
                if(key.y == texcoords.length()) texcoords.add(tc);
            }
            idx += es.length;
        }
    }

    vec center(-(bbmax.x + bbmin.x)/2, -(bbmax.y + bbmin.y)/2, -bbmin.z);
    loopv(verts)
    {
        vec v = verts[i];
        v.add(center);
        if(v.y != floor(v.y)) f->printf("v %.3f ", -v.y); else f->printf("v %d ", int(-v.y));
        if(v.z != floor(v.z)) f->printf("%.3f ", v.z); else f->printf("%d ", int(v.z));
        if(v.x != floor(v.x)) f->printf("%.3f\n", v.x); else f->printf("%d\n", int(v.x));
    }
    f->printf("\n");
    loopv(texcoords)
    {
        const vec &tc = texcoords[i];
        f->printf("vt %.6f %.6f\n", tc.x, 1-tc.y);
    }
    f->printf("\n");

    usedmtl.sort();
    loopv(usedmtl)
    {
        vector<ivec2> &keys = mtls[usedmtl[i]];
        f->printf("g slot%d\n", usedmtl[i]);
        f->printf("usemtl slot%d\n\n", usedmtl[i]);
        for(int i = 0; i < keys.length(); i += 3)
        {
            f->printf("f");
            loopk(3) f->printf(" %d/%d", keys[i+2-k].x+1, keys[i+2-k].y+1);
            f->printf("\n");
        }
        f->printf("\n");
    }
    delete f;

    f = openfile(mtlname, "w");
    if(!f) return;
    f->printf("# mtl file of Cube 2 level\n\n");
    loopv(usedmtl)
    {
        VSlot &vslot = lookupvslot(usedmtl[i], false);
        f->printf("newmtl slot%d\n", usedmtl[i]);
        f->printf("map_Kd %s\n", vslot.slot->sts.empty() ? notexture->name : path(makerelpath("media", vslot.slot->sts[0].name)));
        f->printf("\n");
    }
    delete f;

    conoutf("generated model %s", fname);
}

COMMAND(writeobj, "s");

void writecollideobj(char *name)
{
    extern bool havesel;
    extern selinfo sel;
    if(!havesel)
    {
        conoutf(CON_ERROR, "geometry for collide model not selected");
        return;
    }
    vector<extentity *> &ents = entities::getents();
    extentity *mm = NULL;
    loopv(entgroup)
    {
        extentity &e = *ents[entgroup[i]];
        if(e.type != ET_MAPMODEL || !pointinsel(sel, e.o)) continue;
        mm = &e;
        break;
    }
    if(!mm) loopv(ents)
    {
        extentity &e = *ents[i];
        if(e.type != ET_MAPMODEL || !pointinsel(sel, e.o)) continue;
        mm = &e;
        break;
    }
    if(!mm)
    {
        conoutf(CON_ERROR, "could not find map model in selection");
        return;
    }
    model *m = loadmapmodel(mm->attr1);
    if(!m)
    {
        mapmodelinfo *mmi = getmminfo(mm->attr1);
        if(mmi) conoutf(CON_ERROR, "could not load map model: %s", mmi->name);
        else conoutf(CON_ERROR, "could not find map model: %d", mm->attr1);
        return;
    }

    matrix4x3 xform;
    m->calctransform(xform);
    float scale = mm->attr5 > 0 ? mm->attr5/100.0f : 1;
    int yaw = mm->attr2, pitch = mm->attr3, roll = mm->attr4;
    matrix3 orient;
    orient.identity();
    if(scale != 1) orient.scale(scale);
    if(yaw) orient.rotate_around_z(sincosmod360(yaw));
    if(pitch) orient.rotate_around_x(sincosmod360(pitch));
    if(roll) orient.rotate_around_y(sincosmod360(-roll));
    xform.mul(orient, mm->o, matrix4x3(xform));
    xform.invert();

    ivec selmin = sel.o, selmax = ivec(sel.s).mul(sel.grid).add(sel.o);
    vector<vec> verts;
    hashtable<vec, int> shareverts;
    vector<int> tris;
    loopv(valist)
    {
        vtxarray &va = *valist[i];
        if(va.geommin.x > selmax.x || va.geommin.y > selmax.y || va.geommin.z > selmax.z ||
           va.geommax.x < selmin.x || va.geommax.y < selmin.y || va.geommax.z < selmin.z)
            continue;
        if(!va.edata || !va.vdata) continue;
        ushort *edata = va.edata + va.eoffset;
        vertex *vdata = va.vdata;
        ushort *idx = edata;
        loopj(va.texs)
        {
            elementset &es = va.texelems[j];
            for(int k = 0; k < es.length; k += 3)
            {
                const vec &v0 = vdata[idx[k]].pos, &v1 = vdata[idx[k+1]].pos, &v2 = vdata[idx[k+2]].pos;
                if(!v0.insidebb(selmin, selmax) || !v1.insidebb(selmin, selmax) || !v2.insidebb(selmin, selmax))
                    continue;
                int i0 = shareverts.access(v0, verts.length());
                if(i0 == verts.length()) verts.add(v0);
                tris.add(i0);
                int i1 = shareverts.access(v1, verts.length());
                if(i1 == verts.length()) verts.add(v1);
                tris.add(i1);
                int i2 = shareverts.access(v2, verts.length());
                if(i2 == verts.length()) verts.add(v2);
                tris.add(i2);
            }
            idx += es.length;
        }
    }

    defformatstring(fname, "%s.obj", name);
    stream *f = openfile(path(fname), "w");
    if(!f) return;
    f->printf("# obj file of Cube 2 collide model\n\n");
    loopv(verts)
    {
        vec v = xform.transform(verts[i]);
        if(v.y != floor(v.y)) f->printf("v %.3f ", -v.y); else f->printf("v %d ", int(-v.y));
        if(v.z != floor(v.z)) f->printf("%.3f ", v.z); else f->printf("%d ", int(v.z));
        if(v.x != floor(v.x)) f->printf("%.3f\n", v.x); else f->printf("%d\n", int(v.x));
    }
    f->printf("\n");
    for(int i = 0; i < tris.length(); i += 3)
       f->printf("f %d %d %d\n", tris[i+2]+1, tris[i+1]+1, tris[i]+1);
    f->printf("\n");

    delete f;

    conoutf("generated collide model %s", fname);
}

COMMAND(writecollideobj, "s");

#else
#define WORLDIO_STANDALONE_CONTENT_IMPLEMENTATION
#include "../game/worldcontent.cpp"
#undef WORLDIO_STANDALONE_CONTENT_IMPLEMENTATION

#endif
