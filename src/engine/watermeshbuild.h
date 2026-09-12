#ifndef ENGINE_WATERMESHBUILD_H
#define ENGINE_WATERMESHBUILD_H
#include "watermeshpacket.h"

template<class CellAt>
static void buildwatermeshpacket(watermeshpacket &resource, const materialsurface *surfaces, int count, CellAt cellat)
{

    vector<waterfacepatch> faces[8], pending;
    vector<int> wavesizes[4];
    loopi(count)
    {
        const materialsurface &m = surfaces[i];
        if((m.material & MATF_VOLUME) != MAT_WATER || m.visible == MATSURF_EDIT_ONLY) continue;
        const int group = (m.material & MATF_INDEX) + (m.orient == O_TOP ? 0 : 4);
        exposedwaterpatches(waterfacepatch(m.o, m.orient, m.rsize, m.csize), cellat,
            [&](const waterfacepatch &face)
            {
                if(face.orient == O_TOP)
                {
                    // Fixed tessellation: camera and waves never change the index/vertex buffers.
                    for(int y = 0; y < face.csize; y += 4) for(int x = 0; x < face.rsize; x += 4)
                    {
                        faces[group].add(waterfacepatch(ivec(face.origin).add(ivec(x, y, 0)), O_TOP,
                                                       min(4, face.rsize - x), min(4, face.csize - y)));
                        wavesizes[group].add(m.csize);
                    }
                }
                else if(face.orient == O_BOTTOM) faces[group].add(face);
                else
                {
                    const int dim = dimension(face.orient), width = dim == 0 ? face.rsize : face.csize;
                    for(int along = 0; along < width;)
                    {
                        ivec origin(face.origin);
                        origin[1 - dim] += along;
                        const int length = min(width - along, 16 - (origin[1 - dim] & 15));
                        faces[group].add(waterfacepatch(origin, face.orient, dim == 0 ? length : face.rsize,
                                                       dim == 0 ? face.csize : length));
                        along += length;
                    }
                }
            }, pending);
    }
    bool any = false;
    loopi(8) if(!faces[i].empty()) any = true;
    if(!any) return;
    ZoneScopedN("Water/Build section topology");


    vector<watermeshvertex> &vertices = resource.vertices;
    vector<uint> &indices = resource.indices;
    loop(g, 8)
    {
        if(g >= 4) mergewaterfallpatches(faces[g]);
        resource.first[g] = indices.length();
        loopv(faces[g])
        {
            const waterfacepatch &face = faces[g][i];
            const ivec &o = face.origin;
            vec p[4];
            if(dimension(face.orient) == 2)
            {
                p[0] = vec(o);
                p[1] = vec(o).add(vec(face.rsize, 0, 0));
                p[2] = vec(o).add(vec(face.rsize, face.csize, 0));
                p[3] = vec(o).add(vec(0, face.csize, 0));
                if(face.orient == O_BOTTOM) swap(p[1], p[3]);
            }
            else
            {
                const int dim = dimension(face.orient), sign = dimcoord(face.orient) ? 1 : -1;
                p[0] = p[3] = vec(o);
                p[1] = p[2] = vec(o);
                p[1][1 - dim] += dim == 0 ? face.rsize : face.csize;
                p[2][1 - dim] = p[1][1 - dim];
                loopj(4) p[j][dim] += sign * 0.1f;
                if((dim == 0) != (sign > 0)) swap(p[1], p[3]);
            }
            watermeshpatch &patch = resource.patches.add();
            patch.face = face;
            patch.material = g & 3;
            patch.first = vertices.length();
            const int wavemask = g < 4 ? wavesizes[g][i] - 1 : 0;
            loopj(4)
            {
                watermeshvertex &v = vertices.add();
                v.position = vec4(p[j], float((int(p[j].x) & wavemask) * (int(p[j].y) & wavemask)) * (59.0f / 23.0f / (2 * M_PI)));
                v.normal = vec(0, 0, 0);
                v.normal[dimension(face.orient)] = dimcoord(face.orient) ? 1 : -1;
            }
            static const int order[6] = { 0, 1, 2, 0, 2, 3 };
            loopj(6) indices.add(patch.first + order[j]);
        }
        resource.count[g] = indices.length() - resource.first[g];
    }
}

#endif
