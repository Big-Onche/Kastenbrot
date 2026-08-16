// worldrender.cpp: Kastenbrot streamed scatter presentation

#ifdef WORLDIO_MODULE_IMPLEMENTATION

namespace game
{
    namespace environment
    {
        extern float getambientlightlevel();
    }
}

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

struct worldplaceableinstance
{
    int chunkx, chunky;
    worldscatterinstance scatter;

    worldplaceableinstance(int chunkx, int chunky, const worldscatterinstance &scatter) : chunkx(chunkx), chunky(chunky), scatter(scatter) {}
};

static vector<worldplaceableinstance> worldplaceableinstances;

struct worldscattermeshvertex
{
    vec position, normal;
    vec2 texcoord;

    worldscattermeshvertex(const vec &position = vec(0, 0, 0), const vec &normal = vec(0, 0, 1), const vec2 &texcoord = vec2(0, 0))
        : position(position), normal(normal), texcoord(texcoord)
    {
    }
};

struct worldscattermeshbatch
{
    Texture *texture;
    int offset, length;

    worldscattermeshbatch(Texture *texture = NULL, int offset = 0, int length = 0) : texture(texture), offset(offset), length(length) {}
};

struct worldscattermesh
{
    int chunkx, chunky, tile, section, instances, grasses, flowers;
    GLuint vbo, ebo;
    ivec bbmin, bbmax;
    vector<worldscattermeshbatch> batches;
    bool dirty, boundsvalid;

    worldscattermesh(int chunkx, int chunky, int tile, int section)
        : chunkx(chunkx), chunky(chunky), tile(tile), section(section), instances(0), grasses(0), flowers(0), vbo(0), ebo(0), bbmin(0, 0, 0),
          bbmax(0, 0, 0), dirty(true), boundsvalid(false)
    {
    }
};

static vector<worldscattermesh *> worldscattermeshes;
static int worldscatterdrawn = 0, worldgrassdrawn = 0, worldflowerdrawn = 0;

FVARP(scattermeshalphatest, 0.0f, 0.5f, 1.0f);
FVARP(scattermeshwind, 0.0f, 1.0f, 8.0f);

static void cleanupworldscattermesh(worldscattermesh &mesh)
{
    if(mesh.vbo) glDeleteBuffers_(1, &mesh.vbo);
    if(mesh.ebo) glDeleteBuffers_(1, &mesh.ebo);
    mesh.vbo = mesh.ebo = 0;
    mesh.batches.setsize(0);
}

static void clearworldscattermeshes()
{
    loopv(worldscattermeshes)
    {
        cleanupworldscattermesh(*worldscattermeshes[i]);
        delete worldscattermeshes[i];
    }
    worldscattermeshes.setsize(0);
    worldplaceableinstances.setsize(0);
    worldscatterdrawn = worldgrassdrawn = worldflowerdrawn = 0;
}

static worldscattermesh *findworldscattermesh(int chunkx, int chunky, int tile, int section, bool create = false)
{
    loopv(worldscattermeshes)
    {
        worldscattermesh &mesh = *worldscattermeshes[i];
        if(mesh.chunkx == chunkx && mesh.chunky == chunky && mesh.tile == tile && mesh.section == section) return &mesh;
    }
    if(!create) return NULL;
    worldscattermesh *mesh = new worldscattermesh(chunkx, chunky, tile, section);
    worldscattermeshes.add(mesh);
    return mesh;
}

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
        if(scatter.orient == O_TOP)
        {
            const int item = getworldscatteritem(scatter.type);
            int slots = 0;
            if(getworldchestconfig(item, slots))
            {
                const ivec target(chunk.x * WORLD_CHUNK_SIZE + scatter.x, chunk.y * WORLD_CHUNK_SIZE + scatter.y, scatter.z);
                yaw = game::getchestyaw(target);
                pitch = int(game::getchestlidangle(target) + 0.5f);
            }
            return;
        }

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

static void worldscattermeshregion(const worldscatterinstance &scatter, int &tile, int &section)
{
    const int tilex = clamp(scatter.x / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1),
              tiley = clamp(scatter.y / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_COLUMNS) - 1);
    tile = tiley * WORLD_SECTION_COLUMNS + tilex;
    section = clamp((scatter.z - 1) / WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1);
}

static void dirtyworldscattermesh(worldchunk &chunk, const worldscatterinstance &scatter)
{
    if(isworldplaceable(scatter.type))
    {
        chunk.placeablesregistered = false;
        return;
    }
    int tile, section;
    worldscattermeshregion(scatter, tile, section);
    findworldscattermesh(chunk.x, chunk.y, tile, section, true)->dirty = true;
}

static void registerworldplaceables(worldchunk &chunk)
{
    if(chunk.placeablesregistered) return;
    for(int i = worldplaceableinstances.length() - 1; i >= 0; --i)
        if(worldplaceableinstances[i].chunkx == chunk.x && worldplaceableinstances[i].chunky == chunk.y)
            worldplaceableinstances.removeunordered(i);
    loopv(chunk.scatter) if(isworldplaceable(chunk.scatter[i].type))
        worldplaceableinstances.add(worldplaceableinstance(chunk.x, chunk.y, chunk.scatter[i]));
    chunk.placeablesregistered = true;
}

static void registerworldscattermeshes(worldchunk &chunk)
{
    if(chunk.scattermeshesregistered) return;
    loopv(chunk.scatter) dirtyworldscattermesh(chunk, chunk.scatter[i]);
    chunk.scattermeshesregistered = true;
}

static void pruneworldscattermeshes()
{
    for(int i = worldscattermeshes.length() - 1; i >= 0; --i)
    {
        worldscattermesh *mesh = worldscattermeshes[i];
        if(findworldchunk(mesh->chunkx, mesh->chunky) >= 0) continue;
        cleanupworldscattermesh(*mesh);
        delete mesh;
        worldscattermeshes.removeunordered(i);
    }
    for(int i = worldplaceableinstances.length() - 1; i >= 0; --i)
        if(findworldchunk(worldplaceableinstances[i].chunkx, worldplaceableinstances[i].chunky) < 0)
            worldplaceableinstances.removeunordered(i);
}

static void addworldscatterquad(vector<worldscattermeshvertex> &vertices, vector<uint> &indices, const vec &center, float halfwidth, float bottom,
                                float top, float angle, worldscattermesh &mesh)
{
    const vec direction(cosf(angle) * halfwidth, sinf(angle) * halfwidth, 0);
    const vec left = vec(center).sub(direction), right = vec(center).add(direction);
    const vec positions[4] =
    {
        vec(left.x, left.y, bottom), vec(right.x, right.y, bottom), vec(right.x, right.y, top), vec(left.x, left.y, top)
    };
    static const vec2 texcoords[4] = { vec2(0, 1), vec2(1, 1), vec2(1, 0), vec2(0, 0) };
    const uint base = vertices.length();
    loopi(4)
    {
        worldscattermeshvertex &v = vertices.add(worldscattermeshvertex(positions[i], vec(0, 0, 1), texcoords[i]));
        const ivec minimum(int(floorf(v.position.x - 8.0f)), int(floorf(v.position.y - 8.0f)), int(floorf(v.position.z))),
                   maximum(int(ceilf(v.position.x + 8.0f)), int(ceilf(v.position.y + 8.0f)), int(ceilf(v.position.z)));
        if(!mesh.boundsvalid)
        {
            mesh.bbmin = minimum;
            mesh.bbmax = maximum;
            mesh.boundsvalid = true;
        }
        else
        {
            mesh.bbmin.min(minimum);
            mesh.bbmax.max(maximum);
        }
    }
    static const uint quadindices[6] = { 0, 1, 2, 0, 2, 3 };
    loopi(6) indices.add(base + quadindices[i]);
}

static void addworldscattergeometry(const worldchunk &chunk, const worldscatterinstance &scatter, vector<worldscattermeshvertex> &vertices,
                                    vector<uint> &indices, worldscattermesh &mesh)
{
    const worldscatterrenderdefinition &type = worldscatterrenderdefinitions[scatter.type];
    game::cacheworldscattertransform(chunk.x, chunk.y, game::getworldscattermaxoffset(), scatter);
    const float yaw = scatter.renderyaw * RAD, cosine = cosf(yaw), sine = sinf(yaw);
    const vec localcenter(type.center.x * cosine - type.center.y * sine, type.center.x * sine + type.center.y * cosine, type.center.z);
    const vec center(scatter.x + WORLD_BLOCK_SIZE * 0.5f + scatter.renderoffsetx + localcenter.x,
                     scatter.y + WORLD_BLOCK_SIZE * 0.5f + scatter.renderoffsety + localcenter.y, scatter.z + localcenter.z);
    const float halfwidth = max(max(type.radius.x, type.radius.y), 0.5f),
                bottom = center.z - max(type.radius.z, 0.5f), top = center.z + max(type.radius.z, 0.5f);
    addworldscatterquad(vertices, indices, center, halfwidth, bottom, top, yaw, mesh);
    addworldscatterquad(vertices, indices, center, halfwidth, bottom, top, yaw + M_PI / 2.0f, mesh);
}

static void rebuildworldscattermesh(worldscattermesh &mesh, const worldchunk &chunk)
{
    vector<worldscattermeshvertex> vertices;
    vector<uint> indices;
    mesh.batches.setsize(0);
    mesh.instances = mesh.grasses = mesh.flowers = 0;
    mesh.boundsvalid = false;

    loopv(worldscatterdefinitions)
    {
        if(isworldplaceable(i) || !worldscatterrenderdefinitions.inrange(i) || !worldscatterrenderdefinitions[i].texture[0]) continue;
        const int offset = indices.length();
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(scatter.type != i) continue;
            int tile, section;
            worldscattermeshregion(scatter, tile, section);
            if(tile != mesh.tile || section != mesh.section) continue;
            addworldscattergeometry(chunk, scatter, vertices, indices, mesh);
            mesh.instances++;
            if(scatter.type == worldgrassscatter) mesh.grasses++;
            if(scatter.type == worldrosescatter || scatter.type == worldtulipscatter || scatter.type == worlddandelionscatter) mesh.flowers++;
        }
        if(indices.length() == offset) continue;
        Texture *texture = textureload(worldscatterrenderdefinitions[i].texture, 3, true, false, true);
        if(texture != notexture) mesh.batches.add(worldscattermeshbatch(texture, offset, indices.length() - offset));
    }

    if(vertices.empty() || indices.empty() || mesh.batches.empty())
    {
        cleanupworldscattermesh(mesh);
        mesh.instances = mesh.grasses = mesh.flowers = 0;
        mesh.boundsvalid = false;
        mesh.dirty = false;
        return;
    }

    if(!mesh.vbo) glGenBuffers_(1, &mesh.vbo);
    if(!mesh.ebo) glGenBuffers_(1, &mesh.ebo);
    gle::bindvbo(mesh.vbo);
    glBufferData_(GL_ARRAY_BUFFER, vertices.length() * sizeof(worldscattermeshvertex), vertices.getbuf(), GL_STATIC_DRAW);
    gle::bindebo(mesh.ebo);
    glBufferData_(GL_ELEMENT_ARRAY_BUFFER, indices.length() * sizeof(uint), indices.getbuf(), GL_STATIC_DRAW);
    mesh.dirty = false;
}

static bool worldscattermeshresident(const worldscattermesh &mesh, const worldchunk &chunk)
{
    const uint tilebit = 1U << mesh.tile;
    return chunk.mountedtiles[mesh.section] & tilebit && !(chunk.renderdata.flags[mesh.section][mesh.tile] & SECTION_NO_RENDER) &&
           worldsectionvaactive(chunk.varesidency[mesh.section][mesh.tile]);
}

static void worldscattermeshbounds(const worldscattermesh &mesh, const worldchunk &chunk, ivec &bbmin, ivec &bbmax)
{
    const ivec origin = worldchunkorigin(chunk);
    if(mesh.boundsvalid)
    {
        bbmin = ivec(mesh.bbmin).add(ivec(origin.x, origin.y, 0));
        bbmax = ivec(mesh.bbmax).add(ivec(origin.x, origin.y, 0));
        return;
    }
    const int x = mesh.tile % WORLD_SECTION_COLUMNS, y = mesh.tile / WORLD_SECTION_COLUMNS;
    bbmin = ivec(origin).add(ivec(x * WORLD_SECTION_SIZE, y * WORLD_SECTION_SIZE, mesh.section * WORLD_SECTION_SIZE));
    bbmax = ivec(bbmin).add(WORLD_SECTION_SIZE);
}

static bool worldscattermeshvisible(const worldscattermesh &mesh, const worldchunk &chunk)
{
    if(!worldscattermeshresident(mesh, chunk) || !(chunk.visibletiles[mesh.section] & (1U << mesh.tile))) return false;
    ivec bbmin, bbmax;
    worldscattermeshbounds(mesh, chunk, bbmin, bbmax);
    return isvisiblebb(bbmin, ivec(bbmax).sub(bbmin)) != VFC_NOT_VISIBLE && !pvsoccluded(bbmin, bbmax);
}

static bool worldscattermeshshadowvisible(const worldscattermesh &mesh, const worldchunk &chunk)
{
    if(!worldscattermeshresident(mesh, chunk)) return false;
    ivec bbmin, bbmax;
    worldscattermeshbounds(mesh, chunk, bbmin, bbmax);
    switch(shadowmapping)
    {
        case SM_CASCADE: return (calcbbcsmsplits(bbmin, bbmax) & (1 << shadowside)) != 0;
        case SM_CUBEMAP:
            return (!smdistcull || shadoworigin.dist_to_bb(bbmin, bbmax) < shadowradius) &&
                   (!smbbcull || (calcbbsidemask(bbmin, bbmax, shadoworigin, shadowradius, shadowbias) & (1 << shadowside)) != 0);
        case SM_SPOT:
            return (!smdistcull || shadoworigin.dist_to_bb(bbmin, bbmax) < shadowradius) &&
                   (!smbbcull || bbinsidespot(shadoworigin, shadowdir, shadowspot, bbmin, bbmax));
        default: return false;
    }
}

static void drawworldscattermeshes(bool shadow)
{
    if(!shadow) worldscatterdrawn = worldgrassdrawn = worldflowerdrawn = 0;
    Shader *shader = shadow ? lookupshaderbyname("smscatter") : useshaderbyname("scatterworld");
    if(!shader || worldscattermeshes.empty()) return;
    shader->set();
    LOCALPARAMF(scatteralphatest, scattermeshalphatest);
    if(!shadow)
    {
        LOCALPARAMF(colorparams, 1, 1, 1, 1);
        LOCALPARAMF(texgenscroll, 0, 0);
    }

    glActiveTexture_(GL_TEXTURE0);
    glDisable(GL_CULL_FACE);
    gle::enablevertex();
    gle::enabletexcoord0();
    if(!shadow) gle::enablenormal();
    loopv(worldscattermeshes)
    {
        worldscattermesh &mesh = *worldscattermeshes[i];
        const int chunkindex = findworldchunk(mesh.chunkx, mesh.chunky);
        if(!worldchunks.inrange(chunkindex)) continue;
        const worldchunk &chunk = worldchunks[chunkindex];
        if(shadow ? !worldscattermeshshadowvisible(mesh, chunk) : !worldscattermeshvisible(mesh, chunk)) continue;
        if(mesh.dirty) rebuildworldscattermesh(mesh, chunk);
        if(!mesh.vbo || !mesh.ebo || mesh.batches.empty()) continue;

        const ivec origin = worldchunkorigin(chunk);
        LOCALPARAMF(scatterparams, float(origin.x), float(origin.y), 0.0f, scattermeshwind);
        gle::bindvbo(mesh.vbo);
        gle::bindebo(mesh.ebo);
        const worldscattermeshvertex *pointer = 0;
        gle::vertexpointer(sizeof(worldscattermeshvertex), pointer->position.v);
        gle::texcoord0pointer(sizeof(worldscattermeshvertex), pointer->texcoord.v);
        if(!shadow) gle::normalpointer(sizeof(worldscattermeshvertex), pointer->normal.v);
        loopvj(mesh.batches)
        {
            const worldscattermeshbatch &batch = mesh.batches[j];
            glBindTexture(GL_TEXTURE_2D, batch.texture->id);
            glDrawElements(GL_TRIANGLES, batch.length, GL_UNSIGNED_INT, (uint *)0 + batch.offset);
            glde++;
            xtravertsva += batch.length;
        }
        if(!shadow)
        {
            worldscatterdrawn += mesh.instances;
            worldgrassdrawn += mesh.grasses;
            worldflowerdrawn += mesh.flowers;
        }
    }
    gle::clearvbo();
    gle::clearebo();
    if(!shadow) gle::disablenormal();
    gle::disabletexcoord0();
    gle::disablevertex();
    glEnable(GL_CULL_FACE);
}

void renderworldscattermeshes()
{
    drawworldscattermeshes(false);
}

void renderworldscattershadows()
{
    drawworldscattermeshes(true);
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
    static float meshmaxoffset = -1.0f;
    const float scattermaxoffset = game::getworldscattermaxoffset();
    if(meshmaxoffset != scattermaxoffset)
    {
        meshmaxoffset = scattermaxoffset;
        loopv(worldscattermeshes) worldscattermeshes[i]->dirty = true;
    }
    pruneworldscattermeshes();
    loopv(worldchunks) if(!worldchunks[i].loading && worldchunks[i].root)
    {
        registerworldscattermeshes(worldchunks[i]);
        registerworldplaceables(worldchunks[i]);
    }

    const vec *focus = player ? &player->o : camera1 ? &camera1->o : NULL;
    if(staticentsmaxdistance <= 0 || staticentsmaxamount <= 0 || !focus || worldchunks.empty())
    {
        clearworldscattererentities();
        return;
    }

    vector<worldgrasscandidate> candidates;
    vector<worldscatterchunkcandidate> scatterchunks;
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
        loopvj(worldplaceableinstances)
        {
            const worldplaceableinstance &placeable = worldplaceableinstances[j];
            if(placeable.chunkx != chunk.x || placeable.chunky != chunk.y) continue;
            const worldscatterinstance &scatter = placeable.scatter;
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

void updateworldchestanimations()
{
    updateworldscatterers();
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
    intret(worldgrassdrawn));

ICOMMAND(getworldflowercount, "", (),
    intret(worldflowerdrawn));

ICOMMAND(getworldscatterdrawn, "", (), intret(worldscatterdrawn + worldgrassentities.length()));

bool isworldscatterentity(int id)
{
    loopv(worldgrassentities) if(worldgrassentities[i].id == id) return true;
    return false;
}

static bool findworldscatterentity(int id, const worldchunk *&foundchunk, const worldscatterinstance *&foundscatter)
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
                foundchunk = &chunk;
                foundscatter = &scatter;
                return true;
            }
        }
        break;
    }
    return false;
}

bool getworldscatterentitybox(int id, vec &center, vec &radius)
{
    if(!isworldscatterentity(id)) return false;
    const vector<extentity *> &ents = entities::getents();
    if(!ents.inrange(id)) return false;
    const extentity &e = *ents[id];

    const worldchunk *chunk = NULL;
    const worldscatterinstance *scatter = NULL;
    if(findworldscatterentity(id, chunk, scatter))
    {
        int slots = 0;
        if(getworldchestconfig(getworldscatteritem(scatter->type), slots))
        {
            center = vec(e.o).addz(WORLD_BLOCK_SIZE * 0.5f);
            radius = vec(WORLD_BLOCK_SIZE * 0.5f);
            return true;
        }
    }

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
    const worldchunk *chunk = NULL;
    const worldscatterinstance *scatter = NULL;
    if(!findworldscatterentity(id, chunk, scatter)) return false;
    type = scatter->type;
    orient = scatter->orient;
    support = ivec(worldchunkorigin(*chunk)).add(ivec(scatter->x, scatter->y, scatter->z))
        .sub(ivec(worldorientnormal(orient)).mul(WORLD_BLOCK_SIZE));
    return true;
}

static bool worldrayboxdistance(const vec &minimum, const vec &size, const vec &origin, const vec &direction, float reach, float &distance)
{
    float nearbound = 0.0f, farbound = reach;
    loopi(3)
    {
        const float maximum = minimum[i] + size[i];
        if(fabsf(direction[i]) <= 1e-6f)
        {
            if(origin[i] < minimum[i] || origin[i] > maximum) return false;
            continue;
        }
        float first = (minimum[i] - origin[i]) / direction[i], second = (maximum - origin[i]) / direction[i];
        if(first > second) swap(first, second);
        nearbound = max(nearbound, first);
        farbound = min(farbound, second);
        if(nearbound > farbound) return false;
    }
    distance = nearbound;
    return farbound >= 0.0f && nearbound <= reach;
}

static void worldscatterlogicalbox(const worldchunk &chunk, const worldscatterinstance &scatter, vec &center, vec &radius)
{
    vec position;
    int yaw, pitch, roll;
    worldscattertransform(chunk, scatter, game::getworldscattermaxoffset(), position, yaw, pitch, roll);
    const worldscatterrenderdefinition &type = worldscatterrenderdefinitions[scatter.type];
    center = type.center;
    radius = type.radius;
    rotatebb(center, radius, yaw, pitch, roll);
    center.add(position);
}

bool getworldscatterhit(const vec &origin, const vec &direction, float reach, int &type, ivec &support, int &orient, vec &center, vec &radius, float &distance)
{
    float closest = reach;
    bool found = false;
    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        const ivec chunkorigin = worldchunkorigin(chunk);
        float chunkdistance = 0;
        if(!worldrayboxdistance(vec(chunkorigin.x - WORLD_BLOCK_SIZE * 2, chunkorigin.y - WORLD_BLOCK_SIZE * 2, 0),
                                vec(WORLD_CHUNK_SIZE + WORLD_BLOCK_SIZE * 4, WORLD_CHUNK_SIZE + WORLD_BLOCK_SIZE * 4, WORLD_MAP_SIZE), origin,
                                direction, closest, chunkdistance))
            continue;
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(isworldplaceable(scatter.type) || !worldscatterdefinitions.inrange(scatter.type) ||
               !worldscatterrenderdefinitions.inrange(scatter.type) || !worldscattermounted(chunk, scatter))
                continue;
            vec scattercenter, scatterradius;
            worldscatterlogicalbox(chunk, scatter, scattercenter, scatterradius);
            float hitdistance = 0;
            if(!worldrayboxdistance(vec(scattercenter).sub(scatterradius), vec(scatterradius).mul(2), origin, direction, closest,
                                    hitdistance))
                continue;
            closest = hitdistance;
            type = scatter.type;
            orient = scatter.orient;
            support = ivec(chunkorigin).add(ivec(scatter.x, scatter.y, scatter.z))
                .sub(ivec(worldorientnormal(orient)).mul(WORLD_BLOCK_SIZE));
            center = scattercenter;
            radius = scatterradius;
            found = true;
        }
    }
    distance = closest;
    return found;
}

bool getworldchesthit(const vec &origin, const vec &direction, float reach, int &type, ivec &support, int &orient)
{
    float closest = reach;
    bool found = false;
    const float scattermaxoffset = game::getworldscattermaxoffset();
    loopv(worldchunks)
    {
        const worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        loopvj(chunk.scatter)
        {
            const worldscatterinstance &scatter = chunk.scatter[j];
            if(!worldscattermounted(chunk, scatter)) continue;
            int slots = 0;
            if(!getworldchestconfig(getworldscatteritem(scatter.type), slots)) continue;

            vec position;
            int yaw, pitch, roll;
            worldscattertransform(chunk, scatter, scattermaxoffset, position, yaw, pitch, roll);
            float distance = 0;
            const vec minimum = vec(position.x - WORLD_BLOCK_SIZE * 0.5f, position.y - WORLD_BLOCK_SIZE * 0.5f, position.z),
                      size(WORLD_BLOCK_SIZE, WORLD_BLOCK_SIZE, WORLD_BLOCK_SIZE);
            if(!worldrayboxdistance(minimum, size, origin, direction, closest, distance)) continue;

            closest = distance;
            type = scatter.type;
            orient = scatter.orient;
            support = ivec(worldchunkorigin(chunk)).add(ivec(scatter.x, scatter.y, scatter.z))
                .sub(ivec(worldorientnormal(orient)).mul(WORLD_BLOCK_SIZE));
            found = true;
        }
    }
    return found;
}




int getworldlightlevel(const vec &position)
{
    float level = clamp(game::environment::getambientlightlevel(), 0.0f, 16.0f);
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
