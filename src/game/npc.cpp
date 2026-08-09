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
            renderlastmillis, staggeruntil, crawlstart, lastlimbhop, ragdollstart[6], ragdollend[6], partdetachedmillis[6], partlastblood[6];
        float renderstride, totalhealth, parthealth[NUM_HUMANOID_HITBOXES];
        uint detachedparts;
        bool frozen, wanderpaused;
        vec spawn, destination;
        physent *target;
        vector<characterhitbox> hitboxes;

        npc(npcdefinition *definition, int instanceid)
            : definition(definition), instanceid(instanceid), attitude(definition->attitude), behavior(definition->behavior), nextdecision(0),
              wanderpauseuntil(0), lastmovementmillis(-1), lastjump(-1000), lastattack(-1000), lastdebugtext(-1000),
              renderlastmillis(-1), staggeruntil(0), crawlstart(0), lastlimbhop(-1000), renderstride(0),
              totalhealth(definition->health), detachedparts(0), frozen(false), wanderpaused(false), spawn(0, 0, 0), destination(0, 0, 0),
              target(NULL)
        {
            type = ENT_PLAYER;
            state = CS_ALIVE;
            maxspeed = definition->speed;
            radius = xradius = yradius = 4.1f;
            maxheight = eyeheight = 28.0f;
            aboveeye = 2.0f;
            loopi(6)
            {
                ragdollstart[i] = ragdollend[i] = -1;
                partdetachedmillis[i] = partlastblood[i] = 0;
            }
            parthealth[HITBOX_TORSO] = definition->health;
            loopi(NUM_HUMANOID_HITBOXES - 1) parthealth[i + 1] = max(definition->health * 0.25f, 1.0f);
        }
    };

    struct severedlimb
    {
        physent body;
        string model;
        vec angularvelocity, localradius, modelcenter;
        float renderyaw, renderpitch, renderroll, settlepitch, settleroll;
        int part, lastupdate, sleepmillis, settleaxis, detachedmillis, lastblood;
        bool sleeping;

        severedlimb(const char *model, int part)
            : angularvelocity(0, 0, 0), localradius(part == HITBOX_HEAD ? vec(4, 4, 4) : vec(2, 2, part >= HITBOX_LEFT_LEG ? 6 : 5)),
              modelcenter(0, 0, part == HITBOX_HEAD ? localradius.z : -localradius.z), renderyaw(0), renderpitch(0), renderroll(0),
              settlepitch(0), settleroll(0), part(part), lastupdate(lastmillis), sleepmillis(0), settleaxis(0), detachedmillis(lastmillis),
              lastblood(lastmillis), sleeping(false)
        {
            copystring(this->model, model);
            ::model *geometry = loadmodel(model);
            if(geometry) geometry->collisionbox(modelcenter, localradius);
            body.type = ENT_BOUNCE;
            body.state = CS_DEAD;
            body.collidetype = COLLIDE_OBB;
            body.yaw = body.pitch = body.roll = 0;
            body.obbradius = localradius;
            body.radius = max(localradius.x, localradius.y);
            body.xradius = localradius.x;
            body.yradius = localradius.y;
            body.eyeheight = body.aboveeye = localradius.z;
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
    static const float STANDING_HEIGHT = 28.0f, CRAWLING_EYEHEIGHT = 6.0f, CRAWLING_ABOVEEYE = 4.0f;
    static vector<npcdefinition *> npcdefinitions;
    static vector<npc *> npcs;
    static vector<severedlimb *> severedlimbs;
    static int nextnpcid = 1, debughitboxenabled = 0, debugnpcenabled = 0;

    VARP(npcmaxdist, 1, 256, 4096);
    VARP(npcdebrisduration, 1000, 20000, 120000);
    VARP(npcdebrissinktime, 250, 3000, 10000);
    VAR(npcbleedduration, 0, 2000, 10000);
    FVAR(npclimbvelocitymultiplier, 0.0f, 8.0f, 10.0f);
    FVAR(npclimbangularvelocitymultiplier, 0.0f, 4.0f, 10.0f);
    FVAR(npclimbbounciness, 0.0f, 0.75f, 8.0f);
    FVAR(npcragdollvelocitymultiplier, 0.0f, 2.5f, 10.0f);
    FVARP(npcragdollheadmaxangle, 15.0f, 55.0f, 120.0f);
    FVARP(npcragdollarmmaxangle, 30.0f, 95.0f, 160.0f);
    FVARP(npcragdolllegmaxangle, 15.0f, 70.0f, 120.0f);

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
        const vec ragdolloffset(-shiftx, -shifty, 0);
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
            translateragdoll(&mob, ragdolloffset);
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

    static int npcremaininglegs(const npc &mob)
    {
        int legs = 2;
        if(mob.detachedparts & (1U << HITBOX_LEFT_LEG)) --legs;
        if(mob.detachedparts & (1U << HITBOX_RIGHT_LEG)) --legs;
        return legs;
    }

    static matrix3 modelorientation(float yaw, float pitch, float roll)
    {
        matrix3 orient;
        orient.identity();
        if(roll) orient.rotate_around_y(sincosmod360(roll));
        if(pitch) orient.rotate_around_x(sincosmod360(-pitch));
        if(yaw) orient.rotate_around_z(sincosmod360(-yaw));
        return orient;
    }

    static vec transformedmodelvector(const vec &value, float yaw, float pitch, float roll)
    {
        return modelorientation(yaw, pitch, roll).transposedtransform(value);
    }

    static vec transformedmodelradius(const vec &radius, float yaw, float pitch, float roll)
    {
        return modelorientation(yaw, pitch, roll).abstransposedtransform(radius);
    }

    struct npcpose
    {
        vec torsoorigin;
        float torsopitch, torsoroll, headpitch, leftarmpitch, rightarmpitch, leftlegpitch, rightlegpitch, crawlprogress;

        npcpose() : torsoorigin(0, 0, 0), torsopitch(0), torsoroll(0), headpitch(0), leftarmpitch(0), rightarmpitch(0), leftlegpitch(0),
                    rightlegpitch(0), crawlprogress(0)
        {
        }
    };

    static void calcnpcpose(npc &mob, npcpose &pose)
    {
        const int legs = npcremaininglegs(mob);
        const float gamespeed = horizontalmeterspersecond(&mob) * GAMEUNITSPERMETER,
                    movement = clamp(gamespeed / max(STANDING_HEIGHT * 2.25f, 1.0f), 0.0f, 1.0f),
                    stride = sinf(mob.renderstride) * movement,
                    smoothcrawl = legs ? 0.0f : clamp((lastmillis - mob.crawlstart) / 350.0f, 0.0f, 1.0f);
        pose.crawlprogress = smoothcrawl * smoothcrawl * (3.0f - 2.0f * smoothcrawl);
        pose.torsopitch = -90.0f * pose.crawlprogress;
        pose.headpitch = -30.0f * pose.crawlprogress;
        pose.leftarmpitch = -stride * 28.0f;
        pose.rightarmpitch = stride * 28.0f;
        pose.leftlegpitch = stride * 32.0f;
        pose.rightlegpitch = -stride * 32.0f;

        float torsoheight = HIP_HEIGHT + fabsf(cosf(mob.renderstride)) * 0.45f * movement;
        if(legs == 1)
        {
            const bool leftmissing = (mob.detachedparts & (1U << HITBOX_LEFT_LEG)) != 0;
            pose.torsoroll = (leftmissing ? -8.0f : 8.0f) * (0.65f + 0.35f * fabsf(sinf(mob.renderstride)));
            torsoheight += fabsf(sinf(mob.renderstride)) * 0.9f * movement;
            pose.leftlegpitch *= 0.75f;
            pose.rightlegpitch *= 0.75f;
        }
        else if(!legs)
        {
            const float crawlstroke = sinf(mob.renderstride) * 35.0f * movement;
            const float leftarmtarget = clamp(110.0f + crawlstroke, 90.0f, 140.0f),
                        rightarmtarget = clamp(110.0f - crawlstroke, 90.0f, 140.0f);
            torsoheight += (2.25f - torsoheight) * pose.crawlprogress;
            torsoheight += fabsf(sinf(mob.renderstride * 2.0f)) * 0.3f * movement * pose.crawlprogress;
            pose.leftarmpitch += (leftarmtarget - pose.leftarmpitch) * pose.crawlprogress;
            pose.rightarmpitch += (rightarmtarget - pose.rightarmpitch) * pose.crawlprogress;
        }
        pose.torsoorigin = mob.feetpos(torsoheight);
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

    static bool npcjointposition(npc &mob, int part, vec &position)
    {
        if(part <= NPC_PART_TORSO || part >= NUM_NPC_PARTS) return false;
        if(mob.state == CS_DEAD && getragdollvertex(&mob, mob.ragdollstart[part], position)) return true;

        npcpose pose;
        calcnpcpose(mob, pose);
        string torso;
        humanoidmodelpath(mob.definition->model, NPC_PART_TORSO, torso);
        if(modeltagposition(torso, humanoidtags[part], position, pose.torsoorigin, mob.yaw, pose.torsopitch, pose.torsoroll)) return true;

        static const vec fallbackoffsets[NUM_NPC_PARTS] =
        {
            vec(0, 0, 0), vec(0, 0, 12), vec(-6, 0, 10), vec(6, 0, 10), vec(-2, 0, 0), vec(2, 0, 0)
        };
        position = vec(pose.torsoorigin).add(transformedmodelvector(fallbackoffsets[part], mob.yaw, pose.torsopitch, pose.torsoroll));
        return true;
    }

    static bool npcjointdirection(npc &mob, int part, vec &direction)
    {
        if(part != NPC_PART_HEAD && part != NPC_PART_LEFT_ARM && part != NPC_PART_RIGHT_ARM) return false;
        direction = vec(0, 0, 0);
        if(mob.state == CS_DEAD)
        {
            vec leftshoulder, rightshoulder;
            if(part == NPC_PART_HEAD)
            {
                vec pelvis, neck;
                if(getragdollvertex(&mob, mob.ragdollstart[NPC_PART_TORSO], pelvis) &&
                   getragdollvertex(&mob, mob.ragdollstart[NPC_PART_HEAD], neck)) direction = vec(neck).sub(pelvis);
            }
            else if(getragdollvertex(&mob, mob.ragdollstart[NPC_PART_LEFT_ARM], leftshoulder) &&
                    getragdollvertex(&mob, mob.ragdollstart[NPC_PART_RIGHT_ARM], rightshoulder))
                direction = part == NPC_PART_LEFT_ARM ? vec(leftshoulder).sub(rightshoulder) : vec(rightshoulder).sub(leftshoulder);
            if(!direction.iszero())
            {
                direction.normalize();
                return true;
            }
        }

        npcpose pose;
        calcnpcpose(mob, pose);
        const vec localdirection = part == NPC_PART_HEAD ? vec(0, 0, 1) : part == NPC_PART_LEFT_ARM ? vec(-1, 0, 0) : vec(1, 0, 0);
        direction = transformedmodelvector(localdirection, mob.yaw, pose.torsopitch, pose.torsoroll).safenormalize();
        return true;
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
        mob.hitboxes.setsize(0);
        npcpose pose;
        calcnpcpose(mob, pose);
        string torso;
        humanoidmodelpath(mob.definition->model, NPC_PART_TORSO, torso);
        vec origins[NUM_NPC_PARTS];
        bool found[NUM_NPC_PARTS] = { false };
        modeltagpositions(torso, &humanoidtags[NPC_PART_HEAD], &origins[NPC_PART_HEAD], &found[NPC_PART_HEAD], NUM_NPC_PARTS - NPC_PART_HEAD,
                          pose.torsoorigin, mob.yaw, pose.torsopitch, pose.torsoroll);

        if(!found[NPC_PART_HEAD]) origins[NPC_PART_HEAD] = humanoidoffset(pose.torsoorigin, mob.yaw, 0, 0, 12);
        if(!found[NPC_PART_LEFT_ARM]) origins[NPC_PART_LEFT_ARM] = humanoidoffset(pose.torsoorigin, mob.yaw, -6, 0, 10);
        if(!found[NPC_PART_RIGHT_ARM]) origins[NPC_PART_RIGHT_ARM] = humanoidoffset(pose.torsoorigin, mob.yaw, 6, 0, 10);
        if(!found[NPC_PART_LEFT_LEG]) origins[NPC_PART_LEFT_LEG] = humanoidoffset(pose.torsoorigin, mob.yaw, -2, 0, 0);
        if(!found[NPC_PART_RIGHT_LEG]) origins[NPC_PART_RIGHT_LEG] = humanoidoffset(pose.torsoorigin, mob.yaw, 2, 0, 0);

        mob.hitboxes.add(characterhitbox(vec(pose.torsoorigin).add(transformedmodelvector(vec(0, 0, 6), mob.yaw, pose.torsopitch,
                                                                                          pose.torsoroll)),
                                             transformedmodelradius(vec(4, 2, 6), mob.yaw, pose.torsopitch, pose.torsoroll), HITBOX_TORSO));
        mob.hitboxes.add(characterhitbox(vec(origins[NPC_PART_HEAD]).add(transformedmodelvector(vec(0, 0, 4), mob.yaw, pose.headpitch,
                                                                                                pose.torsoroll)),
                                             transformedmodelradius(vec(4, 4, 4), mob.yaw, pose.headpitch, pose.torsoroll), HITBOX_HEAD));
        mob.hitboxes.add(characterhitbox(vec(origins[NPC_PART_LEFT_ARM]).add(transformedmodelvector(vec(0, 0, -5), mob.yaw,
                                                                                                    pose.leftarmpitch, pose.torsoroll)),
                                             transformedmodelradius(vec(2, 2, 5), mob.yaw, pose.leftarmpitch, pose.torsoroll), HITBOX_LEFT_ARM));
        mob.hitboxes.add(characterhitbox(vec(origins[NPC_PART_RIGHT_ARM]).add(transformedmodelvector(vec(0, 0, -5), mob.yaw,
                                                                                                     pose.rightarmpitch, pose.torsoroll)),
                                             transformedmodelradius(vec(2, 2, 5), mob.yaw, pose.rightarmpitch, pose.torsoroll), HITBOX_RIGHT_ARM));
        mob.hitboxes.add(characterhitbox(vec(origins[NPC_PART_LEFT_LEG]).add(transformedmodelvector(vec(0, 0, -6), mob.yaw,
                                                                                                    pose.leftlegpitch, pose.torsoroll)),
                                             transformedmodelradius(vec(2, 2, 6), mob.yaw, pose.leftlegpitch, pose.torsoroll), HITBOX_LEFT_LEG));
        mob.hitboxes.add(characterhitbox(vec(origins[NPC_PART_RIGHT_LEG]).add(transformedmodelvector(vec(0, 0, -6), mob.yaw,
                                                                                                     pose.rightlegpitch, pose.torsoroll)),
                                             transformedmodelradius(vec(2, 2, 6), mob.yaw, pose.rightlegpitch, pose.torsoroll), HITBOX_RIGHT_LEG));
        for(int i = mob.hitboxes.length() - 1; i >= 0; --i)
            if(mob.detachedparts & (1U << mob.hitboxes[i].part)) mob.hitboxes.remove(i);
    }

    static characterhitbox *findnpchitbox(npc &mob, int part)
    {
        loopv(mob.hitboxes) if(mob.hitboxes[i].part == part) return &mob.hitboxes[i];
        return NULL;
    }

    static void spawnblood(const vec &position, bool detached)
    {
        regular_particle_splash(PART_BLOOD, detached ? 9 : 3, detached ? 900 : 450, position, 0x60FFFF, detached ? 1.50f : 1.25f, detached ? 150 : 75, 2);
    }

    static void spawnbleedingparticle(const vec &position, const vec *direction = NULL)
    {
        if(direction) regular_particle_spray(PART_BLOOD, 1, 400, position, *direction, 0x60FFFF, 1.0f, 175, 30, 2);
        else regular_particle_splash(PART_BLOOD, 1, 400, position, 0x60FFFF, 1.0f, 90, 2);
    }

    static bool shouldemitblood(int detachedmillis, int &lastblood)
    {
        static const int bleedinterval = 50;
        if(npcbleedduration <= 0 || lastmillis - detachedmillis >= npcbleedduration || lastmillis - lastblood < bleedinterval) return false;
        lastblood = lastmillis;
        return true;
    }

    static matrix3 severedorientation(const severedlimb &limb)
    {
        return modelorientation(limb.renderyaw, limb.renderpitch, limb.renderroll);
    }

    static vec rotateseveredvector(const severedlimb &limb, const vec &value)
    {
        return severedorientation(limb).transposedtransform(value);
    }

    static void updateseveredcollision(severedlimb &limb)
    {
        const vec radius = severedorientation(limb).abstransposedtransform(limb.localradius);
        limb.body.yaw = limb.renderyaw;
        limb.body.pitch = limb.renderpitch;
        limb.body.roll = limb.renderroll;
        limb.body.xradius = radius.x;
        limb.body.yradius = radius.y;
        limb.body.radius = max(radius.x, radius.y);
        limb.body.eyeheight = limb.body.aboveeye = radius.z;
    }

    static vec strikeimpulse(const vec &direction, float damage)
    {
        vec impulse(direction);
        if(impulse.iszero()) impulse = vec(0, 1, 0);
        impulse.safenormalize();
        impulse.z = clamp(impulse.z, -0.45f, 0.65f);
        impulse.safenormalize();
        return impulse.mul(16.0f + min(damage, 12.0f) * 3.0f);
    }

    static void addseveredimpulse(severedlimb &limb, const vec &hitposition, const vec &impulse)
    {
        const vec scaledimpulse = vec(impulse).mul(npclimbvelocitymultiplier);
        limb.sleeping = false;
        limb.sleepmillis = 0;
        limb.settleaxis = 0;
        limb.lastupdate = lastmillis;
        limb.body.vel.add(scaledimpulse);
        const float maximumvelocity = 120.0f * max(npclimbvelocitymultiplier, 1.0f);
        if(limb.body.vel.magnitude() > maximumvelocity) limb.body.vel.rescale(maximumvelocity);

        const matrix3 orient = severedorientation(limb);
        const vec localoffset = orient.transform(vec(hitposition).sub(limb.body.o)), localimpulse = orient.transform(impulse);
        vec angularimpulse;
        angularimpulse.cross(localoffset, localimpulse);
        const vec inertia(max((limb.localradius.y * limb.localradius.y + limb.localradius.z * limb.localradius.z) / 3.0f, 1.0f),
                          max((limb.localradius.x * limb.localradius.x + limb.localradius.z * limb.localradius.z) / 3.0f, 1.0f),
                          max((limb.localradius.x * limb.localradius.x + limb.localradius.y * limb.localradius.y) / 3.0f, 1.0f));
        angularimpulse.div(inertia).mul(npclimbangularvelocitymultiplier / RAD);
        const float maximumangularvelocity = 540.0f * max(npclimbangularvelocitymultiplier, 1.0f);
        limb.angularvelocity.add(vec(angularimpulse.x, -angularimpulse.y, angularimpulse.z)).clamp(-maximumangularvelocity,
                                                                                                  maximumangularvelocity);
        limb.body.resetinterp();
    }

    static bool rayseveredlimb(const severedlimb &limb, const vec &origin, const vec &direction, float &distance)
    {
        const matrix3 orient = severedorientation(limb);
        const vec localorigin = orient.transform(vec(origin).sub(limb.body.o)), localdirection = orient.transform(direction),
                  minimum = vec(limb.localradius).neg(), size = vec(limb.localradius).mul(2.0f);
        int hitface = -1;
        return rayboxintersect(minimum, size, localorigin, localdirection, distance, hitface) && distance >= 0;
    }

    static void addnpcknockback(npc &mob, const vec &impulse, float damage)
    {
        mob.vel.add(impulse);
        const float maximumspeed = max(mob.definition->speed * 2.0f, 72.0f);
        if(mob.vel.magnitude() > maximumspeed) mob.vel.rescale(maximumspeed);
        mob.staggeruntil = max(mob.staggeruntil, lastmillis + 140 + int(min(damage, 10.0f) * 18.0f));
    }

    static float angledifference(float target, float angle)
    {
        return fmodf(target - angle + 540.0f, 360.0f) - 180.0f;
    }

    static void beginseveredsettle(severedlimb &limb)
    {
        static const float orientations[][2] =
        {
            { 0, 90 }, { 0, 270 },
            { 90, 0 }, { 270, 0 },
            { 0, 0 }, { 180, 0 }
        };
        const float verticalradius[] =
        {
            limb.localradius.x, limb.localradius.x,
            limb.localradius.y, limb.localradius.y,
            limb.localradius.z, limb.localradius.z
        };
        const float lowestradius = min(limb.localradius.x, min(limb.localradius.y, limb.localradius.z));
        int best = -1;
        float bestdistance = 1e16f;
        loopi(int(sizeof(orientations) / sizeof(orientations[0])))
        {
            if(verticalradius[i] > lowestradius + 0.01f) continue;
            const float distance = fabsf(angledifference(orientations[i][0], limb.renderpitch)) +
                                   fabsf(angledifference(orientations[i][1], limb.renderroll));
            if(distance >= bestdistance) continue;
            best = i;
            bestdistance = distance;
        }
        limb.settleaxis = 1;
        limb.settlepitch = orientations[max(best, 0)][0];
        limb.settleroll = orientations[max(best, 0)][1];
    }

    static float turnangle(float angle, float target, float amount)
    {
        return fmodf(angle + clamp(angledifference(target, angle), -amount, amount) + 360.0f, 360.0f);
    }

    static bool raiseseveredclear(severedlimb &limb)
    {
        loopi(48)
        {
            if(!collide(&limb.body, vec(0, 0, 0), 0, false)) return true;
            limb.body.o.z += 0.125f;
        }
        return !collide(&limb.body, vec(0, 0, 0), 0, false);
    }

    static bool settleseveredlimb(severedlimb &limb, int elapsed, float grounddistance)
    {
        if(!limb.settleaxis) beginseveredsettle(limb);

        const float groundheight = limb.body.o.z - grounddistance, amount = 720.0f * elapsed / 1000.0f;
        limb.renderpitch = turnangle(limb.renderpitch, limb.settlepitch, amount);
        limb.renderroll = turnangle(limb.renderroll, limb.settleroll, amount);
        updateseveredcollision(limb);
        limb.body.o.z = groundheight + limb.body.eyeheight + 0.05f;
        bool collisionclear = raiseseveredclear(limb);

        const bool settled = fabsf(angledifference(limb.settlepitch, limb.renderpitch)) < 0.1f &&
                             fabsf(angledifference(limb.settleroll, limb.renderroll)) < 0.1f;
        if(settled)
        {
            limb.renderpitch = limb.settlepitch;
            limb.renderroll = limb.settleroll;
            updateseveredcollision(limb);
            limb.body.o.z = groundheight + limb.body.eyeheight + 0.05f;
            collisionclear = raiseseveredclear(limb);
        }
        limb.body.resetinterp();
        return settled && collisionclear;
    }

    static bool severedlimbhasstableface(const severedlimb &limb)
    {
        const matrix3 orient = severedorientation(limb);
        float cornerheights[8], lowest = 1e16f;
        loopi(8)
        {
            const vec corner(i & 1 ? limb.localradius.x : -limb.localradius.x,
                             i & 2 ? limb.localradius.y : -limb.localradius.y,
                             i & 4 ? limb.localradius.z : -limb.localradius.z);
            cornerheights[i] = orient.transposedtransform(corner).z;
            lowest = min(lowest, cornerheights[i]);
        }
        int supportingcorners = 0;
        loopi(8) if(cornerheights[i] - lowest < 0.05f) ++supportingcorners;
        return supportingcorners >= 4;
    }

    static void beginnpccrawl(npc &mob)
    {
        if(mob.crawlstart) return;
        const vec feet = mob.feetpos();
        mob.crawlstart = lastmillis;
        mob.maxheight = mob.eyeheight = CRAWLING_EYEHEIGHT;
        mob.aboveeye = CRAWLING_ABOVEEYE;
        mob.radius = mob.xradius = mob.yradius = 6.0f;
        mob.o = vec(feet).addz(mob.eyeheight);
        mob.newpos = mob.o;
        mob.deltapos = vec(0, 0, 0);
    }

    static void detachnpcpart(npc &mob, int part, const vec &hitposition, const vec &impulse)
    {
        if(part == HITBOX_TORSO || mob.detachedparts & (1U << part)) return;
        characterhitbox *hitbox = findnpchitbox(mob, part);
        if(!hitbox) return;

        string model;
        npcmodelpath(*mob.definition, part, model);
        severedlimb *limb = new severedlimb(model, part);
        npcpose pose;
        calcnpcpose(mob, pose);
        limb->renderyaw = mob.yaw;
        limb->renderroll = pose.torsoroll;
        if(part == HITBOX_HEAD) limb->renderpitch = pose.headpitch;
        else if(part == HITBOX_LEFT_ARM) limb->renderpitch = pose.leftarmpitch;
        else if(part == HITBOX_RIGHT_ARM) limb->renderpitch = pose.rightarmpitch;
        else if(part == HITBOX_LEFT_LEG) limb->renderpitch = pose.leftlegpitch;
        else if(part == HITBOX_RIGHT_LEG) limb->renderpitch = pose.rightlegpitch;
        updateseveredcollision(*limb);
        limb->body.o = hitbox->center;
        loopi(16)
        {
            if(!collide(&limb->body, vec(0, 0, 0), 0, false)) break;
            limb->body.o.z += 1.0f;
        }
        limb->body.vel = vec(mob.vel).mul(0.65f);
        vec separation = vec(limb->body.o).sub(mob.o).safenormalize();
        limb->body.vel.madd(separation, 6.0f);
        addseveredimpulse(*limb, hitposition, vec(impulse).mul(1.25f));
        severedlimbs.add(limb);

        spawnblood(vec(limb->body.o).add(rotateseveredvector(*limb, vec(limb->modelcenter).neg())), true);
        mob.detachedparts |= 1U << part;
        mob.partdetachedmillis[part] = mob.partlastblood[part] = lastmillis;
        if(!npcremaininglegs(mob)) beginnpccrawl(mob);
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
        return item >= 0 && isinventorytool(item) ? getinventorytooldamage(item) : 1.0f;
    }

    static void ragdollnpc(npc &mob, const vec &hitposition, const vec &impulse)
    {
        if(mob.state == CS_DEAD) return;

        npcpose pose;
        calcnpcpose(mob, pose);
        string torso;
        humanoidmodelpath(mob.definition->model, NPC_PART_TORSO, torso);
        vec origins[NUM_NPC_PARTS];
        bool found[NUM_NPC_PARTS] = { false };
        modeltagpositions(torso, &humanoidtags[NPC_PART_HEAD], &origins[NPC_PART_HEAD], &found[NPC_PART_HEAD], NUM_NPC_PARTS - NPC_PART_HEAD,
                          pose.torsoorigin, mob.yaw, pose.torsopitch, pose.torsoroll);

        const vec fallbackoffsets[NUM_NPC_PARTS] =
        {
            vec(0, 0, 0), vec(0, 0, 12), vec(-6, 0, 10), vec(6, 0, 10), vec(-2, 0, 0), vec(2, 0, 0)
        };
        for(int i = NPC_PART_HEAD; i < NUM_NPC_PARTS; ++i) if(!found[i])
            origins[i] = vec(pose.torsoorigin).add(transformedmodelvector(fallbackoffsets[i], mob.yaw, pose.torsopitch, pose.torsoroll));

        vector<vec> positions;
        vector<float> radii;
        vector<int> links;
        vector<int> triangles;
        vector<ragdollrotconstraint> rotconstraints;
        const auto addvertex = [&positions, &radii](const vec &position, float radius)
        {
            radii.add(radius);
            positions.add(position);
            return positions.length() - 1;
        };
        const auto addlink = [&links](int first, int second)
        {
            links.add(first);
            links.add(second);
        };
        const auto addtriangle = [&triangles](int first, int second, int third)
        {
            triangles.add(first);
            triangles.add(second);
            triangles.add(third);
            return triangles.length() / 3 - 1;
        };
        const auto addrotconstraint = [&rotconstraints](int bodytriangle, int parttriangle, float maxangle)
        {
            rotconstraints.add(ragdollrotconstraint(bodytriangle, parttriangle, maxangle));
        };

        loopi(NUM_NPC_PARTS) mob.ragdollstart[i] = mob.ragdollend[i] = -1;
        const int pelvis = addvertex(pose.torsoorigin, 3.0f),
                  neck = addvertex(origins[NPC_PART_HEAD], 2.5f),
                  leftshoulder = addvertex(origins[NPC_PART_LEFT_ARM], 2.0f),
                  rightshoulder = addvertex(origins[NPC_PART_RIGHT_ARM], 2.0f),
                  lefthip = addvertex(origins[NPC_PART_LEFT_LEG], 2.0f),
                  righthip = addvertex(origins[NPC_PART_RIGHT_LEG], 2.0f);
        const int core[] = { pelvis, neck, leftshoulder, rightshoulder, lefthip, righthip };
        loopi(int(sizeof(core) / sizeof(core[0]))) for(int j = i + 1; j < int(sizeof(core) / sizeof(core[0])); ++j)
            addlink(core[i], core[j]);

        mob.ragdollstart[NPC_PART_TORSO] = pelvis;
        mob.ragdollend[NPC_PART_TORSO] = neck;
        mob.ragdollstart[NPC_PART_HEAD] = neck;
        mob.ragdollstart[NPC_PART_LEFT_ARM] = leftshoulder;
        mob.ragdollstart[NPC_PART_RIGHT_ARM] = rightshoulder;
        mob.ragdollstart[NPC_PART_LEFT_LEG] = lefthip;
        mob.ragdollstart[NPC_PART_RIGHT_LEG] = righthip;

        const float defaultendpoints[NUM_NPC_PARTS] = { 0, 8, -10, -10, -12, -12 };
        const float defaultpartradii[NUM_NPC_PARTS] = { 0, 3.5f, 2, 2, 2, 2 };
        for(int part = NPC_PART_HEAD; part < NUM_NPC_PARTS; ++part)
        {
            if(mob.detachedparts & (1U << part)) continue;
            float pitch = pose.headpitch;
            if(part == NPC_PART_LEFT_ARM) pitch = pose.leftarmpitch;
            else if(part == NPC_PART_RIGHT_ARM) pitch = pose.rightarmpitch;
            else if(part == NPC_PART_LEFT_LEG) pitch = pose.leftlegpitch;
            else if(part == NPC_PART_RIGHT_LEG) pitch = pose.rightlegpitch;

            float endpointoffset = defaultendpoints[part], partradius = defaultpartradii[part];
            string partmodel;
            npcmodelpath(*mob.definition, part, partmodel);
            ::model *geometry = loadmodel(partmodel);
            if(geometry)
            {
                vec center, radius;
                geometry->collisionbox(center, radius);
                endpointoffset = center.z + (part == NPC_PART_HEAD ? radius.z : -radius.z);
                partradius = max(max(radius.x, radius.y), 0.5f);
            }
            const vec endpoint = vec(origins[part]).add(transformedmodelvector(vec(0, 0, endpointoffset), mob.yaw, pitch, pose.torsoroll));
            mob.ragdollend[part] = addvertex(endpoint, partradius);
            addlink(mob.ragdollstart[part], mob.ragdollend[part]);
        }

        if(mob.ragdollend[NPC_PART_HEAD] >= 0)
            addrotconstraint(addtriangle(neck, rightshoulder, pelvis),
                             addtriangle(neck, mob.ragdollend[NPC_PART_HEAD], rightshoulder), npcragdollheadmaxangle);
        if(mob.ragdollend[NPC_PART_LEFT_ARM] >= 0)
            addrotconstraint(addtriangle(leftshoulder, neck, pelvis),
                             addtriangle(leftshoulder, mob.ragdollend[NPC_PART_LEFT_ARM], neck), npcragdollarmmaxangle);
        if(mob.ragdollend[NPC_PART_RIGHT_ARM] >= 0)
            addrotconstraint(addtriangle(rightshoulder, pelvis, neck),
                             addtriangle(rightshoulder, neck, mob.ragdollend[NPC_PART_RIGHT_ARM]), npcragdollarmmaxangle);
        if(mob.ragdollend[NPC_PART_LEFT_LEG] >= 0)
            addrotconstraint(addtriangle(lefthip, leftshoulder, pelvis),
                             addtriangle(lefthip, pelvis, mob.ragdollend[NPC_PART_LEFT_LEG]), npcragdolllegmaxangle);
        if(mob.ragdollend[NPC_PART_RIGHT_LEG] >= 0)
            addrotconstraint(addtriangle(righthip, pelvis, rightshoulder),
                             addtriangle(righthip, mob.ragdollend[NPC_PART_RIGHT_LEG], pelvis), npcragdolllegmaxangle);

        mob.state = CS_DEAD;
        mob.collidetype = COLLIDE_NONE;
        mob.stopmoving();
        mob.target = NULL;
        mob.hitboxes.setsize(0);
        loopv(npcs) if(npcs[i]->target == &mob) npcs[i]->target = NULL;
        if(createcustomragdoll(&mob, positions.getbuf(), radii.getbuf(), positions.length(), links.getbuf(), links.length() / 2,
                               triangles.getbuf(), triangles.length() / 3, rotconstraints.getbuf(), rotconstraints.length()))
        {
            int nearest = 0;
            float nearestdistance = positions[0].squaredist(hitposition);
            loopi(positions.length())
            {
                const float distance = positions[i].squaredist(hitposition);
                if(distance >= nearestdistance) continue;
                nearest = i;
                nearestdistance = distance;
            }
            pushragdollvertex(&mob, nearest, vec(impulse).mul(1.35f));
            scaleragdollvelocity(&mob, npcragdollvelocitymultiplier);
        }
        cleardynentcache();
    }

    bool attacknpc()
    {
        if(multiplayer(false) || editmode || (!m_creative && !m_survival) || !player1 || player1->state != CS_ALIVE || !camera1) return false;

        static const float attackreach = 5.0f * GAMEUNITSPERMETER;
        float worlddistance = raycube(camera1->o, camdir, attackreach, RAY_CLIPMAT | RAY_SKIPFIRST);
        if(worlddistance < 0 || worlddistance > attackreach) worlddistance = attackreach;

        npc *hitmob = NULL;
        severedlimb *hitlimb = NULL;
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
                hitlimb = NULL;
                hitpart = hitbox.part;
                hitdistance = distance;
            }
        }
        loopv(severedlimbs)
        {
            float distance = hitdistance;
            if(!rayseveredlimb(*severedlimbs[i], camera1->o, camdir, distance) || distance > hitdistance) continue;
            hitmob = NULL;
            hitlimb = severedlimbs[i];
            hitdistance = distance;
        }
        if(!hitmob && !hitlimb) return false;

        const vec hitposition = vec(camera1->o).madd(camdir, hitdistance);
        if(hitlimb)
        {
            const float damage = heldattackdamage();
            spawnblood(hitposition, false);
            addseveredimpulse(*hitlimb, hitposition, strikeimpulse(camdir, damage).mul(1.1f));
            return true;
        }

        const float damage = heldattackdamage() * npcdamagemultiplier(hitpart);
        const vec impulse = strikeimpulse(camdir, damage);
        spawnblood(hitposition, false);
        hitmob->totalhealth = max(hitmob->totalhealth - damage, 0.0f);
        if(hitpart != HITBOX_TORSO)
        {
            hitmob->parthealth[hitpart] = max(hitmob->parthealth[hitpart] - damage, 0.0f);
            if(hitmob->parthealth[hitpart] <= 0) detachnpcpart(*hitmob, hitpart, hitposition, impulse);
        }
        addnpcknockback(*hitmob, impulse, damage);
        if(hitmob->totalhealth <= 0) ragdollnpc(*hitmob, hitposition, impulse);
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
        const int legs = npcremaininglegs(mob);
        mob.lastmovementmillis = lastmillis;
        mob.maxspeed = mob.definition->speed * (legs == 1 ? 0.58f : !legs ? 0.34f : 1.0f);
        if(lastmillis < mob.staggeruntil)
        {
            mob.stopmoving();
            moveplayer(&mob, 10, true);
            return;
        }
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
        if(legs == 1)
        {
            if(mob.physstate >= PHYS_SLOPE && lastmillis - mob.lastlimbhop >= 480)
            {
                mob.vel.z = max(mob.vel.z, 62.0f);
                mob.physstate = PHYS_FALL;
                mob.lastlimbhop = lastmillis;
            }
            else if(mob.physstate >= PHYS_SLOPE) mob.move = 0;
        }
        else if(legs == 2 && mob.blocked && mob.physstate >= PHYS_SLOPE && lastmillis - mob.lastjump >= 400)
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
        const int legs = npcremaininglegs(mob);
        const char *activity = mob.state == CS_DEAD ? mob.frozen ? " / Ragdoll / Frozen" : " / Ragdoll" : mob.frozen ? " / Frozen" :
                               !legs ? " / Crawling" : legs == 1 ? " / Hopping" :
                               mob.behavior == NPC_WANDERING && mob.wanderpaused ? " / Paused" : "";
        defformatstring(text, "%s #%d\n%s / %s%s\nHP %.2f/%d  target: %s", mob.definition->name, mob.instanceid, attitudename(mob.attitude),
                        behaviorname(mob.behavior), activity, mob.totalhealth, mob.definition->health, targetname);
        particle_textcopy(mob.abovehead(), text, PART_TEXT, 150, 0xFFE28A, 1.5f, 0);
    }

    static void updatenpcbleeding(npc &mob)
    {
        for(int part = NPC_PART_HEAD; part < NUM_NPC_PARTS; ++part)
        {
            if(!(mob.detachedparts & (1U << part)) ||
               !shouldemitblood(mob.partdetachedmillis[part], mob.partlastblood[part])) continue;
            vec position, direction;
            if(!npcjointposition(mob, part, position)) continue;
            spawnbleedingparticle(position, npcjointdirection(mob, part, direction) ? &direction : NULL);
        }
    }

    static void updateseveredlimbs()
    {
        for(int i = 0; i < severedlimbs.length();)
        {
            severedlimb &limb = *severedlimbs[i];
            if(shouldemitblood(limb.detachedmillis, limb.lastblood))
                spawnbleedingparticle(vec(limb.body.o).add(rotateseveredvector(limb, vec(limb.modelcenter).neg())));
            if(limb.sleeping)
            {
                if(lastmillis - limb.sleepmillis >= npcdebrisduration)
                {
                    delete severedlimbs[i];
                    severedlimbs.remove(i);
                    continue;
                }
                ++i;
                continue;
            }

            const int elapsed = clamp(lastmillis - limb.lastupdate, 0, 100);
            limb.lastupdate = lastmillis;
            if(!limb.settleaxis)
            {
                const float oldyaw = limb.renderyaw, oldpitch = limb.renderpitch, oldroll = limb.renderroll;
                limb.renderyaw = fmodf(limb.renderyaw + limb.angularvelocity.z * elapsed / 1000.0f + 360.0f, 360.0f);
                limb.renderpitch = fmodf(limb.renderpitch + limb.angularvelocity.x * elapsed / 1000.0f + 360.0f, 360.0f);
                limb.renderroll = fmodf(limb.renderroll + limb.angularvelocity.y * elapsed / 1000.0f + 360.0f, 360.0f);
                updateseveredcollision(limb);
                if(collide(&limb.body, vec(0, 0, 0), 0, false))
                {
                    limb.renderyaw = oldyaw;
                    limb.renderpitch = oldpitch;
                    limb.renderroll = oldroll;
                    limb.angularvelocity.mul(-0.15f);
                    updateseveredcollision(limb);
                }
            }
            limb.body.resetinterp();
            bounce(&limb.body, npclimbbounciness, 3.0f, 1.0f);

            const float groundreach = limb.body.eyeheight + 0.75f,
                        grounddistance = raycube(limb.body.o, vec(0, 0, -1), groundreach, RAY_CLIPMAT | RAY_SKIPFIRST);
            const bool grounded = grounddistance >= 0 && grounddistance < groundreach;
            bool settled = false;
            if(grounded)
            {
                const float friction = powf(0.025f, elapsed / 1000.0f);
                limb.body.vel.x *= friction;
                limb.body.vel.y *= friction;
                limb.body.vel.z *= friction;
                if(fabsf(limb.body.vel.z) < 1.0f) limb.body.vel.z = 0;
                limb.angularvelocity.mul(powf(0.001f, elapsed / 1000.0f));
                settled = settleseveredlimb(limb, elapsed, grounddistance);
            }
            else
            {
                limb.settleaxis = 0;
                limb.angularvelocity.mul(powf(0.55f, elapsed / 1000.0f));
            }
            if(limb.body.vel.squaredlen() <= 6.25f && grounded && settled && severedlimbhasstableface(limb))
            {
                if(!limb.sleepmillis) limb.sleepmillis = lastmillis;
                else if(lastmillis - limb.sleepmillis >= 350)
                {
                    limb.sleeping = true;
                    limb.sleepmillis = lastmillis;
                    limb.body.vel = limb.body.falling = vec(0, 0, 0);
                    limb.body.newpos = limb.body.o;
                    limb.body.deltapos = vec(0, 0, 0);
                    limb.angularvelocity = vec(0, 0, 0);
                }
            }
            else limb.sleepmillis = 0;
            ++i;
        }
    }

    void updatenpcs()
    {
        if(multiplayer(false))
        {
            if(npcs.length() || severedlimbs.length()) resetnpcs();
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
            if(mob.state == CS_DEAD)
            {
                if(mob.frozen) freezeragdoll(&mob);
                else moveragdoll(&mob);
                mob.hitboxes.setsize(0);
                updatenpcbleeding(mob);
                shownpcdebugtext(mob);
                continue;
            }
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
            updatenpcbleeding(mob);
            shownpcdebugtext(mob);
        }
        updateseveredlimbs();
    }

    static bool ragdollmodelangles(const npc &mob, int part, float &yaw, float &pitch, float &roll)
    {
        vec start, end, leftshoulder, rightshoulder;
        if(!getragdollvertex(&mob, mob.ragdollstart[part], start) || !getragdollvertex(&mob, mob.ragdollend[part], end) ||
           !getragdollvertex(&mob, mob.ragdollstart[NPC_PART_LEFT_ARM], leftshoulder) ||
           !getragdollvertex(&mob, mob.ragdollstart[NPC_PART_RIGHT_ARM], rightshoulder)) return false;

        vec z = vec(end).sub(start);
        if(part >= NPC_PART_LEFT_ARM) z.neg();
        if(z.squaredlen() < 1e-6f) return false;
        z.normalize();
        vec x = vec(rightshoulder).sub(leftshoulder);
        x.msub(z, x.dot(z));
        if(x.squaredlen() < 1e-6f)
        {
            x.cross(fabsf(z.z) < 0.9f ? vec(0, 0, 1) : vec(0, 1, 0), z);
        }
        x.normalize();
        vec y;
        y.cross(z, x).normalize();

        pitch = asinf(clamp(y.z, -1.0f, 1.0f)) / RAD;
        const float cp = cosf(pitch * RAD);
        if(fabsf(cp) > 1e-4f)
        {
            yaw = atan2f(-y.x, y.y) / RAD;
            roll = atan2f(x.z, z.z) / RAD;
        }
        else
        {
            yaw = y.z >= 0 ? atan2f(z.x, -z.y) / RAD : atan2f(-z.x, z.y) / RAD;
            roll = 0;
        }
        yaw = fmodf(yaw + 360.0f, 360.0f);
        return true;
    }

    static void rendernpcragdoll(npc &mob)
    {
        const int flags = MDL_CULL_VFC | MDL_CULL_DIST | MDL_CULL_OCCLUDED;
        loopi(NUM_NPC_PARTS)
        {
            if(i != NPC_PART_TORSO && mob.detachedparts & (1U << i)) continue;
            vec origin;
            float yaw, pitch, roll;
            if(!getragdollvertex(&mob, mob.ragdollstart[i], origin) || !ragdollmodelangles(mob, i, yaw, pitch, roll)) continue;
            string model;
            npcmodelpath(*mob.definition, i, model);
            rendermodel(model, ANIM_MAPMODEL | ANIM_LOOP, origin, yaw, pitch, roll, flags);
        }
    }

    static void rendernpc(npc &mob)
    {
        string models[NUM_NPC_PARTS];
        loopi(NUM_NPC_PARTS) npcmodelpath(*mob.definition, i, models[i]);

        const int flags = MDL_CULL_VFC | MDL_CULL_DIST | MDL_CULL_OCCLUDED;
        const int legs = npcremaininglegs(mob);
        const float speed = horizontalmeterspersecond(&mob),
                    gamespeed = speed * GAMEUNITSPERMETER,
                    gaitcycletravel = legs == 1 ? 10.0f : !legs ? 7.0f : STANDING_HEIGHT * 0.75f;
        if(mob.renderlastmillis < 0 || lastmillis < mob.renderlastmillis) mob.renderlastmillis = lastmillis;
        const int elapsed = min(lastmillis - mob.renderlastmillis, 100);
        mob.renderlastmillis = lastmillis;
        if(gamespeed > 0.05f)
            mob.renderstride = fmodf(mob.renderstride + 2.0f * PI * gamespeed * elapsed / (1000.0f * gaitcycletravel), 2.0f * PI);

        npcpose pose;
        calcnpcpose(mob, pose);
        vec origins[NUM_NPC_PARTS];
        bool found[NUM_NPC_PARTS] = { false };
        rendermodel(models[NPC_PART_TORSO], ANIM_MAPMODEL | ANIM_LOOP, pose.torsoorigin, mob.yaw, pose.torsopitch, pose.torsoroll, flags, &mob);
        modeltagpositions(models[NPC_PART_TORSO], &humanoidtags[NPC_PART_HEAD], &origins[NPC_PART_HEAD], &found[NPC_PART_HEAD],
                          NUM_NPC_PARTS - NPC_PART_HEAD, pose.torsoorigin, mob.yaw, pose.torsopitch, pose.torsoroll);
        if(found[NPC_PART_HEAD] && !(mob.detachedparts & (1U << HITBOX_HEAD)))
            rendermodel(models[NPC_PART_HEAD], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_HEAD], mob.yaw, pose.headpitch, pose.torsoroll, flags,
                        &mob);
        if(found[NPC_PART_LEFT_ARM] && !(mob.detachedparts & (1U << HITBOX_LEFT_ARM)))
            rendermodel(models[NPC_PART_LEFT_ARM], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_LEFT_ARM], mob.yaw, pose.leftarmpitch,
                        pose.torsoroll, flags, &mob);
        if(found[NPC_PART_RIGHT_ARM] && !(mob.detachedparts & (1U << HITBOX_RIGHT_ARM)))
            rendermodel(models[NPC_PART_RIGHT_ARM], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_RIGHT_ARM], mob.yaw, pose.rightarmpitch,
                        pose.torsoroll, flags, &mob);
        if(found[NPC_PART_LEFT_LEG] && !(mob.detachedparts & (1U << HITBOX_LEFT_LEG)))
            rendermodel(models[NPC_PART_LEFT_LEG], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_LEFT_LEG], mob.yaw, pose.leftlegpitch,
                        pose.torsoroll, flags, &mob);
        if(found[NPC_PART_RIGHT_LEG] && !(mob.detachedparts & (1U << HITBOX_RIGHT_LEG)))
            rendermodel(models[NPC_PART_RIGHT_LEG], ANIM_MAPMODEL | ANIM_LOOP, origins[NPC_PART_RIGHT_LEG], mob.yaw, pose.rightlegpitch,
                        pose.torsoroll, flags, &mob);
    }

    static void renderseveredlimb(const severedlimb &limb)
    {
        const int sinktime = min(npcdebrissinktime, npcdebrisduration), sinkstart = npcdebrisduration - sinktime;
        const float sinkamount = limb.sleeping && sinktime > 0
                               ? clamp((lastmillis - limb.sleepmillis - sinkstart) / float(sinktime), 0.0f, 1.0f)
                               : 0.0f,
                    smoothsink = sinkamount * sinkamount * (3.0f - 2.0f * sinkamount);
        const vec offset = rotateseveredvector(limb, vec(limb.modelcenter).neg());
        const vec origin = vec(limb.body.o).add(offset).addz(-smoothsink * (limb.localradius.z * 2.0f + 1.0f));
        const int flags = MDL_CULL_VFC | MDL_CULL_DIST | MDL_CULL_OCCLUDED;
        rendermodel(limb.model, ANIM_MAPMODEL | ANIM_LOOP, origin, limb.renderyaw, limb.renderpitch, limb.renderroll, flags);
    }

    void rendernpcs()
    {
        loopv(npcs)
        {
            if(npcs[i]->state == CS_DEAD && npcs[i]->ragdoll) rendernpcragdoll(*npcs[i]);
            else rendernpc(*npcs[i]);
        }
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
        loopv(severedlimbs) renderorientedboundingbox(severedlimbs[i]->body.o, severedlimbs[i]->localradius, severedlimbs[i]->renderyaw,
                                                      severedlimbs[i]->renderpitch, severedlimbs[i]->renderroll);
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
