#ifndef ENGINE_SOUNDOCCLUSION_H
#define ENGINE_SOUNDOCCLUSION_H

// Four nearby rays reject isolated corner hits while retaining full wall/roof blockage.
template<class Trace>
static float soundpointocclusion(const vec &from, const vec &to, Trace trace)
{
    vec dir = vec(to).sub(from);
    float length = dir.magnitude();
    if(length <= 1) return 0;
    dir.div(length);
    vec side(-dir.y, dir.x, 0);
    if(side.iszero()) side = vec(1, 0, 0);
    side.safenormalize();
    const float tolerance = min(max(length * 0.02f, 1.0f), 4.0f);
    vec offsets[4] = { vec(0, 0, 0), vec(side).mul(2), vec(side).mul(-2), vec(0, 0, 2) };
    int blocked = 0;
    loopi(4)
    {
        float hit = trace(vec(from).add(offsets[i]), dir, length);
        if(hit >= 0 && hit < length - tolerance) ++blocked;
    }
    return blocked <= 1 ? 0 : (blocked - 1) / 3.0f;
}

// Block-grid traversal also sees streamed chunk geometry that is not mounted in the runtime octree.
template<class Solid>
static float soundworldhit(const vec &start, const vec &direction, float length, float grid, Solid solid)
{
    float distance = 0;
    while(distance < length)
    {
        const vec point = vec(start).madd(direction, distance);
        if(solid(ivec(int(floorf(point.x)), int(floorf(point.y)), int(floorf(point.z))))) return distance;
        float step = length - distance;
        loopi(3) if(fabsf(direction[i]) > 1e-6f)
        {
            float boundary = (direction[i] > 0 ? floorf(point[i] / grid) + 1 : ceilf(point[i] / grid) - 1) * grid;
            step = min(step, (boundary - point[i]) / direction[i]);
        }
        distance += max(step, 0.0f) + 0.01f;
    }
    return length;
}

#endif
