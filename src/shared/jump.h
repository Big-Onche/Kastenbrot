#ifndef SHARED_JUMP_H
#define SHARED_JUMP_H

// Unobstructed dry-ground apex under the same velocity damping and gravity as moveplayer.
static inline float jumpapex(float impulse, float gravity, int millis)
{
    const float seconds = millis / 1000.0f, airdamping = powf(29.0f / 30.0f, millis / 20.0f);
    float velocity = impulse * powf(5.0f / 6.0f, millis / 20.0f), falling = 0, height = velocity * seconds;
    for(;;)
    {
        velocity *= airdamping;
        falling += gravity * seconds;
        if(velocity <= falling) return height;
        height += (velocity - falling) * seconds;
    }
}

static inline float scaledjumpimpulse(float impulse, float gravity, float height, int millis)
{
    if(height == 1) return impulse;
    const float target = jumpapex(impulse, gravity, millis) * height;
    float minimum = 0, maximum = impulse * max(height, 1.0f);
    loopi(24)
    {
        const float middle = (minimum + maximum) * 0.5f;
        if(jumpapex(middle, gravity, millis) < target) minimum = middle;
        else maximum = middle;
    }
    return (minimum + maximum) * 0.5f;
}

#endif
