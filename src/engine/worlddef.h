#ifndef __ENGINE_WORLDDEF_H__
#define __ENGINE_WORLDDEF_H__

struct worldpersistentkey
{
    ullong id;
    worldpersistentkey(ullong id = 0) : id(id) {}
};

static inline uint hthash(const worldpersistentkey &key) { return uint(key.id) ^ uint(key.id >> 32); }
static inline bool htcmp(const worldpersistentkey &a, const worldpersistentkey &b) { return a.id == b.id; }

struct worlddropdefinition
{
    string itemid;
    int item, mincount, maxcount;
    float chance;

    worlddropdefinition() : item(-1), mincount(0), maxcount(0), chance(1.0f)
    {
        itemid[0] = '\0';
    }
};

struct worlddefinition
{
    string id, name, texture, icon, cubetexture, sidetexture, bottom, bottomtexture, model, modelicon, lightcolor;
    string preferredtool, tooltype;
    ullong persistentid;
    float worldsize, heldsize, texsize, lightradius, hardness, toolspeed, tooldamage, foodhealth;
    int maxstack, item, slot, sideslot, bottomslot, mapmodel, furnaceinputslots, furnaceinputlimit, foodtime;
    int requiredtier, toolwear, tooltier, maxdurability, supportdistance;
    vector<worlddropdefinition> drops;
    bool hasitem, hasheld, hascube, scatter, placeable, hasmining, hastool, hasfurnace, hasfood, hassupport;
    bool itemstackset, cubetextureset, scattermodelset, placeablemodelset, hardnessset, tooltierset, toolspeedset;
    bool explicitdrops, errorfallback, fall, heldflipx, heldflipy, handbreakable, supportdecay, supportpersistentonplace;

    worlddefinition(const char *id = "");
};

extern vector<worlddefinition *> worlddefinitions;
extern vector<worlddefinition *> worldcubedefinitions, worldscatterdefinitions, inventoryitemdefinitions;
extern hashtable<worldpersistentkey, int> worldcubepersistentindexes, worldscatterpersistentindexes, inventoryitempersistentindexes;
extern int worlderrorcube, worlderrorobject, worlderroritem;

extern worlddefinition *findworlddefinition(const char *id);
extern worlddefinition *findworldcube(const char *id);
extern worlddefinition *findworldscatter(const char *id);
extern worlddefinition *findinventoryitem(const char *id);
extern void resetworlddefinitionregistry();
extern bool resolveworlddefinitionregistry();

#endif
