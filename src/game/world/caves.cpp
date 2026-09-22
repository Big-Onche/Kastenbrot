#include "game.h"
#include "engine.h"
#include "worlddef.h"
#ifdef STANDALONE
#include "worldcube.h"
#endif

worldcavesegment::worldcavesegment(const vec &start, const vec &end, float startradius, float endradius, float verticalscale, uint roughness,
                                   bool entrance)
    : start(start), end(end), startradius(startradius), endradius(endradius), verticalscale(verticalscale), roughness(roughness), entrance(entrance)
{
}
worldcavechamber::worldcavechamber(const vec &center, float radiusx, float radiusy, float radiusz, float angle, uint roughness)
    : center(center), radiusx(radiusx), radiusy(radiusy), radiusz(radiusz), anglecos(cosf(angle)), anglesin(sinf(angle)), roughness(roughness)
{
}
enum
{
    // A halo wider than the longest worm plus its largest chamber makes clipping seamless at chunk edges.
    WORLD_CAVE_REGION_SIZE = 144,
    WORLD_CAVE_REGION_HALO = 400
};

struct worldcaverandom
{
    uint state;

    worldcaverandom(uint seed) : state(seed ? seed : 0xA341316CU) {}

    uint next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float unit()
    {
        return float(next() & 0x00FFFFFFU) / float(0x01000000U);
    }
    int range(int low, int high)
    {
        return low + int(next() % uint(max(high - low + 1, 1)));
    }
};

struct worldcaveanchor
{
    vec position;
    int worm;

    worldcaveanchor(const vec &position, int worm) : position(position), worm(worm) {}
};

static float worldcaveradius(worldcaverandom &random)
{
    // Target durations are independent of category, so these are also the long-run passage proportions.
    const float category = random.unit();
    if(category < 0.20f) return 2.0f + random.unit();
    if(category < 0.75f) return 3.0f + random.unit() * 3.0f;
    if(category < 0.95f) return 6.0f + random.unit() * 4.0f;
    return 10.0f + random.unit() * 10.0f;
}

static void addworldcaveworm(const worldgencontext &ctx, worldcaverandom &random, vector<worldcavesegment> &segments,
                             vector<worldcaveanchor> &anchors, const vec &origin, float yaw, float pitch, int steps, int worm)
{
    const int mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth),
              bottom = WORLD_MIN_HEIGHT + clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)) + 3;

    vec position(origin);

    const float initialdeepness = worldsmoothstep(180.0f, 228.0f, -origin.z);
    float radius = min(3.0f + random.unit() * 3.0f, 4.25f - initialdeepness * 1.75f), targetradius = radius,
          turnrate = (random.unit() < 0.5f ? -1.0f : 1.0f) * (0.14f + random.unit() * 0.25f);
    int radiussteps = 1, turnsteps = random.range(1, 3);

    anchors.add(worldcaveanchor(position, worm));

    loopi(steps)
    {
        const float deepness = worldsmoothstep(180.0f, 228.0f, -position.z);
        if(--turnsteps <= 0)
        {
            turnrate = (random.unit() < 0.5f ? -1.0f : 1.0f) * (0.14f + random.unit() * 0.29f);
            turnsteps = random.range(1, 3);
        }
        yaw += turnrate * (1.0f + deepness * 0.90f) + (random.unit() - 0.5f) * (0.24f + deepness * 0.22f);
        if(random.unit() < 0.17f + deepness * 0.15f)
        {
            yaw += (random.unit() < 0.5f ? -1.0f : 1.0f) * (0.48f + random.unit() * 0.82f);
            turnrate = -turnrate * (0.55f + random.unit() * 0.35f);
            turnsteps = random.range(1, 3);
        }
        pitch = pitch * (0.68f - deepness * 0.12f) + (random.unit() - 0.5f) * (0.32f + deepness * 0.24f);
        if(random.unit() < 0.10f + deepness * 0.08f)
            pitch += (random.unit() < 0.72f ? -1.0f : 1.0f) * (0.38f + random.unit() * (0.44f + deepness * 0.20f));
        pitch = clamp(pitch, -0.85f, 0.70f);

        if(--radiussteps <= 0)
        {
            targetradius = worldcaveradius(random);
            radiussteps = random.range(2, 6);
        }
        const float unrestrictedradius = radius + clamp(targetradius - radius, -3.0f, 3.0f), deepcap = 4.25f - deepness * 1.75f,
                    nextradius = min(unrestrictedradius, deepcap), steplength = (3.5f + random.unit() * 3.0f) * (1.0f - deepness * 0.34f),
                    horizontal = cosf(pitch) * steplength;
        vec next(position.x + cosf(yaw) * horizontal, position.y + sinf(yaw) * horizontal, position.z + sinf(pitch) * steplength);
        const int surface = ctx.generator.baseheight(int(floorf(next.x)), int(floorf(next.y))),
                  ceiling = surface - max(mindepth, int(ceilf(nextradius))) - 1;
        next.z = clamp(next.z, float(bottom), float(max(ceiling, bottom)));

        const float verticalscale = 0.52f - deepness * 0.09f + random.unit() * (0.83f - deepness * 0.19f);
        segments.add(worldcavesegment(position, next, radius, nextradius, verticalscale, random.next()));
        position = next;
        radius = nextradius;
        if(i % 3 == 2 || i == steps - 1) anchors.add(worldcaveanchor(position, worm));
    }
}

static void addworldcaveconnection(worldcaverandom &random, vector<worldcavesegment> &segments, const vec &start, const vec &end, float startradius,
                                   float endradius, bool entrance = false)
{
    const float deepness = worldsmoothstep(180.0f, 228.0f, -min(start.z, end.z));
    const float dx = end.x - start.x, dy = end.y - start.y, dz = end.z - start.z, distance = sqrtf(dx * dx + dy * dy + dz * dz),
                horizontal = max(sqrtf(dx * dx + dy * dy), 0.001f), direction = random.unit() < 0.5f ? -1.0f : 1.0f,
                curve = direction * min(distance * (0.15f + random.unit() * 0.13f + deepness * 0.05f), 28.0f + deepness * 8.0f),
                wiggle = -direction * min(distance * (0.05f + random.unit() * 0.08f + deepness * 0.04f), 12.0f + deepness * 6.0f),
                verticalcurve = (random.unit() - 0.62f) * min(distance * 0.14f, 15.0f);
    const float smallwiggle = direction * min(distance * (0.025f + random.unit() * 0.045f + deepness * 0.035f), 7.0f + deepness * 5.0f);
    const int steps = max(int(ceilf(distance / (5.5f - deepness * 1.8f))), 3);
    vec previous(start);
    float previousradius = startradius;
    for(int i = 1; i <= steps; ++i)
    {
        const float amount = i / float(steps),
                    lateral = sinf(amount * M_PI) * curve + sinf(amount * 2.0f * M_PI) * wiggle + sinf(amount * 3.0f * M_PI) * smallwiggle,
                    vertical = sinf(amount * M_PI) * verticalcurve, radius = startradius + (endradius - startradius) * amount;
        const vec next(start.x + dx * amount - dy / horizontal * lateral, start.y + dy * amount + dx / horizontal * lateral,
                       start.z + dz * amount + vertical);
        segments.add(worldcavesegment(previous, next, previousradius, radius, 0.58f + random.unit() * 0.70f, random.next(), entrance));
        previous = next;
        previousradius = radius;
    }
}

static void addworldcavedeepdescent(const worldgencontext &ctx, worldcaverandom &random, vector<worldcavesegment> &segments,
                                    vector<worldcaveanchor> &anchors, const vec &start, int worm)
{
    const int bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS));
    const float lavaceiling = WORLD_MIN_HEIGHT + bottomlayers + 0.5f, verticaldistance = max(start.z - lavaceiling, 1.0f);
    const int levels = clamp(int(ceilf(verticaldistance / 44.0f)), 3, 9);
    vec previous(start);
    float previousradius = min(3.2f + random.unit() * 1.4f, 4.2f), angle = random.unit() * 2.0f * M_PI;

    anchors.add(worldcaveanchor(previous, worm));

    for(int i = 1; i <= levels; ++i)
    {
        const float amount = i / float(levels), deepness = worldsmoothstep(0.45f, 1.0f, amount),
                    horizontal = i == levels ? random.unit() * 10.0f : 13.0f + random.unit() * (25.0f - deepness * 8.0f),
                    radius = 3.2f + (1.65f - 3.2f) * deepness + random.unit() * (0.65f - deepness * 0.25f);

        angle += (random.unit() < 0.5f ? -1.0f : 1.0f) * (0.55f + random.unit() * 1.35f);

        const vec next(start.x + cosf(angle) * horizontal, start.y + sinf(angle) * horizontal, start.z + (lavaceiling - start.z) * amount);

        addworldcaveconnection(random, segments, previous, next, previousradius, radius);
        previous = next;
        previousradius = radius;
        anchors.add(worldcaveanchor(previous, worm));
    }
}

static void addworldcavechamber(worldcaverandom &random, vector<worldcavechamber> &chambers, const vec &attachment)
{
    const float category = random.unit(),
                generatedradius = category < 0.78f   ? 5.0f + random.unit() * 5.0f
                                  : category < 0.97f ? 10.0f + random.unit() * 10.0f
                                                     : 20.0f + random.unit() * 15.0f,
                deepness = worldsmoothstep(180.0f, 228.0f, -attachment.z), baseradius = min(generatedradius, 6.0f - deepness * 2.5f);
    const int lobes = random.range(3, category >= 0.97f ? 7 : 5);
    const float mainangle = random.unit() * 2.0f * M_PI;

    loopi(lobes)
    {
        const float offsetangle = mainangle + (random.unit() - 0.5f) * M_PI, offsetdistance = i ? baseradius * random.unit() * 0.32f : 0.0f,
                    verticaloffset = i ? (random.unit() - 0.5f) * baseradius * 0.30f : 0.0f, radiusx = baseradius * (0.66f + random.unit() * 0.34f),
                    radiusy = baseradius * (0.55f + random.unit() * 0.43f), radiusz = baseradius * (0.35f + random.unit() * 0.34f),
                    angle = mainangle + (random.unit() - 0.5f) * 0.8f;
        const vec center(attachment.x + cosf(offsetangle) * offsetdistance, attachment.y + sinf(offsetangle) * offsetdistance,
                         attachment.z + verticaloffset);

        chambers.add(worldcavechamber(center, radiusx, radiusy, radiusz, angle, random.next()));
    }
}

static bool generateworldcavesystem(const worldgencontext &ctx, long long regionx, long long regiony, vector<worldcavesegment> &segments,
                                    vector<worldcavechamber> &chambers)
{
    // Neighboring regions share a density class, producing compact quiet areas and dense labyrinth clusters.
    const long long clusterx = worldfloordiv(regionx, 2), clustery = worldfloordiv(regiony, 2);
    const float cluster = worldtreeunit(hashworldfeature(uint(ctx.seed), clusterx, clustery, 0, 0x6E624EB7U));
    const int density = cluster < 0.14f ? 0 : cluster < 0.68f ? 1 : 2;
    const float systemchance = density == 0 ? 0.56f : density == 1 ? 0.84f : 0.99f;
    const uint systemhash = hashworldfeature(uint(ctx.seed), regionx, regiony, density, 0xB5297A4DU);

    if(worldtreeunit(systemhash) >= systemchance) return false;

    worldcaverandom random(systemhash ^ 0x68E31DA4U);
    const float originx = float(regionx * WORLD_CAVE_REGION_SIZE + 24) + random.unit() * (WORLD_CAVE_REGION_SIZE - 48),
                originy = float(regiony * WORLD_CAVE_REGION_SIZE + 24) + random.unit() * (WORLD_CAVE_REGION_SIZE - 48);
    const int surface = ctx.generator.baseheight(int(floorf(originx)), int(floorf(originy))),
              bottom = WORLD_MIN_HEIGHT + clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)) + 8,
              mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth),
              depth = random.range(28, density == 2 ? 105 : 82) + (random.unit() < 0.18f ? random.range(35, 90) : 0),
              originz = clamp(surface - depth, bottom, surface - mindepth - 8);

    if(originz <= bottom && surface - bottom < mindepth + 12) return false;

    vector<worldcaveanchor> anchors;
    const vec origin(originx, originy, float(originz));
    const int primaryworms = random.range(density == 0 ? 3 : 4, density == 2 ? 9 : density == 1 ? 7 : 5);
    int worm = 0;

    loopi(primaryworms)
    {
        const float yaw = random.unit() * 2.0f * M_PI, pitch = (random.unit() - 0.58f) * (random.unit() < 0.16f ? 0.85f : 0.25f);
        const int steps = random.range(density == 0 ? 18 : density == 1 ? 21 : 24, density == 0 ? 26 : density == 1 ? 30 : 34);
        addworldcaveworm(ctx, random, segments, anchors, origin, yaw, pitch, steps, worm++);
        if(random.unit() < 0.24f) addworldcavechamber(random, chambers, anchors.last().position);
    }
    if(random.unit() < 0.52f) addworldcavechamber(random, chambers, origin);

    const int junctions = random.range(density == 0 ? 1 : 3, density == 2 ? 8 : density == 1 ? 5 : 3);
    loopi(junctions)
    {
        if(!anchors.length()) break;
        vec attachment(origin);
        loopj(8)
        {
            const vec &candidate = anchors[random.next() % uint(anchors.length())].position;
            const float dx = candidate.x - origin.x, dy = candidate.y - origin.y, dz = candidate.z - origin.z;
            if(dx * dx + dy * dy + dz * dz <= 145.0f * 145.0f)
            {
                attachment = candidate;
                break;
            }
        }
        const float baseyaw = random.unit() * 2.0f * M_PI;
        int forks = random.unit() < 0.30f ? 2 : 1;
        if(density == 2 && random.unit() < 0.10f) forks = 3;
        loopj(forks)
        {
            const float yaw = baseyaw + j * 2.0f * M_PI / forks + (random.unit() - 0.5f) * 0.45f, pitch = (random.unit() - 0.62f) * 0.62f;
            addworldcaveworm(ctx, random, segments, anchors, attachment, yaw, pitch, random.range(9, 18), worm++);
            if(random.unit() < 0.18f) addworldcavechamber(random, chambers, anchors.last().position);
        }
    }

    int connections = density == 0 ? 2 : density == 1 ? 4 : 6;
    // These cross-worm links turn the origin's branching tree into a graph while leaving unselected ends intact.
    for(int attempt = 0; attempt < 64 && connections > 0 && anchors.length() >= 2; ++attempt)
    {
        const worldcaveanchor &from = anchors[random.next() % uint(anchors.length())], &to = anchors[random.next() % uint(anchors.length())];
        if(from.worm == to.worm) continue;
        const float dx = to.position.x - from.position.x, dy = to.position.y - from.position.y, dz = to.position.z - from.position.z,
                    distance = sqrtf(dx * dx + dy * dy + dz * dz);
        if(distance < 28.0f || distance > 155.0f || fabsf(dz) > 72.0f) continue;

        const float radius = random.unit() < 0.18f ? 7.0f + random.unit() * 6.0f : 3.5f + random.unit() * 3.5f;
        addworldcaveconnection(random, segments, from.position, to.position, radius * 0.85f, radius * 0.85f);
        --connections;
    }

    const game::worldtectonicsample terrain = ctx.generator.tectonics(int(floorf(originx)), int(floorf(originy)));
    const float hillweight = worldsmoothstep(0.12f, 0.68f, terrain.terrainroughness), entrancechance = 0.48f + hillweight * 0.49f;
    int entrances = random.unit() < entrancechance ? 1 : 0;
    if(entrances && density == 2 && random.unit() < hillweight * 0.42f) ++entrances;
    loopi(entrances)
    {
        const worldcaveanchor &attachment = anchors[random.next() % uint(anchors.length())];
        const float angle = random.unit() * 2.0f * M_PI, distance = 10.0f + random.unit() * 34.0f, entrancex = originx + cosf(angle) * distance,
                    entrancey = originy + sinf(angle) * distance;
        const int entranceheight = ctx.generator.baseheight(int(floorf(entrancex)), int(floorf(entrancey)));
        if(entranceheight <= ctx.settings.sealevel + 2) continue;

        const vec entrance(entrancex, entrancey, entranceheight - 0.25f);
        const float radius = 3.0f + random.unit() * 2.5f;
        addworldcaveconnection(random, segments, attachment.position, entrance, radius * 1.25f, max(radius * 0.72f, 2.2f), true);
    }

    const int descents = 1 + (density == 2 && random.unit() < 0.42f ? 1 : 0);
    loopi(descents)
    {
        vec descentstart(origin);
        if(i == 0)
        {
            loopj(anchors.length())
                if(anchors[j].position.z < descentstart.z) descentstart = anchors[j].position;
        }
        else
            loopj(12)
            {
                const vec &candidate = anchors[random.next() % uint(anchors.length())].position;
                if(candidate.z < descentstart.z) descentstart = candidate;
            }

        vector<worldcaveanchor> deepanchors;
        addworldcavedeepdescent(ctx, random, segments, deepanchors, descentstart, worm++);
        const int descentanchors = deepanchors.length();
        const vec descentend = deepanchors.last().position;
        const int deepbranches = random.range(density == 0 ? 2 : 3, density == 2 ? 5 : density == 1 ? 4 : 3);
        loopj(deepbranches)
        {
            vec attachment(descentend);
            loopk(8)
            {
                const vec &candidate = deepanchors[random.next() % uint(descentanchors)].position;
                if(candidate.z <= -190.0f)
                {
                    attachment = candidate;
                    break;
                }
            }
            const float yaw = random.unit() * 2.0f * M_PI, pitch = (random.unit() - 0.58f) * 0.48f;
            addworldcaveworm(ctx, random, segments, deepanchors, attachment, yaw, pitch, random.range(10, 16), worm++);
            if(random.unit() < 0.12f) addworldcavechamber(random, chambers, deepanchors.last().position);
        }
    }
    return true;
}

static float worldcavesegmentradius(const worldgencontext &ctx, const worldcavesegment &segment, float x, float y, float z, float radius)
{
    // Cheese fields may perturb an existing wall, but can never establish the cave's primary route.
    const float offsetx = float(segment.roughness & 0xFFU) * 7.0f, offsety = float((segment.roughness >> 8) & 0xFFU) * 7.0f,
                offsetz = float((segment.roughness >> 16) & 0xFFU) * 3.0f,
                coarse = ctx.generator.caves.GetNoise(x + offsetx, y - offsety, z + offsetz),
                fine = ctx.generator.caves.GetNoise(x * 1.85f - offsety, y * 1.85f + offsetx, z * 1.55f - offsetz),
                roughness = coarse * 0.62f + fine * 0.38f, secondary = ctx.generator.largecaves.GetNoise(x - offsety, y + offsetx, z - offsetz),
                widening = max(secondary - max(ctx.settings.largecavethreshold, 0.72f), 0.0f) * 7.0f;
    float carvedradius = max(radius + roughness * min(radius * 0.28f, 2.20f) + widening, 1.35f);
    if(z < -196.0f) carvedradius = min(carvedradius, max(4.0f - (-z - 196.0f) * 0.075f, 2.35f));
    return carvedradius;
}

static bool worldcavesegmentcontains(const worldgencontext &ctx, const worldcavesegment &segment, float x, float y, float z)
{
    const float scale = max(segment.verticalscale, 0.1f), ax = segment.start.x, ay = segment.start.y, az = segment.start.z / scale,
                bx = segment.end.x, by = segment.end.y, bz = segment.end.z / scale, px = x, py = y, pz = z / scale, dx = bx - ax, dy = by - ay,
                dz = bz - az, length2 = dx * dx + dy * dy + dz * dz;
    const float amount = length2 > 0.0001f ? clamp(((px - ax) * dx + (py - ay) * dy + (pz - az) * dz) / length2, 0.0f, 1.0f) : 0.0f,
                centerx = ax + dx * amount, centery = ay + dy * amount, centerz = az + dz * amount,
                baseradius = segment.startradius + (segment.endradius - segment.startradius) * amount, distancex = px - centerx,
                distancey = py - centery, distancez = pz - centerz, distance2 = distancex * distancex + distancey * distancey + distancez * distancez,
                maximumradius = baseradius + 4.2f;
    if(distance2 > maximumradius * maximumradius) return false;
    const float radius = worldcavesegmentradius(ctx, segment, x, y, z, baseradius);
    return distance2 <= radius * radius;
}

static bool worldcavechambercontains(const worldgencontext &ctx, const worldcavechamber &chamber, float x, float y, float z)
{
    const float dx = x - chamber.center.x, dy = y - chamber.center.y, rotatedx = dx * chamber.anglecos + dy * chamber.anglesin,
                rotatedy = -dx * chamber.anglesin + dy * chamber.anglecos, dz = z - chamber.center.z,
                ellipsoiddistance = rotatedx * rotatedx / (chamber.radiusx * chamber.radiusx) +
                                    rotatedy * rotatedy / (chamber.radiusy * chamber.radiusy) + dz * dz / (chamber.radiusz * chamber.radiusz);
    if(ellipsoiddistance > 1.24f) return false;
    const float offsetx = float(chamber.roughness & 0xFFU) * 5.0f, offsety = float((chamber.roughness >> 8) & 0xFFU) * 5.0f,
                roughness = ctx.generator.caves.GetNoise(x + offsetx, y - offsety, z + 4700.5f),
                secondary = ctx.generator.largecaves.GetNoise(x - offsety, y + offsetx, z - 4700.5f),
                boundary = 1.0f + roughness * 0.12f + max(secondary - max(ctx.settings.largecavethreshold, 0.74f), 0.0f) * 0.45f;
    return ellipsoiddistance <= boundary;
}

static void carveworldcavesegment(const worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky, const worldcavesegment &segment)
{
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS, chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS;
    const float radius = max(segment.startradius, segment.endradius) + 4.25f, verticalradius = radius * max(segment.verticalscale, 1.0f);
    const int xmin = max(int(floorf(min(segment.start.x, segment.end.x) - radius - chunkstartx)), 0),
              xmax = min(int(ceilf(max(segment.start.x, segment.end.x) + radius - chunkstartx)), WORLD_CHUNK_BLOCKS - 1),
              ymin = max(int(floorf(min(segment.start.y, segment.end.y) - radius - chunkstarty)), 0),
              ymax = min(int(ceilf(max(segment.start.y, segment.end.y) + radius - chunkstarty)), WORLD_CHUNK_BLOCKS - 1),
              zmin = max(int(floorf(min(segment.start.z, segment.end.z) - verticalradius)), int(WORLD_MIN_HEIGHT)),
              zmax = min(int(ceilf(max(segment.start.z, segment.end.z) + verticalradius)), int(WORLD_MAX_HEIGHT) - 1),
              mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth);
    if(xmin > xmax || ymin > ymax || zmin > zmax) return;

    for(int y = ymin; y <= ymax; ++y)
        for(int x = xmin; x <= xmax; ++x)
        {
            const int surface = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE, ceiling = surface - 1, localzmax = min(zmax, ceiling);
            for(int z = zmin; z <= localzmax; ++z)
            {
                if(!segment.entrance && surface - z < mindepth) continue;
                if(worldcavesegmentcontains(ctx, segment, float(chunkstartx + x) + 0.5f, float(chunkstarty + y) + 0.5f, z + 0.5f))
                {
                    uchar &carve = carvemap[worldcarveindex(x, y, z - WORLD_MIN_HEIGHT)];
                    carve = WORLD_CARVE_AIR | (carve & WORLD_CARVE_ENTRANCE) | (segment.entrance ? WORLD_CARVE_ENTRANCE : 0);
                }
            }
        }
}

static void carveworldcavechamber(const worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky, const worldcavechamber &chamber)
{
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS, chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS;
    const float horizontalradius = max(chamber.radiusx, chamber.radiusy) * 1.12f, verticalradius = chamber.radiusz * 1.12f;
    const int xmin = max(int(floorf(chamber.center.x - horizontalradius - chunkstartx)), 0),
              xmax = min(int(ceilf(chamber.center.x + horizontalradius - chunkstartx)), WORLD_CHUNK_BLOCKS - 1),
              ymin = max(int(floorf(chamber.center.y - horizontalradius - chunkstarty)), 0),
              ymax = min(int(ceilf(chamber.center.y + horizontalradius - chunkstarty)), WORLD_CHUNK_BLOCKS - 1),
              zmin = max(int(floorf(chamber.center.z - verticalradius)), int(WORLD_MIN_HEIGHT)),
              zmax = min(int(ceilf(chamber.center.z + verticalradius)), int(WORLD_MAX_HEIGHT) - 1),
              mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth);
    if(xmin > xmax || ymin > ymax || zmin > zmax) return;

    for(int y = ymin; y <= ymax; ++y)
        for(int x = xmin; x <= xmax; ++x)
        {
            const int surface = ctx.heightmap[y * WORLD_CHUNK_BLOCKS + x] / WORLD_BLOCK_SIZE, localzmax = min(zmax, surface - mindepth);
            for(int z = zmin; z <= localzmax; ++z)
                if(worldcavechambercontains(ctx, chamber, float(chunkstartx + x) + 0.5f, float(chunkstarty + y) + 0.5f, z + 0.5f))
                {
                    uchar &carve = carvemap[worldcarveindex(x, y, z - WORLD_MIN_HEIGHT)];
                    carve = WORLD_CARVE_AIR | (carve & WORLD_CARVE_ENTRANCE);
                }
        }
}

static bool generateworldcavenetworks(worldgencontext &ctx, uchar *carvemap, int chunkx, int chunky)
{
    ctx.cavesegments.setsize(0);
    ctx.cavechambers.setsize(0);
    const long long chunkstartx = (long long)chunkx * WORLD_CHUNK_BLOCKS, chunkstarty = (long long)chunky * WORLD_CHUNK_BLOCKS,
                    minregionx = worldfloordiv(chunkstartx - WORLD_CAVE_REGION_HALO, WORLD_CAVE_REGION_SIZE),
                    maxregionx = worldfloordiv(chunkstartx + WORLD_CHUNK_BLOCKS - 1 + WORLD_CAVE_REGION_HALO, WORLD_CAVE_REGION_SIZE),
                    minregiony = worldfloordiv(chunkstarty - WORLD_CAVE_REGION_HALO, WORLD_CAVE_REGION_SIZE),
                    maxregiony = worldfloordiv(chunkstarty + WORLD_CHUNK_BLOCKS - 1 + WORLD_CAVE_REGION_HALO, WORLD_CAVE_REGION_SIZE);
    vector<worldcavesegment> systemsegments;
    vector<worldcavechamber> systemchambers;
    const float cacheminx = float(chunkstartx) - 6.0f, cachemaxx = float(chunkstartx + WORLD_CHUNK_BLOCKS) + 6.0f,
                cacheminy = float(chunkstarty) - 6.0f, cachemaxy = float(chunkstarty + WORLD_CHUNK_BLOCKS) + 6.0f;
    for(long long regiony = minregiony; regiony <= maxregiony; ++regiony)
        for(long long regionx = minregionx; regionx <= maxregionx; ++regionx)
        {
            if(ctx.iscanceled()) return false;
            systemsegments.setsize(0);
            systemchambers.setsize(0);
            if(!generateworldcavesystem(ctx, regionx, regiony, systemsegments, systemchambers)) continue;
            loopv(systemsegments)
            {
                const worldcavesegment &segment = systemsegments[i];
                const float radius = max(segment.startradius, segment.endradius) + 4.25f;
                if(max(segment.start.x, segment.end.x) + radius < cacheminx || min(segment.start.x, segment.end.x) - radius > cachemaxx ||
                   max(segment.start.y, segment.end.y) + radius < cacheminy || min(segment.start.y, segment.end.y) - radius > cachemaxy)
                    continue;
                ctx.cavesegments.add(segment);
            }
            loopv(systemchambers)
            {
                const worldcavechamber &chamber = systemchambers[i];
                const float radius = max(chamber.radiusx, chamber.radiusy) * 1.12f;
                if(chamber.center.x + radius < cacheminx || chamber.center.x - radius > cachemaxx || chamber.center.y + radius < cacheminy ||
                   chamber.center.y - radius > cachemaxy)
                    continue;
                ctx.cavechambers.add(chamber);
            }
        }

    loopv(ctx.cavesegments)
    {
        if((i & 31) == 0 && ctx.iscanceled()) return false;
        carveworldcavesegment(ctx, carvemap, chunkx, chunky, ctx.cavesegments[i]);
    }
    loopv(ctx.cavechambers)
    {
        if((i & 15) == 0 && ctx.iscanceled()) return false;
        carveworldcavechamber(ctx, carvemap, chunkx, chunky, ctx.cavechambers[i]);
    }
    return true;
}

bool placeworldcaves(worldgencontext &ctx, cube *root, int chunkx, int chunky)
{
    const int mapblocks = WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS * WORLD_HEIGHT_BLOCKS;
    vector<uchar> carvemap;
    uchar *carve;
    {
        ZoneScopedN("Chunks/Allocate cave map");
        carve = carvemap.pad(mapblocks);
        memset(carve, WORLD_CARVE_NONE, mapblocks * sizeof(uchar));
    }

    {
        ZoneScopedN("Chunks/Generate cave networks");
        if(!generateworldcavenetworks(ctx, carve, chunkx, chunky)) return false;
    }
    {
        ZoneScopedN("Chunks/Generate lava lakes");
        if(!generateworldlavalakes(ctx, carve, chunkx, chunky)) return false;
    }

    {
        ZoneScopedN("Chunks/Apply cave map");
        const int bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS));
        loop(z, bottomlayers)
            loop(y, WORLD_CHUNK_BLOCKS)
                loop(x, WORLD_CHUNK_BLOCKS)
                {
                    uchar &carveflags = carve[worldcarveindex(x, y, z)];
                    carveflags = WORLD_CARVE_LAVA | (carveflags & WORLD_CARVE_ENTRANCE);
                }

        loop(z, WORLD_HEIGHT_BLOCKS)
        {
            if(ctx.iscanceled()) return false;
            loop(y, WORLD_CHUNK_BLOCKS)
                loop(x, WORLD_CHUNK_BLOCKS)
                {
                    const uchar carveflags = carve[worldcarveindex(x, y, z)], type = carveflags & WORLD_CARVE_TYPE;
                    if(type == WORLD_CARVE_NONE) continue;
                    cube &c = lookupworldgenblock(ctx, root, ivec(x * WORLD_BLOCK_SIZE, y * WORLD_BLOCK_SIZE, z * WORLD_BLOCK_SIZE));
                    bool affected = false;
                    if(type == WORLD_CARVE_LAVA)
                    {
                        setworldcubematerial(c, MAT_LAVA);
                        affected = true;
                    }
                    else if(!isempty(c) && c.material == MAT_AIR)
                    {
                        setworldcubematerial(c, MAT_AIR);
                        affected = true;
                    }
                    if(!affected) continue;
                    markworldgencarvedsection(ctx, x, y, z, (carveflags & WORLD_CARVE_ENTRANCE) != 0);
                }
        }
    }
    return true;
}

bool worldcaveairat(const worldgencontext &ctx, int worldx, int worldy, int elevation)
{
    const int bottomlayers = clamp(ctx.settings.bottomlavalayers, 0, int(WORLD_HEIGHT_BLOCKS)), minheight = WORLD_MIN_HEIGHT + bottomlayers,
              mindepth = min(ctx.settings.cavemindepth, ctx.settings.cavefulldepth), surfaceheight = ctx.generator.baseheight(worldx, worldy),
              caveceiling = min(surfaceheight - 1, WORLD_MAX_HEIGHT - 1);
    if(elevation < minheight || elevation > caveceiling) return false;
    const float x = worldx + 0.5f, y = worldy + 0.5f, z = elevation + 0.5f;
    loopv(ctx.cavesegments)
    {
        const worldcavesegment &segment = ctx.cavesegments[i];
        if(!segment.entrance && surfaceheight - elevation < mindepth) continue;
        if(worldcavesegmentcontains(ctx, segment, x, y, z)) return true;
    }
    if(surfaceheight - elevation < mindepth) return false;
    loopv(ctx.cavechambers)
        if(worldcavechambercontains(ctx, ctx.cavechambers[i], x, y, z)) return true;
    return false;
}
