// worldvbo.cpp: streamed geometry buffer ownership and GPU retirement

#ifdef OCTARENDER_MODULE_IMPLEMENTATION

struct vboinfo
{
    int uses, capacity, type;
    bool streaming;
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

// Poll retired allocations without waiting for their last draw. Unsignaled fences
// never become writable merely because a fixed number of frames has elapsed.
struct retiredworldvbo
{
    GLuint buffer;
    GLsync fence;
    int capacity, type;
};

static vector<retiredworldvbo> retiredworldvbos;
static int retiredworldvbobytes = 0;
static PFNGLFENCESYNCPROC worldfencesync = NULL;
static PFNGLCLIENTWAITSYNCPROC worldclientwaitsync = NULL;
static PFNGLDELETESYNCPROC worlddeletesync = NULL;
static bool worldsyncinitialized = false;
VARP(chunkvbocachemb, 0, 32, 256);

static bool initworldvbosync()
{
    if(!worldsyncinitialized)
    {
        worldsyncinitialized = true;
        if(glversion >= 320 || SDL_GL_ExtensionSupported("GL_ARB_sync"))
        {
            worldfencesync = (PFNGLFENCESYNCPROC)SDL_GL_GetProcAddress("glFenceSync");
            worldclientwaitsync = (PFNGLCLIENTWAITSYNCPROC)SDL_GL_GetProcAddress("glClientWaitSync");
            worlddeletesync = (PFNGLDELETESYNCPROC)SDL_GL_GetProcAddress("glDeleteSync");
        }
    }
    return worldfencesync && worldclientwaitsync && worlddeletesync;
}

static void discardretiredworldvbo(int index)
{
    retiredworldvbo &entry = retiredworldvbos[index];
    worlddeletesync(entry.fence);
    glDeleteBuffers_(1, &entry.buffer);
    retiredworldvbobytes -= entry.capacity;
    retiredworldvbos.removeunordered(index);
}

void cleanupstreamingvbos()
{
    while(!retiredworldvbos.empty()) discardretiredworldvbo(retiredworldvbos.length() - 1);
    worldsyncinitialized = false;
    worldfencesync = NULL;
    worldclientwaitsync = NULL;
    worlddeletesync = NULL;
}

static GLuint acquireworldvbo(int type, int capacity)
{
    while(!retiredworldvbos.empty() && retiredworldvbobytes > chunkvbocachemb * 1024 * 1024)
        discardretiredworldvbo(retiredworldvbos.length() - 1);
    loopv(retiredworldvbos)
    {
        retiredworldvbo &entry = retiredworldvbos[i];
        if(entry.type != type || entry.capacity != capacity) continue;
        GLenum status = worldclientwaitsync(entry.fence, 0, 0);
        if(status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) continue;
        GLuint buffer = entry.buffer;
        worlddeletesync(entry.fence);
        retiredworldvbobytes -= entry.capacity;
        retiredworldvbos.removeunordered(i);
        return buffer;
    }
    return 0;
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
        GLsync fence = NULL;
        if(vbi.streaming && retiredworldvbos.length() < 256 &&
           retiredworldvbobytes + vbi.capacity <= chunkvbocachemb * 1024 * 1024 && initworldvbosync())
            fence = worldfencesync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if(fence)
        {
            retiredworldvbo &entry = retiredworldvbos.add();
            entry.buffer = vbo;
            entry.fence = fence;
            entry.capacity = vbi.capacity;
            entry.type = vbi.type;
            retiredworldvbobytes += entry.capacity;
        }
        else glDeleteBuffers_(1, &vbo);
        if(vbi.data) delete[] vbi.data;
        vbos.remove(vbo);
    }
}

void genvbo(int type, uchar *buf, int len, vtxarray **vas, int numva)
{
    ZoneScopedN("Geometry/Upload vertex buffer");
    ZoneValue(len);
    gle::disable();

    const bool streaming = getworldsectionsize() > 0 && initworldvbosync();
    int capacity = len;
    if(streaming)
    {
        capacity = 4096;
        while(capacity < len) capacity *= 2;
    }
    GLuint vbo = streaming ? acquireworldvbo(type, capacity) : 0;
    const bool reused = vbo != 0;
    if(!reused) glGenBuffers_(1, &vbo);
    GLenum target = type==VBO_VBUF ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
    glBindBuffer_(target, vbo);
    if(streaming)
    {
        if(!reused) glBufferData_(target, capacity, NULL, GL_DYNAMIC_DRAW);
        glBufferSubData_(target, 0, len, buf);
    }
    else glBufferData_(target, len, buf, GL_STATIC_DRAW);
    glBindBuffer_(target, 0);
    worldvauploadbytes += len;
    if(type == VBO_VBUF) worldvauploadvertices += len / int(sizeof(vertex));

    vboinfo &vbi = vbos[vbo];
    vbi.uses = numva;
    vbi.capacity = capacity;
    vbi.type = type;
    vbi.streaming = streaming;
    // Transfer staging storage to the shadow-mesh/export readers without copying.
    vbi.data = buf;
    TracyPlot("Chunks/Retired GPU bytes", int64_t(retiredworldvbobytes));
    TracyPlot("Chunks/GPU buffer reused", int64_t(reused));

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
    const int len = data.length();
    genvbo(type, data.disown(), len, vas.getbuf(), vas.length());
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


#endif
