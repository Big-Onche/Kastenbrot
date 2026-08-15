// worldlod.cpp: lightweight Kastenbrot distant terrain surfaces

#ifdef WORLDIO_MODULE_IMPLEMENTATION

VARP(worldlod, 0, 1, 1);
VARP(worldlod1resolution, 4, 32, WORLD_CHUNK_BLOCKS);
VARP(worldlod2resolution, 4, 16, WORLD_CHUNK_BLOCKS);
VAR(worldlodneardistance, 16, 192, 4096); // blocks from the chunk AABB
VAR(worldlodfardistance, 32, 512, 4096);
VAR(worldlodmaxdistance, 64, 768, 4096);
VARP(worldlodhysteresis, 0, 16, 512);
VARP(worldlodskirtdepth, 1, 4, 64);
VARP(worldlodthreads, 1, 2, 4);
VARP(worldlodpendinglimit, 4, 32, 512);
VARP(worldlodcachelimit, 16, 384, 4096);
VARP(worldloduploadlimit, 1, 4, 16);
VARP(worldloddebug, 0, 0, 3);
VARP(worldlodwireframe, 0, 0, 1);

enum
{
    WORLD_LOD_GRASS_TOP = 0,
    WORLD_LOD_GRASS_SIDE,
    WORLD_LOD_DIRT,
    WORLD_LOD_STONE,
    WORLD_LOD_SAND,
    WORLD_LOD_SNOW,
    WORLD_LOD_WATER,
    WORLD_LOD_MATERIALS
};

struct worldlodvertex
{
    vec position, normal;
    vec2 texcoord;
    bvec4 material;

    worldlodvertex(const vec &position = vec(0, 0, 0), const vec &normal = vec(0, 0, 1), const vec2 &texcoord = vec2(0, 0),
                   int material = WORLD_LOD_GRASS_TOP)
        : position(position), normal(normal), texcoord(texcoord), material(uchar(material), 0, 0, 255) {}
};

struct worldlodcpumesh
{
    vector<worldlodvertex> vertices;
    vector<uint> indices;
    int terrainindices, waterindices, topfaces, sidefaces;
    vec bbmin, bbmax;

    worldlodcpumesh() : terrainindices(0), waterindices(0), topfaces(0), sidefaces(0), bbmin(0, 0, 0), bbmax(0, 0, 0) {}
};

struct worldlodkey
{
    int x, y, lod, resolution, skirtdepth, seed;
    ullong generation;

    worldlodkey(int x = 0, int y = 0, int lod = 1, int resolution = 0, int skirtdepth = 0, int seed = 0, ullong generation = 0)
        : x(x), y(y), lod(lod), resolution(resolution), skirtdepth(skirtdepth), seed(seed), generation(generation) {}

    bool operator==(const worldlodkey &other) const
    {
        return x == other.x && y == other.y && lod == other.lod && resolution == other.resolution && skirtdepth == other.skirtdepth &&
               seed == other.seed && generation == other.generation;
    }
};

struct worldlodchunk
{
    worldlodkey key;
    GLuint vbo, ebo;
    int vertices, terrainindices, waterindices, topfaces, sidefaces, lastused;
    vec bbmin, bbmax;
    bool active;

    worldlodchunk(const worldlodkey &key = worldlodkey())
        : key(key), vbo(0), ebo(0), vertices(0), terrainindices(0), waterindices(0), topfaces(0), sidefaces(0), lastused(0), bbmin(0, 0, 0),
          bbmax(0, 0, 0), active(false) {}
};

struct worldlodjob
{
    worldlodkey key;
    int priority;
    float distance, alignment;
    uint epoch;
    SDL_atomic_t cancelled;
    worldlodcpumesh mesh;
    double generationmillis, samplingmillis, buildmillis;
    bool succeeded;

    worldlodjob(const worldlodkey &key, int priority, float distance, float alignment, uint epoch)
        : key(key), priority(priority), distance(distance), alignment(alignment), epoch(epoch), generationmillis(0), samplingmillis(0),
          buildmillis(0), succeeded(false)
    {
        SDL_AtomicSet(&cancelled, 0);
    }
};

struct worldlodselection
{
    int x, y, desired, active, lastseen;
    float distance;

    worldlodselection(int x = 0, int y = 0)
        : x(x), y(y), desired(0), active(-1), lastseen(0), distance(0) {}
};

static vector<worldlodchunk> worldlodcache;
static vector<worldlodselection> worldlodselections;
static vector<worldlodjob *> worldlodjobs, worldlodactivejobs, worldlodresults;
static vector<SDL_Thread *> worldlodworkers;
static SDL_mutex *worldlodmutex = NULL;
static SDL_cond *worldlodcond = NULL;
static bool stopworldlodthreads = false;
static uint worldlodepoch = 1;
static ullong worldlodsettings = 0;
static int worldlodcachehits = 0, worldlodcachemisses = 0;
static int worldlodrendervertices = 0, worldlodrendertriangles = 0, worldlodrendertopfaces = 0, worldlodrendersidefaces = 0;
static int worldlodmissingchunks = 0;
static double worldlodlastgeneration = 0, worldlodlastupload = 0;

static int worldlodtopmaterial(int material)
{
    switch(material)
    {
        case WORLD_SURFACE_STONE: return WORLD_LOD_STONE;
        case WORLD_SURFACE_SAND: return WORLD_LOD_SAND;
        case WORLD_SURFACE_SNOW: return WORLD_LOD_SNOW;
        case WORLD_SURFACE_DIRT: return WORLD_LOD_DIRT;
        default: return WORLD_LOD_GRASS_TOP;
    }
}

static int worldlodsidematerial(int material)
{
    return material == WORLD_SURFACE_GRASS ? WORLD_LOD_GRASS_SIDE : worldlodtopmaterial(material);
}

static int findworldlodselection(int x, int y)
{
    loopv(worldlodselections) if(worldlodselections[i].x == x && worldlodselections[i].y == y) return i;
    return -1;
}

static int findworldlodcache(const worldlodkey &key)
{
    loopv(worldlodcache) if(worldlodcache[i].key == key) return i;
    return -1;
}

static worldlodkey currentworldlodkey(int x, int y, int lod)
{
    return worldlodkey(x, y, lod, lod == 1 ? worldlod1resolution : worldlod2resolution, worldlodskirtdepth, game::getworldseed(),
                       game::worldgenerationparameterhash());
}

static bool worldlodpriorityless(int apriority, float adistance, float aalignment, int bpriority, float bdistance, float balignment)
{
    if(apriority != bpriority) return apriority < bpriority;
    if(adistance != bdistance) return adistance < bdistance;
    return aalignment > balignment;
}

static bool worldlodjobless(const worldlodjob &a, const worldlodjob &b)
{
    return worldlodpriorityless(a.priority, a.distance, a.alignment, b.priority, b.distance, b.alignment);
}

static vec2 worldlodtexcoord(const vec &position, int orient)
{
    switch(orient)
    {
        case O_LEFT: return vec2(position.y, -position.z).div(WORLD_BLOCK_SIZE);
        case O_RIGHT: return vec2(-position.y, -position.z).div(WORLD_BLOCK_SIZE);
        case O_BACK: return vec2(-position.x, -position.z).div(WORLD_BLOCK_SIZE);
        case O_FRONT: return vec2(position.x, -position.z).div(WORLD_BLOCK_SIZE);
        default: return vec2(position.x, position.y).div(WORLD_BLOCK_SIZE);
    }
}

static void addworldlodquad(worldlodcpumesh &mesh, const vec &a, const vec &b, const vec &c, const vec &d, const vec &normal, int material,
                            int orient)
{
    const uint first = mesh.vertices.length();
    mesh.vertices.add(worldlodvertex(a, normal, worldlodtexcoord(a, orient), material));
    mesh.vertices.add(worldlodvertex(b, normal, worldlodtexcoord(b, orient), material));
    mesh.vertices.add(worldlodvertex(c, normal, worldlodtexcoord(c, orient), material));
    mesh.vertices.add(worldlodvertex(d, normal, worldlodtexcoord(d, orient), material));
    mesh.indices.add(first); mesh.indices.add(first + 1); mesh.indices.add(first + 2);
    mesh.indices.add(first); mesh.indices.add(first + 2); mesh.indices.add(first + 3);
}

static void addworldlodtop(worldlodcpumesh &mesh, float x0, float y0, float x1, float y1, float z, int material)
{
    addworldlodquad(mesh, vec(x0, y0, z), vec(x1, y0, z), vec(x1, y1, z), vec(x0, y1, z), vec(0, 0, 1), material, O_TOP);
    mesh.topfaces++;
}

static void addworldlodsidequad(worldlodcpumesh &mesh, float x0, float y0, float x1, float y1, float bottom, float top, int orient, int material)
{
    switch(orient)
    {
        case O_LEFT:
            addworldlodquad(mesh, vec(x0, y1, top), vec(x0, y1, bottom), vec(x0, y0, bottom), vec(x0, y0, top), vec(-1, 0, 0), material, orient);
            break;
        case O_RIGHT:
            addworldlodquad(mesh, vec(x1, y1, top), vec(x1, y0, top), vec(x1, y0, bottom), vec(x1, y1, bottom), vec(1, 0, 0), material, orient);
            break;
        case O_BACK:
            addworldlodquad(mesh, vec(x1, y0, top), vec(x0, y0, top), vec(x0, y0, bottom), vec(x1, y0, bottom), vec(0, -1, 0), material, orient);
            break;
        default:
            addworldlodquad(mesh, vec(x0, y1, bottom), vec(x0, y1, top), vec(x1, y1, top), vec(x1, y1, bottom), vec(0, 1, 0), material, orient);
            break;
    }
    mesh.sidefaces++;
}

static void addworldlodcolumnside(worldlodcpumesh &mesh, float x0, float y0, float x1, float y1, float bottom, float top, int orient, int material)
{
    if(bottom >= top) return;
    if(material == WORLD_SURFACE_GRASS)
    {
        const float grassbottom = max(bottom, top - WORLD_BLOCK_SIZE);
        if(bottom < grassbottom) addworldlodsidequad(mesh, x0, y0, x1, y1, bottom, grassbottom, orient, WORLD_LOD_DIRT);
        addworldlodsidequad(mesh, x0, y0, x1, y1, grassbottom, top, orient, WORLD_LOD_GRASS_SIDE);
    }
    else addworldlodsidequad(mesh, x0, y0, x1, y1, bottom, top, orient, worldlodsidematerial(material));
}

static void addworldlodskirt(worldlodcpumesh &mesh, const vector<int> &heights, const vector<uchar> &materials, int resolution, int edge,
                             float skirtdepth)
{
    const int stride = resolution + 1;
    loopi(resolution)
    {
        int a, b;
        vec normal;
        bool reverse;
        switch(edge)
        {
            case 0: a = i; b = i + 1; normal = vec(0, -1, 0); reverse = true; break;
            case 1: a = resolution * stride + i; b = a + 1; normal = vec(0, 1, 0); reverse = false; break;
            case 2: a = i * stride; b = (i + 1) * stride; normal = vec(-1, 0, 0); reverse = false; break;
            default: a = i * stride + resolution; b = (i + 1) * stride + resolution; normal = vec(1, 0, 0); reverse = true; break;
        }
        const worldlodvertex &va = mesh.vertices[a], &vb = mesh.vertices[b];
        const uint first = mesh.vertices.length();
        const int material = worldlodsidematerial(materials[a]), orient = edge == 0 ? O_BACK : edge == 1 ? O_FRONT : edge == 2 ? O_LEFT : O_RIGHT;
        mesh.vertices.add(worldlodvertex(va.position, normal, worldlodtexcoord(va.position, orient), material));
        mesh.vertices.add(worldlodvertex(vb.position, normal, worldlodtexcoord(vb.position, orient), material));
        const vec lowerb = vec(vb.position).sub(vec(0, 0, skirtdepth)), lowera = vec(va.position).sub(vec(0, 0, skirtdepth));
        mesh.vertices.add(worldlodvertex(lowerb, normal, worldlodtexcoord(lowerb, orient), material));
        mesh.vertices.add(worldlodvertex(lowera, normal, worldlodtexcoord(lowera, orient), material));
        if(reverse)
        {
            mesh.indices.add(first); mesh.indices.add(first + 3); mesh.indices.add(first + 2);
            mesh.indices.add(first); mesh.indices.add(first + 2); mesh.indices.add(first + 1);
        }
        else
        {
            mesh.indices.add(first); mesh.indices.add(first + 1); mesh.indices.add(first + 2);
            mesh.indices.add(first); mesh.indices.add(first + 2); mesh.indices.add(first + 3);
        }
        mesh.sidefaces++;
    }
}

struct worldlodcolumn
{
    int height, waterheight, material;

    worldlodcolumn() : height(0), waterheight(0), material(WORLD_SURFACE_GRASS) {}
};

static bool sampleworldlodcolumn(worldgencontext *generation, const worldlodkey &key, int cellx, int celly, worldlodcolumn &column)
{
    const int beginx = int(floor(double(cellx) * WORLD_CHUNK_BLOCKS / key.resolution)),
              endx = int(ceil(double(cellx + 1) * WORLD_CHUNK_BLOCKS / key.resolution)) - 1,
              beginy = int(floor(double(celly) * WORLD_CHUNK_BLOCKS / key.resolution)),
              endy = int(ceil(double(celly + 1) * WORLD_CHUNK_BLOCKS / key.resolution)) - 1;
    bool selected = false;
    for(int y = beginy; y <= endy; ++y) for(int x = beginx; x <= endx; ++x)
    {
        worldsurfacesample surface;
        if(!game::sampleterrainsurface(generation, key.x * WORLD_CHUNK_BLOCKS + x, key.y * WORLD_CHUNK_BLOCKS + y, surface)) return false;
        if(!selected || surface.height > column.height)
        {
            column.height = surface.height;
            column.waterheight = surface.waterheight;
            column.material = surface.material;
            selected = true;
        }
    }
    return selected;
}

static bool buildworldlod1mesh(worldlodjob &job, worldgencontext *generation, Uint64 frequency)
{
    const int resolution = job.key.resolution, stride = resolution + 2, columns = stride * stride;
    vector<worldlodcolumn> samples;
    samples.pad(columns);

    const Uint64 samplingstart = SDL_GetPerformanceCounter();
    {
        ZoneScopedN("LOD/Surface sampling");
        for(int y = -1; y <= resolution; ++y) for(int x = -1; x <= resolution; ++x)
        {
            if(SDL_AtomicGet(&job.cancelled) || !sampleworldlodcolumn(generation, job.key, x, y, samples[(y + 1) * stride + x + 1])) return false;
        }
    }
    job.samplingmillis = (SDL_GetPerformanceCounter() - samplingstart) * 1000.0 / frequency;

    const Uint64 buildstart = SDL_GetPerformanceCounter();
    {
        ZoneScopedN("LOD/Mesh build");
        worldlodcpumesh &mesh = job.mesh;
        mesh.vertices.growbuf(resolution * resolution * 20);
        mesh.indices.growbuf(resolution * resolution * 30);
        int minimumheight = INT_MAX, maximumheight = INT_MIN, waterheight = INT_MIN;
        vector<uchar> merged;
        merged.pad(resolution * resolution);
        loopv(merged) merged[i] = 0;
        loop(y, resolution) loop(x, resolution)
        {
            const int mergeindex = y * resolution + x;
            if(merged[mergeindex]) continue;
            const worldlodcolumn &column = samples[(y + 1) * stride + x + 1];
            int width = 1, height = 1;
            while(x + width < resolution)
            {
                const worldlodcolumn &next = samples[(y + 1) * stride + x + width + 1];
                if(merged[mergeindex + width] || next.height != column.height || next.material != column.material) break;
                width++;
            }
            for(; y + height < resolution; ++height)
            {
                bool matches = true;
                loopi(width)
                {
                    const int nextindex = (y + height) * resolution + x + i;
                    const worldlodcolumn &next = samples[(y + height + 1) * stride + x + i + 1];
                    if(merged[nextindex] || next.height != column.height || next.material != column.material) { matches = false; break; }
                }
                if(!matches) break;
            }
            loop(oy, height) loop(ox, width) merged[(y + oy) * resolution + x + ox] = 1;
            const float x0 = x * WORLD_CHUNK_SIZE / float(resolution), x1 = (x + width) * WORLD_CHUNK_SIZE / float(resolution),
                        y0 = y * WORLD_CHUNK_SIZE / float(resolution), y1 = (y + height) * WORLD_CHUNK_SIZE / float(resolution),
                        top = WORLD_GROUND_HEIGHT + column.height * WORLD_BLOCK_SIZE;
            addworldlodtop(mesh, x0, y0, x1, y1, top, worldlodtopmaterial(column.material));
        }
        loop(y, resolution) loop(x, resolution)
        {
            const worldlodcolumn &column = samples[(y + 1) * stride + x + 1],
                                 &left = samples[(y + 1) * stride + x], &right = samples[(y + 1) * stride + x + 2],
                                 &back = samples[y * stride + x + 1], &front = samples[(y + 2) * stride + x + 1];
            const float x0 = x * WORLD_CHUNK_SIZE / float(resolution), x1 = (x + 1) * WORLD_CHUNK_SIZE / float(resolution),
                        y0 = y * WORLD_CHUNK_SIZE / float(resolution), y1 = (y + 1) * WORLD_CHUNK_SIZE / float(resolution),
                        top = WORLD_GROUND_HEIGHT + column.height * WORLD_BLOCK_SIZE;
            if(column.height > left.height)
                addworldlodcolumnside(mesh, x0, y0, x1, y1, WORLD_GROUND_HEIGHT + left.height * WORLD_BLOCK_SIZE, top, O_LEFT, column.material);
            if(column.height > right.height)
                addworldlodcolumnside(mesh, x0, y0, x1, y1, WORLD_GROUND_HEIGHT + right.height * WORLD_BLOCK_SIZE, top, O_RIGHT, column.material);
            if(column.height > back.height)
                addworldlodcolumnside(mesh, x0, y0, x1, y1, WORLD_GROUND_HEIGHT + back.height * WORLD_BLOCK_SIZE, top, O_BACK, column.material);
            if(column.height > front.height)
                addworldlodcolumnside(mesh, x0, y0, x1, y1, WORLD_GROUND_HEIGHT + front.height * WORLD_BLOCK_SIZE, top, O_FRONT, column.material);
            minimumheight = min(minimumheight, min(column.height, min(min(left.height, right.height), min(back.height, front.height))));
            maximumheight = max(maximumheight, column.height);
            waterheight = max(waterheight, column.waterheight);
        }
        mesh.terrainindices = mesh.indices.length();
        loopv(merged) merged[i] = 0;
        loop(y, resolution) loop(x, resolution)
        {
            const int mergeindex = y * resolution + x;
            if(merged[mergeindex]) continue;
            const worldlodcolumn &column = samples[(y + 1) * stride + x + 1];
            if(column.height >= column.waterheight) continue;
            int width = 1, height = 1;
            while(x + width < resolution)
            {
                const worldlodcolumn &next = samples[(y + 1) * stride + x + width + 1];
                if(merged[mergeindex + width] || next.height >= next.waterheight || next.waterheight != column.waterheight) break;
                width++;
            }
            for(; y + height < resolution; ++height)
            {
                bool matches = true;
                loopi(width)
                {
                    const int nextindex = (y + height) * resolution + x + i;
                    const worldlodcolumn &next = samples[(y + height + 1) * stride + x + i + 1];
                    if(merged[nextindex] || next.height >= next.waterheight || next.waterheight != column.waterheight) { matches = false; break; }
                }
                if(!matches) break;
            }
            loop(oy, height) loop(ox, width) merged[(y + oy) * resolution + x + ox] = 1;
            const float x0 = x * WORLD_CHUNK_SIZE / float(resolution), x1 = (x + width) * WORLD_CHUNK_SIZE / float(resolution),
                        y0 = y * WORLD_CHUNK_SIZE / float(resolution), y1 = (y + height) * WORLD_CHUNK_SIZE / float(resolution),
                        top = WORLD_GROUND_HEIGHT + column.waterheight * WORLD_BLOCK_SIZE;
            addworldlodtop(mesh, x0, y0, x1, y1, top, WORLD_LOD_WATER);
        }
        mesh.waterindices = mesh.indices.length() - mesh.terrainindices;
        mesh.bbmin = vec(0, 0, WORLD_GROUND_HEIGHT + minimumheight * WORLD_BLOCK_SIZE);
        mesh.bbmax = vec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, WORLD_GROUND_HEIGHT + max(maximumheight, waterheight) * WORLD_BLOCK_SIZE);
    }
    job.buildmillis = (SDL_GetPerformanceCounter() - buildstart) * 1000.0 / frequency;
    return !SDL_AtomicGet(&job.cancelled);
}

static bool buildworldlodmesh(worldlodjob &job)
{
    const Uint64 frequency = SDL_GetPerformanceFrequency(), generationstart = SDL_GetPerformanceCounter();
    worldgencontext *generation = game::createworldgeneration(false, false, &job.cancelled);
    if(!generation) return false;
    if(job.key.lod == 1)
    {
        const bool succeeded = buildworldlod1mesh(job, generation, frequency);
        job.generationmillis = (SDL_GetPerformanceCounter() - generationstart) * 1000.0 / frequency;
        game::destroyworldgeneration(generation);
        return succeeded;
    }

    const int resolution = job.key.resolution, stride = resolution + 1, samples = stride * stride;
    vector<int> heights;
    vector<uchar> materials, waters;
    heights.pad(samples);
    materials.pad(samples);
    waters.pad(samples);
    int waterheight = 0;

    const Uint64 samplingstart = SDL_GetPerformanceCounter();
    {
        ZoneScopedN("LOD/Surface sampling");
        loop(y, stride) loop(x, stride)
        {
            if(SDL_AtomicGet(&job.cancelled)) { game::destroyworldgeneration(generation); return false; }
            const int centerx = int(floorf(x * WORLD_CHUNK_BLOCKS / float(resolution) + 0.5f)),
                      centery = int(floorf(y * WORLD_CHUNK_BLOCKS / float(resolution) + 0.5f));
            int selectedx = centerx, selectedy = centery, selectedheight = 0;
            if(x == 0 || y == 0 || x == resolution || y == resolution)
            {
                if(!game::sampleterrainheight(generation, job.key.x * WORLD_CHUNK_BLOCKS + centerx,
                                              job.key.y * WORLD_CHUNK_BLOCKS + centery, selectedheight))
                {
                    game::destroyworldgeneration(generation);
                    return false;
                }
            }
            else
            {
                const int beginx = max(int(floorf((x - 0.5f) * WORLD_CHUNK_BLOCKS / resolution)), 0),
                          endx = min(int(ceilf((x + 0.5f) * WORLD_CHUNK_BLOCKS / resolution)), int(WORLD_CHUNK_BLOCKS)),
                          beginy = max(int(floorf((y - 0.5f) * WORLD_CHUNK_BLOCKS / resolution)), 0),
                          endy = min(int(ceilf((y + 0.5f) * WORLD_CHUNK_BLOCKS / resolution)), int(WORLD_CHUNK_BLOCKS));
                int minimum = INT_MAX, maximum = INT_MIN, minimumx = centerx, minimumy = centery, maximumx = centerx, maximumy = centery;
                long long total = 0;
                int count = 0;
                for(int sy = beginy; sy <= endy; ++sy) for(int sx = beginx; sx <= endx; ++sx)
                {
                    int height;
                    if(!game::sampleterrainheight(generation, job.key.x * WORLD_CHUNK_BLOCKS + sx,
                                                  job.key.y * WORLD_CHUNK_BLOCKS + sy, height))
                    {
                        game::destroyworldgeneration(generation);
                        return false;
                    }
                    if(height < minimum) { minimum = height; minimumx = sx; minimumy = sy; }
                    if(height > maximum) { maximum = height; maximumx = sx; maximumy = sy; }
                    total += height;
                    count++;
                }
                const float average = count ? total / float(count) : 0.0f;
                if(maximum - minimum <= 2)
                    selectedheight = int(floorf(average + 0.5f));
                else if(maximum - average >= average - minimum)
                {
                    selectedheight = maximum;
                    selectedx = maximumx;
                    selectedy = maximumy;
                }
                else
                {
                    selectedheight = minimum;
                    selectedx = minimumx;
                    selectedy = minimumy;
                }
            }

            worldsurfacesample surface;
            if(!game::sampleterrainsurface(generation, job.key.x * WORLD_CHUNK_BLOCKS + selectedx,
                                          job.key.y * WORLD_CHUNK_BLOCKS + selectedy, surface))
            {
                game::destroyworldgeneration(generation);
                return false;
            }
            const int index = y * stride + x;
            heights[index] = selectedheight;
            materials[index] = uchar(surface.material);
            waters[index] = selectedheight < surface.waterheight;
            waterheight = surface.waterheight;
        }
    }
    job.samplingmillis = (SDL_GetPerformanceCounter() - samplingstart) * 1000.0 / frequency;

    const Uint64 buildstart = SDL_GetPerformanceCounter();
    {
        ZoneScopedN("LOD/Mesh build");
        worldlodcpumesh &mesh = job.mesh;
        mesh.vertices.growbuf(samples + resolution * 16 + samples);
        mesh.indices.growbuf(resolution * resolution * 12 + resolution * 24);
        float minimumz = FLT_MAX, maximumz = -FLT_MAX;
        loop(y, stride) loop(x, stride)
        {
            const int index = y * stride + x, left = y * stride + max(x - 1, 0), right = y * stride + min(x + 1, resolution),
                      down = max(y - 1, 0) * stride + x, up = min(y + 1, resolution) * stride + x;
            const float stepx = max((min(x + 1, resolution) - max(x - 1, 0)) * WORLD_CHUNK_SIZE / float(resolution), 1.0f),
                        stepy = max((min(y + 1, resolution) - max(y - 1, 0)) * WORLD_CHUNK_SIZE / float(resolution), 1.0f),
                        dzdx = (heights[right] - heights[left]) * WORLD_BLOCK_SIZE / stepx,
                        dzdy = (heights[up] - heights[down]) * WORLD_BLOCK_SIZE / stepy,
                        z = WORLD_GROUND_HEIGHT + heights[index] * WORLD_BLOCK_SIZE;
            vec normal(-dzdx, -dzdy, 1.0f);
            normal.normalize();
            const vec position(x * WORLD_CHUNK_SIZE / float(resolution), y * WORLD_CHUNK_SIZE / float(resolution), z);
            mesh.vertices.add(worldlodvertex(position, normal, worldlodtexcoord(position, O_TOP), worldlodtopmaterial(materials[index])));
            minimumz = min(minimumz, z);
            maximumz = max(maximumz, z);
        }
        loop(y, resolution) loop(x, resolution)
        {
            const uint a = y * stride + x, b = a + 1, d = (y + 1) * stride + x, c = d + 1;
            mesh.indices.add(a); mesh.indices.add(b); mesh.indices.add(c);
            mesh.indices.add(a); mesh.indices.add(c); mesh.indices.add(d);
            mesh.topfaces++;
        }
        const float skirtdepth = job.key.skirtdepth * WORLD_BLOCK_SIZE;
        loopi(4) addworldlodskirt(mesh, heights, materials, resolution, i, skirtdepth);
        mesh.terrainindices = mesh.indices.length();

        const uint waterbase = mesh.vertices.length();
        const float waterz = WORLD_GROUND_HEIGHT + waterheight * WORLD_BLOCK_SIZE;
        loop(y, stride) loop(x, stride)
        {
            const vec position(x * WORLD_CHUNK_SIZE / float(resolution), y * WORLD_CHUNK_SIZE / float(resolution), waterz);
            mesh.vertices.add(worldlodvertex(position, vec(0, 0, 1), worldlodtexcoord(position, O_TOP), WORLD_LOD_WATER));
        }
        loop(y, resolution) loop(x, resolution)
        {
            const uint a = y * stride + x, b = a + 1, d = (y + 1) * stride + x, c = d + 1;
            if(waters[a] && waters[b] && waters[c])
            {
                mesh.indices.add(waterbase + a); mesh.indices.add(waterbase + b); mesh.indices.add(waterbase + c);
            }
            if(waters[a] && waters[c] && waters[d])
            {
                mesh.indices.add(waterbase + a); mesh.indices.add(waterbase + c); mesh.indices.add(waterbase + d);
            }
        }
        mesh.waterindices = mesh.indices.length() - mesh.terrainindices;
        mesh.bbmin = vec(0, 0, minimumz - skirtdepth);
        mesh.bbmax = vec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, max(maximumz, waterz));
    }
    job.buildmillis = (SDL_GetPerformanceCounter() - buildstart) * 1000.0 / frequency;
    job.generationmillis = (SDL_GetPerformanceCounter() - generationstart) * 1000.0 / frequency;
    game::destroyworldgeneration(generation);
    return !SDL_AtomicGet(&job.cancelled);
}

static int worldlodworker(void *)
{
#ifdef TRACY_ENABLE
    tracy::SetThreadName("World LOD worker");
#endif
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW);
    for(;;)
    {
        SDL_LockMutex(worldlodmutex);
        while(worldlodjobs.empty() && !stopworldlodthreads) SDL_CondWait(worldlodcond, worldlodmutex);
        if(stopworldlodthreads)
        {
            SDL_UnlockMutex(worldlodmutex);
            return 0;
        }
        int best = 0;
        loopv(worldlodjobs) if(i && worldlodjobless(*worldlodjobs[i], *worldlodjobs[best])) best = i;
        worldlodjob *job = worldlodjobs.remove(best);
        worldlodactivejobs.add(job);
        SDL_UnlockMutex(worldlodmutex);

        if(job->key.lod == 1)
        {
            ZoneScopedN("LOD/LOD1 generation");
            job->succeeded = buildworldlodmesh(*job);
        }
        else
        {
            ZoneScopedN("LOD/LOD2 generation");
            job->succeeded = buildworldlodmesh(*job);
        }

        SDL_LockMutex(worldlodmutex);
        worldlodactivejobs.removeobj(job);
        if(stopworldlodthreads)
        {
            SDL_UnlockMutex(worldlodmutex);
            delete job;
            return 0;
        }
        worldlodresults.add(job);
        SDL_CondBroadcast(worldlodcond);
        SDL_UnlockMutex(worldlodmutex);
    }
}

static bool startworldlodworkers()
{
    if(!worldlodworkers.empty()) return true;
    worldlodmutex = SDL_CreateMutex();
    worldlodcond = SDL_CreateCond();
    stopworldlodthreads = false;
    if(!worldlodmutex || !worldlodcond)
    {
        if(worldlodcond) SDL_DestroyCond(worldlodcond);
        if(worldlodmutex) SDL_DestroyMutex(worldlodmutex);
        worldlodmutex = NULL;
        worldlodcond = NULL;
        return false;
    }
    loopi(worldlodthreads)
    {
        SDL_Thread *worker = SDL_CreateThread(worldlodworker, "world LOD worker", NULL);
        if(worker) worldlodworkers.add(worker);
    }
    return !worldlodworkers.empty();
}

static void shutdownworldlodworkers()
{
    if(!worldlodworkers.empty())
    {
        SDL_LockMutex(worldlodmutex);
        stopworldlodthreads = true;
        loopv(worldlodactivejobs) SDL_AtomicSet(&worldlodactivejobs[i]->cancelled, 1);
        SDL_CondBroadcast(worldlodcond);
        SDL_UnlockMutex(worldlodmutex);
        loopv(worldlodworkers) SDL_WaitThread(worldlodworkers[i], NULL);
        worldlodworkers.setsize(0);
    }
    loopv(worldlodjobs) delete worldlodjobs[i];
    worldlodjobs.setsize(0);
    loopv(worldlodresults) delete worldlodresults[i];
    worldlodresults.setsize(0);
    ASSERT(worldlodactivejobs.empty());
    if(worldlodcond) SDL_DestroyCond(worldlodcond);
    if(worldlodmutex) SDL_DestroyMutex(worldlodmutex);
    worldlodcond = NULL;
    worldlodmutex = NULL;
    stopworldlodthreads = false;
}

static void deleteworldlodchunk(worldlodchunk &chunk)
{
    if(chunk.vbo) glDeleteBuffers_(1, &chunk.vbo);
    if(chunk.ebo) glDeleteBuffers_(1, &chunk.ebo);
    chunk.vbo = chunk.ebo = 0;
}

static void clearworldlods()
{
    ZoneScopedN("LOD/Clear");
    shutdownworldlodworkers();
    loopv(worldlodcache) deleteworldlodchunk(worldlodcache[i]);
    worldlodcache.setsize(0);
    worldlodselections.setsize(0);
    worldlodsettings = 0;
    worldlodlastgeneration = worldlodlastupload = 0;
    worldlodrendervertices = worldlodrendertriangles = worldlodrendertopfaces = worldlodrendersidefaces = 0;
    worldlodmissingchunks = 0;
    ++worldlodepoch;
}

static ullong currentworldlodsettings()
{
    ullong hash = game::worldgenerationparameterhash() ^ ullong(uint(game::getworldseed())) ^ ullong(WORLDGEN_VERSION) << 32;
    const int values[] = { worldlod1resolution, worldlod2resolution, worldlodneardistance, worldlodfardistance, worldlodmaxdistance,
                           worldlodhysteresis, worldlodskirtdepth, worldlodthreads };
    loopi(sizeof(values) / sizeof(values[0]))
    {
        hash ^= uint(values[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool queueworldlodjob(const worldlodkey &key, int priority, float distance, float alignment = 0)
{
    if(findworldlodcache(key) >= 0 || !startworldlodworkers()) return false;
    bool queued = false;
    SDL_LockMutex(worldlodmutex);
    loopv(worldlodjobs) if(worldlodjobs[i]->key == key)
    {
        worldlodjobs[i]->priority = priority;
        worldlodjobs[i]->distance = distance;
        worldlodjobs[i]->alignment = alignment;
        SDL_UnlockMutex(worldlodmutex);
        return false;
    }
    loopv(worldlodactivejobs) if(worldlodactivejobs[i]->key == key) { SDL_UnlockMutex(worldlodmutex); return false; }
    loopv(worldlodresults) if(worldlodresults[i]->key == key) { SDL_UnlockMutex(worldlodmutex); return false; }
    const int outstanding = worldlodjobs.length() + worldlodactivejobs.length() + worldlodresults.length();
    if(outstanding < worldlodpendinglimit)
    {
        worldlodjobs.add(new worldlodjob(key, priority, distance, alignment, worldlodepoch));
        worldlodcachemisses++;
        queued = true;
        SDL_CondSignal(worldlodcond);
    }
    SDL_UnlockMutex(worldlodmutex);
    return queued;
}

static bool requiredworldlodjob(const worldlodjob &job)
{
    loopv(worldlodselections)
    {
        const worldlodselection &selection = worldlodselections[i];
        if(selection.lastseen == totalmillis && selection.x == job.key.x && selection.y == job.key.y && selection.desired == job.key.lod) return true;
    }
    return false;
}

static void canceloptionalworldlodjobs()
{
    if(!worldlodmutex) return;
    SDL_LockMutex(worldlodmutex);
    for(int i = worldlodjobs.length() - 1; i >= 0; --i) if(!requiredworldlodjob(*worldlodjobs[i])) delete worldlodjobs.remove(i);
    loopv(worldlodactivejobs) if(!requiredworldlodjob(*worldlodactivejobs[i])) SDL_AtomicSet(&worldlodactivejobs[i]->cancelled, 1);
    for(int i = worldlodresults.length() - 1; i >= 0; --i) if(!requiredworldlodjob(*worldlodresults[i])) delete worldlodresults.remove(i);
    SDL_UnlockMutex(worldlodmutex);
}

static int worldlodoutstandingjobs()
{
    if(!worldlodmutex) return 0;
    SDL_LockMutex(worldlodmutex);
    const int outstanding = worldlodjobs.length() + worldlodactivejobs.length() + worldlodresults.length();
    SDL_UnlockMutex(worldlodmutex);
    return outstanding;
}

static void processworldlodresults()
{
    if(!worldlodmutex) return;
    loopi(worldloduploadlimit)
    {
        worldlodjob *job = NULL;
        SDL_LockMutex(worldlodmutex);
        if(!worldlodresults.empty()) job = worldlodresults.remove(0);
        SDL_UnlockMutex(worldlodmutex);
        if(!job) break;
        if(job->epoch != worldlodepoch || !job->succeeded || SDL_AtomicGet(&job->cancelled) || job->key.seed != game::getworldseed() ||
           job->key.generation != game::worldgenerationparameterhash())
        {
            delete job;
            continue;
        }

        const Uint64 start = SDL_GetPerformanceCounter();
        {
            ZoneScopedN("LOD/GPU upload");
            worldlodchunk &chunk = worldlodcache.add(worldlodchunk(job->key));
            glGenBuffers_(1, &chunk.vbo);
            glGenBuffers_(1, &chunk.ebo);
            gle::bindvbo(chunk.vbo);
            glBufferData_(GL_ARRAY_BUFFER, job->mesh.vertices.length() * sizeof(worldlodvertex), job->mesh.vertices.getbuf(), GL_STATIC_DRAW);
            gle::bindebo(chunk.ebo);
            glBufferData_(GL_ELEMENT_ARRAY_BUFFER, job->mesh.indices.length() * sizeof(uint), job->mesh.indices.getbuf(), GL_STATIC_DRAW);
            gle::clearebo();
            gle::clearvbo();
            chunk.vertices = job->mesh.vertices.length();
            chunk.terrainindices = job->mesh.terrainindices;
            chunk.waterindices = job->mesh.waterindices;
            chunk.topfaces = job->mesh.topfaces;
            chunk.sidefaces = job->mesh.sidefaces;
            chunk.bbmin = job->mesh.bbmin;
            chunk.bbmax = job->mesh.bbmax;
            chunk.lastused = totalmillis;
        }
        worldlodlastupload = (SDL_GetPerformanceCounter() - start) * 1000.0 / SDL_GetPerformanceFrequency();
        worldlodlastgeneration = job->generationmillis;
        if(job->key.lod == 1) TracyPlot("LOD/LOD1 generation ms", job->generationmillis);
        else TracyPlot("LOD/LOD2 generation ms", job->generationmillis);
        TracyPlot("LOD/Surface sampling ms", job->samplingmillis);
        TracyPlot("LOD/Mesh build ms", job->buildmillis);
        TracyPlot("LOD/GPU upload ms", worldlodlastupload);
        delete job;
    }
}

static float worldloddistance(int x, int y, const vec &focus)
{
    const float minx = (x - worldfirstchunkx) * WORLD_CHUNK_SIZE, miny = (y - worldfirstchunky) * WORLD_CHUNK_SIZE,
                maxx = minx + WORLD_CHUNK_SIZE, maxy = miny + WORLD_CHUNK_SIZE,
                dx = focus.x < minx ? minx - focus.x : focus.x > maxx ? focus.x - maxx : 0.0f,
                dy = focus.y < miny ? miny - focus.y : focus.y > maxy ? focus.y - maxy : 0.0f;
    return sqrtf(dx * dx + dy * dy);
}

struct worldlodcandidate
{
    worldlodkey key;
    int priority;
    float distance, alignment;

    worldlodcandidate(const worldlodkey &key = worldlodkey(), int priority = 2, float distance = 0, float alignment = 0)
        : key(key), priority(priority), distance(distance), alignment(alignment) {}
};

static worldlodcandidate makeworldlodcandidate(const worldlodselection &selection)
{
    const vec center((selection.x - worldfirstchunkx + 0.5f) * WORLD_CHUNK_SIZE,
                     (selection.y - worldfirstchunky + 0.5f) * WORLD_CHUNK_SIZE, camera1->o.z),
              delta = vec(center).sub(camera1->o);
    const float directionlength = sqrtf(camdir.x * camdir.x + camdir.y * camdir.y), deltalength = sqrtf(delta.x * delta.x + delta.y * delta.y),
                alignment = directionlength > 1e-4f && deltalength > 1e-4f
                          ? (camdir.x * delta.x + camdir.y * delta.y) / (directionlength * deltalength) : 0;
    const ivec origin((selection.x - worldfirstchunkx) * WORLD_CHUNK_SIZE, (selection.y - worldfirstchunky) * WORLD_CHUNK_SIZE, 0);
    const bool visible = !viewfrustumvalid() || isvisiblebb(origin, ivec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, WORLD_MAP_SIZE)) < VFC_FOGGED;
    const int camerachunkx = worldfirstchunkx + int(floorf(camera1->o.x / WORLD_CHUNK_SIZE)),
              camerachunky = worldfirstchunky + int(floorf(camera1->o.y / WORLD_CHUNK_SIZE));
    // The camera chunk is first, followed by frustum-intersecting chunks nearest-first, then surrounding chunks nearest-first.
    const int priority = selection.x == camerachunkx && selection.y == camerachunky ? 0 : visible ? 1 : 2;
    return worldlodcandidate(currentworldlodkey(selection.x, selection.y, selection.desired), priority, selection.distance, alignment);
}

static bool sortworldlodcandidates(const worldlodcandidate &a, const worldlodcandidate &b)
{
    if(worldlodpriorityless(a.priority, a.distance, a.alignment, b.priority, b.distance, b.alignment)) return true;
    if(worldlodpriorityless(b.priority, b.distance, b.alignment, a.priority, a.distance, a.alignment)) return false;
    if(a.key.y != b.key.y) return a.key.y < b.key.y;
    if(a.key.x != b.key.x) return a.key.x < b.key.x;
    return a.key.lod < b.key.lod;
}

static int desiredworldlod(int previous, float distance)
{
    const float nearthreshold = worldlodneardistance * WORLD_BLOCK_SIZE,
                farthreshold = max(worldlodfardistance, worldlodneardistance + 1) * WORLD_BLOCK_SIZE,
                hysteresis = worldlodhysteresis * WORLD_BLOCK_SIZE;
    switch(previous)
    {
        case 0: return distance > nearthreshold + hysteresis ? 1 : 0;
        case 2: return distance < farthreshold - hysteresis ? 1 : 2;
        default:
            if(distance < nearthreshold - hysteresis) return 0;
            if(distance > farthreshold + hysteresis) return 2;
            return 1;
    }
}

static bool worldlodfullready(int x, int y)
{
    const int index = findworldchunk(x, y);
    return worldchunks.inrange(index) && !worldchunks[index].loading && worldchunks[index].root && worldchunkmounted(worldchunks[index]);
}

static bool worldlodrequiresvoxel(const worldchunk &chunk)
{
    const int index = findworldlodselection(chunk.x, chunk.y);
    return index < 0 || worldlodselections[index].desired == 0;
}

static void pruneworldlodcache()
{
    while(worldlodcache.length() > worldlodcachelimit)
    {
        int oldest = -1;
        loopv(worldlodcache)
        {
            if(worldlodcache[i].active) continue;
            if(oldest < 0 || worldlodcache[i].lastused < worldlodcache[oldest].lastused) oldest = i;
        }
        if(oldest < 0) break;
        deleteworldlodchunk(worldlodcache[oldest]);
        worldlodcache.removeunordered(oldest);
    }
}

static void updateworldlods(int chunkx, int chunky)
{
    if(!worldlod || !camera1)
    {
        if(!worldlodselections.empty() || !worldlodcache.empty() || !worldlodworkers.empty()) clearworldlods();
        return;
    }
    ullong settings = currentworldlodsettings();
    if(worldlodsettings && settings != worldlodsettings) clearworldlods();
    worldlodsettings = settings;
    processworldlodresults();

    const vec &focus = camera1->o;
    const float maxdistance = worldlodmaxdistance * WORLD_BLOCK_SIZE;
    const int radius = int(ceilf(maxdistance / WORLD_CHUNK_SIZE)) + 1, diameter = radius * 2 + 1;
    vector<int> grid;
    grid.pad(diameter * diameter);
    loopv(grid) grid[i] = -1;

    for(int y = chunky - radius; y <= chunky + radius; ++y) for(int x = chunkx - radius; x <= chunkx + radius; ++x)
    {
        const float distance = worldloddistance(x, y, focus);
        if(distance > maxdistance) continue;
        int selectionindex = findworldlodselection(x, y);
        if(selectionindex < 0)
        {
            selectionindex = worldlodselections.length();
            worldlodselection &selection = worldlodselections.add(worldlodselection(x, y));
            selection.active = worldlodfullready(x, y) ? 0 : -1;
            selection.desired = desiredworldlod(0, distance);
        }
        worldlodselection &selection = worldlodselections[selectionindex];
        selection.distance = distance;
        selection.lastseen = totalmillis;
        selection.desired = desiredworldlod(selection.desired, distance);
        grid[(y - (chunky - radius)) * diameter + x - (chunkx - radius)] = selectionindex;
    }

    // A one-chunk LOD1 collar prevents direct LOD0/LOD2 neighbors even if users configure very narrow distance bands.
    loop(y, diameter) loop(x, diameter)
    {
        const int selectionindex = grid[y * diameter + x];
        if(selectionindex < 0 || worldlodselections[selectionindex].desired != 2) continue;
        bool toucheslod0 = false;
        for(int oy = -1; oy <= 1 && !toucheslod0; ++oy) for(int ox = -1; ox <= 1; ++ox)
        {
            if(!ox && !oy) continue;
            const int nx = x + ox, ny = y + oy;
            if(nx < 0 || nx >= diameter || ny < 0 || ny >= diameter) continue;
            const int neighbor = grid[ny * diameter + nx];
            if(neighbor >= 0 && worldlodselections[neighbor].desired == 0) { toucheslod0 = true; break; }
        }
        if(toucheslod0) worldlodselections[selectionindex].desired = 1;
    }

    loopv(worldlodcache) worldlodcache[i].active = false;
    int active1 = 0, active2 = 0, missing = 0, centerjobs = 0, visiblejobs = 0, surroundingjobs = 0;
    loopv(worldlodselections)
    {
        worldlodselection &selection = worldlodselections[i];
        if(selection.lastseen != totalmillis) continue;
        if(selection.active > 0)
        {
            const int activeindex = findworldlodcache(currentworldlodkey(selection.x, selection.y, selection.active));
            if(activeindex >= 0) worldlodcache[activeindex].active = true;
        }
        if(selection.desired == 0)
        {
            if(worldlodfullready(selection.x, selection.y)) selection.active = 0;
        }
        else
        {
            const worldlodkey key = currentworldlodkey(selection.x, selection.y, selection.desired);
            const int cacheindex = findworldlodcache(key);
            if(cacheindex >= 0)
            {
                if(selection.active != selection.desired) worldlodcachehits++;
                selection.active = selection.desired;
                worldlodcache[cacheindex].lastused = totalmillis;
                worldlodcache[cacheindex].active = true;
                const int chunkindex = findworldchunk(selection.x, selection.y);
                if(worldchunks.inrange(chunkindex) && worldchunkmounted(worldchunks[chunkindex])) unmountworldchunk(worldchunks[chunkindex]);
            }
            else missing++;
        }
        if(selection.active == 1) active1++;
        else if(selection.active == 2) active2++;
    }
    worldlodmissingchunks = missing;

    if(missing > 0)
    {
        canceloptionalworldlodjobs();
        vector<worldlodcandidate> candidates;
        loopv(worldlodselections)
        {
            const worldlodselection &selection = worldlodselections[i];
            if(selection.lastseen != totalmillis || selection.desired <= 0) continue;
            const worldlodkey key = currentworldlodkey(selection.x, selection.y, selection.desired);
            if(findworldlodcache(key) >= 0) continue;
            worldlodcandidate &candidate = candidates.add(makeworldlodcandidate(selection));
            if(candidate.priority == 0) centerjobs++;
            else if(candidate.priority == 1) visiblejobs++;
            else surroundingjobs++;
        }
        if(candidates.length() > 1) candidates.sort(sortworldlodcandidates);
        loopv(candidates) queueworldlodjob(candidates[i].key, candidates[i].priority, candidates[i].distance, candidates[i].alignment);
    }
    else
    {
        int prefetchbudget = max(worldlodcachelimit - worldlodcache.length() - worldlodoutstandingjobs(), 0);
        loopv(worldlodselections)
        {
            if(prefetchbudget <= 0) break;
            const worldlodselection &selection = worldlodselections[i];
            if(selection.lastseen != totalmillis || selection.desired <= 0) continue;
            int lod = 0;
            if(selection.desired == 1 && selection.distance > (worldlodfardistance - 2 * worldlodhysteresis) * WORLD_BLOCK_SIZE) lod = 2;
            else if(selection.desired == 2 && selection.distance < (worldlodfardistance + 2 * worldlodhysteresis) * WORLD_BLOCK_SIZE) lod = 1;
            if(lod && queueworldlodjob(currentworldlodkey(selection.x, selection.y, lod), 3, selection.distance)) prefetchbudget--;
        }
    }

    for(int i = worldlodselections.length() - 1; i >= 0; --i)
        if(worldlodselections[i].lastseen != totalmillis) worldlodselections.removeunordered(i);
    pruneworldlodcache();

    int queued = 0;
    if(worldlodmutex)
    {
        SDL_LockMutex(worldlodmutex);
        queued = worldlodjobs.length() + worldlodactivejobs.length() + worldlodresults.length();
        SDL_UnlockMutex(worldlodmutex);
    }
    TracyPlot("LOD/Active LOD1 chunks", int64_t(active1));
    TracyPlot("LOD/Active LOD2 chunks", int64_t(active2));
    TracyPlot("LOD/Missing required chunks", int64_t(missing));
    TracyPlot("LOD/Center-priority jobs", int64_t(centerjobs));
    TracyPlot("LOD/Frustum-priority jobs", int64_t(visiblejobs));
    TracyPlot("LOD/Surrounding jobs", int64_t(surroundingjobs));
    TracyPlot("LOD/Queued jobs", int64_t(queued));
    TracyPlot("LOD/Cache hits", int64_t(worldlodcachehits));
    TracyPlot("LOD/Cache misses", int64_t(worldlodcachemisses));
    (void)queued;
}

static int findworldlodtextureslot(const char *id, bool side)
{
    loopv(worldgentextures) if(!strcmp(worldgentextures[i].id, id)) return side ? worldgentextures[i].side : worldgentextures[i].top;
    return DEFAULT_GEOM;
}

static GLuint worldlodtexture(const char *id, bool side = false)
{
    VSlot &vslot = lookupvslot(findworldlodtextureslot(id, side), true);
    Texture *texture = vslot.slot && !vslot.slot->sts.empty() && vslot.slot->sts[0].t ? vslot.slot->sts[0].t : notexture;
    return texture->id;
}

static void bindworldlodtextures()
{
    static const char * const ids[] = { "grass", "grass", "dirt", "stone", "sand", "snow" };
    loopi(sizeof(ids) / sizeof(ids[0]))
    {
        glActiveTexture_(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, worldlodtexture(ids[i], i == WORLD_LOD_GRASS_SIDE));
    }
    glActiveTexture_(GL_TEXTURE0);
}

void renderworldlods()
{
    worldlodrendervertices = worldlodrendertriangles = worldlodrendertopfaces = worldlodrendersidefaces = 0;
    if(!worldlod || !camera1 || worldlodselections.empty()) return;
    Shader *shader = useshaderbyname("worldlod");
    if(!shader) return;
    ZoneScopedN("Render/G-buffer/World LOD");
    shader->set();
    bindworldlodtextures();
    LOCALPARAMF(loddebug, float(worldloddebug));
    int vertices = 0, triangles = 0, topfaces = 0, sidefaces = 0;
    if(worldlodwireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    loopv(worldlodselections)
    {
        const worldlodselection &selection = worldlodselections[i];
        if(selection.active < 1) continue;
        const int cacheindex = findworldlodcache(currentworldlodkey(selection.x, selection.y, selection.active));
        if(cacheindex < 0) continue;
        worldlodchunk &chunk = worldlodcache[cacheindex];
        const ivec origin((selection.x - worldfirstchunkx) * WORLD_CHUNK_SIZE, (selection.y - worldfirstchunky) * WORLD_CHUNK_SIZE, 0),
                   bbmin = ivec(chunk.bbmin).add(origin), bbmax = ivec(chunk.bbmax).add(origin);
        if(isvisiblebb(bbmin, ivec(bbmax).sub(bbmin)) >= VFC_FOGGED) continue;
        gle::bindvbo(chunk.vbo);
        gle::bindebo(chunk.ebo);
        const worldlodvertex *pointer = 0;
        gle::vertexpointer(sizeof(worldlodvertex), pointer->position.v);
        gle::normalpointer(sizeof(worldlodvertex), pointer->normal.v);
        gle::texcoord0pointer(sizeof(worldlodvertex), pointer->texcoord.v);
        gle::colorpointer(sizeof(worldlodvertex), pointer->material.v);
        gle::enablevertex();
        gle::enablenormal();
        gle::enabletexcoord0();
        gle::enablecolor();
        LOCALPARAM(lodmeshoffset, vec(origin));
        const int indices = chunk.terrainindices + chunk.waterindices;
        glDrawElements(GL_TRIANGLES, indices, GL_UNSIGNED_INT, 0);
        vertices += chunk.vertices;
        triangles += indices / 3;
        topfaces += chunk.topfaces;
        sidefaces += chunk.sidefaces;
        glde++;
    }
    if(worldlodwireframe) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    gle::disablevertex();
    gle::disablenormal();
    gle::disabletexcoord0();
    gle::disablecolor();
    gle::clearebo();
    gle::clearvbo();
    worldlodrendervertices = vertices;
    worldlodrendertriangles = triangles;
    worldlodrendertopfaces = topfaces;
    worldlodrendersidefaces = sidefaces;
    TracyPlot("LOD/Vertices", int64_t(vertices));
    TracyPlot("LOD/Triangles", int64_t(triangles));
    TracyPlot("LOD/Top faces", int64_t(topfaces));
    TracyPlot("LOD/Side faces", int64_t(sidefaces));
}

ICOMMAND(getdebuglod1chunks, "", (),
{
    int count = 0;
    loopv(worldlodselections) if(worldlodselections[i].active == 1) count++;
    intret(count);
});
ICOMMAND(getdebuglod2chunks, "", (),
{
    int count = 0;
    loopv(worldlodselections) if(worldlodselections[i].active == 2) count++;
    intret(count);
});
ICOMMAND(getdebuglodqueued, "", (),
{
    int count = 0;
    if(worldlodmutex)
    {
        SDL_LockMutex(worldlodmutex);
        count = worldlodjobs.length() + worldlodactivejobs.length() + worldlodresults.length();
        SDL_UnlockMutex(worldlodmutex);
    }
    intret(count);
});
ICOMMAND(getdebuglodmissing, "", (), intret(worldlodmissingchunks));
ICOMMAND(getdebuglodcachehits, "", (), intret(worldlodcachehits));
ICOMMAND(getdebuglodcachemisses, "", (), intret(worldlodcachemisses));
ICOMMAND(getdebuglodvertices, "", (), intret(worldlodrendervertices));
ICOMMAND(getdebuglodtriangles, "", (), intret(worldlodrendertriangles));
ICOMMAND(getdebuglodtopfaces, "", (), intret(worldlodrendertopfaces));
ICOMMAND(getdebuglodsidefaces, "", (), intret(worldlodrendersidefaces));
ICOMMAND(getdebuglodgenerationms, "", (), floatret(float(worldlodlastgeneration)));
ICOMMAND(getdebugloduploadms, "", (), floatret(float(worldlodlastupload)));

#endif
