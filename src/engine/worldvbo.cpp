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

// Keep one append-only page open across frame boundaries. Every stream rotates
// together, preserving the renderer's vbuf -> index-buffer pairing invariant.
static GLuint worldvbopage[NUMVBO] = { 0 };
static int worldvbopageused[NUMVBO] = { 0 }, worldvbopageverts = 0;
VAR(chunkvbopages, 0, 1, 1);
static void releaseworldvbopage();

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
static GLuint worldmeshpages[2] = { 0, 0 };
static int worldmeshused[2] = { 0, 0 };
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
    loopi(2)
    {
        if(worldmeshpages[i]) destroyvbo(worldmeshpages[i]);
        worldmeshpages[i] = 0;
        worldmeshused[i] = 0;
    }
    releaseworldvbopage();
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

void getworldvauploadstats(int &bytes, int &vertices, bool includepending)
{
    bytes = worldvauploadbytes;
    vertices = worldvauploadvertices;
    if(includepending)
    {
        loopi(NUMVBO) bytes += vbodata[i].length();
        vertices += vbosize[VBO_VBUF];
    }
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

// Byte-addressed ranges for section attachments. Use the world's reference counting,
// page ownership and fence retirement; published mesh ranges are never overwritten.
void uploadworldmesh(worldmeshrange &range, GLenum target, const void *data, int bytes, int alignment)
{
    if(range.buffer) destroyvbo(range.buffer);
    range = worldmeshrange();
    if(!bytes) return;
    const int stream = target == GL_ARRAY_BUFFER ? 0 : 1, type = NUMVBO + stream;
    const int capacity = max(1 << 20, (bytes + 4095) & ~4095);
    ASSERT(alignment > 0);
    int offset = ((worldmeshused[stream] + alignment - 1) / alignment) * alignment;
    if(worldmeshpages[stream] && offset + bytes > vbos[worldmeshpages[stream]].capacity)
    {
        destroyvbo(worldmeshpages[stream]);
        worldmeshpages[stream] = 0;
    }
    gle::disable();
    if(!worldmeshpages[stream])
    {
        GLuint buffer = initworldvbosync() ? acquireworldvbo(type, capacity) : 0;
        const bool reused = buffer != 0;
        if(!reused) glGenBuffers_(1, &buffer);
        glBindBuffer_(target, buffer);
        if(!reused) glBufferData_(target, capacity, NULL, GL_DYNAMIC_DRAW);
        vboinfo &info = vbos[buffer];
        info.uses = 1;
        info.capacity = capacity;
        info.type = type;
        info.streaming = true;
        info.data = NULL;
        worldmeshpages[stream] = buffer;
        offset = 0;
    }
    range.buffer = worldmeshpages[stream];
    range.offset = offset;
    ++vbos[range.buffer].uses;
    glBindBuffer_(target, range.buffer);
    glBufferSubData_(target, offset, bytes, data);
    glBindBuffer_(target, 0);
    worldmeshused[stream] = offset + bytes;
    worldvauploadbytes += bytes;
}

static void releaseworldvbopage()
{
    loopi(NUMVBO)
    {
        if(worldvbopage[i]) destroyvbo(worldvbopage[i]);
        worldvbopage[i] = 0;
        worldvbopageused[i] = 0;
    }
    worldvbopageverts = 0;
}

static bool flushworldvbopage()
{
    if(!chunkvbopages || !getworldsectionsize())
    {
        releaseworldvbopage();
        return false;
    }
    if(vbodata[VBO_VBUF].empty()) return false;
    loopi(NUMVBO) if(vbosize[i] > (i == VBO_VBUF ? int(USHRT_MAX) + 1 : int(USHRT_MAX)))
    {
        // Oversized standalone VAs retain the original dedicated-buffer path.
        releaseworldvbopage();
        return false;
    }
    ZoneScopedN("Geometry/Pack streaming buffers");
    // A single VA can exceed the preferred VBO size. Always fit that VA while
    // retaining the engine's 16-bit vertex and index-offset limits.
    const int vertexlimit = min(max(maxvbosize, vbosize[VBO_VBUF]), int(USHRT_MAX) + 1);
    bool fits = worldvbopageverts >= vertexlimit;
    loopi(NUMVBO) if(worldvbopageused[i] + vbosize[i] > (i == VBO_VBUF ? worldvbopageverts : int(USHRT_MAX))) fits = false;
    if(!fits) releaseworldvbopage();
    gle::disable();
    if(!worldvbopage[VBO_VBUF])
    {
        worldvbopageverts = vertexlimit;
        loopi(NUMVBO)
        {
            const int capacity = i == VBO_VBUF ? vertexlimit * int(sizeof(vertex)) : USHRT_MAX * int(sizeof(ushort));
            GLuint buffer = initworldvbosync() ? acquireworldvbo(i, capacity) : 0;
            const bool reused = buffer != 0;
            if(!reused) glGenBuffers_(1, &buffer);
            const GLenum target = i == VBO_VBUF ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
            glBindBuffer_(target, buffer);
            if(!reused) glBufferData_(target, capacity, NULL, GL_DYNAMIC_DRAW);
            glBindBuffer_(target, 0);
            worldvbopage[i] = buffer;
            vboinfo &vbi = vbos[buffer];
            vbi.uses = 1; // The open page owns one reference, in addition to its VAs.
            vbi.capacity = capacity;
            vbi.type = i;
            vbi.streaming = true;
            vbi.data = NULL;
        }
    }
    const int vertexbase = worldvbopageused[VBO_VBUF];
    if(vertexbase)
    {
        loopi(NUMVBO) if(i != VBO_VBUF)
        {
            ushort *indices = (ushort *)vbodata[i].getbuf();
            loopj(vbosize[i]) indices[j] += vertexbase;
        }
        loopv(vbovas[VBO_VBUF])
        {
            vtxarray &va = *vbovas[VBO_VBUF][i];
            va.minvert += vertexbase;
            va.maxvert += vertexbase;
            const int texs = va.texs + va.blends + va.alphaback + va.alphafront + va.refract;
            loopj(texs) if(va.texelems[j].length)
            {
                va.texelems[j].minvert += vertexbase;
                va.texelems[j].maxvert += vertexbase;
            }
            loopj(va.decaltexs) if(va.decalelems[j].length)
            {
                va.decalelems[j].minvert += vertexbase;
                va.decalelems[j].maxvert += vertexbase;
            }
        }
    }
    loopi(NUMVBO)
    {
        vector<uchar> &data = vbodata[i];
        vector<vtxarray *> &vas = vbovas[i];
        if(data.empty()) continue;
        vboinfo &vbi = vbos[worldvbopage[i]];
        if(!vbi.data) vbi.data = new uchar[vbi.capacity];
        const int offset = worldvbopageused[i], byteoffset = offset * (i == VBO_VBUF ? int(sizeof(vertex)) : int(sizeof(ushort)));
        const GLenum target = i == VBO_VBUF ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
        ASSERT(byteoffset + data.length() <= vbi.capacity);
        memcpy(vbi.data + byteoffset, data.getbuf(), data.length());
        glBindBuffer_(target, worldvbopage[i]);
        // Only the unused tail is written. Previously published ranges and CPU
        // pointers remain stable until the last owning VA releases the page.
        glBufferSubData_(target, byteoffset, data.length(), data.getbuf());
        glBindBuffer_(target, 0);
        worldvauploadbytes += data.length();
        if(i == VBO_VBUF) worldvauploadvertices += vbosize[i];
        vbi.uses += vas.length();
        loopvj(vas)
        {
            vtxarray &va = *vas[j];
            switch(i)
            {
                case VBO_VBUF: va.vbuf = worldvbopage[i]; va.vdata = (vertex *)vbi.data; va.voffset += offset; break;
                case VBO_EBUF: va.ebuf = worldvbopage[i]; va.edata = (ushort *)vbi.data; va.eoffset += offset; break;
                case VBO_SKYBUF: va.skybuf = worldvbopage[i]; va.skydata = (ushort *)vbi.data; va.skyoffset += offset; break;
                case VBO_DECALBUF: va.decalbuf = worldvbopage[i]; va.decaldata = (ushort *)vbi.data; va.decaloffset += offset; break;
            }
        }
        worldvbopageused[i] += vbosize[i];
        data.setsize(0);
        vas.setsize(0);
        vbosize[i] = 0;
    }
    TracyPlot("Chunks/Open buffer page vertices", int64_t(worldvbopageused[VBO_VBUF]));
    return true;
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

void flushvbo(int type)
{
    if(type < 0)
    {
        if(flushworldvbopage()) return;
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
