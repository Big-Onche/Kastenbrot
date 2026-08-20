#ifndef __ENGINE_WORLDCUBE_H__
#define __ENGINE_WORLDCUBE_H__

struct cubeext;

struct cube
{
    cube *children;
    cubeext *ext;
    union
    {
        uchar edges[12];
        uint faces[3];
    };
    ushort texture[6];
    ushort material;
    uchar merged;
    union
    {
        uchar escaped;
        uchar visible;
    };
};

const uint F_EMPTY = 0;
const uint F_SOLID = 0x80808080;

#define isempty(c) ((c).faces[0] == F_EMPTY)
#define isentirelysolid(c) ((c).faces[0] == F_SOLID && (c).faces[1] == F_SOLID && (c).faces[2] == F_SOLID)
#define setfaces(c, face) { (c).faces[0] = (c).faces[1] = (c).faces[2] = face; }
#define solidfaces(c) setfaces(c, F_SOLID)
#define emptyfaces(c) setfaces(c, F_EMPTY)

#define edgemake(a, b) ((b) << 4 | (a))
#define edgeget(edge, coord) ((coord) ? (edge) >> 4 : (edge) & 0xF)
#define edgeset(edge, coord, val) ((edge) = ((coord) ? ((edge) & 0xF) | ((val) << 4) : ((edge) & 0xF0) | (val)))
#define cubeedge(c, d, x, y) ((c).edges[((d) << 2) + ((y) << 1) + (x)])

static inline void getworldcubevector(const cube &c, int axis, int x, int y, int endpoint, ivec &value)
{
    const ivec vertex(axis, x, y, endpoint);
    loopi(3) value[i] = edgeget(cubeedge(c, i, vertex[R[i]], vertex[C[i]]), vertex[D[i]]);
}

static inline void pushworldcubeedge(uchar &edge, int direction, int endpoint)
{
    const int next = clamp(edgeget(edge, endpoint) + direction, 0, 8);
    edgeset(edge, endpoint, next);
    const int other = edgeget(edge, 1 - endpoint);
    if((direction < 0 && endpoint && other > next) || (direction > 0 && !endpoint && other < next)) edgeset(edge, 1 - endpoint, next);
}

static inline void pushworldcubecorneredge(cube &c, int axis, int x, int y, int endpoint, int direction)
{
    ivec selected, candidate;
    getworldcubevector(c, axis, x, y, endpoint, selected);
    loopi(2) loopj(2)
    {
        getworldcubevector(c, axis, i, j, endpoint, candidate);
        if(selected == candidate) pushworldcubeedge(cubeedge(c, axis, i, j), direction, endpoint);
    }
}

#define octadim(d) (1 << (d))
#define octacoord(d, i) (((i) & octadim(d)) >> (d))
#define oppositeocta(d, i) ((i) ^ octadim(D[d]))
#define octaindex(d, x, y, z) (((z) << D[d]) + ((y) << C[d]) + ((x) << R[d]))
#define octastep(x, y, z, scale) (((((z) >> (scale)) & 1) << 2) | ((((y) >> (scale)) & 1) << 1) | (((x) >> (scale)) & 1))

enum
{
    O_LEFT = 0,
    O_RIGHT,
    O_BACK,
    O_FRONT,
    O_BOTTOM,
    O_TOP,
    O_ANY
};

#define dimension(orient) ((orient) >> 1)
#define dimcoord(orient) ((orient) & 1)
#define opposite(orient) ((orient) ^ 1)

#endif
