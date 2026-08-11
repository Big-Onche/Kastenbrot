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
        int action, orient, item;
        ivec target;

        predictedworldaction() : requestid(0), action(-1), orient(0), item(-1), target(0, 0, 0) {}
    };

    static vector<predictedworldaction *> predictedworldactions;
    static vector<worlddrop *> worlddrops;
    static vector<fallingblock *> fallingblocks;
    static vector<ivec> fallblockchecks;
    static uint nextlocaldropid = 1;
    static uint nextlocalfallblockid = 1;
    static int personaldrops = 0, droptimeout = 300, maxdrop = 1024, dynamicentsmaxdistance = 64, requireconfirmeditems = 1;
    static void updateworlddrops();
    static void updatefallingblocks();
    static void queuefallblockcheck(const ivec &cell);
    static void updatefurnaces();
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

    static string connectpass = "";
    static int lastpositionsend = -1000;
    static string sentname = "";
#ifndef STANDALONE
    static void updatesurvivalbreaking();
    static void cancelclientbreakrequest(uint requestid);
    static void hidedeathscreen();
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
                mpeditface(-1, 1, sel, false);
                paintworldcube(type, placed, false);
                waterterrainchanged(absoluteplacedorigin);
                if(!waitforserveredit()) queuefallblockcheck(absoluteplacedorigin);
                return true;
            }
            case WORLD_ACTION_PLACE_SCATTER:
                return getworlditemtype(item) == WORLD_ITEM_SCATTER && applyworldscatteraction(item, target, orient, true);
            case WORLD_ACTION_PLACE_ITEM:
                return getworlditemtype(item) == WORLD_ITEM_PLACEABLE && applyworldscatteraction(item, target, orient, true);
            case WORLD_ACTION_BREAK_CUBE_START:
                mpdelcube(sel, false);
                waterterrainchanged(absolutetarget);
                if(!waitforserveredit()) queuefallblockcheck(ivec(absolutetarget).add(ivec(0, 0, 16)));
                return true;
            case WORLD_ACTION_BREAK_SCATTER_START:
                return (getworlditemtype(item) == WORLD_ITEM_SCATTER || getworlditemtype(item) == WORLD_ITEM_PLACEABLE) &&
                       applyworldscatteraction(item, target, orient, false);
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
            selinfo sel;
            worldactionselection(sel, prediction.target, prediction.orient);
            worldselectiontolocal(sel);
            editworldscatter(getworlditemindex(prediction.item), sel.o, prediction.orient, false);
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
            if(type >= 0) editworldscatter(type, sel.o, prediction.orient, true);
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
        resetfurnaces();
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
        // A listen server owns a separate seed and journal. Leaving it connected
        // would make its N_WORLDSTATE replace the saved world after loading.
        if(isconnected(false, false)) disconnect(false, false);
        if(isconnected(false, true)) server::localdisconnect(false);
#endif
        connected = remote = false;
        localworldactive = true;
        resetwatersimulationsettings();
        resetfallingblocks();
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
            setworldeditauthor(edit.author);
            setworldeditrevision(edit.revision);
            return applyworldaction(edit.args[0], ivec(edit.args[1], edit.args[2], edit.args[3]), edit.args[4], edit.args[5]);
        }

        selinfo sel = edit.selection;
        worldselectiontolocal(sel);
        if(!sel.validate() || !worldselectionready(sel)) return false;

        setworldeditauthor(edit.author);
        setworldeditrevision(edit.revision);
        switch(edit.type)
        {
            case N_EDITF: mpeditface(edit.args[0], edit.args[1], sel, false); break;
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
        updateworlddrops();
        updatefallingblocks();
        updatefurnaces();
#endif
        gets2c();
        c2sinfo();
    }

    void physicstrigger(physent *d, bool local, int floorlevel, int waterlevel, int material) {}
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
        putint(p, type);
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
        resetnpcs();
        resetwatersimulation();
        if(!pendingnetworkworld) resetworlddrops();
        if(!pendingnetworkworld) resetfallingblocks();
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
        worldactionselection(selection, localtarget, orient);
        if(waitforserveredit()) worldselectiontoabsolute(selection);
        addmsg(N_WORLDACTION, "ri8", int(requestid), action, selection.o.x, selection.o.y, selection.o.z, orient, item, slot);
    }

    static void addpredictedworldaction(uint requestid, int action, const ivec &absolutetarget, int orient, int item)
    {
        predictedworldaction *prediction = new predictedworldaction;
        prediction->requestid = requestid;
        prediction->action = action;
        prediction->target = absolutetarget;
        prediction->orient = orient;
        prediction->item = item;
        predictedworldactions.add(prediction);
    }

    static uint predictworldaction(int action, const ivec &localtarget, int orient, int item, int slot)
    {
        const uint requestid = newworldrequestid();
        selinfo selection;
        worldactionselection(selection, localtarget, orient);
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
        if(open) execute("hideui survival_inventory; hideui crafting_table; showui furnace");
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
        execute("hideui survival_inventory; hideui crafting_table; showui furnace");
#endif
        return true;
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
        return multiplayer(false) ? authoritativenpcsimulationmaxdist : simulationmaxdist;
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
            ok = f->printf("inventory %d %d %d %d\n", i, survivalitems[i], survivalcounts[i], survivaldurabilities[i]) > 0;
        if(ok && inventorycursoritem >= 0 && inventorycursorcount > 0)
            ok = f->printf("inventory_cursor %d %d %d\n", inventorycursoritem, inventorycursorcount, inventorycursordurability) > 0;
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
        execute("hideui survival_inventory; hideui crafting_table; hideui furnace; showui death_screen");
    }

    static void hidedeathscreen()
    {
        execute("hideui death_screen");
        thirdperson = deaththirdperson;
    }

    static void setplayerdead(gameent &d, const vec &impulse)
    {
        if(d.state == CS_DEAD) return;
        d.state = CS_DEAD;
        d.collidetype = COLLIDE_NONE;
        d.stopmoving();
        d.vel = d.falling = vec(0, 0, 0);
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
        d.collidetype = COLLIDE_ELLIPSE;
        d.stopmoving();
        d.vel = d.falling = vec(0, 0, 0);
        d.resetinterp();
        if(&d == player1) hidedeathscreen();
        cleardynentcache();
    }

    void damageplayer(float damage, const vec &source)
    {
        if(!m_survival || multiplayer(false) || !player1 || player1->state != CS_ALIVE || damage <= 0) return;
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
        if(multiplayer(false)) addmsg(N_RESPAWN, "r");
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
        int type, entity;
        selinfo cube;
        vec center, radius, hitpoint;

        creativetarget() : type(CREATIVE_TARGET_NONE), entity(-1), center(0, 0, 0), radius(0, 0, 0), hitpoint(0, 0, 0) {}
    };

    static bool findcreativetarget(creativetarget &target)
    {
        if(!buildenabled()) return false;

        const vec origin = camera1 ? camera1->o : player1->o;
        int orient = -1, entity = -1;
        rayent(origin, camdir, buildactionreach(), RAY_CLIPMAT | RAY_ENTS | RAY_SKIPFIRST,
               CREATIVE_GRID, orient, entity);
        if(entity >= 0 && isworldscatterentity(entity) &&
           getworldscatterentitybox(entity, target.center, target.radius))
        {
            target.type = CREATIVE_TARGET_SCATTER;
            target.entity = entity;
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
        execute("hideui survival_inventory; showui crafting_table");
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
            if(!editworldscatter(worldindex, hit.o, hit.orient, true)) return;
            if(waitforserveredit())
                predictworldaction(type == WORLD_ITEM_PLACEABLE ? WORLD_ACTION_PLACE_ITEM : WORLD_ACTION_PLACE_SCATTER,
                                   hit.o, hit.orient, selected, clampcreativehotbarslot());
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
        if(!waitforserveredit())
        {
            mpeditface(-1, 1, hit, true);
            paintworldcube(worldindex, placed, true);
            selinfo absolute = placed;
            worldselectiontoabsolute(absolute);
            waterterrainchanged(absolute.o);
            queuefallblockcheck(absolute.o);
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
            int type, mountorient;
            ivec support;
            if(getworldscatterentityedit(target.entity, type, support, mountorient))
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

    static void cancelsurvivalbreak()
    {
        const uint requestid = survivalbreakrequestid;
        if(survivalbreakrequestid && waitforserveredit())
        {
            if(survivalbreaktarget.type == CREATIVE_TARGET_SCATTER)
            {
                int type, orient;
                ivec support;
                if(getworldscatterentityedit(survivalbreaktarget.entity, type, support, orient))
                    sendworldaction(survivalbreakrequestid, WORLD_ACTION_BREAK_CANCEL, support, orient, getworldscatteritem(type), -1);
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
        if(a.type == CREATIVE_TARGET_SCATTER) return a.entity == b.entity;
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
            int orient;
            ivec support;
            if(!getworldscatterentityedit(target.entity, index, support, orient)) return false;
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
                    int type, mountorient;
                    ivec support;
                    if(!getworldscatterentityedit(target.entity, type, support, mountorient))
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
            int type, mountorient;
            ivec support;
            if(getworldscatterentityedit(survivalbreaktarget.entity, type, support, mountorient))
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
                else if(survivalenabled() && !hitnpc) updatesurvivalbreaking();
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
    ICOMMAND(creativeplaceblock, "D", (int *down), { if(*down) creativeplace(); });
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
        uchar physstate = d->physstate | ((d->move&3)<<4) | ((d->strafe&3)<<6);
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
