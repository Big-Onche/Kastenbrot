#ifndef GAME_NPCANIMATION_H
#define GAME_NPCANIMATION_H

enum { NPC_ANIM_IDLE, NPC_ANIM_WALK, NPC_ANIM_ATTACK, NPC_ANIM_LIMP, NPC_ANIM_CRAWL, NPC_ANIM_RUN, NUM_NPC_ANIMS };
enum
{
    NPC_ANIM_HEIGHT, NPC_ANIM_TORSOPITCH, NPC_ANIM_TORSOROLL, NPC_ANIM_HEAD,
    NPC_ANIM_LEFTARM, NPC_ANIM_RIGHTARM, NPC_ANIM_LEFTLEG, NPC_ANIM_RIGHTLEG, NPC_ANIM_REARANCHOR, NUM_NPC_ANIM_CHANNELS
};
enum { NPC_ANIM_CONSTANT, NPC_ANIM_SIN, NPC_ANIM_ABSSIN, NPC_ANIM_ABSCOS, NPC_ANIM_PULSE, NPC_ANIM_KEYS };
enum { NPC_ANIM_ADD, NPC_ANIM_SET, NPC_ANIM_MULTIPLY };

struct npcanimationkey
{
    float time, value;
    npcanimationkey(float time, float value) : time(time), value(value)
    {
    }
};

struct npcanimationtrack
{
    int channel, wave, mode;
    float base, amplitude, frequency, phase, peak, minimum, maximum;
    bool movement, amplitudemovement, mirror, envelope;
    vector<npcanimationkey> keys;

    npcanimationtrack(int channel = 0) : channel(channel), wave(NPC_ANIM_CONSTANT), mode(NPC_ANIM_ADD), base(0), amplitude(0),
        frequency(1), phase(0), peak(0.5f), minimum(-1e6f), maximum(1e6f), movement(false), amplitudemovement(false), mirror(false), envelope(false)
    {
    }
};

struct npcanimationdefinition
{
    string id;
    int rig, duration, transition;
    float travel, fullspeed;
    bool travelheight;
    vector<npcanimationtrack> tracks;

    npcanimationdefinition(const char *id = "") : rig(-1), duration(1000), transition(350), travel(0.75f), fullspeed(2.25f), travelheight(true)
    {
        copystring(this->id, id);
    }
};

static inline void applynpcanimation(const npcanimationdefinition *animation, float cycle, float movement, float mirror, float weight,
                                     float *channels)
{
    if(!animation || weight <= 0) return;
    loopv(animation->tracks)
    {
        const npcanimationtrack &track = animation->tracks[i];
        const float phase = cycle * track.frequency + track.phase;
        float wave = 0;
        switch(track.wave)
        {
            case NPC_ANIM_SIN: wave = sinf(phase * 2 * PI); break;
            case NPC_ANIM_ABSSIN: wave = fabsf(sinf(phase * 2 * PI)); break;
            case NPC_ANIM_ABSCOS: wave = fabsf(cosf(phase * 2 * PI)); break;
            case NPC_ANIM_KEYS:
            {
                if(track.keys.empty()) break;
                wave = track.keys[track.keys.length() - 1].value;
                loopvj(track.keys) if(phase <= track.keys[j].time)
                {
                    if(!j) wave = track.keys[0].value;
                    else
                    {
                        const npcanimationkey &a = track.keys[j - 1], &b = track.keys[j];
                        float blend = (phase - a.time) / (b.time - a.time);
                        blend = blend * blend * (3 - 2 * blend);
                        wave = a.value + (b.value - a.value) * blend;
                    }
                    break;
                }
                break;
            }
            case NPC_ANIM_PULSE:
            {
                const float progress = clamp(phase, 0.0f, 1.0f);
                wave = progress < track.peak ? progress / track.peak : (1 - progress) / (1 - track.peak);
                wave = wave * wave * (3 - 2 * wave);
                break;
            }
        }
        float value = clamp(track.base + track.amplitude * wave * (track.amplitudemovement ? movement : 1), track.minimum, track.maximum);
        if(track.movement) value *= movement;
        if(track.mirror) value *= mirror;
        const float blend = weight * (track.envelope ? wave : 1);
        float &target = channels[track.channel];
        if(track.mode == NPC_ANIM_SET) target += (value - target) * blend;
        else if(track.mode == NPC_ANIM_MULTIPLY) target *= 1 + (value - 1) * blend;
        else target += value * blend;
    }
}

#endif
