#include "game.h"

namespace game
{
    static const int DOOR_ANIMATION_MILLIS = 400;
    static vector<doorinstance *> localdoors;

    static doorinstance *findlocaldoor(const ivec &target)
    {
        loopv(localdoors) if(localdoors[i]->target == target) return localdoors[i];
        return NULL;
    }

    const vector<doorinstance *> &getlocaldoors() { return localdoors; }

    bool getlocaldoor(const ivec &target, doorinstance &door)
    {
        doorinstance *found = findlocaldoor(target);
        if(!found) return false;
        door = *found;
        return true;
    }

    void resetdoors()
    {
        localdoors.deletecontents();
    }

    void removelocaldoor(const ivec &target)
    {
        loopv(localdoors) if(localdoors[i]->target == target)
        {
            delete localdoors.remove(i);
#ifndef STANDALONE
            updateworlddooranimations();
#endif
            return;
        }
    }

    void addlocaldoor(const doorinstance &source)
    {
        doorinstance *door = findlocaldoor(source.target);
        if(!door)
        {
            door = new doorinstance(source);
            localdoors.add(door);
        }
        else *door = source;
#ifndef STANDALONE
        door->fromangle = door->open ? 90.0f * door->swing : 0.0f;
        door->started = 0;
#endif
#ifndef STANDALONE
        updateworlddooranimations();
#endif
    }

    static float doorangle(const doorinstance &door)
    {
        const float destination = door.open ? 90.0f * door.swing : 0.0f;
#ifndef STANDALONE
        if(door.started > 0)
        {
            const float amount = clamp((lastmillis - door.started) / float(DOOR_ANIMATION_MILLIS), 0.0f, 1.0f),
                        smooth = amount * amount * (3.0f - 2.0f * amount);
            return door.fromangle + (destination - door.fromangle) * smooth;
        }
#endif
        return destination;
    }

    void receivedoorstate(const doorinstance &source, bool animate)
    {
        doorinstance *door = findlocaldoor(source.target);
        if(!door)
        {
            addlocaldoor(source);
            return;
        }
#ifndef STANDALONE
        const float previous = doorangle(*door);
#endif
        *door = source;
#ifndef STANDALONE
        door->fromangle = animate ? previous : (door->open ? 90.0f * door->swing : 0.0f);
        door->started = animate ? lastmillis : 0;
#endif
#ifndef STANDALONE
        updateworlddooranimations();
#endif
    }

    bool dooroccupiescell(const ivec &cell, const ivec *ignore)
    {
        loopv(localdoors)
        {
            const doorinstance &door = *localdoors[i];
            if(ignore && door.target == *ignore) continue;
            if(cell == door.target || cell == ivec(door.target).add(ivec(0, 0, 16))) return true;
        }
        return false;
    }

    bool getdoortransform(const ivec &target, vec &position, int &yaw)
    {
        doorinstance *door = findlocaldoor(target);
        if(!door) return false;
        const float baseyaw = door->yaw * RAD, angle = doorangle(*door), radians = angle * RAD,
                    hingesign = door->hingright ? 1.0f : -1.0f;
        const vec normal(-sinf(baseyaw), cosf(baseyaw), 0), right(cosf(baseyaw), sinf(baseyaw), 0);
        // worldscattertransform supplies a mounted, chunk-relative base position. The door key is absolute only for persistence lookup.
        position.madd(normal, (door->depth - 1) * (16.0f / 3.0f));
        const vec hinge = vec(position).madd(right, 8.0f * hingesign);
        vec offset = vec(right).mul(-8.0f * hingesign);
        const float x = offset.x * cosf(radians) - offset.y * sinf(radians),
                    y = offset.x * sinf(radians) + offset.y * cosf(radians);
        position = vec(hinge).add(vec(x, y, 0));
        yaw = int(door->yaw + angle + (angle >= 0 ? 0.5f : -0.5f));
        return true;
    }

    bool interactlocaldoor(const ivec &target, const vec &playerposition)
    {
        doorinstance *door = findlocaldoor(target);
        if(!door) return false;
        if(!door->open)
        {
            const float radians = door->yaw * RAD;
            const vec normal(-sinf(radians), cosf(radians), 0), center = vec(target).add(vec(8, 8, 16));
            door->swing = vec(playerposition).sub(center).dot(normal) >= 0 ? -1 : 1;
        }
        doorinstance changed = *door;
        changed.open = !door->open;
        receivedoorstate(changed);
        return true;
    }
}
