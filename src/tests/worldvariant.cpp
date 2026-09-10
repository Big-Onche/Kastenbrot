// Standalone registry regression test; command registration is unnecessary for this test.
#include "engine.h"
#include <cassert>
#undef main
#undef ICOMMAND
#undef ICOMMANDS
#define ICOMMAND(...)
#define ICOMMANDS(...)
#include "../engine/worlddef.cpp"

void conoutf(int type, const char *fmt, ...)
{
}

int cubecasecmp(const char *a, const char *b, int n)
{
    return _strnicmp(a, b, n);
}

static worlddefinition *addcube(const char *id, float hardness, int tier)
{
    worlddefinition *cube = new worlddefinition(id);
    cube->hascube = cube->hasitem = cube->hasmining = cube->hardnessset = true;
    cube->hardness = hardness;
    cube->requiredtier = tier;
    worlddefinitions.add(cube);
    worldcubedefinitions.add(cube);
    inventoryitemdefinitions.add(cube);
    return cube;
}

int main()
{
    worlddefinition *ore = addcube("ore", 10, 3);
    addcube("gabbro", 4.5f, 2);
    addcube("peridotitis", 6, 4);
    copystring(ore->drops.add().itemid, "gem");
    ore->drops[0].mincount = ore->drops[0].maxcount = 1;
    registerworldvariant("ore_gabbro", "ore", "gabbro", "gabbro.png");
    registerworldvariant("ore_peridotitis", "ore", "peridotitis", "peridotitis.png");
    worlddefinition *gabbro = findworldcube("ore_gabbro"), *peridotitis = findworldcube("ore_peridotitis");
    assert(gabbro && peridotitis && worlddefinitionerrors == 0);
    assert(gabbro->hardness == 4.5f && gabbro->requiredtier == 3);
    assert(peridotitis->hardness == 6 && peridotitis->requiredtier == 4);
    assert(ore->hardness == 10 && ore->requiredtier == 3);
    assert(gabbro->persistentid != ore->persistentid && gabbro->persistentid != peridotitis->persistentid);
    assert(findinventoryitem("ore_gabbro") == gabbro);
    assert(!strcmp(gabbro->drops[0].itemid, "gem"));
    gabbro->drops[0].mincount = 2;
    assert(ore->drops[0].mincount == 1 && peridotitis->drops[0].mincount == 1);
    registerworldvariant("duplicate", "ore", "gabbro", "texture.png");
    registerworldvariant("missing", "unknown", "gabbro", "texture.png");
    registerworldvariant("nested", "ore_gabbro", "peridotitis", "texture.png");
    assert(worlddefinitionerrors == 3 && worlddefinitions.length() == 5);
    resetworlddefinitionregistry();
    assert(worlddefinitions.empty() && worldcubedefinitions.empty() && inventoryitemdefinitions.empty());
    puts("World variant registry tests passed");
    return 0;
}
