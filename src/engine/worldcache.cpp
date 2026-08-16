// worldcache.cpp: disposable persistent pristine generated-chunk cache

#ifdef WORLDIO_MODULE_IMPLEMENTATION

enum
{
    WORLD_CHUNK_CACHE_FORMAT_VERSION = 4,
    WORLD_CHUNK_CACHE_HEADER_SIZE = 48,
    WORLD_CHUNK_CACHE_MAX_PAYLOAD = 512 << 20
};

enum
{
    WORLD_CACHE_NODE_CHILDREN = 0,
    WORLD_CACHE_NODE_LEAF = 1
};

struct worldchunkcachewritejob
{
    int x, y, seed;
    uint id;
    ullong worldgenhash;
    bool remip;
    string filename;
    vector<uchar> payload;
    SDL_atomic_t cancelled;

    worldchunkcachewritejob(int x, int y, int seed, ullong worldgenhash, bool remip, uint id)
        : x(x), y(y), seed(seed), id(id), worldgenhash(worldgenhash), remip(remip)
    {
        filename[0] = '\0';
        SDL_AtomicSet(&cancelled, 0);
    }
};

static void worldcacheput32(vector<uchar> &out, uint value)
{
    loopi(4) out.add(uchar(value >> (8 * i)));
}

static void worldcacheput64(vector<uchar> &out, ullong value)
{
    loopi(8) out.add(uchar(value >> (8 * i)));
}

struct worldcachereader
{
    const uchar *pos, *end;

    worldcachereader(const uchar *data, size_t length) : pos(data), end(data + length) {}

    bool readbyte(uchar &value)
    {
        if(pos >= end) return false;
        value = *pos++;
        return true;
    }

    bool readushort(ushort &value)
    {
        if(end - pos < 2) return false;
        value = ushort(pos[0] | (uint(pos[1]) << 8));
        pos += 2;
        return true;
    }

    bool readuint(uint &value)
    {
        if(end - pos < 4) return false;
        value = uint(pos[0]) | (uint(pos[1]) << 8) | (uint(pos[2]) << 16) | (uint(pos[3]) << 24);
        pos += 4;
        return true;
    }

    bool readullong(ullong &value)
    {
        if(end - pos < 8) return false;
        value = 0;
        loopi(8) value |= ullong(pos[i]) << (8 * i);
        pos += 8;
        return true;
    }

    bool read(void *destination, size_t length)
    {
        if(size_t(end - pos) < length) return false;
        memcpy(destination, pos, length);
        pos += length;
        return true;
    }

    int remaining() const { return int(end - pos); }
};

static void resetworldcachecube(cube &c)
{
    c.children = NULL;
    c.ext = NULL;
    c.visible = 0;
    c.merged = 0;
    c.material = MAT_AIR;
    emptyfaces(c);
    loopi(6) c.texture[i] = DEFAULT_GEOM;
}

static void serializeworldcachecube(const cube &c, vector<uchar> &out)
{
    if(c.children)
    {
        out.add(WORLD_CACHE_NODE_CHILDREN);
        loopi(8) serializeworldcachecube(c.children[i], out);
        return;
    }
    out.add(WORLD_CACHE_NODE_LEAF);
    out.put(c.edges, sizeof(c.edges));
    loopi(6)
    {
        out.add(uchar(c.texture[i]));
        out.add(uchar(c.texture[i] >> 8));
    }
    out.add(uchar(c.material));
    out.add(uchar(c.material >> 8));
}

static bool serializeworldchunkcache(cube *root, const vector<worldscatterinstance> &scatter, vector<uchar> &payload,
                                     const worldsectionrenderdata *renderdata)
{
    ZoneScopedN("Chunks/Cache serialize");
    payload.setsize(0);
    if(!root) return false;
    loopi(8) serializeworldcachecube(root[i], payload);
    worldcacheput32(payload, uint(scatter.length()));
    loopv(scatter)
    {
        const worldscatterinstance &instance = scatter[i];
        worldcacheput32(payload, uint(instance.x));
        worldcacheput32(payload, uint(instance.y));
        worldcacheput32(payload, uint(instance.z));
        worldcacheput64(payload, getworldscatterpersistentid(instance.type));
        worldcacheput32(payload, uint(instance.orient));
    }
    if(!renderdata) return false;
    payload.put(&renderdata->flags[0][0], sizeof(renderdata->flags));
    return payload.length() > 0 && payload.length() <= WORLD_CHUNK_CACHE_MAX_PAYLOAD;
}

static cube *newworldcachefamily(bool prepared, int &families)
{
    if(!prepared) return newcubes(F_EMPTY);
    cube *children = new cube[8];
    loopi(8) resetworldcachecube(children[i]);
    families++;
    return children;
}

static void freeworldcachetree(cube *root, bool prepared)
{
    if(!root) return;
    if(!prepared)
    {
        freeocta(root);
        return;
    }
    loopi(8) if(root[i].children) freeworldcachetree(root[i].children, true);
    delete[] root;
}

static bool deserializeworldcachecube(worldcachereader &reader, cube &c, int size, bool prepared, int &families, uint &nodes)
{
    uchar type;
    if(!reader.readbyte(type) || ++nodes > uint(WORLD_CHUNK_CACHE_MAX_PAYLOAD)) return false;
    resetworldcachecube(c);
    if(type == WORLD_CACHE_NODE_CHILDREN)
    {
        if(size <= 1) return false;
        c.children = newworldcachefamily(prepared, families);
        loopi(8) if(!deserializeworldcachecube(reader, c.children[i], size >> 1, prepared, families, nodes)) return false;
        return true;
    }
    if(type != WORLD_CACHE_NODE_LEAF || !reader.read(c.edges, sizeof(c.edges))) return false;
    loopi(6) if(!reader.readushort(c.texture[i])) return false;
    if(!reader.readushort(c.material)) return false;
    c.visible = c.merged = 0;
    c.ext = NULL;
    return true;
}

static cube *deserializeworldchunkcache(const uchar *data, int length, vector<worldscatterinstance> &scatter, bool prepared, int &families,
                                        worldsectionrenderdata *renderdata)
{
    ZoneScopedN("Chunks/Cache deserialize");
    worldcachereader reader(data, length);
    cube *root = newworldcachefamily(prepared, families);
    uint nodes = 0;
    loopi(8) if(!deserializeworldcachecube(reader, root[i], WORLD_CHUNK_ROOT_SIZE, prepared, families, nodes))
    {
        freeworldcachetree(root, prepared);
        return NULL;
    }
    uint count;
    if(!reader.readuint(count) || count > 1000000U || count > uint(reader.remaining() / 24))
    {
        freeworldcachetree(root, prepared);
        return NULL;
    }
    loopi(count)
    {
        uint value;
        ullong persistentid;
        worldscatterinstance instance;
        if(!reader.readuint(value)) break;
        instance.x = int(value);
        if(!reader.readuint(value)) break;
        instance.y = int(value);
        if(!reader.readuint(value)) break;
        instance.z = int(value);
        if(!reader.readullong(persistentid)) break;
        instance.type = getworldscatterpersistentindex(persistentid, false);
        if(!reader.readuint(value)) break;
        instance.orient = int(value);
        if(instance.type < 0 || instance.x < 0 || instance.x >= WORLD_CHUNK_SIZE || instance.y < 0 || instance.y >= WORLD_CHUNK_SIZE ||
           instance.z < 0 || instance.z >= WORLD_MAP_SIZE || instance.orient < O_LEFT || instance.orient > O_TOP)
            break;
        scatter.add(instance);
    }
    worldsectionrenderdata cachedrenderdata;
    if(scatter.length() != int(count) || !reader.read(&cachedrenderdata.flags[0][0], sizeof(cachedrenderdata.flags)) || reader.remaining())
    {
        scatter.setsize(0);
        freeworldcachetree(root, prepared);
        return NULL;
    }
    const int validflags = SECTION_EXTERIOR | SECTION_INTERIOR | SECTION_CAVE_ENTRANCE | SECTION_WATER | SECTION_FULLY_SOLID | SECTION_NO_RENDER;
    loopi(WORLD_SECTION_LAYERS) loopj(WORLD_SECTION_TILES)
    {
        const int flags = cachedrenderdata.flags[i][j];
        if((flags&~validflags) || ((flags&SECTION_NO_RENDER) && !(flags&SECTION_FULLY_SOLID)))
        {
            scatter.setsize(0);
            freeworldcachetree(root, prepared);
            return NULL;
        }
    }
    if(renderdata) *renderdata = cachedrenderdata;
    return root;
}

static void worldchunkcachefilename(char *name, size_t length, const char *folder, int x, int y)
{
    snprintf(name, length, "media/map/%s/chunkcache/%d_%d.wcc", folder, x, y);
    path(name);
}

static cube *loadworldchunkcache(const char *filename, int x, int y, int seed, ullong worldgenhash, bool remip,
                                  vector<worldscatterinstance> &scatter, bool prepared, int &families, int &error,
                                  worldsectionrenderdata *renderdata)
{
    ZoneScopedN("Chunks/Cache lookup");
    error = 0;
    scatter.setsize(0);
    if(!renderdata) return NULL;
    const char *found = findfile(filename, "rb");
    if(!found || !fileexists(found, "r")) return NULL;
    string foundpath;
    copystring(foundpath, found);

    vector<uchar> filebytes;
    {
        ZoneScopedN("Chunks/Cache read");
        stream *file = openrawfile(filename, "rb");
        if(!file) { error = 1; return NULL; }
        const stream::offset length = file->size();
        if(length < WORLD_CHUNK_CACHE_HEADER_SIZE || length > WORLD_CHUNK_CACHE_HEADER_SIZE + WORLD_CHUNK_CACHE_MAX_PAYLOAD)
        {
            delete file;
            error = 2;
            return NULL;
        }
        uchar *destination = filebytes.pad(int(length));
        const bool read = file->read(destination, size_t(length)) == size_t(length);
        delete file;
        if(!read) { error = 3; return NULL; }
    }

    worldcachereader header(filebytes.getbuf(), filebytes.length());
    char magic[4];
    uint formatversion, genversion, storedremip, storedseed, storedx, storedy, uncompressedsize, compressedsize, checksum;
    ullong storedhash;
    if(!header.read(magic, sizeof(magic)) || memcmp(magic, "WCC1", 4) || !header.readuint(formatversion) ||
       !header.readuint(genversion) || !header.readuint(storedremip) || !header.readuint(storedseed) || !header.readuint(storedx) ||
       !header.readuint(storedy) ||
       !header.readullong(storedhash) || !header.readuint(uncompressedsize) || !header.readuint(compressedsize) || !header.readuint(checksum) ||
       formatversion != WORLD_CHUNK_CACHE_FORMAT_VERSION || genversion != WORLDGEN_VERSION || storedremip != uint(remip) || int(storedseed) != seed ||
       int(storedx) != x ||
       int(storedy) != y || storedhash != worldgenhash || !uncompressedsize || uncompressedsize > WORLD_CHUNK_CACHE_MAX_PAYLOAD ||
       !compressedsize || compressedsize > WORLD_CHUNK_CACHE_MAX_PAYLOAD || compressedsize != uint(header.remaining()))
    {
        error = 4;
        remove(foundpath);
        return NULL;
    }

    vector<uchar> payload;
    payload.pad(int(uncompressedsize));
    {
        ZoneScopedN("Chunks/Cache decompress");
        uLongf destinationlength = uLongf(uncompressedsize);
        if(uncompress((Bytef *)payload.getbuf(), &destinationlength, (const Bytef *)header.pos, uLong(compressedsize)) != Z_OK ||
           destinationlength != uLongf(uncompressedsize))
        {
            error = 5;
            remove(foundpath);
            return NULL;
        }
    }
    if(uint(crc32(0, (const Bytef *)payload.getbuf(), uInt(payload.length()))) != checksum)
    {
        error = 6;
        remove(foundpath);
        return NULL;
    }
    cube *root = deserializeworldchunkcache(payload.getbuf(), payload.length(), scatter, prepared, families, renderdata);
    if(!root)
    {
        error = 7;
        remove(foundpath);
    }
    else game::cacheworldscattertransforms(x, y, game::getworldscattermaxoffset(), scatter);
    return root;
}

static bool replaceworldchunkcachefile(const char *temporary, const char *finalname)
{
#ifdef WIN32
    return MoveFileEx(temporary, finalname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temporary, finalname) == 0;
#endif
}

static bool writeworldchunkcache(worldchunkcachewritejob &job)
{
    if(SDL_AtomicGet(&job.cancelled) || job.payload.empty()) return false;
    vector<uchar> compressed;
    {
        ZoneScopedN("Chunks/Cache compress");
        uLongf compressedlength = compressBound(uLong(job.payload.length()));
        if(compressedlength > WORLD_CHUNK_CACHE_MAX_PAYLOAD) return false;
        compressed.pad(int(compressedlength));
        if(compress2((Bytef *)compressed.getbuf(), &compressedlength, (const Bytef *)job.payload.getbuf(), uLong(job.payload.length()),
                     Z_BEST_SPEED) != Z_OK)
            return false;
        compressed.setsize(int(compressedlength));
    }
    if(SDL_AtomicGet(&job.cancelled)) return false;

    vector<uchar> contents;
    contents.put((const uchar *)"WCC1", 4);
    worldcacheput32(contents, WORLD_CHUNK_CACHE_FORMAT_VERSION);
    worldcacheput32(contents, WORLDGEN_VERSION);
    worldcacheput32(contents, uint(job.remip));
    worldcacheput32(contents, uint(job.seed));
    worldcacheput32(contents, uint(job.x));
    worldcacheput32(contents, uint(job.y));
    worldcacheput64(contents, job.worldgenhash);
    worldcacheput32(contents, uint(job.payload.length()));
    worldcacheput32(contents, uint(compressed.length()));
    worldcacheput32(contents, uint(crc32(0, (const Bytef *)job.payload.getbuf(), uInt(job.payload.length()))));
    contents.put(compressed.getbuf(), compressed.length());

    ZoneScopedN("Chunks/Cache write");
    defformatstring(temporary, "%s.%u.tmp", job.filename, job.id);
    string temporarypath, finalpath;
    copystring(temporarypath, findfile(temporary, "wb"));
    copystring(finalpath, findfile(job.filename, "wb"));
    stream *file = openrawfile(temporary, "wb");
    const bool written = file && file->write(contents.getbuf(), contents.length()) == size_t(contents.length()) && file->flush();
    delete file;
    if(!written || SDL_AtomicGet(&job.cancelled))
    {
        remove(temporarypath);
        return false;
    }
    if(!replaceworldchunkcachefile(temporarypath, finalpath))
    {
        remove(temporarypath);
        return false;
    }
    return true;
}

static int removeworldchunkcachefiles(const char *folder)
{
    defformatstring(directory, "media/map/%s/chunkcache", folder);
    path(directory);
    int removed = 0;
    vector<char *> files;
    listfiles(directory, "wcc", files);
    loopv(files)
    {
        defformatstring(relative, "%s/%s.wcc", directory, files[i]);
        const char *filename = findfile(path(relative), "wb");
        if(filename && !remove(filename)) removed++;
    }
    files.deletecontents();
    listfiles(directory, "tmp", files);
    loopv(files)
    {
        defformatstring(relative, "%s/%s.tmp", directory, files[i]);
        const char *filename = findfile(path(relative), "wb");
        if(filename && !remove(filename)) removed++;
    }
    files.deletecontents();
    return removed;
}

static void reportworldchunkcachestats(const char *folder)
{
    defformatstring(directory, "media/map/%s/chunkcache", folder);
    path(directory);
    vector<char *> files;
    listfiles(directory, "wcc", files);
    ullong bytes = 0;
    int count = 0;
    loopv(files)
    {
        defformatstring(relative, "%s/%s.wcc", directory, files[i]);
        stream *file = openrawfile(path(relative), "rb");
        if(!file) continue;
        const stream::offset size = file->size();
        delete file;
        if(size < 0) continue;
        bytes += ullong(size);
        count++;
    }
    files.deletecontents();
    conoutf("generated chunk cache: %d entries, %.2f MiB", count, double(bytes) / (1024.0 * 1024.0));
}

#endif
