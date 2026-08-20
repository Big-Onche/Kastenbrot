#include "game.h"

#ifndef STANDALONE
#include "weather.h"
extern int mainmenu;
extern int initing;
extern int simulationmaxdist;
#endif

const gamemodeinfo gamemodes[3] =
{
    { "creative", "Creative", M_CREATIVE, "Build freely with fixed-size voxel blocks." },
    { "edit", "Edit", M_EDIT, "Cooperative map editing." },
    { "survival", "Survival", M_SURVIVAL, "Gather resources and break blocks by hand." }
};

namespace game
{
    static void paintworldcube(int worldindex, const selinfo &selection, bool local)
    {
#ifndef STANDALONE
        loopi(6)
        {
            selinfo face = selection;
            face.orient = i;
            mpedittex(getworldcubefaceslot(worldindex, i), 0, face, local);
        }
#else
        (void)worldindex;
        (void)selection;
        (void)local;
#endif
    }

    int gamemode = STARTGAMEMODE;
    string clientmap = "";
    bool connected = false, remote = false, gamepaused = false;
    static bool localworldactive = false;
    int mastermode = MM_OPEN;
    gameent *player1 = NULL;
    vector<gameent *> players, clients;
    vector<uchar> messages;
#ifndef STANDALONE
    static uint nextworldrequestid = 1;

    struct predictedworldaction
    {
        uint requestid;
        int action, orient, item, yaw;
        ivec target;

        predictedworldaction() : requestid(0), action(-1), orient(0), item(-1), yaw(0), target(0, 0, 0) {}
    };

    static vector<predictedworldaction *> predictedworldactions;
    static vector<worlddrop *> worlddrops;
    static vector<fallingblock *> fallingblocks;
    static vector<ivec> fallblockchecks;

    struct localsupportcell
    {
        int distance, index, unsupportedindex;

        localsupportcell(int distance = 0, int index = -1) : distance(distance), index(index), unsupportedindex(-1) {}
    };

    struct localsupportcheck
    {
        ivec cell;
        int remaining;

        localsupportcheck(const ivec &cell = ivec(0, 0, 0), int remaining = 0) : cell(cell), remaining(remaining) {}
    };

    enum
    {
        LOCAL_SUPPORT_SECTION_SIZE = 16 * 16,
        LOCAL_SUPPORT_SECTION_LAYERS = 512 / 16
    };

    static hashtable<ivec, localsupportcell> localsupportcells(1 << 12);
    static hashtable<ivec, int> localsupportpersistent(1 << 10);
    static hashtable<ivec, int> localsupportscannedsections(1 << 10);
    static hashtable<ivec, int> localsupportqueuedsections(1 << 10);
    static vector<ivec> localsupportpositions, localunsupportedpositions;
    static vector<ivec> localsupportscannedpositions, localsupportsectionchecks;
    static vector<localsupportcheck> localsupportchecks;
    static uint nextlocaldropid = 1;
    static uint nextlocalfallblockid = 1;
    static int localsupportlasttick = 0;
    static ivec localsupportlastsection(INT_MIN, INT_MIN, INT_MIN);
    static int localsupportlastdistance = -1;
    static int personaldrops = 0, droptimeout = 300, maxdrop = 1024, dynamicentsmaxdistance = 64, requireconfirmeditems = 1;
    VARP(supportdecaymillis, 10, 3000, 60000);
    static void updateworlddrops();
    static void updatefallingblocks();
    static void queuefallblockcheck(const ivec &cell);
    static void updatesupportblocks();
    static void queuesupportchange(const ivec &cell);
    static void setlocalsupportpersistent(const ivec &cell, bool persistent);
    static void resetlocalsupportblocks();
    static void updatefurnaces();
    static void updatechests();
    static void setchestvisual(const ivec &target, int yaw);
    static void removechestvisual(const ivec &target);
    static chestinstance *findlocalchest(const ivec &target);
    static void predictsurvivaldrops(int objectitem, uint requestid, const ivec &target, int action, int orient);
#endif

    float horizontalmeterspersecond(const physent *d)
    {
        if(!d) return 0.0f;
        float movescale = d->inwater && d->state != CS_EDITING && d->state != CS_SPECTATOR ? 0.5f : 1.0f;
        float x = d->vel.x * movescale + d->falling.x,
              y = d->vel.y * movescale + d->falling.y;
        return sqrtf(x*x + y*y) / GAMEUNITSPERMETER;
    }

    int fallimpactdamage(float distance)
    {
        const float damagingblocks = max(distance / GAMEUNITSPERMETER - 3.0f, 0.0f);
        return int(ceilf(damagingblocks * 0.5f));
    }

    static string connectpass = "";
    static int lastpositionsend = -1000;
    static string sentname = "";
    static void sendposition(gameent *d, packetbuf &q);
#ifndef STANDALONE
    static void updatesurvivalbreaking();
    static void cancelclientbreakrequest(uint requestid);
    static void hidedeathscreen();
    static void updatefooduse();
    static bool survivalenabled();
#endif

    static void putsel(packetbuf &p, const selinfo &sel)
    {
        putint(p, sel.o.x); putint(p, sel.o.y); putint(p, sel.o.z);
        putint(p, sel.s.x); putint(p, sel.s.y); putint(p, sel.s.z);
        putint(p, sel.grid); putint(p, sel.orient);
        putint(p, sel.cx); putint(p, sel.cxs); putint(p, sel.cy); putint(p, sel.cys);
        putint(p, sel.corner);
    }

#ifndef STANDALONE
    static uint newworldrequestid()
    {
        if(!nextworldrequestid) ++nextworldrequestid;
        return nextworldrequestid++;
    }

    static predictedworldaction *findpredictedworldaction(uint requestid)
    {
        loopv(predictedworldactions) if(predictedworldactions[i]->requestid == requestid) return predictedworldactions[i];
        return NULL;
    }

    static void worldactionselection(selinfo &sel, const ivec &origin, int orient)
    {
        sel.o = origin;
        sel.s = ivec(1, 1, 1);
        sel.grid = 16;
        sel.orient = orient;
        sel.cx = sel.cy = sel.corner = 0;
        sel.cxs = sel.cys = 2;
    }

    static ivec worldactionplacecell(const ivec &support, int orient)
    {
        ivec target = support;
        const int dimension = orient >> 1;
        target[dimension] += orient&1 ? 16 : -16;
        return target;
    }

    static int worldmountorient(int orient) { return ((orient % 6) + 6) % 6; }
    static int worldplaceyaw(int orient) { return clamp(orient / 6, 0, 3) * 90; }

    static bool applyworldscatteraction(int item, const ivec &support, int orient, bool place)
    {
        const int type = getworlditemindex(item);
        if(type < 0) return false;
        selinfo occupied;
        worldactionselection(occupied, worldactionplacecell(support, orient), orient);
        if(!occupied.validate() || !worldselectionready(occupied)) return false;
        const int existing = getworldscatterindexat(support, orient);
        if((place && existing == type) || (!place && existing < 0)) return true;
        return editworldscatter(type, support, orient, place);
    }

    static bool applyworldaction(int action, const ivec &absolutetarget, int orient, int item)
    {
        const int packed = orient;
        orient = worldmountorient(orient);
        selinfo sel;
        worldactionselection(sel, absolutetarget, orient);
        worldselectiontolocal(sel);
        if(!sel.validate() || !worldselectionready(sel)) return false;
        const ivec target = sel.o;
        switch(action)
        {
            case WORLD_ACTION_PLACE_CUBE:
            {
                if(getworlditemtype(item) != WORLD_ITEM_CUBE) return false;
                const int type = getworlditemindex(item);
                const ivec placedorigin = worldactionplacecell(target, orient),
                           absoluteplacedorigin = worldactionplacecell(absolutetarget, orient);
                selinfo placed;
                worldactionselection(placed, placedorigin, orient);
                if(!worldselectionready(placed)) return false;
                mpeditface(-1, 1, sel, false);
                paintworldcube(type, placed, false);
                waterterrainchanged(absoluteplacedorigin);
                if(!waitforserveredit())
                {
                    setlocalsupportpersistent(absoluteplacedorigin, false);
                    queuefallblockcheck(absoluteplacedorigin);
                    queuesupportchange(absoluteplacedorigin);
                }
                return true;
            }
            case WORLD_ACTION_PLACE_SCATTER:
                return getworlditemtype(item) == WORLD_ITEM_SCATTER && applyworldscatteraction(item, target, orient, true);
            case WORLD_ACTION_PLACE_ITEM:
            {
                if(getworlditemtype(item) != WORLD_ITEM_PLACEABLE || !applyworldscatteraction(item, target, orient, true)) return false;
                int slots = 0;
                if(getworldchestconfig(item, slots)) setchestvisual(worldactionplacecell(absolutetarget, orient), worldplaceyaw(packed));
                return true;
            }
            case WORLD_ACTION_BREAK_CUBE_START:
                mpdelcube(sel, false);
                waterterrainchanged(absolutetarget);
                if(!waitforserveredit())
                {
                    setlocalsupportpersistent(absolutetarget, false);
                    queuefallblockcheck(ivec(absolutetarget).add(ivec(0, 0, 16)));
                    queuesupportchange(absolutetarget);
                }
                return true;
            case WORLD_ACTION_BREAK_SCATTER_START:
            {
                if((getworlditemtype(item) != WORLD_ITEM_SCATTER && getworlditemtype(item) != WORLD_ITEM_PLACEABLE) ||
                   !applyworldscatteraction(item, target, orient, false)) return false;
                int slots = 0;
                if(getworldchestconfig(item, slots)) removechestvisual(worldactionplacecell(absolutetarget, orient));
                return true;
            }
            default:
                return false;
        }
    }

    static void rollbackworldaction(const predictedworldaction &prediction)
    {
        if(prediction.action == WORLD_ACTION_PLACE_CUBE)
        {
            ivec target = worldactionplacecell(prediction.target, prediction.orient);
            selinfo sel;
            worldactionselection(sel, target, prediction.orient);
            worldselectiontolocal(sel);
            mpdelcube(sel, false);
            waterterrainchanged(target);
        }
        else if(prediction.action == WORLD_ACTION_PLACE_SCATTER || prediction.action == WORLD_ACTION_PLACE_ITEM)
        {
            const int orient = worldmountorient(prediction.orient);
            selinfo sel;
            worldactionselection(sel, prediction.target, orient);
            worldselectiontolocal(sel);
            editworldscatter(getworlditemindex(prediction.item), sel.o, orient, false);
            int slots = 0;
            if(getworldchestconfig(prediction.item, slots)) removechestvisual(worldactionplacecell(prediction.target, orient));
        }
        else if(prediction.action == WORLD_ACTION_BREAK_CUBE_START)
        {
            ivec target = prediction.target,
                 support = target;
            const int dimension = prediction.orient >> 1;
            support[dimension] += prediction.orient&1 ? -16 : 16;
            applyworldaction(WORLD_ACTION_PLACE_CUBE, support, prediction.orient, prediction.item);
        }
        else if(prediction.action == WORLD_ACTION_BREAK_SCATTER_START)
        {
            selinfo sel;
            worldactionselection(sel, prediction.target, prediction.orient);
            worldselectiontolocal(sel);
            const int type = prediction.item >= 0 ? getworlditemindex(prediction.item) : getworldscatterindexat(sel.o, prediction.orient);
            if(type >= 0)
            {
                editworldscatter(type, sel.o, prediction.orient, true);
                int slots = 0;
                if(getworldchestconfig(prediction.item, slots))
                    setchestvisual(worldactionplacecell(prediction.target, prediction.orient), prediction.yaw);
            }
        }
    }
#endif

    #ifndef STANDALONE
    static void putvslot(packetbuf &p, int index)
    {
        vector<uchar> buf;
        packvslot(buf, index);
        if(buf.length()) p.put(buf.getbuf(), buf.length());
    }

    static void putvslot(packetbuf &p, const VSlot *vs)
    {
        vector<uchar> buf;
        packvslot(buf, vs);
        if(buf.length()) p.put(buf.getbuf(), buf.length());
    }
    #endif

    bool addmsg(int type, const char *fmt, ...)
    {
#ifdef STANDALONE
        return false;
#else
        if(!fmt) fmt = "";
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, type);
        va_list args;
        va_start(args, fmt);
        while(*fmt) switch(*fmt++)
        {
            case 'r': break;
            case 'c':
            {
                (void)va_arg(args, gameent *);
                break;
            }
            case 'i':
            {
                int n = isdigit(*fmt) ? *fmt++ - '0' : 1;
                loopi(n) putint(p, va_arg(args, int));
                break;
            }
            case 'f':
            {
                int n = isdigit(*fmt) ? *fmt++ - '0' : 1;
                loopi(n) putfloat(p, (float)va_arg(args, double));
                break;
            }
            case 's':
                sendstring(va_arg(args, const char *), p);
                break;
        }
        va_end(args);
        sendclientpacket(p.finalize(), 1);
        return true;
#endif
    }

    bool waitforserveredit()
    {
#ifdef STANDALONE
        return false;
#else
        return !localworldactive && (remote || (connected && isconnected(false, true)));
#endif
    }

    bool islocalworld()
    {
        return localworldactive;
    }

    void requestworldcommand(const char *command)
    {
        if(!waitforserveredit())
        {
            conoutf(CON_ERROR, "server command is only available in multiplayer");
            return;
        }
        addmsg(N_SERVERCOMMAND, "rs", command ? command : "");
    }

    void parseoptions(vector<const char *> &args)
    {
        loopv(args) conoutf(CON_ERROR, "unknown command-line option: %s", args[i]);
    }

    const char *gameident() { return "CubeCraft"; }

#ifndef STANDALONE
    const char *gameconfig() { return "config/game.cfg"; }
    const char *savedconfig() { return "config/saved.cfg"; }
    const char *restoreconfig() { return "config/restore.cfg"; }
    const char *defaultconfig() { return "config/default.cfg"; }
    const char *autoexec() { return "config/autoexec.cfg"; }
    const char *savedservers() { return "config/servers.cfg"; }
    void loadconfigs()
    {
        execute("if (|| (=s (getbind F2) []) (=s (getbind F2) [togglevar debughud])) [bind F2 [toggleui debughud]]");
        loopi(7)
        {
            defformatstring(command,
                "if (|| (=s (getbind %d) []) (=s (getbind %d) [creativeselect %d])) [bind %d [creativehotbarselect %d]]",
                i + 1, i + 1, i, i + 1, i);
            execute(command);
        }
        execute("if (=s (getbind 8) []) [bind 8 [creativehotbarselect 7]]");
        execute("if (|| (=s (getbind 9) []) (=s (getbind 9) [if (allowthirdperson) [togglevar thirdperson]])) "
                "[bind 9 [creativehotbarselect 8]]");
        execute("if (=s (getbind F5) []) [bindvar F5 [thirdperson] [allowthirdperson]]");
    }

    void initclient()
    {
        player1 = new gameent;
        copystring(player1->name, "camera");
        players.add(player1);
    }

    void resetgamestate() {}
    static void clearclients()
    {
        loopv(clients) if(clients[i]) delete clients[i];
        clients.setsize(0);
        players.setsize(0);
        if(player1) players.add(player1);
        cleardynentcache();
    }

    void gamedisconnect(bool cleanup)
    {
        connected = remote = false;
#ifndef STANDALONE
        resetnpcs();
        if(player1 && player1->state == CS_DEAD) hidedeathscreen();
        clearplayerragdoll(player1);
#endif
        predictedworldactions.deletecontents();
        resetworlddrops();
        resetfallingblocks();
        resetlocalsupportblocks();
        resetfurnaces();
        resetchests();
        nextworldrequestid = 1;
        resetsurvivalinventory();
        receiveserversettings(5000, 250, 1024, 128, 4000, 128);
        resetwatersimulationsettings();
#ifndef STANDALONE
        resetclientreceive();
#endif
        localworldactive = false;
        clearclients();
        if(player1)
        {
            player1->clientnum = -1;
            player1->privilege = PRIV_NONE;
            player1->health = PLAYER_MAX_HEALTH;
            player1->state = player1->editstate = CS_ALIVE;
            player1->collidetype = COLLIDE_ELLIPSE;
        }
#ifndef STANDALONE
        if(editmode) toggleedit(true);
#endif
        lastpositionsend = -1000;
        sentname[0] = '\0';
    }
    void connectattempt(const char *name, const char *password, const ENetAddress &address) { copystring(connectpass, password ? password : ""); }
    void connectfail() {}

    void gameconnect(bool _remote)
    {
        // Explicitly connecting starts an authoritative multiplayer session.
        // Saved procedural worlds remain offline until this point.
        localworldactive = false;
        remote = _remote;
        if(remote) addmsg(N_CONNECT, "rs", connectpass);
        else connected = true;
    }

    void beginlocalworld()
    {
#ifndef STANDALONE
        // A listen server owns a separate session seed.
        if(isconnected(false, false)) disconnect(false, false);
        if(isconnected(false, true)) server::localdisconnect(false);
#endif
        connected = remote = false;
        localworldactive = true;
        resetwatersimulationsettings();
        resetfallingblocks();
        resetlocalsupportblocks();
#ifndef STANDALONE
        resetclientreceive();
#endif
        if(player1)
        {
            player1->clientnum = -1;
            player1->privilege = PRIV_ADMIN;
        }
    }

    bool allowedittoggle()
    {
        // Always permit leaving edit mode, including after privilege is lost.
        if(editmode) return true;
        if(player1 && player1->privilege >= PRIV_ADMIN) return true;
        conoutf(CON_ERROR, "full edit mode requires admin privilege");
        return false;
    }

    void edittoggled(bool on)
    {
        addmsg(N_EDITMODE, "ri", on ? 1 : 0);
    }

    void writeclientinfo(stream *f)
    {
        if(player1) f->printf("name %s\n", escapestring(player1->name));
    }

    void toserver(char *text)
    {
        conoutf("%s", text);
        addmsg(N_TEXT, "rs", text);
    }

    void changemap(const char *name, int mode)
    {
        gamemode = m_valid(mode) ? mode : STARTGAMEMODE;
#ifndef STANDALONE
        if(!localworldactive && !remote && !isconnected()) localconnect();
#endif
        if(editmode) toggleedit();
        if(name && name[0]) load_world(name);
        else emptymap(0, true, NULL);
    }

    void changemap(const char *name) { changemap(name, STARTGAMEMODE); }
    bool validgamemode(int mode) { return m_valid(mode); }
    void forceedit(const char *name) { if(name && name[0]) copystring(clientmap, name); }
    bool ispaused() { return gamepaused; }
    int scaletime(int t) { return t*100; }
    bool allowmouselook() { return true; }

    VARP(smoothmove, 0, 75, 100);
    VARP(smoothdist, 0, 32, 64);

    static void predictplayer(gameent *d)
    {
        d->o = d->newpos;
        d->yaw = d->newyaw;
        d->pitch = d->newpitch;
        d->roll = d->newroll;
        moveplayer(d, 1, false);
        d->newpos = d->o;

        float k = 1.0f - float(lastmillis - d->smoothmillis)/smoothmove;
        if(k <= 0) return;
        d->o.add(vec(d->deltapos).mul(k));
        d->yaw += d->deltayaw*k;
        if(d->yaw < 0) d->yaw += 360;
        else if(d->yaw >= 360) d->yaw -= 360;
        d->pitch += d->deltapitch*k;
        d->roll += d->deltaroll*k;
    }

    static void otherplayers()
    {
        loopv(players)
        {
            gameent *d = players[i];
            if(d == player1) continue;

            int lagtime = (totalmillis ? totalmillis : 1) - d->lastupdate;
            if(!lagtime) continue;
            if(lagtime > 1000 && d->state == CS_ALIVE)
            {
                d->state = CS_LAGGED;
                continue;
            }
            if(d->state == CS_ALIVE || d->state == CS_EDITING)
            {
                if(smoothmove && d->smoothmillis > 0) predictplayer(d);
                else moveplayer(d, 1, false);
            }
        }
    }

    static bool applynetworkedit(networkedit &edit)
    {
        if(edit.type == N_WORLDAUTH)
        {
            if(player1 && edit.author == player1->clientnum && edit.requestid && findpredictedworldaction(edit.requestid)) return true;
            return applyworldaction(edit.args[0], ivec(edit.args[1], edit.args[2], edit.args[3]), edit.args[4], edit.args[5]);
        }

        selinfo sel = edit.selection;
        worldselectiontolocal(sel);
        if(!sel.validate() || !worldselectionready(sel)) return false;

        switch(edit.type)
        {
            case N_EDITF:
                if(edit.args[1] == 2 && sel.cx == -1) pushworldcubecorner(sel, false);
                else mpeditface(edit.args[0], edit.args[1], sel, false);
                break;
            case N_EDITT:
            {
                ucharbuf extra(edit.extra.getbuf(), edit.extra.length());
                mpedittex(edit.args[0], edit.args[1], sel, extra);
                break;
            }
            case N_EDITM: mpeditmat(edit.args[0], edit.args[1], sel, false); break;
            case N_FLIP: mpflip(sel, false); break;
            case N_ROTATE: mprotate(edit.args[0], sel, false); break;
            case N_REPLACE:
            {
                ucharbuf extra(edit.extra.getbuf(), edit.extra.length());
                mpreplacetex(edit.args[0], edit.args[1], edit.args[2] > 0, sel, extra);
                break;
            }
            case N_DELCUBE: mpdelcube(sel, false); break;
            case N_EDITSCATTER:
                return editworldscatter(edit.args[0], sel.o, sel.orient,
                                        edit.args[1] != 0);
            case N_EDITVSLOT:
            {
                ucharbuf extra(edit.extra.getbuf(), edit.extra.length());
                mpeditvslot(edit.args[0], edit.args[1], sel, extra);
                break;
            }
            default: return true;
        }
        return true;
    }

    void processnetworkedits()
    {
        if(pendingnetworkworld) return;
        for(int i = 0; i < pendingnetworkedits.length();)
        {
            networkedit *edit = pendingnetworkedits[i];
            if(!applynetworkedit(*edit))
            {
                ++i;
                continue;
            }
            delete edit;
            pendingnetworkedits.remove(i);
        }
    }

    void updateworld()
    {
#ifndef STANDALONE
        if(pendingnetworkworld)
        {
            const int seed = pendingnetworkseed, timemillis = pendingnetworktime;
            const bool frozen = pendingnetworkfrozen,
                       restoreposition = pendingnetworkrestoreposition;
            const vec savedposition = pendingnetworkposition;
            const vec savedvelocity = pendingnetworkvelocity, savedfalling = pendingnetworkfalling;
            const float savedfalldistance = pendingnetworkfalldistance;
            const int savedphysstate = pendingnetworkphysstate;
            const int savedyaw = pendingnetworkyaw, savedpitch = pendingnetworkpitch;
            pendingnetworkreset = pendingnetworkrestoreposition = false;

            // Keep the server lighting active for the entire network-world load.
            // startmap() sees pendingnetworkworld and preserves this authoritative
            // time instead of briefly installing the local default lighting.
            environment::synctime(timemillis, frozen);
            startnetworkworld(seed);
            if(restoreposition && player1)
            {
                vec restored = savedposition;
                worldpositiontolocal(restored);
                player1->o = restored;
                player1->yaw = savedyaw;
                player1->pitch = savedpitch;
                restorelocalplayermotion(savedvelocity, savedfalling, savedfalldistance, savedphysstate);
                player1->resetinterp();
                updateworldchunks(true);
            }
            environment::synctime(timemillis, frozen);
            pendingnetworkworld = false;
            vec worldspawn;
            float worldspawnyaw = 0, worldspawnpitch = 0;
            if(getpreparedworldspawn(worldspawn, worldspawnyaw, worldspawnpitch))
                addmsg(N_WORLDREADY, "ri5", int(worldspawn.x * DMF), int(worldspawn.y * DMF), int(worldspawn.z * DMF),
                       int(worldspawnyaw), int(worldspawnpitch));
            else addmsg(N_WORLDREADY, "ri5", 0, 0, 0, 0, 0);
            requestnetworkworldchunkvalidation();
        }
        environment::update();
#endif
        updateworldchunks();
        processnetworkedits();
        physicsframe();
#ifndef STANDALONE
        updatenpcs();
#endif
        otherplayers();
        if(player1 && (player1->state == CS_ALIVE || player1->state == CS_EDITING))
        {
            crouchplayer(player1, 10, true);
            moveplayer(player1, 10, true);
            updateworldchunks();
        }
#ifndef STANDALONE
        updatewatersimulation();
        updatesurvivalbreaking();
        updatefooduse();
        updateworlddrops();
        updatefallingblocks();
        updatesupportblocks();
        updatefurnaces();
        updatechests();
#endif
        gets2c();
        c2sinfo();
    }

    void physicstrigger(physent *d, bool local, int floorlevel, int waterlevel, int material) {}

    void falltrigger(physent *d, bool local, float distance, float velocity)
    {
        if(!local || !m_survival || !d || d->state != CS_ALIVE) return;
        const int damage = fallimpactdamage(distance);
        if(damage <= 0) return;
        if(d == player1)
        {
            if(!waitforserveredit()) damageplayer(float(damage), vec(d->o).addz(distance));
            return;
        }
#ifndef STANDALONE
        if(!waitforserveredit()) damagefallingnpc(d, float(damage));
#endif
    }
    void bounced(physent *d, const vec &surface) {}

    void edittrigger(const selinfo &sel, int op, int arg1, int arg2, int arg3, const VSlot *vs)
    {
        if(remote && op == EDIT_COPY) return;
        if(!connected && !remote && !isconnected(false, true)) return;
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_EDITF + op);
        switch(op)
        {
            case EDIT_CALCLIGHT:
            case EDIT_REMIP:
                break;

            case EDIT_UNDO:
            case EDIT_REDO:
            {
                uchar *outbuf = NULL;
                int inlen = 0, outlen = 0;
                if(packundo(op, inlen, outbuf, outlen))
                {
                    putint(p, inlen);
                    putint(p, outlen);
                    if(outlen > 0) p.put(outbuf, outlen);
                    delete[] outbuf;
                }
                break;
            }

            default:
            {
                selinfo networksel = sel;
                if(waitforserveredit()) worldselectiontoabsolute(networksel);
                putsel(p, networksel);
                switch(op)
                {
                    case EDIT_FACE: case EDIT_MAT:
                        putint(p, arg1); putint(p, arg2);
                        break;
                    case EDIT_ROTATE:
                        putint(p, arg1);
                        break;
                    case EDIT_TEX:
                    {
                        int tex1 = shouldpacktex(arg1);
                        putint(p, tex1 ? tex1 : arg1); putint(p, arg2);
                        p.pad(2);
                        int offset = p.length();
                        if(tex1) putvslot(p, arg1);
                        *(ushort *)&p.buf[offset-2] = lilswap(ushort(p.length() - offset));
                        break;
                    }
                    case EDIT_REPLACE:
                    {
                        int tex1 = shouldpacktex(arg1), tex2 = shouldpacktex(arg2);
                        putint(p, tex1 ? tex1 : arg1); putint(p, tex2 ? tex2 : arg2); putint(p, arg3);
                        p.pad(2);
                        int offset = p.length();
                        if(tex1) putvslot(p, arg1);
                        if(tex2) putvslot(p, arg2);
                        *(ushort *)&p.buf[offset-2] = lilswap(ushort(p.length() - offset));
                        break;
                    }
                    case EDIT_VSLOT:
                        putint(p, arg1); putint(p, arg2);
                        p.pad(2);
                        {
                            int offset = p.length();
                            putvslot(p, vs);
                            *(ushort *)&p.buf[offset-2] = lilswap(ushort(p.length() - offset));
                        }
                        break;
                }
                break;
            }
        }
        sendclientpacket(p.finalize(), 1);
    }

    static bool scatteredittrigger(int type, const ivec &support, int orient, bool place)
    {
        if(!waitforserveredit())
            return editworldscatter(type, support, orient, place);
        selinfo sel;
        sel.o = support;
        sel.s = ivec(1, 1, 1);
        sel.grid = 16;
        sel.orient = orient;
        sel.cx = sel.cy = sel.corner = 0;
        sel.cxs = sel.cys = 2;
        worldselectiontoabsolute(sel);
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_EDITSCATTER);
        putsel(p, sel);
        putpersistentid(p, getworldscatterpersistentid(type));
        putint(p, place ? 1 : 0);
        sendclientpacket(p.finalize(), 1);
        return true;
    }

    void vartrigger(ident *id)
    {
        if(!id || (!connected && !remote && !isconnected(false, true))) return;
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_EDITVAR);
        putint(p, id->type);
        sendstring(id->name, p);
        switch(id->type)
        {
            case ID_VAR: putint(p, *id->storage.i); break;
            case ID_FVAR: putfloat(p, *id->storage.f); break;
            case ID_SVAR: sendstring(*id->storage.s, p); break;
        }
        sendclientpacket(p.finalize(), 1);
    }

    void dynentcollide(physent *d, physent *o, const vec &dir) {}
    const char *getclientmap() { return clientmap; }
    int findclientnum(const char *name)
    {
        if(!name || !name[0]) return -1;
        char *end = NULL;
        long numeric = strtol(name, &end, 10);
        if(end != name && !*end) return int(numeric);
        loopv(players) if(players[i] && !cubecasecmp(players[i]->name, name))
            return players[i]->clientnum;
        loopv(clients) if(clients[i] && !cubecasecmp(clients[i]->name, name))
            return clients[i]->clientnum;
        return -1;
    }
    const char *getmapinfo() { return NULL; }
    const char *getscreenshotinfo() { return clientmap; }
    void suicide(physent *d) {}
    void newmap(int size)
    {
#ifndef STANDALONE
        if(!initing)
        {
            connected = true;
            mainmenu = 0;
            if(!editmode) toggleedit(true);
        }
#endif
        if(isconnected(false, true)) addmsg(N_NEWMAP, "ri", size);
    }

    void startmap(const char *name)
    {
#ifndef STANDALONE
        resetlocalpassivenpcstates();
        resetnpcs();
        resetwatersimulation();
        if(!pendingnetworkworld) resetworlddrops();
        if(!pendingnetworkworld) resetfallingblocks();
        if(!pendingnetworkworld) resetlocalsupportblocks();
#endif
        copystring(clientmap, name ? name : "");
#ifndef STANDALONE
        if(pendingnetworkworld) environment::synctime(pendingnetworktime, pendingnetworkfrozen);
        else environment::reset();
        if(!initing)
        {
            if(!localworldactive && !remote && !isconnected()) localconnect();
            mainmenu = 0;
        }
#endif
        findplayerspawn(player1, -1, 0);
        if(player1)
        {
#ifndef STANDALONE
            if(player1->state == CS_DEAD) hidedeathscreen();
            clearplayerragdoll(player1);
#endif
            player1->health = PLAYER_MAX_HEALTH;
            player1->state = player1->editstate = CS_ALIVE;
            player1->collidetype = COLLIDE_ELLIPSE;
            player1->renderbodyyawmillis = -1;
            player1->rendercrouchmillis = -1;
            player1->renderstridemillis = -1;
            player1->renderattacking = false;
            player1->rendereating = false;
            player1->rendereatitem = -1;
            player1->renderattackreleasemillis = -1000;
            player1->renderplacemillis = -1000;
            player1->renderactioninitialized = true;
        }
    }
    void preload()
    {
        entities::preloadentities();
#ifndef STANDALONE
        preloadplayermodels();
        preloadnpcs();
#endif
    }
    float abovegameplayhud(int w, int h) { return 1.0f; }

    enum
    {
        CREATIVE_GRID = 16,
        CREATIVE_REACH = CREATIVE_GRID * 8
    };

    enum
    {
        CREATIVE_HOTBAR_SLOTS = 9
    };

    static int creativehotbar[CREATIVE_HOTBAR_SLOTS] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    static int creativehotbarslot = 0;
    static int survivalitems[SURVIVAL_USABLE_SLOTS] =
    {
        -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1
    };
    static int survivalcounts[SURVIVAL_USABLE_SLOTS] = { 0 };
    static int survivaldurabilities[SURVIVAL_USABLE_SLOTS] = { 0 };
    static int craftingitems[CRAFT_GRID_MAX] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    static int craftingcounts[CRAFT_GRID_MAX] = { 0 };
    static int craftingdurabilities[CRAFT_GRID_MAX] = { 0 };
    static int craftinggridsize = 2, craftingstationitem = -1, craftingrecipe = -1,
               craftingoutputitem = -1, craftingoutputcount = 0, inventorycursoritem = -1, inventorycursorcount = 0,
               inventorycursordurability = 0;
    static vector<furnaceinstance *> localfurnaces;
    static furnaceinstance synchronizedfurnace;
    static ivec openfurnacetarget(0, 0, 0);
    static bool furnaceopen = false, synchronizedfurnacecooking = false;
    static int furnacesyncmillis = 0;
    static vector<chestinstance *> localchests;
    static vector<ivec> localchestrestorepending;
    static chestinstance synchronizedchest;
    static ivec openchesttarget(0, 0, 0);
    static bool chestopen = false;

    struct chestvisual
    {
        ivec target;
        int yaw, started;
        float fromangle, toangle;

        chestvisual(const ivec &target = ivec(0, 0, 0), int yaw = 0)
            : target(target), yaw(yaw), started(lastmillis), fromangle(0), toangle(0) {}
    };

    static vector<chestvisual> chestvisuals;
    static const int CHEST_LID_MILLIS = 250;

    static chestvisual *findchestvisual(const ivec &target)
    {
        loopv(chestvisuals) if(chestvisuals[i].target == target) return &chestvisuals[i];
        return NULL;
    }

    static float chestvisualangle(const chestvisual &visual)
    {
        const float progress = clamp((lastmillis - visual.started) / float(CHEST_LID_MILLIS), 0.0f, 1.0f);
        return visual.fromangle + (visual.toangle - visual.fromangle) * progress;
    }

    static void setchestvisual(const ivec &target, int yaw)
    {
        chestvisual *visual = findchestvisual(target);
        if(!visual) visual = &chestvisuals.add(chestvisual(target, yaw));
        visual->yaw = ((yaw % 360) + 360) % 360;
    }

    static void removechestvisual(const ivec &target)
    {
        loopv(chestvisuals) if(chestvisuals[i].target == target)
        {
            chestvisuals.remove(i);
            return;
        }
    }

    int getchestyaw(const ivec &target)
    {
        chestvisual *visual = findchestvisual(target);
        return visual ? visual->yaw : 0;
    }

    float getchestlidangle(const ivec &target)
    {
        chestvisual *visual = findchestvisual(target);
        return visual ? chestvisualangle(*visual) : 0.0f;
    }

    void receivechestanimation(const ivec &target, bool open)
    {
        chestvisual *visual = findchestvisual(target);
        if(!visual)
        {
            setchestvisual(target, 0);
            visual = findchestvisual(target);
        }
        visual->fromangle = chestvisualangle(*visual);
        visual->toangle = open ? 80.0f : 0.0f;
        visual->started = lastmillis;
    }

    enum
    {
        CREATIVE_ARM_RELEASE = 120,
        SURVIVAL_BREAK_STAGES = 8,
        SURVIVAL_BREAK_PARTICLE_MILLIS = 125,
        SURVIVAL_STACK_SIZE = 64
    };

    static const float CREATIVE_ARM_PITCH = 70.0f;
    static int authoritativebreakmillis = 5000, authoritativescatterbreakmillis = 250;

    static void sendworldaction(uint requestid, int action, const ivec &localtarget, int orient, int item, int slot)
    {
        selinfo selection;
        worldactionselection(selection, localtarget, worldmountorient(orient));
        if(waitforserveredit()) worldselectiontoabsolute(selection);
        packetbuf p(MAXTRANS, ENET_PACKET_FLAG_RELIABLE);
        putint(p, N_WORLDACTION);
        putint(p, int(requestid)); putint(p, action);
        putint(p, selection.o.x); putint(p, selection.o.y); putint(p, selection.o.z);
        putint(p, orient);
        putpersistentid(p, getinventoryitempersistentid(item));
        putint(p, slot);
        sendclientpacket(p.finalize(), 1);
    }

    static void addpredictedworldaction(uint requestid, int action, const ivec &absolutetarget, int orient, int item)
    {
        predictedworldaction *prediction = new predictedworldaction;
        prediction->requestid = requestid;
        prediction->action = action;
        prediction->target = absolutetarget;
        prediction->orient = orient;
        prediction->item = item;
        int slots = 0;
        if(getworldchestconfig(item, slots))
            prediction->yaw = getchestyaw(worldactionplacecell(absolutetarget, worldmountorient(orient)));
        predictedworldactions.add(prediction);
    }

    static uint predictworldaction(int action, const ivec &localtarget, int orient, int item, int slot)
    {
        const uint requestid = newworldrequestid();
        selinfo selection;
        worldactionselection(selection, localtarget, worldmountorient(orient));
        if(waitforserveredit()) worldselectiontoabsolute(selection);
        addpredictedworldaction(requestid, action, selection.o, orient, item);
        sendworldaction(requestid, action, localtarget, orient, item, slot);
        return requestid;
    }

    static int clampcreativehotbarslot()
    {
        creativehotbarslot = clamp(creativehotbarslot, 0, CREATIVE_HOTBAR_SLOTS - 1);
        return creativehotbarslot;
    }

    int selectedcreativeblock()
    {
        const int slot = clampcreativehotbarslot(),
                  item = m_survival ? survivalitems[slot] : creativehotbar[slot],
                  count = numinventoryitems();
        if(m_survival && survivalcounts[slot] <= 0) return -1;
        return item >= 0 && item < count ? item : -1;
    }

    void wearselectedsurvivaltool()
    {
        if(!m_survival) return;
        const int slot = clampcreativehotbarslot(), item = survivalitems[slot];
        if(survivalcounts[slot] <= 0 || survivaldurabilities[slot] <= 0 || !isinventorytool(item)) return;

        if(--survivaldurabilities[slot] <= 0)
        {
            survivalitems[slot] = -1;
            survivalcounts[slot] = survivaldurabilities[slot] = 0;
        }
    }

    void resetsurvivalinventory()
    {
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            survivalitems[i] = -1;
            survivalcounts[i] = 0;
            survivaldurabilities[i] = 0;
        }
        creativehotbarslot = 0;
        loopi(CRAFT_GRID_MAX)
        {
            craftingitems[i] = -1;
            craftingcounts[i] = 0;
            craftingdurabilities[i] = 0;
        }
        craftinggridsize = 2;
        craftingstationitem = craftingrecipe = craftingoutputitem = -1;
        craftingoutputcount = 0;
        inventorycursoritem = -1;
        inventorycursorcount = 0;
        inventorycursordurability = 0;
    }

    void loadsurvivalinventory(const int *items, const int *counts, const int *durabilities, int slots, int cursoritem, int cursorcount, int cursordurability)
    {
        resetsurvivalinventory();
        loopi(min(slots, int(SURVIVAL_USABLE_SLOTS)))
        {
            if(!items || !counts || items[i] < 0 || items[i] >= numinventoryitems() || counts[i] <= 0) continue;
            const int item = items[i];
            survivalitems[i] = item;
            survivalcounts[i] = clamp(counts[i], 1, max(getinventoryitemmaxstack(item), 1));
            survivaldurabilities[i] = isinventorytool(item)
                                    ? clamp(durabilities && durabilities[i] > 0 ? durabilities[i] : getinventorytoolmaxdurability(item),
                                            1, getinventorytoolmaxdurability(item)) : 0;
        }
        if(cursoritem >= 0 && cursoritem < numinventoryitems() && cursorcount > 0)
        {
            inventorycursoritem = cursoritem;
            inventorycursorcount = clamp(cursorcount, 1, max(getinventoryitemmaxstack(cursoritem), 1));
            inventorycursordurability = isinventorytool(cursoritem) ? clamp(cursordurability > 0 ? cursordurability : getinventorytoolmaxdurability(cursoritem), 1, getinventorytoolmaxdurability(cursoritem)) : 0;
        }
    }

    void receiveinventory(const int *items, const int *counts, const int *durabilities, int slots, int selected, int cursoritem, int cursorcount, int cursordurability)
    {
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            survivalitems[i] = i < slots && items[i] >= 0 && counts[i] > 0 ? items[i] : -1;
            survivalcounts[i] = i < slots && items[i] >= 0 && counts[i] > 0 ? clamp(counts[i], 1, max(getinventoryitemmaxstack(items[i]), 1)) : 0;
            survivaldurabilities[i] = survivalitems[i] >= 0 && isinventorytool(survivalitems[i]) ? clamp(durabilities[i], 1, getinventorytoolmaxdurability(survivalitems[i])) : 0;
        }
        inventorycursoritem = cursoritem >= 0 && cursoritem < numinventoryitems() && cursorcount > 0 ? cursoritem : -1;
        inventorycursorcount = inventorycursoritem >= 0 ? clamp(cursorcount, 1, max(getinventoryitemmaxstack(inventorycursoritem), 1)) : 0;
        inventorycursordurability = inventorycursoritem >= 0 && isinventorytool(inventorycursoritem) ? clamp(cursordurability, 1, getinventorytoolmaxdurability(inventorycursoritem)) : 0;
        creativehotbarslot = clamp(selected, 0, CREATIVE_HOTBAR_SLOTS - 1);
    }

    void receivecraftstate(const int *items, const int *counts, const int *durabilities, int slots, int gridsize, int stationitem, int recipe, int outputitem, int outputcount)
    {
        loopi(CRAFT_GRID_MAX)
        {
            craftingitems[i] = i < slots && items[i] >= 0 && counts[i] > 0 ? items[i] : -1;
            craftingcounts[i] = i < slots && items[i] >= 0 && counts[i] > 0 ? counts[i] : 0;
            craftingdurabilities[i] = craftingitems[i] >= 0 && isinventorytool(craftingitems[i]) ? clamp(durabilities[i], 1, getinventorytoolmaxdurability(craftingitems[i])) : 0;
        }
        craftinggridsize = gridsize == 3 ? 3 : 2;
        craftingstationitem = stationitem;
        craftingrecipe = recipe;
        craftingoutputitem = outputitem;
        craftingoutputcount = max(outputcount, 0);
    }

    static void updateclientcraftpreview()
    {
        craftmatch match;
        if(matchcraftrecipe(craftingitems, craftingcounts, craftinggridsize, craftingstationitem, -1, 0, -1, match))
        {
            craftingrecipe = match.recipe;
            craftingoutputitem = match.outputitem;
            craftingoutputcount = match.outputcount;
        }
        else
        {
            craftingrecipe = craftingoutputitem = -1;
            craftingoutputcount = 0;
        }
    }

    static void requestcraftaction(int action, int first = 0, int second = 0, int third = 0, int fourth = 0)
    {
#ifndef STANDALONE
        if(waitforserveredit()) addmsg(N_CRAFTACTION, "ri6", int(newworldrequestid()), action, first, second, third, fourth);
#else
        (void)action; (void)first; (void)second; (void)third; (void)fourth;
#endif
    }

    static void requestfurnaceaction(int action, int first = 0, int second = 0, int third = 0, int fourth = 0)
    {
#ifndef STANDALONE
        if(waitforserveredit()) addmsg(N_FURNACEACTION, "ri6", int(newworldrequestid()), action, first, second, third, fourth);
#else
        (void)action; (void)first; (void)second; (void)third; (void)fourth;
#endif
    }

    static void requestchestaction(int action, int first = 0, int second = 0, int third = 0, int fourth = 0)
    {
#ifndef STANDALONE
        if(waitforserveredit()) addmsg(N_CHESTACTION, "ri6", int(newworldrequestid()), action, first, second, third, fourth);
#else
        (void)action; (void)first; (void)second; (void)third; (void)fourth;
#endif
    }

    static chestinstance *findlocalchest(const ivec &target)
    {
        loopv(localchests) if(localchests[i]->target == target) return localchests[i];
        return NULL;
    }

    static void droplocalchestcontents(const chestinstance &chest);

    static void removelocalchest(const ivec &target, bool dropcontents = true)
    {
        const int pending = localchestrestorepending.find(target);
        if(pending >= 0) localchestrestorepending.removeunordered(pending);
        loopv(localchests) if(localchests[i]->target == target)
        {
            chestinstance *chest = localchests[i];
            if(dropcontents) droplocalchestcontents(*chest);
            if(chestopen && openchesttarget == target)
            {
                chestopen = false;
#ifndef STANDALONE
                execute("hideui chest");
#endif
            }
            delete localchests.remove(i);
            removechestvisual(target);
            return;
        }
        removechestvisual(target);
    }

    static chestinstance *currentchest()
    {
        if(!chestopen) return NULL;
        return waitforserveredit() ? &synchronizedchest : findlocalchest(openchesttarget);
    }

    static furnaceinstance *findlocalfurnace(const ivec &target)
    {
        loopv(localfurnaces) if(localfurnaces[i]->target == target) return localfurnaces[i];
        return NULL;
    }

    static void removelocalfurnace(const ivec &target)
    {
        loopv(localfurnaces) if(localfurnaces[i]->target == target)
        {
            delete localfurnaces.remove(i);
            if(furnaceopen && openfurnacetarget == target)
            {
                furnaceopen = false;
#ifndef STANDALONE
                execute("hideui furnace");
#endif
            }
            return;
        }
    }

    static furnaceinstance *currentfurnace()
    {
        if(!furnaceopen) return NULL;
        return waitforserveredit() ? &synchronizedfurnace : findlocalfurnace(openfurnacetarget);
    }

    static bool limitedinventoryclick(int &cursoritem, int &cursorcount, int &cursordurability, int &slotitem, int &slotcount,
                                      int &slotdurability, int button, int slotlimit)
    {
        if(button != INVENTORY_CLICK_LEFT && button != INVENTORY_CLICK_RIGHT) return false;
        const int oldcursoritem = cursoritem, oldcursorcount = cursorcount, oldcursordurability = cursordurability,
                  oldslotitem = slotitem, oldslotcount = slotcount, oldslotdurability = slotdurability;
        slotlimit = max(slotlimit, 1);
        if(button == INVENTORY_CLICK_LEFT)
        {
            if(cursorcount <= 0)
            {
                if(slotcount <= 0) return false;
                swap(cursoritem, slotitem);
                swap(cursorcount, slotcount);
            }
            else if(slotcount <= 0)
            {
                const int moved = min(cursorcount, slotlimit);
                slotitem = cursoritem;
                slotcount = moved;
                cursorcount -= moved;
            }
            else if(cursoritem != slotitem)
            {
                if(cursorcount > slotlimit) return false;
                swap(cursoritem, slotitem);
                swap(cursorcount, slotcount);
            }
            else
            {
                const int moved = min(cursorcount, slotlimit - slotcount);
                if(moved <= 0) return false;
                slotcount += moved;
                cursorcount -= moved;
            }
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
            if(slotcount > 0 && (slotitem != cursoritem || slotcount >= slotlimit)) return false;
            if(slotcount <= 0) slotitem = cursoritem;
            ++slotcount;
            --cursorcount;
        }
        if(cursorcount <= 0) { cursoritem = -1; cursorcount = 0; }
        if(slotcount <= 0) { slotitem = -1; slotcount = 0; }

        if(cursoritem < 0) cursordurability = 0;
        else if(cursoritem == oldslotitem && oldcursoritem != cursoritem) cursordurability = oldslotdurability;
        else if(cursoritem != oldcursoritem) cursordurability = oldslotdurability;
        if(slotitem < 0) slotdurability = 0;
        else if(slotitem == oldcursoritem && oldslotitem != slotitem) slotdurability = oldcursordurability;
        else if(slotitem != oldslotitem) slotdurability = oldcursordurability;
        (void)oldcursorcount;
        (void)oldslotcount;
        return true;
    }

    static bool takefurnaceoutput(furnaceinstance &furnace, int button)
    {
        if(furnace.outputcount <= 0 || (button != INVENTORY_CLICK_LEFT && button != INVENTORY_CLICK_RIGHT)) return false;
        if(inventorycursorcount > 0 && inventorycursoritem != furnace.outputitem) return false;
        const int capacity = max(getinventoryitemmaxstack(furnace.outputitem), 1) - inventorycursorcount,
                  moved = min(capacity, button == INVENTORY_CLICK_RIGHT ? 1 : furnace.outputcount);
        if(moved <= 0) return false;
        inventorycursoritem = furnace.outputitem;
        inventorycursorcount += moved;
        inventorycursordurability = furnace.outputdurability;
        furnace.outputcount -= moved;
        if(furnace.outputcount <= 0)
        {
            furnace.outputitem = -1;
            furnace.outputcount = furnace.outputdurability = 0;
        }
        return true;
    }

    void receivefurnacestate(const furnaceinstance &furnace, bool open, bool cooking)
    {
        synchronizedfurnace = furnace;
        openfurnacetarget = furnace.target;
        furnaceopen = open;
        synchronizedfurnacecooking = cooking;
        furnacesyncmillis = lastmillis;
#ifndef STANDALONE
        if(open) execute("hideui survival_inventory; hideui crafting_table; hideui chest; showui furnace");
        else execute("hideui furnace");
#endif
    }

    void resetfurnaces()
    {
        localfurnaces.deletecontents();
        synchronizedfurnace = furnaceinstance();
        openfurnacetarget = ivec(0, 0, 0);
        furnaceopen = synchronizedfurnacecooking = false;
        furnacesyncmillis = 0;
    }

    static void updatefurnaces()
    {
        if(waitforserveredit() || !islocalworld()) return;
        loopv(localfurnaces)
        {
            bool syncchanged = false;
            updatefurnaceinstance(*localfurnaces[i], curtime, syncchanged);
        }
    }

    static bool openworldfurnace(const selinfo &hit)
    {
        if(!m_survival) return false;
        const int cube = getworldcubeindexat(ivec(hit.o).add(CREATIVE_GRID / 2), WORLD_ORIENT_TOP), item = getworldcubeitem(cube);
        int inputslots = 0, inputlimit = 0;
        if(!getworldfurnaceconfig(item, inputslots, inputlimit)) return false;
        selinfo absolute = hit;
        worldselectiontoabsolute(absolute);
        openfurnacetarget = absolute.o;
        if(chestopen)
        {
            if(waitforserveredit()) requestchestaction(CHEST_ACTION_CLOSE);
            else receivechestanimation(openchesttarget, false);
            chestopen = false;
        }
        if(waitforserveredit())
        {
            requestfurnaceaction(FURNACE_ACTION_OPEN, absolute.o.x, absolute.o.y, absolute.o.z, 0);
            return true;
        }
        furnaceinstance *furnace = findlocalfurnace(absolute.o);
        if(!furnace)
        {
            furnace = new furnaceinstance(absolute.o, item, inputslots, inputlimit);
            localfurnaces.add(furnace);
        }
        furnaceopen = true;
#ifndef STANDALONE
        execute("hideui survival_inventory; hideui crafting_table; hideui chest; showui furnace");
#endif
        return true;
    }

    static bool openworldchest(int type, const ivec &support, int orient)
    {
        const int item = getworldscatteritem(type);
        int slots = 0;
        if(!getworldchestconfig(item, slots)) return false;
        selinfo selection;
        worldactionselection(selection, support, orient);
        worldselectiontoabsolute(selection);
        const ivec target = worldactionplacecell(selection.o, orient);
        if(furnaceopen)
        {
            if(waitforserveredit()) requestfurnaceaction(FURNACE_ACTION_CLOSE);
            furnaceopen = synchronizedfurnacecooking = false;
        }
        openchesttarget = target;
        if(waitforserveredit())
        {
            requestchestaction(CHEST_ACTION_OPEN, target.x, target.y, target.z, 0);
            return true;
        }
        chestinstance *chest = findlocalchest(target);
        if(!chest)
        {
            chest = new chestinstance(target, item, slots, getchestyaw(target));
            localchests.add(chest);
        }
        chestopen = true;
        receivechestanimation(target, true);
#ifndef STANDALONE
        execute("hideui survival_inventory; hideui crafting_table; hideui furnace; showui chest");
#endif
        return true;
    }

    static bool openworldchest(const selinfo &hit)
    {
        // Mapmodel picking is optional: raycube only sees the supporting voxel when a chest is clicked from most angles.
        const int type = getworldscatterindexat(hit.o, WORLD_ORIENT_TOP);
        return type >= 0 && openworldchest(type, hit.o, WORLD_ORIENT_TOP);
    }

    void receivecheststate(const chestinstance &chest, bool open)
    {
        synchronizedchest = chest;
        openchesttarget = chest.target;
        chestopen = open;
        if(open) setchestvisual(chest.target, chest.yaw);
#ifndef STANDALONE
        if(open) execute("hideui survival_inventory; hideui crafting_table; hideui furnace; showui chest");
        else execute("hideui chest");
#endif
    }

    void resetchests()
    {
        localchests.deletecontents();
        synchronizedchest = chestinstance();
        openchesttarget = ivec(0, 0, 0);
        chestopen = false;
        chestvisuals.setsize(0);
        localchestrestorepending.setsize(0);
    }

    static void updatechests()
    {
        bool animating = false;
        loopv(chestvisuals) if(lastmillis - chestvisuals[i].started < CHEST_LID_MILLIS)
        {
            animating = true;
            break;
        }
        if(animating) updateworldchestanimations();
        if(!waitforserveredit() && islocalworld()) for(int i = localchests.length() - 1; i >= 0; --i)
        {
            chestinstance &chest = *localchests[i];
            selinfo support;
            worldactionselection(support, ivec(chest.target).sub(ivec(0, 0, CREATIVE_GRID)), WORLD_ORIENT_TOP);
            worldselectiontolocal(support);
            if(!support.validate() || !worldselectionready(support)) continue;
            const int type = getworldscatterindexat(support.o, WORLD_ORIENT_TOP);
            const int pending = localchestrestorepending.find(chest.target);
            if(type < 0)
            {
                if(pending >= 0)
                {
                    // The inventory snapshot is the authoritative load-time
                    // record. Repair a missing scatter entry once its
                    // chunk is ready instead of discarding all chest contents.
                    const int worldindex = getworlditemindex(chest.worlditem);
                    if(worldindex >= 0 && editworldscatter(worldindex, support.o, WORLD_ORIENT_TOP, true))
                    {
                        localchestrestorepending.removeunordered(pending);
                        setchestvisual(chest.target, chest.yaw);
                    }
                }
                else removelocalchest(chest.target);
            }
            else if(getworldscatteritem(type) == chest.worlditem)
            {
                if(pending >= 0) localchestrestorepending.removeunordered(pending);
            }
            else removelocalchest(chest.target, false);
        }
        if(!chestopen || !player1) return;
        vec position = player1->o;
        worldpositiontoabsolute(position);
        if(vec(openchesttarget).add(8).dist(position) <= 144.0f) return;
        if(waitforserveredit()) requestchestaction(CHEST_ACTION_CLOSE);
        else receivechestanimation(openchesttarget, false);
        chestopen = false;
#ifndef STANDALONE
        execute("hideui chest");
#endif
    }

    static int authoritativenpcsimulationmaxdist = 128;

    void receiveserversettings(int breakmillis, int scatterbreakmillis, int waterupdates, int waterdistance, int waterspeed, int npcsimulationdistance)
    {
        authoritativebreakmillis = clamp(breakmillis, 100, 60000);
        authoritativescatterbreakmillis = clamp(scatterbreakmillis, 50, 60000);
        authoritativenpcsimulationmaxdist = clamp(npcsimulationdistance, 1, 1024);
        setwatersimulationsettings(waterupdates, waterdistance, waterspeed);
    }

    int getnpcsimulationmaxdist()
    {
#ifdef STANDALONE
        return authoritativenpcsimulationmaxdist;
#else
        return waitforserveredit() ? authoritativenpcsimulationmaxdist : simulationmaxdist;
#endif
    }

    void receiveactionresult(uint requestid, int result, const char *reason)
    {
#ifndef STANDALONE
        if(result != ACTION_RESULT_ACCEPTED) cancelclientbreakrequest(requestid);
        loopv(worlddrops)
        {
            worlddrop &drop = *worlddrops[i];
            if(drop.pickuprequestid != requestid) continue;
            if(result != ACTION_RESULT_ACCEPTED)
            {
                drop.pickuprequestid = 0;
                drop.picking = drop.removed = false;
                drop.picker = -1;
                drop.pickupblocked = true;
            }
            break;
        }
#endif
        loopv(predictedworldactions)
        {
            predictedworldaction *prediction = predictedworldactions[i];
            if(prediction->requestid != requestid) continue;
            if(result == ACTION_RESULT_REJECTED) rollbackworldaction(*prediction);
            delete prediction;
            predictedworldactions.remove(i);
            break;
        }
#ifndef STANDALONE
        for(int i = worlddrops.length() - 1; i >= 0; --i)
        {
            worlddrop *drop = worlddrops[i];
            if(drop->confirmed || drop->sourcerequestid != requestid) continue;
            delete worlddrops.remove(i);
        }
#endif
        if(result != ACTION_RESULT_ACCEPTED && reason && reason[0]) conoutf(CON_WARN, "server action rejected: %s", reason);
    }

#ifndef STANDALONE
    static void emitnetworkblockchips(const selinfo &sel, int orient, int num)
    {
        if(num <= 0 || orient < 0 || orient > 5) return;
        const int dimension = orient>>1;
        vec normal(0, 0, 0), hitpoint = vec(sel.o).add(sel.grid*0.5f);
        normal[dimension] = orient&1 ? 1 : -1;
        hitpoint[dimension] = sel.o[dimension] + (orient&1 ? sel.grid : 0);
        const ivec position = ivec(sel.o).add(sel.grid / 2);
        particle_blockchips(getworldcubetextureslotat(position, orient), hitpoint, normal, num);
    }
#endif

    void receivebreakstate(int actor, uint requestid, int phase, int action, const ivec &absolutetarget, int orient, int stage)
    {
#ifndef STANDALONE
        if(player1 && actor == player1->clientnum && (phase == BREAK_STATE_CANCEL || phase == BREAK_STATE_COMPLETE)) cancelclientbreakrequest(requestid);
        gameent *d = clients.inrange(actor) ? clients[actor] : NULL;
        if(d && d != player1)
        {
            const bool active = phase == BREAK_STATE_START || phase == BREAK_STATE_UPDATE;
            if(active && !d->renderattacking)
            {
                d->renderattacking = true;
                d->renderattackmillis = lastmillis;
            }
            else if(!active && d->renderattacking)
            {
                d->renderattacking = false;
                d->renderattackreleasemillis = lastmillis;
            }
        }
        if(action == WORLD_ACTION_BREAK_CUBE_START)
        {
            if(phase == BREAK_STATE_START || phase == BREAK_STATE_UPDATE)
            {
                selinfo sel;
                worldactionselection(sel, absolutetarget, orient);
                worldselectiontolocal(sel);
                setbreakstain(actor, requestid, sel.o, sel.grid, clamp(stage, 0, SURVIVAL_BREAK_STAGES - 1));
                if(!player1 || actor != player1->clientnum) emitnetworkblockchips(sel, orient, phase == BREAK_STATE_START ? 2 : 3);
            }
            else clearbreakstain(actor, requestid);
        }
#else
        (void)actor; (void)requestid; (void)phase; (void)action; (void)absolutetarget; (void)orient; (void)stage;
#endif
    }

    bool savesurvivalinventory(stream *f)
    {
        if(!f) return false;
        bool ok = f->printf("game_mode %d\n", gamemode) > 0;
        loopi(SURVIVAL_USABLE_SLOTS) if(ok && survivalitems[i] >= 0 && survivalcounts[i] > 0)
            ok = f->printf("inventory %d " PERSISTENT_ULL_FORMAT " %d %d\n", i, getinventoryitempersistentid(survivalitems[i]),
                           survivalcounts[i], survivaldurabilities[i]) > 0;
        if(ok && inventorycursoritem >= 0 && inventorycursorcount > 0)
            ok = f->printf("inventory_cursor " PERSISTENT_ULL_FORMAT " %d %d\n", getinventoryitempersistentid(inventorycursoritem),
                           inventorycursorcount, inventorycursordurability) > 0;
        return ok;
    }

    static bool writefurnacestring(stream &file, const char *value)
    {
        const int length = value ? int(strlen(value)) : 0;
        return length < MAXSTRLEN && file.putlil<ushort>(ushort(length)) &&
               (!length || file.write(value, length) == size_t(length));
    }

    static bool readfurnacestring(stream &file, char *value, int size)
    {
        const uint length = file.getlil<ushort>();
        if(length >= uint(size) || (length && file.read(value, length) != length)) return false;
        value[length] = '\0';
        return true;
    }

    static bool replacechestfile(const char *temporary, const char *finalname)
    {
#ifdef WIN32
        return MoveFileEx(temporary, finalname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        return rename(temporary, finalname) == 0;
#endif
    }

    static bool writefurnacestack(stream &file, int item, int count, int durability)
    {
        return writefurnacestring(file, count > 0 ? getinventoryitemid(item) : "") &&
               file.putlil<int>(max(count, 0)) && file.putlil<int>(max(durability, 0));
    }

    static bool readfurnacestack(stream &file, int &item, int &count, int &durability, int limit)
    {
        string id;
        if(!readfurnacestring(file, id, sizeof(id))) return false;
        count = file.getlil<int>();
        durability = file.getlil<int>();
        item = id[0] ? getinventoryitemindex(id) : -1;
        if(item < 0 || count <= 0)
        {
            item = -1;
            count = durability = 0;
        }
        else count = clamp(count, 1, min(max(getinventoryitemmaxstack(item), 1), max(limit, 1)));
        return true;
    }

    bool savelocalfurnaces(const char *world)
    {
        if(!world || !world[0]) return false;
        defformatstring(name, "media/map/%s/world.furnaces", world);
        stream *file = openrawfile(path(name), "wb");
        if(!file) return false;
        bool ok = file->write("CCFU", 4) == 4 && file->putlil<uint>(2) && file->putlil<uint>(uint(localfurnaces.length()));
        loopv(localfurnaces) if(ok)
        {
            furnaceinstance &furnace = *localfurnaces[i];
            ok = file->putlil<int>(furnace.target.x) && file->putlil<int>(furnace.target.y) && file->putlil<int>(furnace.target.z) &&
                 writefurnacestring(*file, getinventoryitemid(furnace.worlditem));
            loopj(FURNACE_INPUT_MAX) if(ok)
                ok = writefurnacestack(*file, furnace.inputitems[j], furnace.inputcounts[j], furnace.inputdurabilities[j]);
            if(ok) ok = writefurnacestack(*file, furnace.fuelitem, furnace.fuelcount, furnace.fueldurability) &&
                        writefurnacestack(*file, furnace.outputitem, furnace.outputcount, furnace.outputdurability) &&
                        writefurnacestring(*file, getfurnacerecipeid(furnace.activerecipe)) &&
                        file->putlil<int>(max(furnace.progress, 0)) && file->putlil<int>(max(furnace.heat, 0)) &&
                        file->putlil<int>(max(furnace.heatcapacity, 0)) && file->putlil<int>(furnace.baking ? 1 : 0);
        }
        delete file;
        return ok;
    }

    bool loadlocalfurnaces(const char *world)
    {
        localfurnaces.deletecontents();
        if(!world || !world[0]) return false;
        defformatstring(name, "media/map/%s/world.furnaces", world);
        stream *file = openrawfile(path(name), "rb");
        if(!file) return true;
        char magic[4] = { 0, 0, 0, 0 };
        const uint version = file->read(magic, 4) == 4 ? file->getlil<uint>() : 0,
                   count = version >= 1 && version <= 2 ? file->getlil<uint>() : 0;
        bool ok = !memcmp(magic, "CCFU", 4) && version >= 1 && version <= 2 && count <= 100000;
        loopi(ok ? int(count) : 0)
        {
            ivec target;
            target.x = file->getlil<int>(); target.y = file->getlil<int>(); target.z = file->getlil<int>();
            string worlditemid, recipeid;
            ok = readfurnacestring(*file, worlditemid, sizeof(worlditemid));
            const int worlditem = ok ? getinventoryitemindex(worlditemid) : -1;
            int inputslots = 0, inputlimit = 0;
            ok = ok && getworldfurnaceconfig(worlditem, inputslots, inputlimit);
            furnaceinstance *furnace = ok ? new furnaceinstance(target, worlditem, inputslots, inputlimit) : NULL;
            loopj(FURNACE_INPUT_MAX) if(ok)
                ok = readfurnacestack(*file, furnace->inputitems[j], furnace->inputcounts[j], furnace->inputdurabilities[j], inputlimit);
            if(ok) ok = readfurnacestack(*file, furnace->fuelitem, furnace->fuelcount, furnace->fueldurability, INT_MAX) &&
                        readfurnacestack(*file, furnace->outputitem, furnace->outputcount, furnace->outputdurability, INT_MAX) &&
                        readfurnacestring(*file, recipeid, sizeof(recipeid));
            if(ok)
            {
                furnace->activerecipe = recipeid[0] ? getfurnacerecipeindex(recipeid) : -1;
                furnace->progress = clamp(file->getlil<int>(), 0, max(getfurnacerecipeduration(furnace->activerecipe) - 1, 0));
                furnace->heat = max(file->getlil<int>(), 0);
                furnace->heatcapacity = max(file->getlil<int>(), furnace->heat);
                furnace->baking = version >= 2 && file->getlil<int>() != 0;
                if(furnace->fuelcount > 0 && getfurnacefuelmillis(furnace->fuelitem) <= 0)
                    furnace->fuelitem = -1, furnace->fuelcount = furnace->fueldurability = 0;
                bool syncchanged = false;
                updatefurnaceinstance(*furnace, 0, syncchanged);
                localfurnaces.add(furnace);
            }
            else delete furnace;
            if(!ok) break;
        }
        if(ok) ok = file->tell() == file->size();
        delete file;
        if(!ok) localfurnaces.deletecontents();
        return ok;
    }

    bool savelocalchests(const char *world)
    {
        if(!world || !world[0]) return false;
        defformatstring(name, "media/map/%s/world.chests", world);
        defformatstring(tempname, "%s.tmp", name);
        string finalpath, temppath;
        copystring(finalpath, findfile(name, "wb"));
        copystring(temppath, findfile(tempname, "wb"));
        stream *file = openrawfile(tempname, "wb");
        if(!file) return false;
        bool ok = file->write("CCCH", 4) == 4 && file->putlil<uint>(1) && file->putlil<uint>(uint(localchests.length()));
        loopv(localchests) if(ok)
        {
            chestinstance &chest = *localchests[i];
            ok = file->putlil<int>(chest.target.x) && file->putlil<int>(chest.target.y) && file->putlil<int>(chest.target.z) &&
                 writefurnacestring(*file, getinventoryitemid(chest.worlditem)) && file->putlil<int>(chest.yaw);
            loopj(CHEST_SLOTS_MAX) if(ok)
                ok = writefurnacestack(*file, chest.items[j], chest.counts[j], chest.durabilities[j]);
        }
        if(ok) ok = file->flush();
        delete file;
        if(!ok || !replacechestfile(temppath, finalpath))
        {
            remove(temppath);
            return false;
        }
        return true;
    }

    bool loadlocalchests(const char *world)
    {
        resetchests();
        if(!world || !world[0]) return false;
        defformatstring(name, "media/map/%s/world.chests", world);
        stream *file = openrawfile(path(name), "rb");
        if(!file) return true;
        char magic[4] = { 0, 0, 0, 0 };
        const uint version = file->read(magic, 4) == 4 ? file->getlil<uint>() : 0,
                   count = version == 1 ? file->getlil<uint>() : 0;
        bool ok = !memcmp(magic, "CCCH", 4) && version == 1 && count <= 100000;
        loopi(ok ? int(count) : 0)
        {
            ivec target;
            target.x = file->getlil<int>(); target.y = file->getlil<int>(); target.z = file->getlil<int>();
            string worlditemid;
            ok = readfurnacestring(*file, worlditemid, sizeof(worlditemid));
            const int worlditem = ok ? getinventoryitemindex(worlditemid) : -1;
            int slots = 0;
            ok = ok && getworldchestconfig(worlditem, slots);
            const int yaw = ok ? file->getlil<int>() : 0;
            chestinstance *chest = ok ? new chestinstance(target, worlditem, slots, yaw) : NULL;
            loopj(CHEST_SLOTS_MAX) if(ok)
                ok = readfurnacestack(*file, chest->items[j], chest->counts[j], chest->durabilities[j], INT_MAX);
            if(ok)
            {
                localchests.add(chest);
                localchestrestorepending.add(target);
                setchestvisual(target, yaw);
            }
            else delete chest;
            if(!ok) break;
        }
        if(ok) ok = file->tell() == file->size();
        delete file;
        if(!ok) resetchests();
        return ok;
    }

    struct localchunkdatareader
    {
        const uchar *position, *end;

        localchunkdatareader(const uchar *data, int length) : position(data), end(data + length) {}

        bool readuint(uint &value)
        {
            if(end - position < 4) return false;
            value = uint(position[0]) | uint(position[1]) << 8 | uint(position[2]) << 16 | uint(position[3]) << 24;
            position += 4;
            return true;
        }

        bool readint(int &value) { uint raw; if(!readuint(raw)) return false; value = int(raw); return true; }

        bool readstring(char *value, int size)
        {
            uint length;
            if(!readuint(length) || !length || length >= uint(size) || end - position < int(length)) return false;
            memcpy(value, position, length);
            value[length] = '\0';
            position += length;
            return true;
        }

        bool finished() const { return position == end; }
    };

    static void localchunkdataputuint(vector<uchar> &data, uint value) { loopi(4) data.add(uchar(value >> (8 * i))); }

    static bool localchunkdataputstring(vector<uchar> &data, const char *value)
    {
        const int length = value ? int(strlen(value)) : 0;
        if(length <= 0 || length >= MAXSTRLEN) return false;
        localchunkdataputuint(data, uint(length));
        data.put((const uchar *)value, length);
        return true;
    }

    static bool localchunkdataputstack(vector<uchar> &data, int item, int count, int durability)
    {
        if(!localchunkdataputstring(data, count > 0 ? getinventoryitemid(item) : "air")) return false;
        localchunkdataputuint(data, uint(max(count, 0)));
        localchunkdataputuint(data, uint(max(durability, 0)));
        return true;
    }

    static bool localchunkdatareadstack(localchunkdatareader &reader, int &item, int &count, int &durability, int limit)
    {
        string id;
        if(!reader.readstring(id, sizeof(id)) || !reader.readint(count) || !reader.readint(durability) || count < 0 || durability < 0) return false;
        item = !strcmp(id, "air") ? -1 : getinventoryitemindex(id);
        if(item < 0)
        {
            if(strcmp(id, "air") || count) return false;
            count = durability = 0;
        }
        else if(count <= 0 || count > min(max(getinventoryitemmaxstack(item), 1), max(limit, 1))) return false;
        return true;
    }

    static bool localchunkcontains(const ivec &target, int chunkx, int chunky)
    {
        const int chunksize = CREATIVE_GRID * 64;
        return int(floor(double(target.x) / chunksize)) == chunkx && int(floor(double(target.y) / chunksize)) == chunky;
    }

    bool capturelocalchunkdata(int chunkx, int chunky, vector<uchar> &data)
    {
        data.setsize(0);
        localchunkdataputuint(data, 1);
        int furnacecount = 0;
        loopv(localfurnaces) if(localchunkcontains(localfurnaces[i]->target, chunkx, chunky)) ++furnacecount;
        localchunkdataputuint(data, uint(furnacecount));
        loopv(localfurnaces) if(localchunkcontains(localfurnaces[i]->target, chunkx, chunky))
        {
            const furnaceinstance &furnace = *localfurnaces[i];
            localchunkdataputuint(data, uint(furnace.target.x));
            localchunkdataputuint(data, uint(furnace.target.y));
            localchunkdataputuint(data, uint(furnace.target.z));
            if(!localchunkdataputstring(data, getinventoryitemid(furnace.worlditem))) return false;
            loopj(FURNACE_INPUT_MAX)
                if(!localchunkdataputstack(data, furnace.inputitems[j], furnace.inputcounts[j], furnace.inputdurabilities[j])) return false;
            if(!localchunkdataputstack(data, furnace.fuelitem, furnace.fuelcount, furnace.fueldurability) ||
               !localchunkdataputstack(data, furnace.outputitem, furnace.outputcount, furnace.outputdurability) ||
               !localchunkdataputstring(data, furnace.activerecipe >= 0 ? getfurnacerecipeid(furnace.activerecipe) : "none")) return false;
            localchunkdataputuint(data, uint(max(furnace.progress, 0)));
            localchunkdataputuint(data, uint(max(furnace.heat, 0)));
            localchunkdataputuint(data, uint(max(furnace.heatcapacity, 0)));
            localchunkdataputuint(data, furnace.baking ? 1U : 0U);
        }
        int chestcount = 0;
        loopv(localchests) if(localchunkcontains(localchests[i]->target, chunkx, chunky)) ++chestcount;
        localchunkdataputuint(data, uint(chestcount));
        loopv(localchests) if(localchunkcontains(localchests[i]->target, chunkx, chunky))
        {
            const chestinstance &chest = *localchests[i];
            localchunkdataputuint(data, uint(chest.target.x));
            localchunkdataputuint(data, uint(chest.target.y));
            localchunkdataputuint(data, uint(chest.target.z));
            if(!localchunkdataputstring(data, getinventoryitemid(chest.worlditem))) return false;
            localchunkdataputuint(data, uint(chest.yaw));
            loopj(CHEST_SLOTS_MAX) if(!localchunkdataputstack(data, chest.items[j], chest.counts[j], chest.durabilities[j])) return false;
        }
        vector<uchar> npcdata;
#ifndef STANDALONE
        if(!capturelocalchunknpcs(chunkx, chunky, npcdata)) return false;
#endif
        localchunkdataputuint(data, uint(npcdata.length()));
        if(!npcdata.empty()) data.put(npcdata.getbuf(), npcdata.length());
        return true;
    }

    static bool decodelocalchunkdata(int chunkx, int chunky, const uchar *data, int length, vector<furnaceinstance *> &furnaces,
                                     vector<chestinstance *> &chests, vector<uchar> &npcdata)
    {
        localchunkdatareader reader(data, length);
        uint version, furnacecount, chestcount;
        if(!reader.readuint(version) || version != 1 || !reader.readuint(furnacecount) || furnacecount > 100000U) return false;
        loopi(furnacecount)
        {
            ivec target;
            string worlditemid, recipeid;
            if(!reader.readint(target.x) || !reader.readint(target.y) || !reader.readint(target.z) ||
               !reader.readstring(worlditemid, sizeof(worlditemid)) || !localchunkcontains(target, chunkx, chunky)) return false;
            const int worlditem = getinventoryitemindex(worlditemid);
            int inputslots, inputlimit;
            if(worlditem < 0 || !getworldfurnaceconfig(worlditem, inputslots, inputlimit)) return false;
            furnaceinstance *furnace = new furnaceinstance(target, worlditem, inputslots, inputlimit);
            bool ok = true;
            loopj(FURNACE_INPUT_MAX) if(ok)
                ok = localchunkdatareadstack(reader, furnace->inputitems[j], furnace->inputcounts[j], furnace->inputdurabilities[j], inputlimit);
            if(ok) ok = localchunkdatareadstack(reader, furnace->fuelitem, furnace->fuelcount, furnace->fueldurability, INT_MAX) &&
                        localchunkdatareadstack(reader, furnace->outputitem, furnace->outputcount, furnace->outputdurability, INT_MAX) &&
                        reader.readstring(recipeid, sizeof(recipeid)) && reader.readint(furnace->progress) && reader.readint(furnace->heat) &&
                        reader.readint(furnace->heatcapacity);
            int baking = 0;
            if(ok) ok = reader.readint(baking) && furnace->progress >= 0 && furnace->heat >= 0 && furnace->heatcapacity >= furnace->heat;
            if(!ok) { delete furnace; return false; }
            furnace->activerecipe = !strcmp(recipeid, "none") ? -1 : getfurnacerecipeindex(recipeid);
            if(furnace->activerecipe < 0 && strcmp(recipeid, "none")) { delete furnace; return false; }
            furnace->baking = baking != 0;
            furnaces.add(furnace);
        }
        if(!reader.readuint(chestcount) || chestcount > 100000U) return false;
        loopi(chestcount)
        {
            ivec target;
            string worlditemid;
            int yaw;
            if(!reader.readint(target.x) || !reader.readint(target.y) || !reader.readint(target.z) ||
               !reader.readstring(worlditemid, sizeof(worlditemid)) || !reader.readint(yaw) ||
               !localchunkcontains(target, chunkx, chunky)) return false;
            const int worlditem = getinventoryitemindex(worlditemid);
            int slots;
            if(worlditem < 0 || !getworldchestconfig(worlditem, slots)) return false;
            chestinstance *chest = new chestinstance(target, worlditem, slots, yaw);
            bool ok = true;
            loopj(CHEST_SLOTS_MAX) if(ok)
                ok = localchunkdatareadstack(reader, chest->items[j], chest->counts[j], chest->durabilities[j], INT_MAX);
            if(!ok) { delete chest; return false; }
            chests.add(chest);
        }
        uint npclength;
        if(!reader.readuint(npclength) || npclength > uint(reader.end - reader.position)) return false;
        if(npclength)
        {
            npcdata.put(reader.position, int(npclength));
            reader.position += npclength;
        }
        return reader.finished();
    }

    bool restorelocalchunkdata(int chunkx, int chunky, const uchar *data, int length)
    {
        vector<furnaceinstance *> furnaces;
        vector<chestinstance *> chests;
        vector<uchar> npcdata;
        if(!decodelocalchunkdata(chunkx, chunky, data, length, furnaces, chests, npcdata))
        {
            furnaces.deletecontents();
            chests.deletecontents();
            return false;
        }
#ifndef STANDALONE
        if(!restorelocalchunknpcs(chunkx, chunky, npcdata.getbuf(), npcdata.length()))
        {
            furnaces.deletecontents();
            chests.deletecontents();
            return false;
        }
#endif
        loopv(furnaces)
        {
            removelocalfurnace(furnaces[i]->target);
            localfurnaces.add(furnaces[i]);
        }
        furnaces.setsize(0);
        loopv(chests)
        {
            removelocalchest(chests[i]->target, false);
            localchests.add(chests[i]);
            localchestrestorepending.add(chests[i]->target);
            setchestvisual(chests[i]->target, chests[i]->yaw);
        }
        chests.setsize(0);
        return true;
    }

    bool debuglocalchunkdata(stream *file, int chunkx, int chunky, const uchar *data, int length)
    {
        if(!file) return false;
        vector<furnaceinstance *> furnaces;
        vector<chestinstance *> chests;
        vector<uchar> npcdata;
        if(!decodelocalchunkdata(chunkx, chunky, data, length, furnaces, chests, npcdata)) return false;
        bool ok = true;
        loopv(furnaces) if(ok)
        {
            const furnaceinstance &furnace = *furnaces[i];
            ok = file->printf("\nfurnace %d %d %d %s progress %d heat %d capacity %d baking %d\n", furnace.target.x, furnace.target.y,
                              furnace.target.z, getinventoryitemid(furnace.worlditem), furnace.progress, furnace.heat, furnace.heatcapacity,
                              furnace.baking ? 1 : 0) > 0;
            loopj(FURNACE_INPUT_MAX) if(ok && furnace.inputcounts[j] > 0)
                ok = file->printf("input %d %s %d durability %d\n", j, getinventoryitemid(furnace.inputitems[j]), furnace.inputcounts[j],
                                  furnace.inputdurabilities[j]) > 0;
            if(ok && furnace.fuelcount > 0)
                ok = file->printf("fuel %s %d durability %d\n", getinventoryitemid(furnace.fuelitem), furnace.fuelcount,
                                  furnace.fueldurability) > 0;
            if(ok && furnace.outputcount > 0)
                ok = file->printf("output %s %d durability %d\n", getinventoryitemid(furnace.outputitem), furnace.outputcount,
                                  furnace.outputdurability) > 0;
            if(ok && furnace.activerecipe >= 0)
                ok = file->printf("recipe %s\n", getfurnacerecipeid(furnace.activerecipe)) > 0;
        }
        loopv(chests) if(ok)
        {
            const chestinstance &chest = *chests[i];
            ok = file->printf("\nchest %d %d %d %s yaw %d\n", chest.target.x, chest.target.y, chest.target.z,
                              getinventoryitemid(chest.worlditem), chest.yaw) > 0;
            loopj(CHEST_SLOTS_MAX) if(ok && chest.counts[j] > 0)
                ok = file->printf("slot %d %s %d durability %d\n", j, getinventoryitemid(chest.items[j]), chest.counts[j],
                                  chest.durabilities[j]) > 0;
        }
#ifndef STANDALONE
        if(ok && !npcdata.empty()) ok = debuglocalchunknpcs(file, npcdata.getbuf(), npcdata.length());
#endif
        furnaces.deletecontents();
        chests.deletecontents();
        return ok;
    }

    static bool addsurvivalitem(int item)
    {
        if(item < 0 || item >= numinventoryitems()) return false;
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            if(survivalitems[i] != item || survivalcounts[i] >= max(getinventoryitemmaxstack(item), 1)) continue;
            ++survivalcounts[i];
            return true;
        }
        loopi(SURVIVAL_USABLE_SLOTS) if(survivalitems[i] < 0 || survivalcounts[i] <= 0)
        {
            survivalitems[i] = item;
            survivalcounts[i] = 1;
            survivaldurabilities[i] = getinventorytoolmaxdurability(item);
            return true;
        }
        return false;
    }

#ifndef STANDALONE
    enum
    {
        DROP_PICKUP_DISTANCE = 24,
        DROP_PICKUP_MILLIS = 250,
        DROP_PICKUP_DELAY = 500,
        DROP_MAX_PHYSICS_MILLIS = 100
    };

    static const float DROP_GRAVITY = 210.0f, DROP_GROUND_CLEARANCE = 3.0f;

    static bool survivalhasroom(int item, int quantity)
    {
        int room = 0;
        const int stack = max(getinventoryitemmaxstack(item), 1);
        loopi(SURVIVAL_USABLE_SLOTS)
        {
            if(survivalitems[i] == item && survivalcounts[i] > 0) room += max(stack - survivalcounts[i], 0);
            else if(survivalitems[i] < 0 || survivalcounts[i] <= 0) room += stack;
            if(room >= quantity) return true;
        }
        return false;
    }

    static bool addsurvivalitems(int item, int quantity, int durability = 0)
    {
        if(isinventorytool(item))
        {
            if(quantity != 1) return false;
            loopi(SURVIVAL_USABLE_SLOTS) if(survivalitems[i] < 0 || survivalcounts[i] <= 0)
            {
                survivalitems[i] = item;
                survivalcounts[i] = 1;
                survivaldurabilities[i] = clamp(durability > 0 ? durability : getinventorytoolmaxdurability(item),
                                                1, getinventorytoolmaxdurability(item));
                return true;
            }
            return false;
        }
        if(quantity <= 0 || !survivalhasroom(item, quantity)) return false;
        loopi(quantity) if(!addsurvivalitem(item)) return false;
        return true;
    }

    static void giveitems(const char *itemid, int quantity, const char *playername)
    {
        if(waitforserveredit())
        {
            if(playername && playername[0])
            {
                defformatstring(command, "give %s %d %s", itemid ? itemid : "", quantity, playername);
                requestworldcommand(command);
            }
            else
            {
                defformatstring(command, "give %s %d", itemid ? itemid : "", quantity);
                requestworldcommand(command);
            }
            return;
        }

        if(!itemid || !itemid[0] || quantity <= 0)
        {
            conoutf(CON_WARN, "usage: /give <item_name> <amount> [player name]");
            return;
        }
        if(playername && playername[0] && (!player1 || cubecasecmp(player1->name, playername)))
        {
            conoutf(CON_WARN, "giving items to another player is only available in multiplayer");
            return;
        }

        const int item = getinventoryitemindex(itemid);
        if(item < 0)
        {
            conoutf(CON_WARN, "give failed: unknown item '%s'", itemid);
            return;
        }
        if(!survivalhasroom(item, quantity))
        {
            conoutf(CON_WARN, "give failed: inventory does not have room for %d x %s", quantity, itemid);
            return;
        }
        loopi(quantity) addsurvivalitem(item);
        conoutf("gave %d x %s", quantity, itemid);
    }

    ICOMMAND(give, "siS", (char *itemid, int *quantity, char *playername), giveitems(itemid, *quantity, playername));

    static ivec worlddropcell(const ivec &target, int action, int orient)
    {
        ivec cell = target;
        if(action == WORLD_ACTION_BREAK_SCATTER_START && orient >= 0 && orient <= 5)
            cell[orient >> 1] += orient&1 ? 16 : -16;
        return cell;
    }

    static vec worlddroporigin(const ivec &target, int action, int orient)
    {
        const ivec cell = worlddropcell(target, action, orient);
        return vec(cell.x + 8.0f, cell.y + 8.0f, cell.z + 3.0f);
    }

    static worlddrop *findworlddrop(uint id)
    {
        loopv(worlddrops) if(worlddrops[i]->id == id && id) return worlddrops[i];
        return NULL;
    }

    static vec absoluteplayerfeet(gameent *d)
    {
        vec feet = d ? d->feetpos() : vec(0, 0, 0);
        if(waitforserveredit()) worldpositiontoabsolute(feet);
        return feet;
    }

    static bool canpickupdrop(const worlddrop &drop)
    {
        return player1 && player1->state == CS_ALIVE && (!personaldrops || drop.owner == -1 || drop.owner == player1->clientnum);
    }

    static void requestdroppickup(worlddrop &drop)
    {
        if(!drop.confirmed || drop.removed || drop.pickuprequestid || !waitforserveredit()) return;
        vec position = absoluteplayerfeet(player1);
        position.mul(DMF);
        drop.pickuprequestid = newworldrequestid();
        addmsg(N_DROPPICKUP, "ri5", int(drop.pickuprequestid), int(drop.id), int(position.x), int(position.y), int(position.z));
    }

    static void beginlocaldroppickup(worlddrop &drop)
    {
        if(drop.removed || !addsurvivalitems(drop.item, drop.count, drop.durability))
        {
            drop.pickupblocked = true;
            drop.picking = false;
            return;
        }
        drop.removed = drop.picking = true;
        drop.picker = player1 ? player1->clientnum : -1;
        drop.pickupmillis = lastmillis;
        drop.pickupfrom = drop.o;
    }

    static void updateworlddropfall(worlddrop &drop)
    {
        if(drop.settled || drop.picking) return;
        if(!drop.landingknown)
        {
            vec landing = drop.o;
            if(waitforserveredit()) worldpositiontolocal(landing);
            if(!droptofloor(landing, 1.0f, DROP_GROUND_CLEARANCE)) return;
            if(waitforserveredit()) worldpositiontoabsolute(landing);
            drop.landing = landing;
            drop.landingknown = true;
        }
        if(!drop.physicsmillis)
        {
            drop.physicsmillis = lastmillis;
            return;
        }

        const int elapsedmillis = min(max(lastmillis - drop.physicsmillis, 0), int(DROP_MAX_PHYSICS_MILLIS));
        drop.physicsmillis = lastmillis;
        if(!elapsedmillis) return;

        const float seconds = elapsedmillis / 1000.0f,
                    previousvelocity = drop.fallvelocity;
        drop.fallvelocity += DROP_GRAVITY * seconds;
        const float distance = (previousvelocity + drop.fallvelocity) * 0.5f * seconds;
        if(drop.o.z - distance <= drop.landing.z)
        {
            drop.o = drop.landing;
            drop.fallvelocity = 0;
            drop.settled = true;
            drop.settledmillis = lastmillis;
        }
        else drop.o.z -= distance;
    }

    static void updateworlddrops()
    {
        if(!player1) return;
        const vec feet = absoluteplayerfeet(player1);
        for(int i = worlddrops.length() - 1; i >= 0; --i)
        {
            worlddrop &drop = *worlddrops[i];
            if(drop.removed)
            {
                if(lastmillis - drop.pickupmillis >= DROP_PICKUP_MILLIS)
                {
                    delete worlddrops.remove(i);
                    continue;
                }
                continue;
            }
            if(!waitforserveredit() && drop.confirmed && droptimeout > 0 && lastmillis - drop.created >= droptimeout * 1000)
            {
                delete worlddrops.remove(i);
                continue;
            }
            updateworlddropfall(drop);
            if(!drop.settled) continue;
            const float distance = drop.o.dist(feet);
            if(distance > DROP_PICKUP_DISTANCE)
            {
                drop.pickupblocked = false;
                if(!drop.pickuprequestid) drop.picking = false;
                continue;
            }
            if(lastmillis - drop.created < DROP_PICKUP_DELAY) continue;
            if(!canpickupdrop(drop) || drop.pickupblocked) continue;
            if(!drop.picking)
            {
                drop.picking = true;
                drop.picker = player1->clientnum;
                drop.pickupmillis = lastmillis;
                drop.pickupfrom = drop.o;
            }
            if(waitforserveredit()) requestdroppickup(drop);
            else if(drop.confirmed) beginlocaldroppickup(drop);
        }
    }

    static void predictsurvivaldrops(int objectitem, uint requestid, const ivec &target, int action, int orient)
    {
        const int type = getworlditemtype(objectitem), index = getworlditemindex(objectitem), definitions = getworldobjectdropcount(type, index);
        const int source = player1 ? player1->clientnum : -1;
        if(!requestid) requestid = newworldrequestid();
        loopi(definitions)
        {
            int item, mincount, maxcount, quantity;
            float chance;
            if(!getworldobjectdrop(type, index, i, item, mincount, maxcount, chance) || !worlddroproll(source, requestid, objectitem, i, mincount, maxcount, chance, quantity))
                continue;
            while(worlddrops.length() >= maxdrop) delete worlddrops.remove(0);
            worlddrop *drop = new worlddrop;
            drop->id = waitforserveredit() ? 0 : 0x80000000U | nextlocaldropid++;
            drop->sourcerequestid = requestid;
            drop->source = source;
            drop->item = item;
            drop->count = quantity;
            drop->owner = source;
            drop->created = lastmillis;
            drop->physicsmillis = lastmillis;
            drop->confirmed = !waitforserveredit();
            drop->o = worlddroporigin(target, action, orient);
            worlddrops.add(drop);
        }
    }

    static void droplocalchestcontents(const chestinstance &chest)
    {
        const int source = player1 ? player1->clientnum : -1;
        loopi(chest.slots)
        {
            if(chest.items[i] < 0 || chest.counts[i] <= 0) continue;
            while(worlddrops.length() >= maxdrop) delete worlddrops.remove(0);
            worlddrop *drop = new worlddrop;
            drop->id = 0x80000000U | nextlocaldropid++;
            drop->source = source;
            drop->item = chest.items[i];
            drop->count = chest.counts[i];
            drop->durability = chest.durabilities[i];
            drop->owner = source;
            drop->created = lastmillis;
            drop->physicsmillis = lastmillis;
            drop->confirmed = true;
            drop->o = vec(chest.target).add(8);
            worlddrops.add(drop);
        }
    }

    void receivedropsettings(int personal, int timeout, int maximum, int maxdistance, int requireconfirmation)
    {
        personaldrops = personal != 0;
        droptimeout = clamp(timeout, 1, 86400);
        maxdrop = clamp(maximum, 1, 100000);
        dynamicentsmaxdistance = clamp(maxdistance, 1, 4096);
        requireconfirmeditems = requireconfirmation != 0;
        while(worlddrops.length() > maxdrop) delete worlddrops.remove(0);
    }

    void receivedropspawn(uint id, int source, uint sourcerequestid, int item, int count, int durability, int owner, const vec &o)
    {
        if(!id || item < 0 || item >= numinventoryitems() || count <= 0 || findworlddrop(id)) return;
        worlddrop *drop = NULL;
        loopv(worlddrops)
        {
            worlddrop *candidate = worlddrops[i];
            if(!candidate->confirmed && candidate->source == source && candidate->sourcerequestid == sourcerequestid &&
               candidate->item == item && candidate->count == count)
            {
                drop = candidate;
                break;
            }
        }
        const bool predicted = drop != NULL;
        if(!predicted)
        {
            drop = new worlddrop;
            worlddrops.add(drop);
        }
        drop->id = id;
        drop->sourcerequestid = sourcerequestid;
        drop->source = source;
        drop->item = item;
        drop->count = count;
        drop->durability = isinventorytool(item)
                         ? clamp(durability > 0 ? durability : getinventorytoolmaxdurability(item), 1, getinventorytoolmaxdurability(item)) : 0;
        drop->owner = owner;
        drop->created = lastmillis;
        drop->confirmed = true;
        if(!predicted)
        {
            drop->physicsmillis = lastmillis;
            drop->o = o;
        }
        if(drop->picking) requestdroppickup(*drop);
    }

    void receivedropdelete(uint id, int picker)
    {
        worlddrop *drop = findworlddrop(id);
        if(!drop) return;
        if(picker < 0)
        {
            loopv(worlddrops) if(worlddrops[i] == drop)
            {
                delete worlddrops.remove(i);
                return;
            }
        }
        if(!drop->picking)
        {
            drop->pickupmillis = lastmillis;
            drop->pickupfrom = drop->o;
        }
        drop->picker = picker;
        drop->picking = drop->removed = true;
    }

    void resetworlddrops()
    {
        worlddrops.deletecontents();
        nextlocaldropid = 1;
        personaldrops = 0;
        droptimeout = 300;
        maxdrop = 1024;
        dynamicentsmaxdistance = 64;
        requireconfirmeditems = 1;
    }

    const vector<worlddrop *> &getworlddrops()
    {
        return worlddrops;
    }

    static fallingblock *findfallingblock(uint id)
    {
        loopv(fallingblocks) if(fallingblocks[i]->id == id && id) return fallingblocks[i];
        return NULL;
    }

    static bool localfallingblockbelow(const ivec &cell)
    {
        loopv(fallingblocks)
        {
            const fallingblock &block = *fallingblocks[i];
            if(!block.replicated && block.origin.x == cell.x && block.origin.y == cell.y && block.o.z < cell.z + CREATIVE_GRID / 2)
                return true;
        }
        return false;
    }

    static void queuefallblockcheck(const ivec &cell)
    {
        loopv(fallblockchecks) if(fallblockchecks[i] == cell) return;
        fallblockchecks.add(cell);
    }

    static bool localfallblockcell(const ivec &absolute, int &item, bool &solid)
    {
        selinfo selection;
        worldactionselection(selection, absolute, WORLD_ORIENT_TOP);
        worldselectiontolocal(selection);
        if(!selection.validate() || !worldselectionready(selection)) return false;
        const ivec center = ivec(selection.o).add(CREATIVE_GRID / 2);
        solid = isworldcubesolidat(center);
        if(!solid)
        {
            item = -1;
            return true;
        }
        item = getworldcubeitem(getworldcubeindexat(center, WORLD_ORIENT_TOP));
        return true;
    }

    static const int localsupportdirections[6][3] =
    {
        { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 },
        { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }
    };

    static int localmaxsupportdistance()
    {
        int distance = 0;
        loopi(numworldcubes()) distance = max(distance, getworldcubesupportdistance(i));
        return distance;
    }

    static bool localblockworldindex(const ivec &cell, int &worldindex)
    {
        int item = -1;
        bool solid = false;
        if(!localfallblockcell(cell, item, solid)) return false;
        worldindex = solid && getworlditemtype(item) == WORLD_ITEM_CUBE ? getworlditemindex(item) : -1;
        return true;
    }

    static bool localsupportinrange(const ivec &cell)
    {
        if(!player1) return false;
        vec playerposition = player1->o;
        worldpositiontoabsolute(playerposition);
        const float radius = simulationmaxdist * GAMEUNITSPERMETER;
        return vec(cell).add(CREATIVE_GRID / 2).squaredist(playerposition) <= radius * radius;
    }

    static void queuesupportcheck(const ivec &cell, int remaining)
    {
        if(remaining < 0) return;
        loopv(localsupportchecks) if(localsupportchecks[i].cell == cell)
        {
            localsupportchecks[i].remaining = max(localsupportchecks[i].remaining, remaining);
            return;
        }
        localsupportchecks.add(localsupportcheck(cell, remaining));
    }

    static void queuesupportchange(const ivec &cell)
    {
        const int distance = localmaxsupportdistance();
        if(distance <= 0) return;
        queuesupportcheck(cell, distance);
        loopi(6)
            queuesupportcheck(ivec(cell).add(ivec(localsupportdirections[i][0], localsupportdirections[i][1],
                                                  localsupportdirections[i][2]).mul(CREATIVE_GRID)), distance);
    }

    static void setlocalsupportpersistent(const ivec &cell, bool persistent)
    {
        const ivec key = cell;
        if(persistent) localsupportpersistent.access(key, 1);
        else localsupportpersistent.remove(key);
    }

    struct localsupportsearchnode
    {
        ivec cell;
        int reach, distance, worldindex;

        localsupportsearchnode(const ivec &cell, int reach, int worldindex) : cell(cell), reach(reach), distance(0), worldindex(worldindex) {}
    };

    static bool localsupportdistance(const ivec &cell, int maxdistance, int &result)
    {
        vector<localsupportsearchnode> nodes;
        hashtable<ivec, int> indexes(1 << 8);
        int targetindex = -1;
        result = 0;
        if(!localblockworldindex(cell, targetindex)) return false;
        if(targetindex < 0 || getworldcubesupportdistance(targetindex) <= 0) return true;
        nodes.add(localsupportsearchnode(cell, 0, targetindex));
        indexes.access(cell, 0);
        for(int cursor = 0; cursor < nodes.length(); ++cursor)
        {
            const localsupportsearchnode node = nodes[cursor];
            if(node.reach >= maxdistance) continue;
            loopi(6)
            {
                const ivec neighbor = ivec(node.cell).add(ivec(localsupportdirections[i][0], localsupportdirections[i][1],
                                                                localsupportdirections[i][2]).mul(CREATIVE_GRID));
                int worldindex = -1;
                if(!localblockworldindex(neighbor, worldindex)) return false;
                if(worldindex < 0 || getworldcubesupportdistance(worldindex) <= 0 || indexes.access(neighbor)) continue;
                indexes.access(neighbor, nodes.length());
                nodes.add(localsupportsearchnode(neighbor, node.reach + 1, worldindex));
            }
        }

        vector<int> frontier;
        loopv(nodes)
        {
            localsupportsearchnode &node = nodes[i];
            loopj(6)
            {
                const ivec neighbor = ivec(node.cell).add(ivec(localsupportdirections[j][0], localsupportdirections[j][1],
                                                                localsupportdirections[j][2]).mul(CREATIVE_GRID));
                int worldindex = -1;
                if(!localblockworldindex(neighbor, worldindex)) return false;
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
                const ivec neighbor = ivec(origin).add(ivec(localsupportdirections[i][0], localsupportdirections[i][1],
                                                             localsupportdirections[i][2]).mul(CREATIVE_GRID));
                int *index = indexes.access(neighbor);
                if(!index || distance > getworldcubesupportdistance(nodes[*index].worldindex) ||
                   (nodes[*index].distance && nodes[*index].distance <= distance))
                    continue;
                nodes[*index].distance = distance;
                frontier.add(*index);
            }
        }
        result = nodes[0].distance <= maxdistance ? nodes[0].distance : 0;
        return true;
    }

    static void removelocalsupportcell(const ivec &cell)
    {
        const ivec key = cell;
        localsupportcell *state = localsupportcells.access(key);
        if(!state) return;
        const int unsupportedindex = state->unsupportedindex, unsupportedlast = localunsupportedpositions.length() - 1;
        if(unsupportedindex >= 0 && unsupportedindex <= unsupportedlast)
        {
            if(unsupportedindex != unsupportedlast)
            {
                localunsupportedpositions[unsupportedindex] = localunsupportedpositions[unsupportedlast];
                localsupportcell *moved = localsupportcells.access(localunsupportedpositions[unsupportedindex]);
                if(moved) moved->unsupportedindex = unsupportedindex;
            }
            localunsupportedpositions.setsize(unsupportedlast);
        }
        const int index = state->index, last = localsupportpositions.length() - 1;
        if(index >= 0 && index <= last)
        {
            if(index != last)
            {
                localsupportpositions[index] = localsupportpositions[last];
                localsupportcell *moved = localsupportcells.access(localsupportpositions[index]);
                if(moved) moved->index = index;
            }
            localsupportpositions.setsize(last);
        }
        localsupportcells.remove(key);
    }

    static void updatelocalunsupportedstate(const ivec &cell, localsupportcell &state)
    {
        const bool unsupported = state.distance <= 0 && !localsupportpersistent.access(cell);
        if(unsupported == (state.unsupportedindex >= 0)) return;
        if(unsupported)
        {
            state.unsupportedindex = localunsupportedpositions.length();
            localunsupportedpositions.add(cell);
            return;
        }
        const int index = state.unsupportedindex, last = localunsupportedpositions.length() - 1;
        if(index >= 0 && index <= last)
        {
            if(index != last)
            {
                localunsupportedpositions[index] = localunsupportedpositions[last];
                localsupportcell *moved = localsupportcells.access(localunsupportedpositions[index]);
                if(moved) moved->unsupportedindex = index;
            }
            localunsupportedpositions.setsize(last);
        }
        state.unsupportedindex = -1;
    }

    static bool updatesupportcell(const localsupportcheck &check)
    {
        int worldindex = -1;
        if(!localblockworldindex(check.cell, worldindex)) return false;
        const int maxdistance = getworldcubesupportdistance(worldindex);
        localsupportcell *state = localsupportcells.access(check.cell);
        if(maxdistance <= 0)
        {
            if(state) removelocalsupportcell(check.cell);
            return true;
        }
        int distance = 0;
        if(!localsupportdistance(check.cell, maxdistance, distance)) return false;
        const int previous = state ? state->distance : -1;
        if(!state)
        {
            const int index = localsupportpositions.length();
            localsupportpositions.add(check.cell);
            state = &localsupportcells.access(check.cell, localsupportcell(distance, index));
        }
        else state->distance = distance;
        updatelocalunsupportedstate(check.cell, *state);
        if(previous == distance || check.remaining <= 0) return true;
        loopi(6)
            queuesupportcheck(ivec(check.cell).add(ivec(localsupportdirections[i][0], localsupportdirections[i][1],
                                                         localsupportdirections[i][2]).mul(CREATIVE_GRID)), check.remaining - 1);
        return true;
    }

    static void randomticklocalsupportblock(const ivec &cell)
    {
        localsupportcell *state = localsupportcells.access(cell);
        if(!state || state->distance > 0 || !localsupportinrange(cell) || localsupportpersistent.access(cell)) return;
        int worldindex = -1;
        if(!localblockworldindex(cell, worldindex) || worldindex < 0 || !getworldcubesupportdecay(worldindex)) return;
        const int item = getworldcubeitem(worldindex);
        if(item < 0 || !applyworldaction(WORLD_ACTION_BREAK_CUBE_START, cell, WORLD_ORIENT_TOP, item)) return;
        selinfo dropselection;
        worldactionselection(dropselection, cell, WORLD_ORIENT_TOP);
        worldselectiontolocal(dropselection);
        predictsurvivaldrops(item, newworldrequestid(), dropselection.o, WORLD_ACTION_BREAK_CUBE_START, WORLD_ORIENT_TOP);
        removelocalsupportcell(cell);
    }

    static bool localsupportsectioninrange(const ivec &origin, const vec &playerposition, float radiussquared)
    {
        const float dx = playerposition.x < origin.x ? origin.x - playerposition.x
                       : playerposition.x > origin.x + LOCAL_SUPPORT_SECTION_SIZE
                       ? playerposition.x - origin.x - LOCAL_SUPPORT_SECTION_SIZE : 0,
                    dy = playerposition.y < origin.y ? origin.y - playerposition.y
                       : playerposition.y > origin.y + LOCAL_SUPPORT_SECTION_SIZE
                       ? playerposition.y - origin.y - LOCAL_SUPPORT_SECTION_SIZE : 0,
                    dz = playerposition.z < origin.z ? origin.z - playerposition.z
                       : playerposition.z > origin.z + LOCAL_SUPPORT_SECTION_SIZE
                       ? playerposition.z - origin.z - LOCAL_SUPPORT_SECTION_SIZE : 0;
        return dx * dx + dy * dy + dz * dz <= radiussquared;
    }

    static bool localsupportsectionqueued(const ivec &origin)
    {
        return localsupportqueuedsections.access(origin) != NULL;
    }

    static void discoverlocalsupportblocks()
    {
        vec playerposition = player1->o;
        worldpositiontoabsolute(playerposition);
        const int distance = simulationmaxdist * GAMEUNITSPERMETER,
                  sectionx = int(floor(playerposition.x / LOCAL_SUPPORT_SECTION_SIZE)),
                  sectiony = int(floor(playerposition.y / LOCAL_SUPPORT_SECTION_SIZE)),
                  sectionz = int(floor(playerposition.z / LOCAL_SUPPORT_SECTION_SIZE));
        const ivec playersection(sectionx, sectiony, sectionz);
        const float radiussquared = float(distance) * distance;
        if(playersection != localsupportlastsection || distance != localsupportlastdistance)
        {
            for(int i = localsupportsectionchecks.length() - 1; i >= 0; --i)
                if(!localsupportsectioninrange(localsupportsectionchecks[i], playerposition, radiussquared))
                {
                    localsupportqueuedsections.remove(localsupportsectionchecks[i]);
                    localsupportsectionchecks.removeunordered(i);
                }
            for(int i = localsupportscannedpositions.length() - 1; i >= 0; --i)
            {
                const ivec origin = localsupportscannedpositions[i];
                if(localsupportsectioninrange(origin, playerposition, radiussquared)) continue;
                localsupportscannedsections.remove(origin);
                localsupportscannedpositions.removeunordered(i);
            }
            const int radiussections = (distance + LOCAL_SUPPORT_SECTION_SIZE - 1) / LOCAL_SUPPORT_SECTION_SIZE,
                      minimumz = max(sectionz - radiussections, 0),
                      maximumz = min(sectionz + radiussections, int(LOCAL_SUPPORT_SECTION_LAYERS) - 1);
            const ivec currentorigin(sectionx * LOCAL_SUPPORT_SECTION_SIZE, sectiony * LOCAL_SUPPORT_SECTION_SIZE,
                                     sectionz * LOCAL_SUPPORT_SECTION_SIZE);
            if(!localsupportscannedsections.access(currentorigin) && !localsupportsectionqueued(currentorigin))
            {
                localsupportsectionchecks.add(currentorigin);
                if(localsupportsectionchecks.length() > 1)
                    swap(localsupportsectionchecks[0], localsupportsectionchecks.last());
                localsupportqueuedsections.access(currentorigin, 1);
            }
            for(int z = minimumz; z <= maximumz; ++z)
                for(int y = sectiony - radiussections; y <= sectiony + radiussections; ++y)
                    for(int x = sectionx - radiussections; x <= sectionx + radiussections; ++x)
                    {
                        const ivec origin(x * LOCAL_SUPPORT_SECTION_SIZE, y * LOCAL_SUPPORT_SECTION_SIZE,
                                          z * LOCAL_SUPPORT_SECTION_SIZE);
                        if(!localsupportsectioninrange(origin, playerposition, radiussquared) ||
                           localsupportscannedsections.access(origin) || localsupportsectionqueued(origin))
                            continue;
                        localsupportsectionchecks.add(origin);
                        localsupportqueuedsections.access(origin, 1);
                    }
            localsupportlastsection = playersection;
            localsupportlastdistance = distance;
        }

        const int scans = min(localsupportsectionchecks.length(), 8);
        loopi(scans)
        {
            const ivec origin = localsupportsectionchecks.remove(0);
            localsupportqueuedsections.remove(origin);
            if(!localsupportsectioninrange(origin, playerposition, radiussquared)) continue;
            vector<ivec> cells;
            if(!collectworldsupportcells(origin, LOCAL_SUPPORT_SECTION_SIZE, cells))
            {
                localsupportsectionchecks.add(origin);
                localsupportqueuedsections.access(origin, 1);
                continue;
            }
            localsupportscannedsections.access(origin, 1);
            localsupportscannedpositions.add(origin);
            const int distance = localmaxsupportdistance();
            loopv(cells) queuesupportcheck(cells[i], distance);
        }
    }

    static void updatesupportblocks()
    {
        if(waitforserveredit() || !islocalworld() || !player1) return;
        discoverlocalsupportblocks();
        const int checks = min(localsupportchecks.length(), 64);
        loopi(checks)
        {
            const localsupportcheck check = localsupportchecks.remove(0);
            if(!localsupportinrange(check.cell) || !updatesupportcell(check))
            {
                queuesupportcheck(check.cell, check.remaining);
                continue;
            }
        }
        if(totalmillis - localsupportlasttick < supportdecaymillis || localunsupportedpositions.empty()) return;
        localsupportlasttick = totalmillis;
        loopi(3)
        {
            if(localunsupportedpositions.empty()) break;
            randomticklocalsupportblock(localunsupportedpositions[rnd(localunsupportedpositions.length())]);
        }
    }

    static void resetlocalsupportblocks()
    {
        localsupportcells.clear();
        localsupportpersistent.clear();
        localsupportpositions.setsize(0);
        localunsupportedpositions.setsize(0);
        localsupportscannedsections.clear();
        localsupportqueuedsections.clear();
        localsupportscannedpositions.setsize(0);
        localsupportsectionchecks.setsize(0);
        localsupportchecks.setsize(0);
        localsupportlasttick = totalmillis;
        localsupportlastsection = ivec(INT_MIN, INT_MIN, INT_MIN);
        localsupportlastdistance = -1;
    }

    static bool findlocalfallblocklanding(fallingblock &block)
    {
        for(int z = block.origin.z - CREATIVE_GRID; z >= 0; z -= CREATIVE_GRID)
        {
            int item = -1;
            bool solid = false;
            if(!localfallblockcell(ivec(block.origin.x, block.origin.y, z), item, solid)) return false;
            if(!solid) continue;
            block.landing = vec(block.origin.x + CREATIVE_GRID / 2, block.origin.y + CREATIVE_GRID / 2,
                                z + CREATIVE_GRID + CREATIVE_GRID / 2);
            block.landingknown = true;
            return true;
        }
        block.landing = vec(block.origin.x + CREATIVE_GRID / 2, block.origin.y + CREATIVE_GRID / 2, CREATIVE_GRID / 2);
        block.landingknown = true;
        return true;
    }

    static bool startlocalfallingblock(const ivec &cell, int item)
    {
        if(!applyworldaction(WORLD_ACTION_BREAK_CUBE_START, cell, WORLD_ORIENT_TOP, item)) return false;
        fallingblock *block = new fallingblock;
        if(!nextlocalfallblockid || nextlocalfallblockid > uint(INT_MAX)) nextlocalfallblockid = 1;
        block->id = 0x80000000U | nextlocalfallblockid++;
        block->item = item;
        block->origin = cell;
        block->o = vec(cell).add(8.0f);
        block->physicsmillis = lastmillis;
        fallingblocks.add(block);
        findlocalfallblocklanding(*block);
        return true;
    }

    static void updatelocalfallingblock(fallingblock &block)
    {
        if(!findlocalfallblocklanding(block)) return;
        const int elapsedmillis = min(max(lastmillis - block.physicsmillis, 0), 100);
        block.physicsmillis = lastmillis;
        if(!elapsedmillis) return;
        const float seconds = elapsedmillis / 1000.0f, previousvelocity = block.velocity;
        block.velocity += DROP_GRAVITY * seconds;
        const float distance = (previousvelocity + block.velocity) * 0.5f * seconds;
        if(block.o.z - distance > block.landing.z)
        {
            block.o.z -= distance;
            return;
        }

        const ivec destination(block.origin.x, block.origin.y, int(block.landing.z) - CREATIVE_GRID / 2);
        int occupieditem = -1;
        bool occupied = false;
        if(!localfallblockcell(destination, occupieditem, occupied)) return;
        if(occupied)
        {
            findlocalfallblocklanding(block);
            return;
        }
        const ivec support = ivec(destination).sub(ivec(0, 0, CREATIVE_GRID));
        if(!applyworldaction(WORLD_ACTION_PLACE_CUBE, support, WORLD_ORIENT_TOP, block.item)) return;
        block.o = block.landing;
        block.velocity = 0;
        queuefallblockcheck(ivec(block.origin).add(ivec(0, 0, CREATIVE_GRID)));
        queuefallblockcheck(ivec(destination).add(ivec(0, 0, CREATIVE_GRID)));
        block.item = -1;
    }

    static void updatefallingblocks()
    {
        for(int i = fallingblocks.length() - 1; i >= 0; --i)
        {
            fallingblock &block = *fallingblocks[i];
            if(block.replicated)
            {
                const int age = clamp(lastmillis - block.snapshotmillis, 0, 200);
                const float seconds = age / 1000.0f;
                vec target = block.serverposition;
                target.z -= block.servervelocity * seconds + 0.5f * DROP_GRAVITY * seconds * seconds;
                const float blend = fabsf(target.z - block.o.z) > 96.0f ? 1.0f : clamp(curtime / 75.0f, 0.0f, 0.5f);
                block.o.lerp(target, blend);
                continue;
            }
            updatelocalfallingblock(block);
            if(block.item < 0) delete fallingblocks.remove(i);
        }
        if(waitforserveredit() || !islocalworld() || !player1) return;

        vec playerposition = player1->o;
        worldpositiontoabsolute(playerposition);
        const float radius = simulationmaxdist * GAMEUNITSPERMETER, radiussquared = radius * radius;
        const int checks = min(fallblockchecks.length(), 32);
        loopi(checks)
        {
            const ivec cell = fallblockchecks.remove(0);
            const vec center = vec(cell).add(8.0f);
            if(center.squaredist(playerposition) > radiussquared)
            {
                fallblockchecks.add(cell);
                continue;
            }
            int item = -1, belowitem = -1;
            bool solid = false, belowsolid = false;
            if(!localfallblockcell(cell, item, solid) ||
               !localfallblockcell(ivec(cell).sub(ivec(0, 0, CREATIVE_GRID)), belowitem, belowsolid))
            {
                fallblockchecks.add(cell);
                continue;
            }
            const int worldindex = getworlditemtype(item) == WORLD_ITEM_CUBE ? getworlditemindex(item) : -1;
            if(!solid || item < 0 || !getworldcubefall(worldindex)) continue;
            if(!belowsolid) startlocalfallingblock(cell, item);
            else if(localfallingblockbelow(cell)) queuefallblockcheck(cell);
        }
    }

    void receivefallblockspawn(uint id, int item, const vec &position, float velocity)
    {
        if(!id || getworlditemtype(item) != WORLD_ITEM_CUBE) return;
        fallingblock *block = findfallingblock(id);
        if(!block)
        {
            block = new fallingblock;
            block->id = id;
            fallingblocks.add(block);
        }
        block->item = item;
        block->replicated = true;
        block->o = block->serverposition = position;
        block->velocity = block->servervelocity = max(velocity, 0.0f);
        block->snapshotmillis = lastmillis;
    }

    void receivefallblockupdate(uint id, int tick, const vec &position, float velocity)
    {
        fallingblock *block = findfallingblock(id);
        if(!block || !block->replicated || tick <= block->servertick) return;
        block->servertick = tick;
        block->serverposition = position;
        block->servervelocity = max(velocity, 0.0f);
        block->snapshotmillis = lastmillis;
    }

    void receivefallblockdelete(uint id)
    {
        loopv(fallingblocks) if(fallingblocks[i]->id == id)
        {
            delete fallingblocks.remove(i);
            return;
        }
    }

    void resetfallingblocks()
    {
        fallingblocks.deletecontents();
        fallblockchecks.setsize(0);
        nextlocalfallblockid = 1;
    }

    const vector<fallingblock *> &getfallingblocks()
    {
        return fallingblocks;
    }

    int getdynamicentsmaxdistance()
    {
        return dynamicentsmaxdistance;
    }

    static int localdeathsequence = 0, deaththirdperson = 0;

    static void addlocaldeathdrop(int item, int count, int durability, const vec &origin, uint spreadseed)
    {
        if(item < 0 || count <= 0) return;
        while(worlddrops.length() >= maxdrop) delete worlddrops.remove(0);
        const uint hash = worlddrophash(spreadseed), anglehash = worlddrophash(hash ^ 0x9E3779B9U);
        const float angle = float(anglehash % 36000U) * RAD / 100.0f,
                    radius = 1.5f + float(hash % 350U) / 100.0f;
        worlddrop *drop = new worlddrop;
        if(!nextlocaldropid) nextlocaldropid = 1;
        drop->id = 0x80000000U | nextlocaldropid++;
        drop->source = player1 ? player1->clientnum : -1;
        drop->item = item;
        drop->count = count;
        drop->durability = isinventorytool(item) ? clamp(durability, 1, getinventorytoolmaxdurability(item)) : 0;
        drop->owner = -1;
        drop->created = drop->physicsmillis = lastmillis;
        drop->confirmed = true;
        drop->o = vec(origin).add(vec(cosf(angle) * radius, sinf(angle) * radius, 3.0f));
        worlddrops.add(drop);
    }

    void addlocalitemdrop(int item, int count, const vec &origin, uint spreadseed)
    {
        addlocaldeathdrop(item, count, 0, origin, spreadseed);
    }

    static void droplocalplayerinventory(const vec &origin)
    {
        const uint seed = worlddrophash(uint(++localdeathsequence) ^ uint(max(lastmillis, 1)));
        loopi(SURVIVAL_USABLE_SLOTS)
            addlocaldeathdrop(survivalitems[i], survivalcounts[i], survivaldurabilities[i], origin, seed ^ uint(i + 1) * 0x85EBCA6BU);
        loopi(CRAFT_GRID_MAX)
            addlocaldeathdrop(craftingitems[i], craftingcounts[i], craftingdurabilities[i], origin, seed ^ uint(i + 65) * 0xC2B2AE35U);
        addlocaldeathdrop(inventorycursoritem, inventorycursorcount, inventorycursordurability, origin, seed ^ 0x27D4EB2FU);
        resetsurvivalinventory();
    }

    static void showdeathscreen()
    {
        deaththirdperson = thirdperson;
        thirdperson = 1;
        execute("hideui survival_inventory; hideui crafting_table; hideui furnace; hideui chest; showui death_screen");
    }

    static void hidedeathscreen()
    {
        execute("hideui death_screen");
        thirdperson = deaththirdperson;
    }

    static void setplayerdead(gameent &d, const vec &impulse)
    {
        if(d.state == CS_DEAD) return;
        d.rendereating = false;
        d.rendereatitem = -1;
        d.state = CS_DEAD;
        d.collidetype = COLLIDE_NONE;
        d.stopmoving();
        d.vel = d.falling = vec(0, 0, 0);
        d.falldistance = d.fallvelocity = 0;
        beginplayerragdoll(&d, impulse);
        if(&d == player1) showdeathscreen();
        cleardynentcache();
    }

    static void setplayeralive(gameent &d, const vec &position)
    {
        clearplayerragdoll(&d);
        d.o = position;
        d.health = PLAYER_MAX_HEALTH;
        d.state = d.editstate = CS_ALIVE;
        d.rendereating = false;
        d.rendereatitem = -1;
        d.collidetype = COLLIDE_ELLIPSE;
        d.stopmoving();
        d.vel = d.falling = vec(0, 0, 0);
        d.falldistance = d.fallvelocity = 0;
        d.resetinterp();
        if(&d == player1) hidedeathscreen();
        cleardynentcache();
    }

    void damageplayer(float damage, const vec &source)
    {
        if(!m_survival || waitforserveredit() || !player1 || player1->state != CS_ALIVE || damage <= 0) return;
        player1->health = max(player1->health - damage, 0.0f);
        if(player1->health > 0) return;
        vec impulse = vec(player1->o).sub(source);
        if(impulse.squaredlen() > 1e-4f) impulse.normalize().mul(45.0f);
        droplocalplayerinventory(player1->feetpos());
        setplayerdead(*player1, impulse);
    }

    void restorelocalplayerhealth(float health)
    {
        if(!player1) return;
        player1->health = clamp(health, 0.0f, float(PLAYER_MAX_HEALTH));
        if(player1->health <= 0) setplayerdead(*player1, vec(0, 0, 0));
    }

    void getlocalplayermotion(vec &velocity, vec &falling, float &falldistance, int &physstate)
    {
        if(!player1)
        {
            velocity = falling = vec(0, 0, 0);
            falldistance = 0;
            physstate = PHYS_FALL;
            return;
        }
        velocity = player1->vel;
        falling = player1->falling;
        falldistance = max(player1->falldistance, 0.0f);
        physstate = player1->physstate;
    }

    void restorelocalplayermotion(const vec &velocity, const vec &falling, float falldistance, int physstate)
    {
        if(!player1 || player1->state != CS_ALIVE) return;
        player1->vel = velocity;
        player1->falling = falling;
        player1->falldistance = max(falldistance, 0.0f);
        player1->fallvelocity = max(-(velocity.z + falling.z), 0.0f);
        player1->physstate = clamp(physstate, int(PHYS_FLOAT), int(PHYS_BOUNCE));
        player1->resetinterp();
    }

    void savesessionstate()
    {
        if(!connected || !player1 || player1->clientnum < 0) return;
        packetbuf p(100, ENET_PACKET_FLAG_RELIABLE);
        sendposition(player1, p);
        sendclientpacket(p.finalize(), 0);
        flushclient();
    }

    float getlocalplayerhealth()
    {
        return player1 ? player1->health : float(PLAYER_MAX_HEALTH);
    }

    void receiveplayerstate(int clientnum, float health, int state, const vec &absoluteposition, const vec &impulse)
    {
        gameent *d = player1 && clientnum == player1->clientnum ? player1 : clients.inrange(clientnum) ? clients[clientnum] : NULL;
        if(!d) return;
        d->lastupdate = lastmillis;
        if(d != player1) d->smoothmillis = 0;
        vec position(absoluteposition);
        if(waitforserveredit()) worldpositiontolocal(position);
        d->health = clamp(health, 0.0f, float(PLAYER_MAX_HEALTH));
        if(state == CS_DEAD)
        {
            d->o = position;
            d->resetinterp();
            setplayerdead(*d, impulse);
        }
        else if(d->state == CS_DEAD) setplayeralive(*d, position);
        else d->state = CS_ALIVE;
    }

    ICOMMAND(getplayerhealth, "", (), intret(player1 ? int(ceilf(player1->health)) : PLAYER_MAX_HEALTH));
    ICOMMAND(isplayerdead, "", (), intret(player1 && player1->state == CS_DEAD));
    ICOMMAND(respawnplayer, "", (),
    {
        if(!player1 || player1->state != CS_DEAD) return;
        if(waitforserveredit()) addmsg(N_RESPAWN, "r");
        else
        {
            vec worldspawn;
            float worldspawnyaw = 0;
            float worldspawnpitch = 0;
            if(getpreparedworldspawn(worldspawn, worldspawnyaw, worldspawnpitch))
            {
                worldpositiontolocal(worldspawn);
                player1->yaw = worldspawnyaw;
                player1->pitch = worldspawnpitch;
                setplayeralive(*player1, worldspawn);
            }
            else
            {
                findplayerspawn(player1, -1, 0);
                setplayeralive(*player1, player1->o);
            }
        }
    });
#endif

    static void consumesurvivalitem()
    {
        const int slot = clampcreativehotbarslot();
        if(survivalcounts[slot] <= 0) return;
        if(--survivalcounts[slot] <= 0)
        {
            survivalitems[slot] = -1;
            survivalcounts[slot] = survivaldurabilities[slot] = 0;
        }
    }

    static bool fooduseheld = false;

    static void setfoodrenderstate(gameent &d, bool active, int item = -1, int elapsed = 0)
    {
        d.rendereating = active;
        d.rendereatitem = active ? item : -1;
        d.rendereatduration = active ? getinventoryfoodtime(item) : 0;
        d.rendereatmillis = active ? lastmillis - clamp(elapsed, 0, max(d.rendereatduration, 0)) : -1000;
        d.rendereatcrumbmillis = active ? d.rendereatmillis - 1 : -1;
    }

    void receivefoodstate(int clientnum, bool active, int item, int elapsed)
    {
        gameent *d = player1 && clientnum == player1->clientnum ? player1 : clients.inrange(clientnum) ? clients[clientnum] : NULL;
        if(!d) return;
        if(active && isinventoryfood(item)) setfoodrenderstate(*d, true, item, elapsed);
        else
        {
            setfoodrenderstate(*d, false);
            if(d == player1) fooduseheld = false;
        }
    }

    static void stopfooduse(bool notifyserver = true)
    {
        if(!fooduseheld && (!player1 || !player1->rendereating)) return;
        fooduseheld = false;
        if(player1) setfoodrenderstate(*player1, false);
        if(notifyserver && waitforserveredit()) addmsg(N_FOODACTION, "ri", 0);
    }

    static bool beginfooduse()
    {
        if(!survivalenabled() || player1->health >= PLAYER_MAX_HEALTH) return false;
        const int item = selectedcreativeblock();
        if(!isinventoryfood(item)) return false;
        fooduseheld = true;
        setfoodrenderstate(*player1, true, item);
        if(waitforserveredit()) addmsg(N_FOODACTION, "ri", 1);
        return true;
    }

    static void updatefooduse()
    {
        if(!fooduseheld || !player1 || !player1->rendereating) return;
        const int item = selectedcreativeblock();
        if(!survivalenabled() || item != player1->rendereatitem || !isinventoryfood(item) || player1->health >= PLAYER_MAX_HEALTH)
        {
            stopfooduse();
            return;
        }
        if(waitforserveredit() || lastmillis - player1->rendereatmillis < getinventoryfoodtime(item)) return;

        consumesurvivalitem();
        player1->health = min(player1->health + getinventoryfoodhealth(item), float(PLAYER_MAX_HEALTH));
        const int nextitem = selectedcreativeblock();
        if(player1->health < PLAYER_MAX_HEALTH && nextitem == item) setfoodrenderstate(*player1, true, item);
        else stopfooduse(false);
    }

    static bool creativeenabled()
    {
        return m_creative && !editmode && player1 && player1->state == CS_ALIVE;
    }

    static bool survivalenabled()
    {
        return m_survival && !editmode && player1 && player1->state == CS_ALIVE;
    }

    static bool buildenabled()
    {
        return (m_creative || m_survival) && !editmode && player1 && player1->state == CS_ALIVE;
    }

    static float buildactionreach()
    {
        return m_survival ? float(SURVIVAL_BUILD_REACH) : float(CREATIVE_REACH);
    }

    float creativearmwave(int elapsed)
    {
        float progress = clamp(elapsed / float(CREATIVE_ARM_CYCLE), 0.0f, 1.0f);
        return (0.5f - 0.5f * cosf(progress * 2.0f * PI)) * CREATIVE_ARM_PITCH;
    }

    float playerarmactionpitch(const gameent *d)
    {
        if(!d || (d == player1 && !buildenabled())) return -1.0f;

        if(d->renderattacking)
        {
            int elapsed = max(lastmillis - d->renderattackmillis, 0) % CREATIVE_ARM_CYCLE;
            return creativearmwave(elapsed);
        }

        int elapsed = lastmillis - d->renderattackreleasemillis;
        if(elapsed >= 0 && elapsed < CREATIVE_ARM_RELEASE)
            return d->renderattackreleasepitch * (1.0f - elapsed / float(CREATIVE_ARM_RELEASE));

        elapsed = lastmillis - d->renderplacemillis;
        return elapsed >= 0 && elapsed < CREATIVE_ARM_CYCLE ? creativearmwave(elapsed) : -1.0f;
    }

    float playerfooduseamount(const gameent *d)
    {
        if(!d || !d->rendereating || !isinventoryfood(d->rendereatitem)) return 0.0f;
        const int elapsed = max(lastmillis - d->rendereatmillis, 0), cycle = 400;
        const float progress = (elapsed % cycle) / float(cycle);
        const float approach = min(elapsed / 200.0f, 1.0f), bite = 0.5f - 0.5f * cosf(progress * 2.0f * PI);
        return approach * (0.75f + 0.25f * bite);
    }

    static bool creativehit(selinfo &hit, vec *hitpoint = NULL)
    {
        if(!buildenabled()) return false;

        const vec origin = camera1 ? camera1->o : player1->o;
        vec hitpos;
        const float reach = buildactionreach();
        float dist = raycubepos(origin, camdir, hitpos, reach,
                                RAY_CLIPMAT | RAY_SKIPFIRST, CREATIVE_GRID);
        if(dist >= reach) return false;
        if(hitpoint) *hitpoint = hitpos;

        // Step just through the hit surface so flooring selects the occupied cell.
        vec inside = vec(camdir).mul(dist + 0.05f).add(origin);
        if(!insideworld(inside)) return false;

        hit.o = ivec(inside).mask(~(CREATIVE_GRID - 1));
        hit.s = ivec(1, 1, 1);
        hit.grid = CREATIVE_GRID;
        hit.cx = hit.cy = hit.corner = 0;
        hit.cxs = hit.cys = 2;

        float boxdist = 0;
        if(!rayboxintersect(vec(hit.o), vec(CREATIVE_GRID), origin, camdir, boxdist, hit.orient))
            return false;
        return hit.validate();
    }

    enum
    {
        CREATIVE_TARGET_NONE = 0,
        CREATIVE_TARGET_CUBE,
        CREATIVE_TARGET_SCATTER
    };

    struct creativetarget
    {
        int type, scattertype, scatterorient;
        ivec scattersupport;
        selinfo cube;
        vec center, radius, hitpoint;

        creativetarget()
            : type(CREATIVE_TARGET_NONE), scattertype(-1), scatterorient(WORLD_ORIENT_TOP), scattersupport(0, 0, 0), center(0, 0, 0),
              radius(0, 0, 0), hitpoint(0, 0, 0)
        {
        }
    };

    static bool findcreativetarget(creativetarget &target)
    {
        if(!buildenabled()) return false;

        const vec origin = camera1 ? camera1->o : player1->o;
        int orient = -1, entity = -1;
        const float entitydistance = rayent(origin, camdir, buildactionreach(), RAY_CLIPMAT | RAY_ENTS | RAY_SKIPFIRST, CREATIVE_GRID, orient,
                                            entity);
        float scatterdistance = min(entitydistance, buildactionreach());
        if(getworldscatterhit(origin, camdir, scatterdistance, target.scattertype, target.scattersupport, target.scatterorient, target.center, target.radius, scatterdistance))
        {
            target.type = CREATIVE_TARGET_SCATTER;
            return true;
        }
        if(entity >= 0 && isworldscatterentity(entity) && getworldscatterentitybox(entity, target.center, target.radius) &&
           getworldscatterentityedit(entity, target.scattertype, target.scattersupport, target.scatterorient))
        {
            target.type = CREATIVE_TARGET_SCATTER;
            return true;
        }

        if(!creativehit(target.cube, &target.hitpoint)) return false;
        target.type = CREATIVE_TARGET_CUBE;
        target.center = vec(target.cube.o).add(CREATIVE_GRID * 0.5f);
        target.radius = vec(CREATIVE_GRID * 0.5f);
        return true;
    }

    static ivec creativeplacecell(const selinfo &hit)
    {
        ivec target = hit.o;
        int d = hit.orient >> 1;
        target[d] += (hit.orient & 1) ? CREATIVE_GRID : -CREATIVE_GRID;
        return target;
    }

    static bool openlookedatchest()
    {
        const vec origin = camera1 ? camera1->o : player1->o;
        int type = -1, orient = WORLD_ORIENT_TOP;
        ivec support;
        if(getworldchesthit(origin, camdir, buildactionreach(), type, support, orient) && openworldchest(type, support, orient)) return true;

        creativetarget target;
        if(findcreativetarget(target) && target.type == CREATIVE_TARGET_SCATTER)
        {
            if(openworldchest(target.scattertype, target.scattersupport, target.scatterorient)) return true;
        }

        selinfo hit;
        return creativehit(hit) && openworldchest(hit);
    }

    static bool creativeplayeroverlap(const ivec &cell)
    {
        if(!player1) return false;
        const float bx1 = cell.x, by1 = cell.y, bz1 = cell.z,
                    bx2 = cell.x + CREATIVE_GRID, by2 = cell.y + CREATIVE_GRID,
                    bz2 = cell.z + CREATIVE_GRID,
                    px1 = player1->o.x - player1->xradius,
                    py1 = player1->o.y - player1->yradius,
                    pz1 = player1->o.z - player1->eyeheight,
                    px2 = player1->o.x + player1->xradius,
                    py2 = player1->o.y + player1->yradius,
                    pz2 = player1->o.z + player1->aboveeye;
        return bx1 < px2 && bx2 > px1 && by1 < py2 && by2 > py1 && bz1 < pz2 && bz2 > pz1;
    }

    static bool opencraftingtable(const selinfo &hit)
    {
        if(!m_survival) return false;
        const int tableitem = getinventoryitemindex("crafting_table"), cube = getworldcubeindexat(ivec(hit.o).add(CREATIVE_GRID / 2), WORLD_ORIENT_TOP);
        if(tableitem < 0 || getworldcubeitem(cube) != tableitem) return false;
        craftinggridsize = 3;
        craftingstationitem = tableitem;
        updateclientcraftpreview();
        selinfo absolute = hit;
        if(waitforserveredit())
        {
            worldselectiontoabsolute(absolute);
            requestcraftaction(CRAFT_ACTION_OPEN_TABLE, absolute.o.x, absolute.o.y, absolute.o.z, 0);
        }
#ifndef STANDALONE
        execute("hideui survival_inventory; hideui chest; showui crafting_table");
#endif
        return true;
    }

    static void creativeplace()
    {
        selinfo hit;
        if(!creativehit(hit)) return;
        if(openworldfurnace(hit)) return;
        if(opencraftingtable(hit)) return;

        const int selected = selectedcreativeblock(), type = getworlditemtype(selected), worldindex = getworlditemindex(selected);
        if(selected < 0) return;
        if(type == WORLD_ITEM_PLACEABLE || type == WORLD_ITEM_SCATTER)
        {
            if((type == WORLD_ITEM_PLACEABLE && hit.orient == WORLD_ORIENT_BOTTOM) ||
               (type == WORLD_ITEM_SCATTER && hit.orient != WORLD_ORIENT_TOP))
                return;
            int actionorient = hit.orient, chestslots = 0;
            const bool chest = type == WORLD_ITEM_PLACEABLE && getworldchestconfig(selected, chestslots);
            if(chest && hit.orient != WORLD_ORIENT_TOP) return;
            if(chest && creativeplayeroverlap(worldactionplacecell(hit.o, hit.orient))) return;
            if(chest)
            {
                const int yaw = player1 ? (int(floor((player1->yaw + 45.0f) / 90.0f)) % 4 + 4) % 4 : 0;
                actionorient += yaw * 6;
            }
            if(!editworldscatter(worldindex, hit.o, hit.orient, true)) return;
            if(chest)
            {
                selinfo absolute = hit;
                worldselectiontoabsolute(absolute);
                const ivec chesttarget = worldactionplacecell(absolute.o, hit.orient);
                const int chestyaw = worldplaceyaw(actionorient);
                setchestvisual(chesttarget, chestyaw);
                if(!waitforserveredit() && !findlocalchest(chesttarget))
                    localchests.add(new chestinstance(chesttarget, selected, chestslots, chestyaw));
            }
            if(waitforserveredit())
                predictworldaction(type == WORLD_ITEM_PLACEABLE ? WORLD_ACTION_PLACE_ITEM : WORLD_ACTION_PLACE_SCATTER,
                                   hit.o, actionorient, selected, clampcreativehotbarslot());
            if(m_survival) consumesurvivalitem();
            player1->renderplacemillis = lastmillis;
            player1->renderplacetoggle = !player1->renderplacetoggle;
            return;
        }
        if(type != WORLD_ITEM_CUBE) return;

        ivec target = creativeplacecell(hit);
        if(!insideworld(target) || !insideworld(ivec(target).add(CREATIVE_GRID - 1)) ||
           creativeplayeroverlap(target) || worldtorchincell(target))
            return;

        // Extrude exactly one 16-unit voxel, then deliberately paint every face.
        selinfo placed = hit;
        placed.o = target;
        if(!worldselectionready(placed)) return;
        if(!waitforserveredit())
        {
            mpeditface(-1, 1, hit, true);
            paintworldcube(worldindex, placed, true);
            selinfo absolute = placed;
            worldselectiontoabsolute(absolute);
            waterterrainchanged(absolute.o);
            queuefallblockcheck(absolute.o);
            setlocalsupportpersistent(absolute.o, getworldcubesupportpersistentonplace(worldindex));
            queuesupportchange(absolute.o);
        }
        else
        {
            // mpeditface advances hit.o to the placed cell, while the protocol carries the support cell.
            const ivec support = hit.o;
            mpeditface(-1, 1, hit, false);
            paintworldcube(worldindex, placed, false);
            selinfo absolute = placed;
            worldselectiontoabsolute(absolute);
            waterterrainchanged(absolute.o);
            predictworldaction(WORLD_ACTION_PLACE_CUBE, support, hit.orient, selected, clampcreativehotbarslot());
        }
        if(m_survival) consumesurvivalitem();
        player1->renderplacemillis = lastmillis;
        player1->renderplacetoggle = !player1->renderplacetoggle;
    }

    static void creativeremove()
    {
        creativetarget target;
        if(!findcreativetarget(target)) return;
        if(target.type == CREATIVE_TARGET_SCATTER)
        {
            const int type = target.scattertype, mountorient = target.scatterorient;
            const ivec support = target.scattersupport;
            if(type >= 0)
            {
                if(!waitforserveredit()) scatteredittrigger(type, support, mountorient, false);
                else
                {
                    if(!editworldscatter(type, support, mountorient, false)) return;
                    predictworldaction(WORLD_ACTION_BREAK_SCATTER_START, support, mountorient, getworldscatteritem(type), -1);
                    sendworldaction(predictedworldactions.last()->requestid, WORLD_ACTION_BREAK_COMPLETE,
                                    support, mountorient, getworldscatteritem(type), -1);
                }
            }
            return;
        }
        if(!waitforserveredit())
        {
            mpdelcube(target.cube, true);
            selinfo absolute = target.cube;
            worldselectiontoabsolute(absolute);
            waterterrainchanged(absolute.o);
            queuefallblockcheck(ivec(absolute.o).add(ivec(0, 0, CREATIVE_GRID)));
            setlocalsupportpersistent(absolute.o, false);
            queuesupportchange(absolute.o);
        }
        else
        {
            const int item = getworldcubeitem(getworldcubeindexat(ivec(target.cube.o).add(target.cube.grid / 2), target.cube.orient));
            mpdelcube(target.cube, false);
            selinfo absolute = target.cube;
            worldselectiontoabsolute(absolute);
            waterterrainchanged(absolute.o);
            predictworldaction(WORLD_ACTION_BREAK_CUBE_START, target.cube.o, target.cube.orient, item, -1);
            sendworldaction(predictedworldactions.last()->requestid, WORLD_ACTION_BREAK_COMPLETE, target.cube.o, target.cube.orient, item, -1);
        }
    }

#ifndef STANDALONE
    static bool survivalbreakactive = false;
    static bool survivalattackhitnpc = false;
    static creativetarget survivalbreaktarget;
    static int survivalbreakstart = 0, survivalbreakparticlemillis = -1, survivalbreaklaststage = -1,
               survivalbreaktoolslot = -1, survivalbreaktoolitem = -1, survivalbreaktooldurability = 0, survivalbreakduration = 1,
               survivalbreakminingtype = WORLD_ITEM_NONE, survivalbreakminingindex = -1;
    static uint survivalbreakrequestid = 0;
    static int survivalblockitem(const creativetarget &target);

    static bool usesurvivalcornertool(int button)
    {
        const int slot = clampcreativehotbarslot(), tool = survivalitems[slot];
        if(survivalcounts[slot] <= 0 || survivaldurabilities[slot] <= 0 || getinventorytoolcornerpush(tool) != button)
            return false;

        creativetarget target;
        if(!findcreativetarget(target) || target.type != CREATIVE_TARGET_CUBE) return true;
        const ivec center = ivec(target.cube.o).add(target.cube.grid / 2);
        const int worldindex = getworldcubeindexat(center, target.cube.orient);
        if(!isworldcubepushable(worldindex, tool)) return true;

        const int d = target.cube.orient >> 1;
        target.cube.corner = (target.hitpoint[R[d]] - target.cube.o[R[d]] >= target.cube.grid * 0.5f ? 1 : 0) |
                             (target.hitpoint[C[d]] - target.cube.o[C[d]] >= target.cube.grid * 0.5f ? 2 : 0);
        if(waitforserveredit())
        {
            const int packedorient = target.cube.orient + target.cube.corner * 6;
            sendworldaction(newworldrequestid(), WORLD_ACTION_PUSH_CORNER, target.cube.o, packedorient, getworldcubeitem(worldindex), slot);
            player1->renderplacemillis = lastmillis;
            player1->renderplacetoggle = !player1->renderplacetoggle;
        }
        else if(pushworldcubecorner(target.cube, true, tool))
        {
            wearselectedsurvivaltool();
            player1->renderplacemillis = lastmillis;
            player1->renderplacetoggle = !player1->renderplacetoggle;
        }
        return true;
    }

    static void cancelsurvivalbreak()
    {
        const uint requestid = survivalbreakrequestid;
        if(survivalbreakrequestid && waitforserveredit())
        {
            if(survivalbreaktarget.type == CREATIVE_TARGET_SCATTER)
            {
                sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CANCEL, survivalbreaktarget.scattersupport,
                                survivalbreaktarget.scatterorient, getworldscatteritem(survivalbreaktarget.scattertype), -1);
            }
            else
                sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CANCEL, survivalbreaktarget.cube.o,
                                survivalbreaktarget.cube.orient, survivalblockitem(survivalbreaktarget), -1);
        }
        clearbreakstain(player1 ? player1->clientnum : -1, requestid);
        survivalbreakrequestid = 0;
        survivalbreaklaststage = -1;
    }

    static void cancelclientbreakrequest(uint requestid)
    {
        if(!requestid || requestid != survivalbreakrequestid) return;
        clearbreakstain(player1 ? player1->clientnum : -1, requestid);
        survivalbreakactive = false;
        survivalbreakrequestid = 0;
        survivalbreaklaststage = -1;
        survivalbreakparticlemillis = -1;
    }

    static bool samesurvivaltarget(const creativetarget &a,
                                   const creativetarget &b)
    {
        if(a.type != b.type) return false;
        if(a.type == CREATIVE_TARGET_SCATTER)
            return a.scattertype == b.scattertype && a.scatterorient == b.scatterorient && a.scattersupport == b.scattersupport;
        return a.type == CREATIVE_TARGET_CUBE &&
               a.cube.o == b.cube.o && a.cube.grid == b.cube.grid;
    }

    static int survivalblockitem(const creativetarget &target)
    {
        const int cube = getworldcubeindexat(ivec(target.cube.o).add(target.cube.grid / 2), target.cube.orient);
        return getworldcubeitem(cube);
    }

    static bool survivaltargetmining(const creativetarget &target, int &type, int &index)
    {
        if(target.type == CREATIVE_TARGET_CUBE)
        {
            type = WORLD_ITEM_CUBE;
            index = getworldcubeindexat(ivec(target.cube.o).add(target.cube.grid / 2), target.cube.orient);
            return index >= 0;
        }
        if(target.type == CREATIVE_TARGET_SCATTER)
        {
            index = target.scattertype;
            if(index < 0) return false;
            type = isworldplaceable(index) ? WORLD_ITEM_PLACEABLE : WORLD_ITEM_SCATTER;
            return true;
        }
        return false;
    }

    static int selectedsurvivaltool()
    {
        const int slot = clampcreativehotbarslot();
        return survivalcounts[slot] > 0 && survivaldurabilities[slot] > 0 && isinventorytool(survivalitems[slot]) ? survivalitems[slot] : -1;
    }

    static void emitsurvivalblockchips(const creativetarget &target, int num)
    {
        if(target.type != CREATIVE_TARGET_CUBE) return;
        vec normal(0, 0, 0);
        normal[target.cube.orient>>1] = target.cube.orient&1 ? 1 : -1;
        const ivec position = ivec(target.cube.o).add(target.cube.grid / 2);
        particle_blockchips(getworldcubetextureslotat(position, target.cube.orient), target.hitpoint, normal, num);
    }

    static void updatesurvivalbreaking()
    {
        if(!survivalenabled() || !player1->renderattacking || survivalattackhitnpc)
        {
            if(survivalbreakactive) cancelsurvivalbreak();
            survivalbreakactive = false;
            survivalbreakparticlemillis = -1;
            clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
            return;
        }

        creativetarget target;
        if(!findcreativetarget(target))
        {
            if(survivalbreakactive) cancelsurvivalbreak();
            survivalbreakactive = false;
            survivalbreakparticlemillis = -1;
            clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
            return;
        }
        if(!survivalbreakactive || !samesurvivaltarget(target, survivalbreaktarget))
        {
            if(survivalbreakactive) cancelsurvivalbreak();
            survivalbreaktarget = target;
            survivalbreakstart = lastmillis;
            survivalbreakactive = true;
            survivalbreaklaststage = 0;
            survivalbreaktoolslot = clampcreativehotbarslot();
            survivalbreaktoolitem = selectedsurvivaltool();
            survivalbreaktooldurability = survivalbreaktoolitem >= 0 ? survivaldurabilities[survivalbreaktoolslot] : 0;
            const bool miningdefined = survivaltargetmining(target, survivalbreakminingtype, survivalbreakminingindex);
            survivalbreakduration = miningdefined
                                  ? getworldbreakmillis(survivalbreakminingtype, survivalbreakminingindex, survivalbreaktoolitem)
                                  : (target.type == CREATIVE_TARGET_SCATTER ? authoritativescatterbreakmillis : authoritativebreakmillis);
            if(miningdefined && survivalbreaktoolitem < 0 &&
               !isworldobjecthandbreakable(survivalbreakminingtype, survivalbreakminingindex))
            {
                survivalbreakactive = false;
                clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
                return;
            }
            if(waitforserveredit())
            {
                if(target.type == CREATIVE_TARGET_SCATTER)
                {
                    const int type = target.scattertype, mountorient = target.scatterorient;
                    const ivec support = target.scattersupport;
                    if(type < 0)
                    {
                        survivalbreakactive = false;
                        survivalbreakrequestid = 0;
                        return;
                    }
                    survivalbreakrequestid = newworldrequestid();
                    sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_SCATTER_START, support, mountorient,
                                    getworldscatteritem(type), -1);
                }
                else
                {
                    survivalbreakrequestid = newworldrequestid();
                    sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CUBE_START, target.cube.o, target.cube.orient,
                                    survivalblockitem(target), -1);
                }
            }
            if(target.type == CREATIVE_TARGET_CUBE)
            {
                setbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid, target.cube.o, target.cube.grid, 0);
                emitsurvivalblockchips(target, 2);
                survivalbreakparticlemillis = lastmillis;
            }
            else
            {
                clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
                survivalbreakparticlemillis = -1;
            }
            return;
        }
        const int selectedtool = selectedsurvivaltool();
        if(survivalbreaktoolslot != clampcreativehotbarslot() || survivalbreaktoolitem != selectedtool ||
           (selectedtool >= 0 && survivalbreaktooldurability != survivaldurabilities[survivalbreaktoolslot]))
        {
            cancelsurvivalbreak();
            survivalbreakactive = false;
            return;
        }
        const int breakmillis = max(survivalbreakduration, 1);
        const int elapsed = max(lastmillis - survivalbreakstart, 0);
        if(target.type == CREATIVE_TARGET_CUBE)
        {
            const int stage = clamp(elapsed, 0, breakmillis - 1) * SURVIVAL_BREAK_STAGES / breakmillis;
            setbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid, target.cube.o, target.cube.grid, stage);
            if(waitforserveredit() && survivalbreakrequestid && stage != survivalbreaklaststage)
            {
                sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_UPDATE, survivalbreaktarget.cube.o,
                                survivalbreaktarget.cube.orient, survivalblockitem(survivalbreaktarget), stage);
                survivalbreaklaststage = stage;
            }
            if(survivalbreakparticlemillis < 0 || lastmillis - survivalbreakparticlemillis >= SURVIVAL_BREAK_PARTICLE_MILLIS)
            {
                const int num = survivalbreakparticlemillis < 0 ? 1 : min((lastmillis - survivalbreakparticlemillis) / SURVIVAL_BREAK_PARTICLE_MILLIS, 3);
                emitsurvivalblockchips(target, num);
                survivalbreakparticlemillis = lastmillis;
            }
        }
        else
        {
            clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
            survivalbreakparticlemillis = -1;
        }
        if(elapsed < breakmillis) return;

        int item = -1;
        bool broken = false;
        clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
        if(survivalbreaktarget.type == CREATIVE_TARGET_SCATTER)
        {
            const int type = survivalbreaktarget.scattertype, mountorient = survivalbreaktarget.scatterorient;
            const ivec support = survivalbreaktarget.scattersupport;
            if(type >= 0)
            {
                item = getworldscatteritem(type);
                bool removed = false;
                if(!waitforserveredit()) removed = scatteredittrigger(type, support, mountorient, false);
                else
                {
                    removed = editworldscatter(type, support, mountorient, false);
                    if(removed)
                    {
                        selinfo absolute;
                        worldactionselection(absolute, support, mountorient);
                        worldselectiontoabsolute(absolute);
                        addpredictedworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_SCATTER_START, absolute.o, mountorient, item);
                        sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_COMPLETE, support, mountorient, item, -1);
                    }
                    else sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CANCEL, support, mountorient, item, -1);
                }
                if(removed)
                {
                    int chestslots = 0;
                    if(getworldchestconfig(item, chestslots))
                    {
                        selinfo chestselection;
                        worldactionselection(chestselection, support, mountorient);
                        worldselectiontoabsolute(chestselection);
                        const ivec chesttarget = worldactionplacecell(chestselection.o, mountorient);
                        if(waitforserveredit()) removechestvisual(chesttarget);
                        else removelocalchest(chesttarget);
                    }
                    selinfo dropselection;
                    worldactionselection(dropselection, support, mountorient);
                    if(waitforserveredit()) worldselectiontoabsolute(dropselection);
                    if(survivalbreakminingindex >= 0 &&
                       getworlddropeligible(survivalbreakminingtype, survivalbreakminingindex, survivalbreaktoolitem))
                        predictsurvivaldrops(item, survivalbreakrequestid, dropselection.o, WORLD_ACTION_BREAK_SCATTER_START, mountorient);
                    broken = true;
                }
            }
        }
        else
        {
            item = survivalblockitem(survivalbreaktarget);
            emitsurvivalblockchips(target, 8);
            if(!waitforserveredit())
            {
                selinfo absolute = survivalbreaktarget.cube;
                worldselectiontoabsolute(absolute);
                removelocalfurnace(absolute.o);
                mpdelcube(survivalbreaktarget.cube, true);
                waterterrainchanged(absolute.o);
                queuefallblockcheck(ivec(absolute.o).add(ivec(0, 0, CREATIVE_GRID)));
                setlocalsupportpersistent(absolute.o, false);
                queuesupportchange(absolute.o);
            }
            else
            {
                mpdelcube(survivalbreaktarget.cube, false);
                selinfo absolute = survivalbreaktarget.cube;
                worldselectiontoabsolute(absolute);
                waterterrainchanged(absolute.o);
                addpredictedworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CUBE_START, absolute.o,
                                        survivalbreaktarget.cube.orient, item);
                sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_COMPLETE, survivalbreaktarget.cube.o,
                                survivalbreaktarget.cube.orient, item, -1);
            }
            selinfo dropselection = survivalbreaktarget.cube;
            if(waitforserveredit()) worldselectiontoabsolute(dropselection);
            if(survivalbreakminingindex >= 0 && getworlddropeligible(survivalbreakminingtype, survivalbreakminingindex, survivalbreaktoolitem))
                predictsurvivaldrops(item, survivalbreakrequestid, dropselection.o, WORLD_ACTION_BREAK_CUBE_START, survivalbreaktarget.cube.orient);
            broken = true;
        }
        if(broken && !waitforserveredit() && survivalbreaktoolitem >= 0)
        {
            if(survivalbreakminingindex >= 0)
            {
                survivaldurabilities[survivalbreaktoolslot] = max(survivaldurabilities[survivalbreaktoolslot] -
                                                                  getworldbreaktoolwear(survivalbreakminingtype, survivalbreakminingindex,
                                                                                        survivalbreaktoolitem), 0);
                if(survivaldurabilities[survivalbreaktoolslot] <= 0)
                {
                    survivalitems[survivalbreaktoolslot] = -1;
                    survivalcounts[survivalbreaktoolslot] = survivaldurabilities[survivalbreaktoolslot] = 0;
                }
            }
        }
        survivalbreakactive = false;
        survivalbreakparticlemillis = -1;
        survivalbreakrequestid = 0;
        survivalbreaklaststage = -1;
    }
#endif

    void rendercreativetarget()
    {
#ifndef STANDALONE
        rendernpcdebug();
        if(survivalenabled())
        {
            creativetarget target;
            if(!findcreativetarget(target)) return;
            renderboundingbox(target.center, target.radius);
            return;
        }

        creativetarget target;
        if(!findcreativetarget(target)) return;

        renderboundingbox(target.center, target.radius);
#endif
    }

    ICOMMAND(creativeattack, "D", (int *down),
    {
        if(*down)
        {
            if(player1 && !player1->renderattacking)
            {
                player1->renderattacking = buildenabled();
                player1->renderattackmillis = lastmillis;
                player1->renderattackreleasemillis = -1000;
                const bool hitnpc = attacknpc();
#ifndef STANDALONE
                survivalattackhitnpc = survivalenabled() && hitnpc;
#endif
                if(creativeenabled() && !hitnpc) creativeremove();
#ifndef STANDALONE
                else if(survivalenabled() && !hitnpc && !usesurvivalcornertool(TOOL_CORNER_PUSH_LEFT)) updatesurvivalbreaking();
#endif
            }
        }
        else if(player1 && player1->renderattacking)
        {
            int elapsed = max(lastmillis - player1->renderattackmillis, 0) % CREATIVE_ARM_CYCLE;
            player1->renderattackreleasepitch = creativearmwave(elapsed);
            player1->renderattackreleasemillis = lastmillis;
            player1->renderattacking = false;
#ifndef STANDALONE
            survivalattackhitnpc = false;
            if(survivalbreakactive) cancelsurvivalbreak();
            survivalbreakactive = false;
            survivalbreakparticlemillis = -1;
            clearbreakstain(player1 ? player1->clientnum : -1, survivalbreakrequestid);
#endif
        }
    });
    ICOMMAND(creativeplaceblock, "D", (int *down),
    {
        if(*down)
        {
            if(!openlookedatchest() && !beginfooduse() && !(survivalenabled() && usesurvivalcornertool(TOOL_CORNER_PUSH_RIGHT))) creativeplace();
        }
        else stopfooduse();
    });
    ICOMMAND(creativeselect, "i", (int *index),
    {
        int count = numinventoryitems();
        creativehotbar[clampcreativehotbarslot()] = *index >= 0 && *index < count ? *index : -1;
    });
    ICOMMAND(creativecycle, "i", (int *dir),
    {
        creativehotbarslot = (clampcreativehotbarslot() - *dir) % CREATIVE_HOTBAR_SLOTS;
        if(creativehotbarslot < 0) creativehotbarslot += CREATIVE_HOTBAR_SLOTS;
        if(m_survival && waitforserveredit())
            addmsg(N_INVENTORYACTION, "ri4", int(newworldrequestid()), INVENTORY_ACTION_SELECT, creativehotbarslot, 0);
    });
    ICOMMAND(creativehotbarselect, "i", (int *slot),
    {
        creativehotbarslot = clamp(*slot, 0, CREATIVE_HOTBAR_SLOTS - 1);
        if(m_survival && waitforserveredit())
            addmsg(N_INVENTORYACTION, "ri4", int(newworldrequestid()), INVENTORY_ACTION_SELECT, creativehotbarslot, 0);
    });
    ICOMMAND(creativehotbarassign, "ii", (int *slot, int *item),
    {
        const int count = numinventoryitems();
        if(*slot >= 0 && *slot < CREATIVE_HOTBAR_SLOTS)
            creativehotbar[*slot] = *item >= 0 && *item < count ? *item : -1;
    });
    ICOMMAND(creativehotbarswap, "ii", (int *from, int *to),
    {
        if(*from >= 0 && *from < CREATIVE_HOTBAR_SLOTS && *to >= 0 && *to < CREATIVE_HOTBAR_SLOTS)
            swap(creativehotbar[*from], creativehotbar[*to]);
    });
    ICOMMAND(getcreativeblock, "", (), intret(selectedcreativeblock()));
    ICOMMAND(getcreativehotbarslot, "i", (int *slot),
    {
        intret(*slot >= 0 && *slot < CREATIVE_HOTBAR_SLOTS ? creativehotbar[*slot] : -1);
    });
    ICOMMAND(getcreativehotbarselected, "", (), intret(clampcreativehotbarslot()));
    ICOMMAND(creativeactive, "", (), intret(creativeenabled() ? 1 : 0));
    ICOMMAND(survivalactive, "", (), intret(survivalenabled() ? 1 : 0));
    ICOMMAND(getsurvivalinventoryitem, "i", (int *slot),
    {
        intret(*slot >= 0 && *slot < SURVIVAL_USABLE_SLOTS && survivalcounts[*slot] > 0
             ? survivalitems[*slot] : -1);
    });
    ICOMMAND(getsurvivalinventorycount, "i", (int *slot),
    {
        intret(*slot >= 0 && *slot < SURVIVAL_USABLE_SLOTS ? survivalcounts[*slot] : 0);
    });
    ICOMMAND(getsurvivalinventorydurability, "i", (int *slot),
    {
        intret(*slot >= 0 && *slot < SURVIVAL_USABLE_SLOTS ? survivaldurabilities[*slot] : 0);
    });
    ICOMMAND(getinventoryitemmaxdurability, "i", (int *item), intret(getinventorytoolmaxdurability(*item)));
    ICOMMAND(getsurvivalinventorydurabilityratio, "i", (int *slot),
    {
        if(*slot < 0 || *slot >= SURVIVAL_USABLE_SLOTS || survivalcounts[*slot] <= 0 || !isinventorytool(survivalitems[*slot]))
        {
            floatret(-1.0f);
            return;
        }
        floatret(clamp(survivaldurabilities[*slot] / float(max(getinventorytoolmaxdurability(survivalitems[*slot]), 1)), 0.0f, 1.0f));
    });
    ICOMMAND(getsurvivalinventorydurabilitycolor, "i", (int *slot),
    {
        if(*slot < 0 || *slot >= SURVIVAL_USABLE_SLOTS || survivalcounts[*slot] <= 0 || !isinventorytool(survivalitems[*slot]))
        {
            intret(0xFF54C65A);
            return;
        }
        const float ratio = clamp(survivaldurabilities[*slot] / float(max(getinventorytoolmaxdurability(survivalitems[*slot]), 1)), 0.0f, 1.0f);
        const bool low = ratio < 0.5f;
        const float blend = low ? ratio * 2.0f : (ratio - 0.5f) * 2.0f;
        const int red = int((low ? 217 + (229 - 217) * blend : 229 + (84 - 229) * blend) + 0.5f);
        const int green = int((low ? 74 + (200 - 74) * blend : 200 + (198 - 200) * blend) + 0.5f);
        const int blue = int((low ? 58 + (74 - 58) * blend : 74 + (90 - 74) * blend) + 0.5f);
        intret(int(0xFF000000u | uint(red << 16) | uint(green << 8) | uint(blue)));
    });
    ICOMMAND(getinventorycursoritem, "", (), intret(inventorycursorcount > 0 ? inventorycursoritem : -1));
    ICOMMAND(getinventorycursorcount, "", (), intret(inventorycursorcount));
    ICOMMAND(getfurnaceinputslots, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        intret(furnace ? furnace->inputslots : 0);
    });
    ICOMMAND(getfurnaceinputitem, "i", (int *slot),
    {
        furnaceinstance *furnace = currentfurnace();
        intret(furnace && *slot >= 0 && *slot < furnace->inputslots && furnace->inputcounts[*slot] > 0 ? furnace->inputitems[*slot] : -1);
    });
    ICOMMAND(getfurnaceinputcount, "i", (int *slot),
    {
        furnaceinstance *furnace = currentfurnace();
        intret(furnace && *slot >= 0 && *slot < furnace->inputslots ? furnace->inputcounts[*slot] : 0);
    });
    ICOMMAND(getfurnacefuelitem, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        intret(furnace && furnace->fuelcount > 0 ? furnace->fuelitem : -1);
    });
    ICOMMAND(getfurnacefuelcount, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        intret(furnace ? furnace->fuelcount : 0);
    });
    ICOMMAND(getfurnaceoutputitem, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        intret(furnace && furnace->outputcount > 0 ? furnace->outputitem : -1);
    });
    ICOMMAND(getfurnaceoutputcount, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        intret(furnace ? furnace->outputcount : 0);
    });
    ICOMMAND(getfurnaceprogress, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        if(!furnace || furnace->activerecipe < 0) { floatret(0); return; }
        const int duration = max(getfurnacerecipeduration(furnace->activerecipe), 1);
        int progress = clamp(furnace->progress, 0, duration);
        if(waitforserveredit() && synchronizedfurnacecooking)
            progress += min(max(lastmillis - furnacesyncmillis, 0), min(furnace->heat, duration - progress));
        floatret(clamp(progress / float(duration), 0.0f, 1.0f));
    });
    ICOMMAND(getfurnaceheat, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        if(!furnace) { floatret(0); return; }
        const int heat = max(furnace->heat - (waitforserveredit() ? max(lastmillis - furnacesyncmillis, 0) : 0), 0);
        const int tenths = heat / 100 + (heat % 100 >= 50 ? 1 : 0);
        floatret(clamp(tenths / 10.0f, 0.0f, float(INT_MAX) / 1000.0f));
    });
    ICOMMAND(getfurnaceheatratio, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        if(!furnace || furnace->heatcapacity <= 0) { floatret(0); return; }
        const int heat = max(furnace->heat - (waitforserveredit() ? max(lastmillis - furnacesyncmillis, 0) : 0), 0);
        floatret(clamp(heat / float(furnace->heatcapacity), 0.0f, 1.0f));
    });
    ICOMMAND(getfurnacebaking, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        intret(furnace && furnace->baking ? 1 : 0);
    });
    ICOMMAND(bakefurnace, "", (),
    {
        furnaceinstance *furnace = currentfurnace();
        if(!furnace || furnace->baking) return;
        if(waitforserveredit()) requestfurnaceaction(FURNACE_ACTION_BAKE);
        else startfurnaceinstance(*furnace);
    });
    ICOMMAND(furnaceinputclick, "ii", (int *slot, int *button),
    {
        furnaceinstance *furnace = currentfurnace();
        if(!furnace || *slot < 0 || *slot >= furnace->inputslots) return;
        if(waitforserveredit()) requestfurnaceaction(FURNACE_ACTION_CLICK_INPUT, *slot, *button);
        else if(limitedinventoryclick(inventorycursoritem, inventorycursorcount, inventorycursordurability,
                                      furnace->inputitems[*slot], furnace->inputcounts[*slot], furnace->inputdurabilities[*slot],
                                      *button, min(furnace->inputlimit, max(getinventoryitemmaxstack(inventorycursoritem), 1))))
        {
            bool syncchanged = false;
            updatefurnaceinstance(*furnace, 0, syncchanged);
        }
    });
    ICOMMAND(furnacefuelclick, "i", (int *button),
    {
        furnaceinstance *furnace = currentfurnace();
        if(!furnace) return;
        if(waitforserveredit()) requestfurnaceaction(FURNACE_ACTION_CLICK_FUEL, *button);
        else
        {
            if(inventorycursorcount > 0 && getfurnacefuelmillis(inventorycursoritem) <= 0) return;
            limitedinventoryclick(inventorycursoritem, inventorycursorcount, inventorycursordurability,
                                  furnace->fuelitem, furnace->fuelcount, furnace->fueldurability, *button,
                                  max(getinventoryitemmaxstack(inventorycursoritem), 1));
        }
    });
    ICOMMAND(furnaceoutputclick, "i", (int *button),
    {
        furnaceinstance *furnace = currentfurnace();
        if(!furnace) return;
        if(waitforserveredit()) requestfurnaceaction(FURNACE_ACTION_CLICK_OUTPUT, *button);
        else takefurnaceoutput(*furnace, *button);
    });
    ICOMMAND(closefurnace, "", (),
    {
        if(!furnaceopen) return;
        if(waitforserveredit()) requestfurnaceaction(FURNACE_ACTION_CLOSE);
        furnaceopen = synchronizedfurnacecooking = false;
    });
    ICOMMAND(getchestslots, "", (),
    {
        chestinstance *chest = currentchest();
        intret(chest ? chest->slots : 0);
    });
    ICOMMAND(getchestitem, "i", (int *slot),
    {
        chestinstance *chest = currentchest();
        intret(chest && *slot >= 0 && *slot < chest->slots && chest->counts[*slot] > 0 ? chest->items[*slot] : -1);
    });
    ICOMMAND(getchestcount, "i", (int *slot),
    {
        chestinstance *chest = currentchest();
        intret(chest && *slot >= 0 && *slot < chest->slots ? chest->counts[*slot] : 0);
    });
    ICOMMAND(chestslotclick, "ii", (int *slot, int *button),
    {
        chestinstance *chest = currentchest();
        if(!chest || *slot < 0 || *slot >= chest->slots ||
           (*button != INVENTORY_CLICK_LEFT && *button != INVENTORY_CLICK_RIGHT)) return;
        if(waitforserveredit()) requestchestaction(CHEST_ACTION_CLICK, *slot, *button);
        else inventoryinstanceclick(inventorycursoritem, inventorycursorcount, inventorycursordurability,
                                    chest->items[*slot], chest->counts[*slot], chest->durabilities[*slot], *button);
    });
    ICOMMAND(closechest, "", (),
    {
        if(!chestopen) return;
        if(waitforserveredit()) requestchestaction(CHEST_ACTION_CLOSE);
        else receivechestanimation(openchesttarget, false);
        chestopen = false;
    });
    ICOMMAND(survivalinventoryclick, "ii", (int *slot, int *button),
    {
        if(*slot < 0 || *slot >= SURVIVAL_USABLE_SLOTS || (*button != INVENTORY_CLICK_LEFT && *button != INVENTORY_CLICK_RIGHT)) return;
        inventoryinstanceclick(inventorycursoritem, inventorycursorcount, inventorycursordurability,
                               survivalitems[*slot], survivalcounts[*slot], survivaldurabilities[*slot], *button);
        if(waitforserveredit())
            addmsg(N_INVENTORYACTION, "ri4", int(newworldrequestid()), INVENTORY_ACTION_CLICK, *slot, *button);
    });
    ICOMMAND(survivalinventoryswap, "ii", (int *from, int *to),
    {
        if(*from >= 0 && *from < SURVIVAL_USABLE_SLOTS &&
           *to >= 0 && *to < SURVIVAL_USABLE_SLOTS)
        {
            swap(survivalitems[*from], survivalitems[*to]);
            swap(survivalcounts[*from], survivalcounts[*to]);
            swap(survivaldurabilities[*from], survivaldurabilities[*to]);
            if(waitforserveredit())
                addmsg(N_INVENTORYACTION, "ri4", int(newworldrequestid()), INVENTORY_ACTION_SWAP, *from, *to);
        }
    });
    ICOMMAND(opencraftinginventory, "", (),
    {
        craftinggridsize = 2;
        craftingstationitem = -1;
        updateclientcraftpreview();
        requestcraftaction(CRAFT_ACTION_OPEN_PLAYER);
    });
    ICOMMAND(getcraftinggridsize, "", (), intret(craftinggridsize));
    ICOMMAND(getcraftinggriditem, "i", (int *slot),
    {
        intret(*slot >= 0 && *slot < craftinggridsize * craftinggridsize && craftingcounts[*slot] > 0 ? craftingitems[*slot] : -1);
    });
    ICOMMAND(getcraftinggridcount, "i", (int *slot),
    {
        intret(*slot >= 0 && *slot < craftinggridsize * craftinggridsize ? craftingcounts[*slot] : 0);
    });
    ICOMMAND(getcraftingoutputitem, "", (), intret(craftingoutputitem));
    ICOMMAND(getcraftingoutputcount, "", (), intret(craftingoutputcount));
    ICOMMAND(craftinggridclick, "ii", (int *slot, int *button),
    {
        if(*slot < 0 || *slot >= craftinggridsize * craftinggridsize ||
           (*button != INVENTORY_CLICK_LEFT && *button != INVENTORY_CLICK_RIGHT)) return;
        inventoryinstanceclick(inventorycursoritem, inventorycursorcount, inventorycursordurability,
                               craftingitems[*slot], craftingcounts[*slot], craftingdurabilities[*slot], *button);
        updateclientcraftpreview();
        if(waitforserveredit()) requestcraftaction(CRAFT_ACTION_CLICK_GRID, *slot, *button);
    });
    ICOMMAND(craftinginventorytogrid, "ii", (int *inventoryslot, int *gridslot),
    {
        if(*inventoryslot < 0 || *inventoryslot >= SURVIVAL_USABLE_SLOTS || *gridslot < 0 || *gridslot >= craftinggridsize * craftinggridsize) return;
        if(waitforserveredit()) requestcraftaction(CRAFT_ACTION_INVENTORY_TO_GRID, *inventoryslot, *gridslot);
        else
        {
            swap(survivalitems[*inventoryslot], craftingitems[*gridslot]);
            swap(survivalcounts[*inventoryslot], craftingcounts[*gridslot]);
            swap(survivaldurabilities[*inventoryslot], craftingdurabilities[*gridslot]);
            updateclientcraftpreview();
        }
    });
    ICOMMAND(craftinggridtoinventory, "ii", (int *gridslot, int *inventoryslot),
    {
        if(*inventoryslot < 0 || *inventoryslot >= SURVIVAL_USABLE_SLOTS || *gridslot < 0 || *gridslot >= craftinggridsize * craftinggridsize) return;
        if(waitforserveredit()) requestcraftaction(CRAFT_ACTION_GRID_TO_INVENTORY, *gridslot, *inventoryslot);
        else
        {
            swap(survivalitems[*inventoryslot], craftingitems[*gridslot]);
            swap(survivalcounts[*inventoryslot], craftingcounts[*gridslot]);
            swap(survivaldurabilities[*inventoryslot], craftingdurabilities[*gridslot]);
            updateclientcraftpreview();
        }
    });
    ICOMMAND(craftinggridswap, "ii", (int *from, int *to),
    {
        if(*from < 0 || *from >= craftinggridsize * craftinggridsize || *to < 0 || *to >= craftinggridsize * craftinggridsize) return;
        if(waitforserveredit()) requestcraftaction(CRAFT_ACTION_GRID_SWAP, *from, *to);
        else
        {
            swap(craftingitems[*from], craftingitems[*to]);
            swap(craftingcounts[*from], craftingcounts[*to]);
            swap(craftingdurabilities[*from], craftingdurabilities[*to]);
            updateclientcraftpreview();
        }
    });
    ICOMMAND(craftingtakeoutput, "i", (int *inventoryslot),
    {
        if(*inventoryslot < 0 || *inventoryslot >= SURVIVAL_USABLE_SLOTS || craftingrecipe < 0) return;
        if(waitforserveredit()) requestcraftaction(CRAFT_ACTION_TAKE_OUTPUT, craftingrecipe, *inventoryslot);
        else
        {
            if(survivalcounts[*inventoryslot] > 0 && survivalitems[*inventoryslot] != craftingoutputitem) return;
            const int stack = max(getinventoryitemmaxstack(craftingoutputitem), 1);
            const int capacity = stack - survivalcounts[*inventoryslot];
            craftmatch match;
            if(!matchcraftrecipe(craftingitems, craftingcounts, craftinggridsize, craftingstationitem,
                                 -1, 0, craftingrecipe, match, capacity)) return;
            loopi(CRAFT_GRID_MAX) if(match.consume[i] > 0)
            {
                craftingcounts[i] -= match.consume[i];
                if(craftingcounts[i] <= 0)
                {
                    craftingitems[i] = -1;
                    craftingcounts[i] = craftingdurabilities[i] = 0;
                }
            }
            survivalitems[*inventoryslot] = match.outputitem;
            survivalcounts[*inventoryslot] += match.outputcount;
            survivaldurabilities[*inventoryslot] = getinventorytoolmaxdurability(match.outputitem);
            updateclientcraftpreview();
        }
    });
    ICOMMAND(craftingtakeoutputcursor, "i", (int *button),
    {
        if((*button != INVENTORY_CLICK_LEFT && *button != INVENTORY_CLICK_RIGHT) || craftingrecipe < 0) return;
        const int recipe = craftingrecipe;
        if(inventorycursorcount > 0 && inventorycursoritem != craftingoutputitem) return;
        const int stack = max(getinventoryitemmaxstack(craftingoutputitem), 1);
        const int capacity = stack - inventorycursorcount;
        craftmatch match;
        if(!matchcraftrecipe(craftingitems, craftingcounts, craftinggridsize, craftingstationitem,
                             -1, 0, recipe, match, capacity)) return;
        loopi(CRAFT_GRID_MAX) if(match.consume[i] > 0)
        {
            craftingcounts[i] -= match.consume[i];
            if(craftingcounts[i] <= 0)
            {
                craftingitems[i] = -1;
                craftingcounts[i] = craftingdurabilities[i] = 0;
            }
        }
        inventorycursoritem = match.outputitem;
        inventorycursorcount += match.outputcount;
        inventorycursordurability = getinventorytoolmaxdurability(match.outputitem);
        updateclientcraftpreview();
        if(waitforserveredit()) requestcraftaction(CRAFT_ACTION_TAKE_OUTPUT_CURSOR, recipe, *button);
    });
#ifndef STANDALONE
    static void requestdropsetting(const char *name, int value, bool hasvalue)
    {
        defformatstring(command, hasvalue ? "%s %d" : "%s", name, value);
        requestworldcommand(command);
    }

    ICOMMAND(personaldrops, "iN", (int *value, int *numargs), requestdropsetting("personaldrops", *value, *numargs > 0));
    ICOMMAND(droptimeout, "iN", (int *value, int *numargs), requestdropsetting("droptimeout", *value, *numargs > 0));
    ICOMMAND(maxdrop, "iN", (int *value, int *numargs), requestdropsetting("maxdrop", *value, *numargs > 0));
    ICOMMAND(dynamicentsmaxdistance, "iN", (int *value, int *numargs), requestdropsetting("dynamicentsmaxdistance", *value, *numargs > 0));
    ICOMMAND(requireconfirmeditems, "iN", (int *value, int *numargs), requestdropsetting("requireconfirmeditems", *value, *numargs > 0));
    ICOMMAND(confirmeditemcount, "i", (int *item),
    {
        int count = 0;
        if(*item >= 0 && *item < numinventoryitems())
            loopi(SURVIVAL_USABLE_SLOTS) if(survivalitems[i] == *item && survivalcounts[i] > 0) count += survivalcounts[i];
        intret(count);
    });
#endif
    ICOMMAND(creativeblockcount, "", (), intret(numinventoryitems()));
    ICOMMAND(creativecubecount, "", (), intret(numworldcubes()));
    ICOMMAND(creativeblockiscube, "i", (int *index), intret(getworlditemtype(*index) == WORLD_ITEM_CUBE ? 1 : 0));
    ICOMMAND(creativeblockslot, "iiN", (int *index, int *face, int *numargs),
    {
        const int worldindex = getworlditemtype(*index) == WORLD_ITEM_CUBE ? getworlditemindex(*index) : 0;
        const int orient = *numargs >= 2 && *face == WORLD_CUBE_SIDE ? WORLD_ORIENT_FRONT
                         : *numargs >= 2 && *face == WORLD_CUBE_BOTTOM ? WORLD_ORIENT_BOTTOM
                         : WORLD_ORIENT_TOP;
        intret(getworldcubefaceslot(worldindex, orient));
    });
    ICOMMAND(creativeblockname, "i", (int *index),
    {
        result(getinventoryitemname(*index));
    });
    ICOMMAND(creativeblockmodel, "i", (int *index),
    {
        const int type = getworlditemtype(*index);
        const int worldindex = getworlditemindex(*index);
        result(type == WORLD_ITEM_CUBE || type == WORLD_ITEM_NONE ? "" : getworldscattermodel(worldindex));
    });
    ICOMMAND(creativeblockicon, "i", (int *index),
    {
        result(getinventoryitemicon(*index));
    });

    void gameplayhud(int w, int h)
    {
#ifndef STANDALONE
        if(!player1 || player1->state != CS_ALIVE) return;

        const char *hotbar = m_survival ? "survival_hotbar" : m_creative ? "creative_hotbar" : NULL;
        if(hotbar && !UI::uivisible(hotbar)) UI::showui(hotbar);
#endif
    }
    bool canjump() { return player1 && player1->state == CS_ALIVE; }
    bool cancrouch() { return player1 && player1->state == CS_ALIVE; }
    bool allowmove(physent *d) { return !d || d->state == CS_ALIVE || d->state == CS_EDITING; }
    dynent *iterdynents(int i)
    {
        if(players.inrange(i)) return players[i];
#ifndef STANDALONE
        return iternpc(i - players.length());
#else
        return NULL;
#endif
    }
    int numdynents()
    {
#ifndef STANDALONE
        return players.length() + numnpcs();
#else
        return players.length();
#endif
    }

    int numanims() { return ANIM_GAMESPECIFIC; }
    void findanims(const char *pattern, vector<int> &anims) {}
    void writegamedata(vector<char> &extras) {}
    void readgamedata(vector<char> &extras) {}
    float clipconsole(float w, float h) { return 0; }
    const char *defaultcrosshair(int index) { return "media/interface/crosshair/default.png"; }
    int selectcrosshair(vec &col) { return 0; }
    void setupcamera() {}
    bool allowthirdperson(bool msg) { return true; }
    bool detachcamera() { return false; }
    bool collidecamera() { return false; }

    static bool heldtorchflame(gameent *d, vec &flame)
    {
        return heldtorchemitterposition(d, flame);
    }

    static vec heldtorchparticleorigin;
    static int heldtorchparticlemillis = -1;
    FVARP(hudparticlemovementoffset, 0.0f, 0.25f, 2.0f);
    static vec previoushudparticleorigin, hudparticlemovement;
    static int previoushudparticlemillis = -1, hudparticlemovementmillis = -1;

    static bool foodcrumbemitter(gameent *d, vec &mouth, vec &direction, bool &hud)
    {
        if(!d || !d->rendereating || !isinventoryfood(d->rendereatitem) || d->state != CS_ALIVE) return false;
        hud = d == player1 && !isthirdperson();
        if(hud)
        {
            mouth = vec(camera1->o).madd(camdir, 5.0f).madd(camup, -3.0f);
            direction = vec(camdir).madd(camup, -0.35f).normalize();
        }
        else
        {
            vecfromyawpitch(d->yaw, clamp(d->pitch, -80.0f, 80.0f), 1, 0, direction);
            mouth = vec(d->o).madd(direction, 2.25f).addz(-2.5f);
            direction.addz(-0.35f).normalize();
        }
        return true;
    }

    void adddynlights()
    {
        addworldtorchlights();
        loopv(players)
        {
            vec flame;
            if(heldtorchflame(players[i], flame))
                adddynlight(flame, 14.0f * CREATIVE_GRID, vec(1.0f, 0.58f, 0.24f), 0, 0, 0, 0, vec(0, 0, 0), players[i]);
        }
    }

    void addparticles()
    {
#ifndef STANDALONE
        weather::addparticles();
#endif
        addworldtorchparticles();
        loopv(players)
        {
            gameent *d = players[i];
            vec mouth, direction;
            bool hud;
            if(!foodcrumbemitter(d, mouth, direction, hud)) continue;
            const int elapsed = max(lastmillis - d->rendereatmillis, 0),
                      bitetime = d->rendereatmillis + (elapsed / 400) * 400 + 160;
            if(lastmillis < bitetime || d->rendereatcrumbmillis >= bitetime) continue;
            particle_itemchips(getinventoryitemtexture(d->rendereatitem), mouth, direction, 3, hud ? d : NULL);
            d->rendereatcrumbmillis = bitetime;
        }
        heldtorchparticlemillis = -1;
        bool hudtorch = false;
        loopv(players)
        {
            gameent *d = players[i];
            vec flame;
            if(!heldtorchflame(d, flame)) continue;
            if(d == player1 && !isthirdperson())
            {
                hudtorch = true;
                heldtorchparticleorigin = flame;
                heldtorchparticlemillis = lastmillis;
                regular_particle_hud_flame(PART_HUD_FLAME, flame, 0.07f, 0.7f, 0xFF8628, 1, 2.4f, 9.2f, 220.0f, -100, player1);
                regular_particle_hud_flame(PART_HUD_SMOKE, flame, 0.09f, 1.1f, 0xAA8C4E, 1, 3.0f, 4.0f, 1100.0f, -250, player1);
            }
            else
            {
                regular_particle_flame(PART_FLAME, flame, 0.35f, 0.7f, 0xFF8628, 1, 2.4f, 35.0f, 220.0f, -10);
                regular_particle_flame(PART_SMOKE, flame, 0.45f, 1.1f, 0xAA8C4E, 1, 3.0f, 16.0f, 1100.0f, -25);
            }
        }
        if(!hudtorch)
        {
            if(player1) removetrackedparticles(player1);
            previoushudparticlemillis = hudparticlemovementmillis = -1;
            hudparticlemovement = vec(0, 0, 0);
        }
    }

    static void updatehudparticlemovement(physent *owner, const vec &emitter)
    {
        if(hudparticlemovementmillis == totalmillis) return;

        hudparticlemovement = vec(0, 0, 0);
        if(previoushudparticlemillis >= 0)
        {
            const int elapsed = totalmillis - previoushudparticlemillis;
            if(elapsed > 0 && elapsed <= 250 && hudparticlemovementoffset > 0)
            {
                vec velocity(emitter);
                velocity.sub(previoushudparticleorigin).mul(1000.0f/elapsed);
                const float speed = velocity.magnitude();
                if(speed > 1e-4f)
                {
                    const float movement = clamp(speed / max(owner->maxspeed, 1.0f), 0.0f, 1.0f);
                    velocity.div(speed);
                    hudparticlemovement = vec(-velocity.dot(camright), -velocity.dot(camdir), -velocity.dot(camup)).mul(hudparticlemovementoffset * movement);
                }
            }
        }
        previoushudparticleorigin = emitter;
        previoushudparticlemillis = hudparticlemovementmillis = totalmillis;
    }

    void particletrack(physent *owner, vec &o, vec &d) {}

    void hudparticletrack(physent *owner, vec &o, vec &d, int age)
    {
        if(!owner || owner != player1 || heldtorchparticlemillis != lastmillis) return;
        vec emitter;
        if(heldtorchemitterposition(player1, emitter)) heldtorchparticleorigin = emitter;
        updatehudparticlemovement(owner, heldtorchparticleorigin);
        o.madd(hudparticlemovement, age/500.0f);
        const vec localorigin(o), localvelocity(d);
        o = vec(heldtorchparticleorigin).madd(camright, localorigin.x).madd(camdir, localorigin.y).madd(camup, localorigin.z);
        d = vec(camright).mul(localvelocity.x).madd(camdir, localvelocity.y).madd(camup, localvelocity.z);
    }

    bool foodparticletrack(physent *owner, vec &o)
    {
        if(!owner || owner != player1 || !camera1) return false;
        const vec localorigin(o), mouth = vec(camera1->o).madd(camdir, 5.0f).madd(camup, -3.0f);
        o = vec(mouth).madd(camright, localorigin.x).madd(camdir, localorigin.y).madd(camup, localorigin.z);
        return true;
    }
    void dynlighttrack(physent *owner, vec &o, vec &hud) {}
    int maxsoundradius(int n) { return 500; }
    // The procedural world is unbounded while the engine minimap assumes one
    // stable, finite octree. The runtime octree is only a moving chunk window,
    // so a finite-map texture is neither valid nor safe during world rebuilds.
    bool needminimap() { return false; }

    static void sendposition(gameent *d, packetbuf &q)
    {
        putint(q, N_POS);
        putuint(q, d->clientnum);

        vec feet = d->feetpos();
        if(waitforserveredit()) worldpositiontoabsolute(feet);
        ivec o = ivec(feet.mul(DMF));
        putint(q, o.x);
        putint(q, o.y);
        putint(q, o.z);

        // 3 bits physics state, 2 bits movement, and 2 bits strafing.
        uchar physstate = d->physstate | (d->inwater ? 1<<3 : 0) | ((d->move&3)<<4) | ((d->strafe&3)<<6);
        q.put(physstate);

        uint vel = min(int(d->vel.magnitude()*DVELF), 0xFFFF),
             fall = min(int(d->falling.magnitude()*DVELF), 0xFFFF);

        // Extended movement data in the low byte; the high bits carry the
        // selected creative item plus one, leaving zero to mean no held item.
        uint flags = 0;
        if(d->crouching) flags |= 1<<0;
        if(d->renderattacking) flags |= 1<<1;
        if(d->renderplacetoggle) flags |= 1<<2;
        const int selected = selectedcreativeblock();
        if(buildenabled() && selected >= 0) flags |= uint(selected + 1)<<8;
        if(vel > 0xFF) flags |= 1<<3;
        if(fall > 0)
        {
            flags |= 1<<4;
            if(fall > 0xFF) flags |= 1<<5;
            if(d->falling.x || d->falling.y || d->falling.z > 0) flags |= 1<<6;
        }
        if((lookupmaterial(d->feetpos())&MATF_CLIP) == MAT_GAMECLIP) flags |= 1<<7;
        putuint(q, flags);

        uint dir = (d->yaw < 0 ? 360 + int(d->yaw)%360 : int(d->yaw)%360)
                 + clamp(int(d->pitch + 90), 0, 180)*360;
        q.put(dir&0xFF);
        q.put((dir>>8)&0xFF);
        q.put(clamp(int(d->roll + 90), 0, 180));

        q.put(vel&0xFF);
        if(flags&(1<<3)) q.put((vel>>8)&0xFF);
        float velyaw, velpitch;
        vectoyawpitch(d->vel, velyaw, velpitch);
        uint veldir = (velyaw < 0 ? 360 + int(velyaw)%360 : int(velyaw)%360)
                    + clamp(int(velpitch + 90), 0, 180)*360;
        q.put(veldir&0xFF);
        q.put((veldir>>8)&0xFF);

        if(flags&(1<<4))
        {
            q.put(fall&0xFF);
            if(flags&(1<<5)) q.put((fall>>8)&0xFF);
            if(flags&(1<<6))
            {
                float fallyaw, fallpitch;
                vectoyawpitch(d->falling, fallyaw, fallpitch);
                uint falldir = (fallyaw < 0 ? 360 + int(fallyaw)%360 : int(fallyaw)%360)
                              + clamp(int(fallpitch + 90), 0, 180)*360;
                q.put(falldir&0xFF);
                q.put((falldir>>8)&0xFF);
            }
        }
    }

    void c2sinfo(bool force)
    {
        if(!connected || !player1 || player1->clientnum < 0) return;

        if(strcmp(sentname, player1->name))
        {
            addmsg(N_INITCLIENT, "s", player1->name);
            copystring(sentname, player1->name);
        }

        if(!force && totalmillis - lastpositionsend < 33) return;
        lastpositionsend = totalmillis;
        {
            // packetbuf inspects its ENet packet when it leaves scope. Release
            // builds can transmit and free an unreliable packet immediately
            // during flushclient(), so relinquish the stack wrapper first.
            packetbuf p(100);
            sendposition(player1, p);
            sendclientpacket(p.finalize(), 0);
        }
        flushclient();
    }

    ICOMMAND(name, "sN", (char *s, int *numargs),
    {
        if(*numargs > 0 && player1) filtertext(player1->name, s, false, false, MAXSTRLEN);
        else result(player1 ? player1->name : "camera");
    });
    ICOMMAND(getname, "", (), result(player1 ? player1->name : "camera"));
    ICOMMAND(getclientnum, "s", (char *name), intret(player1 ? player1->clientnum : -1));
    ICOMMAND(getclientcolorname, "i", (int *cn), result(player1 ? player1->name : "camera"));
    ICOMMAND(getclientfrags, "i", (int *cn), intret(0));
    ICOMMAND(getclientflags, "i", (int *cn), intret(0));
    ICOMMAND(getclientdeaths, "i", (int *cn), intret(0));
    ICOMMAND(getclientteam, "i", (int *cn), intret(0));
    ICOMMAND(getclientmodel, "i", (int *cn), intret(-1));
    ICOMMAND(getclientcolor, "i", (int *cn), intret(0xFFFFFF));
    ICOMMAND(ismaster, "i", (int *cn),
    {
        gameent *d = clients.inrange(*cn) ? clients[*cn] : NULL;
        if(player1 && player1->clientnum == *cn) d = player1;
        intret(d && d->privilege >= PRIV_ADMIN ? 1 : 0);
    });
    ICOMMAND(isadmin, "i", (int *cn),
    {
        gameent *d = clients.inrange(*cn) ? clients[*cn] : NULL;
        if(player1 && player1->clientnum == *cn) d = player1;
        intret(d && d->privilege >= PRIV_ADMIN ? 1 : 0);
    });
    ICOMMAND(setmaster, "ss", (char *password, char *who),
    {
        if(who[0])
        {
            conoutf(CON_ERROR, "delegating admin is not supported; each admin must authenticate");
            return;
        }
        addmsg(N_SETMASTER, "rs", password);
    });
    ICOMMAND(isai, "ii", (int *cn, int *type), intret(0));
    ICOMMAND(isspectator, "i", (int *cn), intret(0));
    ICOMMAND(isdead, "i", (int *cn), intret(0));
    ICOMMAND(getmastermode, "", (), intret(mastermode));
    ICOMMAND(getmastermodename, "i", (int *mm), result((*mm >= 0 && *mm < 3) ? mastermodes[*mm] : ""));
    ICOMMAND(getmode, "", (), intret(gamemode));
    ICOMMAND(getmodeprettyname, "i", (int *mode), result(m_valid(*mode) ? gamemodes[*mode - STARTGAMEMODE].prettyname : ""));
    ICOMMAND(mode, "iN", (int *mode, int *numargs),
    {
        if(*numargs > 0)
        {
            if(waitforserveredit())
            {
                conoutf(CON_ERROR, "the multiplayer server owns the game mode");
                intret(0);
            }
            if(m_valid(*mode))
            {
                gamemode = *mode;
                intret(1);
            }
            else intret(0);
        }
        else intret(gamemode);
    });
    ICOMMAND(map, "sN", (char *name, int *numargs),
    {
        if(*numargs > 0 && name[0]) changemap(name, gamemode);
        else if(clientmap[0]) changemap(clientmap, gamemode);
        else emptymap(0, true, NULL);
    });
    ICOMMAND(m_timed, "i", (int *mode), intret(0));
    ICOMMANDN(m_edit, _icmd_m_edit_cmd, "i", (int *mode),
              intret(m_valid(*mode) && (gamemodes[*mode - STARTGAMEMODE].flags&M_EDIT) ? 1 : 0));
    ICOMMANDN(m_creative, _icmd_m_creative_cmd, "i", (int *mode),
              intret(m_valid(*mode) && (gamemodes[*mode - STARTGAMEMODE].flags&M_CREATIVE) ? 1 : 0));
    ICOMMANDN(m_survival, _icmd_m_survival_cmd, "i", (int *mode),
              intret(m_valid(*mode) && (gamemodes[*mode - STARTGAMEMODE].flags&M_SURVIVAL) ? 1 : 0));
    ICOMMANDN(m_ctf, _icmd_m_ctf_cmd, "i", (int *mode), intret(0));
    ICOMMANDN(m_teammode, _icmd_m_teammode_cmd, "i", (int *mode), intret(0));
    ICOMMAND(getfollow, "", (), intret(-1));
    ICOMMAND(nextfollow, "i", (int *dir), {});
    VARP(specmode, 0, 0, 2);
    ICOMMAND(spectator, "is", (int *val, char *who), {});
    ICOMMAND(team, "sN", (char *s, int *numargs), { if(*numargs < 0) result(""); });
    ICOMMAND(sayteam, "C", (char *text), toserver(text));
    ICOMMAND(shoot, "D", (int *down), {});
    ICOMMAND(melee, "D", (int *down), {});
    ICOMMAND(taunt, "", (), {});
    ICOMMAND(allowthirdperson, "b", (int *msg), intret(1));
    ICOMMAND(getdebugplayerspeed, "", (),
    {
        defformatstring(speed, "%.2f", horizontalmeterspersecond(player1));
        result(speed);
    });
    ICOMMAND(getdebugplayerverticalspeed, "", (),
    {
        const float speed = player1 ? (player1->vel.z + player1->falling.z) / GAMEUNITSPERMETER : 0.0f;
        defformatstring(verticalspeed, "%.2f", speed);
        result(verticalspeed);
    });
    ICOMMAND(getplayercolor, "ii", (int *model, int *team), intret(0xFFFFFF));
    ICOMMAND(showscores, "D", (int *down), {});
    ICOMMAND(refreshscoreboard, "", (), {});
    ICOMMAND(loopscoreboard, "rie", (ident *id, int *team, uint *body),
    {
        if(!player1) return;
        identstack stack;
        loopiter(id, stack, player1->clientnum);
        execute(body);
        loopend(id, stack);
    });
    ICOMMAND(getteamscore, "i", (int *team), intret(0));
    ICOMMAND(scoreboardpj, "i", (int *cn), intret(0));
    ICOMMAND(scoreboardping, "i", (int *cn), intret(player1 ? player1->ping : 0));
    ICOMMAND(scoreboardstatus, "i", (int *cn), intret(0xFFFFFF));
    ICOMMAND(scoreboardmultiplayer, "", (), intret(multiplayer(false)));
#endif
}
