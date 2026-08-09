#include "game.h"
#include "engine.h"

extern int simulationmaxdist;

namespace game
{
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

    struct npcdefinition
    {
        string id, name, model;
        int attitude, behavior, health, attackmillis;
        float damage, speed, wanderradius, aggrodist, fleedist;

        npcdefinition(const char *id = "")
            : attitude(NPC_NEUTRAL), behavior(NPC_WANDERING), health(20), attackmillis(1000), damage(1), speed(40), wanderradius(8),
              aggrodist(16), fleedist(12)
        {
            copystring(this->id, id);
            copystring(name, id);
            model[0] = '\0';
        }
    };

    struct npc : dynent
    {
        npcdefinition *definition;
        int instanceid, attitude, behavior, nextdecision, wanderpauseuntil, lastmovementmillis, lastjump, lastattack, lastdebugtext,
            renderlastmillis;
        float renderstride, totalhealth, parthealth[NUM_HUMANOID_HITBOXES];
        uint detachedparts;
        bool frozen, wanderpaused;
        vec spawn, destination;
        physent *target;
        vector<characterhitbox> hitboxes;

        npc(npcdefinition *definition, int instanceid)
            : definition(definition), instanceid(instanceid), attitude(definition->attitude), behavior(definition->behavior), nextdecision(0),
              wanderpauseuntil(0), lastmovementmillis(-1), lastjump(-1000), lastattack(-1000), lastdebugtext(-1000),
              renderlastmillis(-1), renderstride(0), totalhealth(definition->health), detachedparts(0), frozen(false), wanderpaused(false),
              spawn(0, 0, 0), destination(0, 0, 0), target(NULL)
        {
            type = ENT_PLAYER;
            state = CS_ALIVE;
            maxspeed = definition->speed;
            radius = xradius = yradius = 4.1f;
            maxheight = eyeheight = 28.0f;
            aboveeye = 2.0f;
            parthealth[HITBOX_TORSO] = definition->health;
            loopi(NUM_HUMANOID_HITBOXES - 1) parthealth[i + 1] = max(definition->health * 0.25f, 1.0f);
        }
    };

    struct severedlimb
    {
        physent body;
        string model;
        vec angularvelocity;
        float renderyaw, renderpitch, renderroll;
        int part, lastupdate, sleepmillis;
        bool sleeping;

        severedlimb(const char *model, int part)
            : angularvelocity(0, 0, 0), renderyaw(0), renderpitch(0), renderroll(0), part(part), lastupdate(lastmillis), sleepmillis(0),
              sleeping(false)
        {
            copystring(this->model, model);
            body.type = ENT_BOUNCE;
            body.state = CS_DEAD;
            body.collidetype = COLLIDE_OBB;
            body.yaw = body.pitch = body.roll = 0;
            body.radius = body.xradius = body.yradius = part == HITBOX_HEAD ? 4.0f : 2.0f;
            body.eyeheight = body.aboveeye = part == HITBOX_HEAD ? 4.0f : part >= HITBOX_LEFT_LEG ? 6.0f : 5.0f;
        }
    };

    enum
    {
        NPC_PART_TORSO = 0,
        NPC_PART_HEAD,
        NPC_PART_LEFT_ARM,
        NPC_PART_RIGHT_ARM,
        NPC_PART_LEFT_LEG,
        NPC_PART_RIGHT_LEG,
        NUM_NPC_PARTS
    };

    static const char * const npcparts[NUM_NPC_PARTS] =
    {
        "torso", "head", "arm/left", "arm/right", "leg/left", "leg/right"
    };

    static const char * const humanoidtags[NUM_NPC_PARTS] =
    {
        NULL, "tag_head", "tag_left_arm", "tag_right_arm", "tag_left_leg", "tag_right_leg"
    };

    static const char * const playerroot = "game/player";
    static const float HIP_HEIGHT = 11.25f;
    static vector<npcdefinition *> npcdefinitions;
    static vector<npc *> npcs;
    static vector<severedlimb *> severedlimbs;
    static int nextnpcid = 1, debughitboxenabled = 0, debugnpcenabled = 0;

    VARP(npcmaxdist, 1, 256, 4096);

    static const char *attitudename(int attitude)
    {
        static const char * const names[] = { "Aggressive", "Neutral", "Friendly", "Scared" };
        return attitude >= 0 && attitude < int(sizeof(names) / sizeof(names[0])) ? names[attitude] : "Neutral";
    }

    static const char *behaviorname(int behavior)
    {
        static const char * const names[] = { "Wandering", "Chase", "Flee" };
        return behavior >= 0 && behavior < int(sizeof(names) / sizeof(names[0])) ? names[behavior] : "Wandering";
    }

    static int parseattitude(const char *name)
    {
        if(!cubecasecmp(name, "aggressive")) return NPC_AGGRESSIVE;
        if(!cubecasecmp(name, "neutral")) return NPC_NEUTRAL;
        if(!cubecasecmp(name, "friendly")) return NPC_FRIENDLY;
        if(!cubecasecmp(name, "scared")) return NPC_SCARED;
        return -1;
    }

    static int parsebehavior(const char *name)
    {
        if(!cubecasecmp(name, "wandering") || !cubecasecmp(name, "wander")) return NPC_WANDERING;
        if(!cubecasecmp(name, "chase")) return NPC_CHASE;
        if(!cubecasecmp(name, "flee")) return NPC_FLEE;
        return -1;
    }

    static npcdefinition *findnpcdefinition(const char *id)
    {
        loopv(npcdefinitions) if(!cubecasecmp(npcdefinitions[i]->id, id)) return npcdefinitions[i];
        return NULL;
    }

    static void clearnpcdefinitions()
    {
        resetnpcs();
        npcdefinitions.deletecontents();
    }

    COMMANDN(npcreset, clearnpcdefinitions, "");

    ICOMMAND(npcdef, "sssssiffifff", (char *id, char *name, char *model, char *attitude, char *behavior, int *health, float *damage,
                                      float *speed, int *attackmillis, float *wanderradius, float *aggrodist, float *fleedist),
    {
        const int parsedattitude = parseattitude(attitude);
        const int parsedbehavior = parsebehavior(behavior);
        if(!id[0] || !model[0] || parsedattitude < 0 || parsedbehavior < 0 || *health <= 0 || *damage < 0 || *speed <= 0 ||
           *attackmillis <= 0 || *wanderradius < 0 || *aggrodist < 0 || *fleedist < 0)
        {
            conoutf(CON_ERROR, "invalid NPC definition %s", id[0] ? id : "<empty>");
            return;
        }

        npcdefinition *definition = findnpcdefinition(id);
        if(!definition) definition = npcdefinitions.add(new npcdefinition(id));
        copystring(definition->name, name[0] ? name : id);
        copystring(definition->model, model);
        definition->attitude = parsedattitude;
        definition->behavior = parsedbehavior;
        definition->health = *health;
        definition->damage = *damage;
        definition->speed = *speed;
        definition->attackmillis = *attackmillis;
        definition->wanderradius = *wanderradius;
        definition->aggrodist = *aggrodist;
        definition->fleedist = *fleedist;
    });

    ICOMMAND(npcload, "", (),
    {
        clearnpcdefinitions();
        if(!execfile("config/npcs.cfg", false)) conoutf(CON_ERROR, "could not load config/npcs.cfg");
    });

    ICOMMAND(debughitbox, "iN", (int *enabled, int *numargs),
    {
        debughitboxenabled = *numargs ? *enabled != 0 : !debughitboxenabled;
        intret(debughitboxenabled);
    });

    ICOMMAND(debugnpc, "iN", (int *enabled, int *numargs),
    {
        debugnpcenabled = *numargs ? *enabled != 0 : !debugnpcenabled;
        intret(debugnpcenabled);
    });

    static void npcmodelpath(const npcdefinition &definition, int part, string &path)
    {
        formatstring(path, "%s/%s", definition.model, npcparts[part]);
    }

    static void humanoidmodelpath(const char *root, int part, string &path)
    {
        formatstring(path, "%s/%s", root, npcparts[part]);
    }

    void preloadnpcs()
    {
        loopv(npcdefinitions) loopj(NUM_NPC_PARTS)
        {
            string path;
            npcmodelpath(*npcdefinitions[i], j, path);
            preloadmodel(path);
        }
    }

    void resetnpcs()
    {
        npcs.deletecontents();
        severedlimbs.deletecontents();
        nextnpcid = 1;
        cleardynentcache();
    }

    int numnpcs()
    {
        return npcs.length();
    }

    dynent *iternpc(int index)
    {
        return npcs.inrange(index) ? npcs[index] : NULL;
    }

    void rebasenpcs(float shiftx, float shifty)
    {
        loopv(npcs)
        {
            npc &mob = *npcs[i];
            mob.o.x -= shiftx;
            mob.o.y -= shifty;
            mob.newpos.x -= shiftx;
            mob.newpos.y -= shifty;
            mob.spawn.x -= shiftx;
            mob.spawn.y -= shifty;
            mob.destination.x -= shiftx;
            mob.destination.y -= shifty;
            loopvj(mob.hitboxes)
            {
                mob.hitboxes[j].center.x -= shiftx;
                mob.hitboxes[j].center.y -= shifty;
            }
        }
        loopv(severedlimbs)
        {
            severedlimb &limb = *severedlimbs[i];
            limb.body.o.x -= shiftx;
            limb.body.o.y -= shifty;
            limb.body.newpos.x -= shiftx;
            limb.body.newpos.y -= shifty;
        }
        cleardynentcache();
    }

    static vec rotatedradius(const vec &radius, float yaw)
    {
        const float c = fabsf(cosf(yaw * RAD)), s = fabsf(sinf(yaw * RAD));
        return vec(c * radius.x + s * radius.y, s * radius.x + c * radius.y, radius.z);
    }

    static vec humanoidoffset(const vec &origin, float yaw, float lateral, float forward, float vertical)
    {
        vec direction, result(origin);
        vecfromyawpitch(yaw, 0, 1, 0, direction);
        result.madd(direction, forward);
        vecfromyawpitch(yaw, 0, 0, 1, direction);
        result.madd(direction, lateral);
        return result.addz(vertical);
    }

    static void buildhumanoidhitboxes(dynent *entity, const char *root, float bodyyaw, vector<characterhitbox> &hitboxes)
    {
        hitboxes.setsize(0);
        if(!entity) return;

        const vec feet = entity->feetpos(), torsoorigin = vec(feet).addz(HIP_HEIGHT);
        string torso;
        humanoidmodelpath(root, NPC_PART_TORSO, torso);
        vec origins[NUM_NPC_PARTS];
        bool found[NUM_NPC_PARTS] = { false };
        modeltagpositions(torso, &humanoidtags[NPC_PART_HEAD], &origins[NPC_PART_HEAD], &found[NPC_PART_HEAD], NUM_NPC_PARTS - NPC_PART_HEAD,
                          torsoorigin, bodyyaw, 0, 0);

        if(!found[NPC_PART_HEAD]) origins[NPC_PART_HEAD] = humanoidoffset(torsoorigin, bodyyaw, 0, 0, 12);
        if(!found[NPC_PART_LEFT_ARM]) origins[NPC_PART_LEFT_ARM] = humanoidoffset(torsoorigin, bodyyaw, -6, 0, 10);
        if(!found[NPC_PART_RIGHT_ARM]) origins[NPC_PART_RIGHT_ARM] = humanoidoffset(torsoorigin, bodyyaw, 6, 0, 10);
        if(!found[NPC_PART_LEFT_LEG]) origins[NPC_PART_LEFT_LEG] = humanoidoffset(torsoorigin, bodyyaw, -2, 0, 0);
        if(!found[NPC_PART_RIGHT_LEG]) origins[NPC_PART_RIGHT_LEG] = humanoidoffset(torsoorigin, bodyyaw, 2, 0, 0);

        hitboxes.add(characterhitbox(vec(torsoorigin).addz(6), rotatedradius(vec(4, 2, 6), bodyyaw), HITBOX_TORSO));
        hitboxes.add(characterhitbox(vec(origins[NPC_PART_HEAD]).addz(4), rotatedradius(vec(4, 4, 4), bodyyaw), HITBOX_HEAD));
        hitboxes.add(characterhitbox(vec(origins[NPC_PART_LEFT_ARM]).addz(-5), rotatedradius(vec(2, 2, 5), bodyyaw), HITBOX_LEFT_ARM));
        hitboxes.add(characterhitbox(vec(origins[NPC_PART_RIGHT_ARM]).addz(-5), rotatedradius(vec(2, 2, 5), bodyyaw), HITBOX_RIGHT_ARM));
        hitboxes.add(characterhitbox(vec(origins[NPC_PART_LEFT_LEG]).addz(-6), rotatedradius(vec(2, 2, 6), bodyyaw), HITBOX_LEFT_LEG));
        hitboxes.add(characterhitbox(vec(origins[NPC_PART_RIGHT_LEG]).addz(-6), rotatedradius(vec(2, 2, 6), bodyyaw), HITBOX_RIGHT_LEG));
    }

    void getplayerhitboxes(gameent *d, vector<characterhitbox> &hitboxes)
    {
        buildhumanoidhitboxes(d, playerroot, d ? d->renderbodyyaw : 0, hitboxes);
    }

    static void updatenpchitboxes(npc &mob)
    {
        buildhumanoidhitboxes(&mob, mob.definition->model, mob.yaw, mob.hitboxes);
        for(int i = mob.hitboxes.length() - 1; i >= 0; --i)
            if(mob.detachedparts & (1U << mob.hitboxes[i].part)) mob.hitboxes.remove(i);
    }

    static characterhitbox *findnpchitbox(npc &mob, int part)
    {
        loopv(mob.hitboxes) if(mob.hitboxes[i].part == part) return &mob.hitboxes[i];
        return NULL;
    }

    static vec hitboxtagposition(const characterhitbox &hitbox)
    {
        vec position = hitbox.center;
        if(hitbox.part == HITBOX_HEAD) position.z -= hitbox.radius.z;
        else if(hitbox.part != HITBOX_TORSO) position.z += hitbox.radius.z;
        return position;
    }

    static void spawnblood(const vec &position, bool detached)
    {
        regular_particle_splash(PART_BLOOD, detached ? 9 : 3, detached ? 900 : 450, position, 0x60FFFF, detached ? 1.50f : 1.25f, detached ? 150 : 75, 2);
    }

    static void detachnpcpart(npc &mob, int part)
    {
        if(part == HITBOX_TORSO || mob.detachedparts & (1U << part)) return;
        characterhitbox *hitbox = findnpchitbox(mob, part);
        if(!hitbox) return;

        string model;
        npcmodelpath(*mob.definition, part, model);
        severedlimb *limb = new severedlimb(model, part);
        limb->body.o = hitbox->center;
        loopi(16)
        {
            if(!collide(&limb->body, vec(0, 0, 0), 0, false)) break;
            limb->body.o.z += 1.0f;
        }
        limb->body.vel = vec(mob.vel).mul(0.35f).add(vec(camdir).mul(24.0f));
        limb->body.vel.x += rnd(25) - 12;
        limb->body.vel.y += rnd(25) - 12;
        limb->body.vel.z = max(limb->body.vel.z + 35.0f + rnd(26), 28.0f);
        limb->body.resetinterp();
        limb->renderyaw = mob.yaw;
        const float gamespeed = horizontalmeterspersecond(&mob) * GAMEUNITSPERMETER,
                    movement = clamp(gamespeed / max(mob.maxheight * 2.25f, 1.0f), 0.0f, 1.0f),
                    stride = sinf(mob.renderstride) * movement;
        if(part == HITBOX_LEFT_ARM) limb->renderpitch = -stride * 28.0f;
        else if(part == HITBOX_RIGHT_ARM) limb->renderpitch = stride * 28.0f;
        else if(part == HITBOX_LEFT_LEG) limb->renderpitch = stride * 32.0f;
        else if(part == HITBOX_RIGHT_LEG) limb->renderpitch = -stride * 32.0f;
        limb->angularvelocity = vec(rnd(241) - 120, rnd(241) - 120, rnd(361) - 180);
        severedlimbs.add(limb);

        spawnblood(hitboxtagposition(*hitbox), true);
        mob.detachedparts |= 1U << part;
        updatenpchitboxes(mob);
    }

    static float npcdamagemultiplier(int part)
    {
        if(part == HITBOX_HEAD) return 2.0f;
        if(part == HITBOX_TORSO) return 1.0f;
        return 0.75f;
    }

    static float heldattackdamage()
    {
        const int item = selectedcreativeblock();
        if(item < 0) return 1.0f;
        const char *id = getinventoryitemid(item);
        if(!cubecasecmp(id, "stone_sword")) return 4.0f;
        if(!cubecasecmp(id, "wood_sword")) return 3.0f;
        return isinventorytool(item) ? 2.0f : 1.0f;
    }

    static void despawnnpc(npc *mob)
    {
        if(!mob) return;
        loopv(npcs) if(npcs[i]->target == mob) npcs[i]->target = NULL;
        const int index = npcs.find(mob);
        if(index < 0) return;
        delete mob;
        npcs.remove(index);
        cleardynentcache();
    }

    bool attacknpc()
    {
        if(multiplayer(false) || editmode || (!m_creative && !m_survival) || !player1 || player1->state != CS_ALIVE || !camera1) return false;

        static const float attackreach = 5.0f * GAMEUNITSPERMETER;
        float worlddistance = raycube(camera1->o, camdir, attackreach, RAY_CLIPMAT | RAY_SKIPFIRST);
        if(worlddistance < 0 || worlddistance > attackreach) worlddistance = attackreach;

        npc *hitmob = NULL;
        int hitpart = HITBOX_TORSO;
        float hitdistance = worlddistance;
        loopv(npcs)
        {
            npc &mob = *npcs[i];
            loopvj(mob.hitboxes)
            {
                const characterhitbox &hitbox = mob.hitboxes[j];
                const vec minimum = vec(hitbox.center).sub(hitbox.radius), size = vec(hitbox.radius).mul(2.0f);
                float distance = 0;
                int orient = -1;
                if(!rayboxintersect(minimum, size, camera1->o, camdir, distance, orient) || distance < 0 || distance > hitdistance) continue;
                hitmob = &mob;
                hitpart = hitbox.part;
                hitdistance = distance;
            }
        }
        if(!hitmob) return false;

        const float damage = heldattackdamage() * npcdamagemultiplier(hitpart);
        spawnblood(vec(camera1->o).madd(camdir, hitdistance), false);
        hitmob->totalhealth = max(hitmob->totalhealth - damage, 0.0f);
        if(hitpart != HITBOX_TORSO)
        {
            hitmob->parthealth[hitpart] = max(hitmob->parthealth[hitpart] - damage, 0.0f);
            if(hitmob->parthealth[hitpart] <= 0) detachnpcpart(*hitmob, hitpart);
        }
        if(hitmob->totalhealth <= 0) despawnnpc(hitmob);
        return true;
    }

    static void beginwanderpause(npc &mob)
    {
        int duration = 600 + rnd(1801);
        if(!rnd(5)) duration += 1000 + rnd(2501);
        mob.wanderpaused = true;
        mob.wanderpauseuntil = lastmillis + duration;
        mob.nextdecision = 0;
        mob.destination = mob.o;
    }

    static void pickwanderdestination(npc &mob)
    {
        const float radius = mob.definition->wanderradius * GAMEUNITSPERMETER,
                    angle = rnd(36000) * RAD / 100.0f,
                    distance = radius * (0.25f + 0.75f * sqrtf(rnd(10001) / 10000.0f));
        mob.destination = vec(mob.spawn).add(vec(cosf(angle) * distance, sinf(angle) * distance, 0));
        mob.wanderpaused = false;
        mob.wanderpauseuntil = 0;
        mob.nextdecision = lastmillis + 8000 + rnd(6001);
    }

    static physent *nearestnpctarget(npc &mob, float radius)
    {
        physent *best = NULL;
        float bestdistance = radius * radius;
        if(player1 && player1->state == CS_ALIVE)
        {
            const float distance = mob.o.squaredist(player1->o);
            if(distance <= bestdistance)
            {
                best = player1;
                bestdistance = distance;
            }
        }
        loopv(npcs)
        {
            npc *candidate = npcs[i];
            if(candidate == &mob || candidate->state != CS_ALIVE) continue;
            const float distance = mob.o.squaredist(candidate->o);
            if(distance <= bestdistance)
            {
                best = candidate;
                bestdistance = distance;
            }
        }
        return best;
    }

    static void updatebehavior(npc &mob)
    {
        const int previousbehavior = mob.behavior;
        if(mob.attitude == NPC_AGGRESSIVE)
        {
            mob.target = nearestnpctarget(mob, mob.definition->aggrodist * GAMEUNITSPERMETER);
            mob.behavior = mob.target ? NPC_CHASE : mob.definition->behavior;
        }
        else if(mob.attitude == NPC_SCARED)
        {
            mob.target = nearestnpctarget(mob, mob.definition->fleedist * GAMEUNITSPERMETER);
            mob.behavior = mob.target ? NPC_FLEE : mob.definition->behavior;
        }
        else
        {
            mob.target = NULL;
            mob.behavior = mob.definition->behavior;
        }

        if(mob.behavior == NPC_WANDERING)
        {
            if(previousbehavior != NPC_WANDERING) beginwanderpause(mob);
            if(mob.wanderpaused)
            {
                if(lastmillis >= mob.wanderpauseuntil) pickwanderdestination(mob);
                return;
            }
            vec delta = vec(mob.destination).sub(mob.o);
            delta.z = 0;
            if(mob.nextdecision <= lastmillis || delta.squaredlen() <= mob.radius * mob.radius) beginwanderpause(mob);
        }
        else if(mob.behavior == NPC_CHASE && mob.target) mob.destination = mob.target->feetpos();
        else if(mob.behavior == NPC_FLEE && mob.target)
        {
            vec away = vec(mob.o).sub(mob.target->o);
            away.z = 0;
            if(away.squaredlen() < 0.01f) vecfromyawpitch(mob.yaw + 180, 0, 1, 0, away);
            else away.normalize();
            mob.destination = vec(mob.o).madd(away, mob.definition->fleedist * GAMEUNITSPERMETER);
        }
    }

    static void walktowardsdestination(npc &mob)
    {
        const int elapsed = mob.lastmovementmillis < 0 || lastmillis < mob.lastmovementmillis
                          ? clamp(curtime, 0, 100)
                          : clamp(lastmillis - mob.lastmovementmillis, 0, 100);
        mob.lastmovementmillis = lastmillis;
        if(mob.behavior == NPC_WANDERING && mob.wanderpaused)
        {
            mob.stopmoving();
            moveplayer(&mob, 10, true);
            return;
        }

        vec direction = vec(mob.destination).sub(mob.o);
        direction.z = 0;
        const float stopdistance = max(mob.radius, 2.0f);
        if(direction.squaredlen() <= stopdistance * stopdistance)
        {
            mob.stopmoving();
            moveplayer(&mob, 10, true);
            return;
        }

        const float wantedyaw = fmodf(-atan2f(direction.x, direction.y) / RAD + 360.0f, 360.0f);
        float turn = fmodf(wantedyaw - mob.yaw + 540.0f, 360.0f) - 180.0f;
        const float maxturn = 180.0f * elapsed / 1000.0f;
        mob.yaw = fmodf(mob.yaw + clamp(turn, -maxturn, maxturn) + 360.0f, 360.0f);
        if(fabsf(turn) > 6.0f)
        {
            mob.stopmoving();
            moveplayer(&mob, 10, true);
            return;
        }

        mob.move = 1;
        mob.strafe = 0;
        if(mob.blocked && mob.physstate >= PHYS_SLOPE && lastmillis - mob.lastjump >= 400)
        {
            mob.jumping = true;
            mob.lastjump = lastmillis;
        }
        moveplayer(&mob, 10, true);
        if(mob.blocked && mob.behavior == NPC_WANDERING) mob.nextdecision = min(mob.nextdecision, lastmillis + 1500);
    }

    static void shownpcdebugtext(npc &mob)
    {
        if(!debugnpcenabled || lastmillis - mob.lastdebugtext < 100) return;
        mob.lastdebugtext = lastmillis;
        const char *targetname = "none";
        if(mob.target == player1) targetname = player1 && player1->name[0] ? player1->name : "player";
        else loopv(npcs) if(npcs[i] == mob.target)
        {
            targetname = npcs[i]->definition->name;
            break;
        }
        const char *activity = mob.frozen ? " / Frozen" : mob.behavior == NPC_WANDERING && mob.wanderpaused ? " / Paused" : "";
        defformatstring(text, "%s #%d\n%s / %s%s\nHP %.2f/%d  target: %s", mob.definition->name, mob.instanceid, attitudename(mob.attitude),
                        behaviorname(mob.behavior), activity, mob.totalhealth, mob.definition->health, targetname);
        particle_textcopy(mob.abovehead(), text, PART_TEXT, 150, 0xFFE28A, 1.5f, 0);
    }

    static void updateseveredlimbs()
    {
        loopv(severedlimbs)
        {
            severedlimb &limb = *severedlimbs[i];
            if(limb.sleeping) continue;

            const int elapsed = clamp(lastmillis - limb.lastupdate, 0, 100);
            limb.lastupdate = lastmillis;
            bounce(&limb.body, 0.38f, 3.0f, 1.0f);
            limb.renderyaw = fmodf(limb.renderyaw + limb.angularvelocity.z * elapsed / 1000.0f + 360.0f, 360.0f);
            limb.renderpitch = fmodf(limb.renderpitch + limb.angularvelocity.x * elapsed / 1000.0f + 360.0f, 360.0f);
            limb.renderroll = fmodf(limb.renderroll + limb.angularvelocity.y * elapsed / 1000.0f + 360.0f, 360.0f);
            limb.angularvelocity.mul(powf(0.35f, elapsed / 1000.0f));

            const float groundreach = limb.body.eyeheight + 0.75f,
                        grounddistance = raycube(limb.body.o, vec(0, 0, -1), groundreach, RAY_CLIPMAT | RAY_SKIPFIRST);
            if(limb.body.vel.squaredlen() <= 6.25f && grounddistance >= 0 && grounddistance < groundreach)
            {
                if(!limb.sleepmillis) limb.sleepmillis = lastmillis;
                else if(lastmillis - limb.sleepmillis >= 650)
                {
                    limb.sleeping = true;
                    limb.body.vel = limb.body.falling = vec(0, 0, 0);
                    limb.body.newpos = limb.body.o;
                    limb.body.deltapos = vec(0, 0, 0);
                    limb.angularvelocity = vec(0, 0, 0);
                }
            }
            else limb.sleepmillis = 0;
        }
    }

    void updatenpcs()
    {
        if(multiplayer(false))
        {
            if(npcs.length()) resetnpcs();
            return;
        }

        const vec focus = camera1 ? camera1->o : player1 ? player1->o : vec(0, 0, 0);
        const float despawndistance = npcmaxdist * GAMEUNITSPERMETER;
        bool removed = false;
        for(int i = 0; i < npcs.length();)
        {
            npc *mob = npcs[i];
            if(mob->o.squaredist(focus) <= despawndistance * despawndistance)
            {
                ++i;
                continue;
            }
            loopv(npcs) if(npcs[i]->target == mob) npcs[i]->target = NULL;
            delete mob;
            npcs.remove(i);
            removed = true;
        }
        if(removed) cleardynentcache();

        const float simulationdistance = simulationmaxdist * GAMEUNITSPERMETER;
        loopv(npcs)
        {
            npc &mob = *npcs[i];
            mob.frozen = mob.o.squaredist(focus) > simulationdistance * simulationdistance;
            if(mob.frozen)
            {
                mob.stopmoving();
                mob.vel = mob.falling = vec(0, 0, 0);
                mob.newpos = mob.o;
                mob.deltapos = vec(0, 0, 0);
            }
            else
            {
                updatebehavior(mob);
                walktowardsdestination(mob);
            }
            updatenpchitboxes(mob);
            shownpcdebugtext(mob);
        }
        updateseveredlimbs();
    }

    static void rendernpc(npc &mob)
    {
        string models[NUM_NPC_PARTS];
        loopi(NUM_NPC_PARTS) npcmodelpath(*mob.definition, i, models[i]);

        const int flags = MDL_CULL_VFC | MDL_CULL_DIST | MDL_CULL_OCCLUDED;
        const float speed = horizontalmeterspersecond(&mob),
                    gamespeed = speed * GAMEUNITSPERMETER,
                    fullswingspeed = max(mob.maxheight * 2.25f, 1.0f),
                    gaitcycletravel = max(mob.maxheight * 0.75f, 1.0f),
                    movement = clamp(gamespeed / fullswingspeed, 0.0f, 1.0f);
        if(mob.renderlastmillis < 0 || lastmillis < mob.renderlastmillis) mob.renderlastmillis = lastmillis;
        const int elapsed = min(lastmillis - mob.renderlastmillis, 100);
        mob.renderlastmillis = lastmillis;
        if(gamespeed > 0.05f)
            mob.renderstride = fmodf(mob.renderstride + 2.0f * PI * gamespeed * elapsed / (1000.0f * gaitcycletravel), 2.0f * PI);

        const float stride = sinf(mob.renderstride) * movement, legpitch = stride * 32.0f, armpitch = stride * 28.0f;
        const vec torsoorigin = mob.feetpos(fabsf(cosf(mob.renderstride)) * 0.45f * movement).addz(HIP_HEIGHT);
        vec origins[NUM_NPC_PARTS];
        bool found[NUM_NPC_PARTS] = { false };
        rendermodel(models[NPC_PART_TORSO], ANIM_MAPMODEL | ANIM_LOOP, torsoorigin, mob.yaw, 0, 0, flags, &mob);
        modeltagpositions(models[NPC_PART_TORSO], &humanoidtags[NPC_PART_HEAD], &origins[NPC_PART_HEAD], &found[NPC_PART_HEAD],
                          NUM_NPC_PARTS - NPC_PART_HEAD, torsoorigin, mob.yaw, 0, 0);
        if(found[NPC_PART_HEAD] && !(mob.detachedparts & (1U << HITBOX_HEAD)))
            rendermodel(models[NPC_PART_HEAD], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_HEAD], mob.yaw, 0, 0, flags, &mob);
        if(found[NPC_PART_LEFT_ARM] && !(mob.detachedparts & (1U << HITBOX_LEFT_ARM)))
            rendermodel(models[NPC_PART_LEFT_ARM], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_LEFT_ARM], mob.yaw, -armpitch, 0, flags, &mob);
        if(found[NPC_PART_RIGHT_ARM] && !(mob.detachedparts & (1U << HITBOX_RIGHT_ARM)))
            rendermodel(models[NPC_PART_RIGHT_ARM], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_RIGHT_ARM], mob.yaw, armpitch, 0, flags, &mob);
        if(found[NPC_PART_LEFT_LEG] && !(mob.detachedparts & (1U << HITBOX_LEFT_LEG)))
            rendermodel(models[NPC_PART_LEFT_LEG], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_LEFT_LEG], mob.yaw, legpitch, 0, flags, &mob);
        if(found[NPC_PART_RIGHT_LEG] && !(mob.detachedparts & (1U << HITBOX_RIGHT_LEG)))
            rendermodel(models[NPC_PART_RIGHT_LEG], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_RIGHT_LEG], mob.yaw, -legpitch, 0, flags, &mob);
    }

    static void renderseveredlimb(const severedlimb &limb)
    {
        vec offset(0, 0, limb.part == HITBOX_HEAD ? -limb.body.aboveeye : limb.body.eyeheight);
        offset.rotate_around_x(limb.renderpitch * RAD).rotate_around_y(limb.renderroll * RAD).rotate_around_z(limb.renderyaw * RAD);
        const vec origin = vec(limb.body.o).add(offset);
        const int flags = MDL_CULL_VFC | MDL_CULL_DIST | MDL_CULL_OCCLUDED;
        rendermodel(limb.model, ANIM_MAPMODEL | ANIM_LOOP, origin, limb.renderyaw, limb.renderpitch, limb.renderroll, flags);
    }

    void rendernpcs()
    {
        loopv(npcs) rendernpc(*npcs[i]);
        loopv(severedlimbs) renderseveredlimb(*severedlimbs[i]);
    }

    void rendernpcdebug()
    {
        if(!debughitboxenabled) return;
        vector<characterhitbox> hitboxes;
        loopv(players) if(players[i] && (players[i]->state == CS_ALIVE || players[i]->state == CS_EDITING))
        {
            getplayerhitboxes(players[i], hitboxes);
            loopvj(hitboxes) renderboundingbox(hitboxes[j].center, hitboxes[j].radius);
        }
        loopv(npcs) loopvj(npcs[i]->hitboxes) renderboundingbox(npcs[i]->hitboxes[j].center, npcs[i]->hitboxes[j].radius);
        loopv(severedlimbs) renderboundingbox(severedlimbs[i]->body.o,
                                              vec(severedlimbs[i]->body.xradius, severedlimbs[i]->body.yradius,
                                                  severedlimbs[i]->body.eyeheight));
    }

    static bool spawnnpc(const char *id)
    {
        if(multiplayer(false))
        {
            conoutf(CON_ERROR, "NPCs are client-side and cannot be spawned in multiplayer");
            return false;
        }
        npcdefinition *definition = findnpcdefinition(id);
        if(!definition)
        {
            conoutf(CON_ERROR, "unknown NPC id: %s", id);
            return false;
        }
        if(!camera1)
        {
            conoutf(CON_ERROR, "cannot spawn an NPC without an active camera");
            return false;
        }

        const float reach = npcmaxdist * GAMEUNITSPERMETER;
        vec surface;
        const float distance = raycubepos(camera1->o, camdir, surface, reach, RAY_CLIPMAT | RAY_POLY | RAY_SKIPFIRST);
        if(distance < 0 || distance >= reach)
        {
            conoutf(CON_ERROR, "the camera is not pointing at a spawn surface");
            return false;
        }

        npc *mob = new npc(definition, nextnpcid);
        vec horizontal(camdir.x, camdir.y, 0);
        if(horizontal.squaredlen() > 0.01f) horizontal.normalize();
        vec feet = vec(surface).madd(horizontal, -(mob->radius + 0.5f));
        const float groundrange = mob->maxheight * 4.0f;
        vec groundprobe = vec(feet).addz(mob->maxheight + 8.0f);
        const float grounddistance = raycube(groundprobe, vec(0, 0, -1), groundrange, RAY_CLIPMAT | RAY_POLY | RAY_SKIPFIRST);
        feet.z = grounddistance < groundrange ? groundprobe.z - grounddistance + 0.1f : surface.z + 0.1f;
        mob->o = feet;
        mob->yaw = fmodf(camera1->yaw + 180.0f, 360.0f);
        if(!entinmap(mob, true))
        {
            conoutf(CON_ERROR, "there is not enough room to spawn %s there", definition->name);
            delete mob;
            return false;
        }
        ++nextnpcid;
        mob->spawn = mob->destination = mob->o;
        npcs.add(mob);
        beginwanderpause(*mob);
        updatenpchitboxes(*mob);
        cleardynentcache();
        conoutf("spawned %s #%d", definition->name, mob->instanceid);
        return true;
    }

    ICOMMAND(spawn, "s", (char *id), spawnnpc(id));
}
