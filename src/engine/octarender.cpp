// octarender.cpp: fill vertex arrays with different cube surfaces.

#include "engine.h"

struct vboinfo
{
    int uses;
    uchar *data;
};

hashtable<GLuint, vboinfo> vbos;

VAR(printvbo, 0, 0, 1);
VARFN(vbosize, maxvbosize, 0, 1<<14, 1<<16, allchanged());

enum
{
    VBO_VBUF = 0,
    VBO_EBUF,
    VBO_SKYBUF,
    VBO_DECALBUF,
    NUMVBO
};

static vector<uchar> vbodata[NUMVBO];
static vector<vtxarray *> vbovas[NUMVBO];
static int vbosize[NUMVBO];
static int worldvauploadbytes = 0, worldvauploadvertices = 0;

static void worldmeshbackendchanged();
VARF(worldmeshbackend, 0, 1, 1, worldmeshbackendchanged());

struct worldmeshpage
{
    GLuint vbuf, ebuf;
    uint vertexcapacity, indexcapacity, usedvertices, usedindices;
    vertex *vertices;
    ushort *indices;
    vector<gpuslice> freevertices, freeindices;

    worldmeshpage(uint vertexcapacity, uint indexcapacity) : vbuf(0), ebuf(0),
        vertexcapacity(vertexcapacity), indexcapacity(indexcapacity), usedvertices(0), usedindices(0)
    {
        ZoneScopedN("VoxelMesh/ArenaAlloc");
        vertices = new vertex[vertexcapacity];
        indices = new ushort[indexcapacity];
        freevertices.add(gpuslice(0, vertexcapacity));
        freeindices.add(gpuslice(0, indexcapacity));

        gle::disable();
        glGenBuffers_(1, &vbuf);
        glBindBuffer_(GL_ARRAY_BUFFER, vbuf);
        glBufferData_(GL_ARRAY_BUFFER, vertexcapacity*sizeof(vertex), NULL, GL_STATIC_DRAW);
        glGenBuffers_(1, &ebuf);
        glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, ebuf);
        glBufferData_(GL_ELEMENT_ARRAY_BUFFER, indexcapacity*sizeof(ushort), NULL, GL_STATIC_DRAW);
        glBindBuffer_(GL_ARRAY_BUFFER, 0);
        glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    ~worldmeshpage()
    {
        if(vbuf) glDeleteBuffers_(1, &vbuf);
        if(ebuf) glDeleteBuffers_(1, &ebuf);
        delete[] vertices;
        delete[] indices;
    }
};

// Pages are shared by many VAs. The 65,535-vertex page boundary deliberately
// preserves Cube's GL_UNSIGNED_SHORT index format until base-vertex drawing is
// introduced; freed VA slices can be reused independently inside each page.
static vector<worldmeshpage *> worldmeshpages;
static uint worldmesharenausedvertices = 0, worldmesharenausedindices = 0;
static uint worldmeshpacketvertices = 0, worldmeshpacketindices = 0, worldmeshlargestpacket = 0;
static int worldmeshrebuilds = 0, worldmeshpackets = 0, worldmeshpacketranges = 0;

static uint meshpacketbytes(const meshpacket &packet)
{
    const uint indices = packet.indices.length() + packet.skyindices.length() + packet.decalindices.length();
    return packet.vertices.length()*sizeof(vertex) + indices*sizeof(uint) +
           (packet.ranges.length() + packet.decalranges.length())*sizeof(meshrange) +
           packet.materials.length()*sizeof(materialsurface) + packet.grasstris.length()*sizeof(grasstri);
}

static void recordmeshpacket(const meshpacket &packet)
{
    const uint indices = packet.indices.length() + packet.skyindices.length() + packet.decalindices.length();
    worldmeshpackets++;
    worldmeshpacketvertices += packet.vertices.length();
    worldmeshpacketindices += indices;
    worldmeshpacketranges += packet.ranges.length() + packet.decalranges.length() + (packet.skyindices.empty() ? 0 : 1);
    worldmeshlargestpacket = max(worldmeshlargestpacket, meshpacketbytes(packet));
}

static ullong hashmeshbytes(ullong hash, const void *data, size_t len)
{
    const uchar *bytes = (const uchar *)data;
    loopi(int(len)) hash = (hash ^ bytes[i]) * 1099511628211ULL;
    return hash;
}

template<class T>
static inline ullong hashmeshvalue(ullong hash, const T &value)
{
    return hashmeshbytes(hash, &value, sizeof(value));
}

static ullong hashmeshrange(ullong hash, const meshrange &range)
{
    hash = hashmeshvalue(hash, range.firstindex);
    hash = hashmeshvalue(hash, range.indexcount);
    hash = hashmeshvalue(hash, range.flags);
    hash = hashmeshvalue(hash, range.texture);
    hash = hashmeshvalue(hash, range.material);
    hash = hashmeshvalue(hash, range.envmap);
    hash = hashmeshvalue(hash, range.minvert);
    hash = hashmeshvalue(hash, range.maxvert);
    hash = hashmeshvalue(hash, range.reuse);
    hash = hashmeshvalue(hash, range.orient);
    return hashmeshvalue(hash, range.layer);
}

static ullong hashmaterialsurface(ullong hash, const materialsurface &surface)
{
    hash = hashmeshvalue(hash, surface.o.x);
    hash = hashmeshvalue(hash, surface.o.y);
    hash = hashmeshvalue(hash, surface.o.z);
    hash = hashmeshvalue(hash, surface.csize);
    hash = hashmeshvalue(hash, surface.rsize);
    hash = hashmeshvalue(hash, surface.material);
    hash = hashmeshvalue(hash, surface.skip);
    hash = hashmeshvalue(hash, surface.orient);
    hash = hashmeshvalue(hash, surface.visible);
    return hashmeshvalue(hash, surface.envmap);
}

static int findslice(const vector<gpuslice> &freelist, uint size)
{
    if(!size) return -2;
    loopv(freelist) if(freelist[i].size >= size) return i;
    return -1;
}

static gpuslice takeslice(vector<gpuslice> &freelist, int index, uint size)
{
    if(index == -2) return gpuslice();
    gpuslice result(freelist[index].offset, size);
    freelist[index].offset += size;
    freelist[index].size -= size;
    if(!freelist[index].size) freelist.remove(index);
    return result;
}

static bool sortslice(const gpuslice &a, const gpuslice &b)
{
    return a.offset < b.offset;
}

static void returnslice(vector<gpuslice> &freelist, const gpuslice &slice)
{
    if(!slice.size) return;
    freelist.add(slice);
    freelist.sort(sortslice);
    for(int i = 1; i < freelist.length();)
    {
        gpuslice &prev = freelist[i-1], &cur = freelist[i];
        if(prev.offset + prev.size < cur.offset) { ++i; continue; }
        prev.size = max(prev.size, cur.offset + cur.size - prev.offset);
        freelist.remove(i);
    }
}

static worldmeshpage *allocworldmeshpage(uint numverts, uint numindices, gpuslice &verts, gpuslice &indices)
{
    ZoneScopedN("VoxelMesh/ArenaAlloc");
    loopv(worldmeshpages)
    {
        worldmeshpage *page = worldmeshpages[i];
        int vi = findslice(page->freevertices, numverts), ii = findslice(page->freeindices, numindices);
        if(vi != -1 && ii != -1)
        {
            verts = takeslice(page->freevertices, vi, numverts);
            indices = takeslice(page->freeindices, ii, numindices);
            page->usedvertices += numverts;
            page->usedindices += numindices;
            return page;
        }
    }

    const uint vertexcapacity = max(uint(USHRT_MAX), numverts),
               indexcapacity = max(uint(USHRT_MAX)*4U, numindices);
    worldmeshpage *page = new worldmeshpage(vertexcapacity, indexcapacity);
    worldmeshpages.add(page);
    verts = takeslice(page->freevertices, findslice(page->freevertices, numverts), numverts);
    indices = takeslice(page->freeindices, findslice(page->freeindices, numindices), numindices);
    page->usedvertices = numverts;
    page->usedindices = numindices;
    return page;
}

static void freeworldmesh(gpumesh *mesh);

static gpumesh *uploadworldmesh(const meshpacket &packet)
{
    ZoneScopedN("VoxelMesh/Upload");
    gle::disable();
    ASSERT(packet.vertices.length() <= USHRT_MAX);
    const uint totalindices = packet.indices.length() + packet.skyindices.length() + packet.decalindices.length();
    gpuslice vertices, allindices;
    worldmeshpage *page = allocworldmeshpage(packet.vertices.length(), totalindices, vertices, allindices);
    gpumesh *mesh = new gpumesh;
    mesh->vertices = vertices;
    mesh->indices = gpuslice(allindices.offset, packet.indices.length());
    mesh->skyindices = gpuslice(mesh->indices.offset + mesh->indices.size, packet.skyindices.length());
    mesh->decalindices = gpuslice(mesh->skyindices.offset + mesh->skyindices.size, packet.decalindices.length());
    mesh->numverts = packet.vertices.length();
    mesh->numindices = totalindices;
    mesh->vbuf = page->vbuf;
    mesh->ebuf = page->ebuf;
    mesh->arena = page;
    mesh->checksum = packet.checksum;
    loopv(packet.ranges)
    {
        meshrange range = packet.ranges[i];
        range.firstindex += mesh->indices.offset;
        range.minvert += mesh->vertices.offset;
        range.maxvert += mesh->vertices.offset;
        mesh->ranges.add(range);
    }
    if(packet.skyindices.length())
    {
        meshrange &range = mesh->ranges.add();
        range.firstindex = mesh->skyindices.offset;
        range.indexcount = packet.skyindices.length();
        range.flags = MESH_RANGE_SKY;
        range.minvert = mesh->vertices.offset;
        range.maxvert = mesh->vertices.offset + mesh->vertices.size - 1;
    }
    loopv(packet.decalranges)
    {
        meshrange range = packet.decalranges[i];
        range.firstindex += mesh->decalindices.offset;
        range.minvert += mesh->vertices.offset;
        range.maxvert += mesh->vertices.offset;
        mesh->ranges.add(range);
    }

    if(mesh->numverts)
    {
        memcpy(page->vertices + vertices.offset, packet.vertices.getbuf(), mesh->numverts*sizeof(vertex));
        glBindBuffer_(GL_ARRAY_BUFFER, page->vbuf);
        glBufferSubData_(GL_ARRAY_BUFFER, vertices.offset*sizeof(vertex), mesh->numverts*sizeof(vertex), packet.vertices.getbuf());
    }

    ushort *dst = page->indices + allindices.offset;
#define UPLOADINDICES(src) do \
    { \
        loopv(src) \
        { \
            ASSERT(src[i] + vertices.offset <= USHRT_MAX); \
            dst[i] = ushort(src[i] + vertices.offset); \
        } \
        dst += src.length(); \
    } while(0)
    UPLOADINDICES(packet.indices);
    UPLOADINDICES(packet.skyindices);
    UPLOADINDICES(packet.decalindices);
#undef UPLOADINDICES
    if(totalindices)
    {
        glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, page->ebuf);
        glBufferSubData_(GL_ELEMENT_ARRAY_BUFFER, allindices.offset*sizeof(ushort), totalindices*sizeof(ushort), page->indices + allindices.offset);
    }
    glBindBuffer_(GL_ARRAY_BUFFER, 0);
    glBindBuffer_(GL_ELEMENT_ARRAY_BUFFER, 0);

    worldmesharenausedvertices += mesh->numverts;
    worldmesharenausedindices += mesh->numindices;
    worldvauploadbytes += mesh->numverts*sizeof(vertex) + mesh->numindices*sizeof(ushort);
    worldvauploadvertices += mesh->numverts;
    worldmeshrebuilds++;
    return mesh;
}

static void installworldmesh(vtxarray *va, gpumesh *mesh)
{
    ZoneScopedN("VoxelMesh/Swap");
    gpumesh *oldmesh = va->mesh;
    worldmeshpage *page = (worldmeshpage *)mesh->arena;
    va->mesh = mesh;
    va->meshchecksum = mesh->checksum;
    va->vbuf = mesh->vbuf;
    va->ebuf = va->skybuf = va->decalbuf = mesh->ebuf;
    va->vdata = page->vertices;
    va->edata = va->skydata = va->decaldata = page->indices;
    va->voffset = mesh->vertices.offset;
    va->eoffset = mesh->indices.offset;
    va->skyoffset = mesh->skyindices.offset;
    va->decaloffset = mesh->decalindices.offset;
    va->minvert = mesh->vertices.offset;
    va->maxvert = mesh->vertices.size ? mesh->vertices.offset + mesh->vertices.size - 1 : mesh->vertices.offset;
    if(oldmesh) freeworldmesh(oldmesh);
}

static void freeworldmesh(gpumesh *mesh)
{
    if(!mesh) return;
    ZoneScopedN("VoxelMesh/FreeOld");
    worldmeshpage *page = (worldmeshpage *)mesh->arena;
    if(page)
    {
        returnslice(page->freevertices, mesh->vertices);
        returnslice(page->freeindices, gpuslice(mesh->indices.offset, mesh->numindices));
        page->usedvertices -= mesh->numverts;
        page->usedindices -= mesh->numindices;
    }
    worldmesharenausedvertices -= mesh->numverts;
    worldmesharenausedindices -= mesh->numindices;
    delete mesh;
}

void cleanupworldmesharena()
{
    worldmeshpages.deletecontents();
    worldmesharenausedvertices = worldmesharenausedindices = 0;
}

static void worldmeshstats()
{
    uint vertexcapacity = 0, indexcapacity = 0, freevertices = 0, freeindices = 0,
         largestvertexfree = 0, largestindexfree = 0;
    loopv(worldmeshpages)
    {
        worldmeshpage &page = *worldmeshpages[i];
        vertexcapacity += page.vertexcapacity;
        indexcapacity += page.indexcapacity;
        loopvj(page.freevertices)
        {
            freevertices += page.freevertices[j].size;
            largestvertexfree = max(largestvertexfree, page.freevertices[j].size);
        }
        loopvj(page.freeindices)
        {
            freeindices += page.freeindices[j].size;
            largestindexfree = max(largestindexfree, page.freeindices[j].size);
        }
    }
    const ullong usedbytes = ullong(worldmesharenausedvertices)*sizeof(vertex) + ullong(worldmesharenausedindices)*sizeof(ushort),
                 freebytes = ullong(freevertices)*sizeof(vertex) + ullong(freeindices)*sizeof(ushort);
    const float vertexfragmentation = freevertices ? 1.0f - largestvertexfree/float(freevertices) : 0,
                indexfragmentation = freeindices ? 1.0f - largestindexfree/float(freeindices) : 0;
    conoutf(CON_DEBUG, "world mesh backend %d: %d arena pages, vertices %u/%u, indices %u/%u, largest free %u verts/%u indices",
        worldmeshbackend, worldmeshpages.length(), worldmesharenausedvertices, vertexcapacity,
        worldmesharenausedindices, indexcapacity, largestvertexfree, largestindexfree);
    conoutf(CON_DEBUG, "arena used/free %llu/%llu bytes, fragmentation vertices %.1f%% / indices %.1f%%",
        usedbytes, freebytes, vertexfragmentation*100.0f, indexfragmentation*100.0f);
    conoutf(CON_DEBUG, "mesh packets %d, vertices %u, indices %u, ranges %d, largest packet %u bytes, resident rebuilds %d",
        worldmeshpackets, worldmeshpacketvertices, worldmeshpacketindices, worldmeshpacketranges,
        worldmeshlargestpacket, worldmeshrebuilds);
}
COMMAND(worldmeshstats, "");

struct worldmeshsnapshot
{
    ullong fingerprint;
    uint vas, vertices, indices, ranges;

    worldmeshsnapshot() : fingerprint(0), vas(0), vertices(0), indices(0), ranges(0) {}

    bool operator==(const worldmeshsnapshot &other) const
    {
        return fingerprint == other.fingerprint && vas == other.vas && vertices == other.vertices &&
               indices == other.indices && ranges == other.ranges;
    }
};

static ullong mixmeshchecksum(ullong value)
{
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

static worldmeshsnapshot snapshotworldmeshes()
{
    worldmeshsnapshot snapshot;
    loopv(valist)
    {
        const vtxarray &va = *valist[i];
        const uint worldindices = 3U*(va.tris + va.blendtris + va.alphatris),
                   totalindices = worldindices + va.sky + 3U*va.decaltris,
                   totalranges = va.texs + va.blends + va.alphaback + va.alphafront + va.refract +
                                 va.decaltexs + (va.sky ? 1 : 0);
        ullong nodehash = hashmeshvalue(va.meshchecksum, va.o.x);
        nodehash = hashmeshvalue(nodehash, va.o.y);
        nodehash = hashmeshvalue(nodehash, va.o.z);
        nodehash = hashmeshvalue(nodehash, va.size);
        snapshot.fingerprint ^= mixmeshchecksum(nodehash);
        snapshot.vas++;
        snapshot.vertices += va.verts;
        snapshot.indices += totalindices;
        snapshot.ranges += totalranges;
    }
    return snapshot;
}

static bool validworldmeshfreelist(const vector<gpuslice> &freelist, uint capacity, uint used)
{
    uint free = 0, end = 0;
    loopv(freelist)
    {
        const gpuslice &slice = freelist[i];
        if(!slice.size || slice.offset < end || slice.offset + slice.size > capacity) return false;
        free += slice.size;
        end = slice.offset + slice.size;
    }
    return free + used == capacity;
}

static void worldmeshvalidate()
{
    uint vertices = 0, indices = 0, ranges = 0;
    int metadataerrors = 0, indexerrors = 0, arenaerrors = 0,
        pooled = 0, legacy = 0, empty = 0, boundsmismatches = 0;
    loopv(valist)
    {
        vtxarray &va = *valist[i];
        const uint worldindices = 3U*(va.tris + va.blendtris + va.alphatris),
                   totalindices = worldindices + va.sky + 3U*va.decaltris,
                   totalranges = va.texs + va.blends + va.alphaback + va.alphafront + va.refract +
                                 va.decaltexs + (va.sky ? 1 : 0);
        vertices += va.verts;
        indices += totalindices;
        ranges += totalranges;
        if((worldmeshbackend != 0) != (va.mesh != NULL) && va.verts) metadataerrors++;
        if(va.mesh)
        {
            pooled++;
            if(va.mesh->numverts != uint(va.verts) || va.mesh->numindices != totalindices ||
               va.mesh->ranges.length() != int(totalranges) || va.vbuf != va.mesh->vbuf ||
               va.ebuf != va.mesh->ebuf || va.skybuf != va.mesh->ebuf || va.decalbuf != va.mesh->ebuf ||
               va.meshchecksum != va.mesh->checksum)
                metadataerrors++;

            worldmeshpage *page = (worldmeshpage *)va.mesh->arena;
            if(!page || worldmeshpages.find(page) < 0 || va.mesh->vertices.offset + va.mesh->vertices.size > page->vertexcapacity ||
               va.mesh->indices.offset + va.mesh->numindices > page->indexcapacity ||
               va.mesh->vbuf != page->vbuf || va.mesh->ebuf != page->ebuf)
                arenaerrors++;
            else
            {
                loopj(va.mesh->numindices)
                {
                    const ushort index = page->indices[va.mesh->indices.offset + j];
                    if(index < va.mesh->vertices.offset || index >= va.mesh->vertices.offset + va.mesh->vertices.size)
                    {
                        indexerrors++;
                        break;
                    }
                }
                uint worldcursor = va.mesh->indices.offset, skycursor = va.mesh->skyindices.offset,
                     decalcursor = va.mesh->decalindices.offset;
                loopvj(va.mesh->ranges)
                {
                    const meshrange &range = va.mesh->ranges[j];
                    const gpuslice &slice = range.flags&MESH_RANGE_SKY ? va.mesh->skyindices :
                                               (range.flags&MESH_RANGE_DECAL ? va.mesh->decalindices : va.mesh->indices);
                    uint &cursor = range.flags&MESH_RANGE_SKY ? skycursor :
                                       (range.flags&MESH_RANGE_DECAL ? decalcursor : worldcursor);
                    if(range.firstindex < slice.offset || range.firstindex + range.indexcount > slice.offset + slice.size ||
                       range.firstindex != cursor || range.minvert < va.mesh->vertices.offset ||
                       range.maxvert >= va.mesh->vertices.offset + va.mesh->vertices.size)
                        metadataerrors++;
                    cursor += range.indexcount;
                }
                if(worldcursor != va.mesh->indices.offset + va.mesh->indices.size ||
                   skycursor != va.mesh->skyindices.offset + va.mesh->skyindices.size ||
                   decalcursor != va.mesh->decalindices.offset + va.mesh->decalindices.size)
                    metadataerrors++;
            }
        }
        else if(va.verts) legacy++;
        else empty++;

        if(va.verts && va.vdata)
        {
            vec bbmin(va.o), bbmax(bbmin);
            bbmin.add(va.size);
            loopj(va.verts)
            {
                const vec &pos = va.vdata[va.voffset + j].pos;
                bbmin.min(pos);
                bbmax.max(pos);
            }
            ivec geommin = ivec(bbmin.mul(8)).shr(3), geommax = ivec(bbmax.mul(8)).add(7).shr(3);
            if(geommin != va.geommin || geommax != va.geommax) boundsmismatches++;
        }
    }

    uint usedvertices = 0, usedindices = 0;
    loopv(worldmeshpages)
    {
        const worldmeshpage &page = *worldmeshpages[i];
        usedvertices += page.usedvertices;
        usedindices += page.usedindices;
        if(!validworldmeshfreelist(page.freevertices, page.vertexcapacity, page.usedvertices) ||
           !validworldmeshfreelist(page.freeindices, page.indexcapacity, page.usedindices))
            arenaerrors++;
    }
    if(usedvertices != worldmesharenausedvertices || usedindices != worldmesharenausedindices) arenaerrors++;

    const worldmeshsnapshot snapshot = snapshotworldmeshes();
    conoutf(metadataerrors || indexerrors || arenaerrors || boundsmismatches ? CON_WARN : CON_DEBUG,
        "world mesh validation: %d legacy / %d pooled / %d empty VAs, %u vertices, %u indices, %u ranges, checksum 0x%llx, errors metadata %d/index %d/arena %d/bounds %d",
        legacy, pooled, empty, vertices, indices, ranges, snapshot.fingerprint,
        metadataerrors, indexerrors, arenaerrors, boundsmismatches);
}
COMMAND(worldmeshvalidate, "");

static int residentworldmeshbackend = 1;

static void worldmeshbackendchanged()
{
    const worldmeshsnapshot before = snapshotworldmeshes();
    const int oldbackend = residentworldmeshbackend;
    residentworldmeshbackend = worldmeshbackend;
    allchanged();
    if(!before.vas) return;

    const worldmeshsnapshot after = snapshotworldmeshes();
    const bool matches = before == after;
    conoutf(matches ? CON_DEBUG : CON_WARN,
        "world mesh backend validation %d -> %d: %s; VAs %u/%u, vertices %u/%u, indices %u/%u, ranges %u/%u, checksums 0x%llx/0x%llx",
        oldbackend, worldmeshbackend, matches ? "identical" : "MISMATCH",
        before.vas, after.vas, before.vertices, after.vertices, before.indices, after.indices,
        before.ranges, after.ranges, before.fingerprint, after.fingerprint);
}

void resetworldvauploadstats()
{
    worldvauploadbytes = worldvauploadvertices = 0;
}

void getworldvauploadstats(int &bytes, int &vertices)
{
    bytes = worldvauploadbytes;
    vertices = worldvauploadvertices;
}

void destroyvbo(GLuint vbo)
{
    vboinfo *exists = vbos.access(vbo);
    if(!exists) return;
    vboinfo &vbi = *exists;
    if(vbi.uses <= 0) return;
    vbi.uses--;
    if(!vbi.uses)
    {
        glDeleteBuffers_(1, &vbo);
        if(vbi.data) delete[] vbi.data;
        vbos.remove(vbo);
    }
}

void genvbo(int type, void *buf, int len, vtxarray **vas, int numva)
{
    gle::disable();

    GLuint vbo;
    glGenBuffers_(1, &vbo);
    GLenum target = type==VBO_VBUF ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
    glBindBuffer_(target, vbo);
    glBufferData_(target, len, buf, GL_STATIC_DRAW);
    glBindBuffer_(target, 0);
    worldvauploadbytes += len;
    if(type == VBO_VBUF) worldvauploadvertices += len / int(sizeof(vertex));

    vboinfo &vbi = vbos[vbo];
    vbi.uses = numva;
    vbi.data = new uchar[len];
    memcpy(vbi.data, buf, len);

    if(printvbo) conoutf(CON_DEBUG, "vbo %d: type %d, size %d, %d uses", vbo, type, len, numva);

    loopi(numva)
    {
        vtxarray *va = vas[i];
        switch(type)
        {
            case VBO_VBUF:
                va->vbuf = vbo;
                va->vdata = (vertex *)vbi.data;
                break;
            case VBO_EBUF:
                va->ebuf = vbo;
                va->edata = (ushort *)vbi.data;
                break;
            case VBO_SKYBUF:
                va->skybuf = vbo;
                va->skydata = (ushort *)vbi.data;
                break;
            case VBO_DECALBUF:
                va->decalbuf = vbo;
                va->decaldata = (ushort *)vbi.data;
                break;
        }
    }
}

void flushvbo(int type = -1)
{
    if(type < 0)
    {
        loopi(NUMVBO) flushvbo(i);
        return;
    }

    vector<uchar> &data = vbodata[type];
    if(data.empty()) return;
    vector<vtxarray *> &vas = vbovas[type];
    genvbo(type, data.getbuf(), data.length(), vas.getbuf(), vas.length());
    data.setsize(0);
    vas.setsize(0);
    vbosize[type] = 0;
}

uchar *addvbo(vtxarray *va, int type, int numelems, int elemsize)
{
    switch(type)
    {
        case VBO_VBUF: va->voffset = vbosize[type]; break;
        case VBO_EBUF: va->eoffset = vbosize[type]; break;
        case VBO_SKYBUF: va->skyoffset = vbosize[type]; break;
        case VBO_DECALBUF: va->decaloffset = vbosize[type]; break;
    }

    vbosize[type] += numelems;

    vector<uchar> &data = vbodata[type];
    vector<vtxarray *> &vas = vbovas[type];

    vas.add(va);

    int len = numelems*elemsize;
    uchar *buf = data.reserve(len).buf;
    data.advance(len);
    return buf;
}

struct verthash
{
    static const int SIZE = 1<<13;
    int table[SIZE];
    vector<vertex> verts;
    vector<int> chain;

    verthash() { clearverts(); }

    void clearverts()
    {
        memset(table, -1, sizeof(table));
        chain.setsize(0);
        verts.setsize(0);
    }

    int addvert(const vertex &v)
    {
        uint h = hthash(v.pos)&(SIZE-1);
        for(int i = table[h]; i>=0; i = chain[i])
        {
            const vertex &c = verts[i];
            if(c.pos==v.pos && c.tc==v.tc && c.norm==v.norm && c.tangent==v.tangent)
                 return i;
        }
        if(verts.length() >= USHRT_MAX) return -1;
        verts.add(v);
        chain.add(table[h]);
        return table[h] = verts.length()-1;
    }

    int addvert(const vec &pos, const vec &tc = vec(0, 0, 0), const bvec &norm = bvec(128, 128, 128), const bvec4 &tangent = bvec4(128, 128, 128, 128))
    {
        vertex vtx;
        vtx.pos = pos;
        vtx.tc = tc;
        vtx.norm = norm;
        vtx.tangent = tangent;
        return addvert(vtx);
    }
};

enum
{
    NO_ALPHA = 0,
    ALPHA_BACK,
    ALPHA_FRONT,
    ALPHA_REFRACT
};

struct sortkey
{
    ushort tex, envmap;
    uchar orient, layer, alpha;

    sortkey() {}
    sortkey(ushort tex, uchar orient, uchar layer = LAYER_TOP, ushort envmap = EMID_NONE, uchar alpha = NO_ALPHA)
     : tex(tex), envmap(envmap), orient(orient), layer(layer), alpha(alpha)
    {}

    bool operator==(const sortkey &o) const { return tex==o.tex && envmap==o.envmap && orient==o.orient && layer==o.layer && alpha==o.alpha; }

    static inline bool sort(const sortkey &x, const sortkey &y)
    {
        if(x.alpha < y.alpha) return true;
        if(x.alpha > y.alpha) return false;
        if(x.layer < y.layer) return true;
        if(x.layer > y.layer) return false;
        if(x.tex == y.tex)
        {
            if(x.envmap < y.envmap) return true;
            if(x.envmap > y.envmap) return false;
            if(x.orient < y.orient) return true;
            if(x.orient > y.orient) return false;
            return false;
        }
        VSlot &xs = lookupvslot(x.tex, false), &ys = lookupvslot(y.tex, false);
        if(xs.slot->shader < ys.slot->shader) return true;
        if(xs.slot->shader > ys.slot->shader) return false;
        if(xs.slot->params.length() < ys.slot->params.length()) return true;
        if(xs.slot->params.length() > ys.slot->params.length()) return false;
        if(x.tex < y.tex) return true;
        else return false;
    }
};

static inline bool htcmp(const sortkey &x, const sortkey &y)
{
    return x == y;
}

static inline uint hthash(const sortkey &k)
{
    return k.tex;
}

struct decalkey
{
    ushort tex, envmap, reuse;

    decalkey() {}
    decalkey(ushort tex, ushort envmap = EMID_NONE, ushort reuse = 0)
     : tex(tex), envmap(envmap), reuse(reuse)
    {}

    bool operator==(const decalkey &o) const { return tex==o.tex && envmap==o.envmap && reuse==o.reuse; }

    static inline bool sort(const decalkey &x, const decalkey &y)
    {
        if(x.tex == y.tex)
        {
            if(x.envmap < y.envmap) return true;
            if(x.envmap > y.envmap) return false;
            if(x.reuse < y.reuse) return true;
            else return false;
        }
        DecalSlot &xs = lookupdecalslot(x.tex, false), &ys = lookupdecalslot(y.tex, false);
        if(xs.slot->shader < ys.slot->shader) return true;
        if(xs.slot->shader > ys.slot->shader) return false;
        if(xs.slot->params.length() < ys.slot->params.length()) return true;
        if(xs.slot->params.length() > ys.slot->params.length()) return false;
        if(x.tex < y.tex) return true;
        else return false;
    }
};

static inline bool htcmp(const decalkey &x, const decalkey &y)
{
    return x == y;
}

static inline uint hthash(const decalkey &k)
{
    return k.tex;
}

struct sortval
{
     vector<ushort> tris;

     sortval() {}
};

struct mergedface
{
    uchar orient, numverts;
    ushort mat, tex, envmap;
    vertinfo *verts;
    int tjoints;
};

static thread_local bool worldmeshworkerthread = false;

struct vacollect : verthash
{
    ivec origin;
    int size;
    hashtable<sortkey, sortval> indices;
    hashtable<decalkey, sortval> decalindices;
    vector<ushort> skyindices;
    vector<sortkey> texs;
    vector<decalkey> decaltexs;
    vector<grasstri> grasstris;
    vector<materialsurface> matsurfs;
    vector<octaentities *> mapmodels, decals, extdecals;
    int worldtris, skytris, decaltris;
    vec alphamin, alphamax;
    vec refractmin, refractmax;
    vec skymin, skymax;
    ivec nogimin, nogimax;

    void clear()
    {
        clearverts();
        worldtris = skytris = decaltris = 0;
        indices.clear();
        decalindices.clear();
        skyindices.setsize(0);
        matsurfs.setsize(0);
        mapmodels.setsize(0);
        decals.setsize(0);
        extdecals.setsize(0);
        grasstris.setsize(0);
        texs.setsize(0);
        decaltexs.setsize(0);
        alphamin = refractmin = skymin = vec(1e16f, 1e16f, 1e16f);
        alphamax = refractmax = skymax = vec(-1e16f, -1e16f, -1e16f);
        nogimin = ivec(INT_MAX, INT_MAX, INT_MAX);
        nogimax = ivec(INT_MIN, INT_MIN, INT_MIN);
    }

    void optimize()
    {
        enumeratekt(indices, sortkey, k, sortval, t,
        {
            if(t.tris.length()) texs.add(k);
        });
        texs.sort(sortkey::sort);

        matsurfs.shrink(optimizematsurfs(matsurfs.getbuf(), matsurfs.length()));
    }

#define GENVERTS(type, ptr, body) do \
    { \
        type *f = (type *)ptr; \
        loopv(verts) \
        { \
            const vertex &v = verts[i]; \
            body; \
            f++; \
        } \
    } while(0)

    void genverts(void *buf)
    {
        GENVERTS(vertex, buf, { *f = v; f->norm.flip(); f->tangent.flip(); });
    }

    void gendecal(const extentity &e, DecalSlot &s, const decalkey &key)
    {
        matrix3 orient;
        orient.identity();
        if(e.attr2) orient.rotate_around_z(sincosmod360(e.attr2));
        if(e.attr3) orient.rotate_around_x(sincosmod360(e.attr3));
        if(e.attr4) orient.rotate_around_y(sincosmod360(-e.attr4));
        vec size(max(float(e.attr5), 1.0f));
        size.y *= s.depth;
        if(!s.sts.empty())
        {
            Texture *t = s.sts[0].t;
            if(t->xs < t->ys) size.x *= t->xs / float(t->ys);
            else if(t->xs > t->ys) size.z *= t->ys / float(t->xs);
        }
        vec center = orient.transform(vec(0, size.y*0.5f, 0)).add(e.o), radius = orient.abstransform(vec(size).mul(0.5f));
        vec bbmin = vec(center).sub(radius), bbmax = vec(center).add(radius);
        vec clipoffset = orient.transposedtransform(center).msub(size, 0.5f);
        loopv(texs)
        {
            const sortkey &k = texs[i];
            if(k.layer == LAYER_BLEND || k.alpha != NO_ALPHA) continue;
            const sortval &t = indices[k];
            if(t.tris.empty()) continue;
            decalkey tkey(key);
            if(shouldreuseparams(s, lookupvslot(k.tex, false))) tkey.reuse = k.tex;
            for(int j = 0; j < t.tris.length(); j += 3)
            {
                const vertex &t0 = verts[t.tris[j]], &t1 = verts[t.tris[j+1]], &t2 = verts[t.tris[j+2]];
                vec v0 = t0.pos, v1 = t1.pos, v2 = t2.pos;
                vec tmin = vec(v0).min(v1).min(v2), tmax = vec(v0).max(v1).max(v2);
                if(tmin.x >= bbmax.x || tmin.y >= bbmax.y || tmin.z >= bbmax.z ||
                   tmax.x <= bbmin.x || tmax.y <= bbmin.y || tmax.z <= bbmin.z)
                    continue;
                float f0 = t0.norm.tonormal().dot(orient.b), f1 = t1.norm.tonormal().dot(orient.b), f2 = t2.norm.tonormal().dot(orient.b);
                if(f0 >= 0 && f1 >= 0 && f2 >= 0) continue; 
                vec p1[9], p2[9];
                p1[0] = v0; p1[1] = v1; p1[2] = v2;
                int nump = polyclip(p1, 3, orient.b, clipoffset.y, clipoffset.y + size.y, p2);
                if(nump < 3) continue;
                nump = polyclip(p2, nump, orient.a, clipoffset.x, clipoffset.x + size.x, p1);
                if(nump < 3) continue;
                nump = polyclip(p1, nump, orient.c, clipoffset.z, clipoffset.z + size.z, p2);
                if(nump < 3) continue;

                bvec4 n0 = t0.norm, n1 = t1.norm, n2 = t2.norm,
                      x0 = t0.tangent, x1 = t1.tangent, x2 = t2.tangent;
                vec e1 = vec(v1).sub(v0), e2 = vec(v2).sub(v0);
                float d11 = e1.dot(e1), d12 = e1.dot(e2), d22 = e2.dot(e2);
                int idx[9];
                loopk(nump)
                {
                    vertex v;
                    v.pos = p2[k];
                    vec ep = vec(v.pos).sub(v0);
                    float dp1 = ep.dot(e1), dp2 = ep.dot(e2), denom = d11*d22 - d12*d12,
                          b1 = (d22*dp1 - d12*dp2) / denom,
                          b2 = (d11*dp2 - d12*dp1) / denom,
                          b0 = 1 - b1 - b2;
                    v.norm.lerp(n0, n1, n2, b0, b1, b2);
                    v.norm.w = uchar(127.5f - 127.5f*(f0*b0 + f1*b1 + f2*b2));
                    vec tc = orient.transposedtransform(vec(center).sub(v.pos)).div(size).add(0.5f);
                    v.tc = vec(tc.x, tc.z, s.fade ? tc.y * s.depth / s.fade : 1.0f);
                    v.tangent.lerp(x0, x1, x2, b0, b1, b2);
                    idx[k] = addvert(v);
                }
                vector<ushort> &tris = decalindices[tkey].tris;
                loopk(nump-2) if(idx[0] != idx[k+1] && idx[k+1] != idx[k+2] && idx[k+2] != idx[0])
                {
                    tris.add(idx[0]);
                    tris.add(idx[k+1]);
                    tris.add(idx[k+2]);
                    decaltris += 3;
                }
            }
        }
    }

    void gendecals()
    {
        if(decals.length()) extdecals.put(decals.getbuf(), decals.length());
        if(extdecals.empty()) return;
        vector<extentity *> &ents = entities::getents();
        hashset<int> rendered(1<<8);
        loopv(extdecals)
        {
            octaentities *oe = extdecals[i];
            loopvj(oe->decals)
            {
                const int entity = oe->decals[j];
                if(rendered.access(entity)) continue;
                rendered.add(entity);
                extentity &e = *ents[entity];
                DecalSlot &s = lookupdecalslot(e.attr1, !worldmeshworkerthread);
                if(!s.shader) continue;
                ushort envmap = s.shader->type&SHADER_ENVMAP ? (s.texmask&(1<<TEX_ENVMAP) ? EMID_CUSTOM : closestenvmap(e.o)) : EMID_NONE;
                decalkey k(e.attr1, envmap);
                gendecal(e, s, k);
            }
        }
        enumeratekt(decalindices, decalkey, k, sortval, t,
        {
            if(t.tris.length()) decaltexs.add(k);
        });
        decaltexs.sort(decalkey::sort);
    }

    void finalizepacket(meshpacket &packet)
    {
        ZoneScopedN("VoxelMesh/FinalizePacket");
        optimize();
        gendecals();
        packet.alphamin = alphamin;
        packet.alphamax = alphamax;
        packet.refractmin = refractmin;
        packet.refractmax = refractmax;
        packet.skymin = skymin;
        packet.skymax = skymax;
        packet.nogimin = nogimin;
        packet.nogimax = nogimax;

        if(verts.length())
        {
            vertex *vdata = packet.vertices.pad(verts.length());
            genverts(vdata);
            loopv(packet.vertices)
            {
                packet.bbmin.min(packet.vertices[i].pos);
                packet.bbmax.max(packet.vertices[i].pos);
            }
        }

        loopv(texs)
        {
            const sortkey &k = texs[i];
            const sortval &t = indices[k];
            meshrange &range = packet.ranges.add();
            range.firstindex = packet.indices.length();
            range.indexcount = t.tris.length();
            range.texture = k.tex;
            range.envmap = k.envmap;
            range.orient = k.orient;
            range.layer = k.layer;
            range.minvert = USHRT_MAX;
            range.maxvert = 0;
            if(k.layer == LAYER_BLEND) range.flags |= MESH_RANGE_BLEND;
            if(k.alpha == ALPHA_BACK) range.flags |= MESH_RANGE_ALPHA_BACK;
            else if(k.alpha == ALPHA_FRONT) range.flags |= MESH_RANGE_ALPHA_FRONT;
            else if(k.alpha == ALPHA_REFRACT) range.flags |= MESH_RANGE_REFRACT;
            if(k.alpha != NO_ALPHA) range.material = MAT_ALPHA;
            loopvj(t.tris)
            {
                range.minvert = min(range.minvert, t.tris[j]);
                range.maxvert = max(range.maxvert, t.tris[j]);
            }
            loopvj(t.tris) packet.indices.add(t.tris[j]);
        }
        loopv(skyindices) packet.skyindices.add(skyindices[i]);
        if(matsurfs.length())
        {
            packet.materials.put(matsurfs.getbuf(), matsurfs.length());
            loopv(packet.materials)
            {
                packet.materials[i].skip = 0;
                packet.materials[i].envmap = EMID_NONE;
            }
        }
        if(grasstris.length()) packet.grasstris.put(grasstris.getbuf(), grasstris.length());
        if(mapmodels.length()) packet.mapmodels.put(mapmodels.getbuf(), mapmodels.length());
        if(decals.length()) packet.decals.put(decals.getbuf(), decals.length());

        loopv(decaltexs)
        {
            const decalkey &k = decaltexs[i];
            const sortval &t = decalindices[k];
            meshrange &range = packet.decalranges.add();
            range.firstindex = packet.decalindices.length();
            range.indexcount = t.tris.length();
            range.flags = MESH_RANGE_DECAL;
            range.texture = k.tex;
            range.envmap = k.envmap;
            range.reuse = k.reuse;
            range.minvert = USHRT_MAX;
            range.maxvert = 0;
            loopvj(t.tris)
            {
                range.minvert = min(range.minvert, t.tris[j]);
                range.maxvert = max(range.maxvert, t.tris[j]);
            }
            loopvj(t.tris) packet.decalindices.add(t.tris[j]);
        }

        ullong checksum = 1469598103934665603ULL;
        const uint numverts = packet.vertices.length(), numworldindices = packet.indices.length(),
                   numskyindices = packet.skyindices.length(), numdecalindices = packet.decalindices.length(),
                   numranges = packet.ranges.length(), numdecalranges = packet.decalranges.length(),
                   nummaterials = packet.materials.length(), numgrasstris = packet.grasstris.length();
        checksum = hashmeshvalue(checksum, numverts);
        checksum = hashmeshvalue(checksum, numworldindices);
        checksum = hashmeshvalue(checksum, numskyindices);
        checksum = hashmeshvalue(checksum, numdecalindices);
        checksum = hashmeshvalue(checksum, numranges);
        checksum = hashmeshvalue(checksum, numdecalranges);
        checksum = hashmeshvalue(checksum, nummaterials);
        checksum = hashmeshvalue(checksum, numgrasstris);
        if(numverts) checksum = hashmeshbytes(checksum, packet.vertices.getbuf(), numverts*sizeof(vertex));
        if(numworldindices) checksum = hashmeshbytes(checksum, packet.indices.getbuf(), numworldindices*sizeof(uint));
        if(numskyindices) checksum = hashmeshbytes(checksum, packet.skyindices.getbuf(), numskyindices*sizeof(uint));
        if(numdecalindices) checksum = hashmeshbytes(checksum, packet.decalindices.getbuf(), numdecalindices*sizeof(uint));
        loopv(packet.ranges) checksum = hashmeshrange(checksum, packet.ranges[i]);
        loopv(packet.decalranges) checksum = hashmeshrange(checksum, packet.decalranges[i]);
        loopv(packet.materials) checksum = hashmaterialsurface(checksum, packet.materials[i]);
        if(numgrasstris) checksum = hashmeshbytes(checksum, packet.grasstris.getbuf(), numgrasstris*sizeof(grasstri));
        checksum = hashmeshvalue(checksum, packet.bbmin.x);
        checksum = hashmeshvalue(checksum, packet.bbmin.y);
        checksum = hashmeshvalue(checksum, packet.bbmin.z);
        checksum = hashmeshvalue(checksum, packet.bbmax.x);
        checksum = hashmeshvalue(checksum, packet.bbmax.y);
        checksum = hashmeshvalue(checksum, packet.bbmax.z);
#define HASHVEC(v) do \
        { \
            checksum = hashmeshvalue(checksum, v.x); \
            checksum = hashmeshvalue(checksum, v.y); \
            checksum = hashmeshvalue(checksum, v.z); \
        } while(0)
        HASHVEC(packet.alphamin);
        HASHVEC(packet.alphamax);
        HASHVEC(packet.refractmin);
        HASHVEC(packet.refractmax);
        HASHVEC(packet.skymin);
        HASHVEC(packet.skymax);
        HASHVEC(packet.nogimin);
        HASHVEC(packet.nogimax);
#undef HASHVEC
        packet.checksum = checksum;
    }

    void setupdata(vtxarray *va, meshpacket &packet)
    {
        va->meshchecksum = packet.checksum;
        va->verts = packet.vertices.length();
        va->tris = packet.indices.length()/3;
        va->vbuf = 0;
        va->vdata = 0;
        va->minvert = 0;
        va->maxvert = va->verts-1;
        va->voffset = 0;
        if(va->verts && !worldmeshbackend)
        {
            if(vbosize[VBO_VBUF] + packet.vertices.length() > maxvbosize ||
               vbosize[VBO_EBUF] + packet.indices.length() > USHRT_MAX ||
               vbosize[VBO_SKYBUF] + packet.skyindices.length() > USHRT_MAX ||
               vbosize[VBO_DECALBUF] + packet.decalindices.length() > USHRT_MAX)
                flushvbo();

            uchar *vdata = addvbo(va, VBO_VBUF, va->verts, sizeof(vertex));
            memcpy(vdata, packet.vertices.getbuf(), va->verts*sizeof(vertex));
            va->minvert += va->voffset;
            va->maxvert += va->voffset;
        }

        va->matbuf = NULL;
        va->matsurfs = packet.materials.length();
        va->matmask = 0;
        if(va->matsurfs)
        {
            va->matbuf = new materialsurface[packet.materials.length()];
            loopv(packet.materials) va->matbuf[i] = packet.materials[i];
            loopv(packet.materials)
            {
                materialsurface &m = packet.materials[i];
                if(m.visible == MATSURF_EDIT_ONLY) continue;
                switch(m.material)
                {
                    case MAT_GLASS: case MAT_LAVA: case MAT_WATER: break;
                    default: continue;
                }
                va->matmask |= 1<<m.material;
            }
        }

        va->skybuf = 0;
        va->skydata = 0;
        va->skyoffset = 0;
        va->sky = packet.skyindices.length();
        if(va->sky && !worldmeshbackend)
        {
            ushort *skydata = (ushort *)addvbo(va, VBO_SKYBUF, va->sky, sizeof(ushort));
            loopi(va->sky)
            {
                ASSERT(packet.skyindices[i] + va->voffset <= USHRT_MAX);
                skydata[i] = ushort(packet.skyindices[i] + va->voffset);
            }
        }

        va->texelems = NULL;
        va->texs = packet.ranges.length();
        va->blendtris = 0;
        va->blends = 0;
        va->alphabacktris = 0;
        va->alphaback = 0;
        va->alphafronttris = 0;
        va->alphafront = 0;
        va->refracttris = 0;
        va->refract = 0;
        va->ebuf = 0;
        va->edata = 0;
        va->eoffset = 0;
        va->texmask = 0;
        va->dyntexs = 0;
        va->dynalphatexs = 0;
        if(va->texs)
        {
            va->texelems = new elementset[va->texs];
            ushort *edata = !worldmeshbackend ? (ushort *)addvbo(va, VBO_EBUF, packet.indices.length(), sizeof(ushort)) : NULL,
                   *curbuf = edata;
            loopv(packet.ranges)
            {
                const meshrange &range = packet.ranges[i];
                elementset &e = va->texelems[i];
                e.texture = range.texture;
                e.orient = range.orient;
                e.layer = range.layer;
                e.envmap = range.envmap;
                e.minvert = USHRT_MAX;
                e.maxvert = 0;

                if(range.indexcount)
                {
                    const uint *src = packet.indices.getbuf() + range.firstindex;

                    loopj(range.indexcount)
                    {
                        ASSERT(src[j] + va->voffset <= USHRT_MAX);
                        ushort index = ushort(src[j] + va->voffset);
                        if(curbuf) curbuf[j] = index;
                        e.minvert = min(e.minvert, index);
                        e.maxvert = max(e.maxvert, index);
                    }

                    if(curbuf) curbuf += range.indexcount;
                }
                e.length = range.indexcount;

                if(range.flags&MESH_RANGE_BLEND) { va->texs--; va->tris -= e.length/3; va->blends++; va->blendtris += e.length/3; }
                else if(range.flags&MESH_RANGE_ALPHA_BACK) { va->texs--; va->tris -= e.length/3; va->alphaback++; va->alphabacktris += e.length/3; }
                else if(range.flags&MESH_RANGE_ALPHA_FRONT) { va->texs--; va->tris -= e.length/3; va->alphafront++; va->alphafronttris += e.length/3; }
                else if(range.flags&MESH_RANGE_REFRACT) { va->texs--; va->tris -= e.length/3; va->refract++; va->refracttris += e.length/3; }

                VSlot &vslot = lookupvslot(range.texture, false);
                if(vslot.isdynamic())
                {
                    va->dyntexs++;
                    if(range.flags&(MESH_RANGE_ALPHA_BACK|MESH_RANGE_ALPHA_FRONT|MESH_RANGE_REFRACT)) va->dynalphatexs++;
                }
                Slot &slot = *vslot.slot;
                loopvj(slot.sts) va->texmask |= 1<<slot.sts[j].type;
                if(slot.shader->type&SHADER_ENVMAP) va->texmask |= 1<<TEX_ENVMAP;
            }
        }

        va->alphatris = va->alphabacktris + va->alphafronttris + va->refracttris;

        va->decalbuf = 0;
        va->decaldata = 0;
        va->decaloffset = 0;
        va->decalelems = NULL;
        va->decaltexs = packet.decalranges.length();
        va->decaltris = packet.decalindices.length()/3;
        if(va->decaltexs)
        {
            va->decalelems = new elementset[va->decaltexs];
            ushort *edata = !worldmeshbackend ? (ushort *)addvbo(va, VBO_DECALBUF, packet.decalindices.length(), sizeof(ushort)) : NULL,
                   *curbuf = edata;
            loopv(packet.decalranges)
            {
                const meshrange &range = packet.decalranges[i];
                elementset &e = va->decalelems[i];
                e.texture = range.texture;
                e.reuse = range.reuse;
                e.envmap = range.envmap;
                e.minvert = USHRT_MAX;
                e.maxvert = 0;

                if(range.indexcount)
                {
                    const uint *src = packet.decalindices.getbuf() + range.firstindex;

                    loopj(range.indexcount)
                    {
                        ASSERT(src[j] + va->voffset <= USHRT_MAX);
                        ushort index = ushort(src[j] + va->voffset);
                        if(curbuf) curbuf[j] = index;
                        e.minvert = min(e.minvert, index);
                        e.maxvert = max(e.maxvert, index);
                    }

                    if(curbuf) curbuf += range.indexcount;
                }
                e.length = range.indexcount;
            }
        }

        if(worldmeshbackend && va->verts)
        {
            gpumesh *mesh;
            {
                ZoneScopedN("VoxelMesh/QueueUpload");
                mesh = uploadworldmesh(packet);
            }
            installworldmesh(va, mesh);
            loopi(va->texs + va->blends + va->alphaback + va->alphafront + va->refract)
            {
                elementset &e = va->texelems[i];
                e.minvert += va->voffset;
                e.maxvert += va->voffset;
            }
            loopi(va->decaltexs)
            {
                elementset &e = va->decalelems[i];
                e.minvert += va->voffset;
                e.maxvert += va->voffset;
            }
        }

        vec geommin(va->o), geommax(geommin);
        geommin.add(va->size);
        if(va->verts)
        {
            geommin.min(packet.bbmin);
            geommax.max(packet.bbmax);
        }
        va->geommin = ivec(geommin.mul(8)).shr(3);
        va->geommax = ivec(geommax.mul(8)).add(7).shr(3);
        if(va->alphatris)
        {
            va->alphamin = ivec(vec(packet.alphamin).mul(8)).shr(3);
            va->alphamax = ivec(vec(packet.alphamax).mul(8)).add(7).shr(3);
        }
        if(va->refracttris)
        {
            va->refractmin = ivec(vec(packet.refractmin).mul(8)).shr(3);
            va->refractmax = ivec(vec(packet.refractmax).mul(8)).add(7).shr(3);
        }
        if(va->sky && packet.skymax.x >= 0)
        {
            va->skymin = ivec(vec(packet.skymin).mul(8)).shr(3);
            va->skymax = ivec(vec(packet.skymax).mul(8)).add(7).shr(3);
        }
        va->nogimin = packet.nogimin;
        va->nogimax = packet.nogimax;
        calcmatbb(va, va->o, va->size, packet.materials);

        if(packet.grasstris.length())
        {
            va->grasstris.move(packet.grasstris);
            loadgrassshaders();
        }

        if(packet.mapmodels.length()) va->mapmodels.put(packet.mapmodels.getbuf(), packet.mapmodels.length());
        if(packet.decals.length()) va->decals.put(packet.decals.getbuf(), packet.decals.length());
    }

    void setupdata(vtxarray *va)
    {
        meshpacket packet;
        finalizepacket(packet);
        recordmeshpacket(packet);
        setupdata(va, packet);
    }

    bool emptyva()
    {
        return verts.empty() && matsurfs.empty() && skyindices.empty() && grasstris.empty() && mapmodels.empty() && decals.empty();
    }
};

#define MAXMERGELEVEL 12

// Every invocation of the voxel mesher owns all of its mutable scratch state.
// The main thread uses mainmeshbuildcontext; workers install a scoped context
// before processing a job. No collector, merge bucket, or entity traversal
// state is shared between jobs, so rendercube() can move into the worker once
// immutable octree job inputs are available.
struct meshbuildcontext
{
    vacollect *collector;
    int hasmerges, mergemax, entdepth;
    vector<mergedface> merges[MAXMERGELEVEL+1];
    octaentities *entstack[32];
    bool worker;

    meshbuildcontext(bool worker = false) : collector(new vacollect), hasmerges(0), mergemax(0), entdepth(-1), worker(worker)
    {
        collector->clear();
        memclear(entstack);
    }

    ~meshbuildcontext()
    {
        delete collector;
    }

    void reset()
    {
        collector->clear();
        hasmerges = mergemax = 0;
        entdepth = -1;
        loopi(MAXMERGELEVEL+1) merges[i].setsize(0);
        memclear(entstack);
    }
};

static meshbuildcontext mainmeshbuildcontext;
static thread_local meshbuildcontext *activemeshbuildcontext = NULL;

static inline meshbuildcontext &currentmeshbuildcontext()
{
    return activemeshbuildcontext ? *activemeshbuildcontext : mainmeshbuildcontext;
}

struct scopedmeshbuildcontext
{
    meshbuildcontext *previous;

    scopedmeshbuildcontext(meshbuildcontext &context) : previous(activemeshbuildcontext)
    {
        activemeshbuildcontext = &context;
    }

    ~scopedmeshbuildcontext()
    {
        activemeshbuildcontext = previous;
    }
};

#define vc (*currentmeshbuildcontext().collector)
#define vahasmerges (currentmeshbuildcontext().hasmerges)
#define vamergemax (currentmeshbuildcontext().mergemax)
#define vamerges (currentmeshbuildcontext().merges)
#define entdepth (currentmeshbuildcontext().entdepth)
#define entstack (currentmeshbuildcontext().entstack)

VARP(asyncworldmesh, 0, 0, 1);
VARP(worldmeshthreads, 1, 2, 8);
VARP(worldmeshjoblimit, 16, 128, 4096);
VARP(worldmeshuploadlimit, 1, 4, 64);
VARP(worldmeshuploadbytes, 64<<10, 2<<20, 64<<20);

struct worldmeshjob
{
    meshbuildcontext context;
    meshpacket *packet;
    vtxarray *target;
    uint generation;

    worldmeshjob() : context(true), packet(NULL), target(NULL), generation(0) {}
    ~worldmeshjob() { delete packet; }
};

static vector<worldmeshjob *> worldmeshjobs, worldmeshresults;
static vector<SDL_Thread *> worldmeshworkers;
static SDL_mutex *worldmeshmutex = NULL;
static SDL_cond *worldmeshcond = NULL;
static bool stopworldmeshworkers = false;
static bool acceptworldmeshjobs = false;
static int activeworldmeshjobs = 0;
static uint nextmeshgeneration = 1;

static int worldmeshworker(void *)
{
#ifdef TRACY_ENABLE
    tracy::SetThreadName("World mesh worker");
#endif
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_NORMAL);
    for(;;)
    {
        SDL_LockMutex(worldmeshmutex);
        while(worldmeshjobs.empty() && !stopworldmeshworkers) SDL_CondWait(worldmeshcond, worldmeshmutex);
        if(stopworldmeshworkers)
        {
            SDL_UnlockMutex(worldmeshmutex);
            return 0;
        }
        worldmeshjob *job = worldmeshjobs.remove(0);
        activeworldmeshjobs++;
        SDL_UnlockMutex(worldmeshmutex);

        {
            ZoneScopedN("VoxelMesh/WorkerPacket");
            scopedmeshbuildcontext scope(job->context);
            worldmeshworkerthread = true;
            job->packet = new meshpacket;
            job->context.collector->finalizepacket(*job->packet);
            worldmeshworkerthread = false;
        }

        SDL_LockMutex(worldmeshmutex);
        activeworldmeshjobs--;
        if(stopworldmeshworkers) delete job;
        else worldmeshresults.add(job);
        SDL_CondBroadcast(worldmeshcond);
        SDL_UnlockMutex(worldmeshmutex);
    }
}

static bool startworldmeshworkers()
{
    if(!worldmeshworkers.empty()) return true;
    worldmeshmutex = SDL_CreateMutex();
    worldmeshcond = SDL_CreateCond();
    stopworldmeshworkers = false;
    if(!worldmeshmutex || !worldmeshcond)
    {
        if(worldmeshcond) SDL_DestroyCond(worldmeshcond);
        if(worldmeshmutex) SDL_DestroyMutex(worldmeshmutex);
        worldmeshcond = NULL;
        worldmeshmutex = NULL;
        return false;
    }
    loopi(worldmeshthreads)
    {
        SDL_Thread *worker = SDL_CreateThread(worldmeshworker, "world mesh worker", NULL);
        if(worker) worldmeshworkers.add(worker);
    }
    return !worldmeshworkers.empty();
}

static bool queueworldmeshpacket(vtxarray *target)
{
    if(!acceptworldmeshjobs || !asyncworldmesh || !worldmeshbackend || !startworldmeshworkers()) return false;
    SDL_LockMutex(worldmeshmutex);
    const bool full = worldmeshjobs.length() + worldmeshresults.length() + activeworldmeshjobs >= worldmeshjoblimit;
    SDL_UnlockMutex(worldmeshmutex);
    if(full) return false;
    // Slot loading/linking is render-thread state. Resolve every decal slot
    // before ownership of the collector crosses the worker boundary.
    vector<extentity *> &ents = entities::getents();
    loopv(vc.decals) loopvj(vc.decals[i]->decals)
        lookupdecalslot(ents[vc.decals[i]->decals[j]]->attr1, true);
    loopv(vc.extdecals) loopvj(vc.extdecals[i]->decals)
        lookupdecalslot(ents[vc.extdecals[i]->decals[j]]->attr1, true);
    worldmeshjob *job = new worldmeshjob;
    delete job->context.collector;
    job->context.collector = currentmeshbuildcontext().collector;
    currentmeshbuildcontext().collector = new vacollect;
    currentmeshbuildcontext().collector->clear();
    job->target = target;
    job->generation = target->meshgeneration;
    target->meshpending = true;

    SDL_LockMutex(worldmeshmutex);
    worldmeshjobs.add(job);
    SDL_CondSignal(worldmeshcond);
    SDL_UnlockMutex(worldmeshmutex);
    return true;
}

void setasyncworldmeshing(bool enabled)
{
    acceptworldmeshjobs = enabled;
}

int recalcprogress = 0;
#define progress(s)     if((recalcprogress++&0xFFF)==0) renderprogress(recalcprogress/(float)allocnodes, s);

vector<tjoint> tjoints;

VARFP(filltjoints, 0, 1, 1, allchanged());

void reduceslope(ivec &n)
{
    int mindim = -1, minval = 64;
    loopi(3) if(n[i])
    {
        int val = abs(n[i]);
        if(mindim < 0 || val < minval)
        {
            mindim = i;
            minval = val;
        }
    }
    if(!(n[R[mindim]]%minval) && !(n[C[mindim]]%minval)) n.div(minval);
    while(!((n.x|n.y|n.z)&1)) n.shr(1);
}

// [rotation][orient]
extern const vec orientation_tangent[8][6] =
{
    { vec( 0,  1,  0), vec( 0, -1,  0), vec(-1,  0,  0), vec( 1,  0,  0), vec( 1,  0,  0), vec( 1,  0,  0) },
    { vec( 0,  0, -1), vec( 0,  0, -1), vec( 0,  0, -1), vec( 0,  0, -1), vec( 0, -1,  0), vec( 0,  1,  0) },
    { vec( 0, -1,  0), vec( 0,  1,  0), vec( 1,  0,  0), vec(-1,  0,  0), vec(-1,  0,  0), vec(-1,  0,  0) },
    { vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  1,  0), vec( 0, -1,  0) },
    { vec( 0, -1,  0), vec( 0,  1,  0), vec( 1,  0,  0), vec(-1,  0,  0), vec(-1,  0,  0), vec(-1,  0,  0) },
    { vec( 0,  1,  0), vec( 0, -1,  0), vec(-1,  0,  0), vec( 1,  0,  0), vec( 1,  0,  0), vec( 1,  0,  0) },
    { vec( 0,  0, -1), vec( 0,  0, -1), vec( 0,  0, -1), vec( 0,  0, -1), vec( 0, -1,  0), vec( 0,  1,  0) },
    { vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  1,  0), vec( 0, -1,  0) },
};
extern const vec orientation_bitangent[8][6] =
{
    { vec( 0,  0, -1), vec( 0,  0, -1), vec( 0,  0, -1), vec( 0,  0, -1), vec( 0, -1,  0), vec( 0,  1,  0) },
    { vec( 0, -1,  0), vec( 0,  1,  0), vec( 1,  0,  0), vec(-1,  0,  0), vec(-1,  0,  0), vec(-1,  0,  0) },
    { vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  1,  0), vec( 0, -1,  0) },
    { vec( 0,  1,  0), vec( 0, -1,  0), vec(-1,  0,  0), vec( 1,  0,  0), vec( 1,  0,  0), vec( 1,  0,  0) },
    { vec( 0,  0, -1), vec( 0,  0, -1), vec( 0,  0, -1), vec( 0,  0, -1), vec( 0, -1,  0), vec( 0,  1,  0) },
    { vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  0,  1), vec( 0,  1,  0), vec( 0, -1,  0) },
    { vec( 0,  1,  0), vec( 0, -1,  0), vec(-1,  0,  0), vec( 1,  0,  0), vec( 1,  0,  0), vec( 1,  0,  0) },
    { vec( 0, -1,  0), vec( 0,  1,  0), vec( 1,  0,  0), vec(-1,  0,  0), vec(-1,  0,  0), vec(-1,  0,  0) },
};

void addtris(VSlot &vslot, int orient, const sortkey &key, vertex *verts, int *index, int numverts, int convex, int tj)
{
    int &total = key.tex==DEFAULT_SKY ? vc.skytris : vc.worldtris;
    int edge = orient*(MAXFACEVERTS+1);
    loopi(numverts-2) if(index[0]!=index[i+1] && index[i+1]!=index[i+2] && index[i+2]!=index[0])
    {
        vector<ushort> &idxs = key.tex==DEFAULT_SKY ? vc.skyindices : vc.indices[key].tris;
        int left = index[0], mid = index[i+1], right = index[i+2], start = left, i0 = left, i1 = -1;
        loopk(4)
        {
            int i2 = -1, ctj = -1, cedge = -1;
            switch(k)
            {
            case 1: i1 = i2 = mid; cedge = edge+i+1; break;
            case 2: if(i1 != mid || i0 == left) { i0 = i1; i1 = right; } i2 = right; if(i+1 == numverts-2) cedge = edge+i+2; break;
            case 3: if(i0 == start) { i0 = i1; i1 = left; } i2 = left; // fall-through
            default: if(!i) cedge = edge; break;
            }
            if(i1 != i2)
            {
                if(total + 3 > USHRT_MAX) return;
                total += 3;
                idxs.add(i0);
                idxs.add(i1);
                idxs.add(i2);
                i1 = i2;
            }
            if(cedge >= 0)
            {
                for(ctj = tj;;)
                {
                    if(ctj < 0) break;
                    if(tjoints[ctj].edge < cedge) { ctj = tjoints[ctj].next; continue; }
                    if(tjoints[ctj].edge != cedge) ctj = -1;
                    break;
                }
            }
            if(ctj >= 0)
            {
                int e1 = cedge%(MAXFACEVERTS+1), e2 = (e1+1)%numverts;
                vertex &v1 = verts[e1], &v2 = verts[e2];
                ivec d(vec(v2.pos).sub(v1.pos).mul(8));
                int axis = abs(d.x) > abs(d.y) ? (abs(d.x) > abs(d.z) ? 0 : 2) : (abs(d.y) > abs(d.z) ? 1 : 2);
                if(d[axis] < 0) d.neg();
                reduceslope(d);
                int origin = int(min(v1.pos[axis], v2.pos[axis])*8)&~0x7FFF,
                    offset1 = (int(v1.pos[axis]*8) - origin) / d[axis],
                    offset2 = (int(v2.pos[axis]*8) - origin) / d[axis];
                vec o = vec(v1.pos).sub(vec(d).mul(offset1/8.0f));
                float doffset = 1.0f / (offset2 - offset1);

                if(i1 < 0) for(;;)
                {
                    tjoint &t = tjoints[ctj];
                    if(t.next < 0 || tjoints[t.next].edge != cedge) break;
                    ctj = t.next;
                }
                while(ctj >= 0)
                {
                    tjoint &t = tjoints[ctj];
                    if(t.edge != cedge) break;
                    float offset = (t.offset - offset1) * doffset;
                    vertex vt;
                    vt.pos = vec(d).mul(t.offset/8.0f).add(o);
                    vt.tc.lerp(v1.tc, v2.tc, offset);
                    vt.norm.lerp(v1.norm, v2.norm, offset);
                    vt.tangent.lerp(v1.tangent, v2.tangent, offset);
                    if(v1.tangent.w != v2.tangent.w)
                        vt.tangent.w = orientation_bitangent[vslot.rotation][orient].scalartriple(vt.norm.tonormal(), vt.tangent.tonormal()) < 0 ? 0 : 255;
                    int i2 = vc.addvert(vt);
                    if(i2 < 0) return;
                    if(i1 >= 0)
                    {
                        if(total + 3 > USHRT_MAX) return;
                        total += 3;
                        idxs.add(i0);
                        idxs.add(i1);
                        idxs.add(i2);
                        i1 = i2;
                    }
                    else start = i0 = i2;
                    ctj = t.next;
                }
            }
        }
    }
}

void addgrasstri(int face, vertex *verts, int numv, ushort texture, int layer)
{
    grasstri &g = vc.grasstris.add();
    int i1, i2, i3, i4;
    if(numv <= 3 && face%2) { i1 = face+1; i2 = face+2; i3 = i4 = 0; }
    else { i1 = 0; i2 = face+1; i3 = face+2; i4 = numv > 3 ? face+3 : i3; }
    g.v[0] = verts[i1].pos;
    g.v[1] = verts[i2].pos;
    g.v[2] = verts[i3].pos;
    g.v[3] = verts[i4].pos;
    g.numv = numv;

    g.surface.toplane(g.v[0], g.v[1], g.v[2]);
    if(g.surface.z <= 0) { vc.grasstris.pop(); return; }

    g.minz = min(min(g.v[0].z, g.v[1].z), min(g.v[2].z, g.v[3].z));
    g.maxz = max(max(g.v[0].z, g.v[1].z), max(g.v[2].z, g.v[3].z));

    g.center = vec(0, 0, 0);
    loopk(numv) g.center.add(g.v[k]);
    g.center.div(numv);
    g.radius = 0;
    loopk(numv) g.radius = max(g.radius, g.v[k].dist(g.center));

    g.texture = texture;
    g.blend = layer == LAYER_BLEND ? ((int(g.center.x)>>12)+1) | (((int(g.center.y)>>12)+1)<<8) : 0;
}

static inline void calctexgen(VSlot &vslot, int orient, vec4 &sgen, vec4 &tgen)
{
    Texture *tex = vslot.slot->sts.empty() ? notexture : vslot.slot->sts[0].t;
    const texrotation &r = texrotations[vslot.rotation];
    float k = TEX_SCALE/vslot.scale,
          xs = r.flipx ? -tex->xs : tex->xs,
          ys = r.flipy ? -tex->ys : tex->ys,
          sk = k/xs, tk = k/ys,
          soff = -(r.swapxy ? vslot.offset.y : vslot.offset.x)/xs,
          toff = -(r.swapxy ? vslot.offset.x : vslot.offset.y)/ys;
    sgen = vec4(0, 0, 0, soff);
    tgen = vec4(0, 0, 0, toff);
    if(r.swapxy) switch(orient)
    {
        case 0: sgen.z = -sk; tgen.y = tk;  break;
        case 1: sgen.z = -sk; tgen.y = -tk; break;
        case 2: sgen.z = -sk; tgen.x = -tk; break;
        case 3: sgen.z = -sk; tgen.x = tk;  break;
        case 4: sgen.y = -sk; tgen.x = tk;  break;
        case 5: sgen.y = sk;  tgen.x = tk;  break;
    }
    else switch(orient)
    {
        case 0: sgen.y = sk;  tgen.z = -tk; break;
        case 1: sgen.y = -sk; tgen.z = -tk; break;
        case 2: sgen.x = -sk; tgen.z = -tk; break;
        case 3: sgen.x = sk;  tgen.z = -tk; break;
        case 4: sgen.x = sk;  tgen.y = -tk; break;
        case 5: sgen.x = sk;  tgen.y = tk;  break;
    }
}

ushort encodenormal(const vec &n)
{
    if(n.iszero()) return 0;
    int yaw = int(-atan2(n.x, n.y)/RAD), pitch = int(asin(n.z)/RAD);
    return ushort(clamp(pitch + 90, 0, 180)*360 + (yaw < 0 ? yaw%360 + 360 : yaw%360) + 1);
}

vec decodenormal(ushort norm)
{
    if(!norm) return vec(0, 0, 1);
    norm--;
    const vec2 &yaw = sincos360[norm%360], &pitch = sincos360[norm/360+270];
    return vec(-yaw.y*pitch.x, yaw.x*pitch.x, pitch.y);
}

void guessnormals(const vec *pos, int numverts, vec *normals)
{
    vec n1, n2;
    n1.cross(pos[0], pos[1], pos[2]);
    if(numverts != 4)
    {
        n1.normalize();
        loopk(numverts) normals[k] = n1;
        return;
    }
    n2.cross(pos[0], pos[2], pos[3]);
    if(n1.iszero())
    {
        n2.normalize();
        loopk(4) normals[k] = n2;
        return;
    }
    else n1.normalize();
    if(n2.iszero())
    {
        loopk(4) normals[k] = n1;
        return;
    }
    else n2.normalize();
    vec avg = vec(n1).add(n2).normalize();
    normals[0] = avg;
    normals[1] = n1;
    normals[2] = avg;
    normals[3] = n2;
}

void addcubeverts(VSlot &vslot, int orient, int size, vec *pos, int convex, ushort texture, vertinfo *vinfo, int numverts, int tj = -1, ushort envmap = EMID_NONE, int grassy = 0, bool alpha = false, int layer = LAYER_TOP)
{
    vec4 sgen, tgen;
    calctexgen(vslot, orient, sgen, tgen);
    vertex verts[MAXFACEVERTS];
    int index[MAXFACEVERTS];
    vec normals[MAXFACEVERTS];
    loopk(numverts)
    {
        vertex &v = verts[k];
        v.pos = pos[k];
        v.tc = vec(sgen.dot(v.pos), tgen.dot(v.pos), 0);
        if(vinfo && vinfo[k].norm)
        {
            vec n = decodenormal(vinfo[k].norm), t = orientation_tangent[vslot.rotation][orient];
            t.project(n).normalize();
            v.norm = bvec(n);
            v.tangent = bvec4(bvec(t), orientation_bitangent[vslot.rotation][orient].scalartriple(n, t) < 0 ? 0 : 255);
        }
        else if(texture != DEFAULT_SKY)
        {
            if(!k) guessnormals(pos, numverts, normals);
            const vec &n = normals[k];
            vec t = orientation_tangent[vslot.rotation][orient];
            t.project(n).normalize();
            v.norm = bvec(n);
            v.tangent = bvec4(bvec(t), orientation_bitangent[vslot.rotation][orient].scalartriple(n, t) < 0 ? 0 : 255);
        }
        else
        {
            v.norm = bvec(128, 128, 255);
            v.tangent = bvec4(255, 128, 128, 255);
        }
        index[k] = vc.addvert(v);
        if(index[k] < 0) return;
    }

    if(alpha)
    {
        loopk(numverts) { vc.alphamin.min(pos[k]); vc.alphamax.max(pos[k]); }
        if(vslot.refractscale > 0) loopk(numverts) { vc.refractmin.min(pos[k]); vc.refractmax.max(pos[k]); }
    }
    if(texture == DEFAULT_SKY) loopi(numverts) if(pos[i][orient>>1] != ((orient&1)<<worldscale))
    {       
        loopk(numverts) { vc.skymin.min(pos[k]); vc.skymax.max(pos[k]); }
        break;
    }

    sortkey key(texture, vslot.scroll.iszero() ? O_ANY : orient, layer&LAYER_BOTTOM ? layer : LAYER_TOP, envmap, alpha ? (vslot.refractscale > 0 ? ALPHA_REFRACT : (vslot.alphaback ? ALPHA_BACK : ALPHA_FRONT)) : NO_ALPHA);
    addtris(vslot, orient, key, verts, index, numverts, convex, tj);

    if(grassy)
    {
        for(int i = 0; i < numverts-2; i += 2)
        {
            int faces = 0;
            if(index[0]!=index[i+1] && index[i+1]!=index[i+2] && index[i+2]!=index[0]) faces |= 1;
            if(i+3 < numverts && index[0]!=index[i+2] && index[i+2]!=index[i+3] && index[i+3]!=index[0]) faces |= 2;
            if(grassy > 1 && faces==3) addgrasstri(i, verts, 4, texture, layer);
            else
            {
                if(faces&1) addgrasstri(i, verts, 3, texture, layer);
                if(faces&2) addgrasstri(i+1, verts, 3, texture, layer);
            }
        }
    }
}

struct edgegroup
{
    ivec slope, origin;
    int axis;
};

static inline uint hthash(const edgegroup &g)
{
    return g.slope.x^(g.slope.y<<2)^(g.slope.z<<4)^g.origin.x^g.origin.y^g.origin.z;
}

static inline bool htcmp(const edgegroup &x, const edgegroup &y)
{
    return x.slope==y.slope && x.origin==y.origin;
}

enum
{
    CE_START = 1<<0,
    CE_END   = 1<<1,
    CE_FLIP  = 1<<2,
    CE_DUP   = 1<<3
};

struct cubeedge
{
    cube *c;
    int next, offset;
    ushort size;
    uchar index, flags;
};

vector<cubeedge> cubeedges;
hashtable<edgegroup, int> edgegroups(1<<13);

void gencubeedges(cube &c, const ivec &co, int size)
{
    ivec pos[MAXFACEVERTS];
    int vis;
    loopi(6) if((vis = visibletris(c, i, co, size)))
    {
        int numverts = c.ext ? c.ext->surfaces[i].numverts&MAXFACEVERTS : 0;
        if(numverts)
        {
            vertinfo *verts = c.ext->verts() + c.ext->surfaces[i].verts;
            ivec vo = ivec(co).mask(~0xFFF).shl(3);
            loopj(numverts)
            {
                vertinfo &v = verts[j];
                pos[j] = ivec(v.x, v.y, v.z).add(vo);
            }
        }
        else if(c.merged&(1<<i)) continue;
        else
        {
            ivec v[4];
            genfaceverts(c, i, v);
            int order = vis&4 || (!flataxisface(c, i) && faceconvexity(v) < 0) ? 1 : 0;
            ivec vo = ivec(co).shl(3);
            pos[numverts++] = v[order].mul(size).add(vo);
            if(vis&1) pos[numverts++] = v[order+1].mul(size).add(vo);
            pos[numverts++] = v[order+2].mul(size).add(vo);
            if(vis&2) pos[numverts++] = v[(order+3)&3].mul(size).add(vo);
        }
        loopj(numverts)
        {
            int e1 = j, e2 = j+1 < numverts ? j+1 : 0;
            ivec d = pos[e2];
            d.sub(pos[e1]);
            if(d.iszero()) continue;
            int axis = abs(d.x) > abs(d.y) ? (abs(d.x) > abs(d.z) ? 0 : 2) : (abs(d.y) > abs(d.z) ? 1 : 2);
            if(d[axis] < 0)
            {
                d.neg();
                swap(e1, e2);
            }
            reduceslope(d);

            int t1 = pos[e1][axis]/d[axis],
                t2 = pos[e2][axis]/d[axis];
            edgegroup g;
            g.origin = ivec(pos[e1]).sub(ivec(d).mul(t1));
            g.slope = d;
            g.axis = axis;
            cubeedge ce;
            ce.c = &c;
            ce.offset = t1;
            ce.size = t2 - t1;
            ce.index = i*(MAXFACEVERTS+1)+j;
            ce.flags = CE_START | CE_END | (e1!=j ? CE_FLIP : 0);
            ce.next = -1;

            bool insert = true;
            int *exists = edgegroups.access(g);
            if(exists)
            {
                int prev = -1, cur = *exists;
                while(cur >= 0)
                {
                    cubeedge &p = cubeedges[cur];
                    if(ce.offset <= p.offset+p.size)
                    {
                        if(ce.offset < p.offset) break;
                        if(p.flags&CE_DUP ?
                            ce.offset+ce.size <= p.offset+p.size :
                            ce.offset==p.offset && ce.size==p.size)
                        {
                            p.flags |= CE_DUP;
                            insert = false;
                            break;
                        }
                        if(ce.offset == p.offset+p.size) ce.flags &= ~CE_START;
                    }
                    prev = cur;
                    cur = p.next;
                }
                if(insert)
                {
                    ce.next = cur;
                    while(cur >= 0)
                    {
                        cubeedge &p = cubeedges[cur];
                        if(ce.offset+ce.size==p.offset) { ce.flags &= ~CE_END; break; }
                        cur = p.next;
                    }
                    if(prev>=0) cubeedges[prev].next = cubeedges.length();
                    else *exists = cubeedges.length();
                }
            }
            else edgegroups[g] = cubeedges.length();

            if(insert) cubeedges.add(ce);
        }
    }
}

void gencubeedges(cube *c = worldroot, const ivec &co = ivec(0, 0, 0), int size = worldsize>>1)
{
    progress("fixing t-joints...");
    neighbourstack[++neighbourdepth] = c;
    loopi(8)
    {
        ivec o(i, co, size);
        if(c[i].ext) c[i].ext->tjoints = -1;
        if(c[i].children) gencubeedges(c[i].children, o, size>>1);
        else if(!isempty(c[i])) gencubeedges(c[i], o, size);
    }
    --neighbourdepth;
}

void gencubeverts(cube &c, const ivec &co, int size, int csi)
{
    if(!(c.visible&0xC0)) return;

    int vismask = ~c.merged & 0x3F;
    if(!(c.visible&0x80)) vismask &= c.visible;
    if(!vismask) return;

    int tj = filltjoints && c.ext ? c.ext->tjoints : -1, vis;
    loopi(6) if(vismask&(1<<i) && (vis = visibletris(c, i, co, size)))
    {
        vec pos[MAXFACEVERTS];
        vertinfo *verts = NULL;
        int numverts = c.ext ? c.ext->surfaces[i].numverts&MAXFACEVERTS : 0, convex = 0;
        if(numverts)
        {
            verts = c.ext->verts() + c.ext->surfaces[i].verts;
            vec vo(ivec(co).mask(~0xFFF));
            loopj(numverts) pos[j] = vec(verts[j].getxyz()).mul(1.0f/8).add(vo);
            if(!flataxisface(c, i)) convex = faceconvexity(verts, numverts, size);
        }
        else
        {
            ivec v[4];
            genfaceverts(c, i, v);
            if(!flataxisface(c, i)) convex = faceconvexity(v);
            int order = vis&4 || convex < 0 ? 1 : 0;
            vec vo(co);
            pos[numverts++] = vec(v[order]).mul(size/8.0f).add(vo);
            if(vis&1) pos[numverts++] = vec(v[order+1]).mul(size/8.0f).add(vo);
            pos[numverts++] = vec(v[order+2]).mul(size/8.0f).add(vo);
            if(vis&2) pos[numverts++] = vec(v[(order+3)&3]).mul(size/8.0f).add(vo);
        }

        VSlot &vslot = lookupvslot(c.texture[i], true),
              *layer = vslot.layer && !(c.material&MAT_ALPHA) ? &lookupvslot(vslot.layer, true) : NULL;
        ushort envmap = vslot.slot->shader->type&SHADER_ENVMAP ? (vslot.slot->texmask&(1<<TEX_ENVMAP) ? EMID_CUSTOM : closestenvmap(i, co, size)) : EMID_NONE,
               envmap2 = layer && layer->slot->shader->type&SHADER_ENVMAP ? (layer->slot->texmask&(1<<TEX_ENVMAP) ? EMID_CUSTOM : closestenvmap(i, co, size)) : EMID_NONE;
        while(tj >= 0 && tjoints[tj].edge < i*(MAXFACEVERTS+1)) tj = tjoints[tj].next;
        int hastj = tj >= 0 && tjoints[tj].edge < (i+1)*(MAXFACEVERTS+1) ? tj : -1;
        int grassy = vslot.slot->grass && i!=O_BOTTOM ? (vis!=3 || convex ? 1 : 2) : 0;
        if(!c.ext)
            addcubeverts(vslot, i, size, pos, convex, c.texture[i], NULL, numverts, hastj, envmap, grassy, (c.material&MAT_ALPHA)!=0);
        else
        {
            const surfaceinfo &surf = c.ext->surfaces[i];
            if(!surf.numverts || surf.numverts&LAYER_TOP)
                addcubeverts(vslot, i, size, pos, convex, c.texture[i], verts, numverts, hastj, envmap, grassy, (c.material&MAT_ALPHA)!=0, surf.numverts&LAYER_BLEND);
            if(surf.numverts&LAYER_BOTTOM)
                addcubeverts(layer ? *layer : vslot, i, size, pos, convex, vslot.layer, verts, numverts, hastj, envmap2, 0, false, surf.numverts&LAYER_TOP ? LAYER_BOTTOM : LAYER_TOP);
        }
    }
}

////////// Vertex Arrays //////////////

int allocva = 0;
int wtris = 0, wverts = 0, vtris = 0, vverts = 0, glde = 0, gbatches = 0;
vector<vtxarray *> valist, varoot;

vtxarray *newva(const ivec &o, int size)
{
    vtxarray *va = new vtxarray;
    va->parent = NULL;
    va->o = o;
    va->size = size;
    va->bbdirty = true;
    va->curvfc = VFC_NOT_VISIBLE;
    va->occluded = OCCLUDE_NOTHING;
    va->occludedframe = 0;
    va->query = NULL;
    va->bbmin = va->alphamin = va->refractmin = va->skymin = ivec(-1, -1, -1);
    va->bbmax = va->alphamax = va->refractmax = va->skymax = ivec(-1, -1, -1);
    va->hasmerges = 0;
    va->mergelevel = -1;
    va->mesh = NULL;
    va->meshchecksum = 0;
    va->meshgeneration = nextmeshgeneration++;
    if(!va->meshgeneration) va->meshgeneration = nextmeshgeneration++;
    va->meshpending = false;
    va->vdata = NULL;
    va->edata = va->skydata = va->decaldata = NULL;
    va->vbuf = va->ebuf = va->skybuf = va->decalbuf = 0;
    va->voffset = va->eoffset = va->skyoffset = va->decaloffset = 0;
    va->texelems = va->decalelems = NULL;
    va->matbuf = NULL;
    va->minvert = va->maxvert = 0;
    va->verts = va->tris = va->texs = va->blendtris = va->blends = 0;
    va->alphabacktris = va->alphaback = va->alphafronttris = va->alphafront = 0;
    va->refracttris = va->refract = va->alphatris = va->texmask = 0;
    va->sky = va->matsurfs = va->matmask = va->distance = va->rdistance = 0;
    va->dyntexs = va->dynalphatexs = va->decaltris = va->decaltexs = 0;
    va->oqcontent = 0;
    va->geommin = va->geommax = va->lavamin = va->lavamax = ivec(-1, -1, -1);
    va->watermin = va->watermax = va->glassmin = va->glassmax = ivec(-1, -1, -1);
    va->nogimin = va->nogimax = ivec(-1, -1, -1);
    va->shadowmask = va->shadowtransparent = 0;

    if(!queueworldmeshpacket(va)) vc.setupdata(va);
    va->oqcontent = va->alphatris || va->matmask || !va->mapmodels.empty() || !va->decals.empty();

    wverts += va->verts;
    wtris  += va->tris + va->blends + va->alphatris + va->decaltris;
    allocva++;
    valist.add(va);

    return va;
}

static bool liveworldmeshnode(vtxarray *va, uint generation)
{
    return va && valist.find(va) >= 0 && va->meshgeneration == generation;
}

static void cancelworldmeshjob(vtxarray *va)
{
    if(!va || !worldmeshmutex) return;
    SDL_LockMutex(worldmeshmutex);
    for(int i = worldmeshjobs.length()-1; i >= 0; --i) if(worldmeshjobs[i]->target == va)
        delete worldmeshjobs.remove(i);
    for(int i = worldmeshresults.length()-1; i >= 0; --i) if(worldmeshresults[i]->target == va)
        delete worldmeshresults.remove(i);
    va->meshpending = false;
    va->meshgeneration = nextmeshgeneration++;
    SDL_UnlockMutex(worldmeshmutex);
}

void invalidatevabb(vtxarray *va)
{
    while(va)
    {
        va->bbdirty = true;
        va = va->parent;
    }
}

void destroyva(vtxarray *va, bool reparent)
{
    cancelworldmeshjob(va);
    vtxarray *parent = va->parent;
    wverts -= va->verts;
    wtris -= va->tris + va->blends + va->alphatris + va->decaltris;
    allocva--;
    valist.removeobj(va);
    if(!parent) varoot.removeobj(va);
    if(reparent)
    {
        if(parent) parent->children.removeobj(va);
        loopv(va->children)
        {
            vtxarray *child = va->children[i];
            child->parent = parent;
            if(parent) parent->children.add(child);
            else varoot.add(child);
        }
        invalidatevabb(parent);
    }
    if(va->mesh) freeworldmesh(va->mesh);
    else
    {
        if(va->vbuf) destroyvbo(va->vbuf);
        if(va->ebuf) destroyvbo(va->ebuf);
        if(va->skybuf) destroyvbo(va->skybuf);
        if(va->decalbuf) destroyvbo(va->decalbuf);
    }
    if(va->texelems) delete[] va->texelems;
    if(va->decalelems) delete[] va->decalelems;
    if(va->matbuf) delete[] va->matbuf;
    delete va;
}

static void applyworldmeshresult(worldmeshjob *job)
{
    vtxarray *va = job->target;
    if(!job->packet || !liveworldmeshnode(va, job->generation)) return;

    const int oldverts = va->verts, oldsky = va->sky,
              oldtris = va->tris + va->blends + va->alphatris + va->decaltris;
    if(!va->mesh)
    {
        if(va->vbuf) destroyvbo(va->vbuf);
        if(va->ebuf) destroyvbo(va->ebuf);
        if(va->skybuf) destroyvbo(va->skybuf);
        if(va->decalbuf) destroyvbo(va->decalbuf);
    }
    delete[] va->texelems;
    delete[] va->decalelems;
    delete[] va->matbuf;
    va->texelems = va->decalelems = NULL;
    va->matbuf = NULL;
    va->grasstris.setsize(0);
    va->mapmodels.setsize(0);
    va->decals.setsize(0);

    recordmeshpacket(*job->packet);
    vc.setupdata(va, *job->packet);
    va->meshpending = false;
    va->oqcontent = va->alphatris || va->matmask || !va->mapmodels.empty() || !va->decals.empty();
    wverts += va->verts - oldverts;
    wtris += va->tris + va->blends + va->alphatris + va->decaltris - oldtris;
    explicitsky += va->sky - oldsky;
    int index = valist.find(va);
    if(index >= 0) setupmaterials(index, index+1);
    invalidatevabb(va);
}

void processworldmeshuploads(bool drain)
{
    if(!worldmeshmutex) return;
    ZoneScopedN("VoxelMesh/ProcessCompleted");
    const uint bytebudget = drain ? UINT_MAX : uint(worldmeshuploadbytes);
    const int countbudget = drain ? INT_MAX : worldmeshuploadlimit;
    uint uploaded = 0;
    int handled = 0;
    for(;;)
    {
        worldmeshjob *job = NULL;
        SDL_LockMutex(worldmeshmutex);
        if(worldmeshresults.empty() && drain && (!worldmeshjobs.empty() || activeworldmeshjobs))
        {
            SDL_CondWait(worldmeshcond, worldmeshmutex);
        }
        if(!worldmeshresults.empty())
        {
            worldmeshjob *candidate = worldmeshresults[0];
            uint bytes = candidate->packet ? meshpacketbytes(*candidate->packet) : 0;
            if(handled < countbudget && (!handled || uploaded + bytes <= bytebudget))
            {
                job = worldmeshresults.remove(0);
                uploaded += bytes;
                handled++;
            }
        }
        bool finished = worldmeshjobs.empty() && !activeworldmeshjobs && worldmeshresults.empty();
        SDL_UnlockMutex(worldmeshmutex);

        if(job)
        {
            applyworldmeshresult(job);
            delete job;
            continue;
        }
        if(!drain || finished || handled >= countbudget || uploaded >= bytebudget) break;
    }
    if(handled)
    {
        clearshadowcache();
        updatevabbs();
    }
    SDL_LockMutex(worldmeshmutex);
    const int pendingcpu = worldmeshjobs.length() + activeworldmeshjobs,
              pendinguploads = worldmeshresults.length();
    SDL_UnlockMutex(worldmeshmutex);
    (void)pendingcpu;
    (void)pendinguploads;
    TracyPlot("VoxelMesh/Completed uploads", int64_t(handled));
    TracyPlot("VoxelMesh/Pending CPU jobs", int64_t(pendingcpu));
    TracyPlot("VoxelMesh/Pending uploads", int64_t(pendinguploads));
}

void flushworldmeshjobs()
{
    processworldmeshuploads(true);
}

void shutdownworldmeshworkers()
{
    if(!worldmeshmutex) return;
    SDL_LockMutex(worldmeshmutex);
    stopworldmeshworkers = true;
    SDL_CondBroadcast(worldmeshcond);
    SDL_UnlockMutex(worldmeshmutex);
    loopv(worldmeshworkers) SDL_WaitThread(worldmeshworkers[i], NULL);
    worldmeshworkers.setsize(0);
    loopv(worldmeshjobs) delete worldmeshjobs[i];
    loopv(worldmeshresults) delete worldmeshresults[i];
    worldmeshjobs.setsize(0);
    worldmeshresults.setsize(0);
    SDL_DestroyCond(worldmeshcond);
    SDL_DestroyMutex(worldmeshmutex);
    worldmeshcond = NULL;
    worldmeshmutex = NULL;
    activeworldmeshjobs = 0;
    stopworldmeshworkers = false;
}

void clearvas(cube *c)
{
    loopi(8)
    {
        if(c[i].ext)
        {
            if(c[i].ext->va) destroyva(c[i].ext->va, false);
            c[i].ext->va = NULL;
            c[i].ext->tjoints = -1;
        }
        if(c[i].children) clearvas(c[i].children);
    }
}

ivec worldmin(0, 0, 0), worldmax(0, 0, 0), nogimin(0, 0, 0), nogimax(0, 0, 0);

void updatevabb(vtxarray *va, bool force)
{
    if(!force && !va->bbdirty) return;

    va->oqcontent = va->alphatris || va->matmask || !va->mapmodels.empty() || !va->decals.empty();
    va->bbmin = va->geommin;
    va->bbmax = va->geommax;
    va->bbmin.min(va->lavamin);
    va->bbmax.max(va->lavamax);
    va->bbmin.min(va->watermin);
    va->bbmax.max(va->watermax);
    va->bbmin.min(va->glassmin);
    va->bbmax.max(va->glassmax);
    loopv(va->children)
    {
        vtxarray *child = va->children[i];
        updatevabb(child, force);
        va->oqcontent |= child->oqcontent;
        va->bbmin.min(child->bbmin);
        va->bbmax.max(child->bbmax);
    }
    loopv(va->mapmodels)
    {
        octaentities *oe = va->mapmodels[i];
        va->bbmin.min(oe->bbmin);
        va->bbmax.max(oe->bbmax);
    }
    loopv(va->decals)
    {
        octaentities *oe = va->decals[i];
        va->bbmin.min(oe->bbmin);
        va->bbmax.max(oe->bbmax);
    }
    va->bbmin.max(va->o);
    va->bbmax.min(ivec(va->o).add(va->size));
    worldmin.min(va->bbmin);
    worldmax.max(va->bbmax);
    nogimin.min(va->nogimin);
    nogimax.max(va->nogimax);
    va->bbdirty = false;
}

void updatevabbs(bool force)
{
    worldmin = nogimin = ivec(worldsize, worldsize, worldsize);
    worldmax = nogimax = ivec(0, 0, 0);
    loopv(varoot) updatevabb(varoot[i], force);
    if(!force)
    {
        // Cached VAs return early from updatevabb(). Their aggregate bounds are
        // still valid, so fold them into the world bounds without recomputing
        // any unrelated VA hierarchy.
        loopv(varoot)
        {
            worldmin.min(varoot[i]->bbmin);
            worldmax.max(varoot[i]->bbmax);
        }
        loopv(valist)
        {
            nogimin.min(valist[i]->nogimin);
            nogimax.max(valist[i]->nogimax);
        }
    }
    if(worldmin.x >= worldmax.x)
    {
        worldmin = ivec(0, 0, 0);
        worldmax = ivec(worldsize, worldsize, worldsize);
    }
}

int genmergedfaces(cube &c, const ivec &co, int size, int minlevel = -1)
{
    if(!c.ext || isempty(c)) return -1;
    int tj = c.ext->tjoints, maxlevel = -1;
    loopi(6) if(c.merged&(1<<i))
    {
        surfaceinfo &surf = c.ext->surfaces[i];
        int numverts = surf.numverts&MAXFACEVERTS;
        if(!numverts)
        {
            if(minlevel < 0) vahasmerges |= MERGE_PART;
            continue;
        }
        mergedface mf;
        mf.orient = i;
        mf.mat = c.material;
        mf.tex = c.texture[i];
        mf.envmap = EMID_NONE;
        mf.numverts = surf.numverts;
        mf.verts = c.ext->verts() + surf.verts;
        mf.tjoints = -1;
        int level = calcmergedsize(i, co, size, mf.verts, mf.numverts&MAXFACEVERTS);
        if(level > minlevel)
        {
            maxlevel = max(maxlevel, level);

            while(tj >= 0 && tjoints[tj].edge < i*(MAXFACEVERTS+1)) tj = tjoints[tj].next;
            if(tj >= 0 && tjoints[tj].edge < (i+1)*(MAXFACEVERTS+1)) mf.tjoints = tj;

            VSlot &vslot = lookupvslot(mf.tex, true),
                  *layer = vslot.layer && !(c.material&MAT_ALPHA) ? &lookupvslot(vslot.layer, true) : NULL;
            if(vslot.slot->shader->type&SHADER_ENVMAP)
                mf.envmap = vslot.slot->texmask&(1<<TEX_ENVMAP) ? EMID_CUSTOM : closestenvmap(i, co, size);
            ushort envmap2 = layer && layer->slot->shader->type&SHADER_ENVMAP ? (layer->slot->texmask&(1<<TEX_ENVMAP) ? EMID_CUSTOM : closestenvmap(i, co, size)) : EMID_NONE;

            if(surf.numverts&LAYER_TOP) vamerges[level].add(mf);
            if(surf.numverts&LAYER_BOTTOM)
            {
                mf.tex = vslot.layer;
                mf.envmap = envmap2;
                mf.numverts &= ~LAYER_BLEND;
                mf.numverts |= surf.numverts&LAYER_TOP ? LAYER_BOTTOM : LAYER_TOP;
                vamerges[level].add(mf);
            }
        }
    }
    if(maxlevel >= 0)
    {
        vamergemax = max(vamergemax, maxlevel);
        vahasmerges |= MERGE_ORIGIN;
    }
    return maxlevel;
}

int findmergedfaces(cube &c, const ivec &co, int size, int csi, int minlevel)
{
    if(c.ext && c.ext->va && !(c.ext->va->hasmerges&MERGE_ORIGIN)) return c.ext->va->mergelevel;
    else if(c.children)
    {
        int maxlevel = -1;
        loopi(8)
        {
            ivec o(i, co, size/2);
            int level = findmergedfaces(c.children[i], o, size/2, csi-1, minlevel);
            maxlevel = max(maxlevel, level);
        }
        return maxlevel;
    }
    else if(c.ext && c.merged) return genmergedfaces(c, co, size, minlevel);
    else return -1;
}

void addmergedverts(int level, const ivec &o)
{
    vector<mergedface> &mfl = vamerges[level];
    if(mfl.empty()) return;
    vec vo(ivec(o).mask(~0xFFF));
    vec pos[MAXFACEVERTS];
    loopv(mfl)
    {
        mergedface &mf = mfl[i];
        int numverts = mf.numverts&MAXFACEVERTS;
        loopi(numverts)
        {
            vertinfo &v = mf.verts[i];
            pos[i] = vec(v.x, v.y, v.z).mul(1.0f/8).add(vo);
        }
        VSlot &vslot = lookupvslot(mf.tex, true);
        int grassy = vslot.slot->grass && mf.orient!=O_BOTTOM && mf.numverts&LAYER_TOP ? 2 : 0;
        addcubeverts(vslot, mf.orient, 1<<level, pos, 0, mf.tex, mf.verts, numverts, mf.tjoints, mf.envmap, grassy, (mf.mat&MAT_ALPHA)!=0, mf.numverts&LAYER_BLEND);
        vahasmerges |= MERGE_USE;
    }
    mfl.setsize(0);
}

static inline void finddecals(vtxarray *va)
{
    if(va->hasmerges&(MERGE_ORIGIN|MERGE_PART))
    {
        loopv(va->decals) vc.extdecals.add(va->decals[i]);
        loopv(va->children) finddecals(va->children[i]);
    }
}

void rendercube(cube &c, const ivec &co, int size, int csi, int &maxlevel) // creates vertices and indices ready to be put into a va
{
    //if(size<=16) return;
    if(c.ext && c.ext->va)
    {
        maxlevel = max(maxlevel, c.ext->va->mergelevel);
        finddecals(c.ext->va);
        return; // don't re-render
    }

    if(c.children)
    {
        neighbourstack[++neighbourdepth] = c.children;
        c.escaped = 0;
        loopi(8)
        {
            ivec o(i, co, size/2);
            int level = -1;
            rendercube(c.children[i], o, size/2, csi-1, level);
            if(level >= csi)
                c.escaped |= 1<<i;
            maxlevel = max(maxlevel, level);
        }
        --neighbourdepth;

        if(csi <= MAXMERGELEVEL && vamerges[csi].length()) addmergedverts(csi, co);

        if(c.ext && c.ext->ents)
        {
            if(c.ext->ents->mapmodels.length()) vc.mapmodels.add(c.ext->ents);
            if(c.ext->ents->decals.length()) vc.decals.add(c.ext->ents);
        }
        return;
    }

    if(!isempty(c))
    {
        gencubeverts(c, co, size, csi);
        if(c.merged) maxlevel = max(maxlevel, genmergedfaces(c, co, size));
    }
    if(c.material != MAT_AIR)
    {
        genmatsurfs(c, co, size, vc.matsurfs);
        if(c.material&MAT_NOGI)
        {
            vc.nogimin.min(co);
            vc.nogimax.max(ivec(co).add(size));
        }
    }

    if(c.ext && c.ext->ents)
    {
        if(c.ext->ents->mapmodels.length()) vc.mapmodels.add(c.ext->ents);
        if(c.ext->ents->decals.length()) vc.decals.add(c.ext->ents);
    }

    if(csi <= MAXMERGELEVEL && vamerges[csi].length()) addmergedverts(csi, co);
}

void setva(cube &c, const ivec &co, int size, int csi, bool force = false)
{
    ZoneScopedN("VoxelMesh/Build");
    ASSERT(size <= 0x1000);

    int vamergeoffset[MAXMERGELEVEL+1];
    loopi(MAXMERGELEVEL+1) vamergeoffset[i] = vamerges[i].length();

    vc.origin = co;
    vc.size = size;

    loopi(entdepth+1)
    {
        octaentities *oe = entstack[i];
        if(oe->decals.length()) vc.extdecals.add(oe);
    }

    int maxlevel = -1;
    {
        ZoneScopedN("VoxelMesh/RenderCube");
        rendercube(c, co, size, csi, maxlevel);
    }

    if(force || size == min(0x1000, worldsize/2) || !vc.emptyva())
    {
        vtxarray *va = newva(co, size);
        ext(c).va = va;
        va->hasmerges = vahasmerges;
        va->mergelevel = vamergemax;
    }
    else
    {
        loopi(MAXMERGELEVEL+1) vamerges[i].setsize(vamergeoffset[i]);
    }

    vc.clear();
}

static inline int setcubevisibility(cube &c, const ivec &co, int size)
{
    if(isempty(c) && (c.material&MATF_CLIP) != MAT_CLIP) return 0;
    int numvis = 0, vismask = 0, collidemask = 0, checkmask = 0;
    loopi(6)
    {
        int facemask = classifyface(c, i, co, size);
        if(facemask&1)
        {
            vismask |= 1<<i;
            if(c.merged&(1<<i))
            {
                if(c.ext && c.ext->surfaces[i].numverts&MAXFACEVERTS) numvis++;
            }
            else
            {
                numvis++;
                if(c.texture[i] != DEFAULT_SKY && !(c.ext && c.ext->surfaces[i].numverts&MAXFACEVERTS)) checkmask |= 1<<i;
            }
        }
        if(facemask&2) collidemask |= 1<<i;
    }
    c.visible = collidemask | (vismask ? (vismask != collidemask ? (checkmask ? 0x80|0x40 : 0x80) : 0x40) : 0);
    return numvis;
}

VARF(vafacemax, 64, 384, 256*256, allchanged());
VARF(vafacemin, 0, 96, 256*256, allchanged());
VARF(vacubesize, 32, 128, 0x1000, allchanged());
// MAXFACEVERTS is 15, so 4096 post-merge faces remain below the 65,535 vertex
// packet ceiling even for pathological carved geometry. Stable planar terrain
// is counted after genmerges and therefore stays far below this fallback.
VARF(worldmeshdomainsplitfaces, 1024, 4096, 4096, allchanged());

int updateva(cube *c, const ivec &co, int size, int csi,
             int worldsectionsize, int facemax, int maxvasize)
{
    progress("recalculating geometry...");
    int ccount = 0, cmergemax = vamergemax, chasmerges = vahasmerges;
    neighbourstack[++neighbourdepth] = c;
    loopi(8)                                    // counting number of semi-solid/solid children cubes
    {
        int count = 0, childpos = varoot.length();
        ivec o(i, co, size);
        vamergemax = 0;
        vahasmerges = 0;
        if(worldsectionsize > 0 && size == worldsectionsize && !worldsectionvaenabled(o, size)) continue;
        if(c[i].ext && c[i].ext->va)
        {
            varoot.add(c[i].ext->va);
            if(c[i].ext->va->hasmerges&MERGE_ORIGIN) findmergedfaces(c[i], o, size, csi, csi);
        }
        else
        {
            if(c[i].children)
            {
                if(c[i].ext && c[i].ext->ents) entstack[++entdepth] = c[i].ext->ents;
                count += updateva(c[i].children, o, size/2, csi-1,
                                  worldsectionsize, facemax, maxvasize);
                if(c[i].ext && c[i].ext->ents) --entdepth;
            }
            else count += setcubevisibility(c[i], o, size);
            int tcount = count + (csi <= MAXMERGELEVEL ? vamerges[csi].length() : 0);
            bool makegroup = worldsectionsize > 0 && size >= worldsectionsize &&
                             size <= maxvasize &&
                             (tcount > 0 || varoot.length() > childpos);
            const bool domainoverflow = worldsectionsize > 0 && size < worldsectionsize &&
                                        tcount > worldmeshdomainsplitfaces;
            if((!worldsectionsize && tcount > facemax) || domainoverflow || makegroup ||
               (!worldsectionsize && tcount >= vafacemin && size >= vacubesize) ||
               size == maxvasize)
            {
                loadprogress = clamp(recalcprogress/float(allocnodes), 0.0f, 1.0f);
                setva(c[i], o, size, csi, makegroup);
                if(c[i].ext && c[i].ext->va)
                {
                    while(varoot.length() > childpos)
                    {
                        vtxarray *child = varoot.pop();
                        c[i].ext->va->children.add(child);
                        child->parent = c[i].ext->va;
                    }
                    varoot.add(c[i].ext->va);
                    if(vamergemax > size)
                    {
                        cmergemax = max(cmergemax, vamergemax);
                        chasmerges |= vahasmerges&~MERGE_USE;
                    }
                    continue;
                }
                else count = 0;
            }
        }
        if(csi+1 <= MAXMERGELEVEL && vamerges[csi].length()) vamerges[csi+1].move(vamerges[csi]);
        cmergemax = max(cmergemax, vamergemax);
        chasmerges |= vahasmerges;
        ccount += count;
    }
    --neighbourdepth;
    vamergemax = cmergemax;
    vahasmerges = chasmerges;

    return ccount;
}

void addtjoint(const edgegroup &g, const cubeedge &e, int offset)
{
    int vcoord = (g.slope[g.axis]*offset + g.origin[g.axis]) & 0x7FFF;
    tjoint &tj = tjoints.add();
    tj.offset = vcoord / g.slope[g.axis];
    tj.edge = e.index;

    int prev = -1, cur = ext(*e.c).tjoints;
    while(cur >= 0)
    {
        tjoint &o = tjoints[cur];
        if(tj.edge < o.edge || (tj.edge==o.edge && (e.flags&CE_FLIP ? tj.offset > o.offset : tj.offset < o.offset))) break;
        prev = cur;
        cur = o.next;
    }

    tj.next = cur;
    if(prev < 0) e.c->ext->tjoints = tjoints.length()-1;
    else tjoints[prev].next = tjoints.length()-1;
}

void findtjoints(int cur, const edgegroup &g)
{
    int active = -1;
    while(cur >= 0)
    {
        cubeedge &e = cubeedges[cur];
        int prevactive = -1, curactive = active;
        while(curactive >= 0)
        {
            cubeedge &a = cubeedges[curactive];
            if(a.offset+a.size <= e.offset)
            {
                if(prevactive >= 0) cubeedges[prevactive].next = a.next;
                else active = a.next;
            }
            else
            {
                prevactive = curactive;
                if(!(a.flags&CE_DUP))
                {
                    if(e.flags&CE_START && e.offset > a.offset && e.offset < a.offset+a.size)
                        addtjoint(g, a, e.offset);
                    if(e.flags&CE_END && e.offset+e.size > a.offset && e.offset+e.size < a.offset+a.size)
                        addtjoint(g, a, e.offset+e.size);
                }
                if(!(e.flags&CE_DUP))
                {
                    if(a.flags&CE_START && a.offset > e.offset && a.offset < e.offset+e.size)
                        addtjoint(g, e, a.offset);
                    if(a.flags&CE_END && a.offset+a.size > e.offset && a.offset+a.size < e.offset+e.size)
                        addtjoint(g, e, a.offset+a.size);
                }
            }
            curactive = a.next;
        }
        int next = e.next;
        e.next = active;
        active = cur;
        cur = next;
    }
}

void findtjoints()
{
    recalcprogress = 0;
    gencubeedges();
    tjoints.setsize(0);
    enumeratekt(edgegroups, edgegroup, g, int, e, findtjoints(e, g));
    cubeedges.setsize(0);
    edgegroups.clear();
}

void octarender()                               // creates va s for all leaf cubes that don't already have them
{
    ZoneScopedN("Geometry/Update octree render");
    int csi = 0;
    while(1<<csi < worldsize) csi++;
    const int worldsectionsize = getworldmeshdomainsize(),
              facemax = worldsectionsize ? max(vafacemax, 8192) : vafacemax,
              maxvasize = min(0x1000, worldsize/2);

    recalcprogress = 0;
    varoot.setsize(0);
    {
        ZoneScopedN("Geometry/Build changed vertex arrays");
        ZoneValue(valist.length());
        updateva(worldroot, ivec(0, 0, 0), worldsize/2, csi-1,
                 worldsectionsize, facemax, maxvasize);
    }
    loadprogress = 0;
    {
        ZoneScopedN("Geometry/Flush vertex buffers");
        flushvbo();
    }

    ullong arenacapacitybytes = 0;
    loopv(worldmeshpages) arenacapacitybytes += ullong(worldmeshpages[i]->vertexcapacity)*sizeof(vertex) +
                                                ullong(worldmeshpages[i]->indexcapacity)*sizeof(ushort);
    const ullong arenausedbytes = ullong(worldmesharenausedvertices)*sizeof(vertex) +
                                 ullong(worldmesharenausedindices)*sizeof(ushort);
    (void)arenausedbytes;
    TracyPlot("VoxelMesh/Uploaded bytes", int64_t(worldvauploadbytes));
    TracyPlot("VoxelMesh/Arena used bytes", int64_t(arenausedbytes));
    TracyPlot("VoxelMesh/Arena free bytes", int64_t(arenacapacitybytes - arenausedbytes));
    TracyPlot("VoxelMesh/Mesh packets", int64_t(worldmeshpackets));
    TracyPlot("VoxelMesh/Largest packet bytes", int64_t(worldmeshlargestpacket));

    {
        ZoneScopedN("Geometry/Recount explicit sky");
        explicitsky = 0;
        loopv(valist)
        {
            vtxarray *va = valist[i];
            explicitsky += va->sky;
        }
    }

    visibleva = NULL;
}

void precachetextures()
{
    vector<int> texs;
    loopv(valist)
    {
        vtxarray *va = valist[i];
        loopj(va->texs + va->blends)
        {
            int tex = va->texelems[j].texture;
            if(texs.find(tex) < 0)
            {
                texs.add(tex);

                VSlot &vslot = lookupvslot(tex, false);
                if(vslot.layer && texs.find(vslot.layer) < 0) texs.add(vslot.layer);
                if(vslot.detail && texs.find(vslot.detail) < 0) texs.add(vslot.detail);
            }
        }
    }
    loopv(texs)
    {
        loadprogress = float(i+1)/texs.length();
        lookupvslot(texs[i]);
    }
    loadprogress = 0;
}

void allchanged(bool load)
{
    flushworldmeshjobs();
    if(mainmenu && !isconnected()) load = false;
    if(load) initlights();
    renderprogress(0, "clearing vertex arrays...");
    clearvas(worldroot);
    resetqueries();
    resetclipplanes();
    if(load) initenvmaps();
    entitiesinoctanodes();
    tjoints.setsize(0);
    if(filltjoints) findtjoints();
    octarender();
    if(load) precachetextures();
    setupmaterials();
    clearshadowcache();
    updatevabbs(true);
    if(load)
    {
        genshadowmeshes();
        updateblendtextures();
        seedparticles();
        genenvmaps();
        drawminimap();
    }
}

void recalc()
{
    allchanged(true);
}

COMMAND(recalc, "");
