#include "game.h"
#include <errno.h>
#include "world.h"
#include "worldcube.h"
#include "../engine/world.h"
#include "worlddef.h"
#include <SDL.h>
#include <SDL_atomic.h>
#include "worldruntime.h"
#include "worldgen.h"
#define WORLDIO_MODULE_IMPLEMENTATION
#define WORLD_SNAPSHOT_SERVER_CODEC
#include "worldcache.cpp"
#undef WORLD_SNAPSHOT_SERVER_CODEC
#undef WORLDIO_MODULE_IMPLEMENTATION

namespace server
{
    static void decodedirection(int yaw, int pitch, vec &direction)
    {
        const float pitchcos = cosf(RAD*pitch);
        direction = vec(-sinf(RAD*yaw)*pitchcos,
                         cosf(RAD*yaw)*pitchcos,
                         sinf(RAD*pitch));
    }

    enum
    {
        SURVIVAL_HOTBAR_SLOTS = game::SURVIVAL_HOTBAR_SLOTS,
        SURVIVAL_USABLE_SLOTS = game::SURVIVAL_USABLE_SLOTS
    };

    enum
    {
        SERVER_DAY_MILLIS = 20 * 60 * 1000,
        SERVER_START_MILLIS = 8 * SERVER_DAY_MILLIS / 24,
        SERVER_IDENTITY_DB_VERSION = 2,
        PLAYER_IDENTITY_VERSION = 1,
        PLAYER_IDENTITY_TIMEOUT = 15000,
        PLAYER_IDENTITY_MAX_RECORDS = 100000,
        PLAYER_STATE_VERSION = 4,
        DROP_PICKUP_DELAY = 500
    };

    enum identityauthstate
    {
        IDENTITY_UNAUTHENTICATED = 0,
        IDENTITY_AWAITING_IDENTITY,
        IDENTITY_AWAITING_RESPONSE,
        IDENTITY_AUTHENTICATED,
        IDENTITY_REJECTED
    };

    enum identitykind
    {
        IDENTITY_KIND_NONE = 0,
        IDENTITY_KIND_NEW,
        IDENTITY_KIND_RETURNING,
        IDENTITY_KIND_RECOVERY
    };

    SVAR(serverpass, "");
    SVAR(adminpass, "");
    SVAR(serverworld, "multiplayer");
    SVAR(serverdesc, "Cube-Craft authoritative server");
    SVAR(servermotd, "");
    VAR(serverworldseed, 0, 1337, INT_MAX);
    VAR(serverweatherseed, 0, 0, INT_MAX);
    FVAR(servercloudwindspeed, 0.0f, 16.0f, 64.0f);
    FVAR(servercloudwindangle, 0.0f, 18.0f, 360.0f);
    VAR(identityduplicatepolicy, 0, 0, 1);
    VAR(creativemode, 0, 1, 1);
    VAR(inventorysaveinterval, 1, 15, 3600);
    VAR(playerstatesaveinterval, 1, 15, 3600);
    VAR(buildreach, 16, 160, 1024);
    VAR(placementratelimit, 1, 12, 100);
    VAR(destructionratelimit, 1, 12, 100);
    VAR(survivalbreakmillis, 100, 5000, 60000);
    VAR(survivalscatterbreakmillis, 50, 250, 60000);
    VAR(serverwaterupdatespertick, 1, 1024, 16384);
    VAR(serverwatersimulationmaxdist, 1, 128, 1024);
    FVAR(serverwaterflowspeed, 0.1f, 4.0f, 20.0f);
    VAR(breaktimetolerance, 0, 125, 2000);
    VAR(breakcancelgrace, 0, 125, 2000);
    VAR(breaknetworkrange, 16, 512, 4096);
    VAR(desynctolerance, 0, 4, 100);
    VAR(violationresetinterval, 1, 60, 3600);
    VAR(identitybankicks, 1, 3, 100);
    VAR(serversimulationmaxdist, 1, 128, 1024);
    VAR(servernpcmaxdist, 1, 256, 4096);
    VAR(servernpcsnapshotmillis, 33, 100, 1000);
    VAR(servernpcdeathtimeout, 1000, 20000, 120000);
    VAR(servernpcspawnmillis, 100, 500, 60000);
    VAR(serversupportdecaymillis, 10, 3000, 60000);
    VAR(maxchunkdist, 1, 3, WORLD_MAX_CHUNK_DIST);
#ifdef STANDALONE
    VAR(personaldrops, 0, 0, 1);
    VAR(droptimeout, 1, 300, 86400);
    VAR(maxdrop, 1, 1024, 100000);
    VAR(dynamicentsmaxdistance, 1, 64, 4096);
    VAR(requireconfirmeditems, 0, 1, 1);
#else
    VAR(serverpersonaldrops, 0, 0, 1);
    VAR(serverdroptimeout, 1, 300, 86400);
    VAR(servermaxdrop, 1, 1024, 100000);
    VAR(serverdynamicentsmaxdistance, 1, 64, 4096);
    VAR(serverrequireconfirmeditems, 0, 1, 1);
    static int personaldrops = 0, droptimeout = 300, maxdrop = 1024, dynamicentsmaxdistance = 64, requireconfirmeditems = 1;
#endif

    struct serveridentity
    {
        string playerid, publickey, nickname;
        int permissions, kicks;
        bool revoked, banned;

        serveridentity() : permissions(0), kicks(0), revoked(false), banned(false)
        {
            playerid[0] = publickey[0] = nickname[0] = '\0';
        }
    };

    static vector<serveridentity *> serveridentities;
    static string persistentserverid = "";
    static bool serveridentitiesloaded = false;

    struct identityratelimit
    {
        uint ip;
        int failures, window;
    };

    static vector<identityratelimit> identityratelimits;

    struct clientinfo
    {
        int clientnum, privilege, lastpositionmillis, lastpositionsave, positionyaw, positionpitch, positionphysstate, identitystate, identitykind,
            identitychallengemillis,
            identityfailures, identityfailurewindow, selectedslot, inventorycursoritem, inventorycursorcount, lastinventorysave,
            violations, violationwindow, actionwindow, placements, destructions,
            breakaction, breakorient, breakitem, breakstart, breakupdate, breakstage, breakrelease,
            breakduration, breaktoolitem, breaktoolslot, breaktooldurability, selectedcreative, lastnpcattack, lastnpcattackattempt,
            deathsequence, foodstart, fooditem, foodslot;
        float health, falldistance;
        uint ip;
        uint lastrequestid, breakrequestid, lastnpcattackrequest;
        bool connected, local, worldready, hasposition, positiondirty, inventoryloaded, inventorydirty, breakactive, breakdropeligible, furnaceopen,
             chestopen, dead, foodactive;
        string name, playerid, pendingpublickey, pendingname;
        int inventoryitems[SURVIVAL_USABLE_SLOTS], inventorycounts[SURVIVAL_USABLE_SLOTS], inventorydurabilities[SURVIVAL_USABLE_SLOTS];
        int craftingitems[CRAFT_GRID_MAX], craftingcounts[CRAFT_GRID_MAX], craftingdurabilities[CRAFT_GRID_MAX],
            craftinggridsize, craftingstationitem, inventorycursordurability;
        ivec positioncoords, breaktarget, craftingstationtarget, furnacetarget, chesttarget;
        vector<uchar> position;
        vector<uint> knownnpcs;
        vec o, velocity, falling;
        ENetPacket *getmap;
        void *identitychallenge;
        serveridentity *identity;

        clientinfo() : clientnum(-1), privilege(PRIV_NONE), lastpositionmillis(0), lastpositionsave(0), positionyaw(0), positionpitch(0),
                       positionphysstate(PHYS_FALL),
                       identitystate(IDENTITY_UNAUTHENTICATED), identitykind(IDENTITY_KIND_NONE),
                       identitychallengemillis(0),
                       identityfailures(0), identityfailurewindow(0),
                       selectedslot(0), inventorycursoritem(-1), inventorycursorcount(0), lastinventorysave(0),
                       violations(0), violationwindow(0),
                       actionwindow(0), placements(0), destructions(0), breakaction(-1),
                       breakorient(0), breakitem(-1), breakstart(0), breakupdate(0), breakstage(0), breakrelease(0),
                       breakduration(0), breaktoolitem(-1), breaktoolslot(-1), breaktooldurability(0), selectedcreative(-1),
                       lastnpcattack(-1000), lastnpcattackattempt(-1000), deathsequence(0), foodstart(0), fooditem(-1), foodslot(-1),
                       health(game::PLAYER_MAX_HEALTH), falldistance(0),
                       ip(0),
                       lastrequestid(0), breakrequestid(0), lastnpcattackrequest(0),
                       connected(false), local(false),
                       worldready(false), hasposition(false), positiondirty(false), inventoryloaded(false), inventorydirty(false),
                       breakactive(false), breakdropeligible(true), furnaceopen(false), chestopen(false), dead(false), foodactive(false),
                       craftinggridsize(2), craftingstationitem(-1), inventorycursordurability(0),
                       positioncoords(0, 0, 0), breaktarget(0, 0, 0), craftingstationtarget(0, 0, 0), furnacetarget(0, 0, 0), chesttarget(0, 0, 0),
                       o(0, 0, 0), velocity(0, 0, 0), falling(0, 0, 0), getmap(NULL),
                       identitychallenge(NULL), identity(NULL)
        {
            name[0] = playerid[0] = pendingpublickey[0] = pendingname[0] = '\0';
            loopi(SURVIVAL_USABLE_SLOTS)
            {
                inventoryitems[i] = -1;
                inventorycounts[i] = 0;
                inventorydurabilities[i] = 0;
            }
            loopi(CRAFT_GRID_MAX)
            {
                craftingitems[i] = -1;
                craftingcounts[i] = 0;
                craftingdurabilities[i] = 0;
            }
        }

        ~clientinfo()
        {
            if(identitychallenge) freechallenge(identitychallenge);
        }
    };

    struct serveredit
    {
        uint revision, timestamp;
        int author, type;
        uint requestid;
        bool active, hasselection;
        string ownerid;
        selinfo selection;
        vector<uchar> payload;

        serveredit() : revision(0), timestamp(0), author(-1), type(-1), requestid(0),
                       active(true), hasselection(false)
        {
            ownerid[0] = '\0';
        }
    };

    struct serverworldaction
    {
        ivec target;
        int action, orient, item;
        serverworldaction() : target(0, 0, 0), action(-1), orient(0), item(-1) {}
    };

    enum
    {
        SERVER_WORLD_BLOCK_SIZE = 16,
        SERVER_WORLD_CHUNK_BLOCKS = 64,
        SERVER_WORLD_CHUNK_SIZE = SERVER_WORLD_BLOCK_SIZE * SERVER_WORLD_CHUNK_BLOCKS,
        SERVER_WORLD_GROUND_HEIGHT = 4096,
        SERVER_WORLD_MAP_SIZE = SERVER_WORLD_GROUND_HEIGHT * 2,
        SERVER_PLAYER_EYE_HEIGHT = 28
    };

    struct servercollisionchunk
    {
        int x, y, lastused, supportscan;
        short heights[SERVER_WORLD_CHUNK_BLOCKS * SERVER_WORLD_CHUNK_BLOCKS];

        servercollisionchunk(int x = 0, int y = 0) : x(x), y(y), lastused(totalmillis), supportscan(0)
        {
            memset(heights, 0, sizeof(heights));
        }
    };

    struct serverchunk
    {
        int x, y, saveafter;
        uint revision, serializedrevision, storageversion;
        cube *root;
        worldsectionrenderdata renderdata;
        vector<worldscatterinstance> scatter;
        vector<uchar> gameplay, vox, dat;
        bool loading, saving, dirty, playeredited, corrupted;

        serverchunk(int x, int y)
            : x(x), y(y), saveafter(0), revision(1), serializedrevision(0), storageversion(1), root(NULL), loading(true), saving(false), dirty(false),
              playeredited(false), corrupted(false)
        {
        }

        ~serverchunk() { freeworldsnapshotfamily(root); }
    };

    enum { SERVER_CHUNK_LOAD = 0, SERVER_CHUNK_SAVE };

    struct serverchunkjob
    {
        int type, x, y;
        uint revision, storageversion;
        cube *root;
        worldsectionrenderdata renderdata;
        vector<worldscatterinstance> scatter;
        vector<uchar> gameplay, vox, dat;
        bool playeredited, success, missing, compress;
        string error;

        serverchunkjob(int type, int x, int y)
            : type(type), x(x), y(y), revision(1), storageversion(1), root(NULL), playeredited(false), success(false), missing(false),
              compress(compresschunks != 0)
        {
            error[0] = '\0';
        }

        ~serverchunkjob() { freeworldsnapshotfamily(root); }
    };

    struct serverchunkrequest
    {
        int clientnum, x, y;
        serverchunkrequest(int clientnum, int x, int y) : clientnum(clientnum), x(x), y(y) {}
    };

    struct serverchunkdelivery
    {
        int clientnum, x, y;
        uint revision;

        serverchunkdelivery(int clientnum = -1, int x = 0, int y = 0, uint revision = 0)
            : clientnum(clientnum), x(x), y(y), revision(revision) {}
    };

    struct serverdeadpassivenpc
    {
        ullong key;
        int chunkx, chunky;

        serverdeadpassivenpc(ullong key = 0, int chunkx = 0, int chunky = 0) : key(key), chunkx(chunkx), chunky(chunky) {}
    };

    struct servernpc
    {
        uint id, detachedparts;
        ullong spawnkey;
        npcdefinition *definition;
        vec o, velocity, spawn, destination, fleeorigin;
        float yaw, health, falldistance, parthealth[NUM_HUMANOID_HITBOXES];
        int behavior, nextdecision, pauseuntil, lastupdate, lastattack, deathmillis, fleeuntil;
        bool paused, frozen, attacking, airborne;

        servernpc(uint id, npcdefinition *definition)
            : id(id), detachedparts(0), spawnkey(0), definition(definition), o(0, 0, 0), velocity(0, 0, 0), spawn(0, 0, 0), destination(0, 0, 0),
              fleeorigin(0, 0, 0), yaw(0),
              health(definition->health), falldistance(0), behavior(definition->behavior), nextdecision(0), pauseuntil(0), lastupdate(totalmillis),
              lastattack(-1000), deathmillis(0), fleeuntil(0), paused(true), frozen(false), attacking(false), airborne(false)
        {
            parthealth[HITBOX_TORSO] = definition->health;
            loopi(NUM_HUMANOID_HITBOXES - 1) parthealth[i + 1] = max(definition->health * 0.25f, 1.0f);
        }
    };

    struct serverdrop
    {
        uint id, sourcerequestid;
        int source, item, count, durability, created;
        string ownerid;
        vec o;

        serverdrop() : id(0), sourcerequestid(0), source(-1), item(-1), count(0), durability(0), created(0), o(0, 0, 0)
        {
            ownerid[0] = '\0';
        }
    };

    struct serverfallingblock
    {
        uint id;
        int item, lastupdate;
        float velocity;
        ivec origin;
        vec o;

        serverfallingblock() : id(0), item(-1), lastupdate(totalmillis), velocity(0), origin(0, 0, 0), o(0, 0, 0) {}
    };

    struct serversupportcell
    {
        int distance, index, unsupportedindex;

        serversupportcell(int distance = 0, int index = -1) : distance(distance), index(index), unsupportedindex(-1) {}
    };

    struct serversupportcheck
    {
        ivec cell;
        int remaining;

        serversupportcheck(const ivec &cell = ivec(0, 0, 0), int remaining = 0) : cell(cell), remaining(remaining) {}
    };

    static vector<servercollisionchunk *> servercollisionchunks;
    static vector<serverchunk *> serverchunks;
    static vector<serverchunkrequest> serverchunkrequests;
    static vector<serverchunkdelivery> serverchunkdeliveries;
    static vector<serverchunkjob *> serverchunkjobs, serverchunkresults;
    static SDL_Thread *serverchunkthread = NULL;
    static SDL_mutex *serverchunkmutex = NULL;
    static SDL_cond *serverchunkcond = NULL;
    static bool stopserverchunkthread = false;
    static vector<servernpc *> servernpcs;
    static vector<serverdeadpassivenpc> serverdeadpassivenpcs;
    static game::worldgenerator *serverworldgenerator = NULL;
    static bool servermapspawnready = false;
    static vec servermapspawn;
    static int servermapspawnyaw = 0, servermapspawnpitch = 0;
    static uint nextnpcid = 1;
    static int lastnpcsnapshot = 0, lastnpcspawnattempt = 0, lastpassivenpcscan = 0;
    static vector<serverdrop *> serverdrops;
    static vector<serverfallingblock *> serverfallingblocks;
    static vector<ivec> serverfallblockchecks;
    static hashtable<ivec, serversupportcell> serversupportcells(1 << 12);
    static vector<ivec> serversupportpositions;
    static vector<ivec> serverunsupportedpositions;
    static vector<serversupportcheck> serversupportchecks;
    static vector<furnaceinstance *> serverfurnaces;
    static vector<chestinstance *> serverchests;
    static uint nextdropid = 1;
    static uint nextfallblockid = 1;
    static serverworldaction *findworldaction(const ivec &target, int action);
    static serverchunk *serverchunkforcell(const ivec &cell, bool queue = false);
    static void setworldactionstate(const ivec &target, int action, int orient, int item, bool playerplaced = false);
    static void queueserverfallblockcheck(const ivec &cell);
    static void queueserversupportchange(const ivec &cell);
    static void sendfallblockspawn(int cn, const serverfallingblock &block);
    static void broadcastfallblockspawn(const serverfallingblock &block);
    static void broadcastdropspawn(const serverdrop &drop);
    static chestinstance *findserverchest(const ivec &target);
    static int serverfloordiv(int value, int divisor);
    static int serverblockitem(const ivec &cell);
    static void dirtyserverchunk(serverchunk &chunk);
    static void dirtyservergameplaycell(const ivec &cell);

    vector<clientinfo *> clients;
    string smapname = "";
    stream *mapdata = NULL;
    int gamemode = STARTGAMEMODE;
    uint worldeditrevision = 0;
    int worldclockmillis = SERVER_START_MILLIS, lastworldtimesync = 0;
    uint weatherclockmillis = 0;
    bool worldtimefrozen = false, serverworldready = true, serverworldinitialized = false;

    static bool servercreative()
    {
        return gamemode == STARTGAMEMODE;
    }

    static bool valididentityhex(const char *value, int minlen, int maxlen)
    {
        if(!value) return false;
        int len = 0;
        for(; value[len]; ++len)
            if(!isxdigit((uchar)value[len]) || len >= maxlen) return false;
        return len >= minlen;
    }

    static bool valididentitypoint(const char *value)
    {
        return value && (*value == '+' || *value == '-') && valididentityhex(value + 1, 1, 64);
    }

    static bool writeserveridentitystring(stream &file, const char *value)
    {
        int len = value ? int(strlen(value)) : 0;
        return len <= USHRT_MAX && file.putlil<ushort>(ushort(len)) && (!len || file.write(value, len) == size_t(len));
    }

    static bool readserveridentitystring(stream &file, char *value, int size)
    {
        uint len = file.getlil<ushort>();
        if(len >= uint(size)) return false;
        if(len && file.read(value, len) != len) return false;
        value[len] = '\0';
        return true;
    }

    static bool replaceserveridentityfile(const char *temporary, const char *finalname)
    {
#ifdef WIN32
        return MoveFileEx(temporary, finalname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        return rename(temporary, finalname) == 0;
#endif
    }

    static bool writeserveridentities()
    {
        const char *name = "config/server-identities.dat";
        defformatstring(tempname, "%s.tmp", name);
        string finalpath, temppath;
        copystring(finalpath, findfile(name, "wb"));
        copystring(temppath, findfile(tempname, "wb"));
        stream *file = openrawfile(tempname, "wb");
        if(!file) return false;
        bool ok = file->write("CCSI", 4) == 4 &&
                  file->putlil<uint>(SERVER_IDENTITY_DB_VERSION) &&
                  writeserveridentitystring(*file, persistentserverid) &&
                  file->putlil<uint>(uint(serveridentities.length()));
        loopv(serveridentities)
        {
            serveridentity &identity = *serveridentities[i];
            if(ok) ok = writeserveridentitystring(*file, identity.playerid) &&
                        writeserveridentitystring(*file, identity.publickey) &&
                        writeserveridentitystring(*file, identity.nickname) &&
                        file->putlil<int>(identity.permissions) &&
                        file->putlil<int>(identity.kicks) &&
                        file->putlil<uint>(identity.revoked ? 1U : 0U) &&
                        file->putlil<uint>(identity.banned ? 1U : 0U);
        }
        delete file;
        if(!ok)
        {
            remove(temppath);
            return false;
        }
        if(!replaceserveridentityfile(temppath, finalpath))
        {
            remove(temppath);
            return false;
        }
        return true;
    }

    static void makepersistentid(char *id, int size, int discriminator = 0)
    {
        (void)discriminator;
        if(!identityrandomhex(id, size, 24)) id[0] = '\0';
    }

    static serveridentity *findserveridentity(const char *playerid)
    {
        loopv(serveridentities)
        {
            if(!strcmp(serveridentities[i]->playerid, playerid)) return serveridentities[i];
        }
        return NULL;
    }

    static serveridentity *findserveridentitybykey(const char *publickey)
    {
        loopv(serveridentities)
        {
            if(!strcmp(serveridentities[i]->publickey, publickey)) return serveridentities[i];
        }
        return NULL;
    }

    static bool loadserveridentities()
    {
        if(serveridentitiesloaded) return persistentserverid[0] != '\0';
        serveridentitiesloaded = true;
        stream *file = openrawfile("config/server-identities.dat", "rb");
        if(!file)
        {
            makepersistentid(persistentserverid, sizeof(persistentserverid));
            bool saved = persistentserverid[0] && writeserveridentities();
            if(saved) conoutf("created persistent server identity; player database is empty");
            else conoutf(CON_ERROR, "could not create the persistent server identity database");
            return saved;
        }
        char magic[4];
        uint version = 0, count = 0;
        bool ok = file->read(magic, 4) == 4 && !memcmp(magic, "CCSI", 4) &&
                  (version = file->getlil<uint>()) >= 1 && version <= SERVER_IDENTITY_DB_VERSION &&
                  readserveridentitystring(*file, persistentserverid, sizeof(persistentserverid)) &&
                  valididentityhex(persistentserverid, 48, 48) &&
                  (count = file->getlil<uint>()) <= PLAYER_IDENTITY_MAX_RECORDS;
        loopi(ok ? int(count) : 0)
        {
            serveridentity *identity = new serveridentity;
            uint revoked = 0, banned = 0;
            ok = readserveridentitystring(*file, identity->playerid, sizeof(identity->playerid)) &&
                 readserveridentitystring(*file, identity->publickey, sizeof(identity->publickey)) &&
                 readserveridentitystring(*file, identity->nickname, sizeof(identity->nickname)) &&
                 (identity->permissions = file->getlil<int>(), true) &&
                 (version < 2 || (identity->kicks = file->getlil<int>(), true)) &&
                 (revoked = file->getlil<uint>(), true) &&
                 (banned = file->getlil<uint>(), true) &&
                 valididentityhex(identity->playerid, 48, 48) &&
                 valididentitypoint(identity->publickey) && identity->kicks >= 0 && revoked <= 1 && banned <= 1 &&
                 !findserveridentity(identity->playerid) &&
                 !findserveridentitybykey(identity->publickey);
            void *parsed = ok ? parsepubkey(identity->publickey) : NULL;
            if(parsed) freepubkey(parsed);
            else ok = false;
            identity->revoked = revoked != 0;
            identity->banned = banned != 0;
            if(ok) serveridentities.add(identity);
            else delete identity;
            if(!ok) break;
        }
        if(ok) ok = file->tell() == file->size();
        delete file;
        if(!ok)
        {
            persistentserverid[0] = '\0';
            serveridentities.deletecontents();
            conoutf(CON_ERROR, "server identity database is corrupt");
        }
        else
        {
            int revoked = 0, banned = 0;
            loopv(serveridentities)
            {
                if(serveridentities[i]->revoked) ++revoked;
                if(serveridentities[i]->banned) ++banned;
            }
            conoutf("loaded %d registered player identities (%d revoked, %d banned)", serveridentities.length(), revoked, banned);
        }
        return ok;
    }

    static bool worldactionusessupport(int action)
    {
        // Packets identify the supporting face, while authoritative occupancy
        // is keyed by the adjacent cell that is actually modified.
        return action == WORLD_ACTION_PLACE_CUBE || action == WORLD_ACTION_PLACE_SCATTER ||
               action == WORLD_ACTION_PLACE_ITEM || action == WORLD_ACTION_BREAK_SCATTER_START;
    }

    static ivec worldactionstatecell(const ivec &target, int action, int orient)
    {
        ivec cell = target;
        if(worldactionusessupport(action) && orient >= 0 && orient <= 5)
            cell[orient >> 1] += orient&1 ? 16 : -16;
        return cell;
    }

    static int worldmountorient(int orient) { return ((orient % 6) + 6) % 6; }
    static int worldplaceyaw(int orient) { return clamp(orient / 6, 0, 3) * 90; }

    static void loadserverstate()
    {
        serverdrops.deletecontents();
        serverfallingblocks.deletecontents();
        serverfallblockchecks.setsize(0);
        serversupportcells.clear();
        serversupportpositions.setsize(0);
        serverunsupportedpositions.setsize(0);
        serversupportchecks.setsize(0);
        serverfurnaces.deletecontents();
        serverchests.deletecontents();
        serverdeadpassivenpcs.setsize(0);
        nextdropid = 1;
        nextfallblockid = 1;
        worldeditrevision = 0;
        serverworldready = true;

    }

    static bool ensureserverworld()
    {
        if(!serverworldinitialized)
        {
            serverworldinitialized = true;
            loadserverstate();
        }
        return serverworldready;
    }

    static serverchunk *findserverchunk(int x, int y)
    {
        loopv(serverchunks) if(serverchunks[i]->x == x && serverchunks[i]->y == y) return serverchunks[i];
        return NULL;
    }

    static void serverchunkfolder(char *folder, size_t length)
    {
        size_t written = 0;
        const char *storageworld = serverworldinitialized && smapname[0] ? smapname : serverworld;
        for(const char *source = storageworld; *source && written + 1 < length; ++source)
            if(iscubealnum(*source) || *source == '_' || *source == '-') folder[written++] = *source;
        folder[written] = '\0';
        if(!folder[0]) copystring(folder, "multiplayer", length);
    }

    static int serverchunkworker(void *)
    {
        for(;;)
        {
            SDL_LockMutex(serverchunkmutex);
            while(serverchunkjobs.empty() && !stopserverchunkthread) SDL_CondWait(serverchunkcond, serverchunkmutex);
            if(stopserverchunkthread)
            {
                SDL_UnlockMutex(serverchunkmutex);
                return 0;
            }
            serverchunkjob *job = serverchunkjobs.remove(0);
            SDL_UnlockMutex(serverchunkmutex);

            string folder;
            serverchunkfolder(folder, sizeof(folder));
            if(job->type == SERVER_CHUNK_LOAD)
            {
                const worldsnapshotloadresult loaded = loadworldchunksnapshotdata(folder, job->x, job->y, job->root, job->scatter,
                                                                                  job->renderdata, job->gameplay, job->error, &job->revision,
                                                                                  &job->playeredited, &job->vox, &job->dat);
                if(loaded == WORLD_SNAPSHOT_MISSING)
                {
                    job->missing = true;
                    worldgencontext *generation = game::createworldgeneration(true, false, NULL, true);
                    int families = 0, optimized = 0;
                    job->root = game::generateworldchunk(generation, job->x, job->y, families, optimized, &job->renderdata);
                    if(job->root) game::generateworldscatter(generation, job->root, job->x, job->y, job->scatter);
                    game::destroyworldgeneration(generation);
                    job->revision = 1;
                    job->playeredited = false;
                    job->success = job->root != NULL;
                    if(!job->success && !job->error[0]) copystring(job->error, "world generation failed");
                }
                else if(loaded == WORLD_SNAPSHOT_LOADED) job->success = true;
            }
            else
            {
                job->success = serializeworldchunksnapshot(job->root, job->x, job->y, job->revision, job->playeredited, job->renderdata,
                                                           job->scatter, job->gameplay, job->vox, job->dat, job->error);
                if(job->success)
                {
                    string voxname, datname;
                    worldchunksnapshotfilename(voxname, sizeof(voxname), folder, job->x, job->y, "vox");
                    worldchunksnapshotfilename(datname, sizeof(datname), folder, job->x, job->y, "dat");
                    job->success = writeworldsnapshotfile(voxname, job->vox, job->compress) &&
                                   writeworldsnapshotfile(datname, job->dat, job->compress);
                    if(!job->success) copystring(job->error, "could not write .vox/.dat chunk pair");
                }
            }

            SDL_LockMutex(serverchunkmutex);
            serverchunkresults.add(job);
            SDL_CondBroadcast(serverchunkcond);
            SDL_UnlockMutex(serverchunkmutex);
        }
    }

    static bool startserverchunkworker()
    {
        if(serverchunkthread) return true;
        serverchunkmutex = SDL_CreateMutex();
        serverchunkcond = SDL_CreateCond();
        if(!serverchunkmutex || !serverchunkcond) return false;
        stopserverchunkthread = false;
        serverchunkthread = SDL_CreateThread(serverchunkworker, "server chunk storage", NULL);
        return serverchunkthread != NULL;
    }

    static void prioritizeserverchunkload(int x, int y)
    {
        if(!serverchunkmutex) return;
        SDL_LockMutex(serverchunkmutex);
        loopv(serverchunkjobs)
        {
            serverchunkjob *job = serverchunkjobs[i];
            if(job->type != SERVER_CHUNK_LOAD || job->x != x || job->y != y) continue;
            if(i) serverchunkjobs.insert(0, serverchunkjobs.remove(i));
            SDL_CondSignal(serverchunkcond);
            break;
        }
        SDL_UnlockMutex(serverchunkmutex);
    }

    static serverchunk *queueserverchunk(int x, int y, bool prioritize = false)
    {
        if(serverchunk *chunk = findserverchunk(x, y))
        {
            if(prioritize && chunk->loading) prioritizeserverchunkload(x, y);
            return chunk;
        }
        serverchunk *chunk = new serverchunk(x, y);
        serverchunks.add(chunk);
        if(!startserverchunkworker())
        {
            chunk->loading = false;
            chunk->corrupted = true;
            return chunk;
        }
        SDL_LockMutex(serverchunkmutex);
        serverchunkjob *job = new serverchunkjob(SERVER_CHUNK_LOAD, x, y);
        if(prioritize) serverchunkjobs.insert(0, job);
        else serverchunkjobs.add(job);
        SDL_CondSignal(serverchunkcond);
        SDL_UnlockMutex(serverchunkmutex);
        return chunk;
    }

    static void serverchunkdataputuint(vector<uchar> &data, uint value) { loopi(4) data.add(uchar(value >> (8 * i))); }

    static bool captureserverchunknpcs(int chunkx, int chunky, vector<uchar> &data)
    {
        data.setsize(0);
        serverchunkdataputuint(data, 1);
        int count = 0;
        loopv(serverdeadpassivenpcs)
            if(serverdeadpassivenpcs[i].chunkx == chunkx && serverdeadpassivenpcs[i].chunky == chunky) ++count;
        serverchunkdataputuint(data, uint(count));
        loopv(serverdeadpassivenpcs) if(serverdeadpassivenpcs[i].chunkx == chunkx && serverdeadpassivenpcs[i].chunky == chunky)
        {
            serverchunkdataputuint(data, uint(serverdeadpassivenpcs[i].key));
            serverchunkdataputuint(data, uint(serverdeadpassivenpcs[i].key >> 32));
        }
        return true;
    }

    static bool restoreserverchunknpcs(int chunkx, int chunky, const vector<uchar> &data)
    {
        if(data.length() < 8) return false;
        const uchar *position = data.getbuf(), *end = position + data.length();
        #define READ_CHUNK_UINT(destination) \
            do \
            { \
                if(end - position < 4) return false; \
                destination = uint(position[0]) | uint(position[1]) << 8 | uint(position[2]) << 16 | uint(position[3]) << 24; \
                position += 4; \
            } while(0)
        uint version, count;
        READ_CHUNK_UINT(version);
        READ_CHUNK_UINT(count);
        if(version != 1 || count > 100000U || end - position != int(count * 8U)) return false;
        loopi(count)
        {
            uint low, high;
            READ_CHUNK_UINT(low);
            READ_CHUNK_UINT(high);
            const ullong key = ullong(low) | ullong(high) << 32;
            if(!key) return false;
            bool duplicate = false;
            loopv(serverdeadpassivenpcs) if(serverdeadpassivenpcs[i].key == key) { duplicate = true; break; }
            if(!duplicate) serverdeadpassivenpcs.add(serverdeadpassivenpc(key, chunkx, chunky));
        }
        #undef READ_CHUNK_UINT
        return position == end;
    }

    static bool restoreserverchunkgameplay(serverchunkjob &job)
    {
        vector<furnaceinstance *> furnaces;
        vector<chestinstance *> chests;
        vector<uchar> npcdata;
        vector<chunkfallingblockstate> falling;
        vector<chunkdropstate> drops;
        if(!game::decodechunkdata(job.x, job.y, job.gameplay.getbuf(), job.gameplay.length(), furnaces, chests, npcdata, falling, drops) ||
           !restoreserverchunknpcs(job.x, job.y, npcdata))
        {
            furnaces.deletecontents();
            chests.deletecontents();
            return false;
        }
        loopv(furnaces) serverfurnaces.add(furnaces[i]);
        furnaces.setsize(0);
        loopv(chests) serverchests.add(chests[i]);
        chests.setsize(0);
        loopv(falling)
        {
            const chunkfallingblockstate &saved = falling[i];
            serverfallingblock *block = new serverfallingblock;
            if(!nextfallblockid || nextfallblockid > uint(INT_MAX)) nextfallblockid = 1;
            block->id = nextfallblockid++;
            block->item = saved.item;
            block->origin = saved.origin;
            block->o = saved.position;
            block->velocity = saved.velocity;
            block->lastupdate = totalmillis;
            serverfallingblocks.add(block);
            broadcastfallblockspawn(*block);
        }
        loopv(drops)
        {
            const chunkdropstate &saved = drops[i];
            if(saved.age >= droptimeout * 1000)
            {
                dirtyservergameplaycell(ivec(int(floorf(saved.position.x)), int(floorf(saved.position.y)), int(floorf(saved.position.z))));
                continue;
            }
            serverdrop *drop = new serverdrop;
            if(!nextdropid || nextdropid > uint(INT_MAX)) nextdropid = 1;
            drop->id = nextdropid++;
            drop->item = saved.item;
            drop->count = saved.count;
            drop->durability = saved.durability;
            drop->created = totalmillis - saved.age;
            drop->o = saved.position;
            copystring(drop->ownerid, saved.ownerid);
            serverdrops.add(drop);
            broadcastdropspawn(*drop);
        }
        return true;
    }

    static bool serverchunkcontains(const vec &position, int chunkx, int chunky)
    {
        return serverfloordiv(int(floorf(position.x)), SERVER_WORLD_CHUNK_SIZE) == chunkx &&
               serverfloordiv(int(floorf(position.y)), SERVER_WORLD_CHUNK_SIZE) == chunky;
    }

    static bool queueserverchunksave(serverchunk &chunk)
    {
        if(chunk.loading || chunk.saving || chunk.corrupted || !chunk.root || !startserverchunkworker()) return false;
        vector<uchar> npcdata;
        vector<chunkfallingblockstate> falling;
        vector<chunkdropstate> drops;
        loopv(serverfallingblocks)
        {
            const serverfallingblock &source = *serverfallingblocks[i];
            if(source.item < 0 || !serverchunkcontains(source.o, chunk.x, chunk.y)) continue;
            chunkfallingblockstate &block = falling.add();
            block.item = source.item;
            block.origin = source.origin;
            block.position = source.o;
            block.velocity = source.velocity;
        }
        loopv(serverdrops)
        {
            const serverdrop &source = *serverdrops[i];
            if(source.item < 0 || !serverchunkcontains(source.o, chunk.x, chunk.y)) continue;
            chunkdropstate &drop = drops.add();
            drop.item = source.item;
            drop.count = source.count;
            drop.durability = source.durability;
            drop.age = min(max(totalmillis - source.created, 0), 86400000);
            drop.position = source.o;
            copystring(drop.ownerid, source.ownerid);
        }
        if(!captureserverchunknpcs(chunk.x, chunk.y, npcdata) ||
           !game::capturechunkdata(chunk.x, chunk.y, serverfurnaces, serverchests, npcdata, falling, drops, chunk.gameplay)) return false;
        serverchunkjob *job = new serverchunkjob(SERVER_CHUNK_SAVE, chunk.x, chunk.y);
        job->revision = chunk.revision;
        job->storageversion = chunk.storageversion;
        job->playeredited = chunk.playeredited;
        job->root = allocworldsnapshotfamily();
        loopi(8) copyworldsnapshotcube(chunk.root[i], job->root[i]);
        job->renderdata = chunk.renderdata;
        if(!chunk.scatter.empty()) job->scatter.put(chunk.scatter.getbuf(), chunk.scatter.length());
        if(!chunk.gameplay.empty()) job->gameplay.put(chunk.gameplay.getbuf(), chunk.gameplay.length());
        chunk.saving = true;
        SDL_LockMutex(serverchunkmutex);
        serverchunkjobs.add(job);
        SDL_CondSignal(serverchunkcond);
        SDL_UnlockMutex(serverchunkmutex);
        return true;
    }

    static void processserverchunkresults()
    {
        for(;;)
        {
            SDL_LockMutex(serverchunkmutex);
            serverchunkjob *job = serverchunkresults.empty() ? NULL : serverchunkresults.remove(0);
            SDL_UnlockMutex(serverchunkmutex);
            if(!job) break;
            serverchunk *chunk = findserverchunk(job->x, job->y);
            if(!chunk) { delete job; continue; }
            if(job->type == SERVER_CHUNK_LOAD)
            {
                chunk->loading = false;
                chunk->corrupted = !job->success;
                if(job->success && job->gameplay.empty())
                {
                    vector<furnaceinstance *> furnaces;
                    vector<chestinstance *> chests;
                    vector<uchar> npcdata;
                    vector<chunkfallingblockstate> falling;
                    vector<chunkdropstate> drops;
                    serverchunkdataputuint(npcdata, 1);
                    serverchunkdataputuint(npcdata, 0);
                    job->success = game::capturechunkdata(job->x, job->y, furnaces, chests, npcdata, falling, drops, job->gameplay);
                    chunk->corrupted = !job->success;
                    if(!job->success) copystring(job->error, "could not initialize chunk gameplay data");
                }
                if(job->success && !restoreserverchunkgameplay(*job))
                {
                    job->success = false;
                    chunk->corrupted = true;
                    copystring(job->error, "chunk gameplay data is corrupt or incompatible");
                }
                if(job->success)
                {
                    chunk->root = job->root;
                    job->root = NULL;
                    chunk->revision = job->revision;
                    chunk->playeredited = job->playeredited;
                    chunk->renderdata = job->renderdata;
                    chunk->scatter.move(job->scatter);
                    chunk->gameplay.move(job->gameplay);
                    chunk->vox.move(job->vox);
                    chunk->dat.move(job->dat);
                    chunk->serializedrevision = chunk->vox.empty() ? 0 : chunk->revision;
                    if(job->missing) chunk->dirty = true;
                }
            }
            else
            {
                chunk->saving = false;
                if(job->success)
                {
                    chunk->vox.move(job->vox);
                    chunk->dat.move(job->dat);
                    chunk->serializedrevision = job->revision;
                    if(chunk->storageversion == job->storageversion) chunk->dirty = false;
                }
            }
            if(!job->success)
                conoutf(CON_ERROR, "authoritative chunk %d_%d failed: %s", job->x, job->y, job->error[0] ? job->error : "unknown error");
            delete job;
        }
    }

    static bool serverchunkinrange(const clientinfo &ci, int chunkx, int chunky)
    {
        const int focusx = ci.hasposition ? serverfloordiv(int(floorf(ci.o.x)), SERVER_WORLD_CHUNK_SIZE) : 0,
                  focusy = ci.hasposition ? serverfloordiv(int(floorf(ci.o.y)), SERVER_WORLD_CHUNK_SIZE) : 0;
        return max(abs(chunkx - focusx), abs(chunky - focusy)) <= maxchunkdist;
    }

    static uint serverchunkdeliveredrevision(int clientnum, int chunkx, int chunky)
    {
        loopv(serverchunkdeliveries)
        {
            const serverchunkdelivery &delivery = serverchunkdeliveries[i];
            if(delivery.clientnum == clientnum && delivery.x == chunkx && delivery.y == chunky) return delivery.revision;
        }
        return 0;
    }

    static void markserverchunkdelivered(int clientnum, int chunkx, int chunky, uint revision)
    {
        loopv(serverchunkdeliveries)
        {
            serverchunkdelivery &delivery = serverchunkdeliveries[i];
            if(delivery.clientnum != clientnum || delivery.x != chunkx || delivery.y != chunky) continue;
            delivery.revision = revision;
            return;
        }
        serverchunkdeliveries.add(serverchunkdelivery(clientnum, chunkx, chunky, revision));
    }

    static void sendserverchunk(clientinfo &ci, serverchunk &chunk)
    {
        if(!chunk.playeredited || chunk.serializedrevision != chunk.revision || chunk.vox.empty() || chunk.dat.empty()) return;
        packetbuf packet(MAXTRANS + chunk.vox.length() + chunk.dat.length(), ENET_PACKET_FLAG_RELIABLE);
        putint(packet, N_CHUNKDATA);
        putint(packet, chunk.x); putint(packet, chunk.y); putint(packet, int(chunk.revision));
        putint(packet, chunk.vox.length()); putint(packet, chunk.dat.length());
        packet.put(chunk.vox.getbuf(), chunk.vox.length());
        packet.put(chunk.dat.getbuf(), chunk.dat.length());
        sendpacket(ci.clientnum, 2, packet.finalize());
        markserverchunkdelivered(ci.clientnum, chunk.x, chunk.y, chunk.revision);
    }

    static void updateserverchunkrequests()
    {
        processserverchunkresults();
        loopv(clients)
        {
            const clientinfo *ci = clients[i];
            if(!ci || !ci->connected || !ci->worldready) continue;
            const int focusx = ci->hasposition ? serverfloordiv(int(floorf(ci->o.x)), SERVER_WORLD_CHUNK_SIZE) : 0,
                      focusy = ci->hasposition ? serverfloordiv(int(floorf(ci->o.y)), SERVER_WORLD_CHUNK_SIZE) : 0;
            for(int distance = 0; distance <= maxchunkdist; ++distance)
                for(int y = focusy - distance; y <= focusy + distance; ++y)
                    for(int x = focusx - distance; x <= focusx + distance; ++x)
                        if(max(abs(x - focusx), abs(y - focusy)) == distance) queueserverchunk(x, y);
        }
        for(int i = serverchunkrequests.length() - 1; i >= 0; --i)
        {
            serverchunkrequest request = serverchunkrequests[i];
            clientinfo *ci = clients.inrange(request.clientnum) ? clients[request.clientnum] : NULL;
            if(!ci || !ci->connected || !ci->worldready || !serverchunkinrange(*ci, request.x, request.y))
            {
                serverchunkrequests.remove(i);
                continue;
            }
            serverchunk *chunk = queueserverchunk(request.x, request.y, true);
            if(chunk->loading || chunk->saving) continue;
            if(chunk->corrupted) { serverchunkrequests.remove(i); continue; }
            if(chunk->dirty)
            {
                if(totalmillis >= chunk->saveafter) queueserverchunksave(*chunk);
                continue;
            }
            serverchunkrequests.remove(i);
            sendserverchunk(*ci, *chunk);
        }
        loopv(serverchunks) if(serverchunks[i]->dirty && !serverchunks[i]->saving && totalmillis >= serverchunks[i]->saveafter)
            queueserverchunksave(*serverchunks[i]);
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(!ci || !ci->connected || !ci->worldready) continue;
            loopvj(serverchunks)
            {
                serverchunk &chunk = *serverchunks[j];
                if(!chunk.playeredited || chunk.loading || chunk.saving || chunk.dirty || chunk.corrupted ||
                   !serverchunkinrange(*ci, chunk.x, chunk.y) || serverchunkdeliveredrevision(ci->clientnum, chunk.x, chunk.y) >= chunk.revision)
                    continue;
                sendserverchunk(*ci, chunk);
            }
        }
    }

    static void shutdownserverchunks(bool flush)
    {
        if(flush && serverchunkthread)
        {
            for(;;)
            {
                processserverchunkresults();
                loopv(serverchunks) if(serverchunks[i]->dirty && !serverchunks[i]->loading && !serverchunks[i]->saving)
                    queueserverchunksave(*serverchunks[i]);
                SDL_LockMutex(serverchunkmutex);
                const bool queued = !serverchunkjobs.empty() || !serverchunkresults.empty();
                SDL_UnlockMutex(serverchunkmutex);
                bool active = false;
                loopv(serverchunks) if(serverchunks[i]->loading || serverchunks[i]->saving || serverchunks[i]->dirty) { active = true; break; }
                if(!queued && !active) break;
                SDL_Delay(1);
            }
        }
        if(serverchunkthread)
        {
            SDL_LockMutex(serverchunkmutex);
            stopserverchunkthread = true;
            SDL_CondBroadcast(serverchunkcond);
            SDL_UnlockMutex(serverchunkmutex);
            SDL_WaitThread(serverchunkthread, NULL);
        }
        serverchunkthread = NULL;
        loopv(serverchunkjobs) delete serverchunkjobs[i];
        serverchunkjobs.setsize(0);
        loopv(serverchunkresults) delete serverchunkresults[i];
        serverchunkresults.setsize(0);
        if(serverchunkcond) SDL_DestroyCond(serverchunkcond);
        if(serverchunkmutex) SDL_DestroyMutex(serverchunkmutex);
        serverchunkcond = NULL;
        serverchunkmutex = NULL;
        stopserverchunkthread = false;
        serverchunkrequests.setsize(0);
        serverchunkdeliveries.setsize(0);
        serverchunks.deletecontents();
    }

    clientinfo *getinfo(int n)
    {
        return clients.inrange(n) ? clients[n] : NULL;
    }

    static void clearinventory(clientinfo &ci)
    {
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            ci.inventoryitems[i] = -1;
            ci.inventorycounts[i] = 0;
            ci.inventorydurabilities[i] = 0;
        }
        ci.selectedslot = 0;
        ci.inventorycursoritem = -1;
        ci.inventorycursorcount = 0;
        ci.inventorycursordurability = 0;
        loopi(CRAFT_GRID_MAX)
        {
            ci.craftingitems[i] = -1;
            ci.craftingcounts[i] = 0;
            ci.craftingdurabilities[i] = 0;
        }
        ci.craftinggridsize = 2;
        ci.craftingstationitem = -1;
        ci.craftingstationtarget = ivec(0, 0, 0);
        ci.inventorydirty = false;
    }

    static void playerstatename(char *name, size_t len, const char *playerid, const char *suffix = "")
    {
        string safe;
        int n = 0;
        for(const char *s = serverworld; *s && n < 64; ++s)
            if(iscubealnum(*s) || *s == '_' || *s == '-') safe[n++] = *s;
        safe[n] = '\0';
        if(!safe[0]) copystring(safe, "multiplayer");
        snprintf(name, len, "config/server-player-states/%s_%08x_%u/%s.dat%s", safe, hthash(serverworld), uint(serverworldseed), playerid,
                 suffix ? suffix : "");
        path(name);
    }

    static bool writeserveritemstate(stream &file, int item, int count, int durability)
    {
        const ullong persistent = getinventoryitempersistentid(item);
        return file.putlil<uint>(uint(persistent)) && file.putlil<uint>(uint(persistent >> 32)) &&
               file.putlil<int>(count) && file.putlil<int>(durability);
    }

    static bool readserveritemstate(stream &file, int &item, int &count, int &durability)
    {
        const ullong persistent = ullong(file.getlil<uint>()) | ullong(file.getlil<uint>()) << 32;
        count = file.getlil<int>();
        durability = file.getlil<int>();
        item = persistent ? getinventoryitempersistentindex(persistent, false) : -1;
        if(!persistent) return item == -1 && count == 0 && durability == 0;
        if(item < 0 || item >= numinventoryitems() || getinventoryitempersistentid(item) != persistent || count <= 0 ||
           count > max(getinventoryitemmaxstack(item), 1)) return false;
        return isinventorytool(item) ? count == 1 && durability > 0 && durability <= getinventorytoolmaxdurability(item) : durability == 0;
    }

    static bool saveplayerstate(clientinfo &ci, bool force = false)
    {
        if(!ci.playerid[0] || (!force && !ci.positiondirty && !ci.inventorydirty)) return true;
        string relative, temporary, finalpath, temppath;
        playerstatename(relative, sizeof(relative), ci.playerid);
        playerstatename(temporary, sizeof(temporary), ci.playerid, ".tmp");
        copystring(finalpath, findfile(relative, "wb"));
        copystring(temppath, findfile(temporary, "wb"));
        stream *file = openrawfile(temporary, "wb");
        if(!file)
        {
            ci.lastpositionsave = ci.lastinventorysave = max(totalmillis, 1);
            return false;
        }
        bool ok = file->write("CCPS", 4) == 4 &&
                  file->putlil<uint>(PLAYER_STATE_VERSION) &&
                  writeserveridentitystring(*file, serverworld) &&
                  file->putlil<uint>(uint(serverworldseed)) &&
                  file->putlil<int>(ci.hasposition ? 1 : 0) &&
                  file->putlil<int>(ci.positioncoords.x) &&
                  file->putlil<int>(ci.positioncoords.y) &&
                  file->putlil<int>(ci.positioncoords.z) &&
                  file->putlil<int>(ci.positionyaw) &&
                  file->putlil<int>(ci.positionpitch) &&
                  file->putlil<int>(int(ci.health * 1000.0f)) &&
                  file->putlil<int>(ci.dead ? 1 : 0) &&
                  file->putlil<int>(int(ci.velocity.x * DNF)) &&
                  file->putlil<int>(int(ci.velocity.y * DNF)) &&
                  file->putlil<int>(int(ci.velocity.z * DNF)) &&
                  file->putlil<int>(int(ci.falling.x * DNF)) &&
                  file->putlil<int>(int(ci.falling.y * DNF)) &&
                  file->putlil<int>(int(ci.falling.z * DNF)) &&
                  file->putlil<int>(int(ci.falldistance * DMF)) &&
                  file->putlil<int>(ci.positionphysstate) &&
                  file->putlil<int>(ci.inventoryloaded ? 1 : 0) &&
                  file->putlil<int>(ci.selectedslot) &&
                  writeserveritemstate(*file, ci.inventorycursoritem, ci.inventorycursorcount, ci.inventorycursordurability);
        loopi(SURVIVAL_USABLE_SLOTS) if(ok)
            ok = writeserveritemstate(*file, ci.inventoryitems[i], ci.inventorycounts[i], ci.inventorydurabilities[i]);
        const ullong station = getinventoryitempersistentid(ci.craftingstationitem);
        if(ok) ok = file->putlil<int>(ci.craftinggridsize) &&
                    file->putlil<uint>(uint(station)) && file->putlil<uint>(uint(station >> 32)) &&
                    file->putlil<int>(ci.craftingstationtarget.x) && file->putlil<int>(ci.craftingstationtarget.y) &&
                    file->putlil<int>(ci.craftingstationtarget.z);
        loopi(CRAFT_GRID_MAX) if(ok)
            ok = writeserveritemstate(*file, ci.craftingitems[i], ci.craftingcounts[i], ci.craftingdurabilities[i]);
        delete file;
        if(!ok || !replaceserveridentityfile(temppath, finalpath))
        {
            remove(temppath);
            ci.lastpositionsave = ci.lastinventorysave = max(totalmillis, 1);
            return false;
        }
        ci.positiondirty = ci.inventorydirty = false;
        ci.lastpositionsave = ci.lastinventorysave = max(totalmillis, 1);
        return true;
    }

    static bool loadplayerstate(clientinfo &ci)
    {
        ci.hasposition = ci.positiondirty = false;
        ci.lastpositionsave = ci.lastinventorysave = max(totalmillis, 1);
        ci.positioncoords = ivec(0, 0, 0);
        ci.positionyaw = ci.positionpitch = 0;
        ci.positionphysstate = PHYS_FALL;
        ci.velocity = ci.falling = vec(0, 0, 0);
        ci.falldistance = 0;
        ci.health = game::PLAYER_MAX_HEALTH;
        ci.dead = false;
        clearinventory(ci);
        ci.inventoryloaded = true;

        string relative;
        playerstatename(relative, sizeof(relative), ci.playerid);
        stream *file = openrawfile(relative, "rb");
        if(!file) return true;

        char magic[4], world[MAXSTRLEN] = "";
        uint version = 0, seed = 0;
        int hasposition = 0, inventoryloaded = 0, yaw = 0, pitch = 0;
        ivec position;
        bool valid = file->read(magic, 4) == 4 && !memcmp(magic, "CCPS", 4) &&
                     (version = file->getlil<uint>()) == PLAYER_STATE_VERSION &&
                     readserveridentitystring(*file, world, sizeof(world)) &&
                     (seed = file->getlil<uint>()) == uint(serverworldseed) && !strcmp(world, serverworld);
        if(valid)
        {
            hasposition = file->getlil<int>();
            position.x = file->getlil<int>(); position.y = file->getlil<int>(); position.z = file->getlil<int>();
            yaw = file->getlil<int>(); pitch = file->getlil<int>();
            ci.health = file->getlil<int>() / 1000.0f;
            ci.dead = file->getlil<int>() != 0;
            loopk(3) ci.velocity[k] = file->getlil<int>() / DNF;
            loopk(3) ci.falling[k] = file->getlil<int>() / DNF;
            ci.falldistance = file->getlil<int>() / DMF;
            ci.positionphysstate = file->getlil<int>();
            inventoryloaded = file->getlil<int>();
            ci.selectedslot = file->getlil<int>();
            valid = readserveritemstate(*file, ci.inventorycursoritem, ci.inventorycursorcount, ci.inventorycursordurability);
            loopi(SURVIVAL_USABLE_SLOTS) if(valid)
                valid = readserveritemstate(*file, ci.inventoryitems[i], ci.inventorycounts[i], ci.inventorydurabilities[i]);
            ci.craftinggridsize = file->getlil<int>();
            const ullong station = ullong(file->getlil<uint>()) | ullong(file->getlil<uint>()) << 32;
            ci.craftingstationitem = station ? getinventoryitempersistentindex(station, false) : -1;
            ci.craftingstationtarget.x = file->getlil<int>();
            ci.craftingstationtarget.y = file->getlil<int>();
            ci.craftingstationtarget.z = file->getlil<int>();
            loopi(CRAFT_GRID_MAX) if(valid)
                valid = readserveritemstate(*file, ci.craftingitems[i], ci.craftingcounts[i], ci.craftingdurabilities[i]);
            valid = valid && (hasposition == 0 || hasposition == 1) && inventoryloaded == 1 &&
                    ci.selectedslot >= 0 && ci.selectedslot < SURVIVAL_HOTBAR_SLOTS &&
                    (ci.craftinggridsize == 2 || ci.craftinggridsize == 3) &&
                    (!station || (ci.craftingstationitem >= 0 && getinventoryitempersistentid(ci.craftingstationitem) == station)) &&
                    (!hasposition || (position.z >= 0 && position.z <= int((1 << 13) * DMF) &&
                                      yaw >= 0 && yaw < 360 && pitch >= -90 && pitch <= 90)) &&
                    ci.health >= 0 && ci.health <= game::PLAYER_MAX_HEALTH && ci.dead == (ci.health <= 0) &&
                    ci.falldistance >= 0 && ci.falldistance <= (1 << 13) &&
                    ci.positionphysstate >= PHYS_FLOAT && ci.positionphysstate <= PHYS_BOUNCE && file->tell() == file->size();
        }
        delete file;
        if(!valid)
        {
            conoutf(CON_WARN, "ignoring corrupt or incompatible player state %s (version %u, seed %u)", relative, version, seed);
            clearinventory(ci);
            ci.inventoryloaded = false;
            return false;
        }

        if(hasposition)
        {
            ci.positioncoords = position;
            ci.o = vec(position.x / DMF, position.y / DMF, position.z / DMF);
            ci.positionyaw = yaw;
            ci.positionpitch = pitch;
            ci.hasposition = true;
            ci.lastpositionmillis = max(totalmillis, 1);
        }
        return true;
    }
    static bool serverpassivenpcdead(ullong key)
    {
        loopv(serverdeadpassivenpcs) if(serverdeadpassivenpcs[i].key == key) return true;
        return false;
    }

    static void killserverpassivenpc(ullong key, const vec &position)
    {
        if(!key || serverpassivenpcdead(key)) return;
        const int chunkx = serverfloordiv(int(floorf(position.x)), SERVER_WORLD_CHUNK_SIZE),
                  chunky = serverfloordiv(int(floorf(position.y)), SERVER_WORLD_CHUNK_SIZE);
        serverdeadpassivenpcs.add(serverdeadpassivenpc(key, chunkx, chunky));
        serverchunk *chunk = queueserverchunk(chunkx, chunky);
        if(chunk && chunk->loading) chunk->dirty = true;
        else if(chunk && !chunk->corrupted) dirtyservergameplaycell(ivec(chunkx * SERVER_WORLD_CHUNK_SIZE, chunky * SERVER_WORLD_CHUNK_SIZE, 0));
    }
    static void sendinventory(clientinfo &ci)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_INVENTORYSTATE);
        putint(p, SURVIVAL_USABLE_SLOTS);
        putint(p, ci.selectedslot);
        putpersistentid(p, getinventoryitempersistentid(ci.inventorycursoritem));
        putint(p, ci.inventorycursorcount);
        putint(p, ci.inventorycursordurability);
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            putpersistentid(p, getinventoryitempersistentid(ci.inventoryitems[i]));
            putint(p, ci.inventorycounts[i]);
            putint(p, ci.inventorydurabilities[i]);
        }
        sendpacket(ci.clientnum, 1, p.finalize());
    }

    static bool craftingstationvalid(const clientinfo &ci)
    {
        if(ci.craftinggridsize == 2) return ci.craftingstationitem < 0;
        if(ci.craftinggridsize != 3 || !ci.hasposition || ci.craftingstationitem < 0) return false;
        serverworldaction *state = findworldaction(ci.craftingstationtarget, WORLD_ACTION_PLACE_CUBE);
        if(!state || state->action != WORLD_ACTION_PLACE_CUBE || state->item != ci.craftingstationitem) return false;
        return vec(ci.craftingstationtarget).add(8).dist(ci.o) <= 144.0f;
    }

    static bool servercraftmatch(const clientinfo &ci, int requestedrecipe, craftmatch &match, int maxoutput = INT_MAX)
    {
        // Skill progression is intentionally separate from recipe matching. Until
        // that subsystem exists, players have no named skill and level zero.
        return craftingstationvalid(ci) && matchcraftrecipe(ci.craftingitems, ci.craftingcounts, ci.craftinggridsize,
                                                           ci.craftingstationitem, -1, 0, requestedrecipe, match, maxoutput);
    }

    static void sendcraftstate(clientinfo &ci)
    {
        craftmatch match;
        servercraftmatch(ci, -1, match);
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_CRAFTSTATE);
        putint(p, CRAFT_GRID_MAX);
        putint(p, ci.craftinggridsize);
        putpersistentid(p, getinventoryitempersistentid(ci.craftingstationitem));
        putint(p, match.recipe);
        putpersistentid(p, getinventoryitempersistentid(match.outputitem));
        putint(p, match.outputcount);
        loopi(CRAFT_GRID_MAX)
        {
            putpersistentid(p, getinventoryitempersistentid(ci.craftingitems[i]));
            putint(p, ci.craftingcounts[i]);
            putint(p, ci.craftingdurabilities[i]);
        }
        sendpacket(ci.clientnum, 1, p.finalize());
    }

    static furnaceinstance *findserverfurnace(const ivec &target)
    {
        loopv(serverfurnaces) if(serverfurnaces[i]->target == target) return serverfurnaces[i];
        return NULL;
    }

    static bool furnaceblockvalid(const furnaceinstance &furnace)
    {
        serverworldaction *state = findworldaction(furnace.target, WORLD_ACTION_PLACE_CUBE);
        int inputslots = 0, inputlimit = 0;
        return state && state->action == WORLD_ACTION_PLACE_CUBE && state->item == furnace.worlditem &&
               getworldfurnaceconfig(state->item, inputslots, inputlimit) && inputslots == furnace.inputslots && inputlimit == furnace.inputlimit;
    }

    static bool furnaceaccessible(const clientinfo &ci, const furnaceinstance &furnace)
    {
        return ci.hasposition && furnaceblockvalid(furnace) && vec(furnace.target).add(8).dist(ci.o) <= 144.0f;
    }

    static bool furnaceiscooking(const furnaceinstance &furnace)
    {
        furnacematch match;
        if(!furnace.baking || furnace.heat <= 0 || furnace.activerecipe < 0 ||
           !matchfurnacerecipe(furnace.inputitems, furnace.inputcounts, furnace.inputslots, furnace.activerecipe, match)) return false;
        if(furnace.outputcount > 0 && furnace.outputitem != match.outputitem) return false;
        return furnace.outputcount + match.outputcount <= max(getinventoryitemmaxstack(match.outputitem), 1);
    }

    static void sendfurnacestate(clientinfo &ci, const furnaceinstance &furnace, bool open)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_FURNACESTATE);
        putint(p, open ? 1 : 0);
        putint(p, furnace.target.x); putint(p, furnace.target.y); putint(p, furnace.target.z);
        putpersistentid(p, getinventoryitempersistentid(furnace.worlditem)); putint(p, furnace.inputslots); putint(p, furnace.inputlimit);
        putint(p, furnace.activerecipe); putint(p, furnace.progress); putint(p, furnace.heat); putint(p, furnace.heatcapacity);
        putint(p, furnace.baking ? 1 : 0);
        putint(p, furnaceiscooking(furnace) ? 1 : 0);
        loopi(FURNACE_INPUT_MAX)
        {
            putpersistentid(p, getinventoryitempersistentid(furnace.inputitems[i]));
            putint(p, furnace.inputcounts[i]);
            putint(p, furnace.inputdurabilities[i]);
        }
        putpersistentid(p, getinventoryitempersistentid(furnace.fuelitem)); putint(p, furnace.fuelcount); putint(p, furnace.fueldurability);
        putpersistentid(p, getinventoryitempersistentid(furnace.outputitem)); putint(p, furnace.outputcount); putint(p, furnace.outputdurability);
        sendpacket(ci.clientnum, 1, p.finalize());
    }

    static void closefurnace(clientinfo &ci)
    {
        ci.furnaceopen = false;
        sendfurnacestate(ci, furnaceinstance(), false);
    }

    static void syncfurnaceviewers(const furnaceinstance &furnace)
    {
        loopv(clients)
        {
            clientinfo *viewer = clients[i];
            if(viewer && viewer->connected && viewer->furnaceopen && viewer->furnacetarget == furnace.target)
                sendfurnacestate(*viewer, furnace, true);
        }
    }

    static chestinstance *findserverchest(const ivec &target)
    {
        loopv(serverchests) if(serverchests[i]->target == target) return serverchests[i];
        return NULL;
    }

    static bool chestblockvalid(const chestinstance &chest)
    {
        serverworldaction *state = findworldaction(chest.target, WORLD_ACTION_PLACE_ITEM);
        int slots = 0;
        return state && state->action == WORLD_ACTION_PLACE_ITEM && state->item == chest.worlditem &&
               getworldchestconfig(state->item, slots) && slots == chest.slots;
    }

    static bool chestaccessible(const clientinfo &ci, const chestinstance &chest)
    {
        return ci.hasposition && chestblockvalid(chest) && vec(chest.target).add(8).dist(ci.o) <= 144.0f;
    }

    static bool chesthasviewers(const ivec &target)
    {
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->chestopen && clients[i]->chesttarget == target) return true;
        return false;
    }

    static void sendchestanimation(const ivec &target, bool open)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_CHESTANIM);
        putint(p, target.x); putint(p, target.y); putint(p, target.z); putint(p, open ? 1 : 0);
        ENetPacket *packet = p.finalize();
        packet->referenceCount++;
        const float range = max(dynamicentsmaxdistance, 1) * 16.0f;
        loopv(clients)
        {
            clientinfo *recipient = clients[i];
            if(!recipient || !recipient->connected || !recipient->worldready || !recipient->hasposition) continue;
            if(vec(target).add(8).dist(recipient->o) <= range) sendpacket(recipient->clientnum, 1, packet);
        }
        if(--packet->referenceCount == 0) enet_packet_destroy(packet);
    }

    static void sendcheststate(clientinfo &ci, const chestinstance &chest, bool open)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_CHESTSTATE);
        putint(p, open ? 1 : 0);
        putint(p, chest.target.x); putint(p, chest.target.y); putint(p, chest.target.z);
        putpersistentid(p, getinventoryitempersistentid(chest.worlditem));
        putint(p, open ? chest.slots : 0);
        putint(p, chest.yaw);
        loopi(CHEST_SLOTS_MAX)
        {
            putpersistentid(p, getinventoryitempersistentid(open ? chest.items[i] : -1));
            putint(p, open ? chest.counts[i] : 0);
            putint(p, open ? chest.durabilities[i] : 0);
        }
        sendpacket(ci.clientnum, 1, p.finalize());
    }

    static void closechest(clientinfo &ci)
    {
        if(!ci.chestopen) return;
        const ivec target = ci.chesttarget;
        ci.chestopen = false;
        sendcheststate(ci, chestinstance(), false);
        if(!chesthasviewers(target)) sendchestanimation(target, false);
    }

    static void syncchestviewers(const chestinstance &chest)
    {
        loopv(clients)
        {
            clientinfo *viewer = clients[i];
            if(viewer && viewer->connected && viewer->chestopen && viewer->chesttarget == chest.target)
                sendcheststate(*viewer, chest, true);
        }
    }

    static bool serverlimitedinventoryclick(int &cursoritem, int &cursorcount, int &cursordurability, int &slotitem, int &slotcount,
                                            int &slotdurability, int button, int slotlimit)
    {
        if(button != INVENTORY_CLICK_LEFT && button != INVENTORY_CLICK_RIGHT) return false;
        const int oldcursoritem = cursoritem, oldcursordurability = cursordurability,
                  oldslotitem = slotitem, oldslotdurability = slotdurability;
        slotlimit = max(slotlimit, 1);
        if(button == INVENTORY_CLICK_LEFT)
        {
            if(cursorcount <= 0)
            {
                if(slotcount <= 0) return false;
                swap(cursoritem, slotitem); swap(cursorcount, slotcount);
            }
            else if(slotcount <= 0)
            {
                const int moved = min(cursorcount, slotlimit);
                slotitem = cursoritem; slotcount = moved; cursorcount -= moved;
            }
            else if(cursoritem != slotitem)
            {
                if(cursorcount > slotlimit) return false;
                swap(cursoritem, slotitem); swap(cursorcount, slotcount);
            }
            else
            {
                const int moved = min(cursorcount, slotlimit - slotcount);
                if(moved <= 0) return false;
                slotcount += moved; cursorcount -= moved;
            }
        }
        else if(cursorcount <= 0)
        {
            if(slotcount <= 0) return false;
            const int moved = (slotcount + 1) / 2;
            cursoritem = slotitem; cursorcount = moved; slotcount -= moved;
        }
        else
        {
            if(slotcount > 0 && (slotitem != cursoritem || slotcount >= slotlimit)) return false;
            if(slotcount <= 0) slotitem = cursoritem;
            ++slotcount; --cursorcount;
        }
        if(cursorcount <= 0) { cursoritem = -1; cursorcount = 0; }
        if(slotcount <= 0) { slotitem = -1; slotcount = 0; }
        if(cursoritem < 0) cursordurability = 0;
        else if(cursoritem == oldslotitem && oldcursoritem != cursoritem) cursordurability = oldslotdurability;
        else if(cursoritem != oldcursoritem) cursordurability = oldslotdurability;
        if(slotitem < 0) slotdurability = 0;
        else if(slotitem == oldcursoritem && oldslotitem != slotitem) slotdurability = oldcursordurability;
        else if(slotitem != oldslotitem) slotdurability = oldcursordurability;
        return true;
    }

    static bool servertakefurnaceoutput(clientinfo &ci, furnaceinstance &furnace, int button)
    {
        if(furnace.outputcount <= 0 || (button != INVENTORY_CLICK_LEFT && button != INVENTORY_CLICK_RIGHT)) return false;
        if(ci.inventorycursorcount > 0 && ci.inventorycursoritem != furnace.outputitem) return false;
        const int capacity = max(getinventoryitemmaxstack(furnace.outputitem), 1) - ci.inventorycursorcount,
                  moved = min(capacity, button == INVENTORY_CLICK_RIGHT ? 1 : furnace.outputcount);
        if(moved <= 0) return false;
        ci.inventorycursoritem = furnace.outputitem;
        ci.inventorycursorcount += moved;
        ci.inventorycursordurability = furnace.outputdurability;
        furnace.outputcount -= moved;
        if(furnace.outputcount <= 0)
        {
            furnace.outputitem = -1;
            furnace.outputcount = furnace.outputdurability = 0;
        }
        return true;
    }

    static void sendactionresult(clientinfo &ci, uint requestid, int result, const char *reason = "")
    {
        sendf(ci.clientnum, 1, "ri3s", N_ACTIONRESULT, int(requestid), result, reason ? reason : "");
    }

    static void cancelbreak(clientinfo &ci, bool broadcast = true);

    static bool kickviolation(clientinfo &ci, const char *reason)
    {
        string playerid;
        copystring(playerid, ci.playerid);
        int kicks = 0;
        bool banned = false;
        if(ci.identity)
        {
            kicks = ++ci.identity->kicks;
            if(kicks >= identitybankicks) ci.identity->banned = banned = true;
            if(!writeserveridentities()) conoutf(CON_ERROR, "could not persist kick/ban state for player ID %s", playerid);
        }
        defformatstring(message, "%s: %s", banned ? "identity automatically banned" : "kicked", reason ? reason : "illegal action");
        sendf(ci.clientnum, 1, "ris", N_SERVMSG, message);
        conoutf(CON_WARN, "%s player ID %s (client %d, kick %d/%d): %s",
                banned ? "banned" : "kicked", playerid[0] ? playerid : "(local)", ci.clientnum, kicks, identitybankicks,
                reason ? reason : "illegal action");
        cancelbreak(ci);
        saveplayerstate(ci, true);
        if(!ci.local) disconnect_client(ci.clientnum, DISC_KICK);
        return false;
    }

    static bool addviolation(clientinfo &ci, const char *reason, bool malicious)
    {
        const int now = max(totalmillis, 1);
        if(!ci.violationwindow || now - ci.violationwindow >= violationresetinterval * 1000)
        {
            ci.violationwindow = now;
            ci.violations = 0;
        }
        ++ci.violations;
        conoutf(CON_WARN, "illegal action by player ID %s (client %d, violation %d/%d): %s",
                ci.playerid, ci.clientnum, ci.violations, desynctolerance, reason ? reason : "validation failed");
        if(malicious || ci.violations > desynctolerance) return kickviolation(ci, reason);
        return true;
    }

    static bool rejectaction(clientinfo &ci, uint requestid, const char *reason, bool violation = false, bool malicious = false,
                             bool predictioniscorrect = false)
    {
        sendactionresult(ci, requestid, predictioniscorrect ? ACTION_RESULT_CORRECTED : ACTION_RESULT_REJECTED, reason);
        sendinventory(ci);
        return !violation || addviolation(ci, reason, malicious);
    }

    static void markinventorydirty(clientinfo &ci)
    {
        if(!servercreative()) ci.inventorydirty = true;
    }

    static bool addinventoryitem(clientinfo &ci, int item)
    {
        if(item < 0) return false;
        const int maxstack = max(getinventoryitemmaxstack(item), 1);
        loopi(SURVIVAL_USABLE_SLOTS) if(ci.inventoryitems[i] == item && ci.inventorycounts[i] > 0 && ci.inventorycounts[i] < maxstack)
        {
            ++ci.inventorycounts[i];
            markinventorydirty(ci);
            return true;
        }
        loopi(SURVIVAL_USABLE_SLOTS) if(ci.inventorycounts[i] <= 0)
        {
            ci.inventoryitems[i] = item;
            ci.inventorycounts[i] = 1;
            ci.inventorydurabilities[i] = getinventorytoolmaxdurability(item);
            markinventorydirty(ci);
            return true;
        }
        return false;
    }

    static bool inventoryhasroom(const clientinfo &ci, int item, int quantity)
    {
        int room = 0;
        const int stack = max(getinventoryitemmaxstack(item), 1);
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            if(ci.inventoryitems[i] == item && ci.inventorycounts[i] > 0) room += max(stack - ci.inventorycounts[i], 0);
            else if(ci.inventorycounts[i] <= 0) room += stack;
            if(room >= quantity) return true;
        }
        return false;
    }

    static bool addinventoryitems(clientinfo &ci, int item, int quantity, int durability = 0)
    {
        if(isinventorytool(item))
        {
            if(quantity != 1) return false;
            loopi(SURVIVAL_USABLE_SLOTS) if(ci.inventoryitems[i] < 0 || ci.inventorycounts[i] <= 0)
            {
                ci.inventoryitems[i] = item;
                ci.inventorycounts[i] = 1;
                ci.inventorydurabilities[i] = clamp(durability > 0 ? durability : getinventorytoolmaxdurability(item),
                                                    1, getinventorytoolmaxdurability(item));
                markinventorydirty(ci);
                return true;
            }
            return false;
        }
        if(quantity <= 0 || !inventoryhasroom(ci, item, quantity)) return false;
        loopi(quantity) if(!addinventoryitem(ci, item)) return false;
        return true;
    }

    static clientinfo *dropowner(const serverdrop &drop)
    {
        if(!drop.ownerid[0]) return NULL;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci && ci->connected && !strcmp(ci->playerid, drop.ownerid)) return ci;
        }
        return NULL;
    }

    static clientinfo *randomdropowner()
    {
        int available = 0;
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready) ++available;
        if(!available) return NULL;
        int selected = rnd(available);
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready && selected-- == 0) return clients[i];
        return NULL;
    }

    static void senddropsettings(int cn = -1)
    {
        if(cn >= 0)
            sendf(cn, 1, "ri6", N_DROPSETTINGS, personaldrops, droptimeout, maxdrop, dynamicentsmaxdistance, requireconfirmeditems);
        else loopv(clients) if(clients[i] && clients[i]->connected)
            sendf(clients[i]->clientnum, 1, "ri6", N_DROPSETTINGS, personaldrops, droptimeout, maxdrop,
                  dynamicentsmaxdistance, requireconfirmeditems);
    }

    static void senddropspawn(int cn, const serverdrop &drop)
    {
        clientinfo *owner = dropowner(drop);
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_DROPSPAWN); putint(p, int(drop.id)); putint(p, drop.source); putint(p, int(drop.sourcerequestid));
        putpersistentid(p, getinventoryitempersistentid(drop.item));
        putint(p, drop.count); putint(p, drop.durability); putint(p, owner ? owner->clientnum : drop.ownerid[0] ? -2 : -1);
        putint(p, int(drop.o.x)); putint(p, int(drop.o.y)); putint(p, int(drop.o.z));
        sendpacket(cn, 1, p.finalize());
    }

    static void broadcastdropspawn(const serverdrop &drop)
    {
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready) senddropspawn(clients[i]->clientnum, drop);
    }

    static void removeserverdrop(int index, int picker = -1)
    {
        if(!serverdrops.inrange(index)) return;
        serverdrop *drop = serverdrops.remove(index);
        dirtyservergameplaycell(ivec(int(floorf(drop->o.x)), int(floorf(drop->o.y)), int(floorf(drop->o.z))));
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready)
            sendf(clients[i]->clientnum, 1, "ri3", N_DROPDELETE, int(drop->id), picker);
        delete drop;
    }

    static vec serverdroporigin(const ivec &target, int action, int orient)
    {
        ivec cell = target;
        if(action == WORLD_ACTION_BREAK_SCATTER_START && orient >= 0 && orient <= 5)
            cell[orient >> 1] += orient&1 ? 16 : -16;
        return vec(cell.x + 8.0f, cell.y + 8.0f, cell.z + 3.0f);
    }

    static void addworlddrops(clientinfo *provoker, uint requestid, int action, const ivec &target, int orient, int objectitem)
    {
        clientinfo *owner = provoker ? provoker : randomdropowner();
        const int source = provoker ? provoker->clientnum : owner ? owner->clientnum : -1;
        const int type = getworlditemtype(objectitem), index = getworlditemindex(objectitem), definitions = getworldobjectdropcount(type, index);
        loopi(definitions)
        {
            int item, mincount, maxcount, quantity;
            float chance;
            if(!getworldobjectdrop(type, index, i, item, mincount, maxcount, chance) || !worlddroproll(source, requestid, objectitem, i, mincount, maxcount, chance, quantity))
                continue;
            while(serverdrops.length() >= maxdrop) removeserverdrop(0);
            serverdrop *drop = new serverdrop;
            if(!nextdropid || nextdropid > uint(INT_MAX)) nextdropid = 1;
            drop->id = nextdropid++;
            drop->sourcerequestid = requestid;
            drop->source = source;
            drop->item = item;
            drop->count = quantity;
            drop->created = max(totalmillis, 1);
            drop->o = serverdroporigin(target, action, orient);
            if(owner) copystring(drop->ownerid, owner->playerid);
            serverdrops.add(drop);
            dirtyservergameplaycell(target);
            broadcastdropspawn(*drop);
        }
    }

    static void addcontainercontentsdrop(clientinfo *owner, const ivec &target, int item, int count, int durability = 0)
    {
        if(item < 0 || count <= 0) return;
        if(!owner) owner = randomdropowner();
        while(serverdrops.length() >= maxdrop) removeserverdrop(0);
        serverdrop *drop = new serverdrop;
        if(!nextdropid || nextdropid > uint(INT_MAX)) nextdropid = 1;
        drop->id = nextdropid++;
        drop->source = owner ? owner->clientnum : -1;
        drop->item = item;
        drop->count = count;
        drop->durability = durability;
        drop->created = max(totalmillis, 1);
        drop->o = vec(target).add(8);
        if(owner) copystring(drop->ownerid, owner->playerid);
        serverdrops.add(drop);
        dirtyservergameplaycell(target);
        broadcastdropspawn(*drop);
    }

    static void removeserverfurnace(const ivec &target, clientinfo *owner = NULL)
    {
        loopv(serverfurnaces) if(serverfurnaces[i]->target == target)
        {
            furnaceinstance *furnace = serverfurnaces[i];
            loopj(FURNACE_INPUT_MAX)
                addcontainercontentsdrop(owner, target, furnace->inputitems[j], furnace->inputcounts[j], furnace->inputdurabilities[j]);
            addcontainercontentsdrop(owner, target, furnace->fuelitem, furnace->fuelcount, furnace->fueldurability);
            addcontainercontentsdrop(owner, target, furnace->outputitem, furnace->outputcount, furnace->outputdurability);
            loopvj(clients) if(clients[j] && clients[j]->furnaceopen && clients[j]->furnacetarget == target)
                closefurnace(*clients[j]);
            delete serverfurnaces.remove(i);
            dirtyservergameplaycell(target);
            return;
        }
    }

    static void removeserverchest(const ivec &target, clientinfo *owner = NULL)
    {
        loopv(serverchests) if(serverchests[i]->target == target)
        {
            chestinstance *chest = serverchests[i];
            loopj(chest->slots)
                addcontainercontentsdrop(owner, target, chest->items[j], chest->counts[j], chest->durabilities[j]);
            loopvj(clients) if(clients[j] && clients[j]->chestopen && clients[j]->chesttarget == target)
                closechest(*clients[j]);
            delete serverchests.remove(i);
            dirtyservergameplaycell(target);
            return;
        }
    }

    static serverworldaction *findworldaction(const ivec &target, int action)
    {
        const bool scatter = action == WORLD_ACTION_PLACE_SCATTER || action == WORLD_ACTION_PLACE_ITEM ||
                             action == WORLD_ACTION_BREAK_SCATTER_START;
        static serverworldaction state;
        state = serverworldaction();
        state.target = target;
        if(!scatter)
        {
            state.item = serverblockitem(target);
            state.action = state.item >= 0 ? WORLD_ACTION_PLACE_CUBE : WORLD_ACTION_BREAK_CUBE_START;
            return &state;
        }
        serverchunk *chunk = serverchunkforcell(target);
        if(chunk && !chunk->loading && !chunk->corrupted)
        {
            const int localx = target.x - chunk->x * SERVER_WORLD_CHUNK_SIZE,
                      localy = target.y - chunk->y * SERVER_WORLD_CHUNK_SIZE;
            loopv(chunk->scatter)
            {
                const worldscatterinstance &instance = chunk->scatter[i];
                if(instance.x != localx || instance.y != localy || instance.z != target.z) continue;
                state.orient = instance.orient;
                state.item = getworldscatteritem(instance.type);
                state.action = getworlditemtype(state.item) == WORLD_ITEM_PLACEABLE ? WORLD_ACTION_PLACE_ITEM : WORLD_ACTION_PLACE_SCATTER;
                return &state;
            }
        }
        state.action = WORLD_ACTION_BREAK_SCATTER_START;
        return &state;
    }

    static void queueserverfallblockcheck(const ivec &cell)
    {
        loopv(serverfallblockchecks) if(serverfallblockchecks[i] == cell) return;
        serverfallblockchecks.add(cell);
    }

    static int servermaxsupportdistance()
    {
        int distance = 0;
        loopi(numworldcubes()) distance = max(distance, getworldcubesupportdistance(i));
        return distance;
    }

    static void queueserversupportcheck(const ivec &cell, int remaining)
    {
        if(remaining < 0) return;
        loopv(serversupportchecks) if(serversupportchecks[i].cell == cell)
        {
            serversupportchecks[i].remaining = max(serversupportchecks[i].remaining, remaining);
            return;
        }
        serversupportchecks.add(serversupportcheck(cell, remaining));
    }

    static void queueserversupportchange(const ivec &cell)
    {
        const int distance = servermaxsupportdistance();
        if(distance <= 0) return;
        static const int directions[6][3] =
        {
            { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 },
            { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
        };
        queueserversupportcheck(cell, distance);
        loopi(6) queueserversupportcheck(ivec(cell).add(ivec(directions[i][0], directions[i][1], directions[i][2]).mul(SERVER_WORLD_BLOCK_SIZE)),
                                          distance);
    }

    static void setworldactionstate(const ivec &target, int action, int orient, int item, bool playerplaced)
    {
        if(action == WORLD_ACTION_PLACE_SCATTER || action == WORLD_ACTION_PLACE_ITEM || action == WORLD_ACTION_BREAK_SCATTER_START)
        {
            serverchunk *chunk = serverchunkforcell(target);
            if(chunk && !chunk->loading && !chunk->corrupted)
            {
                const int localx = target.x - chunk->x * SERVER_WORLD_CHUNK_SIZE,
                          localy = target.y - chunk->y * SERVER_WORLD_CHUNK_SIZE;
                for(int i = chunk->scatter.length() - 1; i >= 0; --i)
                {
                    const worldscatterinstance &instance = chunk->scatter[i];
                    if(instance.x == localx && instance.y == localy && instance.z == target.z) chunk->scatter.remove(i);
                }
                if(action == WORLD_ACTION_PLACE_SCATTER || action == WORLD_ACTION_PLACE_ITEM)
                {
                    const int type = getworlditemindex(item);
                    if(type >= 0) chunk->scatter.add(worldscatterinstance(localx, localy, target.z, type, orient));
                }
            }
        }
        (void)playerplaced;
        queueserverfallblockcheck(target);
        queueserverfallblockcheck(ivec(target).add(ivec(0, 0, SERVER_WORLD_BLOCK_SIZE)));
        queueserversupportchange(target);
    }

    static int serverfloordiv(int value, int divisor)
    {
        int quotient = value / divisor, remainder = value % divisor;
        if(remainder < 0) --quotient;
        return quotient;
    }

    static serverchunk *serverchunkforcell(const ivec &cell, bool queue)
    {
        const int chunkx = serverfloordiv(cell.x, SERVER_WORLD_CHUNK_SIZE), chunky = serverfloordiv(cell.y, SERVER_WORLD_CHUNK_SIZE);
        return queue ? queueserverchunk(chunkx, chunky) : findserverchunk(chunkx, chunky);
    }

    static cube *serverchunkcubeat(serverchunk &chunk, const ivec &cell, bool create)
    {
        if(!chunk.root || cell.z < 0 || cell.z >= SERVER_WORLD_MAP_SIZE) return NULL;
        const ivec position(cell.x - chunk.x * SERVER_WORLD_CHUNK_SIZE, cell.y - chunk.y * SERVER_WORLD_CHUNK_SIZE, cell.z);
        int scale = WORLD_CHUNK_SCALE - 1, size = 1 << scale;
        cube *current = &chunk.root[octastep(position.x, position.y, position.z, scale)];
        while(size > SERVER_WORLD_BLOCK_SIZE)
        {
            if(!current->children)
            {
                if(!create) return current;
                current->children = allocworldsnapshotfamily();
                loopi(8)
                {
                    cube &child = current->children[i];
                    cube *children = child.children;
                    child = *current;
                    child.children = children;
                    child.ext = NULL;
                    child.visible = child.merged = 0;
                }
            }
            --scale;
            size >>= 1;
            current = &current->children[octastep(position.x, position.y, position.z, scale)];
        }
        return current;
    }

    static const cube *serverchunkcubeat(const ivec &cell)
    {
        serverchunk *chunk = serverchunkforcell(cell);
        return chunk && !chunk->loading && !chunk->corrupted ? serverchunkcubeat(*chunk, cell, false) : NULL;
    }

    static void dirtyserverchunk(serverchunk &chunk)
    {
        ++chunk.revision;
        ++chunk.storageversion;
        chunk.playeredited = chunk.dirty = true;
        chunk.saveafter = totalmillis;
        chunk.serializedrevision = 0;
        chunk.vox.setsize(0);
        chunk.dat.setsize(0);
    }

    static void dirtyservergameplaycell(const ivec &cell)
    {
        serverchunk *chunk = serverchunkforcell(cell);
        if(!chunk || chunk->loading || chunk->corrupted) return;
        if(!chunk->dirty || chunk->saving) chunk->saveafter = totalmillis + 5000;
        ++chunk->storageversion;
        chunk->dirty = true;
        chunk->serializedrevision = 0;
        chunk->vox.setsize(0);
        chunk->dat.setsize(0);
    }

    static bool updateserverchunkblock(const ivec &cell, int action, int item)
    {
        serverchunk *chunk = serverchunkforcell(cell, true);
        if(!chunk || chunk->loading || chunk->corrupted) return false;
        cube *voxel = serverchunkcubeat(*chunk, cell, true);
        if(!voxel) return false;
        if(action == WORLD_ACTION_PLACE_CUBE)
        {
            const int worldindex = getworlditemtype(item) == WORLD_ITEM_CUBE ? getworlditemindex(item) : -1;
            if(worldindex < 0 || worldindex >= numworldcubes()) return false;
            freeworldsnapshotfamily(voxel->children);
            voxel->children = NULL;
            voxel->ext = NULL;
            voxel->material = MAT_AIR;
            voxel->visible = voxel->merged = 0;
            solidfaces(*voxel);
            loopi(6) voxel->texture[i] = ushort(worldindex);
        }
        else if(action == WORLD_ACTION_BREAK_CUBE_START)
        {
            freeworldsnapshotfamily(voxel->children);
            resetworldsnapshotcube(*voxel);
        }
        else return true;
        dirtyserverchunk(*chunk);
        return true;
    }

    static bool updateserverchunkcorner(const ivec &cell, int orient, int corner)
    {
        serverchunk *chunk = serverchunkforcell(cell, true);
        if(!chunk || chunk->loading || chunk->corrupted) return false;
        cube *voxel = serverchunkcubeat(*chunk, cell, true);
        if(!voxel || isempty(*voxel) || orient < O_LEFT || orient > O_TOP || corner < 0 || corner > 3) return false;
        const int axis = dimension(orient), endpoint = dimcoord(orient), direction = endpoint ? -1 : 1;
        const int previous = edgeget(cubeedge(*voxel, axis, corner & 1, corner >> 1), endpoint);
        if(previous + direction < 0 || previous + direction > 8) return false;
        pushworldcubecorneredge(*voxel, axis, corner & 1, corner >> 1, endpoint, direction);
        dirtyserverchunk(*chunk);
        return true;
    }

    static vec serverdirection(float yaw, float pitch)
    {
        const float pitchcos = cosf(RAD * pitch);
        return vec(-sinf(RAD * yaw) * pitchcos, cosf(RAD * yaw) * pitchcos, sinf(RAD * pitch));
    }

    static servercollisionchunk *getservercollisionchunk(int x, int y)
    {
        loopv(servercollisionchunks) if(servercollisionchunks[i]->x == x && servercollisionchunks[i]->y == y)
        {
            servercollisionchunks[i]->lastused = totalmillis;
            return servercollisionchunks[i];
        }
        if(!serverworldgenerator) serverworldgenerator = new game::worldgenerator(serverworldseed);
        servercollisionchunk *chunk = new servercollisionchunk(x, y);
        loop(row, SERVER_WORLD_CHUNK_BLOCKS) loop(column, SERVER_WORLD_CHUNK_BLOCKS)
            chunk->heights[row * SERVER_WORLD_CHUNK_BLOCKS + column] = short(serverworldgenerator->height(x * SERVER_WORLD_CHUNK_BLOCKS + column,
                                                                                                         y * SERVER_WORLD_CHUNK_BLOCKS + row));
        servercollisionchunks.add(chunk);
        return chunk;
    }

    static int serverbasesurface(int x, int y)
    {
        const int blockx = serverfloordiv(x, SERVER_WORLD_BLOCK_SIZE), blocky = serverfloordiv(y, SERVER_WORLD_BLOCK_SIZE),
                  chunkx = serverfloordiv(blockx, SERVER_WORLD_CHUNK_BLOCKS), chunky = serverfloordiv(blocky, SERVER_WORLD_CHUNK_BLOCKS),
                  localx = blockx - chunkx * SERVER_WORLD_CHUNK_BLOCKS, localy = blocky - chunky * SERVER_WORLD_CHUNK_BLOCKS;
        servercollisionchunk *chunk = getservercollisionchunk(chunkx, chunky);
        return SERVER_WORLD_GROUND_HEIGHT + chunk->heights[localy * SERVER_WORLD_CHUNK_BLOCKS + localx] * SERVER_WORLD_BLOCK_SIZE;
    }


    static int serverblockitem(const ivec &cell)
    {
        serverchunkforcell(cell, true);
        if(const cube *voxel = serverchunkcubeat(cell))
        {
            if(isempty(*voxel)) return -1;
            loopi(6) if(voxel->texture[i] != voxel->texture[0]) return -1;
            return getworldcubeitem(int(voxel->texture[0]));
        }
        return -1;
    }

    static int servercornerpushcount(const ivec &cell, int orient, int corner)
    {
        const cube *voxel = serverchunkcubeat(cell);
        if(!voxel || isempty(*voxel) || orient < O_LEFT || orient > O_TOP || corner < 0 || corner > 3) return 0;
        const int axis = dimension(orient), endpoint = dimcoord(orient), value = edgeget(cubeedge(*voxel, axis, corner & 1, corner >> 1), endpoint);
        return endpoint ? 8 - value : value;
    }

    static bool serverblocksolid(const ivec &cell)
    {
        if(cell.z < 0 || cell.z >= 8192) return cell.z < 0;
        if(const cube *voxel = serverchunkcubeat(cell)) return !isempty(*voxel);
        return cell.z < serverbasesurface(cell.x + SERVER_WORLD_BLOCK_SIZE / 2, cell.y + SERVER_WORLD_BLOCK_SIZE / 2);
    }

    static ivec serverblockat(const vec &position)
    {
        return ivec(serverfloordiv(int(floorf(position.x)), SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE,
                    serverfloordiv(int(floorf(position.y)), SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE,
                    serverfloordiv(int(floorf(position.z)), SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE);
    }

    static float servergroundheight(float x, float y)
    {
        int top = serverbasesurface(int(floorf(x)), int(floorf(y)));
        const ivec column(serverfloordiv(int(floorf(x)), SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE,
                          serverfloordiv(int(floorf(y)), SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE, 0);
        if(serverchunkforcell(column)) top = SERVER_WORLD_MAP_SIZE;
        while(top > 0 && !serverblocksolid(ivec(serverfloordiv(int(floorf(x)), SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE,
                                                 serverfloordiv(int(floorf(y)), SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE,
                                                 top - SERVER_WORLD_BLOCK_SIZE))) top -= SERVER_WORLD_BLOCK_SIZE;
        return float(top);
    }

    static bool servernaturalwaterat(const vec &position)
    {
        const int surface = serverbasesurface(int(floorf(position.x)), int(floorf(position.y))),
                  watertop = SERVER_WORLD_GROUND_HEIGHT + serverworldgenerator->settings.sealevel * SERVER_WORLD_BLOCK_SIZE;
        return surface < watertop && position.z >= surface && position.z < watertop;
    }

    static float serversunlightintensity()
    {
        static const float hours[] = { 0, 5, 6, 7, 8, 16, 17, 18, 19, 24 };
        static const float intensities[] = { 0.06f, 0.05f, 0.30f, 0.75f, 1.0f, 1.0f, 0.75f, 0.28f, 0.05f, 0.06f };
        const float hour = float(worldclockmillis) * 24.0f / SERVER_DAY_MILLIS;
        loopi(int(sizeof(hours) / sizeof(hours[0])) - 1) if(hour <= hours[i + 1])
        {
            float blend = clamp((hour - hours[i]) / (hours[i + 1] - hours[i]), 0.0f, 1.0f);
            blend = blend * blend * (3.0f - 2.0f * blend);
            return intensities[i] + (intensities[i + 1] - intensities[i]) * blend;
        }
        return intensities[0];
    }

    static float serverambientlightlevel()
    {
        static const float hours[] = { 0, 5, 6, 7, 8, 16, 17, 18, 19, 24 };
        static const int colors[] =
        {
            0x080C20, 0x10172D, 0x302A40, 0x4B4658, 0x5A5A6E, 0x5A5A6E, 0x4B4658, 0x30243A, 0x10172D, 0x080C20
        };
        const float hour = float(worldclockmillis) * 24.0f / SERVER_DAY_MILLIS;
        loopi(int(sizeof(hours) / sizeof(hours[0])) - 1) if(hour <= hours[i + 1])
        {
            float blend = clamp((hour - hours[i]) / (hours[i + 1] - hours[i]), 0.0f, 1.0f);
            blend = blend * blend * (3.0f - 2.0f * blend);
            bvec color;
            color.lerp(bvec::hexcolor(colors[i]), bvec::hexcolor(colors[i + 1]), blend);
            return (color.r * 0.2126f + color.g * 0.7152f + color.b * 0.0722f) * (1.25f * 16.0f / 255.0f);
        }
        const bvec color = bvec::hexcolor(colors[0]);
        return (color.r * 0.2126f + color.g * 0.7152f + color.b * 0.0722f) * (1.25f * 16.0f / 255.0f);
    }

    static bool servercelldirectsky(const ivec &cell)
    {
        if(cell.x < 0 || cell.y < 0 || cell.x >= SERVER_WORLD_MAP_SIZE || cell.y >= SERVER_WORLD_MAP_SIZE || cell.z < 0 ||
           cell.z >= SERVER_WORLD_MAP_SIZE || serverblocksolid(cell)) return false;
        for(int z = cell.z + SERVER_WORLD_BLOCK_SIZE; z < SERVER_WORLD_MAP_SIZE; z += SERVER_WORLD_BLOCK_SIZE)
            if(serverblocksolid(ivec(cell.x, cell.y, z))) return false;
        return true;
    }

    static int serverskylightlevel(const vec &position)
    {
        struct skynode
        {
            ivec cell;
            int distance;

            skynode(const ivec &cell, int distance) : cell(cell), distance(distance) {}
        };
        static const ivec directions[] =
        {
            ivec(SERVER_WORLD_BLOCK_SIZE, 0, 0), ivec(-SERVER_WORLD_BLOCK_SIZE, 0, 0),
            ivec(0, SERVER_WORLD_BLOCK_SIZE, 0), ivec(0, -SERVER_WORLD_BLOCK_SIZE, 0),
            ivec(0, 0, SERVER_WORLD_BLOCK_SIZE), ivec(0, 0, -SERVER_WORLD_BLOCK_SIZE)
        };
        const ivec start = serverblockat(position);
        if(serverblocksolid(start)) return 0;
        vector<skynode> queue;
        hashtable<ivec, int> visited(1 << 12);
        queue.add(skynode(start, 0));
        visited[start] = 0;
        loopv(queue)
        {
            const skynode node = queue[i];
            if(servercelldirectsky(node.cell)) return 16 - node.distance;
            if(node.distance >= 15) continue;
            loopj(int(sizeof(directions) / sizeof(directions[0])))
            {
                const ivec next = ivec(node.cell).add(directions[j]);
                if(next.x < 0 || next.y < 0 || next.z < 0 || next.x >= SERVER_WORLD_MAP_SIZE || next.y >= SERVER_WORLD_MAP_SIZE ||
                   next.z >= SERVER_WORLD_MAP_SIZE || visited.access(next) || serverblocksolid(next)) continue;
                visited[next] = node.distance + 1;
                queue.add(skynode(next, node.distance + 1));
            }
        }
        return 0;
    }

    static int serverequippeditem(const clientinfo &ci)
    {
        if(servercreative()) return ci.selectedcreative;
        return ci.selectedslot >= 0 && ci.selectedslot < SURVIVAL_HOTBAR_SLOTS && ci.inventorycounts[ci.selectedslot] > 0
             ? ci.inventoryitems[ci.selectedslot] : -1;
    }

    static int serverlightlevel(const vec &position)
    {
        const float skyexposure = serverskylightlevel(position) / 16.0f;
        float level = skyexposure * (serversunlightintensity() * 16.0f + serverambientlightlevel());
        loopv(serverchunks)
        {
            const serverchunk &chunk = *serverchunks[i];
            if(chunk.loading || chunk.corrupted) continue;
            loopvj(chunk.scatter)
            {
                const worldscatterinstance &scatter = chunk.scatter[j];
                const int item = getworldscatteritem(scatter.type);
                if(getworlditemtype(item) != WORLD_ITEM_PLACEABLE) continue;
                const float radius = getworlditemlightradius(item);
                if(radius <= 0) continue;
                const vec emitter(chunk.x * SERVER_WORLD_CHUNK_SIZE + scatter.x + SERVER_WORLD_BLOCK_SIZE * 0.5f,
                                  chunk.y * SERVER_WORLD_CHUNK_SIZE + scatter.y + SERVER_WORLD_BLOCK_SIZE * 0.5f,
                                  scatter.z + SERVER_WORLD_BLOCK_SIZE);
                level = max(level, radius - emitter.dist(position) / SERVER_WORLD_BLOCK_SIZE);
            }
        }
        loopv(clients)
        {
            const clientinfo *ci = clients[i];
            if(!ci || !ci->connected || !ci->worldready || !ci->hasposition || ci->dead) continue;
            const float radius = getworlditemlightradius(serverequippeditem(*ci));
            if(radius > 0) level = max(level, radius - vec(ci->o).addz(SERVER_PLAYER_EYE_HEIGHT).dist(position) / SERVER_WORLD_BLOCK_SIZE);
        }
        return clamp(int(floorf(level + 0.5f)), 0, 16);
    }

    static int serveraggressivespawnlightlevel(const vec &position)
    {
        const vec feet = vec(position).subz(28.0f);
        const vec cell(floorf(feet.x / SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE + SERVER_WORLD_BLOCK_SIZE * 0.5f,
                       floorf(feet.y / SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE + SERVER_WORLD_BLOCK_SIZE * 0.5f,
                       floorf(feet.z / SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE + SERVER_WORLD_BLOCK_SIZE * 0.5f);
        return max(serverlightlevel(position), serverlightlevel(cell));
    }

    static void sendplayerstate(int cn, const clientinfo &subject, const vec &impulse = vec(0, 0, 0))
    {
        const vec position = vec(subject.o).addz(SERVER_PLAYER_EYE_HEIGHT);
        sendf(cn, 1, "ri9", N_PLAYERSTATE, subject.clientnum, int(subject.health * 1000.0f), subject.dead ? CS_DEAD : CS_ALIVE,
              int(position.x * DMF), int(position.y * DMF), int(position.z * DMF), int(impulse.x * DNF), int(impulse.y * DNF),
              int(impulse.z * DNF));
    }

    static void broadcastplayerstate(const clientinfo &subject, const vec &impulse = vec(0, 0, 0))
    {
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready)
            sendplayerstate(clients[i]->clientnum, subject, impulse);
    }

    static void sendfoodstate(int cn, const clientinfo &subject)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_FOODSTATE);
        putint(p, subject.clientnum);
        putint(p, subject.foodactive ? 1 : 0);
        putpersistentid(p, getinventoryitempersistentid(subject.foodactive ? subject.fooditem : -1));
        putint(p, subject.foodactive ? max(totalmillis - subject.foodstart, 0) : 0);
        sendpacket(cn, 1, p.finalize());
    }

    static void broadcastfoodstate(const clientinfo &subject)
    {
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready)
            sendfoodstate(clients[i]->clientnum, subject);
    }

    static void cancelfooduse(clientinfo &ci, bool broadcast = true)
    {
        if(!ci.foodactive) return;
        ci.foodactive = false;
        ci.foodstart = 0;
        ci.fooditem = ci.foodslot = -1;
        if(broadcast) broadcastfoodstate(ci);
    }

    static void handlefoodaction(clientinfo &ci, bool active)
    {
        if(!active)
        {
            cancelfooduse(ci);
            return;
        }
        if(ci.foodactive)
        {
            sendfoodstate(ci.clientnum, ci);
            return;
        }
        const int slot = ci.selectedslot,
                  item = slot >= 0 && slot < SURVIVAL_HOTBAR_SLOTS && ci.inventorycounts[slot] > 0 ? ci.inventoryitems[slot] : -1;
        if(servercreative() || ci.dead || !ci.worldready || ci.health >= game::PLAYER_MAX_HEALTH || !isinventoryfood(item))
        {
            sendfoodstate(ci.clientnum, ci);
            return;
        }
        ci.foodactive = true;
        ci.foodstart = max(totalmillis, 1);
        ci.fooditem = item;
        ci.foodslot = slot;
        broadcastfoodstate(ci);
    }

    static void updatefooduse(clientinfo &ci)
    {
        if(!ci.foodactive) return;
        const bool valid = !servercreative() && !ci.dead && ci.worldready && ci.health < game::PLAYER_MAX_HEALTH &&
                           ci.foodslot == ci.selectedslot && ci.foodslot >= 0 && ci.foodslot < SURVIVAL_HOTBAR_SLOTS &&
                           ci.inventorycounts[ci.foodslot] > 0 && ci.inventoryitems[ci.foodslot] == ci.fooditem &&
                           isinventoryfood(ci.fooditem);
        if(!valid)
        {
            cancelfooduse(ci);
            return;
        }
        if(totalmillis - ci.foodstart < getinventoryfoodtime(ci.fooditem)) return;

        if(--ci.inventorycounts[ci.foodslot] <= 0)
        {
            ci.inventoryitems[ci.foodslot] = -1;
            ci.inventorycounts[ci.foodslot] = ci.inventorydurabilities[ci.foodslot] = 0;
        }
        ci.health = min(ci.health + getinventoryfoodhealth(ci.fooditem), float(game::PLAYER_MAX_HEALTH));
        ci.positiondirty = true;
        markinventorydirty(ci);
        sendinventory(ci);
        broadcastplayerstate(ci);

        if(ci.health < game::PLAYER_MAX_HEALTH && ci.inventorycounts[ci.foodslot] > 0 && ci.inventoryitems[ci.foodslot] == ci.fooditem)
        {
            ci.foodstart = max(totalmillis, 1);
            broadcastfoodstate(ci);
        }
        else cancelfooduse(ci);
    }

    static void addserverplayerdrop(clientinfo &ci, int item, int count, int durability, const vec &origin, uint spreadseed)
    {
        if(item < 0 || count <= 0) return;
        while(serverdrops.length() >= maxdrop) removeserverdrop(0);
        const uint hash = worlddrophash(spreadseed), anglehash = worlddrophash(hash ^ 0x9E3779B9U);
        const float angle = float(anglehash % 36000U) * RAD / 100.0f,
                    radius = 1.5f + float(hash % 350U) / 100.0f;
        serverdrop *drop = new serverdrop;
        if(!nextdropid || nextdropid > uint(INT_MAX)) nextdropid = 1;
        drop->id = nextdropid++;
        drop->sourcerequestid = uint(ci.deathsequence);
        drop->source = ci.clientnum;
        drop->item = item;
        drop->count = count;
        drop->durability = isinventorytool(item) ? clamp(durability, 1, getinventorytoolmaxdurability(item)) : 0;
        drop->created = max(totalmillis, 1);
        drop->o = vec(origin).add(vec(cosf(angle) * radius, sinf(angle) * radius, 3.0f));
        copystring(drop->ownerid, ci.playerid);
        serverdrops.add(drop);
        dirtyservergameplaycell(ivec(int(floorf(drop->o.x)), int(floorf(drop->o.y)), int(floorf(drop->o.z))));
        broadcastdropspawn(*drop);
    }

    static void dropserverplayerinventory(clientinfo &ci)
    {
        const uint seed = worlddrophash(uint(++ci.deathsequence) ^ uint(max(totalmillis, 1)) ^ uint(ci.clientnum + 1) * 0x85EBCA6BU);
        const vec origin = ci.o;
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            addserverplayerdrop(ci, ci.inventoryitems[i], ci.inventorycounts[i], ci.inventorydurabilities[i], origin,
                                seed ^ uint(i + 1) * 0xC2B2AE35U);
            ci.inventoryitems[i] = -1;
            ci.inventorycounts[i] = ci.inventorydurabilities[i] = 0;
        }
        loopi(CRAFT_GRID_MAX)
        {
            addserverplayerdrop(ci, ci.craftingitems[i], ci.craftingcounts[i], ci.craftingdurabilities[i], origin,
                                seed ^ uint(i + 65) * 0x27D4EB2FU);
            ci.craftingitems[i] = -1;
            ci.craftingcounts[i] = ci.craftingdurabilities[i] = 0;
        }
        addserverplayerdrop(ci, ci.inventorycursoritem, ci.inventorycursorcount, ci.inventorycursordurability, origin, seed ^ 0x165667B1U);
        ci.inventorycursoritem = -1;
        ci.inventorycursorcount = ci.inventorycursordurability = 0;
        ci.craftinggridsize = 2;
        ci.craftingstationitem = -1;
        markinventorydirty(ci);
        sendinventory(ci);
        sendcraftstate(ci);
    }

    static void damageserverplayer(clientinfo &ci, float damage, const vec &source)
    {
        if(servercreative() || ci.dead || damage <= 0) return;
        ci.health = max(ci.health - damage, 0.0f);
        ci.positiondirty = true;
        vec impulse = vec(ci.o).sub(source);
        if(impulse.squaredlen() > 1e-4f) impulse.normalize().mul(45.0f);
        if(ci.health <= 0)
        {
            ci.dead = true;
            ci.velocity = ci.falling = vec(0, 0, 0);
            ci.falldistance = 0;
            ci.positionphysstate = PHYS_FALL;
            cancelfooduse(ci);
            ci.position.setsize(0);
            if(ci.breakactive) cancelbreak(ci);
            dropserverplayerinventory(ci);
            ci.positiondirty = true;
            saveplayerstate(ci, true);
        }
        broadcastplayerstate(ci, impulse);
    }

    static void respawnserverplayer(clientinfo &ci)
    {
        if(!ci.dead || !servermapspawnready) return;
        ci.o = vec(servermapspawn).addz(-SERVER_PLAYER_EYE_HEIGHT);
        ci.positioncoords = ivec(int(ci.o.x * DMF), int(ci.o.y * DMF), int(ci.o.z * DMF));
        ci.positionyaw = servermapspawnyaw;
        ci.positionpitch = servermapspawnpitch;
        ci.positionphysstate = PHYS_FALL;
        ci.velocity = ci.falling = vec(0, 0, 0);
        ci.falldistance = 0;
        ci.health = game::PLAYER_MAX_HEALTH;
        ci.dead = false;
        ci.hasposition = ci.positiondirty = true;
        ci.lastpositionmillis = max(totalmillis, 1);
        ci.position.setsize(0);
        broadcastplayerstate(ci);
        saveplayerstate(ci, true);
    }

    static bool serverlineofsight(const vec &from, const vec &to)
    {
        vec delta = vec(to).sub(from);
        const float distance = delta.magnitude();
        if(distance <= 0.01f) return true;
        delta.div(distance);
        for(float traveled = 4.0f; traveled + 4.0f < distance; traveled += 4.0f)
            if(serverblocksolid(serverblockat(vec(from).madd(delta, traveled)))) return false;
        return true;
    }

    static servernpc *findservernpc(uint id)
    {
        loopv(servernpcs) if(servernpcs[i]->id == id) return servernpcs[i];
        return NULL;
    }

    static bool clientknowsnpc(const clientinfo &ci, uint id)
    {
        loopv(ci.knownnpcs) if(ci.knownnpcs[i] == id) return true;
        return false;
    }

    static int servernpcstateflags(const servernpc &mob)
    {
        int flags = mob.deathmillis ? NPC_STATE_DEAD : 0;
        if(mob.frozen) flags |= NPC_STATE_FROZEN;
        if(mob.attacking) flags |= NPC_STATE_ATTACKING;
        const uint legmask = mob.definition->modeltype == NPC_MODEL_QUADRUPED
                           ? (1U << HITBOX_LEFT_ARM) | (1U << HITBOX_RIGHT_ARM) | (1U << HITBOX_LEFT_LEG) | (1U << HITBOX_RIGHT_LEG)
                           : (1U << HITBOX_LEFT_LEG) | (1U << HITBOX_RIGHT_LEG);
        if((mob.detachedparts & legmask) == legmask) flags |= NPC_STATE_CRAWLING;
        return flags;
    }

    static void sendnpcspawn(clientinfo &ci, const servernpc &mob)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_NPCSPAWN);
        putint(p, int(mob.id));
        sendstring(mob.definition->id, p);
        putint(p, int(mob.o.x * DMF)); putint(p, int(mob.o.y * DMF)); putint(p, int(mob.o.z * DMF));
        putint(p, int(mob.yaw * 10.0f));
        putint(p, int(mob.health * 1000.0f));
        putint(p, int(mob.detachedparts));
        putint(p, servernpcstateflags(mob));
        sendpacket(ci.clientnum, 1, p.finalize());
        ci.knownnpcs.add(mob.id);
    }

    static void sendnpcdespawn(clientinfo &ci, uint id)
    {
        sendf(ci.clientnum, 1, "ri3", N_NPCDESPAWN, int(id), 0);
        loopv(ci.knownnpcs) if(ci.knownnpcs[i] == id)
        {
            ci.knownnpcs.remove(i);
            break;
        }
    }

    static void broadcastnpcevent(const servernpc &mob, int event, int part, const vec &position, const vec &impulse)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_NPCEVENT); putint(p, int(mob.id)); putint(p, event); putint(p, totalmillis);
        putint(p, int(mob.health * 1000.0f)); putint(p, int(mob.detachedparts)); putint(p, part);
        putint(p, int(position.x * DMF)); putint(p, int(position.y * DMF)); putint(p, int(position.z * DMF));
        putint(p, int(impulse.x * DNF)); putint(p, int(impulse.y * DNF)); putint(p, int(impulse.z * DNF));
        ENetPacket *packet = p.finalize();
        packet->referenceCount++;
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready && clientknowsnpc(*clients[i], mob.id))
            sendpacket(clients[i]->clientnum, 1, packet);
        if(--packet->referenceCount == 0) enet_packet_destroy(packet);
    }

    static void removeservernpc(int index)
    {
        const uint id = servernpcs[index]->id;
        loopv(clients) if(clients[i] && clientknowsnpc(*clients[i], id)) sendnpcdespawn(*clients[i], id);
        delete servernpcs.remove(index);
    }

    void resetservernpcs()
    {
        while(servernpcs.length()) removeservernpc(servernpcs.length() - 1);
        nextnpcid = 1;
        lastnpcspawnattempt = 0;
        lastpassivenpcscan = 0;
    }

    static bool servernpcclearance(const vec &position, const npcdefinition &definition, uint ignore = 0)
    {
        const float offsets[3] = { -definition.radius, 0.0f, definition.radius };
        const float feet = position.z - definition.height, heights[3] = { feet + 1.0f, feet + definition.height * 0.5f, position.z - 1.0f };
        loopi(3) loopj(3)
        {
            loopk(3) if(serverblocksolid(serverblockat(vec(position.x + offsets[i], position.y + offsets[j], heights[k])))) return false;
        }
        loopv(servernpcs)
            if(servernpcs[i]->id != ignore && !servernpcs[i]->deathmillis)
            {
                const float combined = servernpcs[i]->definition->radius + definition.radius + 1.0f,
                            otherfeet = servernpcs[i]->o.z - servernpcs[i]->definition->height;
                const float dx = servernpcs[i]->o.x - position.x, dy = servernpcs[i]->o.y - position.y;
                if(dx * dx + dy * dy < combined * combined &&
                   feet < servernpcs[i]->o.z && position.z > otherfeet) return false;
            }
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->hasposition)
        {
            const float dx = clients[i]->o.x - position.x, dy = clients[i]->o.y - position.y,
                        combined = definition.radius + 4.1f, npcfeet = position.z - definition.height;
            if(dx * dx + dy * dy < combined * combined && clients[i]->o.z < position.z + 2.0f && clients[i]->o.z + 30.0f > npcfeet) return false;
        }
        return true;
    }

    static bool serverplayeroverlapsnpc(const vec &feet)
    {
        loopv(servernpcs) if(!servernpcs[i]->deathmillis)
        {
            const servernpc &mob = *servernpcs[i];
            const float dx = feet.x - mob.o.x, dy = feet.y - mob.o.y, combined = mob.definition->radius + 4.1f,
                        npcfeet = mob.o.z - mob.definition->height;
            if(dx * dx + dy * dy < combined * combined && feet.z < mob.o.z + 2.0f && feet.z + 30.0f > npcfeet) return true;
        }
        return false;
    }

    static servernpc *spawnservernpc(clientinfo &owner, const char *id)
    {
        npcdefinition *definition = game::findnpcdefinition(id);
        if(!definition || !owner.hasposition) return NULL;
        const vec direction = serverdirection(float(owner.positionyaw), 0);
        vec position = vec(owner.o).madd(direction, SERVER_WORLD_BLOCK_SIZE * 3.0f);
        position.x = floorf(position.x / SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE + SERVER_WORLD_BLOCK_SIZE * 0.5f;
        position.y = floorf(position.y / SERVER_WORLD_BLOCK_SIZE) * SERVER_WORLD_BLOCK_SIZE + SERVER_WORLD_BLOCK_SIZE * 0.5f;
        position.z = servergroundheight(position.x, position.y) + definition->height;
        if(position.z < definition->height || position.z > 8191.0f || !servernpcclearance(position, *definition) ||
           !serverlineofsight(vec(owner.o).addz(28.0f), position)) return NULL;
        servernpc *mob = new servernpc(nextnpcid++, definition);
        mob->o = mob->spawn = mob->destination = position;
        mob->yaw = fmodf(owner.positionyaw + 180.0f, 360.0f);
        mob->pauseuntil = totalmillis + 600;
        servernpcs.add(mob);
        return mob;
    }

    static int livingserveraggressivenpcs()
    {
        int count = 0;
        loopv(servernpcs) if(!servernpcs[i]->deathmillis && servernpcs[i]->definition->attitude == NPC_AGGRESSIVE) ++count;
        return count;
    }

    static npcdefinition *serveraggressivenpcdefinition(uint seed)
    {
        int count = 0;
        loopi(game::numnpcdefinitions()) if(game::getnpcdefinition(i)->attitude == NPC_AGGRESSIVE) ++count;
        if(!count) return NULL;
        int selected = int(seed % uint(count));
        loopi(game::numnpcdefinitions()) if(game::getnpcdefinition(i)->attitude == NPC_AGGRESSIVE && selected-- == 0)
            return game::getnpcdefinition(i);
        return NULL;
    }

    static void tryspawnserveraggressivenpc()
    {
        if(servercreative() || totalmillis - lastnpcspawnattempt < servernpcspawnmillis) return;
        lastnpcspawnattempt = totalmillis;
        const int cap = serversimulationmaxdist / 2;
        if(cap <= 0 || livingserveraggressivenpcs() >= cap || clients.empty()) return;

        const uint seed = worlddrophash(uint(max(totalmillis, 1)) ^ nextnpcid * 0x9E3779B9U);
        clientinfo *owner = NULL;
        const int start = int(seed % uint(clients.length()));
        loopi(clients.length())
        {
            clientinfo *candidate = clients[(start + i) % clients.length()];
            if(candidate && candidate->connected && candidate->worldready && candidate->hasposition && !candidate->dead)
            {
                owner = candidate;
                break;
            }
        }
        if(!owner) return;
        npcdefinition *definition = serveraggressivenpcdefinition(worlddrophash(seed ^ 0x85EBCA6BU));
        if(!definition) return;

        const float simulationdistance = serversimulationmaxdist * GAMEUNITSPERMETER,
                    minimumdistance = min(16.0f * GAMEUNITSPERMETER, simulationdistance * 0.5f),
                    distance = minimumdistance + (simulationdistance - minimumdistance) * float((seed >> 8) & 0xFFFFU) / 65535.0f,
                    angle = float(seed % 36000U) * RAD / 100.0f;
        vec position = vec(owner->o).add(vec(cosf(angle) * distance, sinf(angle) * distance, 0));
        if(position.x < 1 || position.y < 1 || position.x >= SERVER_WORLD_MAP_SIZE - 1 || position.y >= SERVER_WORLD_MAP_SIZE - 1) return;
        position.z = servergroundheight(position.x, position.y) + definition->height;
        if(position.z < definition->height || position.z >= SERVER_WORLD_MAP_SIZE ||
           position.squaredist(owner->o) > simulationdistance * simulationdistance || !servernpcclearance(position, *definition)) return;
        if(servernaturalwaterat(vec(position).subz(definition->height))) return;
        const int light = serveraggressivespawnlightlevel(position);
        if(light > 3) return;

        servernpc *mob = new servernpc(nextnpcid++, definition);
        mob->o = mob->spawn = mob->destination = position;
        mob->yaw = float(worlddrophash(seed ^ 0x27D4EB2FU) % 36000U) / 100.0f;
        mob->pauseuntil = totalmillis + 600;
        servernpcs.add(mob);
    }

    static servernpc *findserverpassivenpc(ullong key)
    {
        loopv(servernpcs) if(servernpcs[i]->spawnkey == key) return servernpcs[i];
        return NULL;
    }

    static bool servernaturalspawnposition(const npcdefinition &definition, const passivenpcspawn &spawn, vec &position)
    {
        position = vec((spawn.blockx + 0.5f) * GAMEUNITSPERMETER, (spawn.blocky + 0.5f) * GAMEUNITSPERMETER, 0);
        if(position.x < 1 || position.y < 1 || position.x >= SERVER_WORLD_MAP_SIZE - 1 || position.y >= SERVER_WORLD_MAP_SIZE - 1) return false;
        if(!serverworldgenerator) serverworldgenerator = new game::worldgenerator(serverworldseed);
        const int height = serverworldgenerator->height(spawn.blockx, spawn.blocky);
        if(serverworldgenerator->biome(spawn.blockx, spawn.blocky, height) != definition.naturalbiome) return false;
        position.z = servergroundheight(position.x, position.y) + definition.height;
        if(position.z < definition.height || position.z >= SERVER_WORLD_MAP_SIZE ||
           servernaturalwaterat(vec(position).subz(definition.height)) || !servernpcclearance(position, definition)) return false;
        return serverlineofsight(position, vec(position.x, position.y, SERVER_WORLD_MAP_SIZE - 1.0f));
    }

    static void tryspawnserverpassivenpcs()
    {
        if(servercreative() || totalmillis - lastpassivenpcscan < 250 || clients.empty()) return;
        lastpassivenpcscan = totalmillis;
        const int margin = PASSIVE_NPC_GROUP_RADIUS_BLOCKS + 1;
        const float simulationdistance = serversimulationmaxdist * GAMEUNITSPERMETER,
                    simulationdistancesquared = simulationdistance * simulationdistance;
        loopv(clients)
        {
            clientinfo *owner = clients[i];
            if(!owner || !owner->connected || !owner->worldready || !owner->hasposition || owner->dead) continue;
            const float focusblockx = owner->o.x / GAMEUNITSPERMETER, focusblocky = owner->o.y / GAMEUNITSPERMETER;
            const int mincellx = int(floorf((focusblockx - serversimulationmaxdist - margin) / PASSIVE_NPC_CELL_BLOCKS)),
                      maxcellx = int(floorf((focusblockx + serversimulationmaxdist + margin) / PASSIVE_NPC_CELL_BLOCKS)),
                      mincelly = int(floorf((focusblocky - serversimulationmaxdist - margin) / PASSIVE_NPC_CELL_BLOCKS)),
                      maxcelly = int(floorf((focusblocky + serversimulationmaxdist + margin) / PASSIVE_NPC_CELL_BLOCKS));
            loopj(game::numnpcdefinitions())
            {
                npcdefinition *definition = game::getnpcdefinition(j);
                if(!definition || definition->naturalbiome < 0) continue;
                for(int cellx = mincellx; cellx <= maxcellx; ++cellx) for(int celly = mincelly; celly <= maxcelly; ++celly)
                {
                    passivenpcspawn spawns[16];
                    const int count = game::generatepassivenpcgroup(*definition, serverworldseed, cellx, celly, spawns, 16);
                    if(!count) continue;
                    const vec anchor((spawns[0].blockx + 0.5f) * GAMEUNITSPERMETER,
                                     (spawns[0].blocky + 0.5f) * GAMEUNITSPERMETER, owner->o.z);
                    if(anchor.squaredist(owner->o) > simulationdistancesquared) continue;

                    vec positions[16];
                    bool needed[16] = { false }, valid = true;
                    loopk(count)
                    {
                        const ivec spawnorigin(spawns[k].blockx * SERVER_WORLD_BLOCK_SIZE, spawns[k].blocky * SERVER_WORLD_BLOCK_SIZE, 0);
                        serverchunk *chunk = serverchunkforcell(spawnorigin, true);
                        if(!chunk || chunk->loading || chunk->corrupted) { valid = false; break; }
                        needed[k] = !serverpassivenpcdead(spawns[k].key) && !findserverpassivenpc(spawns[k].key);
                        if(needed[k] && !servernaturalspawnposition(*definition, spawns[k], positions[k])) { valid = false; break; }
                    }
                    if(!valid) continue;
                    loopk(count) if(needed[k])
                    {
                        servernpc *mob = new servernpc(nextnpcid++, definition);
                        mob->spawnkey = spawns[k].key;
                        mob->o = mob->spawn = mob->destination = positions[k];
                        mob->yaw = spawns[k].yaw;
                        mob->pauseuntil = totalmillis + 600;
                        servernpcs.add(mob);
                    }
                }
            }
        }
    }

    static bool serverraybox(const vec &origin, const vec &direction, const vec &minimum, const vec &maximum, float &distance)
    {
        float nearplane = 0.0f, farplane = distance;
        loopi(3)
        {
            if(fabsf(direction[i]) < 1e-6f)
            {
                if(origin[i] < minimum[i] || origin[i] > maximum[i]) return false;
                continue;
            }
            float first = (minimum[i] - origin[i]) / direction[i], second = (maximum[i] - origin[i]) / direction[i];
            if(first > second) swap(first, second);
            nearplane = max(nearplane, first);
            farplane = min(farplane, second);
            if(nearplane > farplane) return false;
        }
        distance = nearplane;
        return true;
    }

    static void servernpchitbox(const servernpc &mob, int part, vec &center, vec &radius)
    {
        const float feet = mob.o.z - mob.definition->height, torso = feet + mob.definition->rootheight,
                    cosine = cosf(RAD * mob.yaw), sine = sinf(RAD * mob.yaw);
        const vec lateral(cosine, sine, 0), forward(-sine, cosine, 0);
        if(mob.definition->modeltype == NPC_MODEL_QUADRUPED)
        {
            switch(part)
            {
                case HITBOX_HEAD:
                    center = vec(mob.o).madd(forward, 11.0f).addz(-4.5f);
                    radius = vec(4.5f, 4.5f, 4.5f);
                    break;
                case HITBOX_LEFT_ARM: case HITBOX_RIGHT_ARM: case HITBOX_LEFT_LEG: case HITBOX_RIGHT_LEG:
                    center = vec(mob.o).madd(forward, part <= HITBOX_RIGHT_ARM ? 7.0f : -7.0f)
                                          .madd(lateral, part == HITBOX_LEFT_ARM || part == HITBOX_LEFT_LEG ? -4.0f : 4.0f);
                    center.z = feet + 6.0f;
                    radius = vec(2, 2, 6);
                    break;
                default:
                    center = vec(mob.o).addz(-8.0f);
                    radius = vec(fabsf(forward.x) * 9.0f + fabsf(lateral.x) * 6.0f,
                                 fabsf(forward.y) * 9.0f + fabsf(lateral.y) * 6.0f, 5.0f);
                    break;
            }
            return;
        }
        switch(part)
        {
            case HITBOX_HEAD: center = vec(mob.o.x, mob.o.y, torso + 16.0f); radius = vec(4, 4, 4); break;
            case HITBOX_LEFT_ARM: center = vec(mob.o).madd(lateral, -6.0f); center.z = torso + 5.0f; radius = vec(2, 2, 5); break;
            case HITBOX_RIGHT_ARM: center = vec(mob.o).madd(lateral, 6.0f); center.z = torso + 5.0f; radius = vec(2, 2, 5); break;
            case HITBOX_LEFT_LEG: center = vec(mob.o).madd(lateral, -2.0f); center.z = feet + 5.25f; radius = vec(2, 2, 6); break;
            case HITBOX_RIGHT_LEG: center = vec(mob.o).madd(lateral, 2.0f); center.z = feet + 5.25f; radius = vec(2, 2, 6); break;
            default:
                center = vec(mob.o.x, mob.o.y, torso + 6.0f);
                radius = vec(fabsf(cosine) * 4.0f + fabsf(sine) * 2.0f, fabsf(sine) * 4.0f + fabsf(cosine) * 2.0f, 6.0f);
                break;
        }
    }

    static float serveractionreach()
    {
        return servercreative() ? float(buildreach) : float(game::SURVIVAL_BUILD_REACH);
    }

    static bool servernpcinterceptsaction(const clientinfo &ci, const ivec &target)
    {
        const vec origin = vec(ci.o).addz(28.0f), direction = serverdirection(float(ci.positionyaw), float(ci.positionpitch)),
                  minimum(target), maximum = vec(target).add(SERVER_WORLD_BLOCK_SIZE);
        float targetdistance = serveractionreach();
        if(!serverraybox(origin, direction, minimum, maximum, targetdistance)) return false;

        loopv(servernpcs)
        {
            const servernpc &mob = *servernpcs[i];
            if(mob.deathmillis) continue;
            loopj(NUM_HUMANOID_HITBOXES)
            {
                if(j != HITBOX_TORSO && mob.detachedparts & (1U << j)) continue;
                vec center, radius;
                servernpchitbox(mob, j, center, radius);
                float distance = targetdistance;
                if(serverraybox(origin, direction, vec(center).sub(radius), vec(center).add(radius), distance) && distance < targetdistance)
                    return true;
            }
        }
        return false;
    }

    static float servernpcpartmultiplier(int part)
    {
        return part == HITBOX_HEAD ? 2.0f : part == HITBOX_TORSO ? 1.0f : 0.75f;
    }

    static void addservernpcdrop(int item, int count, const vec &origin, uint spreadseed)
    {
        if(item < 0 || count <= 0) return;
        while(serverdrops.length() >= maxdrop) removeserverdrop(0);
        const uint hash = worlddrophash(spreadseed), anglehash = worlddrophash(hash ^ 0x9E3779B9U);
        const float angle = float(anglehash % 36000U) * RAD / 100.0f, radius = 1.5f + float(hash % 350U) / 100.0f;
        serverdrop *drop = new serverdrop;
        if(!nextdropid || nextdropid > uint(INT_MAX)) nextdropid = 1;
        drop->id = nextdropid++;
        drop->source = -1;
        drop->item = item;
        drop->count = count;
        drop->created = max(totalmillis, 1);
        drop->o = vec(origin).add(vec(cosf(angle) * radius, sinf(angle) * radius, 3.0f));
        serverdrops.add(drop);
        dirtyservergameplaycell(ivec(int(floorf(drop->o.x)), int(floorf(drop->o.y)), int(floorf(drop->o.z))));
        broadcastdropspawn(*drop);
    }

    static void dropservernpcloot(servernpc &mob)
    {
        const uint seed = worlddrophash(mob.id ^ uint(max(totalmillis, 1)) * 0x9E3779B9U);
        loopv(mob.definition->drops)
        {
            const npcdropdefinition &entry = mob.definition->drops[i];
            const uint roll = worlddrophash(seed ^ uint(i + 1) * 0x85EBCA6BU);
            if(float(roll & 0xFFFFU) / 65535.0f > entry.chance) continue;
            const int item = getinventoryitemindex(entry.itemid);
            if(item < 0) continue;
            const int range = entry.maxcount - entry.mincount + 1, quantity = entry.mincount + int((roll >> 16) % uint(range));
            addservernpcdrop(item, quantity, vec(mob.o).subz(mob.definition->height), roll);
        }
    }

    static void killservernpc(servernpc &mob, int part, const vec &position, const vec &impulse)
    {
        if(mob.deathmillis) return;
        mob.deathmillis = max(totalmillis, 1);
        mob.velocity = vec(0, 0, 0);
        killserverpassivenpc(mob.spawnkey, mob.spawn);
        dropservernpcloot(mob);
        broadcastnpcevent(mob, NPC_EVENT_DEATH, part, position, impulse);
    }

    static void triggerservernpcherdflee(servernpc &mob, const vec &source)
    {
        if(mob.definition->fleeonhitmillis <= 0) return;
        const float radius = mob.definition->herdradius * GAMEUNITSPERMETER, radiussquared = radius * radius;
        loopv(servernpcs)
        {
            servernpc &candidate = *servernpcs[i];
            if(candidate.deathmillis || candidate.definition != mob.definition || candidate.o.squaredist(mob.o) > radiussquared) continue;
            candidate.fleeorigin = source;
            candidate.fleeuntil = totalmillis + candidate.definition->fleeonhitmillis;
            candidate.paused = false;
        }
    }

    static void handleservernpcattack(clientinfo &ci, uint requestid, uint npcid, int claimedpart)
    {
        if(!requestid || requestid <= ci.lastnpcattackrequest || claimedpart < HITBOX_TORSO || claimedpart >= NUM_HUMANOID_HITBOXES)
        {
            kickviolation(ci, "stale or malformed NPC attack request");
            return;
        }
        ci.lastnpcattackrequest = requestid;
        if(!ci.hasposition || totalmillis - ci.lastnpcattackattempt < 75 || totalmillis - ci.lastnpcattack < game::CREATIVE_ARM_CYCLE ||
           (claimedpart != HITBOX_TORSO && findservernpc(npcid) && (findservernpc(npcid)->detachedparts & (1U << claimedpart)))) return;
        ci.lastnpcattackattempt = totalmillis;
        servernpc *requested = findservernpc(npcid);
        if(!requested || requested->deathmillis) return;

        const int equipped = servercreative() ? ci.selectedcreative : ci.selectedslot >= 0 && ci.selectedslot < SURVIVAL_HOTBAR_SLOTS &&
                             ci.inventorycounts[ci.selectedslot] > 0 ? ci.inventoryitems[ci.selectedslot] : -1;
        if(equipped >= numinventoryitems())
        {
            kickviolation(ci, "invalid equipped item in NPC attack");
            return;
        }
        const float basedamage = equipped >= 0 && isinventorytool(equipped) ? getinventorytooldamage(equipped) : 1.0f,
                    reach = 5.0f * GAMEUNITSPERMETER;
        vec origin = vec(ci.o).addz(28.0f), direction = serverdirection(float(ci.positionyaw), float(ci.positionpitch));
        servernpc *hit = NULL;
        int hitpart = HITBOX_TORSO;
        float hitdistance = reach;
        loopv(servernpcs)
        {
            servernpc &candidate = *servernpcs[i];
            if(candidate.deathmillis) continue;
            loopj(NUM_HUMANOID_HITBOXES)
            {
                if(j != HITBOX_TORSO && candidate.detachedparts & (1U << j)) continue;
                vec center, radius;
                servernpchitbox(candidate, j, center, radius);
                float distance = hitdistance;
                if(!serverraybox(origin, direction, vec(center).sub(radius), vec(center).add(radius), distance) || distance > hitdistance) continue;
                hit = &candidate;
                hitpart = j;
                hitdistance = distance;
            }
        }
        if(hit != requested || hitpart != claimedpart) return;
        const vec hitposition = vec(origin).madd(direction, hitdistance);
        if(!serverlineofsight(origin, hitposition)) return;

        if(ci.breakactive) cancelbreak(ci);
        ci.lastnpcattack = totalmillis;
        const float damage = basedamage * servernpcpartmultiplier(hitpart);
        if(!servercreative() && equipped >= 0 && isinventorytool(equipped))
        {
            const int slot = ci.selectedslot;
            ci.inventorydurabilities[slot] = max(ci.inventorydurabilities[slot] - 1, 0);
            if(ci.inventorydurabilities[slot] <= 0)
            {
                ci.inventoryitems[slot] = -1;
                ci.inventorycounts[slot] = ci.inventorydurabilities[slot] = 0;
            }
            markinventorydirty(ci);
            sendinventory(ci);
        }
        hit->health = max(hit->health - damage, 0.0f);
        triggerservernpcherdflee(*hit, vec(ci.o).addz(28.0f));
        bool detached = false;
        if(hitpart != HITBOX_TORSO)
        {
            hit->parthealth[hitpart] = max(hit->parthealth[hitpart] - damage, 0.0f);
            if(hit->parthealth[hitpart] <= 0 && !(hit->detachedparts & (1U << hitpart)))
            {
                hit->detachedparts |= 1U << hitpart;
                detached = true;
            }
        }
        vec impulse(direction);
        impulse.mul(16.0f + min(damage, 12.0f) * 3.0f);
        if(hit->health <= 0)
            killservernpc(*hit, hitpart, hitposition, impulse);
        else broadcastnpcevent(*hit, detached ? NPC_EVENT_DISMEMBER : NPC_EVENT_DAMAGE, hitpart, hitposition, impulse);
    }

    static clientinfo *nearestservernpcplayer(const servernpc &mob, float radius)
    {
        clientinfo *best = NULL;
        float bestdistance = radius * radius;
        loopv(clients)
        {
            clientinfo *candidate = clients[i];
            if(!candidate || !candidate->connected || !candidate->worldready || !candidate->hasposition || candidate->dead) continue;
            const float distance = mob.o.squaredist(candidate->o);
            if(distance > bestdistance) continue;
            best = candidate;
            bestdistance = distance;
        }
        return best;
    }

    static void damageservernpcfall(servernpc &mob)
    {
        const int damage = game::fallimpactdamage(mob.falldistance);
        mob.falldistance = 0;
        if(servercreative() || damage <= 0 || mob.deathmillis) return;
        mob.health = max(mob.health - damage, 0.0f);
        mob.parthealth[HITBOX_TORSO] = max(mob.parthealth[HITBOX_TORSO] - damage, 0.0f);
        const vec impact = mob.o, impulse(0, 0, -min(45.0f, 12.0f + damage * 2.0f));
        if(mob.health <= 0)
            killservernpc(mob, HITBOX_TORSO, impact, impulse);
        else broadcastnpcevent(mob, NPC_EVENT_DAMAGE, HITBOX_TORSO, impact, impulse);
    }

    static void updateservernpcfall(servernpc &mob, int elapsed)
    {
        const float feet = mob.o.z - mob.definition->height, ground = servergroundheight(mob.o.x, mob.o.y);
        if(servernaturalwaterat(vec(mob.o).subz(mob.definition->height))) mob.falldistance = 0;
        if(!mob.airborne && feet <= ground + 0.1f)
        {
            mob.velocity.z = 0;
            return;
        }
        mob.airborne = true;
        const float seconds = elapsed / 1000.0f, previousvelocity = mob.velocity.z;
        mob.velocity.z -= 210.0f * seconds;
        const float distance = max(-(previousvelocity + mob.velocity.z) * 0.5f * seconds, 0.0f);
        if(feet - distance > ground)
        {
            mob.o.z -= distance;
            mob.falldistance = min(mob.falldistance + distance, float(1<<13));
            return;
        }
        mob.falldistance = min(mob.falldistance + max(feet - ground, 0.0f), float(1<<13));
        mob.o.z = ground + mob.definition->height;
        mob.velocity.z = 0;
        mob.airborne = false;
        damageservernpcfall(mob);
    }

    static void pickservernpcdestination(servernpc &mob)
    {
        const uint hash = worlddrophash(mob.id ^ uint(max(totalmillis, 1)));
        const float angle = float(hash % 36000U) * RAD / 100.0f,
                    distance = mob.definition->wanderradius * GAMEUNITSPERMETER * (0.25f + 0.75f * float((hash >> 16) & 0xFFFFU) / 65535.0f);
        mob.destination = vec(mob.spawn).add(vec(cosf(angle) * distance, sinf(angle) * distance, 0));
        mob.paused = false;
        mob.nextdecision = totalmillis + 8000 + int(hash % 6001U);
    }

    static void updateservernpc(servernpc &mob)
    {
        mob.attacking = false;
        if(mob.deathmillis) return;
        const int elapsed = clamp(totalmillis - mob.lastupdate, 0, 100);
        mob.lastupdate = totalmillis;
        updateservernpcfall(mob, elapsed);
        if(mob.deathmillis) return;
        clientinfo *target = NULL;
        const bool forcedflee = totalmillis < mob.fleeuntil;
        if(!forcedflee && mob.definition->attitude == NPC_AGGRESSIVE)
            target = nearestservernpcplayer(mob, mob.definition->aggrodist * GAMEUNITSPERMETER);
        else if(!forcedflee && mob.definition->attitude == NPC_SCARED)
            target = nearestservernpcplayer(mob, mob.definition->fleedist * GAMEUNITSPERMETER);
        if(forcedflee || target)
        {
            const vec targetposition = target ? vec(target->o).addz(28.0f) : mob.fleeorigin;
            vec offset;
            if(forcedflee || mob.definition->attitude == NPC_SCARED)
            {
                offset = vec(mob.o).sub(targetposition);
                offset.z = 0;
                if(offset.squaredlen() < 0.01f) offset = serverdirection(mob.yaw + 180.0f, 0);
                else offset.normalize();
                mob.destination = vec(mob.o).madd(offset, mob.definition->fleedist * GAMEUNITSPERMETER);
            }
            else
            {
                mob.destination = targetposition;
                if(mob.o.dist(targetposition) <= game::NPC_ATTACK_REACH && serverlineofsight(mob.o, targetposition) &&
                   totalmillis - mob.lastattack >= mob.definition->attackmillis)
                {
                    mob.lastattack = totalmillis;
                    mob.attacking = true;
                    broadcastnpcevent(mob, NPC_EVENT_ATTACK, -1, targetposition, vec(0, 0, 0));
                    damageserverplayer(*target, mob.definition->damage, mob.o);
                }
            }
            mob.paused = false;
        }
        else if(mob.paused)
        {
            mob.velocity.x = mob.velocity.y = 0;
            if(totalmillis >= mob.pauseuntil) pickservernpcdestination(mob);
            return;
        }
        else if(totalmillis >= mob.nextdecision || vec(mob.destination).sub(mob.o).squaredlen() < 16.0f)
        {
            mob.paused = true;
            mob.pauseuntil = totalmillis + 600 + int(worlddrophash(mob.id + uint(totalmillis)) % 1801U);
            mob.velocity.x = mob.velocity.y = 0;
            return;
        }

        vec direction = vec(mob.destination).sub(mob.o);
        direction.z = 0;
        if(direction.squaredlen() < 1.0f) return;
        direction.normalize();
        mob.yaw = fmodf(-atan2f(direction.x, direction.y) / RAD + 360.0f, 360.0f);
        float speed = mob.definition->speed;
        const uint legmask = mob.definition->modeltype == NPC_MODEL_QUADRUPED
                           ? (1U << HITBOX_LEFT_ARM) | (1U << HITBOX_RIGHT_ARM) | (1U << HITBOX_LEFT_LEG) | (1U << HITBOX_RIGHT_LEG)
                           : (1U << HITBOX_LEFT_LEG) | (1U << HITBOX_RIGHT_LEG),
                   missinglegs = mob.detachedparts & legmask;
        if(missinglegs) speed *= missinglegs == legmask ? 0.34f : 0.58f;
        if(forcedflee || (mob.definition->attitude == NPC_SCARED && target)) speed *= mob.definition->fleespeed;
        mob.velocity.x = direction.x * speed;
        mob.velocity.y = direction.y * speed;
        vec next = vec(mob.o).madd(vec(mob.velocity.x, mob.velocity.y, 0), elapsed / 1000.0f);
        const float ground = servergroundheight(next.x, next.y), oldground = mob.o.z - mob.definition->height;
        if(!mob.airborne && fabsf(ground - oldground) <= SERVER_WORLD_BLOCK_SIZE &&
           servernpcclearance(vec(next.x, next.y, ground + mob.definition->height), *mob.definition, mob.id))
        {
            next.z = ground + mob.definition->height;
            mob.o = next;
        }
        else if(ground < oldground - SERVER_WORLD_BLOCK_SIZE && servernpcclearance(vec(next.x, next.y, mob.o.z), *mob.definition, mob.id))
        {
            mob.o.x = next.x;
            mob.o.y = next.y;
            mob.airborne = true;
        }
        else mob.velocity.x = mob.velocity.y = 0;
    }

    static void updateservernpcinterest()
    {
        const float interestdistance = servernpcmaxdist * GAMEUNITSPERMETER, interestdistancesquared = interestdistance * interestdistance;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(!ci || !ci->connected || !ci->worldready || !ci->hasposition) continue;
            loopvj(servernpcs)
            {
                servernpc &mob = *servernpcs[j];
                const bool relevant = ci->o.squaredist(mob.o) <= interestdistancesquared, known = clientknowsnpc(*ci, mob.id);
                if(relevant && !known) sendnpcspawn(*ci, mob);
                else if(!relevant && known) sendnpcdespawn(*ci, mob.id);
            }
        }
    }

    static void sendservernpcsnapshots()
    {
        if(totalmillis - lastnpcsnapshot < servernpcsnapshotmillis) return;
        lastnpcsnapshot = totalmillis;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(!ci || !ci->connected || !ci->worldready) continue;
            loopvj(servernpcs) if(clientknowsnpc(*ci, servernpcs[j]->id))
            {
                const servernpc &mob = *servernpcs[j];
                packetbuf p(64);
                putint(p, N_NPCSNAPSHOT); putint(p, int(mob.id)); putint(p, totalmillis);
                putint(p, int(mob.o.x * DMF)); putint(p, int(mob.o.y * DMF)); putint(p, int(mob.o.z * DMF));
                putint(p, int(mob.velocity.x * DNF)); putint(p, int(mob.velocity.y * DNF)); putint(p, int(mob.velocity.z * DNF));
                putint(p, int(mob.yaw * 10.0f)); putint(p, servernpcstateflags(mob));
                sendpacket(ci->clientnum, 0, p.finalize());
            }
        }
    }

    static void updateservernpcs()
    {
        tryspawnserveraggressivenpc();
        tryspawnserverpassivenpcs();
        const float simulationdistance = serversimulationmaxdist * GAMEUNITSPERMETER,
                    existencedistance = servernpcmaxdist * GAMEUNITSPERMETER;
        for(int i = servernpcs.length() - 1; i >= 0; --i)
        {
            servernpc &mob = *servernpcs[i];
            bool exists = false, simulated = false;
            for(int j = 0; j < clients.length(); ++j)
            {
                clientinfo *ci = clients[j];
                if(!ci || !ci->connected || !ci->worldready || !ci->hasposition) continue;
                const float distance = ci->o.dist(mob.o);
                if(distance <= existencedistance) exists = true;
                if(distance <= simulationdistance) simulated = true;
            }
            if(!exists || (mob.deathmillis && totalmillis - mob.deathmillis >= servernpcdeathtimeout))
            {
                removeservernpc(i);
                continue;
            }
            mob.frozen = !simulated;
            if(simulated) updateservernpc(mob);
            else mob.velocity = vec(0, 0, 0);
        }
        for(int i = servercollisionchunks.length() - 1; i >= 0; --i)
            if(totalmillis - servercollisionchunks[i]->lastused >= 5000) delete servercollisionchunks.remove(i);
        updateservernpcinterest();
        sendservernpcsnapshots();
    }

    static void sendprivilege(int cn, int subject, int privilege);
    static void sendworldstate(clientinfo &ci, bool reset);
    static void sendcommandresult(clientinfo &ci, const char *message);

    static void clearidentitychallenge(clientinfo &ci)
    {
        if(ci.identitychallenge)
        {
            freechallenge(ci.identitychallenge);
            ci.identitychallenge = NULL;
        }
        ci.identitychallengemillis = 0;
    }

    static identityratelimit *identitylimit(uint ip)
    {
        loopv(identityratelimits) if(identityratelimits[i].ip == ip)
            return &identityratelimits[i];
        identityratelimit &limit = identityratelimits.add();
        limit.ip = ip;
        limit.failures = 0;
        limit.window = max(totalmillis, 1);
        return &limit;
    }

    static bool identityratelimited(uint ip)
    {
        loopvrev(identityratelimits)
        {
            if(totalmillis - identityratelimits[i].window > 60000) identityratelimits.remove(i);
        }
        if(identityratelimits.length() >= 4096) identityratelimits.remove(0);
        identityratelimit *limit = identitylimit(ip);
        return limit->failures >= 5;
    }

    static void rejectidentity(clientinfo &ci, const char *reason, bool revoked = false)
    {
        clearidentitychallenge(ci);
        ci.identitystate = IDENTITY_REJECTED;
        identityratelimit *limit = identitylimit(ci.ip);
        if(totalmillis - limit->window > 60000)
        {
            limit->window = max(totalmillis, 1);
            limit->failures = 0;
        }
        ++limit->failures;
        const char *kind = ci.identitykind == IDENTITY_KIND_NEW ? "new player" :
                           ci.identitykind == IDENTITY_KIND_RETURNING ? "returning player" :
                           ci.identitykind == IDENTITY_KIND_RECOVERY ? "registration recovery" :
                           "unclassified identity";
        conoutf(CON_WARN, "identity rejected: client %d, %s, reason: %s (failures %d/5)", ci.clientnum, kind, reason ? reason : "authentication rejected", limit->failures);
        sendf(ci.clientnum, 1, "ri2s", revoked ? N_IDENTITYREVOKED : N_IDENTITYFAILURE, PLAYER_IDENTITY_VERSION, reason ? reason : "authentication rejected");
    }

    static bool beginidentitychallenge(clientinfo &ci, const char *publickey)
    {
        if(!valididentitypoint(publickey)) return false;
        void *parsed = parsepubkey(publickey);
        if(!parsed) return false;
        uint seed[8];
        if(!identityrandombytes((uchar *)seed, sizeof(seed)))
        {
            freepubkey(parsed);
            return false;
        }
        seed[6] ^= uint(max(totalmillis, 1));
        seed[7] ^= uint(ci.clientnum);
        vector<char> challenge;
        clearidentitychallenge(ci);
        ci.identitychallenge = genchallenge(parsed, seed, sizeof(seed), challenge);
        freepubkey(parsed);
        if(!ci.identitychallenge) return false;
        ci.identitychallengemillis = max(totalmillis, 1);
        ci.identitystate = IDENTITY_AWAITING_RESPONSE;
        sendf(ci.clientnum, 1, "ri2s", N_IDENTITYCHALLENGE, PLAYER_IDENTITY_VERSION, challenge.getbuf());
        return true;
    }

    static bool duplicateidentity(clientinfo &ci)
    {
        loopv(clients)
        {
            clientinfo *other = clients[i];
            if(!other || other == &ci || !other->connected || strcmp(other->playerid, ci.playerid)) continue;
            if(identityduplicatepolicy)
            {
                disconnect_client(other->clientnum, DISC_PRIVATE);
                return false;
            }
            rejectidentity(ci, "this player identity is already connected");
            return true;
        }
        return false;
    }

    static void completeidentity(clientinfo &ci)
    {
        if(duplicateidentity(ci)) return;
        if(!loadplayerstate(ci))
        {
            rejectidentity(ci, "server player state is corrupt or incompatible; an administrator must repair it");
            return;
        }
        ci.identitystate = IDENTITY_AUTHENTICATED;
        ci.connected = true;
        const char *kind = ci.identitykind == IDENTITY_KIND_NEW ? "new player" : "returning player";
        conoutf("identity accepted: client %d, %s", ci.clientnum, kind);
        sendf(ci.clientnum, 1, "ri2s", N_IDENTITYSUCCESS, PLAYER_IDENTITY_VERSION, ci.playerid);
        sendinventory(ci);
        sendcraftstate(ci);
        sendf(ci.clientnum, 1, "ri", N_WELCOME);
        loopv(clients)
        {
            clientinfo *other = clients[i];
            if(other && other->connected) sendprivilege(ci.clientnum, other->clientnum, other->privilege);
        }
        if(servermotd[0]) sendcommandresult(ci, servermotd);
        sendworldstate(ci, false);
    }

    void *newclientinfo() { return new clientinfo; }
    void deleteclientinfo(void *info)
    {
        clientinfo *ci = (clientinfo *)info;
        if(ci && clients.inrange(ci->clientnum) && clients[ci->clientnum] == ci) clients[ci->clientnum] = NULL;
        delete ci;
    }
    static void sendprivilege(int cn, int subject, int privilege)
    {
        sendf(cn, 1, "ri3", N_SETPRIVILEGE, subject, privilege);
    }

    static void sendworldtime(int cn = -1)
    {
        sendf(cn, 1, "ri3", N_WORLDTIME, worldclockmillis, worldtimefrozen ? 1 : 0);
    }

    static int authoritativeweatherseed()
    {
        return serverweatherseed ? serverweatherseed : serverworldseed;
    }

    static void sendweatherstate(int cn = -1)
    {
        sendf(cn, 1, "ri5", N_WEATHERSTATE, authoritativeweatherseed(), int(weatherclockmillis),
              int(servercloudwindspeed * 1000.0f + 0.5f), int(servercloudwindangle * 1000.0f + 0.5f));
    }

    static void sendserveredit(int cn, const serveredit &edit)
    {
        packetbuf q(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(q, N_EDITAUTHOR);
        putint(q, edit.author);
        putint(q, int(edit.revision));
        putint(q, int(edit.requestid));
        putint(q, edit.type);
        q.put(edit.payload.getbuf(), edit.payload.length());
        sendpacket(cn, 1, q.finalize());
    }

    static void sendworldstate(clientinfo &ci, bool reset)
    {
        ci.worldready = false;
        ci.knownnpcs.setsize(0);
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_WORLDSTATE);
        putint(p, serverworldseed);
        putint(p, int(worldeditrevision));
        putint(p, worldclockmillis);
        putint(p, worldtimefrozen ? 1 : 0);
        putint(p, authoritativeweatherseed());
        putint(p, int(weatherclockmillis));
        putint(p, int(servercloudwindspeed * 1000.0f + 0.5f));
        putint(p, int(servercloudwindangle * 1000.0f + 0.5f));
        putint(p, reset ? 1 : 0);
        putint(p, gamemode);
        putint(p, survivalbreakmillis);
        putint(p, survivalscatterbreakmillis);
        putint(p, serverwaterupdatespertick);
        putint(p, serverwatersimulationmaxdist);
        putint(p, clamp(int(serverwaterflowspeed * 1000.0f + 0.5f), 100, 20000));
        putint(p, serversimulationmaxdist);
        const bool restoreposition = !reset && ci.hasposition;
        putint(p, restoreposition ? 1 : 0);
        putint(p, restoreposition ? ci.positioncoords.x : 0);
        putint(p, restoreposition ? ci.positioncoords.y : 0);
        putint(p, restoreposition ? ci.positioncoords.z : 0);
        putint(p, restoreposition ? ci.positionyaw : 0);
        putint(p, restoreposition ? ci.positionpitch : 0);
        loopk(3) putint(p, restoreposition ? int(ci.velocity[k] * DNF) : 0);
        loopk(3) putint(p, restoreposition ? int(ci.falling[k] * DNF) : 0);
        putint(p, restoreposition ? int(ci.falldistance * DMF) : 0);
        putint(p, restoreposition ? ci.positionphysstate : PHYS_FALL);
        sendpacket(ci.clientnum, 1, p.finalize());
        senddropsettings(ci.clientnum);
    }

    static void replayworld(clientinfo &ci)
    {
        loopv(serverdrops) senddropspawn(ci.clientnum, *serverdrops[i]);
        loopv(serverfallingblocks) sendfallblockspawn(ci.clientnum, *serverfallingblocks[i]);
        sendf(ci.clientnum, 1, "ri2", N_WORLDSYNC, int(worldeditrevision));
        ci.worldready = true;
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->hasposition)
        {
            sendplayerstate(ci.clientnum, *clients[i]);
            if(clients[i]->foodactive) sendfoodstate(ci.clientnum, *clients[i]);
        }
        loopv(clients) if(clients[i] && clients[i] != &ci && clients[i]->connected && clients[i]->worldready && ci.hasposition)
            sendplayerstate(clients[i]->clientnum, ci);
    }

    void serverinit()
    {
        shutdownserverchunks(serverworldinitialized);
#ifndef STANDALONE
        personaldrops = serverpersonaldrops;
        droptimeout = serverdroptimeout;
        maxdrop = servermaxdrop;
        dynamicentsmaxdistance = serverdynamicentsmaxdistance;
        requireconfirmeditems = serverrequireconfirmeditems;
#endif
        gamemode = creativemode ? STARTGAMEMODE : STARTGAMEMODE + 2;
        game::loadworldseed(serverworldseed);
        copystring(smapname, serverworld);
        serverworldinitialized = false;
        if(!loadserveridentities()) serverworldready = false;
        servernpcs.deletecontents();
        servercollisionchunks.deletecontents();
        delete serverworldgenerator;
        serverworldgenerator = NULL;
        servermapspawnready = false;
        servermapspawn = vec(0, 0, 0);
        servermapspawnyaw = servermapspawnpitch = 0;
        nextnpcid = 1;
        lastnpcsnapshot = 0;
        lastpassivenpcscan = 0;
        lastnpcspawnattempt = 0;
        if(!game::numnpcdefinitions()) game::loadnpcdefinitions();
        worldclockmillis = SERVER_START_MILLIS;
        weatherclockmillis = 0;
        worldtimefrozen = false;
        lastworldtimesync = 0;
        conoutf("server gameplay mode: %s", servercreative() ? "creative" : "survival");
    }
    int reserveclients() { return 0; }
    int numchannels() { return 3; }
    void clientdisconnect(int n)
    {
        for(int i = serverchunkrequests.length() - 1; i >= 0; --i)
            if(serverchunkrequests[i].clientnum == n) serverchunkrequests.remove(i);
        for(int i = serverchunkdeliveries.length() - 1; i >= 0; --i)
            if(serverchunkdeliveries[i].clientnum == n) serverchunkdeliveries.remove(i);
        if(clientinfo *ci = getinfo(n))
        {
            cancelbreak(*ci);
            cancelfooduse(*ci);
            ci->furnaceopen = false;
            closechest(*ci);
            if(ci->connected && !saveplayerstate(*ci, true))
                conoutf(CON_ERROR, "could not save player state for player ID %s on disconnect", ci->playerid);
            if(!ci->connected &&
               (ci->identitystate == IDENTITY_AWAITING_IDENTITY ||
                ci->identitystate == IDENTITY_AWAITING_RESPONSE))
                conoutf(CON_WARN, "identity authentication interrupted: client %d disconnected while %s",
                        ci->clientnum, ci->identitystate == IDENTITY_AWAITING_IDENTITY ? "awaiting identity selection" : "awaiting challenge response");
            if(ci->connected) sendf(-1, 1, "ri2x", N_CDIS, n, n);
            ci->connected = false;
            ci->knownnpcs.setsize(0);
            ci->identitystate = IDENTITY_REJECTED;
            clearidentitychallenge(*ci);
        }
    }

    int clientconnect(int n, uint ip)
    {
        if(!persistentserverid[0] || !ensureserverworld()) return DISC_PRIVATE;
        if(identityratelimited(ip))
        {
            conoutf(CON_WARN, "identity connection rate-limited: 5 failures in 60 seconds");
            return DISC_PRIVATE;
        }
        while(clients.length() <= n) clients.add(NULL);
        clientinfo *ci = (clientinfo *)getclientinfo(n);
        clients[n] = ci;
        ci->clientnum = n;
        ci->ip = ip;
        ci->connected = false;
        ci->local = false;
        ci->identitystate = IDENTITY_UNAUTHENTICATED;
        sendf(n, 1, "ri5ss", N_SERVINFO, n, PROTOCOL_VERSION, rnd(INT_MAX), 0, serverdesc, "");
        sendf(n, 1, "ri2s", N_SERVERIDENTITY, PLAYER_IDENTITY_VERSION, persistentserverid);
        return DISC_NONE;
    }

    void localconnect(int n)
    {
        if(!persistentserverid[0] || !ensureserverworld()) return;
        while(clients.length() <= n) clients.add(NULL);
        clientinfo *ci = (clientinfo *)getclientinfo(n);
        clients[n] = ci;
        ci->clientnum = n;
        ci->connected = ci->local = true;
        ci->identitystate = IDENTITY_AUTHENTICATED;
        ci->privilege = PRIV_ADMIN;
        ci->inventoryloaded = true;
        clearinventory(*ci);
        sendf(n, 1, "ri5ss", N_SERVINFO, n, PROTOCOL_VERSION, rnd(INT_MAX), 0, serverdesc, "");
        sendinventory(*ci);
        sendf(n, 1, "ri", N_WELCOME);
        sendprivilege(n, n, ci->privilege);
        sendworldstate(*ci, false);
    }

    void localdisconnect(int n) { clientdisconnect(n); }
    bool allowbroadcast(int n) { clientinfo *ci = getinfo(n); return ci && ci->connected; }
    void recordpacket(int chan, void *data, int len) {}

    static bool validateedit(clientinfo &ci, int type, packetbuf &p,
                             serveredit &edit, const char *&error)
    {
        (void)ci; (void)type; (void)p; (void)edit;
        error = "direct octree edits are disabled; use the authoritative world action protocol";
        return false;
    }

    static serverchunk *servereditchunk(const serveredit &edit)
    {
        ivec cell;
        if(edit.type == N_WORLDAUTH)
        {
            ucharbuf payload((uchar *)edit.payload.getbuf(), edit.payload.length());
            const int action = getint(payload);
            cell.x = getint(payload); cell.y = getint(payload); cell.z = getint(payload);
            const int orient = getint(payload);
            if(payload.overread()) return NULL;
            cell = worldactionstatecell(cell, action, worldmountorient(orient));
        }
        else if(edit.hasselection) cell = edit.selection.o;
        else return NULL;
        return serverchunkforcell(cell);
    }

    static bool acceptededit(serveredit *edit)
    {
        serverchunk *chunk = servereditchunk(*edit);
        if(!chunk || chunk->loading || chunk->corrupted)
        {
            delete edit;
            return false;
        }
        edit->revision = chunk->revision;
        ++worldeditrevision;
        edit->timestamp = uint(time(NULL));
        loopv(clients)
        {
            clientinfo *recipient = clients[i];
            if(recipient && recipient->connected && recipient->worldready && serverchunkinrange(*recipient, chunk->x, chunk->y) &&
               serverchunkdeliveredrevision(recipient->clientnum, chunk->x, chunk->y) + 1 == chunk->revision)
            {
                sendserveredit(recipient->clientnum, *edit);
                markserverchunkdelivered(recipient->clientnum, chunk->x, chunk->y, chunk->revision);
            }
        }
        delete edit;
        return true;
    }

    static bool acceptsystemworldaction(int action, const ivec &cell, int item)
    {
        const int orient = WORLD_ORIENT_TOP;
        ivec target = cell;
        if(action == WORLD_ACTION_PLACE_CUBE) target.z -= SERVER_WORLD_BLOCK_SIZE;
        serveredit *edit = new serveredit;
        edit->author = -1;
        edit->type = N_WORLDAUTH;
        packetbuf payload(MAXTRANS);
        putint(payload, action);
        putint(payload, target.x); putint(payload, target.y); putint(payload, target.z);
        putint(payload, orient);
        putpersistentid(payload, getinventoryitempersistentid(item));
        edit->payload.put(payload.buf, payload.length());
        if(!updateserverchunkblock(cell, action, item))
        {
            delete edit;
            return false;
        }
        if(!acceptededit(edit)) return false;
        setworldactionstate(cell, action, orient, item);
        return true;
    }

    static void sendfallblockspawn(int cn, const serverfallingblock &block)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_FALLBLOCKSPAWN); putint(p, int(block.id));
        putpersistentid(p, getinventoryitempersistentid(block.item));
        putint(p, int(block.o.x * DMF)); putint(p, int(block.o.y * DMF)); putint(p, int(block.o.z * DMF));
        putint(p, int(block.velocity * DNF));
        sendpacket(cn, 1, p.finalize());
    }

    static void broadcastfallblockspawn(const serverfallingblock &block)
    {
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready) sendfallblockspawn(clients[i]->clientnum, block);
    }

    static bool serverfallingblockbelow(const ivec &cell)
    {
        loopv(serverfallingblocks)
        {
            const serverfallingblock &block = *serverfallingblocks[i];
            if(block.origin.x == cell.x && block.origin.y == cell.y && block.o.z < cell.z + SERVER_WORLD_BLOCK_SIZE / 2)
                return true;
        }
        return false;
    }

    static bool serverfallingblockoccupies(const ivec &cell)
    {
        loopv(serverfallingblocks)
        {
            const serverfallingblock &block = *serverfallingblocks[i];
            if(cell.x == block.origin.x && cell.y == block.origin.y &&
               cell.z + SERVER_WORLD_BLOCK_SIZE > block.o.z - SERVER_WORLD_BLOCK_SIZE / 2 && cell.z < block.o.z + SERVER_WORLD_BLOCK_SIZE / 2)
                return true;
        }
        return false;
    }

    static void removeserverfallingblock(int index)
    {
        if(!serverfallingblocks.inrange(index)) return;
        serverfallingblock *block = serverfallingblocks.remove(index);
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready)
            sendf(clients[i]->clientnum, 1, "ri2", N_FALLBLOCKDELETE, int(block->id));
        delete block;
    }

    static bool serverfallblockinrange(const ivec &cell)
    {
        const vec center = vec(cell).add(SERVER_WORLD_BLOCK_SIZE / 2);
        const float radius = serversimulationmaxdist * GAMEUNITSPERMETER, radiussquared = radius * radius;
        loopv(clients)
        {
            const clientinfo *ci = clients[i];
            if(ci && ci->connected && ci->worldready && ci->hasposition && center.squaredist(ci->o) <= radiussquared) return true;
        }
        return false;
    }

    struct serversupportsearchnode
    {
        ivec cell;
        int reach, distance, worldindex;

        serversupportsearchnode(const ivec &cell, int reach, int worldindex) : cell(cell), reach(reach), distance(0), worldindex(worldindex) {}
    };

    static int serverblockworldindex(const ivec &cell)
    {
        const int item = serverblockitem(cell);
        return getworlditemtype(item) == WORLD_ITEM_CUBE ? getworlditemindex(item) : -1;
    }

    static int serversupportdistance(const ivec &cell, int maxdistance)
    {
        static const int directions[6][3] =
        {
            { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 },
            { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
        };
        vector<serversupportsearchnode> nodes;
        hashtable<ivec, int> indexes(1 << 8);
        const int targetindex = serverblockworldindex(cell);
        if(targetindex < 0 || getworldcubesupportdistance(targetindex) <= 0) return 0;
        nodes.add(serversupportsearchnode(cell, 0, targetindex));
        indexes.access(cell, 0);
        for(int cursor = 0; cursor < nodes.length(); ++cursor)
        {
            const serversupportsearchnode node = nodes[cursor];
            if(node.reach >= maxdistance) continue;
            loopi(6)
            {
                const ivec neighbor = ivec(node.cell).add(ivec(directions[i][0], directions[i][1], directions[i][2]).mul(SERVER_WORLD_BLOCK_SIZE));
                const int worldindex = serverblockworldindex(neighbor);
                if(worldindex < 0 || getworldcubesupportdistance(worldindex) <= 0 || indexes.access(neighbor)) continue;
                indexes.access(neighbor, nodes.length());
                nodes.add(serversupportsearchnode(neighbor, node.reach + 1, worldindex));
            }
        }

        vector<int> frontier;
        loopv(nodes)
        {
            serversupportsearchnode &node = nodes[i];
            loopj(6)
            {
                const ivec neighbor = ivec(node.cell).add(ivec(directions[j][0], directions[j][1], directions[j][2]).mul(SERVER_WORLD_BLOCK_SIZE));
                const int worldindex = serverblockworldindex(neighbor);
                if(worldindex >= 0 && getworldcubesupportdistance(worldindex) <= 0)
                {
                    node.distance = 1;
                    frontier.add(i);
                    break;
                }
            }
        }
        for(int cursor = 0; cursor < frontier.length(); ++cursor)
        {
            const int nodeindex = frontier[cursor], distance = nodes[nodeindex].distance + 1;
            const ivec origin = nodes[nodeindex].cell;
            loopi(6)
            {
                const ivec neighbor = ivec(origin).add(ivec(directions[i][0], directions[i][1], directions[i][2]).mul(SERVER_WORLD_BLOCK_SIZE));
                int *index = indexes.access(neighbor);
                if(!index || distance > getworldcubesupportdistance(nodes[*index].worldindex) ||
                   (nodes[*index].distance && nodes[*index].distance <= distance))
                    continue;
                nodes[*index].distance = distance;
                frontier.add(*index);
            }
        }
        return nodes[0].distance <= maxdistance ? nodes[0].distance : 0;
    }

    static void removeserversupportcell(const ivec &cell)
    {
        const ivec key = cell;
        serversupportcell *state = serversupportcells.access(key);
        if(!state) return;
        const int unsupportedindex = state->unsupportedindex, unsupportedlast = serverunsupportedpositions.length() - 1;
        if(unsupportedindex >= 0 && unsupportedindex <= unsupportedlast)
        {
            if(unsupportedindex != unsupportedlast)
            {
                serverunsupportedpositions[unsupportedindex] = serverunsupportedpositions[unsupportedlast];
                serversupportcell *moved = serversupportcells.access(serverunsupportedpositions[unsupportedindex]);
                if(moved) moved->unsupportedindex = unsupportedindex;
            }
            serverunsupportedpositions.setsize(unsupportedlast);
        }
        const int index = state->index, last = serversupportpositions.length() - 1;
        if(index >= 0 && index <= last)
        {
            if(index != last)
            {
                serversupportpositions[index] = serversupportpositions[last];
                serversupportcell *moved = serversupportcells.access(serversupportpositions[index]);
                if(moved) moved->index = index;
            }
            serversupportpositions.setsize(last);
        }
        serversupportcells.remove(key);
    }

    static bool serversupportpersistent(const ivec &cell)
    {
        const int item = serverblockitem(cell), index = getworlditemtype(item) == WORLD_ITEM_CUBE ? getworlditemindex(item) : -1;
        return index >= 0 && getworldcubesupportpersistentonplace(index);
    }

    static void updateserverunsupportedstate(const ivec &cell, serversupportcell &state)
    {
        const bool unsupported = state.distance <= 0 && !serversupportpersistent(cell);
        if(unsupported == (state.unsupportedindex >= 0)) return;
        if(unsupported)
        {
            state.unsupportedindex = serverunsupportedpositions.length();
            serverunsupportedpositions.add(cell);
            return;
        }
        const int index = state.unsupportedindex, last = serverunsupportedpositions.length() - 1;
        if(index >= 0 && index <= last)
        {
            if(index != last)
            {
                serverunsupportedpositions[index] = serverunsupportedpositions[last];
                serversupportcell *moved = serversupportcells.access(serverunsupportedpositions[index]);
                if(moved) moved->unsupportedindex = index;
            }
            serverunsupportedpositions.setsize(last);
        }
        state.unsupportedindex = -1;
    }

    static void updateserversupportcell(const serversupportcheck &check)
    {
        static const int directions[6][3] =
        {
            { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 },
            { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
        };
        const int worldindex = serverblockworldindex(check.cell), maxdistance = getworldcubesupportdistance(worldindex);
        serversupportcell *state = serversupportcells.access(check.cell);
        if(maxdistance <= 0)
        {
            if(state) removeserversupportcell(check.cell);
            return;
        }
        const int distance = serversupportdistance(check.cell, maxdistance), previous = state ? state->distance : -1;
        if(!state)
        {
            const int index = serversupportpositions.length();
            serversupportpositions.add(check.cell);
            state = &serversupportcells.access(check.cell, serversupportcell(distance, index));
        }
        else state->distance = distance;
        updateserverunsupportedstate(check.cell, *state);
        if(previous == distance || check.remaining <= 0) return;
        loopi(6)
            queueserversupportcheck(ivec(check.cell).add(ivec(directions[i][0], directions[i][1], directions[i][2]).mul(SERVER_WORLD_BLOCK_SIZE)),
                                    check.remaining - 1);
    }

    static void randomtickserversupportblock(const ivec &cell)
    {
        serversupportcell *state = serversupportcells.access(cell);
        if(!state || state->distance > 0 || !serverfallblockinrange(cell) || serversupportpersistent(cell)) return;
        const int item = serverblockitem(cell), worldindex = getworlditemtype(item) == WORLD_ITEM_CUBE ? getworlditemindex(item) : -1;
        if(item < 0 || !getworldcubesupportdecay(worldindex)) return;
        if(!acceptsystemworldaction(WORLD_ACTION_BREAK_CUBE_START, cell, item)) return;
        addworlddrops(NULL, worldeditrevision, WORLD_ACTION_BREAK_CUBE_START, cell, WORLD_ORIENT_TOP, item);
        removeserversupportcell(cell);
    }

    static void updateserversupportblocks()
    {
        int discoverybudget = 8;
        loopv(servercollisionchunks)
        {
            servercollisionchunk &chunk = *servercollisionchunks[i];
            while(discoverybudget > 0 && chunk.supportscan < SERVER_WORLD_CHUNK_BLOCKS * SERVER_WORLD_CHUNK_BLOCKS)
            {
                const int column = chunk.supportscan++, localx = column % SERVER_WORLD_CHUNK_BLOCKS,
                          localy = column / SERVER_WORLD_CHUNK_BLOCKS,
                          blockx = chunk.x * SERVER_WORLD_CHUNK_BLOCKS + localx,
                          blocky = chunk.y * SERVER_WORLD_CHUNK_BLOCKS + localy,
                          surface = SERVER_WORLD_GROUND_HEIGHT + chunk.heights[column] * SERVER_WORLD_BLOCK_SIZE;
                loopj(10)
                {
                    const ivec cell(blockx * SERVER_WORLD_BLOCK_SIZE, blocky * SERVER_WORLD_BLOCK_SIZE, surface + j * SERVER_WORLD_BLOCK_SIZE);
                    const int worldindex = serverblockworldindex(cell), distance = getworldcubesupportdistance(worldindex);
                    if(distance > 0) queueserversupportcheck(cell, distance);
                }
                --discoverybudget;
            }
            if(discoverybudget <= 0) break;
        }

        const int checks = min(serversupportchecks.length(), 64);
        loopi(checks)
        {
            const serversupportcheck check = serversupportchecks.remove(0);
            if(!serverfallblockinrange(check.cell))
            {
                queueserversupportcheck(check.cell, check.remaining);
                continue;
            }
            updateserversupportcell(check);
        }

        static int lastrandomtick = 0;
        if(totalmillis - lastrandomtick < serversupportdecaymillis || serverunsupportedpositions.empty()) return;
        lastrandomtick = totalmillis;
        int players = 0;
        loopv(clients) if(clients[i] && clients[i]->connected && clients[i]->worldready && clients[i]->hasposition) ++players;
        loopi(max(players * 3, 1))
        {
            if(serverunsupportedpositions.empty()) break;
            randomtickserversupportblock(serverunsupportedpositions[rnd(serverunsupportedpositions.length())]);
        }
    }

    static bool startserverfallingblock(const ivec &cell, int item)
    {
        if(!acceptsystemworldaction(WORLD_ACTION_BREAK_CUBE_START, cell, item)) return false;
        serverfallingblock *block = new serverfallingblock;
        if(!nextfallblockid || nextfallblockid > uint(INT_MAX)) nextfallblockid = 1;
        block->id = nextfallblockid++;
        block->item = item;
        block->origin = cell;
        block->o = vec(cell).add(SERVER_WORLD_BLOCK_SIZE / 2);
        block->lastupdate = totalmillis;
        serverfallingblocks.add(block);
        broadcastfallblockspawn(*block);
        return true;
    }

    static float serverfallblocklanding(const serverfallingblock &block)
    {
        int z = block.origin.z - SERVER_WORLD_BLOCK_SIZE;
        for(; z >= -SERVER_WORLD_BLOCK_SIZE; z -= SERVER_WORLD_BLOCK_SIZE)
            if(serverblocksolid(ivec(block.origin.x, block.origin.y, z))) return z + SERVER_WORLD_BLOCK_SIZE * 1.5f;
        return SERVER_WORLD_BLOCK_SIZE / 2.0f;
    }

    static bool updateserverfallingblock(serverfallingblock &block)
    {
        const int elapsedmillis = min(max(totalmillis - block.lastupdate, 0), 100);
        block.lastupdate = totalmillis;
        if(!elapsedmillis) return false;
        const float seconds = elapsedmillis / 1000.0f, previousvelocity = block.velocity;
        block.velocity += 210.0f * seconds;
        const float distance = (previousvelocity + block.velocity) * 0.5f * seconds,
                    landing = serverfallblocklanding(block);
        if(block.o.z - distance > landing)
        {
            block.o.z -= distance;
            return false;
        }
        const ivec destination(block.origin.x, block.origin.y, int(landing) - SERVER_WORLD_BLOCK_SIZE / 2);
        if(serverblocksolid(destination)) return false;
        if(!acceptsystemworldaction(WORLD_ACTION_PLACE_CUBE, destination, block.item)) return false;
        queueserverfallblockcheck(ivec(block.origin).add(ivec(0, 0, SERVER_WORLD_BLOCK_SIZE)));
        queueserverfallblockcheck(ivec(destination).add(ivec(0, 0, SERVER_WORLD_BLOCK_SIZE)));
        return true;
    }

    static void sendserverfallblocksnapshots()
    {
        static int lastsnapshot = 0;
        if(totalmillis - lastsnapshot < 50) return;
        lastsnapshot = totalmillis;
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(!ci || !ci->connected || !ci->worldready) continue;
            loopvj(serverfallingblocks)
            {
                const serverfallingblock &block = *serverfallingblocks[j];
                packetbuf p(48);
                putint(p, N_FALLBLOCKUPDATE); putint(p, int(block.id)); putint(p, totalmillis);
                putint(p, int(block.o.x * DMF)); putint(p, int(block.o.y * DMF)); putint(p, int(block.o.z * DMF));
                putint(p, int(block.velocity * DNF));
                sendpacket(ci->clientnum, 0, p.finalize());
            }
        }
    }

    static void updateserverfallingblocks()
    {
        for(int i = serverfallingblocks.length() - 1; i >= 0; --i)
            if(updateserverfallingblock(*serverfallingblocks[i])) removeserverfallingblock(i);

        const int checks = min(serverfallblockchecks.length(), 64);
        loopi(checks)
        {
            const ivec cell = serverfallblockchecks.remove(0);
            if(!serverfallblockinrange(cell))
            {
                serverfallblockchecks.add(cell);
                continue;
            }
            const int item = serverblockitem(cell), worldindex = getworlditemtype(item) == WORLD_ITEM_CUBE ? getworlditemindex(item) : -1;
            if(item < 0 || !getworldcubefall(worldindex)) continue;
            if(!serverblocksolid(ivec(cell).sub(ivec(0, 0, SERVER_WORLD_BLOCK_SIZE)))) startserverfallingblock(cell, item);
            else if(serverfallingblockbelow(cell)) queueserverfallblockcheck(cell);
        }
        sendserverfallblocksnapshots();
    }

    static bool actiontargetoutofreach(const clientinfo &ci, const ivec &target)
    {
        if(servercreative()) return vec(target.x + 8.0f, target.y + 8.0f, target.z + 8.0f).dist(ci.o) > buildreach;

        const vec origin = vec(ci.o).addz(28.0f), minimum(target), maximum = vec(target).add(SERVER_WORLD_BLOCK_SIZE);
        float squareddistance = 0.0f;
        loopi(3)
        {
            const float offset = origin[i] < minimum[i] ? minimum[i] - origin[i] : origin[i] > maximum[i] ? origin[i] - maximum[i] : 0.0f;
            squareddistance += offset * offset;
        }
        return squareddistance > game::SURVIVAL_BUILD_REACH * game::SURVIVAL_BUILD_REACH;
    }

    static bool validactiontarget(const clientinfo &ci, const ivec &target, int orient, const char *&error)
    {
        if(orient < 0 || orient > 5 || target.x % 16 || target.y % 16 || target.z % 16 ||
           target.z < 0 || target.z > (1 << 13) - 16)
        {
            error = "invalid or unaligned world target";
            return false;
        }
        if(!ci.hasposition)
        {
            error = "a valid player position is required";
            return false;
        }
        if(actiontargetoutofreach(ci, target))
        {
            error = "world target is outside allowed range";
            return false;
        }
        if(servernpcinterceptsaction(ci, target))
        {
            error = "world target is occluded by an NPC";
            return false;
        }
        return true;
    }

    static bool playeroccupies(const ivec &target)
    {
        const vec cellmin(target), cellmax = vec(target).add(16);
        loopv(clients)
        {
            clientinfo *other = clients[i];
            if(!other || !other->connected || !other->hasposition) continue;
            const vec playermin(other->o.x - 6, other->o.y - 6, other->o.z),
                      playermax(other->o.x + 6, other->o.y + 6, other->o.z + 30);
            if(cellmin.x < playermax.x && cellmax.x > playermin.x &&
               cellmin.y < playermax.y && cellmax.y > playermin.y &&
               cellmin.z < playermax.z && cellmax.z > playermin.z)
                return true;
        }
        return false;
    }

    static bool actionrate(clientinfo &ci, bool placement)
    {
        const int now = max(totalmillis, 1);
        if(!ci.actionwindow || now - ci.actionwindow >= 1000)
        {
            ci.actionwindow = now;
            ci.placements = ci.destructions = 0;
        }
        int &count = placement ? ci.placements : ci.destructions;
        const int limit = placement ? placementratelimit : destructionratelimit;
        return ++count <= limit;
    }

    static bool validactionitem(int action, int item)
    {
        if(item < 0)
            return action == WORLD_ACTION_BREAK_SCATTER_START;
        if(item >= numinventoryitems()) return false;
        const int type = getworlditemtype(item);
        if(action == WORLD_ACTION_PLACE_CUBE || action == WORLD_ACTION_BREAK_CUBE_START) return type == WORLD_ITEM_CUBE;
        if(action == WORLD_ACTION_PLACE_SCATTER) return type == WORLD_ITEM_SCATTER;
        if(action == WORLD_ACTION_BREAK_SCATTER_START) return type == WORLD_ITEM_SCATTER || type == WORLD_ITEM_PLACEABLE;
        if(action == WORLD_ACTION_PLACE_ITEM) return type == WORLD_ITEM_PLACEABLE;
        return false;
    }

    static bool validnewrequest(clientinfo &ci, uint requestid, const char *&error)
    {
        if(!requestid)
        {
            error = "zero action request ID";
            return false;
        }
        if(requestid <= ci.lastrequestid)
        {
            error = requestid == ci.lastrequestid ? "duplicate action request ID" : "stale action request ID";
            return false;
        }
        ci.lastrequestid = requestid;
        return true;
    }

    static void sendbreakstate(const clientinfo &ci, int phase, int stage)
    {
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_BREAKSTATE);
        putint(p, ci.clientnum);
        putint(p, int(ci.breakrequestid));
        putint(p, phase);
        putint(p, ci.breakaction);
        putint(p, ci.breaktarget.x);
        putint(p, ci.breaktarget.y);
        putint(p, ci.breaktarget.z);
        putint(p, ci.breakorient);
        putint(p, stage);
        ENetPacket *packet = p.finalize();
        packet->referenceCount++;
        loopv(clients)
        {
            clientinfo *recipient = clients[i];
            if(!recipient || !recipient->connected || !recipient->worldready || !recipient->hasposition) continue;
            if(vec(ci.breaktarget.x + 8.0f, ci.breaktarget.y + 8.0f, ci.breaktarget.z + 8.0f).dist(recipient->o) <= breaknetworkrange)
                sendpacket(recipient->clientnum, 1, packet);
        }
        if(--packet->referenceCount == 0) enet_packet_destroy(packet);
    }

    static void cancelbreak(clientinfo &ci, bool broadcast)
    {
        if(!ci.breakactive) return;
        if(broadcast) sendbreakstate(ci, BREAK_STATE_CANCEL, 0);
        ci.breakactive = false;
        ci.breakrequestid = 0;
        ci.breakaction = -1;
        ci.breakitem = -1;
        ci.breakrelease = 0;
        ci.breakduration = 0;
        ci.breaktoolitem = -1;
        ci.breaktoolslot = -1;
        ci.breaktooldurability = 0;
        ci.breakdropeligible = true;
    }

    static bool acceptworldaction(clientinfo &ci, uint requestid, int action, const ivec &target, int orient, int item)
    {
        const ivec cell = worldactionstatecell(target, action, worldmountorient(orient));
        if(action == WORLD_ACTION_PLACE_CUBE || action == WORLD_ACTION_BREAK_CUBE_START)
        {
            if(!updateserverchunkblock(cell, action, item)) return false;
        }
        else
        {
            serverchunk *chunk = serverchunkforcell(cell, true);
            if(!chunk || chunk->loading || chunk->corrupted) return false;
            dirtyserverchunk(*chunk);
        }
        serveredit *edit = new serveredit;
        edit->author = ci.clientnum;
        edit->requestid = requestid;
        edit->type = N_WORLDAUTH;
        copystring(edit->ownerid, ci.playerid);
        packetbuf payload(MAXTRANS);
        putint(payload, action);
        putint(payload, target.x); putint(payload, target.y); putint(payload, target.z);
        putint(payload, orient);
        putpersistentid(payload, getinventoryitempersistentid(item));
        edit->payload.put(payload.buf, payload.length());
        return acceptededit(edit);
    }

    static void sendworldcorrection(clientinfo &ci, const serverworldaction &state)
    {
        serveredit correction;
        correction.author = -1;
        correction.revision = worldeditrevision;
        correction.type = N_WORLDAUTH;
        int action = state.action;
        ivec target = state.target;
        if(worldactionusessupport(action))
            target[state.orient >> 1] += state.orient&1 ? -16 : 16;
        packetbuf payload(MAXTRANS);
        putint(payload, action);
        putint(payload, target.x); putint(payload, target.y); putint(payload, target.z);
        putint(payload, state.orient);
        putpersistentid(payload, getinventoryitempersistentid(state.item));
        correction.payload.put(payload.buf, payload.length());
        sendserveredit(ci.clientnum, correction);
    }

    static bool handleplacement(clientinfo &ci, uint requestid, int action, const ivec &support, int orient, int item, int slot)
    {
        const char *error = NULL;
        if(!validnewrequest(ci, requestid, error)) return rejectaction(ci, requestid, error, requestid == ci.lastrequestid);
        if(action != WORLD_ACTION_PLACE_CUBE && action != WORLD_ACTION_PLACE_SCATTER && action != WORLD_ACTION_PLACE_ITEM)
            return rejectaction(ci, requestid, "invalid placement action", true, true);
        const int packed = orient;
        orient = worldmountorient(orient);
        int chestslots = 0;
        const bool chest = action == WORLD_ACTION_PLACE_ITEM && getworldchestconfig(item, chestslots);
        if(packed < 0 || packed >= 24 || (!chest && packed != orient) || (chest && orient != WORLD_ORIENT_TOP))
            return rejectaction(ci, requestid, "invalid placement orientation", true, true);
        if(!validactiontarget(ci, support, orient, error)) return rejectaction(ci, requestid, error, true);
        const ivec occupied = worldactionstatecell(support, action, orient);
        if(!validactiontarget(ci, occupied, orient, error)) return rejectaction(ci, requestid, error, true);
        if(!actionrate(ci, true)) return rejectaction(ci, requestid, "excessive placement rate", true);
        if((action != WORLD_ACTION_PLACE_ITEM || chest) && (playeroccupies(occupied) || serverfallingblockoccupies(occupied)))
            return rejectaction(ci, requestid, "");
        const serverworldaction cubestate = *findworldaction(occupied, WORLD_ACTION_PLACE_CUBE),
                                scatterstate = *findworldaction(occupied, WORLD_ACTION_PLACE_SCATTER);
        const serverworldaction *state = cubestate.action == WORLD_ACTION_PLACE_CUBE ? &cubestate
                                        : scatterstate.action == WORLD_ACTION_PLACE_SCATTER || scatterstate.action == WORLD_ACTION_PLACE_ITEM
                                          ? &scatterstate : NULL;
        if(state)
        {
            rejectaction(ci, requestid, "placement target is already occupied");
            sendworldcorrection(ci, *state);
            return false;
        }
        if(!servercreative())
        {
            if(slot < 0 || slot >= SURVIVAL_HOTBAR_SLOTS || slot != ci.selectedslot || ci.inventorycounts[slot] <= 0 ||
               ci.inventorycounts[slot] > getinventoryitemmaxstack(item) || ci.inventoryitems[slot] != item)
                return rejectaction(ci, requestid, "placed item is not owned in the requested inventory slot", true, true);
        }
        if(!validactionitem(action, item))
            return rejectaction(ci, requestid, "invalid placed item type", true, true);
        if(!acceptworldaction(ci, requestid, action, support, packed, item))
            return rejectaction(ci, requestid, "server could not persist the placement");
        setworldactionstate(occupied, action, orient, item, true);
        if(chest && !findserverchest(occupied))
        {
            serverchests.add(new chestinstance(occupied, item, chestslots, worldplaceyaw(packed)));
            dirtyservergameplaycell(occupied);
        }
        if(!servercreative())
        {
            if(--ci.inventorycounts[slot] <= 0)
            {
                ci.inventoryitems[slot] = -1;
                ci.inventorycounts[slot] = 0;
                ci.inventorydurabilities[slot] = 0;
            }
            markinventorydirty(ci);
        }
        sendactionresult(ci, requestid, true);
        sendinventory(ci);
        return true;
    }

    static bool handlecornerpush(clientinfo &ci, uint requestid, const ivec &target, int packedorient, int item, int slot)
    {
        const char *error = NULL;
        if(!validnewrequest(ci, requestid, error)) return rejectaction(ci, requestid, error, requestid == ci.lastrequestid);
        if(packedorient < 0 || packedorient >= 24) return rejectaction(ci, requestid, "invalid pushed corner", true, true);
        const int orient = packedorient % 6, corner = packedorient / 6;
        if(!validactiontarget(ci, target, orient, error)) return rejectaction(ci, requestid, error, true);
        if(!actionrate(ci, false)) return rejectaction(ci, requestid, "excessive shaping rate", true);

        if(servercreative() || slot < 0 || slot >= SURVIVAL_HOTBAR_SLOTS || slot != ci.selectedslot || ci.inventorycounts[slot] != 1 ||
           ci.inventorydurabilities[slot] <= 0 || getinventorytoolcornerpush(ci.inventoryitems[slot]) == TOOL_CORNER_PUSH_NONE)
            return rejectaction(ci, requestid, "a shaping tool is not owned in the selected inventory slot", true, true);
        const int tool = ci.inventoryitems[slot];

        const int claimedworldindex = getworlditemtype(item) == WORLD_ITEM_CUBE ? getworlditemindex(item) : -1,
                  blockitem = serverblockitem(target), authoritativeworldindex = getworlditemtype(blockitem) == WORLD_ITEM_CUBE
                                                       ? getworlditemindex(blockitem) : -1;
        if(claimedworldindex < 0 || !isworldcubepushable(claimedworldindex, tool) ||
           (authoritativeworldindex >= 0 && blockitem != item) ||
           (authoritativeworldindex >= 0 && !isworldcubepushable(authoritativeworldindex, tool)))
            return rejectaction(ci, requestid, "target cube cannot be pushed");
        if(servercornerpushcount(target, orient, corner) >= 8)
            return rejectaction(ci, requestid, "target corner is already fully pushed");

        serveredit *edit = new serveredit;
        edit->author = ci.clientnum;
        edit->requestid = requestid;
        edit->type = N_EDITF;
        edit->hasselection = true;
        copystring(edit->ownerid, ci.playerid);
        edit->selection.o = target;
        edit->selection.s = ivec(1, 1, 1);
        edit->selection.grid = SERVER_WORLD_BLOCK_SIZE;
        edit->selection.orient = orient;
        edit->selection.cx = -1;
        edit->selection.cy = 0;
        edit->selection.cxs = edit->selection.cys = 2;
        edit->selection.corner = corner;
        packetbuf payload(MAXTRANS);
        putint(payload, target.x); putint(payload, target.y); putint(payload, target.z);
        putint(payload, 1); putint(payload, 1); putint(payload, 1);
        putint(payload, SERVER_WORLD_BLOCK_SIZE); putint(payload, orient);
        putint(payload, -1); putint(payload, 2); putint(payload, 0); putint(payload, 2); putint(payload, corner);
        putint(payload, 1); putint(payload, 2);
        edit->payload.put(payload.buf, payload.length());
        if(!updateserverchunkcorner(target, orient, corner))
        {
            delete edit;
            return rejectaction(ci, requestid, "server could not persist the corner push");
        }
        if(!acceptededit(edit)) return rejectaction(ci, requestid, "server could not persist the corner push");

        if(ci.breakactive) cancelbreak(ci);
        if(--ci.inventorydurabilities[slot] <= 0)
        {
            ci.inventoryitems[slot] = -1;
            ci.inventorycounts[slot] = ci.inventorydurabilities[slot] = 0;
        }
        markinventorydirty(ci);
        sendinventory(ci);
        sendactionresult(ci, requestid, true);
        return true;
    }

    static bool beginbreak(clientinfo &ci, uint requestid, int action, const ivec &target, int orient, int item)
    {
        const char *error = NULL;
        if(!validnewrequest(ci, requestid, error))
            return rejectaction(ci, requestid, error, requestid == ci.lastrequestid);
        if(action != WORLD_ACTION_BREAK_CUBE_START && action != WORLD_ACTION_BREAK_SCATTER_START)
            return rejectaction(ci, requestid, "invalid destruction action", true, true);
        if(!validactiontarget(ci, target, orient, error)) return rejectaction(ci, requestid, error, true);
        if(!validactionitem(action, item) ||
           (item < 0 && getworldscatterindexat(target, orient) < 0))
            return rejectaction(ci, requestid, "invalid destroyed item type", true, true);
        if(ci.breakactive) cancelbreak(ci);
        const ivec occupied = worldactionstatecell(target, action, orient);
        serverworldaction *state = findworldaction(occupied, action);
        if(state && (state->action == WORLD_ACTION_BREAK_CUBE_START || state->action == WORLD_ACTION_BREAK_SCATTER_START))
        {
            rejectaction(ci, requestid, "destruction target is already absent");
            sendworldcorrection(ci, *state);
            return false;
        }
        if(state && state->item != item)
        {
            rejectaction(ci, requestid, "destruction target does not match authoritative world state");
            sendworldcorrection(ci, *state);
            return false;
        }
        ci.breakactive = true;
        ci.breakrequestid = requestid;
        ci.breakaction = action;
        ci.breaktarget = target;
        ci.breakorient = orient;
        ci.breakitem = state ? state->item : item;
        ci.breaktoolslot = ci.selectedslot;
        ci.breaktoolitem = ci.breaktoolslot >= 0 && ci.breaktoolslot < SURVIVAL_HOTBAR_SLOTS && ci.inventorycounts[ci.breaktoolslot] > 0 &&
                           isinventorytool(ci.inventoryitems[ci.breaktoolslot]) && ci.inventorydurabilities[ci.breaktoolslot] > 0
                         ? ci.inventoryitems[ci.breaktoolslot] : -1;
        ci.breaktooldurability = ci.breaktoolitem >= 0 ? ci.inventorydurabilities[ci.breaktoolslot] : 0;
        const int type = getworlditemtype(ci.breakitem), index = getworlditemindex(ci.breakitem);
        if(ci.breaktoolitem < 0 && !isworldobjecthandbreakable(type, index))
        {
            cancelbreak(ci, false);
            return rejectaction(ci, requestid, "target cannot be broken by hand");
        }
        ci.breakduration = action == WORLD_ACTION_BREAK_SCATTER_START && index < 0
                         ? survivalscatterbreakmillis : getworldbreakmillis(type, index, ci.breaktoolitem);
        ci.breakdropeligible = getworlddropeligible(type, index, ci.breaktoolitem);
        ci.breakstage = 0;
        ci.breakrelease = 0;
        ci.breakstart = ci.breakupdate = max(totalmillis, 1);
        sendbreakstate(ci, BREAK_STATE_START, 0);
        return true;
    }

    static bool updatebreak(clientinfo &ci, uint requestid, const ivec &target, int orient, int stage)
    {
        if(!ci.breakactive || requestid != ci.breakrequestid) return rejectaction(ci, requestid, "no matching break action is active");
        if(target != ci.breaktarget || (ci.breakaction == WORLD_ACTION_BREAK_SCATTER_START && orient != ci.breakorient))
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "break target changed; the previous action was cancelled");
        }
        if(stage < 0 || stage >= 8)
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "invalid break progress stage", true, true);
        }
        const int duration = max(ci.breakduration, 1),
                  elapsed = max(totalmillis - ci.breakstart, 0),
                  allowedstage = min(7, elapsed * 8 / max(duration, 1) + 1),
                  tolerance = min(breaktimetolerance, max(duration / 4, 25));
        if(stage <= ci.breakstage) return true;
        const int toleratedstage = min(7, (elapsed + tolerance) * 8 / max(duration, 1) + 1);
        if(!servercreative() && stage > max(allowedstage, toleratedstage))
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "break progress advanced faster than configured", true);
        }
        ci.breakupdate = max(totalmillis, 1);
        ci.breakstage = stage;
        sendbreakstate(ci, BREAK_STATE_UPDATE, stage);
        return true;
    }

    static bool completebreak(clientinfo &ci, uint requestid, const ivec &target, int orient, int item)
    {
        if(!ci.breakactive || requestid != ci.breakrequestid) return rejectaction(ci, requestid, "no matching break action is active");
        if(target != ci.breaktarget || (ci.breakaction == WORLD_ACTION_BREAK_SCATTER_START && orient != ci.breakorient))
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "break completion target changed; the previous action was cancelled");
        }
        if(item != ci.breakitem)
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "break completion item does not match its start", true, true);
        }
        const char *targeterror = NULL;
        if(!validactiontarget(ci, target, orient, targeterror))
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, targeterror ? targeterror : "break target is out of range");
        }
        const bool toolvalid = ci.breaktoolitem < 0 ||
                               (ci.breaktoolslot == ci.selectedslot && ci.breaktoolslot >= 0 && ci.breaktoolslot < SURVIVAL_HOTBAR_SLOTS &&
                                ci.inventoryitems[ci.breaktoolslot] == ci.breaktoolitem && ci.inventorycounts[ci.breaktoolslot] == 1 &&
                                ci.inventorydurabilities[ci.breaktoolslot] == ci.breaktooldurability);
        if(!toolvalid)
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "held tool changed while breaking");
        }
        const ivec occupied = worldactionstatecell(target, ci.breakaction, ci.breakorient);
        serverworldaction *state = findworldaction(occupied, ci.breakaction);
        if(state && (state->action == WORLD_ACTION_BREAK_CUBE_START || state->action == WORLD_ACTION_BREAK_SCATTER_START))
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "destruction target became stale", false, false, true);
        }
        if(state && state->item != item)
        {
            cancelbreak(ci);
            rejectaction(ci, requestid, "destruction target changed while breaking");
            sendworldcorrection(ci, *state);
            return false;
        }
        const int duration = max(ci.breakduration, 1);
        const int tolerance = min(breaktimetolerance, max(duration / 4, 25));
        if(!servercreative() && totalmillis - ci.breakstart + tolerance < duration)
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "target was broken faster than configured", true);
        }
        if(!actionrate(ci, false))
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "excessive destruction rate", true);
        }
        const int action = ci.breakaction;
        if(!acceptworldaction(ci, requestid, action, target, ci.breakorient, item))
        {
            cancelbreak(ci);
            return rejectaction(ci, requestid, "server could not persist the destruction");
        }
        setworldactionstate(occupied, action, ci.breakorient, item);
        removeserverfurnace(occupied, &ci);
        removeserverchest(occupied, &ci);
        if(!servercreative() && ci.breakdropeligible) addworlddrops(&ci, requestid, action, target, ci.breakorient, item);
        if(!servercreative() && ci.breaktoolitem >= 0)
        {
            const int type = getworlditemtype(item), index = getworlditemindex(item), wear = getworldbreaktoolwear(type, index, ci.breaktoolitem);
            ci.inventorydurabilities[ci.breaktoolslot] = max(ci.inventorydurabilities[ci.breaktoolslot] - wear, 0);
            if(ci.inventorydurabilities[ci.breaktoolslot] <= 0)
            {
                ci.inventoryitems[ci.breaktoolslot] = -1;
                ci.inventorycounts[ci.breaktoolslot] = ci.inventorydurabilities[ci.breaktoolslot] = 0;
            }
            markinventorydirty(ci);
            sendinventory(ci);
        }
        sendbreakstate(ci, BREAK_STATE_COMPLETE, 7);
        ci.breakactive = false;
        ci.breakrequestid = 0;
        sendactionresult(ci, requestid, true);
        return true;
    }

    static bool handleworldaction(clientinfo &ci, uint requestid, int action, const ivec &target, int orient, int item, int slot)
    {
        if(action == WORLD_ACTION_PUSH_CORNER) return handlecornerpush(ci, requestid, target, orient, item, slot);
        switch(action)
        {
            case WORLD_ACTION_PLACE_CUBE:
            case WORLD_ACTION_PLACE_SCATTER:
            case WORLD_ACTION_PLACE_ITEM:
                return handleplacement(ci, requestid, action, target, orient, item, slot);
            case WORLD_ACTION_BREAK_CUBE_START:
            case WORLD_ACTION_BREAK_SCATTER_START:
                return beginbreak(ci, requestid, action, target, orient, item);
            case WORLD_ACTION_BREAK_UPDATE:
                return updatebreak(ci, requestid, target, orient, slot);
            case WORLD_ACTION_BREAK_CANCEL:
                if(ci.breakactive && ci.breakrequestid == requestid)
                {
                    cancelbreak(ci);
                    sendactionresult(ci, requestid, true);
                    return true;
                }
                return rejectaction(ci, requestid, "no matching break action is active");
            case WORLD_ACTION_BREAK_COMPLETE:
                return completebreak(ci, requestid, target, orient, item);
            default:
                return rejectaction(ci, requestid, "unknown world action", true, true);
        }
    }

    static int findserverdrop(uint id)
    {
        loopv(serverdrops) if(serverdrops[i]->id == id) return i;
        return -1;
    }

    static bool handledroppickup(clientinfo &ci, uint requestid, uint dropid, const vec &position)
    {
        const char *error = NULL;
        if(!validnewrequest(ci, requestid, error)) return rejectaction(ci, requestid, error, requestid == ci.lastrequestid);
        if(!ci.worldready || !ci.hasposition) return rejectaction(ci, requestid, "a synchronized player position is required to pick up drops");
        const int elapsed = max(totalmillis - ci.lastpositionmillis, 1);
        if(position.z < 0 || position.z > (1 << 13) || position.dist(ci.o) > 32.0f + elapsed * 0.5f)
            return rejectaction(ci, requestid, "invalid synchronized pickup position", true);
        const int index = findserverdrop(dropid);
        if(index < 0) return rejectaction(ci, requestid, "drop is no longer available");
        serverdrop &drop = *serverdrops[index];
        if(totalmillis - drop.created < DROP_PICKUP_DELAY) return rejectaction(ci, requestid, "drop is not ready for pickup");
        if(personaldrops && drop.ownerid[0] && strcmp(drop.ownerid, ci.playerid))
            return rejectaction(ci, requestid, "drop belongs to another player");
        // Dedicated servers do not load terrain collision. Clients keep a falling drop's X/Y fixed and only lower its Z.
        const float dx = drop.o.x - position.x, dy = drop.o.y - position.y;
        if(dx * dx + dy * dy > 24.0f * 24.0f || position.z > drop.o.z + 24.0f)
            return rejectaction(ci, requestid, "drop is beyond the 24-unit pickup distance");
        if(!inventoryhasroom(ci, drop.item, drop.count)) return rejectaction(ci, requestid, "inventory is full");
        if(!addinventoryitems(ci, drop.item, drop.count, drop.durability))
            return rejectaction(ci, requestid, "server could not add the drop to inventory");
        removeserverdrop(index, ci.clientnum);
        sendactionresult(ci, requestid, true);
        sendinventory(ci);
        return true;
    }

    static bool handleinventoryaction(clientinfo &ci, uint requestid, int action, int first, int second)
    {
        const char *error = NULL;
        if(!validnewrequest(ci, requestid, error))
            return rejectaction(ci, requestid, error, requestid == ci.lastrequestid);
        if(servercreative()) return rejectaction(ci, requestid, "survival inventory is disabled in creative mode");
        if(ci.breakactive) cancelbreak(ci);
        switch(action)
        {
            case INVENTORY_ACTION_SWAP:
                if(first < 0 || first >= SURVIVAL_USABLE_SLOTS || second < 0 || second >= SURVIVAL_USABLE_SLOTS)
                    return rejectaction(ci, requestid, "invalid inventory slot", true, true);
                swap(ci.inventoryitems[first], ci.inventoryitems[second]);
                swap(ci.inventorycounts[first], ci.inventorycounts[second]);
                swap(ci.inventorydurabilities[first], ci.inventorydurabilities[second]);
                markinventorydirty(ci);
                break;
            case INVENTORY_ACTION_SELECT:
                if(first < 0 || first >= SURVIVAL_HOTBAR_SLOTS)
                    return rejectaction(ci, requestid, "invalid hotbar slot", true, true);
                ci.selectedslot = first;
                markinventorydirty(ci);
                break;
            case INVENTORY_ACTION_CLICK:
                if(first < 0 || first >= SURVIVAL_USABLE_SLOTS ||
                   (second != INVENTORY_CLICK_LEFT && second != INVENTORY_CLICK_RIGHT))
                    return rejectaction(ci, requestid, "invalid inventory click", true, true);
                if(inventoryinstanceclick(ci.inventorycursoritem, ci.inventorycursorcount, ci.inventorycursordurability,
                                          ci.inventoryitems[first], ci.inventorycounts[first], ci.inventorydurabilities[first], second))
                    markinventorydirty(ci);
                break;
            default:
                return rejectaction(ci, requestid, "invalid inventory action", true, true);
        }
        sendactionresult(ci, requestid, true);
        sendinventory(ci);
        return true;
    }

    static bool rejectcraftaction(clientinfo &ci, uint requestid, const char *reason, bool violation = false, bool malicious = false)
    {
        const bool result = rejectaction(ci, requestid, reason, violation, malicious);
        sendinventory(ci);
        sendcraftstate(ci);
        return result;
    }

    static bool handlecraftaction(clientinfo &ci, uint requestid, int action, int first, int second, int third, int fourth)
    {
        (void)fourth;
        const char *error = NULL;
        if(!validnewrequest(ci, requestid, error)) return rejectcraftaction(ci, requestid, error, requestid == ci.lastrequestid);
        if(servercreative()) return rejectcraftaction(ci, requestid, "crafting is disabled in creative mode");
        if(ci.breakactive) cancelbreak(ci);
        if((action == CRAFT_ACTION_OPEN_PLAYER || action == CRAFT_ACTION_OPEN_TABLE) && ci.chestopen) closechest(ci);
        switch(action)
        {
            case CRAFT_ACTION_OPEN_PLAYER:
                ci.craftinggridsize = 2;
                ci.craftingstationitem = -1;
                break;
            case CRAFT_ACTION_OPEN_TABLE:
            {
                const int tableitem = getinventoryitemindex("crafting_table");
                const ivec target(first, second, third);
                serverworldaction *state = findworldaction(target, WORLD_ACTION_PLACE_CUBE);
                if(tableitem < 0 || !state || state->action != WORLD_ACTION_PLACE_CUBE || state->item != tableitem)
                    return rejectcraftaction(ci, requestid, "the requested crafting table does not exist");
                if(!ci.hasposition || vec(first + 8, second + 8, third + 8).dist(ci.o) > 144.0f)
                    return rejectcraftaction(ci, requestid, "the crafting table is out of reach");
                ci.craftinggridsize = 3;
                ci.craftingstationitem = tableitem;
                ci.craftingstationtarget = target;
                break;
            }
            case CRAFT_ACTION_INVENTORY_TO_GRID:
                if(!craftingstationvalid(ci)) return rejectcraftaction(ci, requestid, "the crafting station is no longer accessible");
                if(first < 0 || first >= SURVIVAL_USABLE_SLOTS || second < 0 || second >= ci.craftinggridsize * ci.craftinggridsize)
                    return rejectcraftaction(ci, requestid, "invalid crafting slot", true, true);
                swap(ci.inventoryitems[first], ci.craftingitems[second]);
                swap(ci.inventorycounts[first], ci.craftingcounts[second]);
                swap(ci.inventorydurabilities[first], ci.craftingdurabilities[second]);
                markinventorydirty(ci);
                break;
            case CRAFT_ACTION_GRID_TO_INVENTORY:
                if(!craftingstationvalid(ci)) return rejectcraftaction(ci, requestid, "the crafting station is no longer accessible");
                if(first < 0 || first >= ci.craftinggridsize * ci.craftinggridsize || second < 0 || second >= SURVIVAL_USABLE_SLOTS)
                    return rejectcraftaction(ci, requestid, "invalid crafting slot", true, true);
                swap(ci.craftingitems[first], ci.inventoryitems[second]);
                swap(ci.craftingcounts[first], ci.inventorycounts[second]);
                swap(ci.craftingdurabilities[first], ci.inventorydurabilities[second]);
                markinventorydirty(ci);
                break;
            case CRAFT_ACTION_GRID_SWAP:
                if(!craftingstationvalid(ci)) return rejectcraftaction(ci, requestid, "the crafting station is no longer accessible");
                if(first < 0 || first >= ci.craftinggridsize * ci.craftinggridsize || second < 0 || second >= ci.craftinggridsize * ci.craftinggridsize)
                    return rejectcraftaction(ci, requestid, "invalid crafting slot", true, true);
                swap(ci.craftingitems[first], ci.craftingitems[second]);
                swap(ci.craftingcounts[first], ci.craftingcounts[second]);
                swap(ci.craftingdurabilities[first], ci.craftingdurabilities[second]);
                markinventorydirty(ci);
                break;
            case CRAFT_ACTION_CLICK_GRID:
                if(!craftingstationvalid(ci)) return rejectcraftaction(ci, requestid, "the crafting station is no longer accessible");
                if(first < 0 || first >= ci.craftinggridsize * ci.craftinggridsize ||
                   (second != INVENTORY_CLICK_LEFT && second != INVENTORY_CLICK_RIGHT))
                    return rejectcraftaction(ci, requestid, "invalid crafting grid click", true, true);
                if(inventoryinstanceclick(ci.inventorycursoritem, ci.inventorycursorcount, ci.inventorycursordurability,
                                          ci.craftingitems[first], ci.craftingcounts[first], ci.craftingdurabilities[first], second))
                    markinventorydirty(ci);
                break;
            case CRAFT_ACTION_TAKE_OUTPUT:
            {
                if(second < 0 || second >= SURVIVAL_USABLE_SLOTS) return rejectcraftaction(ci, requestid, "invalid output inventory slot", true, true);
                const int outputitem = getcraftrecipeoutputitem(first);
                if(ci.inventorycounts[second] > 0 && ci.inventoryitems[second] != outputitem)
                    return rejectcraftaction(ci, requestid, "the output inventory slot contains another item");
                const int stack = max(getinventoryitemmaxstack(outputitem), 1), capacity = stack - ci.inventorycounts[second];
                craftmatch match;
                if(!servercraftmatch(ci, first, match, capacity))
                    return rejectcraftaction(ci, requestid, "the authoritative crafting grid does not match or fit that recipe");
                loopi(CRAFT_GRID_MAX) if(match.consume[i] > 0)
                {
                    ci.craftingcounts[i] -= match.consume[i];
                    if(ci.craftingcounts[i] <= 0)
                    {
                        ci.craftingitems[i] = -1;
                        ci.craftingcounts[i] = ci.craftingdurabilities[i] = 0;
                    }
                }
                ci.inventoryitems[second] = match.outputitem;
                ci.inventorycounts[second] += match.outputcount;
                ci.inventorydurabilities[second] = getinventorytoolmaxdurability(match.outputitem);
                markinventorydirty(ci);
                break;
            }
            case CRAFT_ACTION_TAKE_OUTPUT_CURSOR:
            {
                if(second != INVENTORY_CLICK_LEFT && second != INVENTORY_CLICK_RIGHT)
                    return rejectcraftaction(ci, requestid, "invalid crafting output click", true, true);
                const int outputitem = getcraftrecipeoutputitem(first);
                if(ci.inventorycursorcount > 0 && ci.inventorycursoritem != outputitem)
                    return rejectcraftaction(ci, requestid, "the cursor contains another item");
                const int stack = max(getinventoryitemmaxstack(outputitem), 1), capacity = stack - ci.inventorycursorcount;
                craftmatch match;
                if(!servercraftmatch(ci, first, match, capacity))
                    return rejectcraftaction(ci, requestid, "the authoritative crafting grid does not match or fit that recipe");
                loopi(CRAFT_GRID_MAX) if(match.consume[i] > 0)
                {
                    ci.craftingcounts[i] -= match.consume[i];
                    if(ci.craftingcounts[i] <= 0)
                    {
                        ci.craftingitems[i] = -1;
                        ci.craftingcounts[i] = ci.craftingdurabilities[i] = 0;
                    }
                }
                ci.inventorycursoritem = match.outputitem;
                ci.inventorycursorcount += match.outputcount;
                ci.inventorycursordurability = getinventorytoolmaxdurability(match.outputitem);
                markinventorydirty(ci);
                break;
            }
            default:
                return rejectcraftaction(ci, requestid, "invalid crafting action", true, true);
        }
        sendactionresult(ci, requestid, true);
        sendinventory(ci);
        sendcraftstate(ci);
        return true;
    }

    static bool rejectfurnaceaction(clientinfo &ci, uint requestid, const char *reason, bool violation = false, bool malicious = false)
    {
        const bool result = rejectaction(ci, requestid, reason, violation, malicious);
        if(ci.furnaceopen)
        {
            furnaceinstance *furnace = findserverfurnace(ci.furnacetarget);
            if(furnace && furnaceaccessible(ci, *furnace)) sendfurnacestate(ci, *furnace, true);
            else closefurnace(ci);
        }
        return result;
    }

    static bool handlefurnaceaction(clientinfo &ci, uint requestid, int action, int first, int second, int third, int fourth)
    {
        (void)fourth;
        const char *error = NULL;
        if(!validnewrequest(ci, requestid, error)) return rejectfurnaceaction(ci, requestid, error, requestid == ci.lastrequestid);
        if(servercreative()) return rejectfurnaceaction(ci, requestid, "furnaces are disabled in creative mode");
        if(ci.breakactive) cancelbreak(ci);
        if(action == FURNACE_ACTION_CLOSE)
        {
            closefurnace(ci);
            sendactionresult(ci, requestid, true);
            return true;
        }
        if(action == FURNACE_ACTION_OPEN)
        {
            const ivec target(first, second, third);
            serverworldaction *state = findworldaction(target, WORLD_ACTION_PLACE_CUBE);
            int inputslots = 0, inputlimit = 0;
            if(!state || state->action != WORLD_ACTION_PLACE_CUBE || !getworldfurnaceconfig(state->item, inputslots, inputlimit))
                return rejectfurnaceaction(ci, requestid, "the requested furnace does not exist");
            if(!ci.hasposition || vec(target).add(8).dist(ci.o) > 144.0f)
                return rejectfurnaceaction(ci, requestid, "the furnace is out of reach");
            furnaceinstance *furnace = findserverfurnace(target);
            if(!furnace)
            {
                furnace = new furnaceinstance(target, state->item, inputslots, inputlimit);
                serverfurnaces.add(furnace);
                dirtyservergameplaycell(target);
            }
            if(ci.chestopen) closechest(ci);
            ci.furnaceopen = true;
            ci.furnacetarget = target;
            sendactionresult(ci, requestid, true);
            sendinventory(ci);
            sendfurnacestate(ci, *furnace, true);
            return true;
        }

        if(!ci.furnaceopen) return rejectfurnaceaction(ci, requestid, "no furnace is open");
        furnaceinstance *furnace = findserverfurnace(ci.furnacetarget);
        if(!furnace || !furnaceaccessible(ci, *furnace)) return rejectfurnaceaction(ci, requestid, "the furnace is no longer accessible");
        bool changed = false;
        switch(action)
        {
            case FURNACE_ACTION_CLICK_INPUT:
                if(first < 0 || first >= furnace->inputslots ||
                   (second != INVENTORY_CLICK_LEFT && second != INVENTORY_CLICK_RIGHT))
                    return rejectfurnaceaction(ci, requestid, "invalid furnace input slot", true, true);
                changed = serverlimitedinventoryclick(ci.inventorycursoritem, ci.inventorycursorcount, ci.inventorycursordurability,
                                                      furnace->inputitems[first], furnace->inputcounts[first],
                                                      furnace->inputdurabilities[first], second,
                                                      min(furnace->inputlimit, max(getinventoryitemmaxstack(ci.inventorycursoritem), 1)));
                break;
            case FURNACE_ACTION_CLICK_FUEL:
                if(first != INVENTORY_CLICK_LEFT && first != INVENTORY_CLICK_RIGHT)
                    return rejectfurnaceaction(ci, requestid, "invalid furnace fuel click", true, true);
                if(ci.inventorycursorcount > 0 && getfurnacefuelmillis(ci.inventorycursoritem) <= 0)
                    return rejectfurnaceaction(ci, requestid, "only configured fuels can enter the fuel slot");
                changed = serverlimitedinventoryclick(ci.inventorycursoritem, ci.inventorycursorcount, ci.inventorycursordurability,
                                                      furnace->fuelitem, furnace->fuelcount, furnace->fueldurability, first,
                                                      max(getinventoryitemmaxstack(ci.inventorycursoritem), 1));
                break;
            case FURNACE_ACTION_CLICK_OUTPUT:
                changed = servertakefurnaceoutput(ci, *furnace, first);
                break;
            case FURNACE_ACTION_BAKE:
                changed = startfurnaceinstance(*furnace);
                break;
            default:
                return rejectfurnaceaction(ci, requestid, "invalid furnace action", true, true);
        }
        if(!changed)
            return rejectfurnaceaction(ci, requestid, action == FURNACE_ACTION_BAKE
                                                          ? "baking requires a valid recipe, output room, and available heat or fuel"
                                                          : "the requested furnace transfer could not be completed");
        bool syncchanged = false;
        updatefurnaceinstance(*furnace, 0, syncchanged);
        dirtyservergameplaycell(furnace->target);
        sendactionresult(ci, requestid, true);
        if(action != FURNACE_ACTION_BAKE)
        {
            markinventorydirty(ci);
            sendinventory(ci);
        }
        syncfurnaceviewers(*furnace);
        return true;
    }

    void servershutdown()
    {
        if(!serverworldinitialized) return;
        shutdownserverchunks(true);
    }

    static bool rejectchestaction(clientinfo &ci, uint requestid, const char *reason, bool violation = false, bool malicious = false)
    {
        const bool result = rejectaction(ci, requestid, reason, violation, malicious);
        if(ci.chestopen)
        {
            chestinstance *chest = findserverchest(ci.chesttarget);
            if(chest && chestaccessible(ci, *chest)) sendcheststate(ci, *chest, true);
            else closechest(ci);
        }
        return result;
    }

    static bool handlechestaction(clientinfo &ci, uint requestid, int action, int first, int second, int third, int fourth)
    {
        (void)fourth;
        const char *error = NULL;
        if(!validnewrequest(ci, requestid, error)) return rejectchestaction(ci, requestid, error, requestid == ci.lastrequestid);
        if(ci.breakactive) cancelbreak(ci);
        if(action == CHEST_ACTION_CLOSE)
        {
            closechest(ci);
            sendactionresult(ci, requestid, true);
            return true;
        }
        if(action == CHEST_ACTION_OPEN)
        {
            const ivec target(first, second, third);
            serverworldaction *state = findworldaction(target, WORLD_ACTION_PLACE_ITEM);
            int slots = 0;
            if(!state || state->action != WORLD_ACTION_PLACE_ITEM || !getworldchestconfig(state->item, slots))
                return rejectchestaction(ci, requestid, "the requested chest does not exist");
            if(!ci.hasposition || vec(target).add(8).dist(ci.o) > 144.0f)
                return rejectchestaction(ci, requestid, "the chest is out of reach");
            chestinstance *chest = findserverchest(target);
            if(!chest)
            {
                chest = new chestinstance(target, state->item, slots, 0);
                serverchests.add(chest);
                dirtyservergameplaycell(target);
            }
            if(ci.chestopen && ci.chesttarget != target) closechest(ci);
            if(ci.furnaceopen) closefurnace(ci);
            const bool animate = !chesthasviewers(target);
            ci.chestopen = true;
            ci.chesttarget = target;
            if(animate) sendchestanimation(target, true);
            sendactionresult(ci, requestid, true);
            sendinventory(ci);
            sendcheststate(ci, *chest, true);
            return true;
        }

        if(!ci.chestopen) return rejectchestaction(ci, requestid, "no chest is open");
        chestinstance *chest = findserverchest(ci.chesttarget);
        if(!chest || !chestaccessible(ci, *chest)) return rejectchestaction(ci, requestid, "the chest is no longer accessible");
        if(action != CHEST_ACTION_CLICK || first < 0 || first >= chest->slots ||
           (second != INVENTORY_CLICK_LEFT && second != INVENTORY_CLICK_RIGHT))
            return rejectchestaction(ci, requestid, "invalid chest slot click", true, true);
        if(!serverlimitedinventoryclick(ci.inventorycursoritem, ci.inventorycursorcount, ci.inventorycursordurability,
                                        chest->items[first], chest->counts[first], chest->durabilities[first], second,
                                        max(getinventoryitemmaxstack(ci.inventorycursoritem), 1)))
            return rejectchestaction(ci, requestid, "the requested chest transfer could not be completed");
        dirtyservergameplaycell(chest->target);
        markinventorydirty(ci);
        sendactionresult(ci, requestid, true);
        sendinventory(ci);
        syncchestviewers(*chest);
        return true;
    }

    static void sendcommandresult(clientinfo &ci, const char *message)
    {
        sendf(ci.clientnum, 1, "ris", N_SERVMSG, message);
    }

    static clientinfo *findconnectedplayer(const char *name)
    {
        if(!name || !name[0]) return NULL;
        loopv(clients)
        {
            clientinfo *candidate = clients[i];
            if(candidate && candidate->connected && !cubecasecmp(candidate->name, name)) return candidate;
        }
        return NULL;
    }

    static void serverworldcommand(clientinfo &ci, const char *request)
    {
        if(ci.privilege < PRIV_ADMIN)
        {
            sendcommandresult(ci, "permission denied: this world command requires admin");
            return;
        }

        string command;
        // Command arguments are separated by whitespace. Preserve and
        // normalize it while stripping other non-printing characters.
        filtertext(command, request ? request : "", true, true, sizeof(command));
        char *args = command;
        while(*args && !iscubespace(*args)) ++args;
        if(*args) *args++ = '\0';
        while(iscubespace(*args)) ++args;

        if(cubecaseequal(command, "give"))
        {
            char *itemid = args;
            while(*args && !iscubespace(*args)) ++args;
            if(*args) *args++ = '\0';
            while(iscubespace(*args)) ++args;

            char *amounttext = args;
            while(*args && !iscubespace(*args)) ++args;
            if(*args) *args++ = '\0';
            while(iscubespace(*args)) ++args;
            char *playername = args;
            char *tail = playername + strlen(playername);
            while(tail > playername && iscubespace(tail[-1])) *--tail = '\0';

            char *end = NULL;
            errno = 0;
            const long amount = strtol(amounttext, &end, 10);
            if(!itemid[0] || end == amounttext || *end || errno == ERANGE || amount <= 0 || amount > INT_MAX)
            {
                sendcommandresult(ci, "usage: /give <item_name> <amount> [player name]");
                return;
            }

            const int item = getinventoryitemindex(itemid);
            if(item < 0)
            {
                defformatstring(message, "give failed: unknown item '%s'", itemid);
                sendcommandresult(ci, message);
                return;
            }

            clientinfo *target = playername[0] ? findconnectedplayer(playername) : &ci;
            if(!target)
            {
                defformatstring(message, "give failed: player '%s' is not connected", playername);
                sendcommandresult(ci, message);
                return;
            }
            if(!inventoryhasroom(*target, item, int(amount)))
            {
                defformatstring(message, "give failed: %s's inventory does not have room for %ld x %s", target->name, amount, itemid);
                sendcommandresult(ci, message);
                return;
            }

            loopi(int(amount)) addinventoryitem(*target, item);
            sendinventory(*target);
            defformatstring(message, "gave %ld x %s to %s", amount, itemid, target->name);
            sendcommandresult(ci, message);
            if(target != &ci)
            {
                defformatstring(notification, "an administrator gave you %ld x %s", amount, itemid);
                sendcommandresult(*target, notification);
            }
            return;
        }

        if(cubecaseequal(command, "spawn"))
        {
            servernpc *mob = args[0] ? spawnservernpc(ci, args) : NULL;
            if(!mob)
            {
                sendcommandresult(ci, args[0] ? "NPC spawn rejected: unknown type, invalid surface, obstructed space, or no line of sight"
                                                : "usage: /spawn <NPC id>");
                return;
            }
            updateservernpcinterest();
            defformatstring(message, "spawned %s #%u", mob->definition->name, mob->id);
            sendcommandresult(ci, message);
            return;
        }

        if(cubecaseequal(command, "simulationmaxdist") || cubecaseequal(command, "npcmaxdist"))
        {
            int &setting = cubecaseequal(command, "simulationmaxdist") ? serversimulationmaxdist : servernpcmaxdist;
            if(args[0])
            {
                char *end = NULL;
                const long value = strtol(args, &end, 10);
                while(end && iscubespace(*end)) ++end;
                if(end == args || (end && *end) || value < 1 || value > (cubecaseequal(command, "simulationmaxdist") ? 1024 : 4096))
                {
                    sendcommandresult(ci, cubecaseequal(command, "simulationmaxdist") ? "usage: /simulationmaxdist <1-1024>" :
                                                                                       "usage: /npcmaxdist <1-4096>");
                    return;
                }
                setting = int(value);
                updateservernpcinterest();
            }
            defformatstring(message, "%s: %d blocks (server authoritative)", command, setting);
            sendcommandresult(ci, message);
            return;
        }

        if(cubecaseequal(command, "personaldrops") || cubecaseequal(command, "requireconfirmeditems"))
        {
            int &setting = cubecaseequal(command, "personaldrops") ? personaldrops : requireconfirmeditems;
            if(args[0])
            {
                char *end = NULL;
                const long value = strtol(args, &end, 10);
                while(end && iscubespace(*end)) ++end;
                if(end == args || (end && *end) || (value != 0 && value != 1))
                {
                    sendcommandresult(ci, cubecaseequal(command, "personaldrops")
                                           ? "usage: /personaldrops <0|1>"
                                           : "usage: /requireconfirmeditems <0|1>");
                    return;
                }
                setting = int(value);
                senddropsettings();
            }
            defformatstring(message, "%s: %s", command, setting ? "enabled" : "disabled");
            sendcommandresult(ci, message);
            return;
        }

        if(cubecaseequal(command, "droptimeout") || cubecaseequal(command, "maxdrop") || cubecaseequal(command, "dynamicentsmaxdistance"))
        {
            char *end = NULL;
            const long value = strtol(args, &end, 10);
            while(end && iscubespace(*end)) ++end;
            const int minimum = 1, maximum = cubecaseequal(command, "droptimeout") ? 86400 :
                                             cubecaseequal(command, "maxdrop") ? 100000 : 4096;
            if(end == args || (end && *end) || value < minimum || value > maximum)
            {
                if(cubecaseequal(command, "droptimeout")) sendcommandresult(ci, "usage: /droptimeout <seconds 1-86400>");
                else if(cubecaseequal(command, "maxdrop")) sendcommandresult(ci, "usage: /maxdrop <count 1-100000>");
                else sendcommandresult(ci, "usage: /dynamicentsmaxdistance <distance 1-4096>");
                return;
            }
            if(cubecaseequal(command, "droptimeout")) droptimeout = int(value);
            else if(cubecaseequal(command, "maxdrop"))
            {
                maxdrop = int(value);
                while(serverdrops.length() > maxdrop) removeserverdrop(0);
            }
            else dynamicentsmaxdistance = int(value);
            senddropsettings();
            defformatstring(message, "%s: %d", command, int(value));
            sendcommandresult(ci, message);
            return;
        }

        if(cubecaseequal(command, "identityrevoke") ||
           cubecaseequal(command, "idrevoke"))
        {
            serveridentity *identity = valididentityhex(args, 48, 48) ? findserveridentity(args) : NULL;
            if(!identity)
            {
                sendcommandresult(ci, "usage: /idrevoke|identityrevoke <player ID>");
                return;
            }
            bool old = identity->revoked;
            identity->revoked = true;
            if(!writeserveridentities())
            {
                identity->revoked = old;
                sendcommandresult(ci, "could not persist identity revocation");
                return;
            }
            conoutf(CON_WARN, "identity administration: identity revoked by client %d", ci.clientnum);
            sendcommandresult(ci, "player identity revoked");
            loopv(clients)
            {
                clientinfo *other = clients[i];
                if(!other || strcmp(other->playerid, identity->playerid)) continue;
                sendf(other->clientnum, 1, "ri2s", N_IDENTITYREVOKED, PLAYER_IDENTITY_VERSION, "revoked by an administrator");
                disconnect_client(other->clientnum, DISC_PRIVATE);
            }
            return;
        }

        if(cubecaseequal(command, "identityreplace") || cubecaseequal(command, "idreplace"))
        {
            char *publickey = args;
            while(*publickey && !iscubespace(*publickey)) ++publickey;
            if(*publickey) *publickey++ = '\0';
            while(iscubespace(*publickey)) ++publickey;
            serveridentity *identity = valididentityhex(args, 48, 48) ? findserveridentity(args) : NULL;
            void *parsed = valididentitypoint(publickey) ? parsepubkey(publickey) : NULL;
            serveridentity *duplicate = parsed ? findserveridentitybykey(publickey) : NULL;
            if(!identity || !parsed || (duplicate && duplicate != identity))
            {
                if(parsed) freepubkey(parsed);
                sendcommandresult(ci, "usage: /idreplace|identityreplace <player ID> <new public key>");
                return;
            }
            freepubkey(parsed);
            string oldkey;
            copystring(oldkey, identity->publickey);
            bool oldrevoked = identity->revoked;
            copystring(identity->publickey, publickey);
            identity->revoked = false;
            if(!writeserveridentities())
            {
                copystring(identity->publickey, oldkey);
                identity->revoked = oldrevoked;
                sendcommandresult(ci, "could not persist identity replacement");
                return;
            }
            conoutf(CON_WARN, "identity administration: public key replaced by client %d", ci.clientnum);
            sendcommandresult(ci, "player identity public key replaced");
            loopv(clients)
            {
                clientinfo *other = clients[i];
                if(other && !strcmp(other->playerid, identity->playerid)) disconnect_client(other->clientnum, DISC_PRIVATE);
            }
            return;
        }

        if(cubecaseequal(command, "identityban") ||
           cubecaseequal(command, "idban") ||
           cubecaseequal(command, "identityunban") ||
           cubecaseequal(command, "idunban"))
        {
            bool banning = cubecaseequal(command, "identityban") || cubecaseequal(command, "idban");
            serveridentity *identity = valididentityhex(args, 48, 48) ? findserveridentity(args) : NULL;
            if(!identity)
            {
                sendcommandresult(ci, "usage: /idban|idunban <player ID>");
                return;
            }
            bool old = identity->banned;
            int oldkicks = identity->kicks;
            identity->banned = banning;
            if(!banning) identity->kicks = 0;
            if(!writeserveridentities())
            {
                identity->banned = old;
                identity->kicks = oldkicks;
                sendcommandresult(ci, "could not persist identity ban state");
                return;
            }
            conoutf(CON_WARN, "identity administration: identity %s by client %d", identity->banned ? "banned" : "unbanned", ci.clientnum);
            sendcommandresult(ci, identity->banned ? "player identity banned" : "player identity unbanned");
            if(identity->banned) loopv(clients)
            {
                clientinfo *other = clients[i];
                if(other && !strcmp(other->playerid, identity->playerid)) disconnect_client(other->clientnum, DISC_PRIVATE);
            }
            return;
        }

        if(cubecaseequal(command, "identitypermission") ||
           cubecaseequal(command, "idpermission"))
        {
            char *permission = args;
            while(*permission && !iscubespace(*permission)) ++permission;
            if(*permission) *permission++ = '\0';
            while(iscubespace(*permission)) ++permission;
            char *end = NULL;
            long value = strtol(permission, &end, 10);
            serveridentity *identity = valididentityhex(args, 48, 48) ? findserveridentity(args) : NULL;
            if(!identity || end == permission || *end || value < INT_MIN || value > INT_MAX)
            {
                sendcommandresult(ci, "usage: /idpermission|identitypermission <player ID> <integer>");
                return;
            }
            int old = identity->permissions;
            identity->permissions = int(value);
            if(!writeserveridentities())
            {
                identity->permissions = old;
                sendcommandresult(ci, "could not persist identity permissions");
                return;
            }
            conoutf("identity administration: permissions changed from %d to %d by client %d", old, identity->permissions, ci.clientnum);
            sendcommandresult(ci, "player identity permissions updated");
            return;
        }

        if(cubecaseequal(command, "time"))
        {
            if(cubecaseequal(args, "freeze")) worldtimefrozen = true;
            else
            {
                char *end = NULL;
                double hour = strtod(args, &end);
                while(end && iscubespace(*end)) ++end;
                if(end == args || (end && *end) || hour < 0 || hour > 24)
                {
                    sendcommandresult(ci, "usage: /time <hour 0-24|freeze>");
                    return;
                }
                if(hour == 24) hour = 0;
                worldclockmillis = int(hour * SERVER_DAY_MILLIS / 24.0);
                worldtimefrozen = false;
            }
            sendworldtime();
            sendcommandresult(ci, "authoritative world time updated");
            return;
        }

        sendcommandresult(ci, "unknown authoritative world command");
    }

    void parsepacket(int sender, int chan, packetbuf &p)
    {
        if(chan == 0)
        {
            clientinfo *ci = getinfo(sender);
            if(ci && !ci->connected)
            {
                p.pad(p.remaining());
                if(ci->identitystate == IDENTITY_REJECTED) disconnect_client(sender, DISC_MSGERR);
                else rejectidentity(*ci, "authentication is required before position updates");
                return;
            }
            while(ci && ci->connected && ci->worldready && p.remaining())
            {
                int packetstart = p.length();
                int type = getint(p);
                if(type != N_POS)
                {
                    p.pad(p.remaining());
                    if(type == N_NPCSPAWN || type == N_NPCDESPAWN || type == N_NPCSNAPSHOT || type == N_NPCEVENT)
                        kickviolation(*ci, "forged server-authoritative NPC packet");
                    break;
                }

                int cn = getuint(p);
                int coords[3];
                loopk(3) coords[k] = getint(p);
                int physstate = p.get();
                uint flags = getuint(p);
                const uint helditem = flags >> 8;
                int dir = p.get();
                dir |= p.get()<<8;
                const int yaw = dir%360, pitch = clamp(dir/360, 0, 180) - 90;
                p.get();
                int mag = p.get();
                if(flags&(1<<3)) mag |= p.get()<<8;
                dir = p.get();
                dir |= p.get()<<8;
                vec velocity, falling;
                decodedirection(dir%360, clamp(dir/360, 0, 180) - 90, velocity);
                velocity.mul(mag / DVELF);
                if(flags&(1<<4))
                {
                    mag = p.get();
                    if(flags&(1<<5)) mag |= p.get()<<8;
                    if(flags&(1<<6))
                    {
                        dir = p.get();
                        dir |= p.get()<<8;
                        decodedirection(dir%360, clamp(dir/360, 0, 180) - 90, falling);
                    }
                    else falling = vec(0, 0, -1);
                    falling.mul(mag / DVELF);
                }
                else falling = vec(0, 0, 0);
                if(p.overread()) return;
                if(cn != sender || (physstate&7) > PHYS_BOUNCE || ci->dead) continue;

                vec nextposition(coords[0]/DMF, coords[1]/DMF, coords[2]/DMF);
                int now = max(totalmillis, 1),
                    elapsed = ci->hasposition ? max(now - ci->lastpositionmillis, 1) : 0;
                if(nextposition.z < 0 || nextposition.z > (1 << 13) || serverplayeroverlapsnpc(nextposition) ||
                   (ci->hasposition && nextposition.dist(ci->o) > 32.0f + elapsed * 0.5f))
                    continue;

                const int movementstate = physstate&7;
                const bool wasfalling = ci->hasposition && ci->positionphysstate == PHYS_FALL,
                           isfalling = movementstate == PHYS_FALL,
                           inwater = servernaturalwaterat(nextposition);
                int falldamage = 0;
                if(inwater) ci->falldistance = 0;
                else
                {
                    if(ci->hasposition && (wasfalling || isfalling) && nextposition.z < ci->o.z)
                        ci->falldistance = min(ci->falldistance + ci->o.z - nextposition.z, float(1<<13));
                    if(wasfalling && movementstate >= PHYS_SLOPE)
                    {
                        falldamage = game::fallimpactdamage(ci->falldistance);
                        ci->falldistance = 0;
                    }
                }

                ci->position.setsize(0);
                ci->position.put(&p.buf[packetstart], p.length() - packetstart);
                ci->positioncoords = ivec(coords[0], coords[1], coords[2]);
                ci->o = nextposition;
                ci->positionyaw = yaw;
                ci->positionpitch = pitch;
                ci->positionphysstate = movementstate;
                ci->velocity = velocity;
                ci->falling = falling;
                ci->selectedcreative = servercreative() && helditem > 0 && helditem <= uint(numinventoryitems()) ? int(helditem - 1) : -1;
                ci->hasposition = true;
                getservercollisionchunk(serverfloordiv(int(floorf(ci->o.x)), SERVER_WORLD_CHUNK_SIZE),
                                        serverfloordiv(int(floorf(ci->o.y)), SERVER_WORLD_CHUNK_SIZE));
                ci->positiondirty = true;
                ci->lastpositionmillis = now;
                if(falldamage > 0) damageserverplayer(*ci, float(falldamage), vec(ci->o).addz(GAMEUNITSPERMETER));
                if(ci->breakactive)
                {
                    if(actiontargetoutofreach(*ci, ci->breaktarget) || servernpcinterceptsaction(*ci, ci->breaktarget))
                        cancelbreak(*ci);
                    else if(flags&(1<<1)) ci->breakrelease = 0;
                    else if(!ci->breakrelease) ci->breakrelease = max(totalmillis, 1);
                }
            }
            return;
        }
        if(chan != 1)
        {
            clientinfo *ci = getinfo(sender);
            if(ci && !ci->connected)
            {
                if(ci->identitystate == IDENTITY_REJECTED) disconnect_client(sender, DISC_MSGERR);
                else rejectidentity(*ci, "authentication is required before data requests");
            }
            return;
        }
        while(p.remaining())
        {
            int type = getint(p);
            clientinfo *senderinfo = getinfo(sender);
            bool identitypacket = type == N_CONNECT || type == N_IDENTITYLOGIN ||
                                  type == N_IDENTITYREGISTER || type == N_IDENTITYRESPONSE;
            bool identitystatevalid = senderinfo &&
                ((type == N_CONNECT && senderinfo->identitystate == IDENTITY_UNAUTHENTICATED) ||
                 ((type == N_IDENTITYLOGIN || type == N_IDENTITYREGISTER) &&
                  senderinfo->identitystate == IDENTITY_AWAITING_IDENTITY) ||
                 (type == N_IDENTITYRESPONSE &&
                  senderinfo->identitystate == IDENTITY_AWAITING_RESPONSE));
            if(senderinfo && !senderinfo->connected && identitypacket && !identitystatevalid)
            {
                p.pad(p.remaining());
                if(senderinfo->identitystate == IDENTITY_REJECTED) disconnect_client(sender, DISC_MSGERR);
                else rejectidentity(*senderinfo, "unexpected player identity message");
                return;
            }
            if(senderinfo && !senderinfo->connected && !identitypacket)
            {
                p.pad(p.remaining());
                if(senderinfo->identitystate == IDENTITY_REJECTED)
                    disconnect_client(sender, DISC_MSGERR);
                else rejectidentity(*senderinfo, "authentication is required before gameplay");
                return;
            }
            switch(type)
            {
                case N_CONNECT:
                {
                    clientinfo *ci = getinfo(sender);
                    string pass;
                    getstring(pass, p, sizeof(pass));
                    if(!ci || ci->connected || ci->identitystate != IDENTITY_UNAUTHENTICATED) break;
                    if(p.remaining())
                    {
                        p.pad(p.remaining());
                        rejectidentity(*ci, "malformed connection negotiation");
                        break;
                    }
                    if(!serverworldready)
                    {
                        disconnect_client(sender, DISC_PRIVATE);
                        return;
                    }
                    if(serverpass[0] && strcmp(pass, serverpass))
                    {
                        disconnect_client(sender, DISC_PASSWORD);
                        return;
                    }
                    ci->identitystate = IDENTITY_AWAITING_IDENTITY;
                    break;
                }
                case N_IDENTITYLOGIN:
                {
                    clientinfo *ci = getinfo(sender);
                    int version = getint(p);
                    string playerid;
                    getstring(playerid, p, sizeof(playerid));
                    if(!ci || ci->identitystate != IDENTITY_AWAITING_IDENTITY) break;
                    if(version != PLAYER_IDENTITY_VERSION || !valididentityhex(playerid, 48, 48) || p.remaining())
                    {
                        rejectidentity(*ci, "malformed identity login");
                        break;
                    }
                    ci->identitykind = IDENTITY_KIND_RETURNING;
                    copystring(ci->playerid, playerid);
                    serveridentity *identity = findserveridentity(playerid);
                    if(!identity)
                    {
                        rejectidentity(*ci, "unknown player identity");
                        break;
                    }
                    if(identity->revoked || identity->banned)
                    {
                        rejectidentity(*ci, identity->banned ? "identity is banned" : "identity was revoked", identity->revoked);
                        break;
                    }
                    ci->identity = identity;
                    if(!beginidentitychallenge(*ci, identity->publickey)) rejectidentity(*ci, "registered public key is invalid");
                    break;
                }
                case N_IDENTITYREGISTER:
                {
                    clientinfo *ci = getinfo(sender);
                    int version = getint(p);
                    string publickey, nickname;
                    getstring(publickey, p, sizeof(publickey));
                    getstring(nickname, p, sizeof(nickname));
                    if(!ci || ci->identitystate != IDENTITY_AWAITING_IDENTITY) break;
                    if(version != PLAYER_IDENTITY_VERSION || !valididentitypoint(publickey) || p.remaining())
                    {
                        rejectidentity(*ci, "malformed registration");
                        break;
                    }
                    void *parsed = parsepubkey(publickey);
                    if(!parsed)
                    {
                        rejectidentity(*ci, "malformed public key");
                        break;
                    }
                    freepubkey(parsed);
                    serveridentity *existing = findserveridentitybykey(publickey);
                    if(existing)
                    {
                        if(existing->revoked || existing->banned) rejectidentity(*ci, existing->banned ? "identity is banned" : "identity was revoked", existing->revoked);
                        else
                        {
                            ci->identity = existing;
                            ci->identitykind = IDENTITY_KIND_RECOVERY;
                            copystring(ci->playerid, existing->playerid);
                            if(!beginidentitychallenge(*ci, existing->publickey)) rejectidentity(*ci, "registered public key is invalid");
                        }
                        break;
                    }
                    filtertext(ci->pendingname, nickname, false, false, MAXSTRLEN);
                    copystring(ci->pendingpublickey, publickey);
                    ci->identitykind = IDENTITY_KIND_NEW;
                    int attempt = 0;
                    do makepersistentid(ci->playerid, sizeof(ci->playerid), ++attempt);
                    while(findserveridentity(ci->playerid) && attempt < 100);
                    if(!ci->playerid[0] || findserveridentity(ci->playerid) || !beginidentitychallenge(*ci, ci->pendingpublickey))
                        rejectidentity(*ci, "could not create registration challenge");
                    break;
                }
                case N_IDENTITYRESPONSE:
                {
                    clientinfo *ci = getinfo(sender);
                    int version = getint(p);
                    string answer;
                    getstring(answer, p, sizeof(answer));
                    if(!ci || ci->identitystate != IDENTITY_AWAITING_RESPONSE || !ci->identitychallenge)
                    {
                        if(ci) rejectidentity(*ci, "no authentication challenge is pending");
                        break;
                    }
                    void *expected = ci->identitychallenge;
                    ci->identitychallenge = NULL;
                    int issued = ci->identitychallengemillis;
                    ci->identitychallengemillis = 0;
                    bool valid = version == PLAYER_IDENTITY_VERSION &&
                                 valididentityhex(answer, 1, 64) &&
                                 !p.remaining() &&
                                 totalmillis - issued <= PLAYER_IDENTITY_TIMEOUT &&
                                 checkchallenge(answer, expected);
                    freechallenge(expected);
                    if(!valid)
                    {
                        rejectidentity(*ci, totalmillis - issued > PLAYER_IDENTITY_TIMEOUT ? "authentication challenge expired" : "challenge verification failed");
                        break;
                    }
                    if(!ci->identity)
                    {
                        if(findserveridentitybykey(ci->pendingpublickey))
                        {
                            rejectidentity(*ci, "public key was registered concurrently");
                            break;
                        }
                        serveridentity *identity = new serveridentity;
                        copystring(identity->playerid, ci->playerid);
                        copystring(identity->publickey, ci->pendingpublickey);
                        copystring(identity->nickname, ci->pendingname);
                        serveridentities.add(identity);
                        if(!writeserveridentities())
                        {
                            serveridentities.removeobj(identity);
                            delete identity;
                            rejectidentity(*ci, "could not persist player registration");
                            break;
                        }
                        ci->identity = identity;
                    }
                    completeidentity(*ci);
                    break;
                }
                case N_INITCLIENT:
                {
                    clientinfo *ci = getinfo(sender);
                    string name;
                    getstring(name, p, sizeof(name));
                    if(!ci || !ci->connected) break;

                    bool firstinit = !ci->name[0];
                    filtertext(ci->name, name, false, false, MAXSTRLEN);
                    if(!ci->name[0]) formatstring(ci->name, "player%d", sender);
                    if(ci->identity && strcmp(ci->identity->nickname, ci->name))
                    {
                        copystring(ci->identity->nickname, ci->name);
                        if(!writeserveridentities())
                            conoutf(CON_ERROR, "could not persist identity nickname for client %d",
                                    ci->clientnum);
                    }
                    if(firstinit) loopv(clients)
                    {
                        clientinfo *other = clients[i];
                        if(i != sender && other && other->connected && other->name[0]) sendf(sender, 1, "ri2s", N_INITCLIENT, i, other->name);
                    }
                    sendf(-1, 1, "ri2s", N_INITCLIENT, sender, ci->name);
                    sendprivilege(-1, sender, ci->privilege);
                    break;
                }
                case N_TEXT:
                {
                    clientinfo *ci = getinfo(sender);
                    string text;
                    getstring(text, p, sizeof(text));
                    if(ci && ci->connected)
                    {
                        defformatstring(message, "%s: %s", ci->name[0] ? ci->name : "player", text);
                        sendf(-1, 1, "ris", N_SERVMSG, message);
                    }
                    break;
                }
                case N_INVENTORYACTION:
                {
                    clientinfo *ci = getinfo(sender);
                    const uint requestid = uint(getint(p));
                    const int action = getint(p), first = getint(p), second = getint(p);
                    if(ci && ci->connected && !p.overread()) handleinventoryaction(*ci, requestid, action, first, second);
                    break;
                }
                case N_FOODACTION:
                {
                    clientinfo *ci = getinfo(sender);
                    const int active = getint(p);
                    if(ci && ci->connected && !p.overread()) handlefoodaction(*ci, active != 0);
                    break;
                }
                case N_CRAFTACTION:
                {
                    clientinfo *ci = getinfo(sender);
                    const uint requestid = uint(getint(p));
                    const int action = getint(p), first = getint(p), second = getint(p), third = getint(p), fourth = getint(p);
                    if(ci && ci->connected && !p.overread()) handlecraftaction(*ci, requestid, action, first, second, third, fourth);
                    break;
                }
                case N_FURNACEACTION:
                {
                    clientinfo *ci = getinfo(sender);
                    const uint requestid = uint(getint(p));
                    const int action = getint(p), first = getint(p), second = getint(p), third = getint(p), fourth = getint(p);
                    if(ci && ci->connected && !p.overread()) handlefurnaceaction(*ci, requestid, action, first, second, third, fourth);
                    break;
                }
                case N_CHESTACTION:
                {
                    clientinfo *ci = getinfo(sender);
                    const uint requestid = uint(getint(p));
                    const int action = getint(p), first = getint(p), second = getint(p), third = getint(p), fourth = getint(p);
                    if(ci && ci->connected && !p.overread()) handlechestaction(*ci, requestid, action, first, second, third, fourth);
                    break;
                }
                case N_WORLDACTION:
                {
                    clientinfo *ci = getinfo(sender);
                    const uint requestid = uint(getint(p));
                    const int action = getint(p);
                    ivec target;
                    target.x = getint(p); target.y = getint(p); target.z = getint(p);
                    const int orient = getint(p);
                    const ullong itemid = getpersistentid(p);
                    const int item = itemid ? getinventoryitempersistentindex(itemid) : -1,
                              slot = getint(p);
                    if(ci && ci->connected && ci->worldready && !ci->dead && !p.overread())
                        handleworldaction(*ci, requestid, action, target, orient, item, slot);
                    else if(ci && ci->connected && ci->dead)
                        rejectaction(*ci, requestid, "dead players cannot change the world");
                    else if(ci && ci->connected && !ci->worldready)
                        rejectaction(*ci, requestid, "world actions are disabled until synchronization completes");
                    break;
                }
                case N_DROPPICKUP:
                {
                    clientinfo *ci = getinfo(sender);
                    const uint requestid = uint(getint(p)), dropid = uint(getint(p));
                    int coords[3];
                    loopk(3) coords[k] = getint(p);
                    if(ci && ci->connected && !ci->dead && !p.overread())
                        handledroppickup(*ci, requestid, dropid, vec(coords[0] / DMF, coords[1] / DMF, coords[2] / DMF));
                    break;
                }
                case N_CHUNKREQUEST:
                {
                    clientinfo *ci = getinfo(sender);
                    const int chunkx = getint(p), chunky = getint(p);
                    if(ci && ci->connected && ci->worldready && !p.overread() && serverchunkinrange(*ci, chunkx, chunky))
                    {
                        bool duplicate = false;
                        loopv(serverchunkrequests) if(serverchunkrequests[i].clientnum == sender && serverchunkrequests[i].x == chunkx &&
                                                      serverchunkrequests[i].y == chunky)
                        {
                            duplicate = true;
                            break;
                        }
                        if(!duplicate) serverchunkrequests.add(serverchunkrequest(sender, chunkx, chunky));
                    }
                    break;
                }
                case N_NPCATTACK:
                {
                    clientinfo *ci = getinfo(sender);
                    const uint requestid = uint(getint(p)), npcid = uint(getint(p));
                    const int part = getint(p);
                    if(ci && ci->connected && ci->worldready && !ci->dead && !p.overread()) handleservernpcattack(*ci, requestid, npcid, part);
                    break;
                }
                case N_RESPAWN:
                {
                    clientinfo *ci = getinfo(sender);
                    if(ci && ci->connected && ci->worldready) respawnserverplayer(*ci);
                    break;
                }
                case N_EDITENT:
                case N_EDITF: case N_EDITT: case N_EDITM: case N_FLIP: case N_COPY: case N_PASTE: case N_ROTATE: case N_REPLACE: case N_DELCUBE: case N_CALCLIGHT: case N_REMIP: case N_EDITVSLOT: case N_EDITSCATTER: case N_UNDO: case N_REDO: case N_EDITVAR:
                {
                    clientinfo *ci = getinfo(sender);
                    if(!ci || !ci->connected || !ci->worldready)
                    {
                        p.pad(p.remaining());
                        break;
                    }
                    serveredit *edit = new serveredit;
                    const char *error = NULL;
                    if(!validateedit(*ci, type, p, *edit, error))
                    {
                        delete edit;
                        sendcommandresult(*ci, error ? error : "world edit rejected");
                        p.pad(p.remaining());
                        break;
                    }
                    acceptededit(edit);
                    break;
                }
                case N_NEWMAP:
                {
                    clientinfo *ci = getinfo(sender);
                    getint(p);
                    if(ci) sendcommandresult(*ci, "newmap is disabled: the server seed owns the base world");
                    break;
                }
                case N_WORLDREADY:
                {
                    clientinfo *ci = getinfo(sender);
                    vec spawn;
                    loopk(3) spawn[k] = getint(p)/DMF;
                    const int yaw = getint(p), pitch = getint(p);
                    if(ci && ci->connected)
                    {
                        if(!servermapspawnready && spawn.z > SERVER_PLAYER_EYE_HEIGHT && spawn.z < SERVER_WORLD_MAP_SIZE)
                        {
                            servermapspawn = spawn;
                            servermapspawnyaw = clamp(yaw, 0, 359);
                            servermapspawnpitch = clamp(pitch, -90, 90);
                            servermapspawnready = true;
                        }
                        replayworld(*ci);
                    }
                    break;
                }
                case N_SETMASTER:
                {
                    clientinfo *ci = getinfo(sender);
                    string password;
                    getstring(password, p, sizeof(password));
                    if(!ci || !ci->connected) break;
                    if(!strcmp(password, "0") && !ci->local)
                    {
                        ci->privilege = PRIV_NONE;
                        sendprivilege(-1, sender, ci->privilege);
                        sendcommandresult(*ci, "admin privilege relinquished");
                    }
                    else if(ci->local || (adminpass[0] && !strcmp(password, adminpass)))
                    {
                        ci->privilege = PRIV_ADMIN;
                        sendprivilege(-1, sender, ci->privilege);
                        sendcommandresult(*ci, "admin privilege granted");
                    }
                    else sendcommandresult(*ci, "admin authentication failed");
                    break;
                }
                case N_SERVERCOMMAND:
                {
                    clientinfo *ci = getinfo(sender);
                    string command;
                    getstring(command, p, sizeof(command));
                    if(ci && ci->connected) serverworldcommand(*ci, command);
                    break;
                }
                case N_GETMAP:
                    if(mapdata) sendfile(sender, 2, mapdata, "i", N_SENDMAP);
                    break;
                case N_EDITMODE:
                {
                    const bool enabled = getint(p) != 0;
                    clientinfo *ci = getinfo(sender);
                    if(enabled && ci && ci->connected && ci->privilege < PRIV_ADMIN)
                    {
                        sendcommandresult(*ci, "permission denied: full edit mode requires admin");
                        sendf(ci->clientnum, 1, "ri2", N_EDITMODE, 0);
                    }
                    break;
                }
                case N_INVENTORYSTATE:
                case N_FURNACESTATE:
                case N_WORLDAUTH:
                case N_ACTIONRESULT:
                case N_BREAKSTATE:
                case N_DROPSETTINGS:
                case N_DROPSPAWN:
                case N_DROPDELETE:
                case N_FALLBLOCKSPAWN:
                case N_FALLBLOCKUPDATE:
                case N_FALLBLOCKDELETE:
                case N_NPCSPAWN:
                case N_NPCDESPAWN:
                case N_NPCSNAPSHOT:
                case N_NPCEVENT:
                case N_FOODSTATE:
                {
                    clientinfo *ci = getinfo(sender);
                    p.pad(p.remaining());
                    if(ci) kickviolation(*ci, "forged server-authoritative gameplay message");
                    return;
                }
                default:
                {
                    int size = msgsizelookup(type);
                    if(size > 0) loopi(size-1) getint(p);
                    p.pad(p.remaining());
                    break;
                }
            }
        }
    }

    void sendservmsg(const char *s) { sendf(-1, 1, "ris", N_SERVMSG, s); }
    static enet_uint32 lastsend = 0;

    static bool sendpositionbatch(int cn, vector<uchar> &batch)
    {
        if(batch.empty()) return false;
        packetbuf p(batch.length());
        p.put(batch.getbuf(), batch.length());
        sendpacket(cn, 0, p.finalize());
        batch.setsize(0);
        return true;
    }

    bool sendpackets(bool force)
    {
        enet_uint32 curtime = enet_time_get() - lastsend;
        if(curtime < 33 && !force) return false;
        lastsend += curtime - (curtime%33);

        bool sent = false;
        int mtu = getservermtu() - 100;
        if(mtu <= 0) mtu = MAXTRANS;
        loopv(clients)
        {
            clientinfo *recipient = clients[i];
            if(!recipient || !recipient->connected || !recipient->worldready) continue;

            vector<uchar> batch;
            loopvj(clients)
            {
                clientinfo *source = clients[j];
                if(!source || !source->connected || !source->worldready || source == recipient || source->position.empty()) continue;
                if(!batch.empty() && batch.length() + source->position.length() > mtu) sent |= sendpositionbatch(recipient->clientnum, batch);
                batch.put(source->position.getbuf(), source->position.length());
            }
            sent |= sendpositionbatch(recipient->clientnum, batch);
        }
        loopv(clients) if(clients[i]) clients[i]->position.setsize(0);
        return sent;
    }
    void serverinforeply(ucharbuf &req, ucharbuf &p)
    {
        putint(p, PROTOCOL_VERSION);
        int players = 0;
        loopv(clients) if(clients[i] && clients[i]->connected) ++players;
        putint(p, players);
        putint(p, maxclients);
        putint(p, 3);
        putint(p, gamemode);
        putint(p, 0);
        putint(p, MM_OPEN);
        sendstring(smapname, p);
        sendstring(serverdesc, p);
        sendserverinforeply(p);
    }

    static void updateserverfurnaces()
    {
        for(int i = serverfurnaces.length() - 1; i >= 0; --i)
        {
            furnaceinstance &furnace = *serverfurnaces[i];
            if(!furnaceblockvalid(furnace))
            {
                removeserverfurnace(furnace.target);
                continue;
            }
            bool syncchanged = false;
            if(updatefurnaceinstance(furnace, curtime, syncchanged)) dirtyservergameplaycell(furnace.target);
            if(syncchanged) syncfurnaceviewers(furnace);
        }
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(!ci || !ci->connected || !ci->furnaceopen) continue;
            furnaceinstance *furnace = findserverfurnace(ci->furnacetarget);
            if(!furnace || !furnaceaccessible(*ci, *furnace)) closefurnace(*ci);
        }
    }

    static void updateserverchests()
    {
        for(int i = serverchests.length() - 1; i >= 0; --i)
        {
            chestinstance &chest = *serverchests[i];
            if(!chestblockvalid(chest)) removeserverchest(chest.target);
        }
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(!ci || !ci->connected || !ci->chestopen) continue;
            chestinstance *chest = findserverchest(ci->chesttarget);
            if(!chest || !chestaccessible(*ci, *chest)) closechest(*ci);
        }
    }

    void serverupdate()
    {
        if(!serverworldinitialized) return;
        updateserverchunkrequests();
        updateserverfurnaces();
        updateserverchests();
        updateserverfallingblocks();
        updateserversupportblocks();
        updateservernpcs();
        loopv(clients)
        {
            clientinfo *ci = clients[i];
            if(ci && ci->identitystate == IDENTITY_AWAITING_RESPONSE && ci->identitychallenge && totalmillis - ci->identitychallengemillis > PLAYER_IDENTITY_TIMEOUT)
                rejectidentity(*ci, "authentication challenge expired");
            if(!ci || !ci->connected) continue;
            updatefooduse(*ci);
            if(ci->violations && totalmillis - ci->violationwindow >= violationresetinterval * 1000)
            {
                ci->violations = 0;
                ci->violationwindow = max(totalmillis, 1);
            }
            if((ci->inventorydirty && totalmillis - ci->lastinventorysave >= inventorysaveinterval * 1000) ||
               (ci->positiondirty && totalmillis - ci->lastpositionsave >= playerstatesaveinterval * 1000))
            {
                if(!saveplayerstate(*ci))
                    conoutf(CON_ERROR, "could not periodically save player state for player ID %s", ci->playerid);
            }
            if(ci->breakactive &&
               (!ci->hasposition ||
                actiontargetoutofreach(*ci, ci->breaktarget) || servernpcinterceptsaction(*ci, ci->breaktarget) ||
                (ci->breakrelease && totalmillis - ci->breakrelease >= breakcancelgrace)))
                cancelbreak(*ci);
        }
        for(int i = serverdrops.length() - 1; i >= 0; --i)
            if(totalmillis - serverdrops[i]->created >= droptimeout * 1000) removeserverdrop(i);
        if(!worldtimefrozen && curtime > 0)
        {
            worldclockmillis += curtime;
            while(worldclockmillis >= SERVER_DAY_MILLIS) worldclockmillis -= SERVER_DAY_MILLIS;
        }
        if(curtime > 0) weatherclockmillis += uint(curtime);
        if(totalmillis - lastworldtimesync >= 5000)
        {
            lastworldtimesync = totalmillis;
            sendworldtime();
            sendweatherstate();
        }
    }
    int protocolversion() { return PROTOCOL_VERSION; }
    int laninfoport() { return TESSERACT_LANINFO_PORT; }
    int serverport() { return TESSERACT_SERVER_PORT; }
    const char *defaultmaster() { return ""; }
    int masterport() { return TESSERACT_MASTER_PORT; }
    void processmasterinput(const char *cmd, int cmdlen, const char *args) {}
    void masterconnected() {}
    void masterdisconnected() {}
    bool ispaused() { return false; }
    int scaletime(int t) { return t*100; }

    const char *modename(int n, const char *unknown) { return m_valid(n) ? gamemodes[n - STARTGAMEMODE].name : unknown; }
    const char *modeprettyname(int n, const char *unknown) { return m_valid(n) ? gamemodes[n - STARTGAMEMODE].prettyname : unknown; }
    const char *mastermodename(int n, const char *unknown) { return n >= 0 && n < 3 ? mastermodes[n] : unknown; }
    void startintermission() {}
    void stopdemo() {}
    void timeupdate(int secs) {}
    const char *getdemofile(const char *file, bool init) { return NULL; }
    void forcemap(const char *map, int mode) {}
    void forcepaused(bool paused) {}
    void forcegamespeed(int speed) {}
    void hashpassword(int cn, int sessionid, const char *pwd, char *result, int maxlen) { hashstring(pwd, result, maxlen); }
    int msgsizelookup(int msg)
    {
        for(const int *p = msgsizes; *p >= 0; p += 2) if(p[0] == msg) return p[1];
        return -1;
    }
    bool serveroption(const char *arg) { return false; }
    bool delayspawn(int type) { return false; }
}
