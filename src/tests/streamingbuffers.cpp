#include <algorithm>
#include <map>
#include <set>
#include <vector>
#include "engine.h"
#undef main

// Actual packing, lifetime and draw-range code; GPU calls record and validate
// buffer contents and offsets instead of requiring a live OpenGL context.
namespace buffertest
{
    static GLuint nextbuffer = 1, boundvertex = 0, boundindex = 0;
    static std::map<GLuint, std::vector<uchar>> buffers;
    static std::map<GLuint, int> written;
    static int glversion = 0, glde = 0, gbatches = 0, vtris = 0;
    static std::set<std::pair<GLuint, size_t>> drawn;

    namespace gle
    {
        static void disable()
        {
        }

        static void bindvbo(GLuint buffer)
        {
            boundvertex = buffer;
        }

        static void bindebo(GLuint buffer)
        {
            boundindex = buffer;
        }

        static void vertexpointer(int stride, const void *offset)
        {
        }
    }

    static int getworldsectionsize()
    {
        return 256;
    }

    static SDL_bool SDL_GL_ExtensionSupported(const char *extension)
    {
        return SDL_FALSE;
    }

    static void testconoutf(int type, const char *format, ...)
    {
    }

    static void glGenBuffers_(GLsizei count, GLuint *names)
    {
        loopi(count) names[i] = nextbuffer++;
    }

    static void glDeleteBuffers_(GLsizei count, const GLuint *names)
    {
        loopi(count)
        {
            buffers.erase(names[i]);
            written.erase(names[i]);
        }
    }

    static void glBindBuffer_(GLenum target, GLuint buffer)
    {
        if(target == GL_ARRAY_BUFFER) boundvertex = buffer;
        else boundindex = buffer;
    }

    static void glBufferData_(GLenum target, GLsizeiptr size, const void *data, GLenum usage)
    {
        const GLuint buffer = target == GL_ARRAY_BUFFER ? boundvertex : boundindex;
        buffers[buffer].resize(size);
        written[buffer] = 0;
        if(data)
        {
            memcpy(buffers[buffer].data(), data, size);
            written[buffer] = int(size);
        }
    }

    static void glBufferSubData_(GLenum target, GLintptr offset, GLsizeiptr size, const void *data)
    {
        const GLuint buffer = target == GL_ARRAY_BUFFER ? boundvertex : boundindex;
        assert(offset >= written[buffer]); // Previously published ranges are immutable.
        assert(offset + size <= GLsizeiptr(buffers[buffer].size()));
        memcpy(buffers[buffer].data() + offset, data, size);
        written[buffer] = int(offset + size);
    }

    #undef VAR
    #undef VARP
    #undef VARFN
    #define VAR(name, minimum, initial, maximum) int name = initial
    #define VARP VAR
    #define VARFN(name, variable, minimum, initial, maximum, body) int variable = initial
    #define conoutf testconoutf
    #define OCTARENDER_MODULE_IMPLEMENTATION
    #include "worldvbo.cpp"

    struct renderstate
    {
    };

    struct geombatch
    {
        const elementset &es;
        vtxarray *va;
        int offset, batch;

        geombatch(vtxarray *va, int next) : es(va->texelems[0]), va(va), offset(0), batch(next)
        {
        }
    };
    static vector<geombatch> geombatches;

    static void glMultiDrawElements_(GLenum mode, const GLsizei *counts, GLenum type, const void * const *offsets, GLsizei count)
    {
        loopi(count)
        {
            const size_t offset = size_t(offsets[i]);
            assert(offset + counts[i] * sizeof(ushort) <= buffers[boundindex].size());
            assert(drawn.insert(std::make_pair(boundindex, offset)).second);
            const ushort *indices = (const ushort *)(buffers[boundindex].data() + offset);
            loopj(counts[i]) assert(size_t(indices[j]) * sizeof(vertex) < size_t(written[boundvertex]));
        }
    }

    static void drawtris(GLsizei count, const void *offset, ushort minimum, ushort maximum)
    {
        glMultiDrawElements_(GL_TRIANGLES, &count, GL_UNSIGNED_SHORT, &offset, 1);
        const ushort *indices = (const ushort *)(buffers[boundindex].data() + size_t(offset));
        loopi(count) assert(indices[i] >= minimum && indices[i] <= maximum);
        ++glde;
    }

    #include "draw-production.h"

    static vtxarray *maketile(int id, bool extras = false)
    {
        vtxarray *va = new vtxarray();
        va->verts = 4;
        va->minvert = vbosize[VBO_VBUF];
        va->maxvert = va->minvert + 3;
        vertex *vertices = (vertex *)addvbo(va, VBO_VBUF, 4, sizeof(vertex));
        memset((void *)vertices, id, 4 * sizeof(vertex));
        va->tris = 2;
        va->texs = 1;
        va->texelems = new elementset[extras ? 5 : 1]();
        if(extras) va->blends = va->alphaback = va->alphafront = va->refract = 1;
        loopi(extras ? 5 : 1)
        {
            va->texelems[i].length = 6;
            va->texelems[i].minvert = va->minvert;
            va->texelems[i].maxvert = va->maxvert;
        }
        ushort *indices = (ushort *)addvbo(va, VBO_EBUF, extras ? 30 : 6, sizeof(ushort));
        loopi(extras ? 30 : 6) indices[i] = va->voffset + i % 4;
        if(extras)
        {
            va->sky = 6;
            indices = (ushort *)addvbo(va, VBO_SKYBUF, 6, sizeof(ushort));
            loopi(6) indices[i] = va->voffset + i % 4;
            va->decaltexs = 1;
            va->decalelems = new elementset[1]();
            va->decalelems[0] = va->texelems[0];
            indices = (ushort *)addvbo(va, VBO_DECALBUF, 6, sizeof(ushort));
            loopi(6) indices[i] = va->voffset + i % 4;
        }
        return va;
    }

    static void freetile(vtxarray *va)
    {
        destroyvbo(va->vbuf);
        if(va->ebuf) destroyvbo(va->ebuf);
        if(va->skybuf) destroyvbo(va->skybuf);
        if(va->decalbuf) destroyvbo(va->decalbuf);
        delete[] va->texelems;
        delete[] va->decalelems;
        delete va;
    }

    static void testpacking()
    {
        vector<vtxarray *> tiles;
        loopi(100)
        {
            tiles.add(maketile(i, true));
            flushvbo(-1); // Separate frames must still share a page.
            const vtxarray &va = *tiles.last();
            assert(va.vbuf == tiles[0]->vbuf && va.ebuf == tiles[0]->ebuf);
            assert(va.voffset == i * 4 && va.eoffset == i * 30 && va.skyoffset == i * 6 && va.decaloffset == i * 6);
            assert(va.minvert == i * 4 && va.maxvert == i * 4 + 3);
            loopj(5) assert(va.texelems[j].minvert == va.minvert && va.texelems[j].maxvert == va.maxvert);
            assert(va.decalelems[0].minvert == va.minvert && va.decalelems[0].maxvert == va.maxvert);
            loopj(30) assert(va.edata[va.eoffset + j] == i * 4 + j % 4);
            loopj(6)
            {
                assert(va.skydata[va.skyoffset + j] == i * 4 + j % 4);
                assert(va.decaldata[va.decaloffset + j] == i * 4 + j % 4);
            }
        }
        const uchar *first = (uchar *)(tiles[0]->vdata + tiles[0]->voffset);
        loopi(4 * int(sizeof(vertex))) assert(first[i] == 0);
        vector<vtxarray *> visible;
        loopi(tiles.length()) visible.add(tiles[tiles.length() - 1 - i]);
        drawdepthvas(visible);
        assert(glde == 1 && drawn.size() == 100 && visible.empty());
        drawn.clear();
        glde = 0;
        loopv(tiles) geombatches.add(geombatch(tiles[i], i == tiles.length() - 1 ? -1 : i + 1));
        renderstate state;
        renderbatch(state, 0, geombatches[0]);
        assert(glde == 1 && drawn.size() == 100);
        geombatches.setsize(0);
        drawn.clear();
        const GLuint original = tiles[0]->vbuf;
        maxvbosize = 512;
        // Exhaust the original page without invalidating its existing owners.
        worldvbopageused[VBO_VBUF] = worldvbopageverts;
        vtxarray *next = maketile(100);
        flushvbo(-1);
        assert(next->vbuf != original && vbos.access(original));
        freetile(next);
        loopv(tiles) freetile(tiles[i]);
        assert(!vbos.access(original));
        cleanupstreamingvbos();
        assert(vbos.numelems == 0 && buffers.empty());
        puts("PASS: 100 separate tile uploads share one page; color and depth submit 1 call instead of 100");
    }

    static void testlimits()
    {
        maxvbosize = 65536;
        vector<vtxarray *> tiles;
        tiles.add(maketile(1));
        flushvbo(-1);
        worldvbopageused[VBO_VBUF] = 65532;
        tiles.add(maketile(2));
        flushvbo(-1);
        assert(tiles[1]->vbuf == tiles[0]->vbuf && tiles[1]->voffset == 65532 && tiles[1]->maxvert == 65535);
        tiles.add(maketile(3));
        flushvbo(-1);
        assert(tiles[2]->vbuf != tiles[1]->vbuf && tiles[2]->voffset == 0);
        worldvbopageused[VBO_EBUF] = 65532;
        tiles.add(maketile(4));
        flushvbo(-1);
        assert(tiles[3]->vbuf != tiles[2]->vbuf && tiles[3]->eoffset == 0);
        // Disabling packing retains dedicated uploads and stable old pages.
        chunkvbopages = 0;
        tiles.add(maketile(5));
        flushvbo(-1);
        assert(tiles[4]->voffset == 0 && !worldvbopage[0]);
        loopv(tiles) freetile(tiles[i]);
        cleanupstreamingvbos();
        assert(vbos.numelems == 0 && buffers.empty());
        puts("PASS: 16-bit vertex/index boundaries, alpha/sky/decal relocation and page retirement");
    }
}

int main()
{
    buffertest::testpacking();
    buffertest::testlimits();
    return 0;
}
