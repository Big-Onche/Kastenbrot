// Included by texture.cpp. Runtime-only copies; loose assets remain authoritative.
struct blocktexturearray
{
    GLuint id = 0;
    int renderclass, width, height, format, clamp, filter, geometry;
    vector<Texture *> textures;

    ~blocktexturearray()
    {
        if(id) glDeleteTextures(1, &id);
    }
};

struct blocktexturesource
{
    Texture *texture;
    int renderclass;
};

static vector<blocktexturesource> blocktexturesources;
static vector<blocktexturearray *> blocktexturearrays;
static uint blocktexturearraygeneration = 1;

uint getblocktexturearraygeneration()
{
    return blocktexturearraygeneration;
}

void cleanupblocktexturearrays()
{
    // Published packets contain GL array names. Retire workers and packets first.
    clearworldmeshpackets();
    ++blocktexturearraygeneration;
    blocktexturearrays.deletecontents();
    blocktexturesources.setsize(0);
}

void beginblocktexturearrays()
{
    cleanupblocktexturearrays();
}

void registerblocktexture(Texture *texture, int renderclass)
{
    if(!texture || texture == notexture || !texture->id || !texture->mipmap || texture->bpp < 3 ||
       !(texture->type & Texture::GEOMETRY) || (texture->type & Texture::TYPE) != Texture::IMAGE) return;
    loopv(blocktexturesources)
        if(blocktexturesources[i].texture == texture && blocktexturesources[i].renderclass == renderclass) return;
    blocktexturesource &source = blocktexturesources.add();
    source.texture = texture;
    source.renderclass = renderclass;
}

int blocktexturemode(const VSlot &vslot)
{
    const Slot &slot = *vslot.slot;
    if(!slot.shader || vslot.layer || vslot.detail || vslot.refractscale > 0) return -1;
    loopv(slot.sts) if(slot.sts[i].type != TEX_DIFFUSE && slot.sts[i].type != TEX_ALPHA) return -1;
    static const char *names[] = { "stdworld", "grassclimateworld", "sandclimateworld", "dirtclimateworld",
                                  "grassclimateworldside", "leafclimateworld", "leafworld", "birchleafclimateworld" };
    loopi(sizeof(names) / sizeof(names[0])) if(!strcmp(slot.shader->name, names[i])) return i;
    return -1;
}

bool compatibleblocktexturestate(const VSlot &a, const VSlot &b)
{
    // UV scale/rotation/offset are already baked in vertices. Scroll remains a uniform.
    if(a.colorscale != b.colorscale || a.scroll != b.scroll || (!a.scroll.iszero() && a.rotation != b.rotation) ||
       a.params.length() != b.params.length() || a.slot->params.length() != b.slot->params.length()) return false;
    loopi(2)
    {
        const vector<SlotShaderParam> &ap = i ? a.params : a.slot->params, &bp = i ? b.params : b.slot->params;
        loopvj(ap) if(strcmp(ap[j].name, bp[j].name) || ap[j].flags != bp[j].flags || memcmp(ap[j].val, bp[j].val, sizeof(ap[j].val)))
            return false;
    }
    return true;
}

GLuint lookupblocktexturearray(Texture *texture, int renderclass, ushort &layer)
{
    loopv(blocktexturearrays)
    {
        const blocktexturearray &array = *blocktexturearrays[i];
        if(!array.id || array.renderclass != renderclass) continue;
        loopvj(array.textures) if(array.textures[j] == texture)
        {
            layer = ushort(j);
            return array.id;
        }
    }
    return 0;
}

void updateblocktexturearrayfilter()
{
    if(blocktexturearrays.empty()) return;
    GLint previous;
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previous);
    loopv(blocktexturearrays)
    {
        const blocktexturearray &array = *blocktexturearrays[i];
        if(!array.id) continue;
        glBindTexture(GL_TEXTURE_2D_ARRAY, array.id);
        settexfilter(GL_TEXTURE_2D_ARRAY, array.filter, array.geometry != 0);
        if(hasAF && array.filter > 1)
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY_EXT, max(1, min(aniso, hwmaxaniso)));
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, previous);
}

static bool blocktexturesourceorder(const blocktexturesource &a, const blocktexturesource &b)
{
    if(a.renderclass != b.renderclass) return a.renderclass < b.renderclass;
    return strcmp(a.texture->name, b.texture->name) < 0;
}

void bakeblocktexturearrays()
{
    if(blocktexturesources.empty() || glversion < 300 || glslversion < 130) return;
    static const char *shaders[] = { "blockarrayworld", "rsmblockarrayworld", "smblockarrayworld", "scatterarrayworld", "smscatterarray" };
    loopi(5)
    {
        Shader *shader = useshaderbyname(shaders[i]);
        if(!shader || shader->isnull() || !shader->loaded()) return;
    }
    GLint limit = 0, old2d = 0, oldarray = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &limit);
    if(limit <= 0) return;
    limit = min(limit, 65536);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old2d);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &oldarray);
    const GLenum stores[] = { GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS,
                              GL_PACK_IMAGE_HEIGHT, GL_PACK_SKIP_IMAGES, GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH,
                              GL_UNPACK_SKIP_PIXELS, GL_UNPACK_SKIP_ROWS, GL_UNPACK_IMAGE_HEIGHT, GL_UNPACK_SKIP_IMAGES };
    GLint storevalues[12], packbuffer = 0, unpackbuffer = 0;
    loopi(12)
    {
        glGetIntegerv(stores[i], &storevalues[i]);
        glPixelStorei(stores[i], i == 0 || i == 6 ? 1 : 0);
    }
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packbuffer);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackbuffer);
    glBindBuffer_(GL_PIXEL_PACK_BUFFER, 0);
    glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, 0);
    blocktexturesources.sort(blocktexturesourceorder);
    loopv(blocktexturesources)
    {
        const blocktexturesource &source = blocktexturesources[i];
        Texture &texture = *source.texture;
        glBindTexture(GL_TEXTURE_2D, texture.id);
        GLint format = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
        // Normalize compatible linear color formats before grouping, so RGB dirt
        // and RGBA grass-side layers can share one RGBA8 array. Keep other formats loose.
        switch(format)
        {
            case GL_RGB: case GL_RGB8: case GL_RGBA: case GL_RGBA8:
            case GL_COMPRESSED_RGB: case GL_COMPRESSED_RGBA:
            case GL_COMPRESSED_RGB_S3TC_DXT1_EXT: case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                format = GL_RGBA8;
                break;
            default: continue;
        }
        const int filter = !texture.canreduce || reducefilter ? 2 : 0, geometry = (texture.type & Texture::GEOMETRY) != 0;
        blocktexturearray *array = NULL;
        loopvj(blocktexturearrays)
        {
            blocktexturearray &candidate = *blocktexturearrays[j];
            if(candidate.renderclass == source.renderclass && candidate.width == texture.w && candidate.height == texture.h &&
               candidate.format == format && candidate.clamp == texture.clamp && candidate.filter == filter &&
               candidate.geometry == geometry && candidate.textures.length() < limit) { array = &candidate; break; }
        }
        if(!array)
        {
            array = new blocktexturearray;
            array->renderclass = source.renderclass;
            array->width = texture.w;
            array->height = texture.h;
            array->format = format;
            array->clamp = texture.clamp;
            array->filter = filter;
            array->geometry = geometry;
            blocktexturearrays.add(array);
        }
        array->textures.add(&texture);
    }
    int hits = 0, baked = 0;
    loopv(blocktexturearrays)
    {
        blocktexturearray &array = *blocktexturearrays[i];
        // RGBA8 stores the sampled values of the original mip chain, including
        // compressed source textures. Layers never mix during mip filtering.
        vector<uchar> layout, pixels;
        const int settings[] = { 1, array.width, array.height, array.format, array.clamp, array.filter, array.geometry,
                                 array.renderclass, array.textures.length(), texreduce, texcompress, texcompressquality, usetexcompress };
        layout.put((const uchar *)settings, sizeof(settings));
        const GLenum driverkeys[] = { GL_VERSION, GL_VENDOR, GL_RENDERER };
        loopj(3)
        {
            const char *driver = (const char *)glGetString(driverkeys[j]);
            layout.put((const uchar *)driver, strlen(driver) + 1);
        }
        loopvj(array.textures)
        {
            const Texture &texture = *array.textures[j];
            layout.put((const uchar *)texture.name, strlen(texture.name) + 1);
            layout.put((const uchar *)&texture.sourcehash, sizeof(texture.sourcehash));
            glBindTexture(GL_TEXTURE_2D, texture.id);
            GLint sourceformat = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &sourceformat);
            layout.put((const uchar *)&sourceformat, sizeof(sourceformat));
        }
        const ullong hash = blocktexturehash(14695981039346656037ULL, layout.getbuf(), layout.length());
        int levels = 0;
        ullong size = 0;
        for(int w = array.width, h = array.height;; w = max(w / 2, 1), h = max(h / 2, 1))
        {
            ++levels;
            size += ullong(w) * h * 4 * array.textures.length();
            if(w == 1 && h == 1) break;
        }
        if(size > 256 * 1024 * 1024) continue;
        pixels.pad(int(size));
        defformatstring(filename, "tmp/blockarray-%d.bin", i);
        // Use the same home-directory resolver as saved worlds (-u). Never use
        // package search paths for disposable cache files.
        string cachepath;
        copystring(cachepath, findfile(path(filename), "w"));
        createdir(findfile("tmp", "w"));
        stream *cache = fileexists(cachepath, "r") ? openrawfile(filename, "rb") : NULL;
        bool hit = false;
        if(cache)
        {
            ullong cachedhash = 0;
            uint checksum = 0;
            vector<uchar> cachedlayout;
            cachedlayout.pad(layout.length());
            hit = cache->size() == stream::offset(sizeof(hash) + layout.length() + size + sizeof(checksum)) &&
                  cache->read(&cachedhash, sizeof(cachedhash)) == sizeof(cachedhash) && cachedhash == hash &&
                  cache->read(cachedlayout.getbuf(), layout.length()) == size_t(layout.length()) &&
                  !memcmp(layout.getbuf(), cachedlayout.getbuf(), layout.length()) &&
                  cache->read(pixels.getbuf(), pixels.length()) == size_t(pixels.length()) &&
                  cache->read(&checksum, sizeof(checksum)) == sizeof(checksum) &&
                  checksum == uint(crc32(0, pixels.getbuf(), pixels.length()));
            delete cache;
        }
        bool complete = true;
        if(!hit)
        {
            int offset = 0;
            loopj(levels)
            {
                const int w = max(array.width >> j, 1), h = max(array.height >> j, 1), bytes = w * h * 4;
                loopvk(array.textures)
                {
                    glBindTexture(GL_TEXTURE_2D, array.textures[k]->id);
                    GLint mw = 0, mh = 0;
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, j, GL_TEXTURE_WIDTH, &mw);
                    glGetTexLevelParameteriv(GL_TEXTURE_2D, j, GL_TEXTURE_HEIGHT, &mh);
                    if(mw != w || mh != h) { complete = false; break; }
                    glGetTexImage(GL_TEXTURE_2D, j, GL_RGBA, GL_UNSIGNED_BYTE, pixels.getbuf() + offset);
                    offset += bytes;
                }
                if(!complete) break;
            }
            if(!complete) continue;
            cache = openrawfile(filename, "wb");
            if(cache)
            {
                const uint checksum = uint(crc32(0, pixels.getbuf(), pixels.length()));
                cache->write(&hash, sizeof(hash));
                cache->write(layout.getbuf(), layout.length());
                cache->write(pixels.getbuf(), pixels.length());
                cache->write(&checksum, sizeof(checksum));
                delete cache; // Partial writes are rejected by size and checksum on next load.
            }
            ++baked;
        }
        else ++hits;
        glGenTextures(1, &array.id);
        glBindTexture(GL_TEXTURE_2D_ARRAY, array.id);
        int offset = 0;
        loopj(levels)
        {
            const int w = max(array.width >> j, 1), h = max(array.height >> j, 1);
            glTexImage3D_(GL_TEXTURE_2D_ARRAY, j, GL_RGBA8, w, h, array.textures.length(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                          pixels.getbuf() + offset);
            offset += w * h * 4 * array.textures.length();
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, levels - 1);
        bool uploaded = true;
        loopj(levels)
        {
            GLint depth = 0, width = 0, height = 0;
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, j, GL_TEXTURE_DEPTH, &depth);
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, j, GL_TEXTURE_WIDTH, &width);
            glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, j, GL_TEXTURE_HEIGHT, &height);
            if(depth != array.textures.length() || width != max(array.width >> j, 1) || height != max(array.height >> j, 1)) uploaded = false;
        }
        if(!uploaded)
        {
            glDeleteTextures(1, &array.id);
            array.id = 0;
            conoutf(CON_WARN, "block texture array upload failed; using loose textures");
            continue;
        }
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S,
                        array.clamp & 1 ? GL_CLAMP_TO_EDGE : (array.clamp & 0x100 ? GL_MIRRORED_REPEAT : GL_REPEAT));
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T,
                        array.clamp & 2 ? GL_CLAMP_TO_EDGE : (array.clamp & 0x200 ? GL_MIRRORED_REPEAT : GL_REPEAT));
    }
    updateblocktexturearrayfilter();
    loopi(12) glPixelStorei(stores[i], storevalues[i]);
    glBindBuffer_(GL_PIXEL_PACK_BUFFER, packbuffer);
    glBindBuffer_(GL_PIXEL_UNPACK_BUFFER, unpackbuffer);
    glBindTexture(GL_TEXTURE_2D, old2d);
    glBindTexture(GL_TEXTURE_2D_ARRAY, oldarray);
    conoutf(CON_INIT, "block texture arrays: %d cached, %d baked, %d sources", hits, baked, blocktexturesources.length());
}

static void reloadblocktexturearrays(Texture *texture)
{
    bool registered = false;
    loopv(blocktexturesources) if(blocktexturesources[i].texture == texture) { registered = true; break; }
    if(!registered) return;
    clearworldmeshpackets();
    ++blocktexturearraygeneration;
    blocktexturearrays.deletecontents();
    bakeblocktexturearrays();
    queueworldmeshworld();
}
