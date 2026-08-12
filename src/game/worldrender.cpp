// worldrender.cpp: Kastenbrot streamed scatter presentation

#ifdef WORLDIO_MODULE_IMPLEMENTATION

struct worldgrasscandidate
{
    ivec key;
    vec position;
    int model, yaw, pitch, roll;
    bool matched;

    worldgrasscandidate(const ivec &key, const vec &position, int model,
                        int yaw, int pitch, int roll)
        : key(key), position(position), model(model), yaw(yaw),
          pitch(pitch), roll(roll), matched(false) {}
};

VARP(staticentsmaxdistance, 0, 64, 1024);
VARP(staticentsmaxamount, 0, 8192, MAXENTS);
VARP(staticlightmaxdistance, 0, 64, 1024);

struct worldscatterchunkcandidate
{
    int chunkindex;
    float distancesquared;

    worldscatterchunkcandidate() : chunkindex(-1), distancesquared(0) {}
    worldscatterchunkcandidate(int chunkindex, float distancesquared)
        : chunkindex(chunkindex), distancesquared(distancesquared) {}

    bool operator<(const worldscatterchunkcandidate &other) const
    {
        return distancesquared < other.distancesquared;
    }
};

struct worldgrassentity
{
    ivec key;
    int id;

    worldgrassentity(const ivec &key, int id) : key(key), id(id) {}
};

static vector<worldgrassentity> worldgrassentities;

static void clearworldscattererentities()
{
    loopv(worldgrassentities) destroyworldmapmodelentity(worldgrassentities[i].id);
    worldgrassentities.setsize(0);
}

static ivec worldscatterkey(const worldchunk &chunk, const worldscatterinstance &scatter)
{
    return ivec(chunk.x * WORLD_CHUNK_BLOCKS + scatter.x / WORLD_BLOCK_SIZE,
                chunk.y * WORLD_CHUNK_BLOCKS + scatter.y / WORLD_BLOCK_SIZE,
                scatter.z);
}

static void worldscattertransform(const worldchunk &chunk, const worldscatterinstance &scatter, float maxoffset, vec &position, int &yaw, int &pitch,
                                  int &roll)
{
    pitch = roll = 0;
    const ivec origin = worldchunkorigin(chunk);
    if(isworldplaceable(scatter.type))
    {
        yaw = 0;
        position = vec(origin.x + scatter.x + WORLD_BLOCK_SIZE * 0.5f,
                       origin.y + scatter.y + WORLD_BLOCK_SIZE * 0.5f,
                       float(scatter.z));
        if(scatter.orient == O_TOP) return;

        const ivec normal = worldorientnormal(scatter.orient);
        position.z += WORLD_BLOCK_SIZE * 0.25f;
        const int axis = dimension(scatter.orient);
        position[axis] -= normal[axis] * WORLD_BLOCK_SIZE * 0.5f;
        position[axis] += normal[axis] * 1.25f;
        switch(scatter.orient)
        {
            case O_BACK:  yaw = 0; break;
            case O_RIGHT: yaw = 90; break;
            case O_FRONT: yaw = 180; break;
            case O_LEFT:  yaw = 270; break;
        }
        pitch = 23;
        return;
    }

    game::cacheworldscattertransform(chunk.x, chunk.y, maxoffset, scatter);
    yaw = scatter.renderyaw;
    position = vec(origin.x + scatter.x + WORLD_BLOCK_SIZE * 0.5f + scatter.renderoffsetx,
                   origin.y + scatter.y + WORLD_BLOCK_SIZE * 0.5f + scatter.renderoffsety, float(scatter.z));
}

static bool worldtorchflameposition(const worldchunk &chunk, const worldscatterinstance &scatter, float maxoffset, vec &flame)
{
    vec position;
    int yaw, pitch, roll;
    worldscattertransform(chunk, scatter, maxoffset, position, yaw, pitch, roll);
    return worldscatterdefinitions.inrange(scatter.type) &&
           modeltagposition(worldscatterdefinitions[scatter.type]->model, "tag_emitter", flame, position, yaw, pitch, roll);
}

static vec worldplacelightcolor(const worlddefinition &type)
{
    if(!type.lightcolor[0]) return vec(1.0f, 0.58f, 0.24f);
    char *end = NULL;
    const long value = strtol(type.lightcolor, &end, 16);
    if(!end || *end || value < 0 || value > 0xFFFFFF) return vec(1.0f, 0.58f, 0.24f);
    return vec(float((value >> 16) & 0xFF) / 255.0f,
               float((value >> 8) & 0xFF) / 255.0f,
               float(value & 0xFF) / 255.0f);
}

static bool worldscattermounted(const worldchunk &chunk, const worldscatterinstance &scatter)
{
    const int tilex = scatter.x / WORLD_SECTION_SIZE,
              tiley = scatter.y / WORLD_SECTION_SIZE,
              tile = tiley * WORLD_SECTION_COLUMNS + tilex,
              section = clamp((scatter.z - 1) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1);
    return (chunk.mountedtiles[section] & (1U << tile)) != 0;
}

static float worldscatterchunkdistance(const worldchunk &chunk, const vec &focus, float expansion)
{
    const ivec origin = worldchunkorigin(chunk);
    const float minx = origin.x - expansion,
                miny = origin.y - expansion,
                maxx = origin.x + WORLD_CHUNK_SIZE + expansion,
                maxy = origin.y + WORLD_CHUNK_SIZE + expansion,
                dx = focus.x < minx ? minx - focus.x
                   : focus.x > maxx ? focus.x - maxx : 0.0f,
                dy = focus.y < miny ? miny - focus.y
                   : focus.y > maxy ? focus.y - maxy : 0.0f;
    return dx * dx + dy * dy;
}

static void updateworldscatterers()
{
    const vec *focus = player ? &player->o : camera1 ? &camera1->o : NULL;
    if(staticentsmaxdistance <= 0 || staticentsmaxamount <= 0 || !focus || worldchunks.empty())
    {
        clearworldscattererentities();
        return;
    }

    vector<worldgrasscandidate> candidates;
    vector<worldscatterchunkcandidate> scatterchunks;
    const float scattermaxoffset = game::getworldscattermaxoffset();
    const float radius = staticentsmaxdistance * WORLD_BLOCK_SIZE,
                radiussquared = radius * radius,
                maxoffset = max(scattermaxoffset, 0.0f) * WORLD_BLOCK_SIZE;

    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        const float distance = worldscatterchunkdistance(chunk, *focus, maxoffset);
        if(distance > radiussquared) continue;
        scatterchunks.add(worldscatterchunkcandidate(i, distance));
    }
    scatterchunks.sort();

    loopv(scatterchunks)
    {
        const worldchunk &chunk = worldchunks[scatterchunks[i].chunkindex];
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(!worldscatterdefinitions.inrange(scatter.type) || worldscatterdefinitions[scatter.type]->mapmodel < 0 ||
               !worldscattermounted(chunk, scatter))
                continue;
            vec position;
            int yaw, pitch, roll;
            worldscattertransform(chunk, scatter, scattermaxoffset, position, yaw, pitch, roll);
            const float dx = position.x - focus->x, dy = position.y - focus->y;
            if(dx * dx + dy * dy > radiussquared) continue;
            candidates.add(worldgrasscandidate(worldscatterkey(chunk, scatter), position, worldscatterdefinitions[scatter.type]->mapmodel, yaw, pitch,
                                                roll));
            if(candidates.length() >= staticentsmaxamount) break;
        }
        if(candidates.length() >= staticentsmaxamount) break;
    }

    hashtable<ivec, int> desired(1<<12);
    loopv(candidates) desired[candidates[i].key] = i;
    for(int i = worldgrassentities.length() - 1; i >= 0; --i)
    {
        worldgrassentity &active = worldgrassentities[i];
        int *candidateindex = desired.access(active.key);
        if(!candidateindex || !isworldmapmodelentity(active.id, candidates[*candidateindex].model))
        {
            destroyworldmapmodelentity(active.id);
            worldgrassentities.removeunordered(i);
            continue;
        }
        worldgrasscandidate &candidate = candidates[*candidateindex];
        if(!updateworldmapmodelentity(active.id, candidate.position, candidate.model, candidate.yaw, candidate.pitch, candidate.roll))
        {
            destroyworldmapmodelentity(active.id);
            worldgrassentities.removeunordered(i);
            continue;
        }
        candidates[*candidateindex].matched = true;
    }

    loopv(candidates) if(!candidates[i].matched)
    {
        int id = createworldmapmodelentity(candidates[i].position, candidates[i].model, candidates[i].yaw, candidates[i].pitch, candidates[i].roll);
        if(id < 0) break;
        worldgrassentities.add(worldgrassentity(candidates[i].key, id));
    }
}

void addworldtorchlights()
{
    if(staticlightmaxdistance <= 0 || !camera1 || worldchunks.empty()) return;

    const float maxdistance = staticlightmaxdistance * WORLD_BLOCK_SIZE,
                maxdistancesquared = maxdistance * maxdistance,
                fullshadowdistance = maxdistance / 3.0f,
                dynshadowdistance = fullshadowdistance * 2.0f;
    const float scattermaxoffset = game::getworldscattermaxoffset();

    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(!isworldtorch(scatter.type) || !worldscattermounted(chunk, scatter)) continue;

            vec flame;
            if(!worldtorchflameposition(chunk, scatter, scattermaxoffset, flame)) continue;

            const float distancesquared = flame.squaredist(camera1->o);
            if(distancesquared > maxdistancesquared) continue;
            const float distance = sqrtf(distancesquared);
            const int flags = distance <= fullshadowdistance ? 0 : distance <= dynshadowdistance ? L_NODYNSHADOW : L_NOSHADOW;
            const worlddefinition &type = *worldscatterdefinitions[scatter.type];
            adddynlight(flame, type.lightradius * WORLD_BLOCK_SIZE, worldplacelightcolor(type), 0, 0, flags | DL_NODIST);
        }
    }
}

void addworldtorchparticles()
{
    if(staticentsmaxdistance <= 0 || !camera1 || worldchunks.empty()) return;

    const float maxdistance = staticentsmaxdistance * WORLD_BLOCK_SIZE, maxdistancesquared = maxdistance * maxdistance;
    const float scattermaxoffset = game::getworldscattermaxoffset();
    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(!isworldtorch(scatter.type) || !worldscattermounted(chunk, scatter)) continue;

            vec flame;
            if(!worldtorchflameposition(chunk, scatter, scattermaxoffset, flame)) continue;
            if(flame.squaredist(camera1->o) > maxdistancesquared) continue;
            regular_particle_flame(PART_FLAME, flame, 0.7f, 0.7f, 0xFF8628, 1, 2.4f, 35.0f, 220.0f, -10);
            regular_particle_flame(PART_SMOKE, flame, 0.9f, 1.1f, 0xAA8C4E, 1, 3.0f, 16.0f, 1100.0f, -25);
        }
    }
}

ICOMMAND(getworldgrasscount, "", (),
{
    int count = 0;
    const int model = worldscatterdefinitions.inrange(worldgrassscatter) ? worldscatterdefinitions[worldgrassscatter]->mapmodel : -1;
    if(model >= 0) loopv(worldgrassentities)
        if(isworldmapmodelentity(worldgrassentities[i].id, model)) ++count;
    intret(count);
});

static int worldflowerscattertype(int flower)
{
    switch(flower)
    {
        case 0: return worldrosescatter;
        case 1: return worldtulipscatter;
        case 2: return worlddandelionscatter;
        default: return -1;
    }
}

ICOMMAND(getworldflowercount, "", (),
{
    int count = 0;
    loopv(worldgrassentities)
    {
        loopj(3)
        {
            const int type = worldflowerscattertype(j);
            const int model = worldscatterdefinitions.inrange(type)
                            ? worldscatterdefinitions[type]->mapmodel : -1;
            if(model >= 0 &&
               isworldmapmodelentity(worldgrassentities[i].id, model))
            {
                ++count;
                break;
            }
        }
    }
    intret(count);
});

ICOMMAND(getworldscatterdrawn, "", (), intret(worldgrassentities.length()));

bool isworldscatterentity(int id)
{
    loopv(worldgrassentities) if(worldgrassentities[i].id == id) return true;
    return false;
}

bool getworldscatterentitybox(int id, vec &center, vec &radius)
{
    if(!isworldscatterentity(id)) return false;
    const vector<extentity *> &ents = entities::getents();
    if(!ents.inrange(id)) return false;
    const extentity &e = *ents[id];
    model *m = loadmapmodel(e.attr1);
    if(!m) return false;

    m->boundbox(center, radius);
    if(e.attr5 > 0)
    {
        const float scale = e.attr5 / 100.0f;
        center.mul(scale);
        radius.mul(scale);
    }
    rotatebb(center, radius, e.attr2, e.attr3, e.attr4);
    center.add(e.o);
    return true;
}

bool getworldscatterentityedit(int id, int &type, ivec &support, int &orient)
{
    loopv(worldgrassentities)
    {
        const worldgrassentity &active = worldgrassentities[i];
        if(active.id != id) continue;
        loopvj(worldchunks)
        {
            const worldchunk &chunk = worldchunks[j];
            loopvk(chunk.scatter)
            {
                const worldscatterinstance &scatter = chunk.scatter[k];
                if(worldscatterkey(chunk, scatter) != active.key) continue;
                type = scatter.type;
                orient = scatter.orient;
                support = ivec(worldchunkorigin(chunk))
                    .add(ivec(scatter.x, scatter.y, scatter.z))
                    .sub(ivec(worldorientnormal(orient)).mul(
                        WORLD_BLOCK_SIZE));
                return true;
            }
        }
    }
    return false;
}




int getworldlightlevel(const vec &position)
{
    const float skyexposure = getworldskyexposure(position),
                ambientlevel = (ambient.r * 0.2126f + ambient.g * 0.7152f + ambient.b * 0.0722f) * ambientscale * (16.0f / 255.0f);
    float level = clamp(skyexposure * (sunlightscale * 16.0f + ambientlevel), 0.0f, 16.0f);
    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(!isworldtorch(scatter.type) || !worldscattermounted(chunk, scatter)) continue;
            vec flame;
            if(!worldtorchflameposition(chunk, scatter, game::getworldscattermaxoffset(), flame)) continue;
            const float torchlevel = worldscatterdefinitions[scatter.type]->lightradius - flame.dist(position) / WORLD_BLOCK_SIZE;
            level = max(level, torchlevel);
        }
    }
    return clamp(int(floorf(level + 0.5f)), 0, 16);
}

#endif
