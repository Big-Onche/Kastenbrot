// worldedit.cpp: streamed-world edit capture, undo, redo, and rollback

#ifdef WORLDIO_MODULE_IMPLEMENTATION

struct worldeditcapture
{
    bool active;
    int operation, author, args[4];
    selinfo selection;
    vector<worldeditrecord *> records;

    worldeditcapture() : active(false), operation(0), author(-1)
    {
        memset(args, 0, sizeof(args));
    }

    void clear()
    {
        records.deletecontents();
        active = false;
    }
};

static worldeditcapture currentworldedit;

static bool sameworlddiffnode(const worlddiffnode &a, const worlddiffnode &b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.size == b.size &&
           a.material == b.material && !memcmp(a.edges, b.edges, sizeof(a.edges)) &&
           !memcmp(a.texture, b.texture, sizeof(a.texture));
}

static bool sameworldscatterlist(const vector<worldscatterinstance> &a, const vector<worldscatterinstance> &b)
{
    if(a.length() != b.length()) return false;
    loopv(a)
    {
        bool found = false;
        loopvj(b) if(a[i] == b[j]) { found = true; break; }
        if(!found) return false;
    }
    return true;
}

static int getworldcubebytextures(const ushort *textures)
{
    loopi(4) if(textures[i] != textures[0]) return -1;
    int *index = worldcubetextureindexes.access(worldcubetexturekey(textures[O_TOP], textures[0], textures[O_BOTTOM]));
    return index ? *index : -1;
}

static void copyworlddiffnode(const cube &c, const ivec &o, int size, const ivec &chunkorigin, worlddiffnode &node)
{
    node.x = o.x - chunkorigin.x;
    node.y = o.y - chunkorigin.y;
    node.z = o.z;
    node.size = size;
    memcpy(node.edges, c.edges, sizeof(node.edges));
    memcpy(node.texture, c.texture, sizeof(node.texture));
    node.material = c.material;
    node.block = getworldcubebytextures(c.texture);
}

static void captureworlddiffnodes(const cube &c, const ivec &o, int size, const ivec &bbmin, const ivec &bbmax, const ivec &chunkorigin,
                                  vector<worlddiffnode> &nodes)
{
    ivec nodeend = ivec(o).add(size);
    if(nodeend.x <= bbmin.x || nodeend.y <= bbmin.y || nodeend.z <= bbmin.z ||
       o.x >= bbmax.x || o.y >= bbmax.y || o.z >= bbmax.z)
        return;

    bool contained = o.x >= bbmin.x && o.y >= bbmin.y && o.z >= bbmin.z &&
                     nodeend.x <= bbmax.x && nodeend.y <= bbmax.y && nodeend.z <= bbmax.z;
    if(contained && !c.children)
    {
        copyworlddiffnode(c, o, size, chunkorigin, nodes.add());
        return;
    }
    if(size <= 1)
    {
        copyworlddiffnode(c, o, size, chunkorigin, nodes.add());
        return;
    }

    int childsize = size >> 1;
    loopi(8)
    {
        ivec co(i, o, childsize);
        captureworlddiffnodes(c.children ? c.children[i] : c, co, childsize,
                              bbmin, bbmax, chunkorigin, nodes);
    }
}

static void captureworlddiffregion(const worldchunk &chunk, const ivec &bbmin, const ivec &bbmax, vector<worlddiffnode> &nodes)
{
    if(!worldroot || !worldchunkmounted(chunk)) return;
    ivec chunkorigin = worldchunkorigin(chunk),
         clipmin(max(bbmin.x, chunkorigin.x), max(bbmin.y, chunkorigin.y),
                 max(bbmin.z, 0)),
         clipmax(min(bbmax.x, chunkorigin.x + WORLD_CHUNK_SIZE),
                 min(bbmax.y, chunkorigin.y + WORLD_CHUNK_SIZE),
                 min(bbmax.z, int(WORLD_MAP_SIZE)));
    if(clipmin.x >= clipmax.x || clipmin.y >= clipmax.y || clipmin.z >= clipmax.z) return;
    int rootsize = worldsize >> 1;
    loopi(8)
        captureworlddiffnodes(worldroot[i], ivec(i, ivec(0, 0, 0), rootsize), rootsize, clipmin, clipmax, chunkorigin, nodes);
}

static bool worldscatterinregion(const worldscatterinstance &scatter, const ivec &chunkorigin, const ivec &bbmin, const ivec &bbmax)
{
    const ivec position = ivec(chunkorigin).add(
        ivec(scatter.x, scatter.y, scatter.z));
    return position.x >= bbmin.x && position.x < bbmax.x &&
           position.y >= bbmin.y && position.y < bbmax.y &&
           position.z >= bbmin.z && position.z < bbmax.z;
}

static void captureworldscatterregion(const worldchunk &chunk, const ivec &bbmin, const ivec &bbmax, vector<worldscatterinstance> &scatter)
{
    if(!worldchunkmounted(chunk)) return;
    const ivec origin = worldchunkorigin(chunk);
    loopv(chunk.scatter)
        if(worldscatterinregion(chunk.scatter[i], origin, bbmin, bbmax))
            scatter.add(chunk.scatter[i]);
}

static ivec worldorientnormal(int orient)
{
    ivec normal(0, 0, 0);
    if(orient >= O_LEFT && orient <= O_TOP)
        normal[dimension(orient)] = dimcoord(orient) ? 1 : -1;
    return normal;
}

static bool validworldscatter(const worldchunk &chunk, const worldscatterinstance &scatter)
{
    if(scatter.type < 0 || scatter.type >= numworldscatters() ||
       scatter.x < 0 || scatter.x >= WORLD_CHUNK_SIZE ||
       scatter.y < 0 || scatter.y >= WORLD_CHUNK_SIZE ||
       scatter.z < 0 || scatter.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE ||
       scatter.orient < O_LEFT || scatter.orient > O_TOP)
        return false;

    const ivec origin = worldchunkorigin(chunk);
    const ivec center = ivec(origin).add(ivec(
        scatter.x + WORLD_BLOCK_SIZE / 2,
        scatter.y + WORLD_BLOCK_SIZE / 2,
        scatter.z + WORLD_BLOCK_SIZE / 2));
    const ivec normal = worldorientnormal(scatter.orient),
               supportcenter = ivec(center).sub(
                   ivec(normal).mul(WORLD_BLOCK_SIZE));
    if(!insideworld(center) || !insideworld(supportcenter)) return false;
    ivec cubeorigin;
    int cubesize;
    const cube occupied = sampleworldblockcube(lookupcube(center, 0, cubeorigin, cubesize), center, cubeorigin, cubesize);
    if(!isempty(occupied) || occupied.material != MAT_AIR) return false;

    const bool placeable = isworldplaceable(scatter.type);
    if((!placeable && scatter.orient != O_TOP) ||
       (placeable && scatter.orient == O_BOTTOM))
        return false;
    const cube support = sampleworldblockcube(lookupcube(supportcenter, 0, cubeorigin, cubesize), supportcenter, cubeorigin, cubesize);
    if(isempty(support) || !isentirelysolid(support) ||
       support.material != MAT_AIR)
        return false;
    return true;
}

static void removeworldinvalidscatter(worldchunk &chunk, const ivec &bbmin, const ivec &bbmax)
{
    const ivec origin = worldchunkorigin(chunk);
    for(int i = chunk.scatter.length() - 1; i >= 0; --i)
        if(worldscatterinregion(chunk.scatter[i], origin, bbmin, bbmax) &&
           !validworldscatter(chunk, chunk.scatter[i]))
            chunk.scatter.removeunordered(i);
}

static worldeditrecord *cloneworldeditrecord(const worldeditrecord &source)
{
    worldeditrecord *copy = new worldeditrecord;
    copy->chunkx = source.chunkx;
    copy->chunky = source.chunky;
    copy->chunkz = source.chunkz;
    copy->operation = source.operation;
    copy->author = source.author;
    memcpy(copy->args, source.args, sizeof(copy->args));
    copy->revision = source.revision;
    copy->timestamp = source.timestamp;
    copy->selection = source.selection;
    loopv(source.before) copy->before.add(source.before[i]);
    loopv(source.after) copy->after.add(source.after[i]);
    loopv(source.scatterbefore) copy->scatterbefore.add(source.scatterbefore[i]);
    loopv(source.scatterafter) copy->scatterafter.add(source.scatterafter[i]);
    return copy;
}

void setworldeditauthor(int author)
{
    worldeditauthor = author;
}

void setworldeditrevision(uint revision)
{
    incomingworldeditrevision = revision;
}

void cancelworldedit()
{
    currentworldedit.clear();
}

void beginworldedit(int operation, const selinfo &selection, int arg1, int arg2, int arg3, int arg4)
{
    cancelworldedit();
    if(worldchunks.empty() || activeworldchunk < 0 || selection.s.iszero()) return;

    currentworldedit.active = true;
    currentworldedit.operation = operation;
    currentworldedit.author = worldeditauthor;
    currentworldedit.args[0] = arg1;
    currentworldedit.args[1] = arg2;
    currentworldedit.args[2] = arg3;
    currentworldedit.args[3] = arg4;
    currentworldedit.selection = selection;

    ivec bbmin = selection.o,
         bbmax = ivec(selection.s).mul(selection.grid).add(selection.o);
    const ivec scattermin = ivec(bbmin).sub(WORLD_BLOCK_SIZE),
               scattermax = ivec(bbmax).add(WORLD_BLOCK_SIZE);
    loopv(worldchunks)
    {
        worldchunk &chunk = worldchunks[i];
        if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) continue;
        ivec origin = worldchunkorigin(chunk);
        if(scattermax.x <= origin.x ||
           scattermin.x >= origin.x + WORLD_CHUNK_SIZE ||
           scattermax.y <= origin.y ||
           scattermin.y >= origin.y + WORLD_CHUNK_SIZE ||
           scattermax.z <= 0 || scattermin.z >= WORLD_MAP_SIZE)
            continue;

        worldeditrecord *record = currentworldedit.records.add(new worldeditrecord);
        record->chunkx = chunk.x;
        record->chunky = chunk.y;
        record->operation = operation;
        record->author = currentworldedit.author;
        memcpy(record->args, currentworldedit.args, sizeof(record->args));
        record->selection = selection;
        captureworlddiffregion(chunk, bbmin, bbmax, record->before);
        captureworldscatterregion(chunk, scattermin, scattermax, record->scatterbefore);
    }
}

void commitworldedit()
{
    if(!currentworldedit.active) return;
    ivec bbmin = currentworldedit.selection.o,
         bbmax = ivec(currentworldedit.selection.s).mul(currentworldedit.selection.grid) .add(currentworldedit.selection.o);
    const ivec scattermin = ivec(bbmin).sub(WORLD_BLOCK_SIZE),
               scattermax = ivec(bbmax).add(WORLD_BLOCK_SIZE);
    ullong timestamp = ullong(time(NULL));
    ullong revision = incomingworldeditrevision
                    ? max(worldeditrevision, incomingworldeditrevision)
                    : worldeditrevision + 1;
    worldeditrevision = revision;
    incomingworldeditrevision = 0;
    bool scatterchanged = false;
    loopv(currentworldedit.records)
    {
        worldeditrecord *record = currentworldedit.records[i];
        int chunkindex = findworldchunk(record->chunkx, record->chunky);
        if(!worldchunks.inrange(chunkindex)) continue;
        worldchunk &chunk = worldchunks[chunkindex];
        removeworldinvalidscatter(chunk, scattermin, scattermax);
        captureworlddiffregion(chunk, bbmin, bbmax, record->after);
        captureworldscatterregion(chunk, scattermin, scattermax,
                                  record->scatterafter);

        bool identical = record->before.length() == record->after.length();
        if(identical) loopvj(record->before)
            if(!sameworlddiffnode(record->before[j], record->after[j]))
            {
                identical = false;
                break;
            }
        const bool samescatter =
            sameworldscatterlist(record->scatterbefore, record->scatterafter);
        if(!samescatter) scatterchanged = true;
        if(identical) identical = samescatter;
        if(identical) continue;

        worldchunkdiffstate *state = findworldchunkdiffstate(record->chunkx, record->chunky, true);
        record->revision = revision;
        state->revision = max(state->revision, revision);
        record->timestamp = timestamp;
        state->pending.add(cloneworldeditrecord(*record));
        state->journal.add(cloneworldeditrecord(*record));
        state->audit.add(cloneworldeditrecord(*record));
        chunk.dirty = true;
    }
    currentworldedit.clear();
    if(scatterchanged) updateworldscatterers();
}

static void commitworldscatterrecord(worldchunk &chunk,
                                     const worldscatterinstance &scatter,
                                     const ivec &support, bool place)
{
    worldeditrecord record;
    record.chunkx = chunk.x;
    record.chunky = chunk.y;
    record.operation = place ? WORLD_EDIT_SET_SCATTER
                             : WORLD_EDIT_DELETE_SCATTER;
    record.author = worldeditauthor;
    record.revision = incomingworldeditrevision
                    ? max(worldeditrevision, incomingworldeditrevision)
                    : worldeditrevision + 1;
    worldeditrevision = record.revision;
    incomingworldeditrevision = 0;
    record.timestamp = ullong(time(NULL));
    record.args[0] = scatter.type;
    record.selection.o = support;
    record.selection.s = ivec(1, 1, 1);
    record.selection.grid = WORLD_BLOCK_SIZE;
    record.selection.orient = scatter.orient;
    record.selection.cx = record.selection.cy = record.selection.corner = 0;
    record.selection.cxs = record.selection.cys = 2;
    if(place) record.scatterafter.add(scatter);
    else record.scatterbefore.add(scatter);

    worldchunkdiffstate *state = findworldchunkdiffstate(chunk.x, chunk.y, true);
    state->revision = max(state->revision, record.revision);
    state->pending.add(cloneworldeditrecord(record));
    state->journal.add(cloneworldeditrecord(record));
    state->audit.add(cloneworldeditrecord(record));
    chunk.dirty = true;
}

bool worldtorchincell(const ivec &cell)
{
    if(cell.x < 0 || cell.y < 0 || cell.z < 0 ||
       cell.x >= worldsize || cell.y >= worldsize ||
       cell.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE)
        return false;
    const int chunkx = worldfirstchunkx + cell.x / WORLD_CHUNK_SIZE,
              chunky = worldfirstchunky + cell.y / WORLD_CHUNK_SIZE,
              chunkindex = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(chunkindex)) return false;
    const worldchunk &chunk = worldchunks[chunkindex];
    const ivec origin = worldchunkorigin(chunk);
    loopv(chunk.scatter)
    {
        const worldscatterinstance &scatter = chunk.scatter[i];
        if(isworldplaceable(scatter.type) &&
           scatter.x == cell.x - origin.x &&
           scatter.y == cell.y - origin.y &&
           scatter.z == cell.z)
            return true;
    }
    return false;
}

int getworldscatterindexat(const ivec &support, int orient)
{
    const ivec target = ivec(support).add(ivec(worldorientnormal(orient)).mul(WORLD_BLOCK_SIZE));
    if(target.x < 0 || target.y < 0 || target.z < 0 || target.x >= worldsize || target.y >= worldsize ||
       target.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE)
        return -1;
    const int chunkx = worldfirstchunkx + target.x / WORLD_CHUNK_SIZE,
              chunky = worldfirstchunky + target.y / WORLD_CHUNK_SIZE,
              chunkindex = findworldchunk(chunkx, chunky);
    if(!worldchunks.inrange(chunkindex)) return -1;
    const worldchunk &chunk = worldchunks[chunkindex];
    const ivec origin = worldchunkorigin(chunk);
    loopv(chunk.scatter)
    {
        const worldscatterinstance &scatter = chunk.scatter[i];
        if(scatter.x == target.x - origin.x && scatter.y == target.y - origin.y && scatter.z == target.z && scatter.orient == orient)
            return scatter.type;
    }
    return -1;
}

bool editworldscatter(int type, const ivec &support, int orient, bool place)
{
    if(!worldscatterdefinitions.inrange(type) || orient < O_LEFT || orient > O_TOP ||
       (!isworldplaceable(type) && orient != O_TOP) || (isworldplaceable(type) && orient == O_BOTTOM))
        return false;

    const ivec target = ivec(support).add(ivec(worldorientnormal(orient)).mul(WORLD_BLOCK_SIZE));

    if(target.x < 0 || target.y < 0 || target.z < 0 || target.x >= worldsize || target.y >= worldsize || target.z + WORLD_BLOCK_SIZE > WORLD_MAP_SIZE)
        return false;

    const int chunkx = worldfirstchunkx + target.x / WORLD_CHUNK_SIZE,
              chunky = worldfirstchunky + target.y / WORLD_CHUNK_SIZE,
              chunkindex = findworldchunk(chunkx, chunky);

    if(!worldchunks.inrange(chunkindex)) return false;
    worldchunk &chunk = worldchunks[chunkindex];
    if(chunk.loading || !chunk.root || !worldchunkmounted(chunk)) return false;
    const ivec origin = worldchunkorigin(chunk);
    worldscatterinstance scatter(target.x - origin.x, target.y - origin.y, target.z, type, orient);
    game::cacheworldscattertransform(chunk.x, chunk.y, game::getworldscattermaxoffset(), scatter);

    int existing = -1;
    loopv(chunk.scatter)
    {
        if(chunk.scatter[i].x == scatter.x && chunk.scatter[i].y == scatter.y && chunk.scatter[i].z == scatter.z)
        {
            existing = i;
            break;
        }
    }

    if(place)
    {
        if(existing >= 0 || !validworldscatter(chunk, scatter)) return false;
        chunk.scatter.add(scatter);
    }
    else
    {
        if(existing < 0 || chunk.scatter[existing].type != type || chunk.scatter[existing].orient != orient)
            return false;

        scatter = chunk.scatter[existing];
        chunk.scatter.removeunordered(existing);
    }
    commitworldscatterrecord(chunk, scatter, support, place);
    updateworldscatterers();
    return true;
}

bool worldselectionready(const selinfo &selection)
{
    if(!worldroot || selection.grid <= 0 || selection.s.x <= 0 ||
       selection.s.y <= 0 || selection.s.z <= 0)
        return false;

    int minx = worldfirstchunkx + int(floor(double(selection.o.x) / WORLD_CHUNK_SIZE)),
        miny = worldfirstchunky + int(floor(double(selection.o.y) / WORLD_CHUNK_SIZE)),
        maxx = worldfirstchunkx + int(floor(double(selection.o.x +
                    selection.s.x * selection.grid - 1) / WORLD_CHUNK_SIZE)),
        maxy = worldfirstchunky + int(floor(double(selection.o.y +
                    selection.s.y * selection.grid - 1) / WORLD_CHUNK_SIZE));
    for(int y = miny; y <= maxy; ++y) for(int x = minx; x <= maxx; ++x)
    {
        int index = findworldchunk(x, y);
        if(!worldchunks.inrange(index) || worldchunks[index].loading ||
           !worldchunks[index].root || !worldchunkmounted(worldchunks[index]))
            return false;
        const worldchunk &chunk = worldchunks[index];
        ivec origin = worldchunkorigin(chunk);
        int localminx = clamp(selection.o.x - origin.x, 0, int(WORLD_CHUNK_SIZE) - 1),
            localminy = clamp(selection.o.y - origin.y, 0, int(WORLD_CHUNK_SIZE) - 1),
            localmaxx = clamp(selection.o.x + selection.s.x * selection.grid - 1 - origin.x,
                              0, int(WORLD_CHUNK_SIZE) - 1),
            localmaxy = clamp(selection.o.y + selection.s.y * selection.grid - 1 - origin.y,
                              0, int(WORLD_CHUNK_SIZE) - 1),
            minsection = clamp(selection.o.z / WORLD_SECTION_SIZE, 0,
                               int(WORLD_SECTION_LAYERS) - 1),
            maxsection = clamp((selection.o.z + selection.s.z * selection.grid - 1) /
                               WORLD_SECTION_SIZE, 0, int(WORLD_SECTION_LAYERS) - 1);
        for(int section = minsection; section <= maxsection; ++section)
            for(int tiley = localminy / WORLD_SECTION_SIZE;
                tiley <= localmaxy / WORLD_SECTION_SIZE; ++tiley)
                for(int tilex = localminx / WORLD_SECTION_SIZE;
                    tilex <= localmaxx / WORLD_SECTION_SIZE; ++tilex)
                {
                    int tile = tiley * WORLD_SECTION_COLUMNS + tilex;
                    if(!(chunk.mountedtiles[section] & (1U << tile))) return false;
                }
    }
    return true;
}

static const char *worldeditoperationname(int operation)
{
    switch(operation)
    {
        case WORLD_EDIT_SET_CUBE: return "SET_CUBE";
        case WORLD_EDIT_DELETE_CUBE: return "DELETE_CUBE";
        case WORLD_EDIT_SET_MATERIAL: return "SET_MATERIAL";
        case WORLD_EDIT_MOVE_CORNER: return "MOVE_CORNER";
        case WORLD_EDIT_FILL_VOLUME: return "FILL_VOLUME";
        case WORLD_EDIT_DELETE_VOLUME: return "DELETE_VOLUME";
        case WORLD_EDIT_PASTE_BLUEPRINT: return "PASTE_BLUEPRINT";
        case WORLD_EDIT_DELETE_BLUEPRINT: return "DELETE_BLUEPRINT";
        case WORLD_EDIT_SET_SCATTER: return "SET_SCATTER";
        case WORLD_EDIT_DELETE_SCATTER: return "DELETE_SCATTER";
        default: return "UNKNOWN";
    }
}

static void pasteworlddiffnode(cube &c, const worlddiffnode &node)
{
    discardchildren(c);
    memcpy(c.edges, node.edges, sizeof(node.edges));
    memcpy(c.texture, node.texture, sizeof(node.texture));
    c.material = node.material;
    c.visible = c.merged = 0;
}

static bool commitworldadminrecord(const worldeditrecord &source, bool inverse)
{
    int chunkindex = findworldchunk(source.chunkx, source.chunky);
    if(!worldchunks.inrange(chunkindex))
    {
        int generated = 0;
        chunkindex = acquireworldchunksync(source.chunkx, source.chunky, generated);
    }
    if(!worldchunks.inrange(chunkindex)) return false;
    worldchunk &chunk = worldchunks[chunkindex];
    const vector<worlddiffnode> &target = inverse ? source.before : source.after;
    const vector<worlddiffnode> &oldstate = inverse ? source.after : source.before;
    const vector<worldscatterinstance> &scattertarget =
        inverse ? source.scatterbefore : source.scatterafter;
    const vector<worldscatterinstance> &scatterold =
        inverse ? source.scatterafter : source.scatterbefore;
    int families = 0;
    loopv(target)
    {
        const worlddiffnode &node = target[i];
        applyworlddiffnode(chunk.root, node, false, families);
        if(worldchunkmounted(chunk))
        {
            ivec pos = ivec(worldchunkorigin(chunk)).add(ivec(node.x, node.y, node.z));
            cube &runtimecube = lookupcube(pos, node.size);
            pasteworlddiffnode(runtimecube, node);
            changed(pos, ivec(pos).add(node.size), false);
        }
    }
    applyworldscatterchange(chunk.scatter, scatterold, scattertarget);
    game::cacheworldscattertransforms(chunk.x, chunk.y, game::getworldscattermaxoffset(), chunk.scatter);
    commitchanges();

    worldchunkdiffstate *state = findworldchunkdiffstate(chunk.x, chunk.y, true);
    worldeditrecord record;
    record.chunkx = chunk.x;
    record.chunky = chunk.y;
    record.operation = inverse ? WORLD_EDIT_DELETE_BLUEPRINT : WORLD_EDIT_PASTE_BLUEPRINT;
    record.author = worldeditauthor;
    record.revision = ++worldeditrevision;
    state->revision = max(state->revision, record.revision);
    record.timestamp = ullong(time(NULL));
    record.args[0] = int(source.revision & 0xFFFFFFFFU);
    record.args[1] = int(source.revision >> 32);
    record.args[2] = inverse ? 1 : 2;
    record.args[3] = INT_MIN;
    record.selection = source.selection;
    loopv(oldstate) record.before.add(oldstate[i]);
    loopv(target) record.after.add(target[i]);
    loopv(scatterold) record.scatterbefore.add(scatterold[i]);
    loopv(scattertarget) record.scatterafter.add(scattertarget[i]);
    state->pending.add(cloneworldeditrecord(record));
    state->journal.add(cloneworldeditrecord(record));
    state->audit.add(cloneworldeditrecord(record));
    state->canonicalhash = hashworldchunk(chunk.root);
    chunk.dirty = true;
    updateworldscatterers();
    return true;
}

static bool worldauditrecordundone(const worldeditrecord &source)
{
    worldchunkdiffstate *state = findworldchunkdiffstate(source.chunkx, source.chunky);
    if(!state) return false;
    int status = 0;
    loopv(state->audit)
    {
        const worldeditrecord &record = *state->audit[i];
        if(record.args[3] != INT_MIN) continue;
        ullong referenced = uint(record.args[0]) | (ullong(uint(record.args[1])) << 32);
        if(referenced == source.revision) status = record.args[2];
    }
    return status == 1;
}

static worldeditrecord *latestworldauditrecord()
{
    worldeditrecord *latest = NULL;
    loopv(worldchunkdiffstates) loopvj(worldchunkdiffstates[i]->audit)
    {
        worldeditrecord *record = worldchunkdiffstates[i]->audit[j];
        if(record->args[3] == INT_MIN) continue;
        if(worldauditrecordundone(*record)) continue;
        if(!latest || record->timestamp > latest->timestamp ||
           (record->timestamp == latest->timestamp && record->revision > latest->revision))
            latest = record;
    }
    return latest;
}

static void worldundocommand(int *requested)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldundo %d", max(*requested, 1));
        game::requestworldcommand(command);
        return;
    }
    int count = max(*requested, 1), applied = 0;
    while(applied < count)
    {
        worldeditrecord *record = latestworldauditrecord();
        if(!record || !commitworldadminrecord(*record, true)) break;
        worldredostack.add(cloneworldeditrecord(*record));
        applied++;
    }
    conoutf("worldundo committed %d inverse revision%s", applied, applied == 1 ? "" : "s");
}

static void worldredocommand(int *requested)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldredo %d", max(*requested, 1));
        game::requestworldcommand(command);
        return;
    }
    int count = max(*requested, 1), applied = 0;
    while(applied < count)
    {
        worldeditrecord *record = NULL;
        bool owned = false;
        if(!worldredostack.empty())
        {
            record = worldredostack.pop();
            owned = true;
        }
        else loopv(worldchunkdiffstates)
        {
            worldchunkdiffstate &state = *worldchunkdiffstates[i];
            loopvj(state.audit)
            {
                worldeditrecord *candidate = state.audit[j];
                if(candidate->args[3] == INT_MIN || !worldauditrecordundone(*candidate)) continue;
                if(!record || candidate->timestamp > record->timestamp ||
                   (candidate->timestamp == record->timestamp &&
                    candidate->revision > record->revision))
                    record = candidate;
            }
        }
        if(!record) break;
        if(commitworldadminrecord(*record, false)) applied++;
        if(owned) delete record;
    }
    conoutf("worldredo committed %d new revision%s", applied, applied == 1 ? "" : "s");
}

COMMANDN(worldundo, worldundocommand, "i");
COMMANDN(worldredo, worldredocommand, "i");

static void worldlogcommand(char *playertext, int *radius, int *minutes)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldlog %s %d %d",
                        playertext ? playertext : "", *radius, *minutes);
        game::requestworldcommand(command);
        return;
    }
    int author = playertext && playertext[0] ? game::findclientnum(playertext) : INT_MIN,
        seconds = *minutes > 0 ? *minutes * 60 : INT_MAX, shown = 0;
    if(playertext && playertext[0] && author < 0)
    {
        conoutf(CON_ERROR, "worldlog: unknown player %s", playertext);
        return;
    }
    ullong now = ullong(time(NULL));
    loopv(worldchunkdiffstates) loopvj(worldchunkdiffstates[i]->audit)
    {
        const worldeditrecord &record = *worldchunkdiffstates[i]->audit[j];
        if(author != INT_MIN && record.author != author) continue;
        if(now > record.timestamp && now - record.timestamp > ullong(seconds)) continue;
        if(*radius > 0 && player)
        {
            int chunkindex = findworldchunk(record.chunkx, record.chunky);
            if(!worldchunks.inrange(chunkindex)) continue;
            ivec origin = worldchunkorigin(worldchunks[chunkindex]);
            if(abs(origin.x - int(player->o.x)) > *radius ||
               abs(origin.y - int(player->o.y)) > *radius)
                continue;
        }
        conoutf("chunk %d %d rev " WORLD_ULL_FORMAT " author %d time "
                WORLD_ULL_FORMAT " %s (%d nodes)",
                record.chunkx, record.chunky, record.revision, record.author,
                record.timestamp, worldeditoperationname(record.operation),
                record.after.length());
        shown++;
    }
    conoutf("worldlog: %d matching revision%s", shown, shown == 1 ? "" : "s");
}

COMMANDN(worldlog, worldlogcommand, "sii");

static void worldrevertcommand(char *mode, char *arg1, char *arg2, char *arg3,
                               char *arg4, char *arg5, char *arg6, char *arg7)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldrevert %s %s %s %s %s %s %s %s",
                        mode, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
        game::requestworldcommand(command);
        return;
    }
    int reverted = 0;
    ullong now = ullong(time(NULL));
    if(!strcmp(mode, "player"))
    {
        int author = game::findclientnum(arg1),
            minutes = arg2[0] ? max(atoi(arg2), 0) : 0;
        if(author < 0)
        {
            conoutf(CON_ERROR, "worldrevert: unknown player %s", arg1);
            return;
        }
        loopv(worldchunkdiffstates)
        {
            worldchunkdiffstate &state = *worldchunkdiffstates[i];
            for(int j = state.audit.length() - 1; j >= 0; --j)
            {
                worldeditrecord &record = *state.audit[j];
                if(record.args[3] == INT_MIN || record.author != author) continue;
                if(minutes > 0 && now > record.timestamp &&
                   now - record.timestamp > ullong(minutes * 60))
                    continue;
                if(commitworldadminrecord(record, true)) reverted++;
            }
        }
    }
    else if(!strcmp(mode, "area"))
    {
        int x1 = atoi(arg1), y1 = atoi(arg2), z1 = atoi(arg3),
            x2 = atoi(arg4), y2 = atoi(arg5), z2 = atoi(arg6),
            minutes = arg7[0] ? max(atoi(arg7), 0) : 0;
        if(x1 > x2) swap(x1, x2);
        if(y1 > y2) swap(y1, y2);
        if(z1 > z2) swap(z1, z2);
        loopv(worldchunkdiffstates)
        {
            worldchunkdiffstate &state = *worldchunkdiffstates[i];
            for(int j = state.audit.length() - 1; j >= 0; --j)
            {
                worldeditrecord &record = *state.audit[j];
                if(record.args[3] == INT_MIN) continue;
                if(minutes > 0 && now > record.timestamp &&
                   now - record.timestamp > ullong(minutes * 60))
                    continue;
                bool intersects = false;
                loopvk(record.after)
                {
                    const worlddiffnode &node = record.after[k];
                    int nx = record.chunkx * WORLD_CHUNK_SIZE + node.x,
                        ny = record.chunky * WORLD_CHUNK_SIZE + node.y;
                    if(nx + node.size > x1 && nx < x2 &&
                       ny + node.size > y1 && ny < y2 &&
                       node.z + node.size > z1 && node.z < z2)
                    {
                        intersects = true;
                        break;
                    }
                }
                if(intersects && commitworldadminrecord(record, true)) reverted++;
            }
        }
    }
    else
    {
        conoutf(CON_ERROR,
                "usage: /worldrevert player <id> [minutes] | area <x1 y1 z1> <x2 y2 z2> [minutes]");
        return;
    }
    conoutf("worldrevert committed %d inverse revision%s",
            reverted, reverted == 1 ? "" : "s");
}

COMMANDN(worldrevert, worldrevertcommand, "ssssssss");

static void worldrestorecommand(char *kind, char *xtext, char *ytext,
                                char *ztext, char *revisiontext)
{
    if(game::waitforserveredit())
    {
        defformatstring(command, "worldrestore %s %s %s %s %s",
                        kind, xtext, ytext, ztext, revisiontext);
        game::requestworldcommand(command);
        return;
    }
    if(strcmp(kind, "chunk"))
    {
        conoutf(CON_ERROR, "usage: /worldrestore chunk <x y z> <revision>");
        return;
    }
    int x = atoi(xtext), y = atoi(ytext), z = atoi(ztext);
    ullong revision = strtoull(revisiontext, NULL, 10);
    if(z != WORLD_DIFF_Z)
    {
        conoutf(CON_ERROR, "this world stores its full vertical band as chunk z=0");
        return;
    }
    worldchunkdiffstate *state = findworldchunkdiffstate(x, y);
    if(!state)
    {
        conoutf(CON_ERROR, "chunk %d %d has no revision history", x, y);
        return;
    }
    int restored = 0;
    for(int i = state->audit.length() - 1; i >= 0; --i)
    {
        worldeditrecord &record = *state->audit[i];
        if(record.args[3] == INT_MIN || record.revision <= revision) continue;
        if(commitworldadminrecord(record, true)) restored++;
    }
    conoutf("worldrestore chunk %d %d to revision " WORLD_ULL_FORMAT
            " committed %d inverse revision%s",
            x, y, revision, restored, restored == 1 ? "" : "s");
}

COMMANDN(worldrestore, worldrestorecommand, "sssss");


#endif
