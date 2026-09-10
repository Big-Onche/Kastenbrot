// Build/run instructions: doc/NPC animations.md. Uses the real CubeScript interpreter.
#include "game.h"
#include "engine.h"
#include "../game/world.h"
#include "npcdef.h"
#include <cassert>

// Standalone command/stream support without starting a game server.
void conoutfv(int, const char *fmt, va_list args)
{
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

void fatal(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    conoutfv(CON_ERROR, fmt, args);
    va_end(args);
    abort();
}

namespace game
{
    ICOMMAND(npcreset, "", (),
    {
        npcdefinitions.deletecontents();
        npcanimations.deletecontents();
    });
}

static void expectclose(float actual, float expected)
{
    assert(fabsf(actual - expected) < 0.001f);
}

static void reject(const char *script)
{
    const int mobs = game::numnpcdefinitions(), clips = game::npcanimations.length();
    execute(script);
    assert(game::numnpcdefinitions() == mobs && game::npcanimations.length() == clips);
    assert(!game::findnpcdefinition("invalid") && !game::findnpcanimation("invalid"));
}

int main()
{
    assert(execfile("config/game/npcs.cfg", true));
    assert(game::numnpcdefinitions() == 6 && game::npcanimations.length() == 13);
    loopi(6)
    {
        const npcdefinition &mob = *game::getnpcdefinition(i);
        loopj(NUM_NPC_ANIMS) assert(mob.animations[j] && mob.animations[j]->rig == mob.modeltype);
    }
    const npcdefinition &cow = *game::findnpcdefinition("cow"), &pig = *game::findnpcdefinition("pig"),
                        &zombie = *game::findnpcdefinition("zombie"), &skeleton = *game::findnpcdefinition("mossy_skeleton");
    assert(cow.animations[NPC_ANIM_WALK] == pig.animations[NPC_ANIM_WALK]);
    assert(zombie.animations[NPC_ANIM_ATTACK] != skeleton.animations[NPC_ANIM_ATTACK]);
    float pose[NUM_NPC_ANIM_CHANNELS] = { 0 };
    applynpcanimation(cow.animations[NPC_ANIM_WALK], 0.25f, 1, 1, 1, pose);
    expectclose(pose[NPC_ANIM_LEFTARM], 33);
    expectclose(pose[NPC_ANIM_RIGHTARM], -33);
    expectclose(pose[NPC_ANIM_LEFTLEG], -33);
    expectclose(pose[NPC_ANIM_RIGHTLEG], 33);
    expectclose(pose[NPC_ANIM_HEIGHT], 0);
    memset(pose, 0, sizeof(pose));
    applynpcanimation(zombie.animations[NPC_ANIM_IDLE], 0, 1, 1, 1, pose);
    applynpcanimation(zombie.animations[NPC_ANIM_WALK], 0.25f, 1, 1, 1, pose);
    expectclose(pose[NPC_ANIM_LEFTARM], 74);
    expectclose(pose[NPC_ANIM_RIGHTARM], 86);
    expectclose(pose[NPC_ANIM_LEFTLEG], 32);
    applynpcanimation(zombie.animations[NPC_ANIM_ATTACK], 0.35f, 1, 1, 1, pose);
    expectclose(pose[NPC_ANIM_LEFTARM], 132);
    expectclose(pose[NPC_ANIM_RIGHTARM], 132);
    applynpcanimation(zombie.animations[NPC_ANIM_CRAWL], 0.25f, 1, 1, 1, pose);
    expectclose(pose[NPC_ANIM_LEFTARM], 140);
    expectclose(pose[NPC_ANIM_RIGHTARM], 90);
    expectclose(pose[NPC_ANIM_TORSOPITCH], -90);
    expectclose(pose[NPC_ANIM_HEAD], -30);
    expectclose(pose[NPC_ANIM_HEIGHT], 2.25f);
    memset(pose, 0, sizeof(pose));
    applynpcanimation(skeleton.animations[NPC_ANIM_ATTACK], 0.5f, 1, 1, 1, pose);
    expectclose(pose[NPC_ANIM_LEFTARM], 75);
    expectclose(pose[NPC_ANIM_RIGHTARM], 75);
    memset(pose, 0, sizeof(pose));
    applynpcanimation(skeleton.animations[NPC_ANIM_WALK], 0.25f, 0.5f, 1, 0.5f, pose);
    applynpcanimation(skeleton.animations[NPC_ANIM_LIMP], 0.25f, 0.5f, -1, 1, pose);
    expectclose(pose[NPC_ANIM_LEFTLEG], 12);
    expectclose(pose[NPC_ANIM_TORSOROLL], -8);
    expectclose(pose[NPC_ANIM_HEIGHT], 0.45f);
    // Partial crawl blending and stationary arm targets.
    memset(pose, 0, sizeof(pose));
    pose[NPC_ANIM_HEIGHT] = 11.25f;
    applynpcanimation(skeleton.animations[NPC_ANIM_CRAWL], 0.25f, 0, 1, 0.5f, pose);
    expectclose(pose[NPC_ANIM_LEFTARM], 55);
    expectclose(pose[NPC_ANIM_TORSOPITCH], -45);
    expectclose(pose[NPC_ANIM_HEIGHT], 6.75f);
    // Rear, hold both hind legs steady, strike together, and settle back to the underlying pose.
    memset(pose, 0, sizeof(pose));
    applynpcanimation(cow.animations[NPC_ANIM_WALK], 0.25f, 1, 1, 1, pose);
    applynpcanimation(cow.animations[NPC_ANIM_ATTACK], 0.45f, 1, 1, 1, pose);
    expectclose(pose[NPC_ANIM_TORSOPITCH], 60);
    expectclose(pose[NPC_ANIM_REARANCHOR], 1);
    expectclose(pose[NPC_ANIM_LEFTLEG], 0);
    expectclose(pose[NPC_ANIM_RIGHTLEG], 0);
    expectclose(pose[NPC_ANIM_LEFTARM], 105);
    expectclose(pose[NPC_ANIM_RIGHTARM], 105);
    memset(pose, 0, sizeof(pose));
    applynpcanimation(cow.animations[NPC_ANIM_ATTACK], 0.125f, 0, 1, 1, pose);
    expectclose(pose[NPC_ANIM_TORSOPITCH], 30);
    memset(pose, 0, sizeof(pose));
    applynpcanimation(cow.animations[NPC_ANIM_ATTACK], 1, 0, 1, 1, pose);
    loopi(NUM_NPC_ANIM_CHANNELS) expectclose(pose[i], 0);
    // Clips can be created and reused entirely from config, independent of NPC IDs.
    execute("npc_test_angle = 24; npcanimdef custom [rig humanoid; duration 800; head [wave sin; amplitude $npc_test_angle]]");
    execute("npcdef custom_mob [model [path test]; animations [idle custom]]");
    const npcdefinition *custom = game::findnpcdefinition("custom_mob");
    assert(custom && custom->animations[NPC_ANIM_IDLE] == game::findnpcanimation("custom"));
    assert(!custom->animations[NPC_ANIM_WALK]);
    memset(pose, 0, sizeof(pose));
    applynpcanimation(custom->animations[NPC_ANIM_IDLE], 0.25f, 0, 1, 1, pose);
    expectclose(pose[NPC_ANIM_HEAD], 24);
    reject("npcanimdef HUMANOID_IDLE [rig humanoid]");
    reject("npcanimdef invalid [rig unknown]");
    reject("npcanimdef invalid [rig humanoid; duration 0]");
    reject("npcanimdef invalid [rig humanoid; travel 0]");
    reject("npcanimdef invalid [rig humanoid; head [wave unknown]]");
    reject("npcanimdef invalid [rig humanoid; head [key 0 0]]");
    reject("npcanimdef invalid [rig humanoid; head [key 0.5 0; key 0.25 1]]");
    reject("npcanimdef invalid [rig humanoid; head [key 0 0; key 2 1]]");
    reject("npcanimdef invalid [rig humanoid; head [mode unknown]]");
    reject("npcanimdef invalid [rig humanoid; head [peak 1]]");
    reject("npcanimdef invalid [rig humanoid; head [min 2; max 1]]");
    reject("npcanimdef invalid [rig humanoid; head [amplitude 1e30]]");
    reject("npcanimdef invalid [rig humanoid; head [health 10]]");
    reject("npcanimdef invalid [rig humanoid; npcanimdef nested [rig humanoid]]");
    reject("npcdef invalid [model [path test]; animations [walk missing]]");
    reject("npcdef invalid [model [path test]; animations [walk quadruped_walk]]");
    reject("npcdef invalid [model [path test]; animations [walk humanoid_walk]; animations [idle humanoid_idle]]");
    execute("npcanim_track_base 5; npcanim_head [base 5]; npcdef_animation_walk humanoid_walk");
    assert(execfile("config/game/npcs.cfg", true));
    assert(game::numnpcdefinitions() == 6 && game::npcanimations.length() == 13);
    assert(!game::findnpcanimation("custom"));
    execute("npcreset");
    puts("NPC animation tests passed");
}
