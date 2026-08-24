#ifndef __GAME_H__
#define __GAME_H__

#include "cube.h"
#include "mining.h"

#define DMF 16.0f
#define DNF 100.0f
#define DVELF 1.0f
#define GAMEUNITSPERMETER 16.0f

enum
{
    NOTUSED = ET_EMPTY,
    LIGHT = ET_LIGHT,
    MAPMODEL = ET_MAPMODEL,
    PLAYERSTART = ET_PLAYERSTART,
    ENVMAP = ET_ENVMAP,
    PARTICLES = ET_PARTICLES,
    MAPSOUND = ET_SOUND,
    SPOTLIGHT = ET_SPOTLIGHT,
    DECAL = ET_DECAL,
    MAXENTTYPES,

    I_FIRST = 0,
    I_LAST = -1
};

struct gameentity : extentity {};

enum
{
    M_EDIT = 1<<0,
    M_LOCAL = 1<<1,
    M_CREATIVE = 1<<2,
    M_SURVIVAL = 1<<3
};

struct gamemodeinfo
{
    const char *name, *prettyname;
    int flags;
    const char *info;
};
extern const gamemodeinfo gamemodes[3];

#define STARTGAMEMODE 0
#define NUMGAMEMODES 3
#define m_valid(mode) ((mode) >= STARTGAMEMODE && (mode) < STARTGAMEMODE + NUMGAMEMODES)
#define m_edit (m_valid(game::gamemode) && (gamemodes[game::gamemode - STARTGAMEMODE].flags&M_EDIT))
#define m_creative (m_valid(game::gamemode) && (gamemodes[game::gamemode - STARTGAMEMODE].flags&M_CREATIVE))
#define m_survival (m_valid(game::gamemode) && (gamemodes[game::gamemode - STARTGAMEMODE].flags&M_SURVIVAL))
#define m_mp(mode) (m_valid(mode) && !(gamemodes[(mode) - STARTGAMEMODE].flags&M_LOCAL))

enum { MM_OPEN = 0, MM_PRIVATE, MM_PASSWORD, MM_INVALID = -1 };
static const char * const mastermodes[] = { "open", "private", "password" };
enum { PRIV_NONE = 0, PRIV_ADMIN = 3 };

enum
{
    N_CONNECT = 0, N_SERVINFO, N_WELCOME, N_INITCLIENT, N_POS, N_TEXT, N_SOUND, N_CDIS,
    N_MAPCHANGE, N_MAPVOTE, N_PING, N_PONG, N_CLIENTPING, N_SERVMSG,
    N_EDITMODE, N_EDITENT, N_EDITF, N_EDITT, N_EDITM, N_FLIP, N_COPY, N_PASTE, N_ROTATE, N_REPLACE, N_DELCUBE, N_CALCLIGHT, N_REMIP, N_EDITVSLOT, N_UNDO, N_REDO, N_NEWMAP, N_GETMAP, N_SENDMAP, N_CLIPBOARD, N_EDITVAR, N_EDITSCATTER,
    N_EDITAUTHOR, N_WORLDSTATE, N_WORLDREADY, N_WORLDSYNC, N_WORLDTIME, N_WEATHERSTATE,
    N_CHUNKREQUEST, N_CHUNKDATA,
    N_SETPRIVILEGE, N_SETMASTER, N_SERVERCOMMAND,
    N_SERVERIDENTITY, N_IDENTITYLOGIN, N_IDENTITYREGISTER, N_IDENTITYCHALLENGE,
    N_IDENTITYRESPONSE, N_IDENTITYSUCCESS, N_IDENTITYFAILURE, N_IDENTITYREVOKED,
    N_INVENTORYSTATE, N_INVENTORYACTION, N_CRAFTSTATE, N_CRAFTACTION, N_WORLDACTION, N_WORLDAUTH, N_ACTIONRESULT,
    N_BREAKSTATE, N_DROPSETTINGS, N_DROPSPAWN, N_DROPDELETE, N_DROPPICKUP,
    N_FALLBLOCKSPAWN, N_FALLBLOCKUPDATE, N_FALLBLOCKDELETE,
    N_FURNACESTATE, N_FURNACEACTION, N_CHESTSTATE, N_CHESTACTION, N_CHESTANIM,
    N_NPCSPAWN, N_NPCDESPAWN, N_NPCSNAPSHOT, N_NPCEVENT, N_NPCATTACK,
    N_PLAYERSTATE, N_RESPAWN, N_FOODACTION, N_FOODSTATE,
    NUMMSG
};

enum
{
    INVENTORY_ACTION_SWAP = 0,
    INVENTORY_ACTION_SELECT,
    INVENTORY_ACTION_CLICK
};

enum
{
    INVENTORY_CLICK_LEFT = 0,
    INVENTORY_CLICK_RIGHT
};

enum
{
    CRAFT_ACTION_OPEN_PLAYER = 0,
    CRAFT_ACTION_OPEN_TABLE,
    CRAFT_ACTION_INVENTORY_TO_GRID,
    CRAFT_ACTION_GRID_TO_INVENTORY,
    CRAFT_ACTION_GRID_SWAP,
    CRAFT_ACTION_TAKE_OUTPUT,
    CRAFT_ACTION_CLICK_GRID,
    CRAFT_ACTION_TAKE_OUTPUT_CURSOR
};

enum
{
    FURNACE_ACTION_OPEN = 0,
    FURNACE_ACTION_CLOSE,
    FURNACE_ACTION_CLICK_INPUT,
    FURNACE_ACTION_CLICK_FUEL,
    FURNACE_ACTION_CLICK_OUTPUT,
    FURNACE_ACTION_BAKE
};

enum
{
    CHEST_ACTION_OPEN = 0,
    CHEST_ACTION_CLOSE,
    CHEST_ACTION_CLICK
};

enum
{
    WORLD_ACTION_PLACE_CUBE = 0,
    WORLD_ACTION_PLACE_SCATTER,
    WORLD_ACTION_BREAK_CUBE_START,
    WORLD_ACTION_BREAK_SCATTER_START,
    WORLD_ACTION_BREAK_UPDATE,
    WORLD_ACTION_BREAK_CANCEL,
    WORLD_ACTION_BREAK_COMPLETE,
    WORLD_ACTION_PLACE_ITEM,
    WORLD_ACTION_PUSH_CORNER
};

enum
{
    BREAK_STATE_START = 0,
    BREAK_STATE_UPDATE,
    BREAK_STATE_COMPLETE,
    BREAK_STATE_CANCEL
};

enum
{
    ACTION_RESULT_REJECTED = 0,
    ACTION_RESULT_ACCEPTED,
    ACTION_RESULT_CORRECTED
};

static const int msgsizes[] =
{
    N_CONNECT, 0, N_SERVINFO, 0, N_WELCOME, 1, N_INITCLIENT, 0, N_POS, 0, N_TEXT, 0, N_SOUND, 2, N_CDIS, 2,
    N_MAPCHANGE, 0, N_MAPVOTE, 0, N_PING, 2, N_PONG, 2, N_CLIENTPING, 2, N_SERVMSG, 0,
    N_EDITMODE, 2, N_EDITENT, 11, N_EDITF, 16, N_EDITT, 16, N_EDITM, 16,
    N_FLIP, 14, N_COPY, 14, N_PASTE, 14, N_ROTATE, 15, N_REPLACE, 17,
    N_DELCUBE, 14, N_CALCLIGHT, 1, N_REMIP, 1, N_EDITVSLOT, 16,
    N_UNDO, 0, N_REDO, 0, N_NEWMAP, 2, N_GETMAP, 1, N_SENDMAP, 0,
    N_CLIPBOARD, 0, N_EDITVAR, 0, N_EDITSCATTER, 16, N_EDITAUTHOR, 4,
    N_WORLDSTATE, 31, N_WORLDREADY, 6, N_WORLDSYNC, 2, N_WORLDTIME, 3, N_WEATHERSTATE, 5,
    N_CHUNKREQUEST, 3, N_CHUNKDATA, 0,
    N_SETPRIVILEGE, 3, N_SETMASTER, 0, N_SERVERCOMMAND, 0,
    N_SERVERIDENTITY, 0, N_IDENTITYLOGIN, 0, N_IDENTITYREGISTER, 0, N_IDENTITYCHALLENGE, 0,
    N_IDENTITYRESPONSE, 0, N_IDENTITYSUCCESS, 0, N_IDENTITYFAILURE, 0, N_IDENTITYREVOKED, 0,
    N_INVENTORYSTATE, 0, N_INVENTORYACTION, 5, N_CRAFTSTATE, 0, N_CRAFTACTION, 7, N_WORLDACTION, 9, N_WORLDAUTH, 7,
    N_ACTIONRESULT, 0, N_BREAKSTATE, 10, N_DROPSETTINGS, 6, N_DROPSPAWN, 11, N_DROPDELETE, 3, N_DROPPICKUP, 6,
    N_FALLBLOCKSPAWN, 7, N_FALLBLOCKUPDATE, 7, N_FALLBLOCKDELETE, 2,
    N_FURNACESTATE, 0, N_FURNACEACTION, 7, N_CHESTSTATE, 0, N_CHESTACTION, 7, N_CHESTANIM, 5,
    N_NPCSPAWN, 0, N_NPCDESPAWN, 3, N_NPCSNAPSHOT, 11, N_NPCEVENT, 0, N_NPCATTACK, 4,
    N_PLAYERSTATE, 10, N_RESPAWN, 1, N_FOODACTION, 2, N_FOODSTATE, 6,
    -1
};

#define TESSERACT_SERVER_PORT 42000
#define TESSERACT_LANINFO_PORT 41998
#define TESSERACT_MASTER_PORT 41999
#define PROTOCOL_VERSION 33

enum
{
    NPC_EVENT_DAMAGE = 0,
    NPC_EVENT_DISMEMBER,
    NPC_EVENT_DEATH,
    NPC_EVENT_ATTACK,
    NPC_EVENT_STATE
};

enum
{
    NPC_STATE_DEAD = 1<<0,
    NPC_STATE_FROZEN = 1<<1,
    NPC_STATE_ATTACKING = 1<<2,
    NPC_STATE_CRAWLING = 1<<3
};

enum npcattitude
{
    NPC_AGGRESSIVE = 0,
    NPC_NEUTRAL,
    NPC_FRIENDLY,
    NPC_SCARED
};

enum npcbehavior
{
    NPC_WANDERING = 0,
    NPC_CHASE,
    NPC_FLEE
};

enum npcmodeltype
{
    NPC_MODEL_HUMANOID = 0,
    NPC_MODEL_QUADRUPED
};

struct npcdropdefinition
{
    string itemid;
    int mincount, maxcount;
    float chance;

    npcdropdefinition(const char *itemid = "", int mincount = 0, int maxcount = 0, float chance = 1.0f)
        : mincount(mincount), maxcount(maxcount), chance(chance)
    {
        copystring(this->itemid, itemid);
    }
};

struct npcdefinition
{
    string id, name, model;
    int attitude, behavior, health, attackmillis, modeltype, naturalbiome, groupmin, groupmax, fleeonhitmillis;
    float damage, speed, wanderradius, aggrodist, fleedist, radius, height, rootheight, spawnchance, fleespeed, herdradius;
    vector<npcdropdefinition> drops;

    npcdefinition(const char *id = "")
        : attitude(NPC_NEUTRAL), behavior(NPC_WANDERING), health(20), attackmillis(1000), modeltype(NPC_MODEL_HUMANOID), naturalbiome(-1),
          groupmin(1), groupmax(1), fleeonhitmillis(0), damage(1), speed(40), wanderradius(8), aggrodist(16), fleedist(12), radius(4.1f),
          height(28.0f), rootheight(11.25f), spawnchance(0), fleespeed(1), herdradius(0)
    {
        copystring(this->id, id);
        copystring(name, id);
        model[0] = '\0';
    }
};

enum
{
    PASSIVE_NPC_CELL_BLOCKS = 32,
    PASSIVE_NPC_GROUP_RADIUS_BLOCKS = 6
};

struct passivenpcspawn
{
    ullong key;
    int blockx, blocky;
    float yaw;

    passivenpcspawn() : key(0), blockx(0), blocky(0), yaw(0) {}
};

static inline bool inventoryslotclick(int &cursoritem, int &cursorcount, int &slotitem, int &slotcount, int button)
{
    if(button != INVENTORY_CLICK_LEFT && button != INVENTORY_CLICK_RIGHT) return false;
    if(cursorcount <= 0) { cursoritem = -1; cursorcount = 0; }
    if(slotcount <= 0) { slotitem = -1; slotcount = 0; }
    if(button == INVENTORY_CLICK_LEFT)
    {
        if(cursorcount <= 0)
        {
            if(slotcount <= 0) return false;
            swap(cursoritem, slotitem);
            swap(cursorcount, slotcount);
            return true;
        }
        if(slotcount <= 0)
        {
            swap(cursoritem, slotitem);
            swap(cursorcount, slotcount);
            return true;
        }
        if(cursoritem != slotitem)
        {
            swap(cursoritem, slotitem);
            swap(cursorcount, slotcount);
            return true;
        }
        const int moved = min(cursorcount, max(getinventoryitemmaxstack(slotitem), 1) - slotcount);
        if(moved <= 0) return false;
        slotcount += moved;
        cursorcount -= moved;
    }
    else if(cursorcount <= 0)
    {
        if(slotcount <= 0) return false;
        const int moved = (slotcount + 1) / 2;
        cursoritem = slotitem;
        cursorcount = moved;
        slotcount -= moved;
    }
    else
    {
        if(slotcount > 0 && (slotitem != cursoritem || slotcount >= max(getinventoryitemmaxstack(slotitem), 1))) return false;
        if(slotcount <= 0) slotitem = cursoritem;
        ++slotcount;
        --cursorcount;
    }
    if(cursorcount <= 0) { cursoritem = -1; cursorcount = 0; }
    if(slotcount <= 0) { slotitem = -1; slotcount = 0; }
    return true;
}

static inline bool inventoryinstanceclick(int &cursoritem, int &cursorcount, int &cursordurability,
                                          int &slotitem, int &slotcount, int &slotdurability, int button)
{
    const int oldcursoritem = cursoritem, oldcursorcount = cursorcount, oldcursordurability = cursordurability,
              oldslotitem = slotitem, oldslotcount = slotcount, oldslotdurability = slotdurability;
    if(!inventoryslotclick(cursoritem, cursorcount, slotitem, slotcount, button)) return false;

    if(cursoritem < 0 || cursorcount <= 0) cursordurability = 0;
    else if(cursoritem == oldslotitem && (oldcursoritem != cursoritem || oldcursorcount <= 0)) cursordurability = oldslotdurability;
    else if(cursoritem != oldcursoritem) cursordurability = oldslotdurability;
    else cursordurability = oldcursordurability;

    if(slotitem < 0 || slotcount <= 0) slotdurability = 0;
    else if(slotitem == oldcursoritem && (oldslotitem != slotitem || oldslotcount <= 0)) slotdurability = oldcursordurability;
    else if(slotitem != oldslotitem) slotdurability = oldcursordurability;
    else slotdurability = oldslotdurability;
    return true;
}

static inline uint worlddrophash(uint value)
{
    value ^= value >> 16;
    value *= 0x7FEB352DU;
    value ^= value >> 15;
    value *= 0x846CA68BU;
    return value ^ (value >> 16);
}

static inline bool worlddroproll(int source, uint requestid, int objectitem, int dropindex, int mincount, int maxcount, float chance, int &quantity)
{
    const uint seed = worlddrophash(requestid ^ uint(source + 1) * 0x9E3779B9U ^ uint(objectitem + 1) * 0x85EBCA6BU ^ uint(dropindex + 1));
    if(float(seed & 0xFFFFFFU) / 16777216.0f >= clamp(chance, 0.0f, 1.0f)) return false;
    quantity = mincount >= maxcount ? mincount : mincount + int(worlddrophash(seed ^ 0xC2B2AE35U) % uint(maxcount - mincount + 1));
    return quantity > 0;
}

struct worlddrop
{
    uint id, sourcerequestid, pickuprequestid;
    int source, item, count, durability, owner, created, pickupmillis, picker, physicsmillis, settledmillis;
    float fallvelocity;
    bool confirmed, picking, removed, pickupblocked, settled, landingknown, geometryready;
    vec o, pickupfrom, landing;

    worlddrop() : id(0), sourcerequestid(0), pickuprequestid(0), source(-1), item(-1), count(0), durability(0), owner(-1), created(0), pickupmillis(0),
                  picker(-1), physicsmillis(0), settledmillis(0), fallvelocity(0), confirmed(false), picking(false), removed(false),
                  pickupblocked(false), settled(false), landingknown(false), geometryready(true), o(0, 0, 0), pickupfrom(0, 0, 0), landing(0, 0, 0)
    {
    }
};

struct fallingblock
{
    uint id;
    int item, snapshotmillis, servertick, physicsmillis;
    float velocity, servervelocity;
    bool replicated, landingknown;
    ivec origin;
    vec o, serverposition, landing;

    fallingblock()
        : id(0), item(-1), snapshotmillis(0), servertick(0), physicsmillis(0), velocity(0), servervelocity(0), replicated(false),
          landingknown(false), origin(0, 0, 0), o(0, 0, 0), serverposition(0, 0, 0), landing(0, 0, 0)
    {
    }
};

struct chunkfallingblockstate
{
    int item;
    ivec origin;
    vec position;
    float velocity;

    chunkfallingblockstate() : item(-1), origin(0, 0, 0), position(0, 0, 0), velocity(0) {}
};

struct chunkdropstate
{
    int item, count, durability, age;
    vec position;
    string ownerid;

    chunkdropstate() : item(-1), count(0), durability(0), age(0), position(0, 0, 0) { ownerid[0] = '\0'; }
};

struct gameent : dynent
{
    int clientnum, privilege, ping, lastupdate, plag;
    editinfo *edit;
    float deltayaw, deltapitch, deltaroll, newyaw, newpitch, newroll;
    float renderbodyyaw, rendercrouch, renderstridephase, renderattackreleasepitch;
    int smoothmillis, renderbodyyawmillis, rendercrouchmillis, renderstridemillis, selectedcreative,
        renderattackmillis, renderattackreleasemillis, renderplacemillis, rendereatmillis, rendereatitem, rendereatduration,
        rendereatcrumbmillis,
        ragdollstart[6], ragdollend[6];
    float health;
    bool renderattacking, rendereating, renderplacetoggle, renderactioninitialized;
    string name;

    gameent() : clientnum(-1), privilege(0), ping(0), lastupdate(0), plag(0), edit(NULL),
                deltayaw(0), deltapitch(0), deltaroll(0), newyaw(0), newpitch(0), newroll(0),
                renderbodyyaw(0), rendercrouch(0), renderstridephase(0), renderattackreleasepitch(0),
                smoothmillis(-1), renderbodyyawmillis(-1), rendercrouchmillis(-1),
                renderstridemillis(-1), selectedcreative(-1), renderattackmillis(0),
                renderattackreleasemillis(-1000),
                renderplacemillis(-1000), rendereatmillis(-1000), rendereatitem(-1), rendereatduration(0), rendereatcrumbmillis(-1),
                health(20.0f), renderattacking(false),
                rendereating(false), renderplacetoggle(false),
                renderactioninitialized(false)
    {
        type = ENT_PLAYER;
        state = editstate = CS_ALIVE;
        maxspeed = 120.0f;
        name[0] = '\0';
        loopi(6) ragdollstart[i] = ragdollend[i] = -1;
    }

    ~gameent()
    {
        freeeditinfo(edit);
    }
};

enum
{
    HITBOX_TORSO = 0,
    HITBOX_HEAD,
    HITBOX_LEFT_ARM,
    HITBOX_RIGHT_ARM,
    HITBOX_LEFT_LEG,
    HITBOX_RIGHT_LEG,
    NUM_HUMANOID_HITBOXES
};

struct characterhitbox
{
    vec center, radius;
    int part;

    characterhitbox(const vec &center = vec(0, 0, 0), const vec &radius = vec(0, 0, 0), int part = HITBOX_TORSO)
        : center(center), radius(radius), part(part)
    {
    }
};

namespace entities
{
    extern vector<extentity *> ents;
    extern void preloadentities();
    extern void renderentities();
}

namespace game
{
    enum
    {
        CREATIVE_ARM_CYCLE = 300,
        SURVIVAL_BUILD_REACH = 4 * 16,
        NPC_ATTACK_REACH = 2 * 16
    };

    struct networkedit
    {
        int type, author, args[6];
        uint revision, requestid;
        selinfo selection;
        vector<uchar> extra;

        networkedit() : type(-1), author(-1), revision(0), requestid(0)
        {
            memset(args, 0, sizeof(args));
        }
    };

    extern int gamemode;
    extern string clientmap;
    extern bool connected, remote;
    extern gameent *player1;
    extern vector<gameent *> players, clients;
    extern npcdefinition *findnpcdefinition(const char *id);
    extern int numnpcdefinitions();
    extern npcdefinition *getnpcdefinition(int index);
    extern void loadnpcdefinitions();
    extern int generatepassivenpcgroup(const npcdefinition &definition, int worldseed, int cellx, int celly, passivenpcspawn *spawns,
                                       int maxspawns);
    extern void resetlocalpassivenpcstates();
    extern bool savelocalpassivenpcs(const char *world);
    extern bool loadlocalpassivenpcs(const char *world);

    extern void changemap(const char *name, int mode);
    extern bool addmsg(int type, const char *fmt = NULL, ...);
    extern void c2sinfo(bool force = false);
    extern void beginlocalworld();
    extern bool islocalworld();
    extern bool waitforserveredit();
    extern void requestworldcommand(const char *command);
    extern float horizontalmeterspersecond(const physent *d);
    extern int fallimpactdamage(float distance);
    extern void addlocalitemdrop(int item, int count, const vec &origin, uint spreadseed);
    extern float playerarmactionpitch(const gameent *d);
    extern float playerfooduseamount(const gameent *d);
    extern float creativearmwave(int elapsed);
    extern int selectedcreativeblock();
    extern int selectedtoolquality();
    extern void wearselectedsurvivaltool();
    extern void damageplayer(float damage, const vec &source);
    extern float getlocalplayerhealth();
    extern void restorelocalplayerhealth(float health);
    extern void getlocalplayermotion(vec &velocity, vec &falling, float &falldistance, int &physstate);
    extern void restorelocalplayermotion(const vec &velocity, const vec &falling, float falldistance, int physstate);
    extern void savesessionstate();
    extern void receiveplayerstate(int clientnum, float health, int state, const vec &position, const vec &impulse);
    extern void receivefoodstate(int clientnum, bool active, int item, int elapsed);
    extern void beginplayerragdoll(gameent *d, const vec &impulse);
    extern void clearplayerragdoll(gameent *d);
    extern void receiveserversettings(int breakmillis, int scatterbreakmillis, int waterupdates, int waterdistance, int waterspeed, int npcsimulationdistance);
    extern int getnpcsimulationmaxdist();
    extern void receivedropsettings(int personal, int timeout, int maximum, int maxdistance, int requireconfirmation);
    extern void receivedropspawn(uint id, int source, uint sourcerequestid, int item, int count, int durability, int owner, const vec &o);
    extern void receivedropdelete(uint id, int picker);
    extern void resetworlddrops();
    extern const vector<worlddrop *> &getworlddrops();
    extern void receivefallblockspawn(uint id, int item, const vec &position, float velocity);
    extern void receivefallblockupdate(uint id, int tick, const vec &position, float velocity);
    extern void receivefallblockdelete(uint id);
    extern void resetfallingblocks();
    extern const vector<fallingblock *> &getfallingblocks();
    extern int getdynamicentsmaxdistance();
    extern void receiveinventory(const int *items, const int *counts, const int *durabilities, int slots, int selected,
                                 int cursoritem, int cursorcount, int cursordurability);
    extern void receivecraftstate(const int *items, const int *counts, const int *durabilities, int slots, int gridsize, int stationitem,
                                  int recipe, int outputitem, int outputcount);
    extern void receivefurnacestate(const furnaceinstance &furnace, bool open, bool cooking);
    extern void receivecheststate(const chestinstance &chest, bool open);
    extern void receivechestanimation(const ivec &target, bool open);
    extern float getchestlidangle(const ivec &target);
    extern int getchestyaw(const ivec &target);
    extern void resetfurnaces();
    extern void resetchests();
    extern bool savelocalfurnaces(const char *world);
    extern bool loadlocalfurnaces(const char *world);
    extern bool savelocalchests(const char *world);
    extern bool loadlocalchests(const char *world);
    extern bool capturelocalchunkdata(int chunkx, int chunky, vector<uchar> &data);
    extern bool restorelocalchunkdata(int chunkx, int chunky, const uchar *data, int length);
    extern bool haslocalchunkdynamicstate(int chunkx, int chunky);
    extern bool debuglocalchunkdata(stream *file, int chunkx, int chunky, const uchar *data, int length);
    extern bool capturechunkdata(int chunkx, int chunky, const vector<furnaceinstance *> &furnaces, const vector<chestinstance *> &chests,
                                 const vector<uchar> &npcdata, const vector<chunkfallingblockstate> &falling,
                                 const vector<chunkdropstate> &drops, vector<uchar> &data);
    extern bool decodechunkdata(int chunkx, int chunky, const uchar *data, int length, vector<furnaceinstance *> &furnaces,
                                vector<chestinstance *> &chests, vector<uchar> &npcdata, vector<chunkfallingblockstate> &falling,
                                vector<chunkdropstate> &drops);
    extern bool capturelocalchunknpcs(int chunkx, int chunky, vector<uchar> &data);
    extern bool restorelocalchunknpcs(int chunkx, int chunky, const uchar *data, int length);
    extern bool debuglocalchunknpcs(stream *file, const uchar *data, int length);
    extern void unloadlocalchunknpcs(int chunkx, int chunky);
    extern void receiveactionresult(uint requestid, int result, const char *reason);
    extern void receivebreakstate(int actor, uint requestid, int phase, int action, const ivec &target, int orient, int stage);
    extern int smoothmove, smoothdist;
    extern vector<networkedit *> pendingnetworkedits;
    extern void processnetworkedits();

#ifndef STANDALONE
    extern void preloadplayermodels();
    extern void preloadnpcs();
    extern void resetnpcs();
    extern void updatenpcs();
    extern void rendernpcs();
    extern void rendernpcdebug();
    extern bool attacknpc();
    extern void damagefallingnpc(physent *d, float damage);
    extern void receivenpcspawn(uint id, const char *definition, const vec &position, float yaw, float health, uint detachedparts, int stateflags);
    extern void receivenpcdespawn(uint id);
    extern void receivenpcsnapshot(uint id, int tick, const vec &position, const vec &velocity, float yaw, int stateflags);
    extern void receivenpcevent(uint id, int event, int tick, float health, uint detachedparts, int part, const vec &position,
                                const vec &impulse);
    extern int numnpcs();
    extern dynent *iternpc(int index);
    extern void getplayerhitboxes(gameent *d, vector<characterhitbox> &hitboxes);
    extern bool heldtorchemitterposition(gameent *d, vec &position);
    extern bool heldtorchworldemitterposition(gameent *d, vec &position);
    extern void resetclientreceive();
    extern bool pendingnetworkworld, pendingnetworkreset, pendingnetworkfrozen,
                pendingnetworkrestoreposition;
    extern int pendingnetworkseed, pendingnetworktime, pendingnetworkyaw, pendingnetworkpitch, pendingnetworkphysstate;
    extern float pendingnetworkfalldistance;
    extern vec pendingnetworkposition, pendingnetworkvelocity, pendingnetworkfalling;

    namespace environment
    {
        extern void reset();
        extern void update();
        extern void synctime(int millis, bool frozen);
        extern int gettimemillis();
        extern float getdayprogress();
        extern float gethourafter(int millis);
        extern bool istimefrozen();
        extern float getambientlightlevel();
    }
#endif
}

namespace server
{
    extern int msgsizelookup(int msg);
    extern void resetservernpcs();
}

#endif
