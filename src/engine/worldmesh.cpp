// Included by octarender.cpp: texture state is captured on the main thread;
// workers only read private octree/texture snapshots and produce CPU packets.

#ifdef WORLDMESH_MODULE_IMPLEMENTATION

#include "worldmesh.h"
#include "watermeshbuild.h"

struct worldmeshtexture
{
    int index;
    bool directional;
    bool cutout, refractive, leaf;
    vec4 sgen[6], tgen[6];
    vec tangent[6], bitangent[6];
};

struct worldmeshsnapshot
{
    cube root[8] = {};
    ivec origin;
    int scale, size;
    vector<worldmeshtexture> textures;

    worldmeshsnapshot(const ivec &origin) : origin(origin), scale(worldscale), size(worldsize)
    {
    }

    static void release(cube *family)
    {
        loopi(8) if(family[i].children)
        {
            release(family[i].children);
            delete[] family[i].children;
        }
    }

    ~worldmeshsnapshot()
    {
        release(root);
    }

    void texture(int index)
    {
        loopv(textures) if(textures[i].index == index) return;
        VSlot &slot = lookupvslot(index, true);
        worldmeshtexture &copy = textures.add();
        copy.index = index;
        const char *shader = slot.slot->shader->name;
        copy.leaf = !strcmp(shader, "leafworld") || !strcmp(shader, "leafclimateworld") || !strcmp(shader, "birchleafclimateworld");
        copy.cutout = copy.leaf || !strcmp(shader, "scatterworld");
        copy.refractive = slot.refractscale > 0;
        // Match legacy O_ANY batching, but retain face direction when selecting
        // a positional environment map at publication time.
        copy.directional = !slot.scroll.iszero() ||
                           ((slot.slot->shader->type & SHADER_ENVMAP) && !(slot.slot->texmask & (1 << TEX_ENVMAP)));
        loopi(6)
        {
            calctexgen(slot, i, copy.sgen[i], copy.tgen[i]);
            copy.tangent[i] = orientation_tangent[slot.rotation][i];
            copy.bitangent[i] = orientation_bitangent[slot.rotation][i];
        }
    }

    void capture(const cube *source, cube *destination, const ivec &co, int size)
    {
        const ivec minimum = ivec(origin).sub(WORLD_BLOCK_SIZE), maximum = ivec(origin).add(WORLD_SECTION_SIZE + WORLD_BLOCK_SIZE);
        loopi(8)
        {
            const ivec o(i, co, size);
            if(o.x >= maximum.x || o.y >= maximum.y || o.z >= maximum.z ||
               o.x + size <= minimum.x || o.y + size <= minimum.y || o.z + size <= minimum.z) continue;
            const cube &src = source[i];
            cube &dst = destination[i];
            memcpy(dst.edges, src.edges, sizeof(dst.edges));
            memcpy(dst.texture, src.texture, sizeof(dst.texture));
            dst.material = src.material;
            dst.playeredited = src.playeredited;
            // Private metadata, never written back to gameplay cubes.
            dst.visible = isworldleafcube(src) ? 1 : 0;
            if(src.children)
            {
                dst.children = new cube[8]();
                capture(src.children, dst.children, o, size / 2);
            }
            else if(o.x < origin.x + WORLD_SECTION_SIZE && o.y < origin.y + WORLD_SECTION_SIZE && o.z < origin.z + WORLD_SECTION_SIZE &&
                    o.x + size > origin.x && o.y + size > origin.y && o.z + size > origin.z && !isempty(src))
                loopj(6) texture(src.texture[j]);
        }
    }

    const cube &neighbor(int orient, const ivec &co, int cubesize, ivec &no, int &nsize) const
    {
        ivec position(co);
        position[dimension(orient)] += dimcoord(orient) ? cubesize : -1;
        return at(position, cubesize, no, nsize);
    }

    const cube &at(const ivec &position, int cubesize, ivec &no, int &nsize) const
    {
        static const cube empty = {};
        if(position.x < 0 || position.y < 0 || position.z < 0 || position.x >= size || position.y >= size || position.z >= size)
        {
            no = position;
            nsize = cubesize;
            return empty;
        }
        int level = scale - 1;
        const cube *c = &root[octastep(position.x, position.y, position.z, level)];
        while(c->children && (1 << level) > cubesize)
        {
            --level;
            c = &c->children[octastep(position.x, position.y, position.z, level)];
        }
        nsize = 1 << level;
        no = ivec(position).mask(~(nsize - 1));
        return *c;
    }
};

struct worldmeshface
{
    vec position[4];
    ushort texture, material;
    int orient, count;
    bool axis, playeredited;
};

struct worldmeshjob
{
    worldmeshsnapshot snapshot;
    worldmeshpacket packet;
    ullong revision, epoch, request;
    bool edited;
    SDL_atomic_t cancelled;

    worldmeshjob(const ivec &origin, ullong revision, ullong epoch, ullong request, bool edited)
        : snapshot(origin), revision(revision), epoch(epoch), request(request), edited(edited)
    {
        SDL_AtomicSet(&cancelled, 0);
    }
};

static vector<worldmeshsection *> worldmeshsections;
static hashtable<ivec, worldmeshsection *> worldmeshowners;
static vector<worldmeshjob *> worldmeshjobs, worldmeshresults;
static SDL_Thread *worldmeshthread = NULL;
static SDL_mutex *worldmeshmutex = NULL;
static SDL_cond *worldmeshcond = NULL;
static bool worldmeshstop = false;
static ullong worldmeshepoch = 1;
static ullong worldmeshrequest = 0;
static ullong worldmeshgeneration = 1;
static worldmeshjob *worldmeshactive = NULL;
static ullong worldmeshcachedbytes = 0, worldmeshgarbagebytes = 0;
static Uint64 worldmeshlastpublish = 0;
static vector<ivec> worldmeshcompactqueue;
static int worldmeshcompactcursor = 0;

// Keep the traditional renderer available while validating the migration, but
// preserve the selected renderer across launches like other graphics settings
VARFP(worldmeshpackets, 0, 1, 1, allchanged());

static bool worldmeshfaceorder(const worldmeshface &a, const worldmeshface &b)
{
    if(a.axis != b.axis) return a.axis > b.axis;
    if(a.orient != b.orient) return a.orient < b.orient;
    const int dim = dimension(a.orient);
    if(a.position[0][dim] != b.position[0][dim]) return a.position[0][dim] < b.position[0][dim];
    if(a.texture != b.texture) return a.texture < b.texture;
    if(a.playeredited != b.playeredited) return a.playeredited < b.playeredited;
    return a.material < b.material;
}

static bool worldmeshsameplane(const worldmeshface &a, const worldmeshface &b)
{
    return a.axis && b.axis && a.orient == b.orient && a.texture == b.texture && a.material == b.material && a.playeredited == b.playeredited &&
           a.position[0][dimension(a.orient)] == b.position[0][dimension(b.orient)];
}

static void collectworldmeshsolidface(worldmeshjob &job, const cube &c, const ivec &co, int size, int orient,
                                      vector<worldmeshface> &faces)
{
    if(SDL_AtomicGet(&job.cancelled)) return;
    ivec no;
    int nsize;
    const cube &neighbor = job.snapshot.neighbor(orient, co, size, no, nsize);
    if(neighbor.children && size > 1 && !c.visible)
    {
        // Resolve partly occluded coarse faces against the actual neighbouring
        // leaves before greedy merging, rather than emitting buried triangles.
        const int dim = dimension(orient), childsize = size / 2;
        loopi(8) if(octacoord(dim, i) == dimcoord(orient))
            collectworldmeshsolidface(job, c, ivec(i, co, childsize), childsize, orient, faces);
        return;
    }
    const bool sharedleaf = size == nsize && !neighbor.children && c.visible && neighbor.visible;
    // visibletrisagainst gives shared leaf planes to positive orientations only.
    // With leavesalpha disabled, snapshot leaf flags are clear and normal solid
    // occlusion removes both internal faces instead.
    const int visible = visibletrisagainst(c, orient, co, size, neighbor, no, nsize, sharedleaf);
    if(!visible) return;
    ivec corners[4];
    genfaceverts(c, orient, corners);
    const int order = visible & 4 ? 1 : 0;
    worldmeshface &face = faces.add();
    face.texture = c.texture[orient];
    face.material = c.material;
    face.playeredited = c.playeredited;
    face.orient = orient;
    face.axis = visible == 3;
    face.count = 0;
    face.position[face.count++] = vec(corners[order]).mul(size / 8.0f).add(vec(co));
    if(visible & 1) face.position[face.count++] = vec(corners[order + 1]).mul(size / 8.0f).add(vec(co));
    face.position[face.count++] = vec(corners[order + 2]).mul(size / 8.0f).add(vec(co));
    if(visible & 2) face.position[face.count++] = vec(corners[(order + 3) & 3]).mul(size / 8.0f).add(vec(co));
}

static void collectworldmeshfaces(worldmeshjob &job, const cube *family, const ivec &origin, int size, vector<worldmeshface> &faces)
{
    const ivec &minimum = job.snapshot.origin;
    const ivec maximum = ivec(minimum).add(WORLD_SECTION_SIZE);
    loopi(8)
    {
        if(SDL_AtomicGet(&job.cancelled)) return;
        const cube &c = family[i];
        const ivec co(i, origin, size);
        if(co.x >= maximum.x || co.y >= maximum.y || co.z >= maximum.z ||
           co.x + size <= minimum.x || co.y + size <= minimum.y || co.z + size <= minimum.z) continue;
        if(c.children)
        {
            collectworldmeshfaces(job, c.children, co, size / 2, faces);
            continue;
        }
        loop(orient, 6)
        {
            ivec no;
            int nsize;
            const cube &neighbor = job.snapshot.neighbor(orient, co, size, no, nsize);
            if(c.material)
            {
                static const ushort masks[] = { MATF_VOLUME | MATF_INDEX, MATF_CLIP, MAT_DEATH, MAT_NOGI, MAT_ALPHA };
                loopk(sizeof(masks) / sizeof(masks[0]))
                {
                    const int mask = masks[k] & ~MATF_INDEX, mat = c.material & mask;
                    if(!mat || !visiblefaceagainst(c, orient, co, size, neighbor, no, nsize, mat, MAT_AIR, mask)) continue;
                    materialsurface m = {};
                    m.o = co;
                    m.orient = orient;
                    m.material = c.material & masks[k];
                    m.visible = mat == MAT_WATER || mat == MAT_GLASS || (mat == MAT_LAVA && orient != O_BOTTOM) ?
                                MATSURF_VISIBLE : MATSURF_EDIT_ONLY;
                    const int dim = dimension(orient), row = R[dim], col = C[dim];
                    if(dimcoord(orient)) m.o[dim] += size;
                    if(m.o[dim] < minimum[dim] || m.o[dim] > maximum[dim]) break;
                    m.o[row] = max(co[row], minimum[row]);
                    m.o[col] = max(co[col], minimum[col]);
                    m.rsize = min(co[row] + size, maximum[row]) - m.o[row];
                    m.csize = min(co[col] + size, maximum[col]) - m.o[col];
                    if(m.rsize && m.csize) job.packet.materials.add(m);
                    break;
                }
            }
            if(isempty(c)) continue;
            if(isentirelysolid(c))
            {
                collectworldmeshsolidface(job, c, co, size, orient, faces);
                continue;
            }
            const bool sharedleaf = size == nsize && !neighbor.children && c.visible && neighbor.visible;
            const int visible = visibletrisagainst(c, orient, co, size, neighbor, no, nsize, sharedleaf);
            if(!visible) continue;
            ivec corners[4];
            genfaceverts(c, orient, corners);
            const int convex = faceconvexity(corners), order = visible & 4 || convex < 0 ? 1 : 0;
            worldmeshface &face = faces.add();
            face.texture = c.texture[orient];
            face.material = c.material;
            face.playeredited = c.playeredited;
            face.orient = orient;
            face.axis = isentirelysolid(c) && visible == 3;
            face.count = 0;
            face.position[face.count++] = vec(corners[order]).mul(size / 8.0f).add(vec(co));
            if(visible & 1) face.position[face.count++] = vec(corners[order + 1]).mul(size / 8.0f).add(vec(co));
            face.position[face.count++] = vec(corners[order + 2]).mul(size / 8.0f).add(vec(co));
            if(visible & 2) face.position[face.count++] = vec(corners[(order + 3) & 3]).mul(size / 8.0f).add(vec(co));
        }
    }
}

static void emitworldmeshtriangle(worldmeshjob &job, const worldmeshface &face, const vec triangle[3])
{
    // Clip each original triangle separately: clipping a non-planar quad would
    // change the Cube diagonal and therefore its pushed-corner surface.
    vec polygon[16], scratch[16];
    int count = 3;
    loopi(3) polygon[i] = triangle[i];
    loop(plane, 6)
    {
        const int dim = dimension(plane);
        const float boundary = job.snapshot.origin[dim] + (dimcoord(plane) ? WORLD_SECTION_SIZE : 0);
        int output = 0;
        loopi(count)
        {
            const vec &a = polygon[i], &b = polygon[(i + 1) % count];
            const float da = (a[dim] - boundary) * (dimcoord(plane) ? -1 : 1),
                        db = (b[dim] - boundary) * (dimcoord(plane) ? -1 : 1);
            if(da >= 0) scratch[output++] = a;
            if((da < 0) != (db < 0)) scratch[output++] = vec(b).sub(a).mul(da / (da - db)).add(a);
        }
        count = output;
        if(count < 3) return;
        loopi(count) polygon[i] = scratch[i];
    }
    vec normal;
    normal.cross(triangle[0], triangle[1], triangle[2]);
    if(normal.iszero()) return;
    normal.normalize();
    const worldmeshtexture *texture = NULL;
    loopv(job.snapshot.textures) if(job.snapshot.textures[i].index == face.texture) { texture = &job.snapshot.textures[i]; break; }
    if(!texture) return;
    worldmeshpacket &packet = job.packet;
    const int orient = texture->directional ? face.orient : O_ANY;
    if(packet.ranges.empty() || packet.ranges.last().texture != face.texture || packet.ranges.last().material != face.material ||
       packet.ranges.last().orient != orient)
    {
        worldmeshdrawrange &range = packet.ranges.add();
        range.first = packet.indices.length();
        range.texture = face.texture;
        range.material = face.material;
        range.orient = orient;
        range.renderclass = texture->refractive && (face.material & MAT_ALPHA) ? WORLDMESH_REFRACTIVE :
                            texture->cutout && (!texture->leaf || (face.material & MAT_ALPHA)) ? WORLDMESH_CUTOUT :
                            (face.material & MAT_ALPHA) ? WORLDMESH_TRANSLUCENT : WORLDMESH_OPAQUE;
        range.alpha = range.renderclass >= WORLDMESH_TRANSLUCENT;
        range.twosided = texture->leaf && range.renderclass == WORLDMESH_CUTOUT;
    }
    const uint base = packet.vertices.length();
    const bool sealedges = face.axis && packet.ranges.last().renderclass == WORLDMESH_OPAQUE;
    const int dim = dimension(face.orient), row = R[dim], col = C[dim];
    vec minimum(face.position[0]), maximum(face.position[0]);
    if(sealedges)
    {
        loopi(face.count) { minimum.min(face.position[i]); maximum.max(face.position[i]); }
        minimum.max(vec(job.snapshot.origin));
        maximum.min(vec(ivec(job.snapshot.origin).add(WORLD_SECTION_SIZE)));
    }
    vec tangent(texture->tangent[face.orient]);
    tangent.project(normal);
    if(!tangent.iszero()) tangent.normalize();
    loopi(count)
    {
        vertex &v = packet.vertices.add();
        v.pos = polygon[i];
        if(sealedges)
        {
            // Greedy rectangles leave T-junctions. Overlap their outer edges in-plane
            // to hide rasterization cracks without extra triangles or shader work.
            // Apply after clipping so section seams are sealed too; leave the face
            // plane and shared triangle diagonal unchanged. 1/64 is representable
            // throughout the streamed world and is less than 0.1% of a terrain cube.
            const float overlap = 1.0f / 64.0f;
            v.pos[row] += polygon[i][row] == minimum[row] ? -overlap : polygon[i][row] == maximum[row] ? overlap : 0;
            v.pos[col] += polygon[i][col] == minimum[col] ? -overlap : polygon[i][col] == maximum[col] ? overlap : 0;
        }
        // Keep provenance separate during vertex deduplication; resolve climate on the main thread at publication.
        v.tc = vec(texture->sgen[face.orient].dot(v.pos), texture->tgen[face.orient].dot(v.pos), face.playeredited ? -1.0f : 0.0f);
        v.norm = bvec(normal);
        v.tangent = bvec4(bvec(tangent), texture->bitangent[face.orient].scalartriple(normal, tangent) < 0 ? 0 : 255);
        // Pack the signed GL_BYTE attributes just like vacollect::genverts().
        // Tangent.w needs the same conversion for the bitangent handedness.
        v.norm.flip();
        v.tangent.flip();
    }
    for(int i = 1; i + 1 < count; ++i)
    {
        packet.indices.add(base);
        packet.indices.add(base + i);
        packet.indices.add(base + i + 1);
        packet.ranges.last().count += 3;
    }
}

static void emitworldmeshface(worldmeshjob &job, const worldmeshface &face)
{
    for(int i = 1; i + 1 < face.count; ++i)
    {
        const vec triangle[3] = { face.position[0], face.position[i], face.position[i + 1] };
        emitworldmeshtriangle(job, face, triangle);
    }
}

static void groupworldmeshranges(worldmeshpacket &packet)
{
    ZoneScopedN("WorldMesh/Group packet ranges");
    packet.sourceranges = packet.ranges.length();
    vector<worldmeshdrawrange> ordered;
    loopv(packet.ranges) if(!packet.ranges[i].alpha) ordered.add(packet.ranges[i]);
    if(ordered.length() > 1)
        ordered.sort(
            [](const worldmeshdrawrange &a, const worldmeshdrawrange &b)
            {
                const worldmeshbatchkey &ka = a, &kb = b;
                return ka == kb ? a.first < b.first : ka < kb;
            });
    // Keep the exact original alpha/refract triangle order, even across keys.
    loopv(packet.ranges) if(packet.ranges[i].alpha) ordered.add(packet.ranges[i]);
    vector<uint> indices;
    indices.reserve(packet.indices.length());
    packet.ranges.setsize(0);
    loopv(ordered)
    {
        const worldmeshdrawrange &source = ordered[i];
        if(!source.count) continue;
        if(packet.ranges.empty() || !(static_cast<const worldmeshbatchkey &>(packet.ranges.last()) == source))
        {
            worldmeshdrawrange &range = packet.ranges.add(source);
            range.first = indices.length();
            range.count = 0;
        }
        indices.put(packet.indices.getbuf() + source.first, source.count);
        packet.ranges.last().count += source.count;
    }
    packet.indices.setsize(0);
    packet.indices.move(indices);
}

static void compactworldmeshvertices(worldmeshpacket &packet)
{
    ZoneScopedN("WorldMesh/Compact vertices");
    TracyPlot("WorldMesh/Packet source vertices", int64_t(packet.vertices.length()));
    // Index identical corners instead of uploading separate vertices for each
    // triangle. Keep texture identity because climate attributes are resolved later.
    const int buckets = 1 << 13;
    vector<int> heads, chain;
    heads.pad(buckets);
    loopi(buckets) heads[i] = -1;
    vector<vertex> vertices;
    vector<ushort> textures;
    vertices.reserve(packet.vertices.length());
    chain.reserve(packet.vertices.length());
    textures.reserve(packet.vertices.length());
    loopv(packet.ranges)
    {
        const worldmeshdrawrange &range = packet.ranges[i];
        loopj(range.count)
        {
            uint &index = packet.indices[range.first + j];
            const vertex &v = packet.vertices[index];
            const uint bucket = hthash(v.pos) & (buckets - 1);
            int found = heads[bucket];
            for(; found >= 0; found = chain[found])
            {
                const vertex &other = vertices[found];
                if(textures[found] == range.texture && other.pos == v.pos && other.tc == v.tc &&
                   other.norm == v.norm && other.tangent == v.tangent) break;
            }
            if(found < 0)
            {
                found = vertices.length();
                vertices.add(v);
                textures.add(range.texture);
                chain.add(heads[bucket]);
                heads[bucket] = found;
            }
            index = found;
        }
    }
    packet.vertices.setsize(0);
    packet.vertices.move(vertices);
    TracyPlot("WorldMesh/Packet indexed vertices", int64_t(packet.vertices.length()));
}

static void buildworldmeshpacket(worldmeshjob &job)
{
    ZoneScopedN("WorldMesh/Build packet");
    vector<worldmeshface> faces;
    collectworldmeshfaces(job, job.snapshot.root, ivec(0, 0, 0), job.snapshot.size / 2, faces);
    faces.sort(worldmeshfaceorder);
    // Rasterize only occupied face planes. Unit precision also handles edit-mode
    // cubes smaller than a terrain block; shaped faces never enter this mask.
    vector<uchar> mask;
    mask.pad(WORLD_SECTION_SIZE * WORLD_SECTION_SIZE);
    for(int first = 0; first < faces.length();)
    {
        if(SDL_AtomicGet(&job.cancelled)) return;
        const worldmeshface &face = faces[first];
        if(!face.axis) { emitworldmeshface(job, face); ++first; continue; }
        const int dim = dimension(face.orient), row = R[dim], col = C[dim], side = WORLD_SECTION_SIZE;
        const ivec &origin = job.snapshot.origin;
        if(face.position[0][dim] < origin[dim] || face.position[0][dim] > origin[dim] + side) { ++first; continue; }
        memset(mask.getbuf(), 0, mask.length());
        int end = first;
        while(end < faces.length() && worldmeshsameplane(face, faces[end]))
        {
            const worldmeshface &f = faces[end++];
            vec minimum(f.position[0]), maximum(f.position[0]);
            loopk(4) { minimum.min(f.position[k]); maximum.max(f.position[k]); }
            const int x0 = max(int(minimum[row]) - origin[row], 0), x1 = min(int(maximum[row]) - origin[row], side),
                      y0 = max(int(minimum[col]) - origin[col], 0), y1 = min(int(maximum[col]) - origin[col], side);
            for(int y = y0; y < y1; ++y) for(int x = x0; x < x1; ++x) mask[y * side + x] = 1;
        }
        loop(y, side) loop(x, side) if(mask[y * side + x])
        {
            int width = 1, height = 1;
            while(x + width < side && mask[y * side + x + width]) ++width;
            while(y + height < side)
            {
                bool full = true;
                loopk(width) if(!mask[(y + height) * side + x + k]) { full = false; break; }
                if(!full) break;
                ++height;
            }
            loopj(height) memset(&mask[(y + j) * side + x], 0, width);
            worldmeshface merged = face;
            // Use the source winding, including the orientation-dependent R/C axes.
            float lowrow = face.position[0][row], highrow = lowrow, lowcol = face.position[0][col], highcol = lowcol;
            loopk(4)
            {
                lowrow = min(lowrow, face.position[k][row]); highrow = max(highrow, face.position[k][row]);
                lowcol = min(lowcol, face.position[k][col]); highcol = max(highcol, face.position[k][col]);
            }
            loopk(4)
            {
                merged.position[k][row] = origin[row] + x + (face.position[k][row] == highrow ? width : 0);
                merged.position[k][col] = origin[col] + y + (face.position[k][col] == highcol ? height : 0);
            }
            emitworldmeshface(job, merged);
        }
        first = end;
    }
    if(SDL_AtomicGet(&job.cancelled)) return;
    groupworldmeshranges(job.packet);
    compactworldmeshvertices(job.packet);
    // Integer culling bounds must contain the small opaque face overlaps.
    job.packet.minimum = ivec(job.snapshot.origin).sub(1);
    job.packet.maximum = ivec(job.snapshot.origin).add(WORLD_SECTION_SIZE + 1);
    if(!SDL_AtomicGet(&job.cancelled))
        buildwatermeshpacket(job.packet.water, job.packet.materials.getbuf(), job.packet.materials.length(),
            [&](const ivec &position)
            {
                ivec origin;
                int size;
                const cube &c = job.snapshot.at(position, 1, origin, size);
                return watergeometrycell(origin, size, (c.material & MATF_VOLUME) == MAT_WATER,
                                         isentirelysolid(c) || isclipped(c.material & MATF_VOLUME),
                                         c.material&(MAT_WATER_SOURCE_MANUAL|MAT_WATER_FLOWING));
            }, true);
}

static int worldmeshworker(void *)
{
    for(;;)
    {
        SDL_LockMutex(worldmeshmutex);
        while(!worldmeshstop && worldmeshjobs.empty()) SDL_CondWait(worldmeshcond, worldmeshmutex);
        if(worldmeshstop) { SDL_UnlockMutex(worldmeshmutex); return 0; }
        worldmeshjob *job = worldmeshjobs.remove(0);
        worldmeshactive = job;
        SDL_UnlockMutex(worldmeshmutex);
        buildworldmeshpacket(*job);
        SDL_LockMutex(worldmeshmutex);
        worldmeshresults.add(job);
        worldmeshactive = NULL;
        SDL_UnlockMutex(worldmeshmutex);
    }
}

static long long worldmeshsectionscore(const ivec &origin, bool edited)
{
    static const long long tierstride = 1LL << 60, editstride = 1LL << 59;
    const vec *focus = player ? &player->o : camera1 ? &camera1->o : NULL;
    if(!focus) return edited ? 0 : editstride;
    const ivec maximum = ivec(origin).add(WORLD_SECTION_SIZE);
    long long distance = 0;
    loopi(3)
    {
        const double delta = (*focus)[i] < origin[i] ? origin[i] - (*focus)[i] :
                             (*focus)[i] > maximum[i] ? (*focus)[i] - maximum[i] : 0;
        distance += static_cast<long long>(delta * delta);
    }
    const int focuschunkx = int(floorf(focus->x / WORLD_CHUNK_SIZE)), focuschunky = int(floorf(focus->y / WORLD_CHUNK_SIZE)),
              sectionchunkx = origin.x / WORLD_CHUNK_SIZE, sectionchunky = origin.y / WORLD_CHUNK_SIZE;
    const bool focuschunk = sectionchunkx == focuschunkx && sectionchunky == focuschunky,
               visible = !viewfrustumvalid() ||
                         isvisiblebb(origin, ivec(WORLD_SECTION_SIZE, WORLD_SECTION_SIZE, WORLD_SECTION_SIZE)) < VFC_FOGGED;
    const int tier = focuschunk ? 0 : visible ? 1 : 2;
    return tier * tierstride + (edited ? 0 : editstride) + min(distance, editstride - 1);
}

static bool worldmeshjobpriority(const worldmeshjob *a, const worldmeshjob *b)
{
    const long long ascore = worldmeshsectionscore(a->snapshot.origin, a->edited),
                    bscore = worldmeshsectionscore(b->snapshot.origin, b->edited);
    if(ascore != bscore) return ascore < bscore;
    const ivec &ao = a->snapshot.origin, &bo = b->snapshot.origin;
    return ao.x != bo.x ? ao.x < bo.x : ao.y != bo.y ? ao.y < bo.y : ao.z < bo.z;
}

static void releaseworldmeshsection(worldmeshsection &section)
{
    const ullong bytes = ullong(section.rendervertices.length()) * sizeof(vertex) + ullong(section.renderindices.length()) * sizeof(uint);
    worldmeshcachedbytes -= bytes;
    worldmeshgarbagebytes += bytes;
    section.rendervertices.setsize(0);
    section.renderindices.setsize(0);
    releasewaterresource(section.water);
    if(section.vertices.buffer) destroyvbo(section.vertices.buffer);
    if(section.indices.buffer) destroyvbo(section.indices.buffer);
}

void clearworldmeshpackets()
{
    ++worldmeshgeneration;
    ++worldmeshepoch;
    if(worldmeshthread)
    {
        SDL_LockMutex(worldmeshmutex);
        worldmeshstop = true;
        if(worldmeshactive) SDL_AtomicSet(&worldmeshactive->cancelled, 1);
        loopv(worldmeshjobs) SDL_AtomicSet(&worldmeshjobs[i]->cancelled, 1);
        SDL_CondBroadcast(worldmeshcond);
        SDL_UnlockMutex(worldmeshmutex);
        SDL_WaitThread(worldmeshthread, NULL);
        worldmeshthread = NULL;
    }
    worldmeshjobs.deletecontents();
    worldmeshresults.deletecontents();
    if(worldmeshcond) { SDL_DestroyCond(worldmeshcond); worldmeshcond = NULL; }
    if(worldmeshmutex) { SDL_DestroyMutex(worldmeshmutex); worldmeshmutex = NULL; }
    loopv(worldmeshsections) { releaseworldmeshsection(*worldmeshsections[i]); delete worldmeshsections[i]; }
    worldmeshsections.setsize(0);
    worldmeshowners.clear();
    closeworldmeshterrainpages();
    worldmeshcachedbytes = worldmeshgarbagebytes = 0;
    worldmeshlastpublish = 0;
    worldmeshcompactqueue.setsize(0);
    worldmeshcompactcursor = 0;
    worldmeshstop = false;
}

static void compactworldmeshpages(Uint64 start, double budget, int uploadlimit, int &uploaded, bool idle)
{
    if(worldmeshcompactqueue.empty())
    {
        // Initial border rebuilds leave dead allocations behind. Wait for mesh
        // work to settle, then relocate published bytes instead of remeshing.
        if(!idle || worldmeshgarbagebytes < max(worldmeshcachedbytes / 2, ullong(1 << 20)) ||
           SDL_GetPerformanceCounter() - worldmeshlastpublish < SDL_GetPerformanceFrequency() / 4) return;
        loopv(worldmeshsections) if(!worldmeshsections[i]->renderindices.empty()) worldmeshcompactqueue.add(worldmeshsections[i]->origin);
        if(worldmeshcompactqueue.empty()) return;
        worldmeshcompactqueue.sort([](const ivec &a, const ivec &b)
        {
            return a.x != b.x ? a.x < b.x : a.y != b.y ? a.y < b.y : a.z < b.z;
        });
        closeworldmeshterrainpages();
        worldmeshgarbagebytes = 0;
        worldmeshcompactcursor = 0;
    }
    ZoneScopedN("WorldMesh/Compact GPU pages");
    while(worldmeshcompactcursor < worldmeshcompactqueue.length())
    {
        if(uploaded >= uploadlimit ||
           (budget >= 0 && (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency() >= budget)) break;
        // Owners can disappear while a multi-frame compaction is in progress.
        worldmeshsection **owner = worldmeshowners.access(worldmeshcompactqueue[worldmeshcompactcursor]);
        if(!owner || (*owner)->dirty || (*owner)->pending || (*owner)->renderindices.empty())
        {
            ++worldmeshcompactcursor;
            continue;
        }
        worldmeshsection &section = **owner;
        const int bytes = section.rendervertices.length() * sizeof(vertex) + section.renderindices.length() * sizeof(uint);
        if(uploaded && bytes > uploadlimit - uploaded) break;
        worldmeshrange vertices, indices;
        uploadworldmeshterrain(vertices, indices, section.rendervertices, section.renderindices);
        // Publish only after both uploads; retire old buffers through the pool's
        // existing reference counts and fences. Water and material data stay put.
        if(section.vertices.buffer) destroyvbo(section.vertices.buffer);
        if(section.indices.buffer) destroyvbo(section.indices.buffer);
        section.vertices = vertices;
        section.indices = indices;
        ++worldmeshgeneration;
        uploaded += bytes;
        ++worldmeshcompactcursor;
    }
    if(worldmeshcompactcursor == worldmeshcompactqueue.length())
    {
        worldmeshcompactqueue.setsize(0);
        worldmeshcompactcursor = 0;
    }
}

void discardworldmeshsection(const ivec &origin)
{
    worldmeshsection **owner = worldmeshowners.access(origin);
    if(!owner) return;
    worldmeshsection *section = *owner;
    ++worldmeshgeneration;
    worldmeshowners.remove(origin);
    worldmeshsections.removeobj(section);
    releaseworldmeshsection(*section);
    delete section;
    if(worldmeshmutex)
    {
        SDL_LockMutex(worldmeshmutex);
        if(worldmeshactive && worldmeshactive->snapshot.origin == origin) SDL_AtomicSet(&worldmeshactive->cancelled, 1);
        loopv(worldmeshjobs) if(worldmeshjobs[i]->snapshot.origin == origin) SDL_AtomicSet(&worldmeshjobs[i]->cancelled, 1);
        SDL_UnlockMutex(worldmeshmutex);
    }
}

static void dirtyworldmeshregion(const ivec &minimum, const ivec &maximum, bool edited = false)
{
    const int size = WORLD_SECTION_SIZE;
    const ivec lo = ivec(minimum).max(0).mask(~(size - 1)), hi = ivec(maximum).min(worldsize);
    for(int z = lo.z; z < hi.z; z += size) for(int y = lo.y; y < hi.y; y += size) for(int x = lo.x; x < hi.x; x += size)
    {
        const ivec origin(x, y, z);
        worldmeshsection **owner = worldmeshowners.access(origin);
        if(!owner)
        {
            if(!worldsectionvaenabled(origin, size)) continue;
            worldmeshsection *section = new worldmeshsection(origin);
            section->edited = edited;
            worldmeshsections.add(section);
            worldmeshowners[origin] = section;
        }
        else
        {
            worldmeshsection &section = **owner;
            ++section.revision;
            section.dirty = true;
            section.edited |= edited;
            if(section.pending && worldmeshmutex)
            {
                SDL_LockMutex(worldmeshmutex);
                if(worldmeshactive && worldmeshactive->request == section.request) SDL_AtomicSet(&worldmeshactive->cancelled, 1);
                loopv(worldmeshjobs) if(worldmeshjobs[i]->request == section.request) SDL_AtomicSet(&worldmeshjobs[i]->cancelled, 1);
                SDL_UnlockMutex(worldmeshmutex);
            }
        }
    }
}

void dirtyworldmeshpackets(const ivec &minimum, const ivec &maximum, bool edited)
{
    if(!worldmeshpackets || !getworldsectionsize() || minimum.x >= maximum.x || minimum.y >= maximum.y || minimum.z >= maximum.z) return;
    dirtyworldmeshregion(minimum, maximum, edited);
    loopi(3)
    {
        ivec lo(minimum), hi(maximum);
        hi[i] = lo[i];
        --lo[i];
        dirtyworldmeshregion(lo, hi, edited);
        lo = minimum;
        hi = maximum;
        lo[i] = hi[i]++;
        dirtyworldmeshregion(lo, hi, edited);
    }
}

int processworldmeshpackets(double budget, int uploadlimit)
{
    if(!worldmeshpackets || !getworldsectionsize() || budget == 0 || uploadlimit <= 0) return 0;
    if(!worldmeshthread)
    {
        worldmeshmutex = SDL_CreateMutex();
        worldmeshcond = SDL_CreateCond();
        if(worldmeshmutex && worldmeshcond) worldmeshthread = SDL_CreateThread(worldmeshworker, "near mesh worker", NULL);
        if(!worldmeshthread) { clearworldmeshpackets(); return 0; }
    }
    const Uint64 start = SDL_GetPerformanceCounter(), frequency = SDL_GetPerformanceFrequency();
    int uploaded = 0, completed = 0;
    for(;;)
    {
        if(budget >= 0 && (SDL_GetPerformanceCounter() - start) * 1000.0 / frequency >= budget) break;
        SDL_LockMutex(worldmeshmutex);
        worldmeshjob *job = worldmeshresults.empty() ? NULL : worldmeshresults[0];
        const int bytes = job ? job->packet.vertices.length() * sizeof(vertex) + job->packet.indices.length() * sizeof(uint) +
                               job->packet.water.vertices.length() * sizeof(watermeshvertex) +
                               job->packet.water.indices.length() * sizeof(uint) +
                               job->packet.water.states.length() * sizeof(watermeshstate) : 0;
        // Allow one oversize packet to make progress; never drain an unbounded queue.
        if(job && (!uploaded || uploaded + bytes <= uploadlimit)) worldmeshresults.remove(0);
        else job = NULL;
        SDL_UnlockMutex(worldmeshmutex);
        if(!job) break;
        worldmeshsection **owner = worldmeshowners.access(job->snapshot.origin);
        if(owner && (*owner)->request == job->request)
        {
            worldmeshsection &section = **owner;
            section.pending = false;
            if(job->epoch == worldmeshepoch && job->revision == section.revision && worldsectionvaenabled(section.origin, WORLD_SECTION_SIZE))
            {
                ZoneScopedN("WorldMesh/Publish packet");
                // Resolve mutable climate settings once, never during drawing.
                vector<uchar> resolved;
                const int numvertices = job->packet.vertices.length();
                if(numvertices > 0) memset(resolved.pad(numvertices), 0, numvertices);
                section.alphapasses = 0;
                loopv(job->packet.ranges)
                {
                    worldmeshdrawrange &range = job->packet.ranges[i];
                    VSlot &slot = lookupvslot(range.texture);
                    if(range.alpha) section.alphapasses |= 2 | (slot.alphaback ? 1 : 0) | (slot.refractscale > 0 ? 4 : 0);
                    range.envmap = slot.slot->shader->type & SHADER_ENVMAP ?
                                   (slot.slot->texmask & (1 << TEX_ENVMAP) ? EMID_CUSTOM :
                                    closestenvmap(range.orient, section.origin, WORLD_SECTION_SIZE)) : EMID_NONE;
                    const int mode = blocktexturemode(slot);
                    ushort textureLayer = 0;
                    if(!range.alpha && mode >= 0 && !slot.slot->sts.empty())
                    {
                        range.texturearray = lookupblocktexturearray(slot.slot->sts[0].t,
                                                                    mode >= 5 ? BLOCKARRAY_CUTOUT : BLOCKARRAY_OPAQUE, textureLayer);
                        if(range.texturearray)
                        {
                            range.arraystate = range.texture;
                            // Canonical uniform state, independent of diffuse texture and climate shader.
                            loopj(range.texture)
                            {
                                VSlot &candidate = lookupvslot(j);
                                if(blocktexturemode(candidate) >= 0 && compatibleblocktexturestate(slot, candidate))
                                {
                                    range.arraystate = j;
                                    break;
                                }
                            }
                        }
                    }
                    const bool climate = terrainclimateslot(slot);
                    loopj(range.count)
                    {
                        const uint index = job->packet.indices[range.first + j];
                        if(resolved[index]) continue;
                        resolved[index] = 1;
                        vertex &v = job->packet.vertices[index];
                        if(climate) v.tc.z = grassclimatevertex(v.pos, v.tc.z < 0);
                        v.textureLayer = textureLayer;
                        v.textureMode = max(mode, 0);
                        v.textureSource = range.texture;
                    }
                }
                // Resolve per-source attributes before coalescing across texture identities.
                const int sourceranges = job->packet.sourceranges;
                groupworldmeshranges(job->packet);
                job->packet.sourceranges = sourceranges;
                worldmeshrange vertices, indices;
                uploadworldmeshterrain(vertices, indices, job->packet.vertices, job->packet.indices);
                releaseworldmeshsection(section);
                section.vertices = vertices;
                section.indices = indices;
                section.rendervertices.move(job->packet.vertices);
                section.renderindices.move(job->packet.indices);
                worldmeshcachedbytes += ullong(section.rendervertices.length()) * sizeof(vertex) +
                                        ullong(section.renderindices.length()) * sizeof(uint);
                worldmeshlastpublish = SDL_GetPerformanceCounter();
                section.ranges.setsize(0);
                section.ranges.move(job->packet.ranges);
                section.materials.setsize(0);
                section.materials.move(job->packet.materials);
                setupworldmeshmaterials(section.materials.getbuf(), section.materials.length());
                uploadwatermeshpacket(section.water, job->packet.water);
                section.minimum = job->packet.minimum;
                section.maximum = job->packet.maximum;
                section.published = section.revision;
                section.sourceranges = job->packet.sourceranges;
                ++worldmeshgeneration;
                TracyPlot("WorldMesh/Packet source ranges", int64_t(section.sourceranges));
                TracyPlot("WorldMesh/Packet grouped ranges", int64_t(section.ranges.length()));
                uploaded += bytes;
                ++completed;
            }
            else section.dirty = true;
        }
        delete job;
        if(uploaded >= uploadlimit) break;
    }
    SDL_LockMutex(worldmeshmutex);
    int outstanding = worldmeshjobs.length() + worldmeshresults.length() + (worldmeshactive ? 1 : 0);
    SDL_UnlockMutex(worldmeshmutex);
    // Camera movement can change priorities while packets are queued. Re-sort
    // every submission pass so only the active worker can remain stale.
    SDL_LockMutex(worldmeshmutex);
    worldmeshjobs.sort(worldmeshjobpriority);
    SDL_UnlockMutex(worldmeshmutex);
    while(outstanding < 8)
    {
        if(budget >= 0 && (SDL_GetPerformanceCounter() - start) * 1000.0 / frequency >= budget) break;
        worldmeshsection *best = NULL;
        long long bestscore = LLONG_MAX;
        loopv(worldmeshsections)
        {
            worldmeshsection &candidate = *worldmeshsections[i];
            if(!candidate.dirty || candidate.pending || !worldsectionvaenabled(candidate.origin, WORLD_SECTION_SIZE)) continue;
            const long long score = worldmeshsectionscore(candidate.origin, candidate.edited);
            if(!best || score < bestscore)
            {
                best = &candidate;
                bestscore = score;
            }
        }
        if(!best) break;
        worldmeshsection &section = *best;
        section.request = ++worldmeshrequest;
        worldmeshjob *job = new worldmeshjob(section.origin, section.revision, worldmeshepoch, section.request, section.edited);
        job->snapshot.capture(worldroot, job->snapshot.root, ivec(0, 0, 0), worldsize / 2);
        section.dirty = false;
        section.pending = true;
        SDL_LockMutex(worldmeshmutex);
        worldmeshjobs.add(job);
        worldmeshjobs.sort(worldmeshjobpriority);
        section.edited = false;
        SDL_CondSignal(worldmeshcond);
        SDL_UnlockMutex(worldmeshmutex);
        ++outstanding;
    }
    bool idle = outstanding == 0;
    if(idle) loopv(worldmeshsections) if(worldmeshsections[i]->dirty && worldsectionvaenabled(worldmeshsections[i]->origin, WORLD_SECTION_SIZE))
    {
        idle = false;
        break;
    }
    compactworldmeshpages(start, budget, uploadlimit, uploaded, idle);
    TracyPlot("WorldMesh/CPU mesh cache bytes", int64_t(worldmeshcachedbytes));
    TracyPlot("WorldMesh/Retired mesh bytes since compaction", int64_t(worldmeshgarbagebytes));
    TracyPlot("WorldMesh/Compaction sections pending", int64_t(worldmeshcompactqueue.length() - worldmeshcompactcursor));
    return completed;
}

bool worldmeshpacketpending(const ivec &origin)
{
    worldmeshsection **owner = worldmeshowners.access(origin);
    return owner && ((*owner)->dirty || (*owner)->pending);
}

const vector<worldmeshsection *> &getworldmeshsections()
{
    return worldmeshsections;
}

ullong getworldmeshgeneration()
{
    return worldmeshgeneration;
}

static void queueworldmeshtree(const cube *family, const ivec &origin, int size)
{
    loopi(8)
    {
        const cube &c = family[i];
        if(!c.children && isempty(c) && !c.material) continue;
        const ivec co(i, origin, size);
        if(size <= WORLD_SECTION_SIZE) dirtyworldmeshregion(co, ivec(co).add(size));
        else if(c.children) queueworldmeshtree(c.children, co, size / 2);
    }
}

void queueworldmeshworld()
{
    if(worldmeshpackets && getworldsectionsize()) queueworldmeshtree(worldroot, ivec(0, 0, 0), worldsize / 2);
}

bool worldmeshsectionvisible(const worldmeshsection &section)
{
    return section.published && worldsectionvavisible(section.origin, WORLD_SECTION_SIZE) &&
           isvisiblebb(section.minimum, ivec(section.maximum).sub(section.minimum)) < VFC_FOGGED;
}

#endif
