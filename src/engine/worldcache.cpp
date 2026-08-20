// worldcache.cpp: authoritative local chunk snapshots

#ifdef WORLDIO_MODULE_IMPLEMENTATION

extern int compresschunks;

enum
{
    WORLD_SNAPSHOT_VOX_VERSION = 2,
    WORLD_SNAPSHOT_DAT_VERSION = 2,
    WORLD_SNAPSHOT_COMPRESSION_VERSION = 1,
    WORLD_SNAPSHOT_MAX_FILE_SIZE = 512 << 20,
    WORLD_SNAPSHOT_MAX_STORED_FILE_SIZE = WORLD_SNAPSHOT_MAX_FILE_SIZE + (WORLD_SNAPSHOT_MAX_FILE_SIZE >> 8) + 65536,
    WORLD_SNAPSHOT_EMPTY = 1 << 0,
    WORLD_SNAPSHOT_SOLID = 1 << 1,
    WORLD_SNAPSHOT_SCATTER = 1
};


struct worldsnapshotvoxel
{
    ushort palette, material;
    uchar orientation, flags, edges[12];

    worldsnapshotvoxel() : palette(0), material(MAT_AIR), orientation(O_TOP), flags(WORLD_SNAPSHOT_EMPTY) { memset(edges, 0, sizeof(edges)); }

    bool operator==(const worldsnapshotvoxel &other) const
    {
        return palette == other.palette && material == other.material && orientation == other.orientation && flags == other.flags &&
               !memcmp(edges, other.edges, sizeof(edges));
    }
};

struct worldsnapshotrun
{
    ushort length;
    worldsnapshotvoxel voxel;

    worldsnapshotrun() : length(0) {}
    worldsnapshotrun(int length, const worldsnapshotvoxel &voxel) : length(ushort(length)), voxel(voxel) {}
};

struct worldsnapshotcolumn { vector<worldsnapshotrun> runs; };

struct worldsnapshotpaletteentry
{
    string id;
    int worldindex;

    worldsnapshotpaletteentry(const char *id = "", int worldindex = -1) : worldindex(worldindex) { copystring(this->id, id); }
};

struct worldsnapshotscatter
{
    ushort x, y, z;
    string id;
    int type, orientation;

    worldsnapshotscatter() : x(0), y(0), z(0), type(-1), orientation(O_TOP) { id[0] = '\0'; }
};

#if !defined(STANDALONE) && !defined(WORLD_SNAPSHOT_SERVER_CODEC)
static void mergeworldsnapshotmountedsections(const worldchunk &chunk, cube *snapshotroot);
#endif

struct worldchunksnapshot
{
    int x, y;
    uint revision;
    bool playeredited;
    worldsectionrenderdata renderdata;
    vector<worldsnapshotpaletteentry> palette;
    vector<worldsnapshotcolumn> columns;
    vector<worldsnapshotscatter> scatter;
    vector<uchar> gameplay;

    worldchunksnapshot(int x = 0, int y = 0) : x(x), y(y), revision(1), playeredited(false) {}
};

#if !defined(STANDALONE) && !defined(WORLD_SNAPSHOT_SERVER_CODEC)
struct worldchunksavejob
{
    int x, y;
    uint epoch, revision;
    cube *root;
    worldsectionrenderdata renderdata;
    vector<worldscatterinstance> scatter;
    vector<uchar> gameplay;
    string folder, error;
    bool success, playeredited, compress;

    worldchunksavejob(int x, int y, uint epoch, uint revision, const char *folder)
        : x(x), y(y), epoch(epoch), revision(revision), root(NULL), success(false), playeredited(false), compress(compresschunks != 0)
    {
        copystring(this->folder, folder ? folder : "");
        error[0] = '\0';
    }

    ~worldchunksavejob()
    {
        if(root) freeocta(root);
    }
};
#endif

enum worldsnapshotloadresult { WORLD_SNAPSHOT_MISSING = 0, WORLD_SNAPSHOT_LOADED, WORLD_SNAPSHOT_INVALID };

struct worldsnapshotreader
{
    const uchar *position, *end;

    worldsnapshotreader(const uchar *data, int length) : position(data), end(data + length) {}

    bool read(void *destination, int length)
    {
        if(length < 0 || end - position < length) return false;
        memcpy(destination, position, length);
        position += length;
        return true;
    }

    bool readbyte(uchar &value) { return read(&value, 1); }

    bool readushort(ushort &value)
    {
        uchar bytes[2];
        if(!read(bytes, 2)) return false;
        value = ushort(bytes[0] | uint(bytes[1]) << 8);
        return true;
    }

    bool readuint(uint &value)
    {
        uchar bytes[4];
        if(!read(bytes, 4)) return false;
        value = uint(bytes[0]) | uint(bytes[1]) << 8 | uint(bytes[2]) << 16 | uint(bytes[3]) << 24;
        return true;
    }

    bool finished() const { return position == end; }
};

static void worldsnapshotputushort(vector<uchar> &output, ushort value)
{
    output.add(uchar(value));
    output.add(uchar(value >> 8));
}

static void worldsnapshotputuint(vector<uchar> &output, uint value) { loopi(4) output.add(uchar(value >> (8 * i))); }

static bool worldsnapshotputstring(vector<uchar> &output, const char *value)
{
    const int length = value ? int(strlen(value)) : 0;
    if(length <= 0 || length >= MAXSTRLEN) return false;
    worldsnapshotputushort(output, ushort(length));
    output.put((const uchar *)value, length);
    return true;
}

static bool worldsnapshotreadstring(worldsnapshotreader &reader, char *value, int size)
{
    ushort length;
    if(!reader.readushort(length) || !length || length >= size || !reader.read(value, length)) return false;
    value[length] = '\0';
    return true;
}

static void resetworldsnapshotcube(cube &c)
{
    c.children = NULL;
    c.ext = NULL;
    c.visible = c.merged = 0;
    c.material = MAT_AIR;
    emptyfaces(c);
    loopi(6) c.texture[i] = DEFAULT_GEOM;
}

static cube *allocworldsnapshotfamily()
{
    cube *family = new cube[8];
    loopi(8) resetworldsnapshotcube(family[i]);
    return family;
}

static void freeworldsnapshotfamily(cube *family)
{
    if(!family) return;
    loopi(8) if(family[i].children) freeworldsnapshotfamily(family[i].children);
    delete[] family;
}

static void worldchunksnapshotfilename(char *name, size_t length, const char *folder, int x, int y, const char *extension)
{
    snprintf(name, length, "media/map/%s/chunks/%d_%d.%s", folder, x, y, extension);
    path(name);
}

static bool readworldsnapshotfile(const char *filename, vector<uchar> &contents, bool allowcompression = true)
{
    stream *file = openrawfile(filename, "rb");
    if(!file) return false;
    const stream::offset length = file->size();
    if(length <= 0 || length > WORLD_SNAPSHOT_MAX_STORED_FILE_SIZE) { delete file; return false; }
    vector<uchar> stored;
    const bool read = file->read(stored.pad(int(length)), size_t(length)) == size_t(length);
    delete file;
    if(!read) return false;
    if(stored.length() < 4 || memcmp(stored.getbuf(), "CCZL", 4))
    {
        if(stored.length() > WORLD_SNAPSHOT_MAX_FILE_SIZE) return false;
        contents.move(stored);
        return true;
    }
    if(!allowcompression || stored.length() < 12) return false;
    worldsnapshotreader reader(stored.getbuf() + 4, 8);
    uint version, uncompressedsize;
    if(!reader.readuint(version) || version != WORLD_SNAPSHOT_COMPRESSION_VERSION || !reader.readuint(uncompressedsize) ||
       !uncompressedsize || uncompressedsize > WORLD_SNAPSHOT_MAX_FILE_SIZE)
        return false;
    uLongf destinationlength = uLongf(uncompressedsize);
    uchar *destination = contents.pad(int(uncompressedsize));
    const int result = uncompress((Bytef *)destination, &destinationlength, (const Bytef *)stored.getbuf() + 12, uLong(stored.length() - 12));
    if(result != Z_OK || destinationlength != uncompressedsize)
    {
        contents.setsize(0);
        return false;
    }
    return true;
}

static bool replaceworldsnapshotfile(const char *temporary, const char *finalname)
{
#ifdef WIN32
    return MoveFileEx(temporary, finalname, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(temporary, finalname) == 0;
#endif
}

static bool writeworldsnapshotfile(const char *filename, const vector<uchar> &contents, bool compress)
{
    vector<uchar> compressed;
    const vector<uchar> *stored = &contents;
    if(compress)
    {
        compressed.put((const uchar *)"CCZL", 4);
        loopi(4) compressed.add(uchar(WORLD_SNAPSHOT_COMPRESSION_VERSION >> (8 * i)));
        loopi(4) compressed.add(uchar(uint(contents.length()) >> (8 * i)));
        const uLong bound = compressBound(uLong(contents.length()));
        if(bound > uLong(INT_MAX - compressed.length())) return false;
        uLongf compressedlength = bound;
        uchar *destination = compressed.pad(int(bound));
        const int result = compress2((Bytef *)destination, &compressedlength, (const Bytef *)contents.getbuf(), uLong(contents.length()),
                                     Z_DEFAULT_COMPRESSION);
        if(result != Z_OK) return false;
        compressed.setsize(12 + int(compressedlength));
        stored = &compressed;
    }
    defformatstring(temporary, "%s.tmp", filename);
    string temporarypath, finalpath;
    copystring(temporarypath, findfile(temporary, "wb"));
    copystring(finalpath, findfile(filename, "wb"));
    stream *file = openrawfile(temporary, "wb");
    const bool written = file && file->write(stored->getbuf(), stored->length()) == size_t(stored->length()) && file->flush();
    delete file;
    if(!written || !replaceworldsnapshotfile(temporarypath, finalpath))
    {
        remove(temporarypath);
        return false;
    }
    return true;
}

static const cube &worldsnapshotcubeat(const cube *root, const ivec &position, int &size)
{
    int scale = WORLD_CHUNK_SCALE - 1;
    size = 1 << scale;
    const cube *current = &root[octastep(position.x, position.y, position.z, scale)];
    while(current->children && size > WORLD_BLOCK_SIZE)
    {
        --scale;
        size >>= 1;
        current = &current->children[octastep(position.x, position.y, position.z, scale)];
    }
    return *current;
}

static void copyworldsnapshotcube(const cube &source, cube &destination)
{
    destination = source;
    destination.ext = NULL;
    destination.visible = destination.merged = 0;
    destination.children = NULL;
    if(source.children)
    {
        destination.children = allocworldsnapshotfamily();
        loopi(8) copyworldsnapshotcube(source.children[i], destination.children[i]);
    }
}

static bool normalizeworldsnapshotcube(cube &c, int size, string &error)
{
    if(size <= WORLD_BLOCK_SIZE)
    {
        if(c.children)
        {
            copystring(error, "chunk contains sub-voxel octree state");
            return false;
        }
        return true;
    }
    if(!c.children && !isempty(c) && !isentirelysolid(c))
    {
#if defined(STANDALONE) || defined(WORLD_SNAPSHOT_SERVER_CODEC)
        copystring(error, "server chunk contains coarse carved geometry");
        return false;
#else
        subdividecube(c, false, false);
#endif
        if(!c.children)
        {
            copystring(error, "could not subdivide carved block geometry for snapshotting");
            return false;
        }
    }
    if(c.children)
    {
        const int childsize = size >> 1;
        loopi(8) if(!normalizeworldsnapshotcube(c.children[i], childsize, error)) return false;
    }
    return true;
}

#if !defined(STANDALONE) && !defined(WORLD_SNAPSHOT_SERVER_CODEC)
struct worldsnapshotsamplingtree
{
    cube *root;

    worldsnapshotsamplingtree() : root(NULL) {}
    ~worldsnapshotsamplingtree() { if(root) freeworldsnapshotfamily(root); }

    bool build(const worldchunk &chunk)
    {
        if(!chunk.root) return false;
        root = allocworldsnapshotfamily();
        loopi(8) copyworldsnapshotcube(chunk.root[i], root[i]);
        mergeworldsnapshotmountedsections(chunk, root);
        return true;
    }
};
#endif

static int worldsnapshotpaletteindex(worldchunksnapshot &snapshot, const char *id, int worldindex)
{
    loopv(snapshot.palette) if(!strcmp(snapshot.palette[i].id, id)) return i;
    if(snapshot.palette.length() >= USHRT_MAX) return -1;
    snapshot.palette.add(worldsnapshotpaletteentry(id, worldindex));
    return snapshot.palette.length() - 1;
}

static int worldsnapshotcubeindex(const cube &source)
{
#if defined(STANDALONE) || defined(WORLD_SNAPSHOT_SERVER_CODEC)
    loopi(6) if(source.texture[i] != source.texture[0]) return -1;
    return int(source.texture[0]);
#else
    return getworldcubebytextures(source.texture);
#endif
}

static bool captureworldsnapshotvoxel(const cube *root, worldchunksnapshot &snapshot, int x, int y, int z,
                                      worldsnapshotvoxel &voxel, string &error)
{
    int size;
    const cube &source = worldsnapshotcubeat(root, ivec(x * WORLD_BLOCK_SIZE, y * WORLD_BLOCK_SIZE, z * WORLD_BLOCK_SIZE), size);
    if(source.children)
    {
        formatstring(error, "voxel at %d %d %d contains sub-voxel octree state", x, y, z);
        return false;
    }
    const bool empty = isempty(source), solid = isentirelysolid(source);
    ASSERT(size <= WORLD_BLOCK_SIZE || empty || solid);
    const int worldindex = empty ? -1 : worldsnapshotcubeindex(source);
    const char *id = empty ? "air" : worldindex >= 0 ? getworldcubename(worldindex) : NULL;
    if(!id || !id[0])
    {
        formatstring(error, "voxel at %d %d %d has no stable block string ID", x, y, z);
        return false;
    }
    const int palette = worldsnapshotpaletteindex(snapshot, id, worldindex);
    if(palette < 0) { copystring(error, "chunk block palette exceeds ushort capacity"); return false; }
    voxel.palette = ushort(palette);
    voxel.material = source.material;
    voxel.orientation = O_TOP;
    voxel.flags = empty ? WORLD_SNAPSHOT_EMPTY : solid ? WORLD_SNAPSHOT_SOLID : 0;
    if(empty) memset(voxel.edges, 0, sizeof(voxel.edges));
    else if(solid) memset(voxel.edges, 0x80, sizeof(voxel.edges));
    else memcpy(voxel.edges, source.edges, sizeof(voxel.edges));
    return true;
}

static bool captureworldchunksnapshot(cube *root, int chunkx, int chunky, const worldsectionrenderdata &renderdata,
                                      const vector<worldscatterinstance> &scatter, const vector<uchar> &gameplay,
                                      worldchunksnapshot &snapshot, string &error)
{
    if(!root) { copystring(error, "chunk is not in a saveable state"); return false; }
    snapshot.x = chunkx;
    snapshot.y = chunky;
    snapshot.renderdata = renderdata;
    loopi(8) if(!normalizeworldsnapshotcube(root[i], WORLD_CHUNK_ROOT_SIZE, error)) return false;
    snapshot.columns.reserve(WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS);
    loopi(WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS) snapshot.columns.add();
    loop(y, WORLD_CHUNK_BLOCKS) loop(x, WORLD_CHUNK_BLOCKS)
    {
        worldsnapshotcolumn &column = snapshot.columns[y * WORLD_CHUNK_BLOCKS + x];
        worldsnapshotvoxel previous;
        int runlength = 0;
        loop(z, WORLD_HEIGHT_BLOCKS)
        {
            worldsnapshotvoxel voxel;
            if(!captureworldsnapshotvoxel(root, snapshot, x, y, z, voxel, error)) return false;
            if(runlength && voxel == previous) ++runlength;
            else
            {
                if(runlength) column.runs.add(worldsnapshotrun(runlength, previous));
                previous = voxel;
                runlength = 1;
            }
        }
        column.runs.add(worldsnapshotrun(runlength, previous));
    }
    loopv(scatter)
    {
        const worldscatterinstance &source = scatter[i];
        const char *id = getworldscattername(source.type);
        if(!id || !id[0] || source.x < 0 || source.x >= WORLD_CHUNK_SIZE || source.y < 0 || source.y >= WORLD_CHUNK_SIZE ||
           source.z < 0 || source.z >= WORLD_MAP_SIZE)
        {
            copystring(error, "chunk contains invalid persistent placeable state");
            return false;
        }
        worldsnapshotscatter &record = snapshot.scatter.add();
        record.x = ushort(source.x);
        record.y = ushort(source.y);
        record.z = ushort(source.z);
        copystring(record.id, id);
        record.type = source.type;
        record.orientation = source.orient;
    }
    if(!gameplay.empty()) snapshot.gameplay.put(gameplay.getbuf(), gameplay.length());
    return true;
}

static bool serializeworldsnapshotvox(const worldchunksnapshot &snapshot, vector<uchar> &output)
{
    output.put((const uchar *)"CCVX", 4);
    worldsnapshotputuint(output, WORLD_SNAPSHOT_VOX_VERSION);
    worldsnapshotputuint(output, uint(snapshot.x));
    worldsnapshotputuint(output, uint(snapshot.y));
    worldsnapshotputushort(output, WORLD_CHUNK_BLOCKS);
    worldsnapshotputushort(output, WORLD_CHUNK_BLOCKS);
    worldsnapshotputushort(output, WORLD_HEIGHT_BLOCKS);
    worldsnapshotputushort(output, ushort(snapshot.palette.length()));
    worldsnapshotputuint(output, uint(snapshot.columns.length()));
    worldsnapshotputuint(output, snapshot.revision);
    output.add(snapshot.playeredited ? 1 : 0);
    output.put(&snapshot.renderdata.flags[0][0], sizeof(snapshot.renderdata.flags));
    loopv(snapshot.palette) if(!worldsnapshotputstring(output, snapshot.palette[i].id)) return false;
    loopv(snapshot.columns)
    {
        const worldsnapshotcolumn &column = snapshot.columns[i];
        worldsnapshotputushort(output, ushort(column.runs.length()));
        loopvj(column.runs)
        {
            const worldsnapshotrun &run = column.runs[j];
            worldsnapshotputushort(output, run.length);
            worldsnapshotputushort(output, run.voxel.palette);
            output.add(run.voxel.orientation);
            output.add(run.voxel.flags);
            worldsnapshotputushort(output, run.voxel.material);
            output.put(run.voxel.edges, sizeof(run.voxel.edges));
        }
    }
    worldsnapshotputuint(output, uint(crc32(0, (const Bytef *)output.getbuf(), uInt(output.length()))));
    return output.length() <= WORLD_SNAPSHOT_MAX_FILE_SIZE;
}

static bool serializeworldsnapshotdat(const worldchunksnapshot &snapshot, uint voxchecksum, vector<uchar> &output)
{
    output.put((const uchar *)"CCDT", 4);
    worldsnapshotputuint(output, WORLD_SNAPSHOT_DAT_VERSION);
    worldsnapshotputuint(output, uint(snapshot.x));
    worldsnapshotputuint(output, uint(snapshot.y));
    worldsnapshotputuint(output, voxchecksum);
    worldsnapshotputuint(output, uint(snapshot.scatter.length()));
    loopv(snapshot.scatter)
    {
        const worldsnapshotscatter &record = snapshot.scatter[i];
        output.add(WORLD_SNAPSHOT_SCATTER);
        worldsnapshotputushort(output, record.x);
        worldsnapshotputushort(output, record.y);
        worldsnapshotputushort(output, record.z);
        output.add(uchar(record.orientation));
        if(!worldsnapshotputstring(output, record.id)) return false;
    }
    worldsnapshotputuint(output, uint(snapshot.gameplay.length()));
    if(!snapshot.gameplay.empty()) output.put(snapshot.gameplay.getbuf(), snapshot.gameplay.length());
    worldsnapshotputuint(output, uint(crc32(0, (const Bytef *)output.getbuf(), uInt(output.length()))));
    return output.length() <= WORLD_SNAPSHOT_MAX_FILE_SIZE;
}

static bool serializeworldchunksnapshot(cube *root, int x, int y, uint revision, bool playeredited,
                                        const worldsectionrenderdata &renderdata, const vector<worldscatterinstance> &scatter,
                                        const vector<uchar> &gameplay, vector<uchar> &vox, vector<uchar> &dat, string &error)
{
    worldchunksnapshot snapshot(x, y);
    snapshot.revision = max(revision, 1U);
    snapshot.playeredited = playeredited;
    if(!captureworldchunksnapshot(root, x, y, renderdata, scatter, gameplay, snapshot, error) || !serializeworldsnapshotvox(snapshot, vox))
    {
        if(!error[0]) copystring(error, "could not serialize voxel data");
        return false;
    }
    const int checksumoffset = vox.length() - 4;
    const uint voxchecksum = uint(vox[checksumoffset]) | uint(vox[checksumoffset + 1]) << 8 | uint(vox[checksumoffset + 2]) << 16 |
                             uint(vox[checksumoffset + 3]) << 24;
    if(!serializeworldsnapshotdat(snapshot, voxchecksum, dat))
    {
        copystring(error, "could not serialize sparse data");
        return false;
    }
    return true;
}

#if !defined(STANDALONE) && !defined(WORLD_SNAPSHOT_SERVER_CODEC)
static bool writeworldchunksnapshot(const char *folder, int x, int y, uint revision, bool playeredited, bool compress, cube *root,
                                    const worldsectionrenderdata &renderdata, const vector<worldscatterinstance> &scatter,
                                    const vector<uchar> &gameplay, string &error)
{
    vector<uchar> vox, dat;
    if(!serializeworldchunksnapshot(root, x, y, revision, playeredited, renderdata, scatter, gameplay, vox, dat, error)) return false;
    string voxname, datname;
    worldchunksnapshotfilename(voxname, sizeof(voxname), folder, x, y, "vox");
    worldchunksnapshotfilename(datname, sizeof(datname), folder, x, y, "dat");
    if(!writeworldsnapshotfile(voxname, vox, compress) || !writeworldsnapshotfile(datname, dat, compress))
    {
        copystring(error, "could not write authoritative chunk snapshot files");
        return false;
    }
    return true;
}
#endif

static bool validateworldsnapshotchecksum(const vector<uchar> &contents)
{
    if(contents.length() < 4) return false;
    worldsnapshotreader reader(contents.getbuf() + contents.length() - 4, 4);
    uint stored;
    return reader.readuint(stored) && stored == uint(crc32(0, (const Bytef *)contents.getbuf(), uInt(contents.length() - 4)));
}

static bool deserializeworldsnapshotvox(const vector<uchar> &contents, int x, int y, worldchunksnapshot &snapshot,
                                        vector<worldsnapshotvoxel> &voxels, string &error)
{
    if(!validateworldsnapshotchecksum(contents)) { copystring(error, "invalid .vox checksum"); return false; }
    worldsnapshotreader reader(contents.getbuf(), contents.length() - 4);
    char magic[4];
    uint version, storedx, storedy, columns;
    uint revision;
    uchar playeredited;
    ushort width, depth, height, palettesize;
    if(!reader.read(magic, 4) || memcmp(magic, "CCVX", 4) || !reader.readuint(version) || version != WORLD_SNAPSHOT_VOX_VERSION ||
       !reader.readuint(storedx) || !reader.readuint(storedy) || int(storedx) != x || int(storedy) != y || !reader.readushort(width) ||
       !reader.readushort(depth) || !reader.readushort(height) || width != WORLD_CHUNK_BLOCKS || depth != WORLD_CHUNK_BLOCKS ||
       height != WORLD_HEIGHT_BLOCKS || !reader.readushort(palettesize) || !palettesize || !reader.readuint(columns) ||
       columns != WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS || !reader.readuint(revision) || !revision || !reader.readbyte(playeredited) ||
       playeredited > 1)
    {
        copystring(error, "invalid or incompatible .vox header");
        return false;
    }
    snapshot.revision = revision;
    snapshot.playeredited = playeredited != 0;
    if(!reader.read(&snapshot.renderdata.flags[0][0], sizeof(snapshot.renderdata.flags)))
    {
        copystring(error, "missing chunk section classification");
        return false;
    }
    const int validsectionflags = SECTION_EXTERIOR | SECTION_INTERIOR | SECTION_CAVE_ENTRANCE | SECTION_WATER | SECTION_FULLY_SOLID |
                                  SECTION_NO_RENDER;
    loop(section, WORLD_SECTION_LAYERS) loop(tile, WORLD_SECTION_TILES)
        if(snapshot.renderdata.flags[section][tile] & ~validsectionflags)
        {
            copystring(error, "invalid chunk section classification");
            return false;
        }
    loopi(palettesize)
    {
        worldsnapshotpaletteentry &entry = snapshot.palette.add();
        if(!worldsnapshotreadstring(reader, entry.id, sizeof(entry.id))) { copystring(error, "invalid block palette"); return false; }
        if(!strcmp(entry.id, "air")) entry.worldindex = -1;
        else
        {
            entry.worldindex = getworldcubeidindex(entry.id);
            if(entry.worldindex < 0) { formatstring(error, "unknown block ID '%s'", entry.id); return false; }
        }
    }
    const int voxelcount = WORLD_CHUNK_BLOCKS * WORLD_CHUNK_BLOCKS * WORLD_HEIGHT_BLOCKS;
    voxels.reserve(voxelcount);
    loopi(voxelcount) voxels.add();
    loop(column, int(columns))
    {
        ushort runcount;
        if(!reader.readushort(runcount) || !runcount || runcount > WORLD_HEIGHT_BLOCKS) return false;
        int z = 0;
        loopj(runcount)
        {
            ushort length, palette, material;
            uchar orientation, flags, edges[12];
            if(!reader.readushort(length) || !length || z + length > WORLD_HEIGHT_BLOCKS || !reader.readushort(palette) || palette >= palettesize ||
               !reader.readbyte(orientation) || !reader.readbyte(flags) || flags & ~(WORLD_SNAPSHOT_EMPTY | WORLD_SNAPSHOT_SOLID) ||
               ((flags & WORLD_SNAPSHOT_EMPTY) && (flags & WORLD_SNAPSHOT_SOLID)) || !reader.readushort(material) ||
               !reader.read(edges, sizeof(edges)) || orientation < O_LEFT || orientation > O_TOP)
            {
                copystring(error, "invalid voxel RLE record");
                return false;
            }
            worldsnapshotvoxel voxel;
            voxel.palette = palette;
            voxel.material = material;
            voxel.orientation = orientation;
            voxel.flags = flags;
            memcpy(voxel.edges, edges, sizeof(edges));
            loopk(length) voxels[column * WORLD_HEIGHT_BLOCKS + z++] = voxel;
        }
        if(z != WORLD_HEIGHT_BLOCKS) { copystring(error, "column RLE does not cover 512 voxels"); return false; }
    }
    if(!reader.finished()) { copystring(error, "trailing data in .vox"); return false; }
    return true;
}

static bool deserializeworldsnapshotdat(const vector<uchar> &contents, int x, int y, uint expectedvoxchecksum, worldchunksnapshot &snapshot,
                                        string &error)
{
    if(!validateworldsnapshotchecksum(contents)) { copystring(error, "invalid .dat checksum"); return false; }
    worldsnapshotreader reader(contents.getbuf(), contents.length() - 4);
    char magic[4];
    uint version, storedx, storedy, voxchecksum, count;
    if(!reader.read(magic, 4) || memcmp(magic, "CCDT", 4) || !reader.readuint(version) || version != WORLD_SNAPSHOT_DAT_VERSION ||
       !reader.readuint(storedx) || !reader.readuint(storedy) || int(storedx) != x || int(storedy) != y || !reader.readuint(voxchecksum) ||
       voxchecksum != expectedvoxchecksum || !reader.readuint(count) || count > 1000000U)
        return false;
    loopi(count)
    {
        uchar type, orientation;
        worldsnapshotscatter record;
        if(!reader.readbyte(type) || type != WORLD_SNAPSHOT_SCATTER || !reader.readushort(record.x) || !reader.readushort(record.y) ||
           !reader.readushort(record.z) || !reader.readbyte(orientation) || !worldsnapshotreadstring(reader, record.id, sizeof(record.id)) ||
           record.x >= WORLD_CHUNK_SIZE || record.y >= WORLD_CHUNK_SIZE || record.z >= WORLD_MAP_SIZE || orientation < O_LEFT || orientation > O_TOP)
        {
            copystring(error, "invalid sparse .dat record");
            return false;
        }
        record.type = getworldscatteridindex(record.id);
        if(record.type < 0) { formatstring(error, "unknown placeable ID '%s'", record.id); return false; }
        record.orientation = orientation;
        snapshot.scatter.add(record);
    }
    uint gameplaylength;
    if(!reader.readuint(gameplaylength) || gameplaylength > uint(reader.end - reader.position)) return false;
    if(gameplaylength && !reader.read(snapshot.gameplay.pad(int(gameplaylength)), int(gameplaylength))) return false;
    return reader.finished();
}

static void buildworldsnapshotcube(cube &destination, const ivec &origin, int size, const worldchunksnapshot &snapshot,
                                   const vector<worldsnapshotvoxel> &voxels)
{
    resetworldsnapshotcube(destination);
    if(origin.x >= WORLD_CHUNK_SIZE || origin.y >= WORLD_CHUNK_SIZE || origin.z >= WORLD_MAP_SIZE) return;
    if(size == WORLD_BLOCK_SIZE)
    {
        const int x = origin.x / WORLD_BLOCK_SIZE, y = origin.y / WORLD_BLOCK_SIZE, z = origin.z / WORLD_BLOCK_SIZE;
        const worldsnapshotvoxel &voxel = voxels[(y * WORLD_CHUNK_BLOCKS + x) * WORLD_HEIGHT_BLOCKS + z];
        destination.material = voxel.material;
        memcpy(destination.edges, voxel.edges, sizeof(destination.edges));
        const int worldindex = snapshot.palette[voxel.palette].worldindex;
        if(worldindex >= 0)
        {
#if defined(STANDALONE) || defined(WORLD_SNAPSHOT_SERVER_CODEC)
            loopi(6) destination.texture[i] = ushort(worldindex);
#else
            loopi(6) destination.texture[i] = getworldcubefaceslot(worldindex, i);
#endif
        }
        return;
    }
    const int childsize = size >> 1;
    destination.children = allocworldsnapshotfamily();
    loopi(8) buildworldsnapshotcube(destination.children[i], ivec(i, origin, childsize), childsize, snapshot, voxels);
    const cube &first = destination.children[0];
    bool identical = !first.children && (isempty(first) || isentirelysolid(first));
    loopi(8) if(identical)
    {
        const cube &child = destination.children[i];
        identical = !child.children && child.material == first.material && !memcmp(child.edges, first.edges, sizeof(first.edges)) &&
                    !memcmp(child.texture, first.texture, sizeof(first.texture));
    }
    if(identical)
    {
        cube *children = destination.children;
        destination = first;
        destination.children = NULL;
        freeworldsnapshotfamily(children);
    }
}

static worldsnapshotloadresult loadworldchunksnapshotdata(const char *folder, int x, int y, cube *&root, vector<worldscatterinstance> &scatter,
                                                           worldsectionrenderdata &renderdata, vector<uchar> &gameplay, string &error,
                                                           uint *revision = NULL, bool *playeredited = NULL, vector<uchar> *voxpayload = NULL,
                                                           vector<uchar> *datpayload = NULL, bool allowcompression = true)
{
    root = NULL;
    scatter.setsize(0);
    string voxname, datname;
    worldchunksnapshotfilename(voxname, sizeof(voxname), folder, x, y, "vox");
    worldchunksnapshotfilename(datname, sizeof(datname), folder, x, y, "dat");
    const char *found = findfile(voxname, "rb");
    const bool hasvox = found && fileexists(found, "r");
    found = findfile(datname, "rb");
    const bool hasdat = found && fileexists(found, "r");
    if(!hasvox && !hasdat) return WORLD_SNAPSHOT_MISSING;
    if(!hasvox || !hasdat) { copystring(error, "authoritative chunk is missing its .vox or .dat sidecar"); return WORLD_SNAPSHOT_INVALID; }
    vector<uchar> voxcontents, datcontents;
    if(!readworldsnapshotfile(voxname, voxcontents, allowcompression) || !readworldsnapshotfile(datname, datcontents, allowcompression))
    {
        copystring(error, "could not read .vox/.dat files");
        return WORLD_SNAPSHOT_INVALID;
    }
    worldchunksnapshot snapshot(x, y);
    vector<worldsnapshotvoxel> voxels;
    if(!deserializeworldsnapshotvox(voxcontents, x, y, snapshot, voxels, error)) return WORLD_SNAPSHOT_INVALID;
    const int checksumoffset = voxcontents.length() - 4;
    const uint voxchecksum = uint(voxcontents[checksumoffset]) | uint(voxcontents[checksumoffset + 1]) << 8 |
                              uint(voxcontents[checksumoffset + 2]) << 16 | uint(voxcontents[checksumoffset + 3]) << 24;
    if(!deserializeworldsnapshotdat(datcontents, x, y, voxchecksum, snapshot, error)) return WORLD_SNAPSHOT_INVALID;
    root = allocworldsnapshotfamily();
    loopi(8) buildworldsnapshotcube(root[i], ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE), WORLD_CHUNK_ROOT_SIZE, snapshot, voxels);
    loopv(snapshot.scatter)
        scatter.add(worldscatterinstance(snapshot.scatter[i].x, snapshot.scatter[i].y, snapshot.scatter[i].z, snapshot.scatter[i].type,
                                         snapshot.scatter[i].orientation));
#if !defined(STANDALONE) && !defined(WORLD_SNAPSHOT_SERVER_CODEC)
    game::cacheworldscattertransforms(x, y, game::getworldscattermaxoffset(), scatter);
#endif
    renderdata = snapshot.renderdata;
    gameplay.move(snapshot.gameplay);
    if(revision) *revision = snapshot.revision;
    if(playeredited) *playeredited = snapshot.playeredited;
    if(voxpayload) voxpayload->move(voxcontents);
    if(datpayload) datpayload->move(datcontents);
    return WORLD_SNAPSHOT_LOADED;
}

#if !defined(STANDALONE) && !defined(WORLD_SNAPSHOT_SERVER_CODEC)
static worldsnapshotloadresult loadworldchunksnapshot(const char *folder, int x, int y, cube *&root, vector<worldscatterinstance> &scatter,
                                                       worldsectionrenderdata &renderdata, string &error, uint *revision = NULL,
                                                       bool *playeredited = NULL, bool allowcompression = true)
{
    vector<uchar> gameplay;
    const worldsnapshotloadresult result = loadworldchunksnapshotdata(folder, x, y, root, scatter, renderdata, gameplay, error, revision,
                                                                       playeredited, NULL, NULL, allowcompression);
    if(result != WORLD_SNAPSHOT_LOADED) return result;
    if(game::restorelocalchunkdata(x, y, gameplay.getbuf(), gameplay.length())) return result;
    copystring(error, "invalid sparse gameplay data");
    freeworldsnapshotfamily(root);
    root = NULL;
    scatter.setsize(0);
    return WORLD_SNAPSHOT_INVALID;
}
#endif

#endif
