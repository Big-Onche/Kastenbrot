#ifndef __ENGINE_WORLDRUNTIME_H__
#define __ENGINE_WORLDRUNTIME_H__

enum
{
    WORLD_GRID_POWER = 4,
    WORLD_BLOCK_SIZE = 1 << WORLD_GRID_POWER,
    WORLD_CHUNK_BLOCKS = 64,
    WORLD_CHUNK_SIZE = WORLD_BLOCK_SIZE * WORLD_CHUNK_BLOCKS,
    WORLD_SECTION_BLOCKS = 16,
    WORLD_SECTION_SIZE = WORLD_BLOCK_SIZE * WORLD_SECTION_BLOCKS,
    WORLD_MIN_HEIGHT = -256,
    WORLD_MAX_HEIGHT = 256,
    WORLD_HEIGHT_BLOCKS = WORLD_MAX_HEIGHT - WORLD_MIN_HEIGHT,
    WORLD_MAP_SIZE = WORLD_HEIGHT_BLOCKS * WORLD_BLOCK_SIZE,
    WORLD_CHUNK_SCALE = WORLD_GRID_POWER + 9,
    WORLD_CHUNK_MAP_SIZE = 1 << WORLD_CHUNK_SCALE,
    WORLD_CHUNK_ROOT_SIZE = WORLD_CHUNK_MAP_SIZE >> 1,
    WORLD_SECTION_LAYERS = WORLD_MAP_SIZE / WORLD_SECTION_SIZE,
    WORLD_GROUND_HEIGHT = -WORLD_MIN_HEIGHT * WORLD_BLOCK_SIZE,
    WORLD_RUNTIME_SCALE = 16,
    WORLD_RUNTIME_SIZE = 1 << WORLD_RUNTIME_SCALE,
    WORLD_RUNTIME_CHUNKS = WORLD_RUNTIME_SIZE / WORLD_CHUNK_SIZE,
    WORLD_RUNTIME_CENTER = WORLD_RUNTIME_CHUNKS / 2,
    WORLD_MAX_CHUNK_DIST = WORLD_RUNTIME_CENTER - 2,
    WORLD_SECTION_COLUMNS = WORLD_CHUNK_SIZE / WORLD_SECTION_SIZE,
    WORLD_SECTION_TILES = WORLD_SECTION_COLUMNS * WORLD_SECTION_COLUMNS,
    WORLD_SECTION_FACE_COUNT = 6,
    WORLD_SECTION_CELL_COUNT = WORLD_SECTION_BLOCKS * WORLD_SECTION_BLOCKS * WORLD_SECTION_BLOCKS,
    WORLD_SECTION_FACE_WORDS = (WORLD_SECTION_BLOCKS * WORLD_SECTION_BLOCKS + 31) / 32,
    WORLD_MAX_PREPARED_CHUNKS = 8,
    WORLD_MAX_COLUMN_CHANGES = 64,
    WORLD_MAX_SECTION_BATCH = 16,
    WORLD_SECTION_PREFETCH_MARGIN = 1
};

enum
{
    WORLD_SAVE_FORMAT_VERSION = 2,
    WORLDGEN_VERSION = 8,
    WORLD_DIFF_Z = 0,
    WORLD_DIFF_FRAME_MAX = 64 << 20,
    WORLD_DIFF_FLUSH_MILLIS = 10000
};

enum
{
    WORLD_SECTION_CONTENT = 1 << 0,
    WORLD_SECTION_OPAQUE = 1 << 1
};

enum
{
    SECTION_EXTERIOR = 1 << 0,
    SECTION_INTERIOR = 1 << 1,
    SECTION_CAVE_ENTRANCE = 1 << 2,
    SECTION_WATER = 1 << 3,
    SECTION_FULLY_SOLID = 1 << 4,
    SECTION_NO_RENDER = 1 << 5
};

enum
{
    NOT_RESIDENT = 0,
    PENDING_BUILD,
    PENDING_UPLOAD,
    RESIDENT,
    EVICTABLE
};

enum
{
    WORLD_VA_EXTERIOR = 0,
    WORLD_VA_INTERIOR,
    WORLD_VA_GEOMETRY_COUNT
};

struct worldsectionvaresidency
{
    uchar state[WORLD_VA_GEOMETRY_COUNT];
    int lastwanted[WORLD_VA_GEOMETRY_COUNT];
};

struct worldsectionrenderdata
{
    uchar flags[WORLD_SECTION_LAYERS][WORLD_SECTION_TILES];

    worldsectionrenderdata() { clear(); }

    void clear() { memclear(flags); }
};

struct worldscatterinstance
{
    int x, y, z, type, orient;
    mutable float renderoffsetx, renderoffsety, rendermaxoffset;
    mutable int renderyaw;
    mutable bool rendertransformvalid;

    worldscatterinstance()
        : x(0), y(0), z(0), type(-1), orient(O_TOP), renderoffsetx(0), renderoffsety(0), rendermaxoffset(0), renderyaw(0),
          rendertransformvalid(false)
    {
    }

    worldscatterinstance(int x, int y, int z, int type, int orient = O_TOP)
        : x(x), y(y), z(z), type(type), orient(orient), renderoffsetx(0), renderoffsety(0), rendermaxoffset(0), renderyaw(0),
          rendertransformvalid(false)
    {
    }

    bool operator==(const worldscatterinstance &other) const
    {
        return x == other.x && y == other.y && z == other.z && type == other.type && orient == other.orient;
    }
};

struct worlddiffnode
{
    int x, y, z, size;
    uchar edges[12];
    ushort texture[6], material;
    int block;

    worlddiffnode() : x(0), y(0), z(0), size(0), material(MAT_AIR), block(-1)
    {
        memset(edges, 0, sizeof(edges));
        loopi(6) texture[i] = DEFAULT_GEOM;
    }
};

struct worldchunkdirtybounds
{
    ivec minimum, maximum;
    bool valid;

    worldchunkdirtybounds() : minimum(0, 0, 0), maximum(0, 0, 0), valid(false) {}

    void include(const worlddiffnode &node)
    {
        const ivec nodemin(node.x, node.y, node.z), nodemax = ivec(nodemin).add(node.size);
        if(!valid)
        {
            minimum = nodemin;
            maximum = nodemax;
            valid = true;
        }
        else
        {
            minimum.min(nodemin);
            maximum.max(nodemax);
        }
    }

    void expandforrebuild()
    {
        if(!valid) return;
        minimum.sub(1).max(0);
        maximum.add(1).min(WORLD_CHUNK_MAP_SIZE);
    }
};

struct worldeditrecord
{
    int chunkx, chunky, chunkz, operation, author;
    int args[4];
    ullong revision, timestamp;
    selinfo selection;
    vector<worlddiffnode> before, after;
    vector<worldscatterinstance> scatterbefore, scatterafter;

    worldeditrecord() : chunkx(0), chunky(0), chunkz(WORLD_DIFF_Z), operation(0), author(-1), revision(0), timestamp(0)
    {
        memset(args, 0, sizeof(args));
    }
};

struct worldchunkdiffstate
{
    int x, y, z;
    ullong revision, snapshotrevision, canonicalhash;
    vector<worldeditrecord *> pending, journal, audit;

    worldchunkdiffstate(int x, int y, int z = WORLD_DIFF_Z) : x(x), y(y), z(z), revision(0), snapshotrevision(0), canonicalhash(0) {}

    ~worldchunkdiffstate()
    {
        pending.deletecontents();
        journal.deletecontents();
        audit.deletecontents();
    }
};

struct worldgencubetextures
{
    string id;
    int top, side, bottom;

    worldgencubetextures(const char *id = "", int top = DEFAULT_GEOM, int side = DEFAULT_GEOM, int bottom = DEFAULT_GEOM)
        : top(top), side(side), bottom(bottom)
    {
        copystring(this->id, id);
    }
};

struct worldchunk
{
    int x, y;
    cube *root;
    vector<worldscatterinstance> scatter;
    uint mountedtiles[WORLD_SECTION_LAYERS];
    uint contentknown[WORLD_SECTION_LAYERS], contenttiles[WORLD_SECTION_LAYERS], opaqueknown[WORLD_SECTION_LAYERS], opaquetiles[WORLD_SECTION_LAYERS],
         portalsknown[WORLD_SECTION_LAYERS], visibletiles[WORLD_SECTION_LAYERS];
    uchar portals[WORLD_SECTION_LAYERS][WORLD_SECTION_TILES][WORLD_SECTION_FACE_COUNT],
          reachablefaces[WORLD_SECTION_LAYERS][WORLD_SECTION_TILES];
    uint portalcellmasks[WORLD_SECTION_LAYERS][WORLD_SECTION_TILES][WORLD_SECTION_FACE_COUNT][WORLD_SECTION_FACE_WORDS];
    worldsectionrenderdata renderdata;
    worldsectionvaresidency varesidency[WORLD_SECTION_LAYERS][WORLD_SECTION_TILES];
    uint request;
    bool loading, generating, saved, dirty, corrupted;

    worldchunk(int x, int y, cube *root, bool loading = false, bool saved = false)
        : x(x), y(y), root(root), request(0), loading(loading), generating(false), saved(saved), dirty(false), corrupted(false)
    {
        memclear(mountedtiles);
        memclear(contentknown);
        memclear(contenttiles);
        memclear(opaqueknown);
        memclear(opaquetiles);
        memclear(portalsknown);
        memclear(portals);
        memclear(portalcellmasks);
        memclear(reachablefaces);
        memclear(visibletiles);
        memclear(varesidency);
    }
};

struct worldsectionowner
{
    int chunkx, chunky;
    ushort section, tile;

    worldsectionowner() : chunkx(0), chunky(0), section(0), tile(0) {}
    worldsectionowner(int chunkx, int chunky, int section, int tile) : chunkx(chunkx), chunky(chunky), section(section), tile(tile) {}

    bool matches(const worldchunk &chunk, int section, int tile) const
    {
        return chunkx == chunk.x && chunky == chunk.y && this->section == section && this->tile == tile;
    }
};

struct worldgencontext;

struct worldchunkjob
{
    int x, y, families, optimized, loaderror;
    uint contenttiles[WORLD_SECTION_LAYERS], opaquetiles[WORLD_SECTION_LAYERS];
    uchar portals[WORLD_SECTION_LAYERS][WORLD_SECTION_TILES][WORLD_SECTION_FACE_COUNT];
    uint portalcellmasks[WORLD_SECTION_LAYERS][WORLD_SECTION_TILES][WORLD_SECTION_FACE_COUNT][WORLD_SECTION_FACE_WORDS];
    worldsectionrenderdata renderdata;
    ullong revision, canonicalhash;
    uint epoch, request;
    bool loaded, cached, remip, leavesalpha, sectionstatesready;
    SDL_atomic_t cancelled;
    cube *root;
    vector<worldscatterinstance> scatter;
    vector<uchar> cachepayload;
    string filename, cachefilename;
    int seed;
    ullong worldgenhash;
    worldgencontext *generation;

    worldchunkjob(int x, int y, uint epoch, uint request);
    ~worldchunkjob();
};

#endif
