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

struct worlddiffmetadata
{
    int seed, worldgenversion, saveformatversion, gamemode, inventorycursoritem, inventorycursorcount, inventorycursordurability;
    float playerhealth, playerfalldistance;
    int playerphysstate;
    vec playervelocity, playerfalling;
    int inventoryitems[game::SURVIVAL_USABLE_SLOTS],
        inventorycounts[game::SURVIVAL_USABLE_SLOTS], inventorydurabilities[game::SURVIVAL_USABLE_SLOTS];
    ullong parameterhash;
    bool valid;

    worlddiffmetadata()
        : seed(0), worldgenversion(0), saveformatversion(0), gamemode(0),
          inventorycursoritem(-1), inventorycursorcount(0), inventorycursordurability(0), playerhealth(game::PLAYER_MAX_HEALTH),
          playerfalldistance(0), playerphysstate(PHYS_FALL), playervelocity(0, 0, 0), playerfalling(0, 0, 0),
          parameterhash(0), valid(false)
    {
        loopi(game::SURVIVAL_USABLE_SLOTS)
        {
            inventoryitems[i] = -1;
            inventorycounts[i] = 0;
            inventorydurabilities[i] = 0;
        }
    }
};

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
#include "worlddiff.cpp"
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

static bool loadworldchunks(const char *mname)
{
    ZoneScopedN("Chunks/Initialize streamed world");
    ZoneText(mname, strlen(mname));
    string mapname;
    validmapname(mapname, mname, NULL, "");
    loopi(strlen(mapname)) if(mapname[i] == '\\') mapname[i] = '/';
    char *slash = strrchr(mapname, '/');
    int currentx, currenty;
    if(!slash || !chunkcoords(slash + 1, currentx, currenty)) return false;
    *slash = '\0';
    worldspawnmetadata storedspawn;
    worlddiffmetadata metadata;
    int entryx, entryy;
    if(!loadworldmetadata(mapname, entryx, entryy, storedspawn, metadata)) return false;
    if(game::getworldseed() != metadata.seed ||
       currentworldparameterhash() != metadata.parameterhash)
    {
        conoutf(CON_ERROR,
                "world %s generator parameter hash does not match world.meta; refusing silent terrain changes",
                mapname);
        return false;
    }
    game::loadworldseed(metadata.seed);
    activeworldmetadata = metadata;

    cube *currentroot = worldroot;
    worldroot = NULL;
    copystring(worldfolder, mapname);
    if(!reconstructedworldscatterready)
    {
        string cachefilename;
        worldchunkcachefilename(cachefilename, sizeof(cachefilename), worldfolder, currentx, currenty);
        int cachefamilies = 0, cacheerror = 0;
        cube *base = generatedchunkcache
                         ? loadworldchunkcache(cachefilename, currentx, currenty, game::getworldseed(), game::worldgenerationparameterhash(),
                                               chunkremip != 0, reconstructedworldscatter, false, cachefamilies, cacheerror)
                         : NULL;
        if(!base)
        {
            ZoneScopedN("Chunks/Generate uncached");
            base = game::generateworldchunk(currentx, currenty);
            if(base) game::generateworldscatter(base, currentx, currenty, reconstructedworldscatter);
            vector<uchar> cachepayload;
            if(generatedchunkcache && base && serializeworldchunkcache(base, reconstructedworldscatter, cachepayload))
                queueworldchunkcachewrite(currentx, currenty, game::getworldseed(), game::worldgenerationparameterhash(), chunkremip != 0,
                                          cachepayload);
        }
        if(base)
        {
            setworldleavesalpha(base, leavesalpha != 0);
            defformatstring(diffname, "media/map/%s/chunks/%d_%d_%d.diff",
                            worldfolder, currentx, currenty, WORLD_DIFF_Z);
            path(diffname);
            const char *found = findfile(diffname, "rb");
            if(found && fileexists(found, "r"))
            {
                int families = 0;
                ullong revision = 0, canonicalhash = 0;
                applyworldchunkdiff(base, currentx, currenty, diffname,
                                    reconstructedworldscatter, false, families,
                                    revision, canonicalhash);
            }
            freeocta(base);
        }
    }
    reconstructedworldscatterready = false;
    activeworldchunk = 0;
    worldchunk &currentchunk =
        worldchunks.add(worldchunk(currentx, currenty, currentroot, false, true));
    indexworldchunk(worldchunks.length() - 1);
    currentchunk.scatter.move(reconstructedworldscatter);
    loadinitialworldchunks(currentx, currenty);

    worldfirstchunkx = currentx - WORLD_RUNTIME_CENTER;
    worldfirstchunky = currenty - WORLD_RUNTIME_CENTER;
    setvar("mapscale", WORLD_RUNTIME_SCALE, true, false);
    setvar("mapsize", WORLD_RUNTIME_SIZE, true, false);
    worldroot = newcubes(F_EMPTY);
    if(player)
    {
        player->o = vec((currentx - worldfirstchunkx) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        (currenty - worldfirstchunky) * WORLD_CHUNK_SIZE + WORLD_CHUNK_SIZE / 2,
                        WORLD_GROUND_HEIGHT + player->eyeheight + 1);
    }
    rebuildworldchunks(currentx, currenty, currentx, currenty, true, false);
    conoutf("loaded infinite world %s around chunk %d_%d", worldfolder, currentx, currenty);
    return true;
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
    const char *forwardslash = strrchr(mname, '/'), *backslash = strrchr(mname, '\\'),
               *basename = !forwardslash ? backslash :
                           !backslash || forwardslash > backslash ? forwardslash : backslash;
    basename = basename ? basename + 1 : mname;
    chunkcoords(basename, hdr.chunkx, hdr.chunky);
    lilswap(&hdr.version, 4);
    f->write(&hdr, sizeof(hdr));

    savec(worldroot, ivec(0, 0, 0), worldsize>>1, f);

    delete f;
    conoutf("wrote lightweight octree %s", ogzname);
    return true;
}

static bool saveworldconfig()
{
    defformatstring(name, "media/map/%s/world.cfg", worldfolder);
    stream *f = openfile(path(name), "w");
    if(!f)
    {
        conoutf(CON_WARN, "could not write world configuration to %s", name);
        return false;
    }

    f->printf(
        "// Generated by newworld. Logical height 0 is local Z=%d.\n"
        "worldchunksize = %d\n"
        "worldgridpower = %d\n"
        "worldblocksize = %d\n"
        "worldminheight = %d\n"
        "worldmaxheight = %d\n"
        "worldinfinite = 1\n\n"
        "worldload\n\n",
        WORLD_GROUND_HEIGHT, WORLD_CHUNK_BLOCKS, WORLD_GRID_POWER, WORLD_BLOCK_SIZE,
        WORLD_MIN_HEIGHT, WORLD_MAX_HEIGHT
    );
    game::saveworldsettings(f);
    delete f;

    return true;
}

static worldspawnmetadata requestedworldspawn;
static bool hasrequestedworldspawn = false;
static bool preparedworldspawn = false;
static vec preparedworldspawnposition;
static vec preparedworldspawnabsolute;
static float preparedworldspawnyaw = 0, preparedworldspawnpitch = 0;

static ullong currentworldparameterhash()
{
    return game::worldgenerationparameterhash();
}

static bool replaceworldmetadatafile(const char *temporary, const char *finalname)
{
#ifdef WIN32
    return MoveFileEx(temporary, finalname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temporary, finalname) == 0;
#endif
}

static bool saveworldmetadata(int chunkx, int chunky)
{
    if(activeworldmetadata.valid &&
       activeworldmetadata.worldgenversion != WORLDGEN_VERSION)
    {
        conoutf(CON_ERROR,
                "refusing to change worldgen version %d to %d for existing world %s",
                activeworldmetadata.worldgenversion, WORLDGEN_VERSION, worldfolder);
        return false;
    }
    defformatstring(name, "media/map/%s/world.meta", worldfolder);
    defformatstring(tempname, "%s.tmp", name);
    string finalpath, temppath;
    copystring(finalpath, findfile(name, "wb"));
    copystring(temppath, findfile(tempname, "wb"));
    stream *f = openrawfile(tempname, "wb");
    if(!f)
    {
        conoutf(CON_WARN, "could not write temporary world metadata to %s", tempname);
        return false;
    }
    activeworldmetadata.seed = game::getworldseed();
    activeworldmetadata.worldgenversion = WORLDGEN_VERSION;
    activeworldmetadata.parameterhash = currentworldparameterhash();
    activeworldmetadata.saveformatversion = WORLD_SAVE_FORMAT_VERSION;
    activeworldmetadata.gamemode = game::gamemode;
    activeworldmetadata.playerhealth = clamp(game::getlocalplayerhealth(), 0.0f, float(game::PLAYER_MAX_HEALTH));
    game::getlocalplayermotion(activeworldmetadata.playervelocity, activeworldmetadata.playerfalling,
                               activeworldmetadata.playerfalldistance, activeworldmetadata.playerphysstate);
    bool ok = f->printf("CUBECRAFT_WORLD 5\n") > 0;
    if(ok) ok = f->printf("world_seed %d\n", activeworldmetadata.seed) > 0;
    if(ok) ok = f->printf("worldgen_version %d\n", activeworldmetadata.worldgenversion) > 0;
    if(ok) ok = f->printf("worldgen_parameter_hash " WORLD_ULL_FORMAT "\n", activeworldmetadata.parameterhash) > 0;
    if(ok) ok = f->printf("save_format_version %d\n", activeworldmetadata.saveformatversion) > 0;
    if(ok) ok = f->printf("player_health %.9g\n", activeworldmetadata.playerhealth) > 0;
    if(ok) ok = f->printf("player_velocity %.9g %.9g %.9g\n", activeworldmetadata.playervelocity.x, activeworldmetadata.playervelocity.y,
                          activeworldmetadata.playervelocity.z) > 0;
    if(ok) ok = f->printf("player_falling %.9g %.9g %.9g\n", activeworldmetadata.playerfalling.x, activeworldmetadata.playerfalling.y,
                          activeworldmetadata.playerfalling.z) > 0;
    if(ok) ok = f->printf("player_fall_distance %.9g\n", activeworldmetadata.playerfalldistance) > 0;
    if(ok) ok = f->printf("player_physics_state %d\n", activeworldmetadata.playerphysstate) > 0;
    if(ok) ok = game::savesurvivalinventory(f);
    if(ok) ok = f->printf("entry %d %d\n", chunkx, chunky) > 0;
    if(ok && player)
    {
        const double absolutex = double(worldfirstchunkx) * WORLD_CHUNK_SIZE + player->o.x,
                     absolutey = double(worldfirstchunky) * WORLD_CHUNK_SIZE + player->o.y;
        ok = f->printf("spawn %.17g %.17g %.9g %.9g %.9g\n", absolutex, absolutey, player->o.z, player->yaw, player->pitch) > 0;
    }
    if(ok) ok = f->flush();
    delete f;
    if(!ok || !replaceworldmetadatafile(temppath, finalpath))
    {
        remove(temppath);
        conoutf(CON_WARN, "could not atomically publish world metadata to %s", name);
        return false;
    }
    activeworldmetadata.valid = true;
    return true;
}

static bool loadworldmetadata(const char *folder, int &chunkx, int &chunky,
                              worldspawnmetadata &spawn, worlddiffmetadata &metadata)
{
    chunkx = chunky = 0;
    spawn = worldspawnmetadata();
    metadata = worlddiffmetadata();
    defformatstring(name, "media/map/%s/world.meta", folder);
    stream *f = openfile(path(name), "r");
    if(!f) return false;
    int metarevision = 0;
    string line;
    while(f->getline(line, sizeof(line)))
    {
        int x, y;
        if(sscanf(line, "CUBECRAFT_WORLD %d", &metarevision) == 1) continue;
        if(sscanf(line, "world_seed %d", &metadata.seed) == 1) continue;
        if(sscanf(line, "worldgen_version %d", &metadata.worldgenversion) == 1) continue;
        if(sscanf(line, "save_format_version %d", &metadata.saveformatversion) == 1)
        {
            if(metadata.saveformatversion != WORLD_SAVE_FORMAT_VERSION) break;
            continue;
        }
        if(sscanf(line, "game_mode %d", &metadata.gamemode) == 1) continue;
        if(sscanf(line, "player_health %f", &metadata.playerhealth) == 1) continue;
        if(sscanf(line, "player_velocity %f %f %f", &metadata.playervelocity.x, &metadata.playervelocity.y,
                  &metadata.playervelocity.z) == 3) continue;
        if(sscanf(line, "player_falling %f %f %f", &metadata.playerfalling.x, &metadata.playerfalling.y,
                  &metadata.playerfalling.z) == 3) continue;
        if(sscanf(line, "player_fall_distance %f", &metadata.playerfalldistance) == 1) continue;
        if(sscanf(line, "player_physics_state %d", &metadata.playerphysstate) == 1) continue;
        ullong inventoryid;
        char inventoryidtext[32];
        if(sscanf(line, "inventory_cursor %31s %d %d", inventoryidtext, &metadata.inventorycursorcount,
                  &metadata.inventorycursordurability) >= 2)
        {
            if(!parseworldpersistentid(inventoryidtext, inventoryid)) continue;
            metadata.inventorycursoritem = getinventoryitempersistentindex(inventoryid);
            continue;
        }
        int inventoryslot, inventorycount, inventorydurability = 0;
        if(sscanf(line, "inventory %d %31s %d %d", &inventoryslot, inventoryidtext, &inventorycount, &inventorydurability) >= 3)
        {
            if(!parseworldpersistentid(inventoryidtext, inventoryid)) continue;
            if(inventoryslot >= 0 && inventoryslot < game::SURVIVAL_USABLE_SLOTS &&
               inventorycount > 0)
            {
                metadata.inventoryitems[inventoryslot] = getinventoryitempersistentindex(inventoryid);
                metadata.inventorycounts[inventoryslot] = inventorycount;
                metadata.inventorydurabilities[inventoryslot] = inventorydurability;
            }
            continue;
        }
        static const char hashprefix[] = "worldgen_parameter_hash ";
        if(!strncmp(line, hashprefix, sizeof(hashprefix) - 1))
        {
            char *end = NULL;
            metadata.parameterhash = strtoull(line + sizeof(hashprefix) - 1, &end, 10);
            if(end != line + sizeof(hashprefix) - 1) continue;
        }
        if(sscanf(line, "entry %d %d", &x, &y) == 2)
        {
            chunkx = x;
            chunky = y;
            continue;
        }
        double spawnx, spawny;
        float spawnz, yaw = 0, pitch = 0;
        if(sscanf(line, "spawn %lf %lf %f %f %f",
                  &spawnx, &spawny, &spawnz, &yaw, &pitch) >= 3)
        {
            spawn.valid = true;
            spawn.x = spawnx;
            spawn.y = spawny;
            spawn.z = spawnz;
            spawn.yaw = yaw;
            spawn.pitch = pitch;
        }
    }
    delete f;

    metadata.valid = metarevision >= 3 && metarevision <= 5 && metadata.seed >= 0 &&
                     metadata.worldgenversion > 0 && metadata.saveformatversion > 0;
    if(!metadata.valid)
    {
        conoutf(CON_ERROR,
                "world %s uses legacy metadata without a pinned generator; explicit migration is required",
                folder);
        return false;
    }
    if(metadata.saveformatversion != WORLD_SAVE_FORMAT_VERSION)
    {
        conoutf(CON_ERROR, "world %s uses unsupported save format version %d",
                folder, metadata.saveformatversion);
        return false;
    }
    if(metadata.worldgenversion != WORLDGEN_VERSION)
    {
        conoutf(CON_ERROR,
                "world %s requires worldgen version %d, but this build provides version %d",
                folder, metadata.worldgenversion, WORLDGEN_VERSION);
        return false;
    }
    if(!game::validgamemode(metadata.gamemode)) metadata.gamemode = 0;
    if(!(metadata.playerhealth >= 0 && metadata.playerhealth <= game::PLAYER_MAX_HEALTH))
        metadata.playerhealth = game::PLAYER_MAX_HEALTH;
    bool validmotion = metadata.playerfalldistance >= 0 && metadata.playerfalldistance <= WORLD_MAP_SIZE &&
                       metadata.playerphysstate >= PHYS_FLOAT && metadata.playerphysstate <= PHYS_BOUNCE;
    loopk(3) validmotion = validmotion && metadata.playervelocity[k] >= -65535.0f && metadata.playervelocity[k] <= 65535.0f &&
                                      metadata.playerfalling[k] >= -65535.0f && metadata.playerfalling[k] <= 65535.0f;
    if(!validmotion)
    {
        metadata.playervelocity = metadata.playerfalling = vec(0, 0, 0);
        metadata.playerfalldistance = 0;
        metadata.playerphysstate = PHYS_FALL;
    }
    return true;
}

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

    const int material = lookupmaterial(vec(player->o.x, player->o.y,
                                            max(player->o.z - player->eyeheight + 1, 0.0f)));
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

static bool saveworldstate();
void saveworld();

static void createworld(const char *requestedname)
{
    chooseworldfolder(requestedname);
    string chosenfolder, activechunkname;
    copystring(chosenfolder, worldfolder);
    formatstring(activechunkname, "%s/0_0", chosenfolder);

    defformatstring(metadatafile, "media/map/%s/world.meta", chosenfolder);
    path(metadatafile);
    const char *existingmetadata = findfile(metadatafile, "rb");
    if(existingmetadata && fileexists(existingmetadata, "r"))
    {
        conoutf(CON_ERROR, "world %s already exists; use loadworld %s or choose a new name", chosenfolder, chosenfolder);
        return;
    }

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

    renderprogress(0.94f, "saving your new home before handing over the keys...");
    saveworld();

    int mounted = 0;
    loopv(worldchunks) if(worldchunkmounted(worldchunks[i])) mounted++;
    conoutf("generated infinite world %s with seed %d and %d initial chunks; %d chunks queued asynchronously",
            worldfolder, game::getworldseed(), mounted, worldchunks.length() - mounted);
    conoutf("new chunks are prepared on demand; use saveworld to write ready chunks");
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

static void loadworldcommand(const char *requested)
{
    if(!requested || !*requested)
    {
        conoutf(CON_ERROR, "usage: loadworld <worldname>");
        return;
    }

    string folder;
    normalizeworldfolder(folder, sizeof(folder), requested);
    int chunkx, chunky;
    worldspawnmetadata spawn;
    worlddiffmetadata metadata;
    if(!loadworldmetadata(folder, chunkx, chunky, spawn, metadata))
    {
        conoutf(CON_ERROR, "could not find a saved world named %s", folder);
        return;
    }

    // load_world() releases the currently mounted chunks. Persist the active
    // offline world before that happens, including its chest snapshot.
    if(game::islocalworld() && !worldchunks.empty() && activeworldchunk >= 0)
    {
        if(!saveworldstate())
        {
            conoutf(CON_ERROR, "could not save the active world; refusing to replace it with %s", folder);
            return;
        }
        // Reload in case the requested world is also the active world whose
        // metadata was just updated by saveworldstate().
        if(!loadworldmetadata(folder, chunkx, chunky, spawn, metadata))
        {
            conoutf(CON_ERROR, "could not reload saved world metadata for %s", folder);
            return;
        }
    }
    game::resetfurnaces();
    game::resetchests();
    game::beginlocalworld();
    game::loadworldseed(metadata.seed);
    game::loadsurvivalinventory(metadata.inventoryitems, metadata.inventorycounts, metadata.inventorydurabilities, game::SURVIVAL_USABLE_SLOTS, metadata.inventorycursoritem, metadata.inventorycursorcount, metadata.inventorycursordurability);
    activeworldmetadata = metadata;
    conoutf("loading saved world %s with pinned seed %d", folder, metadata.seed);
    defformatstring(entry, "%s/%d_%d", folder, chunkx, chunky);
    requestedworldspawn = spawn;
    hasrequestedworldspawn = true;
    applyloadworlddefaults = true;
    game::changemap(entry, metadata.gamemode);
    game::restorelocalplayerhealth(metadata.playerhealth);
    game::restorelocalplayermotion(metadata.playervelocity, metadata.playerfalling, metadata.playerfalldistance, metadata.playerphysstate);
    if(!game::loadlocalfurnaces(folder)) conoutf(CON_ERROR, "saved furnace data for world %s is corrupt", folder);
    if(!game::loadlocalchests(folder)) conoutf(CON_ERROR, "saved chest data for world %s is corrupt", folder);
    applyloadworlddefaults = false;
    hasrequestedworldspawn = false;
}

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
        worldchunk &chunk = worldchunks.add(worldchunk(0, 0, game::generateworldchunk(0, 0)));
        indexworldchunk(worldchunks.length() - 1);
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

ICOMMAND(loadworld, "s", (char *name), loadworldcommand(name));

static bool saveworldstate()
{
    if(worldchunks.empty() || activeworldchunk < 0)
    {
        conoutf(CON_ERROR, "no procedural world is active; use newworld first");
        return false;
    }

    if(!saveworldconfig()) return false;
    flushworlddiffjournals(true);
    loopv(worldchunkdiffstates) if(!worldchunkdiffstates[i]->journal.empty())
    {
        int chunkindex = findworldchunk(worldchunkdiffstates[i]->x, worldchunkdiffstates[i]->y);
        if(worldchunks.inrange(chunkindex)) compactworldchunkdiff(worldchunks[chunkindex]);
    }
    int written = 0, unchanged = 0, ready = 0;
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(!chunk.root || chunk.loading) continue;
        ready++;
        if(!chunk.dirty)
        {
            unchanged++;
            continue;
        }
        chunk.saved = true;
        chunk.dirty = false;
        written++;
    }

    int entryx = lastplayerchunkx, entryy = lastplayerchunky;
    if(player)
    {
        const double absolutex = double(worldfirstchunkx) * WORLD_CHUNK_SIZE + player->o.x,
                     absolutey = double(worldfirstchunky) * WORLD_CHUNK_SIZE + player->o.y;
        entryx = int(floor(absolutex / WORLD_CHUNK_SIZE));
        entryy = int(floor(absolutey / WORLD_CHUNK_SIZE));
    }
    int entry = findworldchunk(entryx, entryy);
    if(!worldchunks.inrange(entry)) entry = activeworldchunk;
    if(worldchunks.inrange(entry))
    {
        entryx = worldchunks[entry].x;
        entryy = worldchunks[entry].y;
    }
    if(!saveworldmetadata(entryx, entryy)) return false;
    if(!game::savelocalfurnaces(worldfolder))
    {
        conoutf(CON_ERROR, "could not save furnace state for world %s", worldfolder);
        return false;
    }
    if(!game::savelocalchests(worldfolder))
    {
        conoutf(CON_ERROR, "could not save chest state for world %s", worldfolder);
        return false;
    }

    int released = 0;
    for(int i = worldchunks.length() - 1; i >= 0; --i)
    {
        worldchunk &chunk = worldchunks[i];
        if(worldchunkmounted(chunk) || chunk.loading || !chunk.root || worldchunkinview(chunk, lastplayerchunkx, lastplayerchunky))
            continue;
        freeocta(chunk.root);
        worldchunks.removeunordered(i);
        released++;
    }
    activeworldchunk = findworldchunk(lastplayerchunkx, lastplayerchunky);
    string name;
    if(worldchunks.inrange(activeworldchunk))
    {
        worldchunkname(name, sizeof(name), worldchunks[activeworldchunk]);
        setmapfilenames(name);
    }
    conoutf("saved world %s: %d chunk journals queued, %d unchanged, %d ready; released %d cached chunks", worldfolder, written, unchanged, ready, released);
    return true;
}

void saveworld()
{
    saveworldstate();
}

void closeproceduralworld(bool save)
{
    // Save while the active folder, mounted chunks and diff states still
    // identify the world. clearworldchunks() then flushes and joins both the
    // diff writer and generation workers before releasing their state.
    if(save && !worldchunks.empty() && activeworldchunk >= 0) saveworld();
    game::resetfurnaces();
    game::resetchests();
    clearworldchunks();
    resetmap();
    freeocta(worldroot);
    worldroot = newcubes(F_SOLID);
}

COMMAND(saveworld, "");

static uint mapcrc = 0;

uint getmapcrc() { return mapcrc; }
void clearmapcrc() { mapcrc = 0; }

static bool loadseedworld(const char *mname, const char *cname)
{
    string folder, normalized;
    copystring(normalized, mname);
    loopi(strlen(normalized)) if(normalized[i] == '\\') normalized[i] = '/';
    char *slash = strrchr(normalized, '/');
    int chunkx, chunky;
    if(!slash || !chunkcoords(slash + 1, chunkx, chunky)) return false;
    *slash = '\0';
    copystring(folder, normalized);

    worldspawnmetadata spawn;
    worlddiffmetadata metadata;
    int entryx, entryy;
    if(!loadworldmetadata(folder, entryx, entryy, spawn, metadata) ||
       entryx != chunkx || entryy != chunky)
        return false;

    setmapfilenames(mname, cname);
    clearworldchunks();
    resetmap();
    activeworldmetadata = metadata;
    game::loadworldseed(metadata.seed);

    identflags |= IDF_OVERRIDDEN;
    execfile("config/default_map_settings.cfg", false);
    defformatstring(worldconfig, "media/map/%s/world.cfg", folder);
    if(!execfile(worldconfig, false))
    {
        identflags &= ~IDF_OVERRIDDEN;
        conoutf(CON_ERROR, "could not load deterministic world configuration %s", worldconfig);
        return false;
    }
    identflags &= ~IDF_OVERRIDDEN;
    if(game::getworldseed() != metadata.seed ||
       currentworldparameterhash() != metadata.parameterhash)
    {
        conoutf(CON_ERROR,
                "world %s generator parameter hash does not match world.meta; refusing silent terrain changes",
                folder);
        return false;
    }
    game::weather::preparemap(folder, metadata.seed);

    setvar("mapscale", WORLD_CHUNK_SCALE, true, false);
    setvar("mapsize", WORLD_CHUNK_MAP_SIZE, true, false);
    texmru.shrink(0);
    freeocta(worldroot);
    string cachefilename;
    worldchunkcachefilename(cachefilename, sizeof(cachefilename), folder, chunkx, chunky);
    int cachefamilies = 0, cacheerror = 0;
    worldroot = generatedchunkcache ? loadworldchunkcache(cachefilename, chunkx, chunky, game::getworldseed(), game::worldgenerationparameterhash(),
                                                          chunkremip != 0, reconstructedworldscatter, false, cachefamilies, cacheerror) : NULL;
    if(!worldroot)
    {
        ZoneScopedN("Chunks/Generate uncached");
        worldroot = game::generateworldchunk(chunkx, chunky);
        if(worldroot) game::generateworldscatter(worldroot, chunkx, chunky, reconstructedworldscatter);
        vector<uchar> cachepayload;
        if(generatedchunkcache && worldroot && serializeworldchunkcache(worldroot, reconstructedworldscatter, cachepayload))
            queueworldchunkcachewrite(chunkx, chunky, game::getworldseed(), game::worldgenerationparameterhash(), chunkremip != 0, cachepayload);
    }
    if(!worldroot) return false;
    setworldleavesalpha(worldroot, leavesalpha != 0);
    reconstructedworldscatterready = true;

    defformatstring(diffrelative, "media/map/%s/chunks/%d_%d_%d.diff",
                    folder, chunkx, chunky, WORLD_DIFF_Z);
    path(diffrelative);
    const char *found = findfile(diffrelative, "rb");
    if(found && fileexists(found, "r"))
    {
        int families = 0;
        ullong revision = 0, canonicalhash = 0;
        worldchunkdirtybounds dirty;
        applyworldchunkdiff(worldroot, chunkx, chunky, diffrelative,
                            reconstructedworldscatter, false, families,
                            revision, canonicalhash, &dirty);
        if(chunkremip && dirty.valid) remipworldchunkbounded(worldroot, false, families, NULL, &dirty);
        worldchunkdiffstate *state = findworldchunkdiffstate(chunkx, chunky, true);
        state->revision = revision;
        worldeditrevision = max(worldeditrevision, revision);
        state->canonicalhash = hashworldchunk(worldroot);
    }

    preparedworldspawn = false;
    requestedworldspawn = spawn;
    hasrequestedworldspawn = true;
    if(!loadworldchunks(mname) || !prepareworldspawn(spawn))
    {
        hasrequestedworldspawn = false;
        return false;
    }
    loadworldauditlog();
    hasrequestedworldspawn = false;
    calcmerges();
    allchanged(true);
    clearmainmenu();
    startmap(cname ? cname : mname);
    restoreworldwatersources();
    applypreparedworldspawn();
    mapcrc = 0;
    conoutf("reconstructed world %s from seed %d and chunk diffs", folder, metadata.seed);
    return true;
}

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
        if(loadseedworld(mname, cname)) return true;
        conoutf(CON_ERROR, "could not read map %s or reconstruct a seed-based world", ogzname);
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
        if(applyloadworlddefaults)
        {
            setvar("ambient", 0x252525);
            setvar("sunlight", 0xFFF8E0);
            setfvar("sunlightyaw", 30);
            setfvar("sunlightpitch", 50);
            setvar("atmo", 1);
        }
        execfile(cfgname, false);
        identflags &= ~IDF_OVERRIDDEN;
    }

    bool streamedworld = false;
    preparedworldspawn = false;
    if(!cname && hdr.worldsize == WORLD_CHUNK_MAP_SIZE)
    {
        streamedworld = loadworldchunks(mname);
        if(streamedworld)
        {
            worldspawnmetadata spawn;
            if(hasrequestedworldspawn) spawn = requestedworldspawn;
            if(!prepareworldspawn(spawn)) return false;
        }
    }

    {
        ZoneScopedN("Chunks/Build entry geometry");
        if(streamedworld) calcmerges();
        allchanged(true);
    }

    renderbackground("loading...", mapshot, mname, game::getmapinfo());

    if(maptitle[0] && strcmp(maptitle, "Untitled Map by Unknown")) conoutf(CON_ECHO, "%s", maptitle);

    startmap(cname ? cname : mname);
    restoreworldwatersources();

    if(streamedworld) applypreparedworldspawn();

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
