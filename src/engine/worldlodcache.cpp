// worldlodcache.cpp: disposable, versioned CPU mesh caches for local worlds

#ifdef WORLDIO_MODULE_IMPLEMENTATION

enum
{
    WORLD_LOD_CACHE_VERSION = 1, // bump when mesh generation, materials or climate packing changes
    WORLD_LOD_CACHE_HEADER_SIZE = 92,
    WORLD_LOD_CACHE_VERTEX_SIZE = 36,
    WORLD_LOD_CACHE_MAX_SIZE = 128 << 20
};

static void worldlodputfloat(vector<uchar> &output, float value)
{
    uint bits;
    memcpy(&bits, &value, sizeof(bits));
    worldsnapshotputuint(output, bits);
}

static bool worldlodreadfloat(worldsnapshotreader &reader, float &value)
{
    uint bits;
    if(!reader.readuint(bits) || (bits & 0x7F800000U) == 0x7F800000U) return false;
    memcpy(&value, &bits, sizeof(value));
    return true;
}

static bool serializeworldlodmesh(worldlodjob &job, vector<uchar> &output)
{
    const worldlodcpumesh &mesh = job.mesh;
    const ullong size = WORLD_LOD_CACHE_HEADER_SIZE + 4ULL + ullong(mesh.vertices.length()) * WORLD_LOD_CACHE_VERTEX_SIZE +
                        ullong(mesh.indices.length()) * 4;
    if(size > WORLD_LOD_CACHE_MAX_SIZE || SDL_AtomicGet(&job.cancelled)) return false;
    output.setsize(0);
    output.growbuf(int(size));
    output.put((const uchar *)"CCLD", 4);
    const uint header[] = { WORLD_LOD_CACHE_VERSION, WORLDGEN_VERSION, uint(job.key.x), uint(job.key.y), uint(job.key.lod),
                            uint(job.key.resolution), uint(job.key.skirtdepth), uint(job.key.seed), uint(job.key.generation),
                            uint(job.key.generation >> 32), uint(mesh.vertices.length()), uint(mesh.terrainindices),
                            uint(mesh.waterindices), uint(mesh.topfaces), uint(mesh.sidefaces) };
    loopi(sizeof(header) / sizeof(header[0])) worldsnapshotputuint(output, header[i]);
    loopi(3) worldlodputfloat(output, mesh.bbmin[i]);
    loopi(3) worldlodputfloat(output, mesh.bbmax[i]);
    worldlodputfloat(output, mesh.waterheight);
    ASSERT(output.length() == WORLD_LOD_CACHE_HEADER_SIZE);
    loopv(mesh.vertices)
    {
        if(!(i & 255) && SDL_AtomicGet(&job.cancelled)) return false;
        const worldlodvertex &vertex = mesh.vertices[i];
        loopj(3) worldlodputfloat(output, vertex.position[j]);
        loopj(3) worldlodputfloat(output, vertex.normal[j]);
        loopj(2) worldlodputfloat(output, vertex.texcoord[j]);
        output.put(vertex.material.v, 4);
    }
    loopv(mesh.indices) worldsnapshotputuint(output, mesh.indices[i]);
    worldsnapshotputuint(output, uint(crc32(0, (const Bytef *)output.getbuf(), uInt(output.length()))));
    return !SDL_AtomicGet(&job.cancelled);
}

static bool deserializeworldlodmesh(worldlodjob &job, const vector<uchar> &contents)
{
    if(contents.length() < WORLD_LOD_CACHE_HEADER_SIZE + 4 || contents.length() > WORLD_LOD_CACHE_MAX_SIZE ||
       SDL_AtomicGet(&job.cancelled) || !validateworldsnapshotchecksum(contents)) return false;
    worldsnapshotreader reader(contents.getbuf(), contents.length() - 4);
    char magic[4];
    if(!reader.read(magic, 4) || memcmp(magic, "CCLD", 4)) return false;
    const uint expected[] = { WORLD_LOD_CACHE_VERSION, WORLDGEN_VERSION, uint(job.key.x), uint(job.key.y), uint(job.key.lod),
                              uint(job.key.resolution), uint(job.key.skirtdepth), uint(job.key.seed), uint(job.key.generation),
                              uint(job.key.generation >> 32) };
    loopi(sizeof(expected) / sizeof(expected[0]))
    {
        uint value;
        if(!reader.readuint(value) || value != expected[i]) return false;
    }
    uint vertices, terrain, water, tops, sides;
    if(!reader.readuint(vertices) || !reader.readuint(terrain) || !reader.readuint(water) ||
       !reader.readuint(tops) || !reader.readuint(sides)) return false;
    const ullong indices = ullong(terrain) + water,
                  size = WORLD_LOD_CACHE_HEADER_SIZE + 4ULL + ullong(vertices) * WORLD_LOD_CACHE_VERTEX_SIZE + indices * 4;
    if(size != ullong(contents.length()) || terrain % 3 || water % 3 || tops > indices || sides > indices) return false;
    worldlodcpumesh mesh;
    mesh.detailed = job.key.lod == 1;
    loopi(3) if(!worldlodreadfloat(reader, mesh.bbmin[i])) return false;
    loopi(3) if(!worldlodreadfloat(reader, mesh.bbmax[i]) || mesh.bbmin[i] > mesh.bbmax[i]) return false;
    if(!worldlodreadfloat(reader, mesh.waterheight)) return false;
    mesh.vertices.pad(int(vertices));
    loopv(mesh.vertices)
    {
        if(!(i & 255) && SDL_AtomicGet(&job.cancelled)) return false;
        worldlodvertex &vertex = mesh.vertices[i];
        loopj(3) if(!worldlodreadfloat(reader, vertex.position[j]) || vertex.position[j] < mesh.bbmin[j] - 0.01f ||
                   vertex.position[j] > mesh.bbmax[j] + 0.01f) return false;
        loopj(3) if(!worldlodreadfloat(reader, vertex.normal[j]) || fabsf(vertex.normal[j]) > 1.0f) return false;
        loopj(2) if(!worldlodreadfloat(reader, vertex.texcoord[j])) return false;
        if(!reader.read(vertex.material.v, 4) || vertex.material.x >= WORLD_LOD_MATERIALS) return false;
    }
    mesh.indices.pad(int(indices));
    loopv(mesh.indices) if(!reader.readuint(mesh.indices[i]) || mesh.indices[i] >= vertices) return false;
    if(!reader.finished() || SDL_AtomicGet(&job.cancelled)) return false;
    mesh.terrainindices = int(terrain);
    mesh.waterindices = int(water);
    mesh.topfaces = int(tops);
    mesh.sidefaces = int(sides);
    job.mesh.vertices.move(mesh.vertices);
    job.mesh.indices.move(mesh.indices);
    job.mesh.terrainindices = mesh.terrainindices;
    job.mesh.waterindices = mesh.waterindices;
    job.mesh.topfaces = mesh.topfaces;
    job.mesh.sidefaces = mesh.sidefaces;
    job.mesh.bbmin = mesh.bbmin;
    job.mesh.bbmax = mesh.bbmax;
    job.mesh.waterheight = mesh.waterheight;
    job.mesh.detailed = mesh.detailed;
    return true;
}

static bool loadworldlodmesh(worldlodjob &job)
{
    ZoneScopedN("LOD/Disk load");
    stream *file = openrawfile(job.cachefile, "rb");
    if(!file) return false;
    const stream::offset length = file->size();
    if(length < WORLD_LOD_CACHE_HEADER_SIZE + 4 || length > WORLD_LOD_CACHE_MAX_SIZE)
    {
        delete file;
        return false;
    }
    vector<uchar> contents;
    const bool read = file->read(contents.pad(int(length)), size_t(length)) == size_t(length);
    delete file;
    return read && deserializeworldlodmesh(job, contents);
}

static bool saveworldlodmesh(worldlodjob &job)
{
    ZoneScopedN("LOD/Disk save");
    vector<uchar> contents;
    // Uncompressed meshes favor fast reuse; the snapshot writer publishes by atomic replacement.
    return serializeworldlodmesh(job, contents) && !SDL_AtomicGet(&job.cancelled) &&
           writeworldsnapshotfile(job.cachefile, contents, false);
}

#endif
