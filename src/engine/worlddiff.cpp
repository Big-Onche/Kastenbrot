// worlddiff.cpp: persistent streamed-world chunk diffs and journals

#ifdef WORLDIO_MODULE_IMPLEMENTATION

static worldchunkdiffstate *findworldchunkdiffstate(int x, int y, bool create)
{
    loopv(worldchunkdiffstates)
    {
        worldchunkdiffstate *state = worldchunkdiffstates[i];
        if(state->x == x && state->y == y && state->z == WORLD_DIFF_Z) return state;
    }
    if(!create) return NULL;
    return worldchunkdiffstates.add(new worldchunkdiffstate(x, y));
}

static void worlddiffput32(vector<uchar> &out, uint value)
{
    loopi(4) out.add(uchar(value >> (i * 8)));
}

static void worlddiffput64(vector<uchar> &out, ullong value)
{
    loopi(8) out.add(uchar(value >> (i * 8)));
}

static void worlddiffputbytes(vector<uchar> &out, const void *data, int length)
{
    out.put((const uchar *)data, length);
}

static uint worlddiffchecksum(const uchar *data, int length)
{
    uint hash = 2166136261U;
    loopi(length)
    {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}

struct worldblockpalette
{
    vector<int> blocks;

    int find(int block) const
    {
        loopv(blocks) if(blocks[i] == block) return i;
        return -1;
    }

    int add(int block)
    {
        if(block < 0) return -1;
        int index = find(block);
        if(index >= 0) return index;
        if(blocks.length() >= 0xFFFF) return -1;
        blocks.add(block);
        return blocks.length() - 1;
    }
};

static void serializeworlddiffnode(vector<uchar> &out, const worlddiffnode &node, const worldblockpalette &palette)
{
    worlddiffput32(out, uint(node.x));
    worlddiffput32(out, uint(node.y));
    worlddiffput32(out, uint(node.z));
    worlddiffput32(out, uint(node.size));
    worlddiffputbytes(out, node.edges, sizeof(node.edges));
    const int paletteindex = palette.find(node.block);
    const ushort encoded = paletteindex >= 0 ? ushort(paletteindex) : 0xFFFF;
    out.add(uchar(encoded));
    out.add(uchar(encoded >> 8));
    if(encoded == 0xFFFF) loopi(6)
    {
        out.add(uchar(node.texture[i]));
        out.add(uchar(node.texture[i] >> 8));
    }
    out.add(uchar(node.material));
    out.add(uchar(node.material >> 8));
}

static void serializeworldscatterinstance(vector<uchar> &out, const worldscatterinstance &scatter)
{
    worlddiffput32(out, uint(scatter.x));
    worlddiffput32(out, uint(scatter.y));
    worlddiffput32(out, uint(scatter.z));
    worlddiffput64(out, getworldscatterpersistentid(scatter.type));
    worlddiffput32(out, uint(scatter.orient));
}

static void serializeworldeditrecord(vector<uchar> &out, const worldeditrecord &record, const worldblockpalette &palette)
{
    vector<uchar> body;
    worlddiffput32(body, uint(record.chunkx));
    worlddiffput32(body, uint(record.chunky));
    worlddiffput32(body, uint(record.chunkz));
    worlddiffput64(body, record.revision);
    worlddiffput64(body, record.timestamp);
    worlddiffput32(body, uint(record.author));
    worlddiffput32(body, uint(record.operation));
    worlddiffput32(body, uint(record.selection.o.x));
    worlddiffput32(body, uint(record.selection.o.y));
    worlddiffput32(body, uint(record.selection.o.z));
    worlddiffput32(body, uint(record.selection.s.x));
    worlddiffput32(body, uint(record.selection.s.y));
    worlddiffput32(body, uint(record.selection.s.z));
    worlddiffput32(body, uint(record.selection.grid));
    worlddiffput32(body, uint(record.selection.orient));
    worlddiffput32(body, uint(record.selection.corner));
    loopi(4) worlddiffput32(body, uint(record.args[i]));
    worlddiffput32(body, uint(record.before.length()));
    loopv(record.before) serializeworlddiffnode(body, record.before[i], palette);
    worlddiffput32(body, uint(record.after.length()));
    loopv(record.after) serializeworlddiffnode(body, record.after[i], palette);
    worlddiffput32(body, uint(record.scatterbefore.length()));
    loopv(record.scatterbefore) serializeworldscatterinstance(body, record.scatterbefore[i]);
    worlddiffput32(body, uint(record.scatterafter.length()));
    loopv(record.scatterafter) serializeworldscatterinstance(body, record.scatterafter[i]);
    worlddiffput32(out, uint(body.length()));
    worlddiffput32(out, worlddiffchecksum(body.getbuf(), body.length()));
    worlddiffputbytes(out, body.getbuf(), body.length());
}

static void makeworlddiffframe(vector<uchar> &frame, uchar type, int chunkx, int chunky, const vector<worldeditrecord *> &records,
                               ullong expectedhash = 0)
{
    vector<uchar> payload;
    worldblockpalette palette;
    loopv(records)
    {
        loopvj(records[i]->before) palette.add(records[i]->before[j].block);
        loopvj(records[i]->after) palette.add(records[i]->after[j].block);
    }
    payload.add(type);
    worlddiffput32(payload, WORLD_SAVE_FORMAT_VERSION);
    worlddiffput32(payload, WORLDGEN_VERSION);
    worlddiffput32(payload, uint(chunkx));
    worlddiffput32(payload, uint(chunky));
    worlddiffput32(payload, WORLD_DIFF_Z);
    worlddiffput64(payload, expectedhash);
    worlddiffput32(payload, uint(palette.blocks.length()));
    loopv(palette.blocks) worlddiffput64(payload, getworldcubepersistentid(palette.blocks[i]));
    worlddiffput32(payload, uint(records.length()));
    loopv(records) serializeworldeditrecord(payload, *records[i], palette);

    worlddiffputbytes(frame, "CDF1", 4);
    worlddiffput32(frame, uint(payload.length()));
    worlddiffput32(frame, worlddiffchecksum(payload.getbuf(), payload.length()));
    worlddiffputbytes(frame, payload.getbuf(), payload.length());
}

struct worlddiffflushjob
{
    string filename, auditfilename;
    int chunkx, chunky;
    vector<worldeditrecord *> records;

    worlddiffflushjob() : chunkx(0), chunky(0) {}
    ~worlddiffflushjob() { records.deletecontents(); }
};

static vector<worlddiffflushjob *> worlddiffflushjobs;
static SDL_mutex *worlddiffwritermutex = NULL;
static SDL_cond *worlddiffwritercond = NULL;
static SDL_Thread *worlddiffwriterthread = NULL;
static bool stopworlddiffwriter = false;

static bool appendworlddiffbytes(const char *filename, const vector<uchar> &bytes)
{
    stream *file = openrawfile(filename, "ab");
    if(!file) return false;
    bool written = file->write(bytes.getbuf(), bytes.length()) == size_t(bytes.length());
    delete file;
    return written;
}

static int worlddiffwriter(void *)
{
#ifdef TRACY_ENABLE
    tracy::SetThreadName("World diff writer");
#endif
    for(;;)
    {
        SDL_LockMutex(worlddiffwritermutex);
        while(worlddiffflushjobs.empty() && !stopworlddiffwriter)
            SDL_CondWait(worlddiffwritercond, worlddiffwritermutex);
        if(worlddiffflushjobs.empty() && stopworlddiffwriter)
        {
            SDL_UnlockMutex(worlddiffwritermutex);
            break;
        }
        worlddiffflushjob *job = worlddiffflushjobs.remove(0);
        SDL_UnlockMutex(worlddiffwritermutex);

        vector<uchar> frame;
        makeworlddiffframe(frame, 2, job->chunkx, job->chunky, job->records);
        if(!appendworlddiffbytes(job->filename, frame))
            conoutf(CON_ERROR, "could not append chunk diff journal %s", job->filename);
        if(!appendworlddiffbytes(job->auditfilename, frame))
            conoutf(CON_ERROR, "could not append world audit journal %s", job->auditfilename);
        delete job;
    }
    return 0;
}

static bool startworlddiffwriter()
{
    if(worlddiffwriterthread) return true;
    worlddiffwritermutex = SDL_CreateMutex();
    worlddiffwritercond = SDL_CreateCond();
    stopworlddiffwriter = false;
    if(!worlddiffwritermutex || !worlddiffwritercond) return false;
    worlddiffwriterthread = SDL_CreateThread(worlddiffwriter, "world diff writer", NULL);
    return worlddiffwriterthread != NULL;
}

static void queueworlddiffflush(worldchunkdiffstate &state, vector<worldeditrecord *> &records)
{
    if(records.empty() || !startworlddiffwriter()) return;
    worlddiffflushjob *job = new worlddiffflushjob;
    defformatstring(relative, "media/map/%s/chunks/%d_%d_%d.diff",
                    worldfolder, state.x, state.y, state.z);
    copystring(job->filename, path(relative));
    defformatstring(auditrelative, "media/map/%s/audit.log", worldfolder);
    copystring(job->auditfilename, path(auditrelative));
    job->chunkx = state.x;
    job->chunky = state.y;
    job->records.move(records);

    SDL_LockMutex(worlddiffwritermutex);
    worlddiffflushjobs.add(job);
    SDL_CondSignal(worlddiffwritercond);
    SDL_UnlockMutex(worlddiffwritermutex);
}

static void flushworlddiffjournals(bool force)
{
    if(worldfolder[0] == '\0') return;
    if(!force && totalmillis - lastworlddiffflush < WORLD_DIFF_FLUSH_MILLIS) return;
    lastworlddiffflush = totalmillis;
    loopv(worldchunkdiffstates)
    {
        worldchunkdiffstate &state = *worldchunkdiffstates[i];
        if(state.pending.empty()) continue;
        vector<worldeditrecord *> flush;
        if(worlddiffwritermutex) SDL_LockMutex(worlddiffwritermutex);
        flush.move(state.pending);
        if(worlddiffwritermutex) SDL_UnlockMutex(worlddiffwritermutex);
        queueworlddiffflush(state, flush);
        flush.deletecontents();
    }
}

static void shutdownworlddiffwriter()
{
    if(!worlddiffwriterthread) return;
    SDL_LockMutex(worlddiffwritermutex);
    stopworlddiffwriter = true;
    SDL_CondBroadcast(worlddiffwritercond);
    SDL_UnlockMutex(worlddiffwritermutex);
    SDL_WaitThread(worlddiffwriterthread, NULL);
    worlddiffwriterthread = NULL;
    worlddiffflushjobs.deletecontents();
    SDL_DestroyCond(worlddiffwritercond);
    SDL_DestroyMutex(worlddiffwritermutex);
    worlddiffwritercond = NULL;
    worlddiffwritermutex = NULL;
    stopworlddiffwriter = false;
}

static cube *newpreparedfamily(int &families)
{
    cube *c = new cube[8];
    loopi(8) resetworldcube(c[i]);
    families++;
    return c;
}

struct worldchunkreader
{
    const uchar *pos, *end;

    worldchunkreader(const uchar *data, size_t length) : pos(data), end(data + length) {}

    bool readbyte(uchar &value)
    {
        if(pos >= end) return false;
        value = *pos++;
        return true;
    }

    bool readushort(ushort &value)
    {
        if(end - pos < 2) return false;
        value = ushort(pos[0] | (uint(pos[1]) << 8));
        pos += 2;
        return true;
    }

    bool read(void *dst, size_t length)
    {
        if(size_t(end - pos) < length) return false;
        memcpy(dst, pos, length);
        pos += length;
        return true;
    }

    bool readuint(uint &value)
    {
        if(end - pos < 4) return false;
        value = uint(pos[0]) | (uint(pos[1]) << 8) | (uint(pos[2]) << 16) |
                (uint(pos[3]) << 24);
        pos += 4;
        return true;
    }

    bool readullong(ullong &value)
    {
        if(end - pos < 8) return false;
        value = 0;
        loopi(8) value |= ullong(pos[i]) << (i * 8);
        pos += 8;
        return true;
    }

    int remaining() const { return int(end - pos); }
};

static bool deserializeworldblockpalette(worldchunkreader &reader, worldblockpalette &palette)
{
    uint count;
    if(!reader.readuint(count) || count > 0xFFFF || count > uint(reader.remaining() / 8)) return false;
    loopi(count)
    {
        ullong persistentid;
        if(!reader.readullong(persistentid)) return false;
        palette.blocks.add(getworldcubepersistentindex(persistentid));
    }
    return true;
}

static void subdivideworlddiffcube(cube &c, bool prepared, int &families)
{
    if(c.children) return;
    cube parent = c;
    c.children = prepared ? newpreparedfamily(families) : newcubes(F_EMPTY);
    loopi(8)
    {
        cube &child = c.children[i];
        memcpy(child.edges, parent.edges, sizeof(child.edges));
        memcpy(child.texture, parent.texture, sizeof(child.texture));
        child.material = parent.material;
        child.visible = child.merged = 0;
        child.ext = NULL;
        child.children = NULL;
    }
}

static void applyworlddiffnode(cube *root, const worlddiffnode &node,
                               bool prepared, int &families)
{
    if(!root || node.size <= 0 || (node.size & (node.size - 1)) ||
       node.x < 0 || node.y < 0 || node.z < 0 ||
       node.x + node.size > WORLD_CHUNK_MAP_SIZE ||
       node.y + node.size > WORLD_CHUNK_MAP_SIZE ||
       node.z + node.size > WORLD_CHUNK_MAP_SIZE)
        return;

    ivec pos(node.x, node.y, node.z);
    int scale = WORLD_CHUNK_SCALE - 1;
    cube *c = &root[octastep(pos.x, pos.y, pos.z, scale)];
    while((1 << scale) > node.size)
    {
        subdivideworlddiffcube(*c, prepared, families);
        --scale;
        c = &c->children[octastep(pos.x, pos.y, pos.z, scale)];
    }
    if(c->children)
    {
        if(prepared) game::freeworldchunk(c->children);
        else discardchildren(*c);
        c->children = NULL;
    }
    memcpy(c->edges, node.edges, sizeof(node.edges));
    memcpy(c->texture, node.texture, sizeof(node.texture));
    c->material = node.material;
    c->visible = c->merged = 0;
    c->ext = NULL;
}

static bool deserializeworlddiffnode(worldchunkreader &reader, worlddiffnode &node, const worldblockpalette &palette)
{
    uint value;
    if(!reader.readuint(value)) return false;
    node.x = int(value);
    if(!reader.readuint(value)) return false;
    node.y = int(value);
    if(!reader.readuint(value)) return false;
    node.z = int(value);
    if(!reader.readuint(value)) return false;
    node.size = int(value);
    if(!reader.read(node.edges, sizeof(node.edges))) return false;
    ushort paletteindex;
    if(!reader.readushort(paletteindex)) return false;
    if(paletteindex == 0xFFFF)
    {
        node.block = -1;
        loopi(6) if(!reader.readushort(node.texture[i])) return false;
    }
    else
    {
        if(!palette.blocks.inrange(paletteindex)) return false;
        node.block = palette.blocks[paletteindex];
        const worlddefinition &definition = *worldcubedefinitions[validworldcubeindex(node.block)];
        loopi(6) node.texture[i] = definition.sideslot;
        node.texture[O_TOP] = definition.slot;
        node.texture[O_BOTTOM] = definition.bottomslot;
    }
    return reader.readushort(node.material);
}

static bool deserializeworldscatterinstance(worldchunkreader &reader,
                                            worldscatterinstance &scatter)
{
    uint value;
    ullong persistentid;
    if(!reader.readuint(value)) return false;
    scatter.x = int(value);
    if(!reader.readuint(value)) return false;
    scatter.y = int(value);
    if(!reader.readuint(value)) return false;
    scatter.z = int(value);
    if(!reader.readullong(persistentid) || !reader.readuint(value)) return false;
    scatter.type = getworldscatterpersistentindex(persistentid);
    scatter.orient = int(value);
    scatter.rendertransformvalid = false;
    return scatter.x >= 0 && scatter.x < WORLD_CHUNK_SIZE &&
           scatter.y >= 0 && scatter.y < WORLD_CHUNK_SIZE &&
           scatter.z >= 0 &&
           scatter.z + WORLD_BLOCK_SIZE <= WORLD_MAP_SIZE &&
           scatter.type >= 0 && scatter.type < numworldscatters() &&
           scatter.orient >= O_LEFT && scatter.orient <= O_TOP;
}

static bool deserializeworldeditrecord(worldchunkreader &reader, worldeditrecord &record, const worldblockpalette &palette)
{
    uint length, checksum;
    if(!reader.readuint(length) || !reader.readuint(checksum) ||
       length > uint(reader.remaining()))
        return false;
    const uchar *recordbytes = reader.pos;
    worldchunkreader body(recordbytes, length);
    reader.pos += length;
    if(worlddiffchecksum(recordbytes, length) != checksum) return false;

    uint value, count;
    if(!body.readuint(value)) return false;
    record.chunkx = int(value);
    if(!body.readuint(value)) return false;
    record.chunky = int(value);
    if(!body.readuint(value)) return false;
    record.chunkz = int(value);
    if(!body.readullong(record.revision) || !body.readullong(record.timestamp)) return false;
    if(!body.readuint(value)) return false;
    record.author = int(value);
    if(!body.readuint(value)) return false;
    record.operation = int(value);
    if(!body.readuint(value)) return false;
    record.selection.o.x = int(value);
    if(!body.readuint(value)) return false;
    record.selection.o.y = int(value);
    if(!body.readuint(value)) return false;
    record.selection.o.z = int(value);
    if(!body.readuint(value)) return false;
    record.selection.s.x = int(value);
    if(!body.readuint(value)) return false;
    record.selection.s.y = int(value);
    if(!body.readuint(value)) return false;
    record.selection.s.z = int(value);
    if(!body.readuint(value)) return false;
    record.selection.grid = int(value);
    if(!body.readuint(value)) return false;
    record.selection.orient = int(value);
    if(!body.readuint(value)) return false;
    record.selection.corner = int(value);
    loopi(4)
    {
        if(!body.readuint(value)) return false;
        record.args[i] = int(value);
    }
    if(!body.readuint(count) || count > uint(body.remaining() / 32)) return false;
    loopi(count) if(!deserializeworlddiffnode(body, record.before.add(), palette)) return false;
    if(!body.readuint(count) || count > uint(body.remaining() / 32)) return false;
    loopi(count) if(!deserializeworlddiffnode(body, record.after.add(), palette)) return false;
    if(!body.readuint(count) || count > uint(body.remaining() / 24)) return false;
    loopi(count)
        if(!deserializeworldscatterinstance(body, record.scatterbefore.add()))
            return false;
    if(!body.readuint(count) || count > uint(body.remaining() / 24)) return false;
    loopi(count)
        if(!deserializeworldscatterinstance(body, record.scatterafter.add()))
            return false;
    return body.remaining() == 0;
}

static void applyworldscatterchange(vector<worldscatterinstance> &scatter,
                                    const vector<worldscatterinstance> &before,
                                    const vector<worldscatterinstance> &after)
{
    loopv(before)
    {
        int index = scatter.find(before[i]);
        if(index >= 0) scatter.removeunordered(index);
    }
    loopv(after) if(scatter.find(after[i]) < 0) scatter.add(after[i]);
}

static ullong hashworlddiffbytes(ullong hash, const void *data, int length)
{
    const uchar *bytes = (const uchar *)data;
    loopi(length)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static ullong hashworlddiff64(ullong hash, ullong value)
{
    loopi(8)
    {
        const uchar byte = uchar(value >> (i * 8));
        hash = hashworlddiffbytes(hash, &byte, 1);
    }
    return hash;
}

static ullong hashworlddiffcube(const cube &c, ullong hash)
{
    uchar children = c.children ? 1 : 0;
    hash = hashworlddiffbytes(hash, &children, sizeof(children));
    if(c.children)
    {
        loopi(8) hash = hashworlddiffcube(c.children[i], hash);
        return hash;
    }
    hash = hashworlddiffbytes(hash, c.edges, sizeof(c.edges));
    const int block = getworldcubebytextures(c.texture);
    const uchar rawtextures = block < 0 ? 1 : 0;
    hash = hashworlddiffbytes(hash, &rawtextures, sizeof(rawtextures));
    if(block >= 0) hash = hashworlddiff64(hash, getworldcubepersistentid(block));
    else hash = hashworlddiffbytes(hash, c.texture, sizeof(c.texture));
    return hashworlddiffbytes(hash, &c.material, sizeof(c.material));
}

static ullong hashworldchunk(cube *root)
{
    ullong hash = 1469598103934665603ULL;
    loopi(8) hash = hashworlddiffcube(root[i], hash);
    return hash;
}

static bool sameworldcubeleaf(const cube &a, const cube &b)
{
    return !a.children && !b.children && a.material == b.material &&
           !memcmp(a.edges, b.edges, sizeof(a.edges)) &&
           !memcmp(a.texture, b.texture, sizeof(a.texture));
}

static bool worldsubtreematchesleaf(const cube &tree, const cube &leaf)
{
    if(!tree.children) return sameworldcubeleaf(tree, leaf);
    loopi(8) if(!worldsubtreematchesleaf(tree.children[i], leaf)) return false;
    return true;
}

static void collectworldchunkoverrides(const cube &current, const cube &base,
                                       const ivec &o, int size,
                                       vector<worlddiffnode> &overrides)
{
    if(!current.children)
    {
        if((!base.children && sameworldcubeleaf(current, base)) ||
           (base.children && worldsubtreematchesleaf(base, current)))
            return;
        copyworlddiffnode(current, o, size, ivec(0, 0, 0), overrides.add());
        return;
    }
    if(!base.children)
    {
        if(worldsubtreematchesleaf(current, base)) return;
        int childsize = size >> 1;
        loopi(8)
            collectworldchunkoverrides(current.children[i], base,
                                       ivec(i, o, childsize), childsize, overrides);
        return;
    }
    int childsize = size >> 1;
    loopi(8)
        collectworldchunkoverrides(current.children[i], base.children[i],
                                   ivec(i, o, childsize), childsize, overrides);
}

static bool applyworldchunkdiff(cube *root, int x, int y, const char *filename,
                                vector<worldscatterinstance> &scatter,
                                bool prepared, int &families,
                                ullong &revision, ullong &canonicalhash, worldchunkdirtybounds *dirty)
{
    ZoneScopedN("Chunks/Apply diff");
    revision = canonicalhash = 0;
    if(!filename || !*filename)
    {
        canonicalhash = hashworldchunk(root);
        return true;
    }
    stream *file = openrawfile(filename, "rb");
    if(!file)
    {
        canonicalhash = hashworldchunk(root);
        conoutf(CON_WARN, "could not open chunk diff %s", filename);
        return false;
    }
    stream::offset filelength = file->size();
    if(filelength <= 0 || filelength > INT_MAX)
    {
        delete file;
        canonicalhash = hashworldchunk(root);
        return false;
    }
    vector<uchar> contents;
    uchar *dst = contents.pad(int(filelength));
    bool readok = file->read(dst, size_t(filelength)) == size_t(filelength);
    delete file;
    if(!readok)
    {
        canonicalhash = hashworldchunk(root);
        return false;
    }

    worldchunkreader reader(contents.getbuf(), contents.length());
    bool valid = true;
    ullong expectedhash = 0;
    while(reader.remaining() >= 12)
    {
        char magic[4];
        uint length, checksum;
        if(!reader.read(magic, sizeof(magic)) || memcmp(magic, "CDF1", 4) ||
           !reader.readuint(length) || !reader.readuint(checksum) ||
           length > WORLD_DIFF_FRAME_MAX || length > uint(reader.remaining()))
        {
            valid = false;
            break;
        }
        const uchar *payloadbytes = reader.pos;
        reader.pos += length;
        if(worlddiffchecksum(payloadbytes, length) != checksum)
        {
            valid = false;
            continue;
        }
        worldchunkreader payload(payloadbytes, length);
        uchar type;
        uint saveversion = 0, genversion, chunkx, chunky, chunkz, count;
        ullong framehash;
        worldblockpalette palette;
        if(!payload.readbyte(type) || !payload.readuint(saveversion) ||
           !payload.readuint(genversion) || !payload.readuint(chunkx) ||
           !payload.readuint(chunky) || !payload.readuint(chunkz) || !payload.readullong(framehash))
        {
            valid = false;
            continue;
        }
        if(saveversion != WORLD_SAVE_FORMAT_VERSION)
        {
            conoutf(CON_ERROR, "chunk diff %s uses unsupported save format version %u", filename, saveversion);
            valid = false;
            continue;
        }
        if(!deserializeworldblockpalette(payload, palette) || !payload.readuint(count) ||
           genversion != WORLDGEN_VERSION || int(chunkx) != x || int(chunky) != y ||
           int(chunkz) != WORLD_DIFF_Z || (type != 1 && type != 2) ||
           count > 1000000U)
        {
            valid = false;
            continue;
        }
        if(framehash) expectedhash = framehash;
        else if(type == 2 && count) expectedhash = 0;
        loopi(count)
        {
            worldeditrecord record;
            if(!deserializeworldeditrecord(payload, record, palette) ||
               record.chunkx != x || record.chunky != y ||
               record.chunkz != WORLD_DIFF_Z || record.revision <= revision)
            {
                valid = false;
                break;
            }
            loopv(record.after)
            {
                const worlddiffnode &node = record.after[i];
                applyworlddiffnode(root, node, prepared, families);
                if(dirty && node.size > 0 && node.size <= WORLD_CHUNK_MAP_SIZE && !(node.size & (node.size - 1)) && node.x >= 0 && node.y >= 0 &&
                   node.z >= 0 && node.x <= WORLD_CHUNK_MAP_SIZE - node.size && node.y <= WORLD_CHUNK_MAP_SIZE - node.size &&
                   node.z <= WORLD_CHUNK_MAP_SIZE - node.size)
                    dirty->include(node);
            }
            applyworldscatterchange(scatter, record.scatterbefore,
                                    record.scatterafter);
            revision = record.revision;
        }
        if(payload.remaining()) valid = false;
    }
    if(reader.remaining()) valid = false;
    for(int i = scatter.length() - 1; i >= 0; --i)
        if(!game::validgeneratedworldscatter(root, scatter[i]))
            scatter.removeunordered(i);
    game::cacheworldscattertransforms(x, y, game::getworldscattermaxoffset(), scatter);
    if(dirty) dirty->expandforrebuild();
    canonicalhash = hashworldchunk(root);
    if(expectedhash && canonicalhash != expectedhash)
    {
        valid = false;
        conoutf(CON_ERROR, "chunk diff %s reconstructed hash " WORLD_ULL_FORMAT " but expected " WORLD_ULL_FORMAT, filename, canonicalhash,
                expectedhash);
    }
    if(!valid)
        conoutf(CON_WARN, "chunk diff %s has an incomplete or corrupt frame; valid revisions were recovered", filename);
    return valid;
}

static bool compactworldchunkdiff(worldchunk &chunk)
{
    if(!chunk.root || chunk.loading || chunk.corrupted) return false;
    flushworlddiffjournals(true);
    shutdownworlddiffwriter();
    if(worldchunkmounted(chunk) && !syncmountedworldchunk(chunk)) return false;

    string cachefilename;
    worldchunkcachefilename(cachefilename, sizeof(cachefilename), worldfolder, chunk.x, chunk.y);
    vector<worldscatterinstance> basescatter;
    worldsectionrenderdata baserenderdata;
    int cachefamilies = 0, cacheerror = 0;
    cube *base = generatedchunkcache ? loadworldchunkcache(cachefilename, chunk.x, chunk.y, game::getworldseed(),
                                                           game::worldgenerationparameterhash(), chunkremip != 0, basescatter, false,
                                                           cachefamilies, cacheerror, &baserenderdata) : NULL;
    if(!base)
    {
        ZoneScopedN("Chunks/Generate uncached");
        base = game::generateworldchunk(chunk.x, chunk.y, &baserenderdata);
        if(base) game::generateworldscatter(base, chunk.x, chunk.y, basescatter);
        vector<uchar> cachepayload;
        if(generatedchunkcache && base && serializeworldchunkcache(base, basescatter, cachepayload, &baserenderdata))
            queueworldchunkcachewrite(chunk.x, chunk.y, game::getworldseed(), game::worldgenerationparameterhash(), chunkremip != 0,
                                      cachepayload);
    }
    if(!base) return false;
    setworldleavesalpha(base, leavesalpha != 0);
    cube *savedroot = copyworldchunkforsave(chunk);
    int families = 0;
    if(chunkremip)
    {
        remipworldchunk(savedroot, false, families);
        remipworldchunk(base, false, families);
    }

    vector<worlddiffnode> overrides;
    loopi(8)
        collectworldchunkoverrides(savedroot[i], base[i],
                                   ivec(i, ivec(0, 0, 0), WORLD_CHUNK_ROOT_SIZE),
                                   WORLD_CHUNK_ROOT_SIZE, overrides);
    ullong finalhash = hashworldchunk(savedroot);
    freeocta(savedroot);
    freeocta(base);

    vector<worldscatterinstance> scatterremoved, scatteradded;
    loopv(basescatter) if(chunk.scatter.find(basescatter[i]) < 0)
        scatterremoved.add(basescatter[i]);
    loopv(chunk.scatter) if(basescatter.find(chunk.scatter[i]) < 0)
        scatteradded.add(chunk.scatter[i]);

    defformatstring(relative, "media/map/%s/chunks/%d_%d_%d.diff",
                    worldfolder, chunk.x, chunk.y, WORLD_DIFF_Z);
    path(relative);
    string finalpath;
    copystring(finalpath, findfile(relative, "wb"));
    worldchunkdiffstate *state = findworldchunkdiffstate(chunk.x, chunk.y, true);
    if(overrides.empty() && scatterremoved.empty() && scatteradded.empty())
    {
        remove(finalpath);
        state->journal.deletecontents();
        state->snapshotrevision = state->revision;
        state->canonicalhash = finalhash;
        chunk.saved = true;
        chunk.dirty = false;
        conoutf("chunk %d %d matches its generated base; removed its override",
                chunk.x, chunk.y);
        return true;
    }

    worldeditrecord snapshot;
    snapshot.chunkx = chunk.x;
    snapshot.chunky = chunk.y;
    snapshot.operation = WORLD_EDIT_SET_CUBE;
    snapshot.author = -1;
    snapshot.revision = state->revision;
    snapshot.timestamp = ullong(time(NULL));
    snapshot.after.move(overrides);
    snapshot.scatterbefore.move(scatterremoved);
    snapshot.scatterafter.move(scatteradded);
    vector<worldeditrecord *> records;
    records.add(&snapshot);
    vector<uchar> frame;
    makeworlddiffframe(frame, 1, chunk.x, chunk.y, records, finalhash);

    defformatstring(temprelative, "%s.tmp", relative);
    string temppath;
    copystring(temppath, findfile(temprelative, "wb"));
    stream *file = openrawfile(temprelative, "wb");
    bool written = file && file->write(frame.getbuf(), frame.length()) == size_t(frame.length());
    delete file;
    if(!written)
    {
        remove(temppath);
        conoutf(CON_ERROR, "could not write compacted chunk diff %s", temppath);
        return false;
    }
    if(rename(temppath, finalpath))
    {
        remove(finalpath);
        if(rename(temppath, finalpath))
        {
            remove(temppath);
            conoutf(CON_ERROR, "could not atomically publish compacted chunk diff %s", finalpath);
            return false;
        }
    }
    state->journal.deletecontents();
    state->snapshotrevision = state->revision;
    state->canonicalhash = finalhash;
    chunk.saved = true;
    chunk.dirty = false;
    conoutf("compacted chunk %d %d to %d sparse overrides (%d bytes)",
            chunk.x, chunk.y, snapshot.after.length(), frame.length());
    return true;
}

static void loadworldauditlog()
{
    defformatstring(relative, "media/map/%s/audit.log", worldfolder);
    stream *file = openfile(path(relative), "rb");
    if(!file) return;
    stream::offset length = file->size();
    if(length <= 0 || length > INT_MAX)
    {
        delete file;
        return;
    }
    vector<uchar> contents;
    uchar *bytes = contents.pad(int(length));
    bool readok = file->read(bytes, size_t(length)) == size_t(length);
    delete file;
    if(!readok) return;

    worldchunkreader reader(contents.getbuf(), contents.length());
    while(reader.remaining() >= 12)
    {
        char magic[4];
        uint framelength, framechecksum;
        if(!reader.read(magic, sizeof(magic)) || memcmp(magic, "CDF1", 4) ||
           !reader.readuint(framelength) || !reader.readuint(framechecksum) ||
           framelength > WORLD_DIFF_FRAME_MAX || framelength > uint(reader.remaining()))
            break;
        const uchar *payloadbytes = reader.pos;
        reader.pos += framelength;
        if(worlddiffchecksum(payloadbytes, framelength) != framechecksum) continue;
        worldchunkreader payload(payloadbytes, framelength);
        uchar type;
        uint saveversion = 0, genversion, chunkx, chunky, chunkz, count;
        ullong ignoredhash;
        worldblockpalette palette;
        if(!payload.readbyte(type) || !payload.readuint(saveversion) ||
           !payload.readuint(genversion) || !payload.readuint(chunkx) ||
           !payload.readuint(chunky) || !payload.readuint(chunkz) || !payload.readullong(ignoredhash))
            continue;
        if(saveversion != WORLD_SAVE_FORMAT_VERSION)
        {
            conoutf(CON_ERROR, "world audit uses unsupported save format version %u", saveversion);
            continue;
        }
        if(!deserializeworldblockpalette(payload, palette) || !payload.readuint(count) || type != 2 ||
           genversion != WORLDGEN_VERSION || int(chunkz) != WORLD_DIFF_Z || count > 1000000U)
            continue;
        worldchunkdiffstate *state =
            findworldchunkdiffstate(int(chunkx), int(chunky), true);
        loopi(count)
        {
            worldeditrecord *record = new worldeditrecord;
            if(!deserializeworldeditrecord(payload, *record, palette))
            {
                delete record;
                break;
            }
            state->audit.add(record);
            state->revision = max(state->revision, record->revision);
            worldeditrevision = max(worldeditrevision, record->revision);
        }
    }
}

static void worlddiffcommand(char *action, char *xtext, char *ytext, char *ztext)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worlddiff %s %s %s %s",
                        action ? action : "", xtext ? xtext : "",
                        ytext ? ytext : "", ztext ? ztext : "");
        game::requestworldcommand(command);
        return;
    }
    if(!action || !action[0])
    {
        conoutf(CON_ERROR, "usage: /worlddiff <stats|compact|verify> [x y z|all]");
        return;
    }
    bool all = xtext && !strcmp(xtext, "all");
    int x = xtext && xtext[0] && !all ? atoi(xtext) : lastplayerchunkx,
        y = ytext && ytext[0] ? atoi(ytext) : lastplayerchunky,
        z = ztext && ztext[0] ? atoi(ztext) : WORLD_DIFF_Z;
    if(z != WORLD_DIFF_Z)
    {
        conoutf(CON_ERROR, "this world stores its full vertical band as chunk z=0");
        return;
    }
    if(!strcmp(action, "stats"))
    {
        worldchunkdiffstate *state = findworldchunkdiffstate(x, y);
        if(!state)
        {
            conoutf("chunk %d %d %d: generated base only, revision 0, zero disk override", x, y, z);
            return;
        }
        conoutf("chunk %d %d %d: revision " WORLD_ULL_FORMAT ", snapshot "
                WORLD_ULL_FORMAT ", %d pending, %d journal, %d audit, hash "
                WORLD_ULL_FORMAT,
                x, y, z, state->revision, state->snapshotrevision,
                state->pending.length(), state->journal.length(), state->audit.length(),
                state->canonicalhash);
        return;
    }
    if(!strcmp(action, "compact"))
    {
        int compacted = 0;
        loopv(worldchunks)
        {
            worldchunk &chunk = worldchunks[i];
            if((all || (chunk.x == x && chunk.y == y)) && compactworldchunkdiff(chunk))
                compacted++;
        }
        conoutf("worlddiff compact: %d chunk%s", compacted, compacted == 1 ? "" : "s");
        return;
    }
    if(!strcmp(action, "verify"))
    {
        int verified = 0, failed = 0;
        flushworlddiffjournals(true);
        shutdownworlddiffwriter();
        loopv(worldchunks)
        {
            worldchunk &chunk = worldchunks[i];
            if(!all && (chunk.x != x || chunk.y != y)) continue;
            if(worldchunkmounted(chunk) && !syncmountedworldchunk(chunk))
            {
                failed++;
                continue;
            }
            ullong livehash = hashworldchunk(chunk.root);
            string cachefilename;
            worldchunkcachefilename(cachefilename, sizeof(cachefilename), worldfolder, chunk.x, chunk.y);
            int cachefamilies = 0, cacheerror = 0;
            vector<worldscatterinstance> reconstructedscatter;
            worldsectionrenderdata reconstructedrenderdata;
            cube *reconstructed = generatedchunkcache ? loadworldchunkcache(cachefilename, chunk.x, chunk.y, game::getworldseed(),
                                                                            game::worldgenerationparameterhash(), chunkremip != 0,
                                                                            reconstructedscatter, false, cachefamilies, cacheerror,
                                                                            &reconstructedrenderdata) : NULL;
            if(!reconstructed)
            {
                ZoneScopedN("Chunks/Generate uncached");
                reconstructed = game::generateworldchunk(chunk.x, chunk.y, &reconstructedrenderdata);
                if(reconstructed) game::generateworldscatter(reconstructed, chunk.x, chunk.y, reconstructedscatter);
                vector<uchar> cachepayload;
                if(generatedchunkcache && reconstructed && serializeworldchunkcache(reconstructed, reconstructedscatter, cachepayload,
                                                                                     &reconstructedrenderdata))
                    queueworldchunkcachewrite(chunk.x, chunk.y, game::getworldseed(), game::worldgenerationparameterhash(), chunkremip != 0,
                                              cachepayload);
            }
            if(!reconstructed)
            {
                failed++;
                continue;
            }
            setworldleavesalpha(reconstructed, leavesalpha != 0);
            defformatstring(relative, "media/map/%s/chunks/%d_%d_%d.diff",
                            worldfolder, chunk.x, chunk.y, WORLD_DIFF_Z);
            path(relative);
            const char *found = findfile(relative, "rb");
            string filename;
            filename[0] = '\0';
            if(found && fileexists(found, "r")) copystring(filename, relative);
            int families = 0;
            ullong revision = 0, reconstructedhash = 0;
            worldchunkdirtybounds dirty;
            bool valid = applyworldchunkdiff(reconstructed, chunk.x, chunk.y,
                                             filename, reconstructedscatter,
                                             false, families,
                                             revision, reconstructedhash, &dirty);
            if(chunkremip && dirty.valid) remipworldchunkbounded(reconstructed, false, families, NULL, &dirty);
            reconstructedhash = hashworldchunk(reconstructed);
            freeocta(reconstructed);
            if(!valid || livehash != reconstructedhash ||
               !sameworldscatterlist(chunk.scatter, reconstructedscatter))
                failed++;
            else
            {
                worldchunkdiffstate *state =
                    findworldchunkdiffstate(chunk.x, chunk.y, true);
                state->revision = max(state->revision, revision);
                worldeditrevision = max(worldeditrevision, revision);
                state->canonicalhash = livehash;
                verified++;
            }
        }
        conoutf("worlddiff verify: %d verified, %d mismatched", verified, failed);
        return;
    }
    conoutf(CON_ERROR, "unknown worlddiff action %s", action);
}

COMMANDN(worlddiff, worlddiffcommand, "ssss");


#endif
