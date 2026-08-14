// worldlod.cpp: lightweight Kastenbrot distant terrain surfaces

#ifdef WORLDIO_MODULE_IMPLEMENTATION

VARP(worldlod, 0, 1, 1);
VARP(worldlod1resolution, 4, 32, WORLD_CHUNK_BLOCKS);
VARP(worldlod2resolution, 4, 16, WORLD_CHUNK_BLOCKS);
VARP(worldlodneardistance, 16, 128, 4096); // blocks from the chunk AABB
VARP(worldlodfardistance, 32, 256, 4096);
VARP(worldlodmaxdistance, 64, 512, 4096);
VARP(worldlodhysteresis, 0, 16, 512);
VARP(worldlodskirtdepth, 1, 4, 64);
VARP(worldlodthreads, 1, 1, 4);
VARP(worldlodpendinglimit, 4, 64, 512);
VARP(worldlodcachelimit, 16, 384, 4096);
VARP(worldloduploadlimit, 1, 2, 16);

struct worldlodvertex
{
    vec position, normal;
    bvec4 color;

    worldlodvertex(const vec &position = vec(0, 0, 0), const vec &normal = vec(0, 0, 1), const bvec4 &color = bvec4(255, 255, 255, 255))
        : position(position), normal(normal), color(color) {}
};

struct worldlodcpumesh
{
    vector<worldlodvertex> vertices;
    vector<uint> indices;
    int terrainindices, waterindices;
    vec bbmin, bbmax;

    worldlodcpumesh() : terrainindices(0), waterindices(0), bbmin(0, 0, 0), bbmax(0, 0, 0) {}
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
    int vertices, terrainindices, waterindices, lastused;
    vec bbmin, bbmax;

    worldlodchunk(const worldlodkey &key = worldlodkey())
        : key(key), vbo(0), ebo(0), vertices(0), terrainindices(0), waterindices(0), lastused(0), bbmin(0, 0, 0), bbmax(0, 0, 0) {}
};

struct worldlodjob
{
    worldlodkey key;
    int priority;
    float distance;
    uint epoch;
    SDL_atomic_t cancelled;
    worldlodcpumesh mesh;
    double generationmillis, samplingmillis, buildmillis;
    bool succeeded;

    worldlodjob(const worldlodkey &key, int priority, float distance, uint epoch)
        : key(key), priority(priority), distance(distance), epoch(epoch), generationmillis(0), samplingmillis(0), buildmillis(0), succeeded(false)
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
static int worldlodrendervertices = 0, worldlodrendertriangles = 0;
static double worldlodlastgeneration = 0, worldlodlastupload = 0;

static bvec4 worldlodmaterialcolor(int material, bool water = false)
{
    if(water) return bvec4(45, 105, 155, 255);
    switch(material)
    {
        case WORLD_SURFACE_STONE: return bvec4(116, 116, 112, 255);
        case WORLD_SURFACE_SAND:  return bvec4(194, 178, 119, 255);
        case WORLD_SURFACE_SNOW:  return bvec4(226, 232, 235, 255);
        case WORLD_SURFACE_DIRT:  return bvec4(105, 78, 49, 255);
        default:                  return bvec4(87, 132, 62, 255);
    }
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

static bool worldlodjobless(const worldlodjob &a, const worldlodjob &b)
{
    return a.priority < b.priority || (a.priority == b.priority && a.distance < b.distance);
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
        mesh.vertices.add(worldlodvertex(va.position, normal, worldlodmaterialcolor(materials[a])));
        mesh.vertices.add(worldlodvertex(vb.position, normal, worldlodmaterialcolor(materials[b])));
        mesh.vertices.add(worldlodvertex(vec(vb.position).sub(vec(0, 0, skirtdepth)), normal, worldlodmaterialcolor(materials[b])));
        mesh.vertices.add(worldlodvertex(vec(va.position).sub(vec(0, 0, skirtdepth)), normal, worldlodmaterialcolor(materials[a])));
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
    }
}

static bool buildworldlodmesh(worldlodjob &job)
{
    const Uint64 frequency = SDL_GetPerformanceFrequency(), generationstart = SDL_GetPerformanceCounter();
    worldgencontext *generation = game::createworldgeneration(false, false, &job.cancelled);
    if(!generation) return false;

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
            mesh.vertices.add(worldlodvertex(vec(x * WORLD_CHUNK_SIZE / float(resolution), y * WORLD_CHUNK_SIZE / float(resolution), z), normal,
                                               worldlodmaterialcolor(materials[index])));
            minimumz = min(minimumz, z);
            maximumz = max(maximumz, z);
        }
        loop(y, resolution) loop(x, resolution)
        {
            const uint a = y * stride + x, b = a + 1, d = (y + 1) * stride + x, c = d + 1;
            mesh.indices.add(a); mesh.indices.add(b); mesh.indices.add(c);
            mesh.indices.add(a); mesh.indices.add(c); mesh.indices.add(d);
        }
        const float skirtdepth = job.key.skirtdepth * WORLD_BLOCK_SIZE;
        loopi(4) addworldlodskirt(mesh, heights, materials, resolution, i, skirtdepth);
        mesh.terrainindices = mesh.indices.length();

        const uint waterbase = mesh.vertices.length();
        const float waterz = WORLD_GROUND_HEIGHT + waterheight * WORLD_BLOCK_SIZE;
        loop(y, stride) loop(x, stride)
            mesh.vertices.add(worldlodvertex(vec(x * WORLD_CHUNK_SIZE / float(resolution), y * WORLD_CHUNK_SIZE / float(resolution), waterz),
                                               vec(0, 0, 1), worldlodmaterialcolor(0, true)));
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

static bool pendingworldlodjob(const worldlodkey &key)
{
    bool pending = false;
    SDL_LockMutex(worldlodmutex);
    loopv(worldlodjobs) if(worldlodjobs[i]->key == key) { pending = true; break; }
    if(!pending) loopv(worldlodactivejobs) if(worldlodactivejobs[i]->key == key) { pending = true; break; }
    if(!pending) loopv(worldlodresults) if(worldlodresults[i]->key == key) { pending = true; break; }
    SDL_UnlockMutex(worldlodmutex);
    return pending;
}

static void queueworldlodjob(const worldlodkey &key, int priority, float distance)
{
    if(findworldlodcache(key) >= 0 || !startworldlodworkers() || pendingworldlodjob(key)) return;
    SDL_LockMutex(worldlodmutex);
    const int outstanding = worldlodjobs.length() + worldlodactivejobs.length() + worldlodresults.length();
    if(outstanding < worldlodpendinglimit)
    {
        worldlodjobs.add(new worldlodjob(key, priority, distance, worldlodepoch));
        worldlodcachemisses++;
        SDL_CondSignal(worldlodcond);
    }
    SDL_UnlockMutex(worldlodmutex);
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

static bool activeworldlodcacheentry(int cacheindex)
{
    if(!worldlodcache.inrange(cacheindex)) return false;
    const worldlodkey &key = worldlodcache[cacheindex].key;
    loopv(worldlodselections) if(worldlodselections[i].active == key.lod && worldlodselections[i].x == key.x && worldlodselections[i].y == key.y)
        return true;
    return false;
}

static void pruneworldlodcache()
{
    while(worldlodcache.length() > worldlodcachelimit)
    {
        int oldest = -1;
        loopv(worldlodcache)
        {
            if(activeworldlodcacheentry(i)) continue;
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

    int active1 = 0, active2 = 0;
    loopv(worldlodselections)
    {
        worldlodselection &selection = worldlodselections[i];
        if(selection.lastseen != totalmillis) continue;
        if(selection.desired == 0)
        {
            if(worldlodfullready(selection.x, selection.y)) selection.active = 0;
            continue;
        }

        const worldlodkey key = currentworldlodkey(selection.x, selection.y, selection.desired);
        int cacheindex = findworldlodcache(key);
        if(cacheindex >= 0)
        {
            if(selection.active != selection.desired) worldlodcachehits++;
            selection.active = selection.desired;
            worldlodcache[cacheindex].lastused = totalmillis;
            const int chunkindex = findworldchunk(selection.x, selection.y);
            if(worldchunks.inrange(chunkindex) && worldchunkmounted(worldchunks[chunkindex])) unmountworldchunk(worldchunks[chunkindex]);
        }
        else
        {
            ivec origin((selection.x - worldfirstchunkx) * WORLD_CHUNK_SIZE, (selection.y - worldfirstchunky) * WORLD_CHUNK_SIZE, 0);
            const bool visible = !viewfrustumvalid() || isvisiblebb(origin, ivec(WORLD_CHUNK_SIZE, WORLD_CHUNK_SIZE, WORLD_MAP_SIZE)) < VFC_FOGGED;
            const int priority = selection.desired == 1 ? 0 : visible ? 1 : 3;
            queueworldlodjob(key, priority, selection.distance);
        }

        if(selection.desired == 1 && selection.distance > (worldlodfardistance - 2 * worldlodhysteresis) * WORLD_BLOCK_SIZE)
            queueworldlodjob(currentworldlodkey(selection.x, selection.y, 2), 2, selection.distance);
        else if(selection.desired == 2 && selection.distance < (worldlodfardistance + 2 * worldlodhysteresis) * WORLD_BLOCK_SIZE)
            queueworldlodjob(currentworldlodkey(selection.x, selection.y, 1), 2, selection.distance);
        if(selection.active == 1) active1++;
        else if(selection.active == 2) active2++;
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
    TracyPlot("LOD/Queued jobs", int64_t(queued));
    TracyPlot("LOD/Cache hits", int64_t(worldlodcachehits));
    TracyPlot("LOD/Cache misses", int64_t(worldlodcachemisses));
    (void)queued;
}

void renderworldlods()
{
    if(!worldlod || !camera1 || worldlodselections.empty()) return;
    Shader *shader = useshaderbyname("worldlod");
    if(!shader) return;
    ZoneScopedN("Render/G-buffer/World LOD");
    shader->set();
    int vertices = 0, triangles = 0;
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
        gle::colorpointer(sizeof(worldlodvertex), pointer->color.v);
        gle::enablevertex();
        gle::enablenormal();
        gle::enablecolor();
        LOCALPARAM(lodmeshoffset, vec(origin));
        const int indices = chunk.terrainindices + chunk.waterindices;
        glDrawElements(GL_TRIANGLES, indices, GL_UNSIGNED_INT, 0);
        vertices += chunk.vertices;
        triangles += indices / 3;
        glde++;
    }
    gle::disablevertex();
    gle::disablenormal();
    gle::disablecolor();
    gle::clearebo();
    gle::clearvbo();
    worldlodrendervertices = vertices;
    worldlodrendertriangles = triangles;
    TracyPlot("LOD/Vertices", int64_t(vertices));
    TracyPlot("LOD/Triangles", int64_t(triangles));
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
ICOMMAND(getdebuglodcachehits, "", (), intret(worldlodcachehits));
ICOMMAND(getdebuglodcachemisses, "", (), intret(worldlodcachemisses));
ICOMMAND(getdebuglodvertices, "", (), intret(worldlodrendervertices));
ICOMMAND(getdebuglodtriangles, "", (), intret(worldlodrendertriangles));
ICOMMAND(getdebuglodgenerationms, "", (), floatret(float(worldlodlastgeneration)));
ICOMMAND(getdebugloduploadms, "", (), floatret(float(worldlodlastupload)));

#endif
