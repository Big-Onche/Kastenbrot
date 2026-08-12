#include "engine.h"
#include "worlddef.h"

#ifdef WIN32
#define WORLDDEF_ULL_FORMAT "%I64u"
#else
#define WORLDDEF_ULL_FORMAT "%llu"
#endif

static ullong worldpersistentid(const char *id)
{
    ullong hash = 14695981039346656037ULL;
    if(!id) return hash;
    for(const uchar *cursor = (const uchar *)id; *cursor; ++cursor)
    {
        const uchar c = *cursor >= 'A' && *cursor <= 'Z' ? *cursor + ('a' - 'A') : *cursor;
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

worlddefinition::worlddefinition(const char *id)
    : persistentid(worldpersistentid(id)), worldsize(1.0f), heldsize(100.0f), texsize(1), lightradius(0), hardness(1.0f), toolspeed(1.0f),
      tooldamage(2.0f),
      foodhealth(0),
      maxstack(64), item(-1), slot(DEFAULT_GEOM), sideslot(DEFAULT_GEOM), bottomslot(DEFAULT_GEOM), mapmodel(-1), furnaceinputslots(0),
      furnaceinputlimit(0), foodtime(0), requiredtier(0), toolwear(1), tooltier(0), maxdurability(0), supportdistance(0), hasitem(false),
      hasheld(false), hascube(false),
      scatter(false), placeable(false), hasmining(false), hastool(false), hasfurnace(false), hasfood(false), hassupport(false), itemstackset(false),
      scattermodelset(false),
      placeablemodelset(false), hardnessset(false), tooltierset(false), toolspeedset(false), explicitdrops(false), errorfallback(false), fall(false),
      heldflipx(false), heldflipy(false), handbreakable(true), supportdecay(false), supportpersistentonplace(false)
{
    copystring(this->id, id);
    name[0] = texture[0] = icon[0] = cubetexture[0] = sidetexture[0] = bottom[0] = bottomtexture[0] = model[0] = modelicon[0] = '\0';
    lightcolor[0] = preferredtool[0] = tooltype[0] = '\0';
}

vector<worlddefinition *> worlddefinitions;
vector<worlddefinition *> worldcubedefinitions, worldscatterdefinitions, inventoryitemdefinitions;
hashtable<worldpersistentkey, int> worldcubepersistentindexes(256), worldscatterpersistentindexes(256), inventoryitempersistentindexes(256);
int worlderrorcube = -1, worlderrorobject = -1, worlderroritem = -1;

static worlddefinition *currentworlddefinition = NULL;
enum
{
    WORLDDEF_NONE = 0, WORLDDEF_ITEM, WORLDDEF_HELD, WORLDDEF_CUBE, WORLDDEF_SCATTER, WORLDDEF_PLACEABLE, WORLDDEF_MINING, WORLDDEF_TOOL,
    WORLDDEF_FURNACE, WORLDDEF_FOOD, WORLDDEF_SUPPORT
};
static int currentworldcomponent = WORLDDEF_NONE, worlddefinitionerrors = 0;

worlddefinition *findworlddefinition(const char *id)
{
    loopv(worlddefinitions) if(!cubecasecmp(worlddefinitions[i]->id, id)) return worlddefinitions[i];
    return NULL;
}

worlddefinition *findworldcube(const char *id)
{
    loopv(worldcubedefinitions) if(!cubecasecmp(worldcubedefinitions[i]->id, id)) return worldcubedefinitions[i];
    return NULL;
}

worlddefinition *findworldscatter(const char *id)
{
    loopv(worldscatterdefinitions) if(!cubecasecmp(worldscatterdefinitions[i]->id, id)) return worldscatterdefinitions[i];
    return NULL;
}

worlddefinition *findinventoryitem(const char *id)
{
    loopv(inventoryitemdefinitions) if(!cubecasecmp(inventoryitemdefinitions[i]->id, id)) return inventoryitemdefinitions[i];
    return NULL;
}

int numworlddefinitions() { return worlddefinitions.length(); }

int getworlddefinitionindex(const char *id)
{
    worlddefinition *definition = findworlddefinition(id);
    return definition ? worlddefinitions.find(definition) : -1;
}

const char *getworlddefinitionid(int index)
{
    return worlddefinitions.inrange(index) ? worlddefinitions[index]->id : "";
}

void resetworlddefinitionregistry()
{
    worlddefinitions.deletecontents();
    worldcubedefinitions.shrink(0);
    worldscatterdefinitions.shrink(0);
    inventoryitemdefinitions.shrink(0);
    worldcubepersistentindexes.clear();
    worldscatterpersistentindexes.clear();
    inventoryitempersistentindexes.clear();
    worlderrorcube = worlderrorobject = worlderroritem = -1;
    currentworlddefinition = NULL;
    currentworldcomponent = WORLDDEF_NONE;
    worlddefinitionerrors = 0;
}

static void worlddefinitionerror(const char *message)
{
    conoutf(CON_ERROR, "worlddef \"%s\": %s", currentworlddefinition ? currentworlddefinition->id : "<none>", message);
    ++worlddefinitionerrors;
}

static const char *worlddefinitioncommand(const char *command, int component)
{
    if(component == WORLDDEF_NONE)
    {
        if(!strcmp(command, "item")) return "worlddef_item";
        if(!strcmp(command, "held")) return "worlddef_held";
        if(!strcmp(command, "cube")) return "worlddef_cube";
        if(!strcmp(command, "scatter")) return "worlddef_scatter";
        if(!strcmp(command, "placeable")) return "worlddef_placeable";
        if(!strcmp(command, "mining")) return "worlddef_mining";
        if(!strcmp(command, "tool")) return "worlddef_tool";
        if(!strcmp(command, "furnace")) return "worlddef_furnace";
        if(!strcmp(command, "food")) return "worlddef_food";
        if(!strcmp(command, "support")) return "worlddef_support";
        if(!strcmp(command, "drop")) return "worlddef_drop";
    }
    else if(component == WORLDDEF_ITEM)
    {
        if(!strcmp(command, "name")) return "worlddef_name";
        if(!strcmp(command, "stack")) return "worlddef_stack";
        if(!strcmp(command, "texture") || !strcmp(command, "model")) return "worlddef_itemtexture";
        if(!strcmp(command, "icon")) return "worlddef_icon";
        if(!strcmp(command, "scale")) return "worlddef_scale";
    }
    else if(component == WORLDDEF_HELD)
    {
        if(!strcmp(command, "flip")) return "worlddef_heldflip";
        if(!strcmp(command, "size")) return "worlddef_heldsize";
    }
    else if(component == WORLDDEF_CUBE)
    {
        if(!strcmp(command, "texture")) return "worlddef_cubetexture";
        if(!strcmp(command, "side")) return "worlddef_side";
        if(!strcmp(command, "bottom")) return "worlddef_bottom";
        if(!strcmp(command, "texsize")) return "worlddef_texsize";
        if(!strcmp(command, "falling")) return "worlddef_falling";
    }
    else if(component == WORLDDEF_SCATTER || component == WORLDDEF_PLACEABLE)
    {
        if(!strcmp(command, "model")) return "worlddef_model";
        if(component == WORLDDEF_PLACEABLE && !strcmp(command, "light")) return "worlddef_light";
        if(component == WORLDDEF_PLACEABLE && !strcmp(command, "lightcolor")) return "worlddef_lightcolor";
    }
    else if(component == WORLDDEF_MINING)
    {
        if(!strcmp(command, "hardness")) return "worlddef_hardness";
        if(!strcmp(command, "tool")) return "worlddef_miningtool";
        if(!strcmp(command, "tier")) return "worlddef_miningtier";
        if(!strcmp(command, "wear")) return "worlddef_wear";
        if(!strcmp(command, "handbreakable")) return "worlddef_handbreakable";
    }
    else if(component == WORLDDEF_TOOL)
    {
        if(!strcmp(command, "type")) return "worlddef_tooltype";
        if(!strcmp(command, "tier")) return "worlddef_tooltier";
        if(!strcmp(command, "speed")) return "worlddef_speed";
        if(!strcmp(command, "durability")) return "worlddef_durability";
        if(!strcmp(command, "damage")) return "worlddef_damage";
    }
    else if(component == WORLDDEF_FURNACE)
    {
        if(!strcmp(command, "slots")) return "worlddef_slots";
        if(!strcmp(command, "capacity")) return "worlddef_capacity";
    }
    else if(component == WORLDDEF_FOOD)
    {
        if(!strcmp(command, "health")) return "worlddef_foodhealth";
        if(!strcmp(command, "time")) return "worlddef_foodtime";
    }
    else if(component == WORLDDEF_SUPPORT)
    {
        if(!strcmp(command, "distance")) return "worlddef_supportdistance";
        if(!strcmp(command, "decay")) return "worlddef_supportdecay";
        if(!strcmp(command, "persistentonplace")) return "worlddef_supportpersistentonplace";
    }
    return NULL;
}

static void executeworlddefinitionbody(const char *body, int component)
{
    vector<char> rewritten;
    bool commandstart = true, quoted = false, comment = false;
    int depth = 0;
    for(const char *cursor = body; cursor && *cursor;)
    {
        if(comment)
        {
            const char c = *cursor++;
            rewritten.add(c);
            if(c == '\n') { comment = false; commandstart = true; }
            continue;
        }
        if(quoted)
        {
            const char c = *cursor++;
            rewritten.add(c);
            if(c == '^' && *cursor) rewritten.add(*cursor++);
            else if(c == '"') quoted = false;
            continue;
        }
        if(cursor[0] == '/' && cursor[1] == '/')
        {
            rewritten.add(*cursor++);
            rewritten.add(*cursor++);
            comment = true;
            continue;
        }
        if(*cursor == '"') { quoted = true; rewritten.add(*cursor++); continue; }
        if(*cursor == '[') { ++depth; rewritten.add(*cursor++); continue; }
        if(*cursor == ']') { if(depth > 0) --depth; rewritten.add(*cursor++); continue; }
        if(depth == 0 && (*cursor == ';' || *cursor == '\n'))
        {
            commandstart = true;
            rewritten.add(*cursor++);
            continue;
        }
        if(depth == 0 && commandstart)
        {
            if(iscubespace(*cursor)) { rewritten.add(*cursor++); continue; }
            const char *start = cursor;
            while(*cursor && !iscubespace(*cursor) && *cursor != ';' && *cursor != '[' && *cursor != ']') ++cursor;
            string command;
            copystring(command, start, min(size_t(cursor - start + 1), sizeof(command)));
            const char *replacement = worlddefinitioncommand(command, component);
            if(!replacement)
            {
                defformatstring(message, "unknown %s command \"%s\"", component == WORLDDEF_NONE ? "worlddef" : "component", command);
                worlddefinitionerror(message);
                return;
            }
            while(*replacement) rewritten.add(*replacement++);
            commandstart = false;
            continue;
        }
        rewritten.add(*cursor++);
    }
    rewritten.add('\0');
    execute(rewritten.getbuf());
}

static bool beginworldcomponent(int component, bool &present, const char *name)
{
    if(!currentworlddefinition || currentworldcomponent != WORLDDEF_NONE)
    {
        worlddefinitionerror("component blocks must be direct children of worlddef");
        return false;
    }
    if(present)
    {
        defformatstring(message, "duplicate %s component", name);
        worlddefinitionerror(message);
        return false;
    }
    present = true;
    currentworldcomponent = component;
    return true;
}

static void endworldcomponent() { currentworldcomponent = WORLDDEF_NONE; }

ICOMMAND(worlddef, "sS", (char *id, char *body),
{
    if(currentworlddefinition)
    {
        worlddefinitionerror("nested worlddef blocks are not allowed");
        return;
    }
    if(!id[0] || findworlddefinition(id))
    {
        conoutf(CON_ERROR, "duplicate or empty worlddef id \"%s\"", id);
        ++worlddefinitionerrors;
        return;
    }
    currentworlddefinition = worlddefinitions.add(new worlddefinition(id));
    executeworlddefinitionbody(body, WORLDDEF_NONE);
    currentworlddefinition = NULL;
    currentworldcomponent = WORLDDEF_NONE;
});

ICOMMANDS("worlddef_item", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_ITEM, currentworlddefinition->hasitem, "item")) return;
    inventoryitemdefinitions.add(currentworlddefinition);
    executeworlddefinitionbody(body, WORLDDEF_ITEM);
    endworldcomponent();
});

ICOMMANDS("worlddef_held", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_HELD, currentworlddefinition->hasheld, "held")) return;
    executeworlddefinitionbody(body, WORLDDEF_HELD);
    endworldcomponent();
});

ICOMMANDS("worlddef_cube", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_CUBE, currentworlddefinition->hascube, "cube")) return;
    worldcubedefinitions.add(currentworlddefinition);
    executeworlddefinitionbody(body, WORLDDEF_CUBE);
    endworldcomponent();
});

ICOMMANDS("worlddef_scatter", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_SCATTER, currentworlddefinition->scatter, "scatter")) return;
    if(worldscatterdefinitions.find(currentworlddefinition) < 0) worldscatterdefinitions.add(currentworlddefinition);
    executeworlddefinitionbody(body, WORLDDEF_SCATTER);
    endworldcomponent();
});

ICOMMANDS("worlddef_placeable", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_PLACEABLE, currentworlddefinition->placeable, "placeable")) return;
    if(worldscatterdefinitions.find(currentworlddefinition) < 0) worldscatterdefinitions.add(currentworlddefinition);
    executeworlddefinitionbody(body, WORLDDEF_PLACEABLE);
    endworldcomponent();
});

ICOMMANDS("worlddef_mining", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_MINING, currentworlddefinition->hasmining, "mining")) return;
    executeworlddefinitionbody(body, WORLDDEF_MINING);
    endworldcomponent();
});

ICOMMANDS("worlddef_tool", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_TOOL, currentworlddefinition->hastool, "tool")) return;
    executeworlddefinitionbody(body, WORLDDEF_TOOL);
    endworldcomponent();
});

ICOMMANDS("worlddef_furnace", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_FURNACE, currentworlddefinition->hasfurnace, "furnace")) return;
    executeworlddefinitionbody(body, WORLDDEF_FURNACE);
    endworldcomponent();
});

ICOMMANDS("worlddef_food", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_FOOD, currentworlddefinition->hasfood, "food")) return;
    executeworlddefinitionbody(body, WORLDDEF_FOOD);
    endworldcomponent();
});

ICOMMANDS("worlddef_support", "S", (char *body),
{
    if(!currentworlddefinition || !beginworldcomponent(WORLDDEF_SUPPORT, currentworlddefinition->hassupport, "support")) return;
    executeworlddefinitionbody(body, WORLDDEF_SUPPORT);
    endworldcomponent();
});

ICOMMANDS("worlddef_name", "s", (char *value), copystring(currentworlddefinition->name, value));
ICOMMANDS("worlddef_stack", "i", (int *value),
{
    currentworlddefinition->maxstack = *value;
    currentworlddefinition->itemstackset = true;
});
ICOMMANDS("worlddef_itemtexture", "s", (char *value), copystring(currentworlddefinition->texture, value));
ICOMMANDS("worlddef_icon", "s", (char *value), copystring(currentworlddefinition->texture, value));
ICOMMANDS("worlddef_scale", "f", (float *value), currentworlddefinition->worldsize = *value);
ICOMMANDS("worlddef_heldflip", "ii", (int *x, int *y),
{
    currentworlddefinition->heldflipx = *x != 0;
    currentworlddefinition->heldflipy = *y != 0;
});
ICOMMANDS("worlddef_heldsize", "f", (float *value), currentworlddefinition->heldsize = *value);
ICOMMANDS("worlddef_cubetexture", "s", (char *value),
{
    copystring(currentworlddefinition->cubetexture, value);
    currentworlddefinition->cubetextureset = value && value[0];
});
ICOMMANDS("worlddef_side", "s", (char *value), copystring(currentworlddefinition->sidetexture, value));
ICOMMANDS("worlddef_bottom", "s", (char *value), copystring(currentworlddefinition->bottom, value));
ICOMMANDS("worlddef_texsize", "f", (float *value), currentworlddefinition->texsize = *value);
ICOMMANDS("worlddef_falling", "i", (int *value), currentworlddefinition->fall = *value != 0);
ICOMMANDS("worlddef_model", "s", (char *value),
{
    copystring(currentworlddefinition->model, value);
    if(currentworldcomponent == WORLDDEF_SCATTER) currentworlddefinition->scattermodelset = value && value[0];
    else if(currentworldcomponent == WORLDDEF_PLACEABLE) currentworlddefinition->placeablemodelset = value && value[0];
});
ICOMMANDS("worlddef_light", "f", (float *value), currentworlddefinition->lightradius = *value);
ICOMMANDS("worlddef_lightcolor", "s", (char *value), copystring(currentworlddefinition->lightcolor, value));
ICOMMANDS("worlddef_hardness", "f", (float *value),
{
    currentworlddefinition->hardness = *value;
    currentworlddefinition->hardnessset = true;
});
ICOMMANDS("worlddef_miningtool", "s", (char *value), copystring(currentworlddefinition->preferredtool, value));
ICOMMANDS("worlddef_miningtier", "i", (int *value), currentworlddefinition->requiredtier = *value);
ICOMMANDS("worlddef_wear", "i", (int *value), currentworlddefinition->toolwear = *value);
ICOMMANDS("worlddef_handbreakable", "i", (int *value), currentworlddefinition->handbreakable = *value != 0);
ICOMMANDS("worlddef_tooltype", "s", (char *value), copystring(currentworlddefinition->tooltype, value));
ICOMMANDS("worlddef_tooltier", "i", (int *value),
{
    currentworlddefinition->tooltier = *value;
    currentworlddefinition->tooltierset = true;
});
ICOMMANDS("worlddef_speed", "f", (float *value),
{
    currentworlddefinition->toolspeed = *value;
    currentworlddefinition->toolspeedset = true;
});
ICOMMANDS("worlddef_durability", "i", (int *value), currentworlddefinition->maxdurability = *value);
ICOMMANDS("worlddef_damage", "f", (float *value), currentworlddefinition->tooldamage = *value);
ICOMMANDS("worlddef_slots", "i", (int *value), currentworlddefinition->furnaceinputslots = *value);
ICOMMANDS("worlddef_capacity", "i", (int *value), currentworlddefinition->furnaceinputlimit = *value);
ICOMMANDS("worlddef_foodhealth", "f", (float *value), currentworlddefinition->foodhealth = *value);
ICOMMANDS("worlddef_foodtime", "i", (int *value), currentworlddefinition->foodtime = *value);
ICOMMANDS("worlddef_supportdistance", "i", (int *value), currentworlddefinition->supportdistance = *value);
ICOMMANDS("worlddef_supportdecay", "i", (int *value), currentworlddefinition->supportdecay = *value != 0);
ICOMMANDS("worlddef_supportpersistentonplace", "i", (int *value), currentworlddefinition->supportpersistentonplace = *value != 0);

ICOMMANDS("worlddef_drop", "siifN", (char *itemid, int *mincount, int *maxcount, float *chance, int *numargs),
{
    if(!currentworlddefinition || currentworldcomponent != WORLDDEF_NONE)
    {
        worlddefinitionerror("drop must be a direct child of worlddef");
        return;
    }
    worlddropdefinition &drop = currentworlddefinition->drops.add();
    copystring(drop.itemid, itemid ? itemid : "");
    drop.item = -2;
    drop.mincount = *mincount;
    drop.maxcount = *maxcount;
    drop.chance = *numargs >= 4 ? *chance : 1.0f;
    currentworlddefinition->explicitdrops = true;
});

static bool validtooltype(const char *type)
{
    return !type[0] || !cubecasecmp(type, "pickaxe") || !cubecasecmp(type, "axe") || !cubecasecmp(type, "shovel") || !cubecasecmp(type, "sword") ||
           !cubecasecmp(type, "hammer_chisel");
}

static bool buildworldpersistentindexes()
{
    bool valid = true;
    worldcubepersistentindexes.clear();
    worldscatterpersistentindexes.clear();
    inventoryitempersistentindexes.clear();
    loopk(3)
    {
        vector<worlddefinition *> &definitions = k == 0 ? worldcubedefinitions : k == 1 ? worldscatterdefinitions : inventoryitemdefinitions;
        hashtable<worldpersistentkey, int> &indexes = k == 0 ? worldcubepersistentindexes
                                                        : k == 1 ? worldscatterpersistentindexes : inventoryitempersistentindexes;
        loopv(definitions)
        {
            worlddefinition &definition = *definitions[i];
            int *previous = indexes.access(worldpersistentkey(definition.persistentid));
            if(previous && cubecasecmp(definitions[*previous]->id, definition.id))
            {
                conoutf(CON_ERROR, "persistent world definition ID collision " WORLDDEF_ULL_FORMAT " between \"%s\" and \"%s\"",
                        definition.persistentid, definitions[*previous]->id, definition.id);
                valid = false;
            }
            else indexes.access(worldpersistentkey(definition.persistentid), i);
        }
    }
    return valid;
}

bool resolveworlddefinitionregistry()
{
    loopv(worlddefinitions)
    {
        worlddefinition &definition = *worlddefinitions[i];
        definition.item = definition.hasitem ? inventoryitemdefinitions.find(&definition) : -1;
        if(definition.hasitem && (!definition.name[0] || !definition.itemstackset || definition.maxstack <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": item requires name and a positive stack", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hasheld && (!definition.hasitem || definition.heldsize <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": held requires item and a positive size percentage", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hascube && (!definition.cubetextureset || !definition.cubetexture[0] || definition.texsize <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": cube requires texture and a positive texsize", definition.id);
            ++worlddefinitionerrors;
        }
        if((definition.scatter && (!definition.scattermodelset || !definition.model[0])) ||
           (definition.placeable && (!definition.placeablemodelset || !definition.model[0])))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": scatter/placeable requires model", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hasmining && (!definition.hardnessset || definition.hardness <= 0 || definition.requiredtier < 0 || definition.toolwear < 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": mining requires positive hardness and non-negative tier/wear", definition.id);
            ++worlddefinitionerrors;
        }
        if(!validtooltype(definition.preferredtool))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": unknown mining tool type \"%s\"", definition.id, definition.preferredtool);
            ++worlddefinitionerrors;
        }
        if(definition.hastool && (!definition.hasitem || !definition.tooltype[0] || !definition.tooltierset || definition.tooltier < 0 ||
           !definition.toolspeedset || definition.toolspeed <= 0 || definition.maxdurability <= 0 || definition.tooldamage <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": tool requires item, type, non-negative tier, positive speed, durability, and damage", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hastool && !validtooltype(definition.tooltype))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": unknown tool type \"%s\"", definition.id, definition.tooltype);
            ++worlddefinitionerrors;
        }
        if(definition.hasfurnace && (!definition.hascube || definition.furnaceinputslots < 1 || definition.furnaceinputslots > FURNACE_INPUT_MAX ||
           definition.furnaceinputlimit < 1))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": furnace requires cube, 1-%d slots, and positive capacity", definition.id, FURNACE_INPUT_MAX);
            ++worlddefinitionerrors;
        }
        if(definition.hasfood && (!definition.hasitem || definition.foodhealth <= 0 || definition.foodtime <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": food requires item, positive health, and positive time", definition.id);
            ++worlddefinitionerrors;
        }
        if(definition.hassupport && (!definition.hascube || definition.supportdistance <= 0))
        {
            conoutf(CON_ERROR, "worlddef \"%s\": support requires cube and a positive distance", definition.id);
            ++worlddefinitionerrors;
        }
        loopv(definition.drops)
        {
            worlddropdefinition &drop = definition.drops[i];
            if(!drop.itemid[0] || !cubecasecmp(drop.itemid, "false"))
            {
                if(drop.mincount < 0 || drop.maxcount < drop.mincount || drop.chance < 0 || drop.chance > 1)
                {
                    conoutf(CON_ERROR, "worlddef \"%s\": invalid drop quantity", definition.id);
                    ++worlddefinitionerrors;
                }
                drop.item = -1;
                continue;
            }
            worlddefinition *target = !cubecasecmp(drop.itemid, "self") ? &definition : findworlddefinition(drop.itemid);
            if(!target || !target->hasitem || drop.mincount < 0 || drop.maxcount < drop.mincount || drop.chance < 0 || drop.chance > 1)
            {
                conoutf(CON_ERROR, "worlddef \"%s\": invalid drop target or quantity for \"%s\"", definition.id, drop.itemid);
                ++worlddefinitionerrors;
                continue;
            }
            drop.item = inventoryitemdefinitions.find(target);
        }
    }
    if(worlddefinitionerrors || !buildworldpersistentindexes()) return false;
    worlddefinition *error = findworlddefinition("error");
    if(!error || !error->hasitem || !error->hascube || !error->scatter || !error->placeable)
    {
        conoutf(CON_ERROR, "worlddef \"error\" must contain item, cube, scatter, and placeable components");
        return false;
    }
    worlderroritem = inventoryitemdefinitions.find(error);
    worlderrorcube = worldcubedefinitions.find(error);
    worlderrorobject = worldscatterdefinitions.find(error);
    return true;
}
