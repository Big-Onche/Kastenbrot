// Data-driven item tags and crafting recipes. The CubeScript commands below
// build temporary definitions; reloadrecipes() validates and compiles them.

#include "game.h"

namespace
{
    enum { RECIPE_UNSET = 0, RECIPE_SHAPED, RECIPE_SHAPELESS };

    struct rawingredient
    {
        string key, value;
        int count;

        rawingredient(const char *key = "", const char *value = "", int count = 1) : count(count)
        {
            copystring(this->key, key);
            copystring(this->value, value);
        }
    };

    struct rawrecipe
    {
        string id, source, output, station, skill;
        int type, outputcount, skilllevel;
        bool mirror, invalid;
        vector<char *> pattern;
        vector<rawingredient> ingredients;

        rawrecipe(const char *id = "", const char *source = "")
            : type(RECIPE_UNSET), outputcount(0), skilllevel(0), mirror(false), invalid(false)
        {
            copystring(this->id, id);
            copystring(this->source, source);
            output[0] = station[0] = skill[0] = '\0';
        }

        ~rawrecipe() { pattern.deletecontents(); }
    };

    struct rawtag
    {
        string id, source;
        vector<char *> items;

        rawtag(const char *id = "", const char *source = "")
        {
            copystring(this->id, id);
            copystring(this->source, source);
        }

        ~rawtag() { items.deletecontents(); }
    };

    struct rawfurnacefuel
    {
        string key, source;
        float seconds;

        rawfurnacefuel(const char *key = "", float seconds = 0, const char *source = "") : seconds(seconds)
        {
            copystring(this->key, key);
            copystring(this->source, source);
        }
    };

    struct rawfurnacerecipe
    {
        string id, source, output;
        int outputcount;
        float seconds;
        bool invalid;
        vector<rawingredient> ingredients;

        rawfurnacerecipe(const char *id = "", const char *source = "") : outputcount(0), seconds(0), invalid(false)
        {
            copystring(this->id, id);
            copystring(this->source, source);
            output[0] = '\0';
        }
    };

    struct itemtagdefinition
    {
        string id;
        vector<uchar> members;
    };

    struct recipeingredientdefinition
    {
        int item, tag, count;

        recipeingredientdefinition(int item = -1, int tag = -1, int count = 1) : item(item), tag(tag), count(count) {}
    };

    struct recipedefinition
    {
        string id, source;
        int type, width, height, outputitem, outputcount, stationitem, skill, skilllevel;
        bool mirror;
        int slots[CRAFT_GRID_MAX];
        vector<recipeingredientdefinition> ingredients;

        recipedefinition()
            : type(RECIPE_UNSET), width(0), height(0), outputitem(-1), outputcount(0), stationitem(-1), skill(-1), skilllevel(0), mirror(false)
        {
            id[0] = source[0] = '\0';
            loopi(CRAFT_GRID_MAX) slots[i] = -1;
        }
    };

    struct furnacerecipedefinition
    {
        string id, source;
        int outputitem, outputcount, duration;
        vector<recipeingredientdefinition> ingredients;

        furnacerecipedefinition() : outputitem(-1), outputcount(0), duration(0)
        {
            id[0] = source[0] = '\0';
        }
    };

    static vector<rawrecipe *> rawrecipes;
    static vector<rawtag *> rawtags;
    static vector<rawfurnacefuel *> rawfurnacefuels;
    static vector<rawfurnacerecipe *> rawfurnacerecipes;
    static vector<recipedefinition *> recipes;
    static vector<itemtagdefinition *> itemtags;
    static vector<furnacerecipedefinition *> furnacerecipes;
    static vector<int> furnacefuelmillis;
    static rawrecipe *currentrecipe = NULL;
    static rawfurnacerecipe *currentfurnacerecipe = NULL;
    static string currentsource = "(console)";
    static int recipeerrors = 0;

    static void recipewarning(const char *id, const char *source, const char *message)
    {
        conoutf(CON_WARN, "recipe %s in %s: %s", id && id[0] ? id : "(unnamed)", source && source[0] ? source : "(unknown)", message);
        ++recipeerrors;
    }

    static int hashskill(const char *name)
    {
        if(!name || !name[0]) return -1;
        uint hash = 2166136261U;
        for(const uchar *c = (const uchar *)name; *c; ++c)
        {
            hash ^= *c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c;
            hash *= 16777619U;
        }
        return int(hash & 0x7FFFFFFFU);
    }

    static int ingredientitem(const char *name)
    {
        return name && name[0] ? getinventoryitemindex(name) : -1;
    }

    static bool sameingredient(const recipeingredientdefinition &a, const recipeingredientdefinition &b)
    {
        return a.item == b.item && a.tag == b.tag;
    }

    static int compileingredient(recipedefinition &recipe, const char *name, int count)
    {
        recipeingredientdefinition ingredient;
        ingredient.count = count;
        if(name && name[0] == '#') ingredient.tag = finditemtag(name + 1);
        else ingredient.item = ingredientitem(name);
        if(ingredient.item < 0 && ingredient.tag < 0) return -1;
        loopv(recipe.ingredients) if(sameingredient(recipe.ingredients[i], ingredient))
        {
            if(recipe.type == RECIPE_SHAPELESS) recipe.ingredients[i].count += count;
            return i;
        }
        recipe.ingredients.add(ingredient);
        return recipe.ingredients.length() - 1;
    }

    static rawingredient *findrawsymbol(rawrecipe &recipe, char symbol)
    {
        loopv(recipe.ingredients) if(recipe.ingredients[i].key[0] == symbol && !recipe.ingredients[i].key[1]) return &recipe.ingredients[i];
        return NULL;
    }

    static bool compilerecipe(rawrecipe &raw)
    {
        loopv(recipes) if(!cubecasecmp(recipes[i]->id, raw.id))
        {
            recipewarning(raw.id, raw.source, "duplicate recipe id");
            return false;
        }
        if(raw.invalid) return false;
        if(!raw.id[0]) { recipewarning(raw.id, raw.source, "missing recipe id"); return false; }
        if(raw.type != RECIPE_SHAPED && raw.type != RECIPE_SHAPELESS)
        {
            recipewarning(raw.id, raw.source, "recipetype must be shaped or shapeless");
            return false;
        }
        const int output = ingredientitem(raw.output);
        if(output < 0 || raw.outputcount <= 0)
        {
            recipewarning(raw.id, raw.source, "recipeoutput references an unknown item or has a non-positive quantity");
            return false;
        }

        recipedefinition *recipe = new recipedefinition;
        copystring(recipe->id, raw.id);
        copystring(recipe->source, raw.source);
        recipe->type = raw.type;
        recipe->outputitem = output;
        recipe->outputcount = raw.outputcount;
        recipe->mirror = raw.mirror;
        recipe->skill = hashskill(raw.skill);
        recipe->skilllevel = max(raw.skilllevel, 0);
        if(raw.station[0])
        {
            recipe->stationitem = ingredientitem(raw.station);
            if(recipe->stationitem < 0)
            {
                recipewarning(raw.id, raw.source, "recipestation references an unknown item");
                delete recipe;
                return false;
            }
        }

        if(raw.type == RECIPE_SHAPED)
        {
            const int rawheight = raw.pattern.length(), rawwidth = rawheight ? int(strlen(raw.pattern[0])) : 0;
            if(rawwidth < 1 || rawwidth > 3 || rawheight < 1 || rawheight > 3)
            {
                recipewarning(raw.id, raw.source, "shaped pattern dimensions must be between 1x1 and 3x3");
                delete recipe;
                return false;
            }
            loopv(raw.pattern) if(int(strlen(raw.pattern[i])) != rawwidth)
            {
                recipewarning(raw.id, raw.source, "all shaped pattern rows must have the same width");
                delete recipe;
                return false;
            }
            int minx = rawwidth, miny = rawheight, maxx = -1, maxy = -1;
            loopi(rawheight) loopj(rawwidth) if(raw.pattern[i][j] != ' ')
            {
                minx = min(minx, j); miny = min(miny, i); maxx = max(maxx, j); maxy = max(maxy, i);
            }
            if(maxx < minx || maxy < miny)
            {
                recipewarning(raw.id, raw.source, "shaped recipes require at least one ingredient");
                delete recipe;
                return false;
            }
            recipe->width = maxx - minx + 1;
            recipe->height = maxy - miny + 1;
            bool used[256] = { false };
            loopi(recipe->height) loopj(recipe->width)
            {
                const uchar symbol = raw.pattern[miny + i][minx + j];
                if(symbol == ' ') continue;
                rawingredient *rawingredient = findrawsymbol(raw, symbol);
                if(!rawingredient)
                {
                    recipewarning(raw.id, raw.source, "shaped pattern uses an undefined ingredient symbol");
                    delete recipe;
                    return false;
                }
                const int ingredient = compileingredient(*recipe, rawingredient->value, 1);
                if(ingredient < 0)
                {
                    recipewarning(raw.id, raw.source, "shaped pattern references an unknown item or tag");
                    delete recipe;
                    return false;
                }
                recipe->slots[i * recipe->width + j] = ingredient;
                used[symbol] = true;
            }
            loopv(raw.ingredients)
            {
                const char *key = raw.ingredients[i].key;
                if(!key[0] || key[1] || key[0] == ' ' || !used[uchar(key[0])])
                {
                    recipewarning(raw.id, raw.source, "shaped ingredient symbols must be one used, non-space character");
                    delete recipe;
                    return false;
                }
            }
        }
        else
        {
            if(!raw.pattern.empty())
            {
                recipewarning(raw.id, raw.source, "shapeless recipes cannot define a pattern");
                delete recipe;
                return false;
            }
            int total = 0;
            loopv(raw.ingredients)
            {
                if(raw.ingredients[i].count <= 0 || compileingredient(*recipe, raw.ingredients[i].key, raw.ingredients[i].count) < 0)
                {
                    recipewarning(raw.id, raw.source, "shapeless recipe references an unknown item/tag or non-positive quantity");
                    delete recipe;
                    return false;
                }
                total += raw.ingredients[i].count;
            }
            if(total <= 0)
            {
                recipewarning(raw.id, raw.source, "shapeless recipes require at least one ingredient");
                delete recipe;
                return false;
            }
        }
        recipes.add(recipe);
        return true;
    }

    static void compiletags()
    {
        loopv(rawtags)
        {
            rawtag &raw = *rawtags[i];
            if(!raw.id[0]) { recipewarning("itemtag", raw.source, "missing tag id"); continue; }
            if(finditemtag(raw.id) >= 0) { recipewarning(raw.id, raw.source, "duplicate item tag id"); continue; }
            itemtagdefinition *tag = new itemtagdefinition;
            copystring(tag->id, raw.id);
            loopj(numinventoryitems()) tag->members.add(0);
            bool valid = !raw.items.empty();
            loopvj(raw.items)
            {
                const int item = ingredientitem(raw.items[j]);
                if(item < 0)
                {
                    defformatstring(message, "item tag references unknown item %s", raw.items[j]);
                    recipewarning(raw.id, raw.source, message);
                    valid = false;
                }
                else tag->members[item] = 1;
            }
            if(!valid)
            {
                if(raw.items.empty()) recipewarning(raw.id, raw.source, "item tag must contain at least one item");
                delete tag;
            }
            else itemtags.add(tag);
        }
    }

    static int compilefurnaceingredient(furnacerecipedefinition &recipe, const char *name, int count)
    {
        recipeingredientdefinition ingredient;
        ingredient.count = count;
        if(name && name[0] == '#') ingredient.tag = finditemtag(name + 1);
        else ingredient.item = ingredientitem(name);
        if(ingredient.item < 0 && ingredient.tag < 0) return -1;
        loopv(recipe.ingredients) if(sameingredient(recipe.ingredients[i], ingredient))
        {
            recipe.ingredients[i].count += count;
            return i;
        }
        recipe.ingredients.add(ingredient);
        return recipe.ingredients.length() - 1;
    }

    static void compilefurnacefuels()
    {
        furnacefuelmillis.shrink(0);
        vector<uchar> exact;
        loopi(numinventoryitems())
        {
            furnacefuelmillis.add(0);
            exact.add(0);
        }
        loopk(2) loopv(rawfurnacefuels)
        {
            rawfurnacefuel &raw = *rawfurnacefuels[i];
            const bool istag = raw.key[0] == '#';
            if(istag != (k == 0)) continue;
            if(raw.seconds <= 0)
            {
                recipewarning(raw.key, raw.source, "furnace fuel duration must be positive");
                continue;
            }
            const double millis = double(raw.seconds) * 1000.0;
            const int duration = int(min(millis + 0.5, double(INT_MAX)));
            if(istag)
            {
                const int tag = finditemtag(raw.key + 1);
                if(tag < 0)
                {
                    recipewarning(raw.key, raw.source, "furnace fuel references an unknown item tag");
                    continue;
                }
                loopj(numinventoryitems()) if(!exact[j] && itemhastag(j, tag)) furnacefuelmillis[j] = duration;
            }
            else
            {
                const int item = ingredientitem(raw.key);
                if(item < 0)
                {
                    recipewarning(raw.key, raw.source, "furnace fuel references an unknown item");
                    continue;
                }
                furnacefuelmillis[item] = duration;
                exact[item] = 1;
            }
        }
    }

    static bool compilefurnacerecipe(rawfurnacerecipe &raw)
    {
        loopv(furnacerecipes) if(!cubecasecmp(furnacerecipes[i]->id, raw.id))
        {
            recipewarning(raw.id, raw.source, "duplicate furnace recipe id");
            return false;
        }
        if(raw.invalid) return false;
        const int output = ingredientitem(raw.output);
        if(!raw.id[0] || output < 0 || raw.outputcount <= 0 || raw.seconds <= 0)
        {
            recipewarning(raw.id, raw.source, "furnace recipe requires an id, known positive output, and positive duration");
            return false;
        }
        furnacerecipedefinition *recipe = new furnacerecipedefinition;
        copystring(recipe->id, raw.id);
        copystring(recipe->source, raw.source);
        recipe->outputitem = output;
        recipe->outputcount = raw.outputcount;
        recipe->duration = int(min(double(raw.seconds) * 1000.0 + 0.5, double(INT_MAX)));
        int total = 0;
        loopv(raw.ingredients)
        {
            if(raw.ingredients[i].count <= 0 || compilefurnaceingredient(*recipe, raw.ingredients[i].key, raw.ingredients[i].count) < 0)
            {
                recipewarning(raw.id, raw.source, "furnace recipe references an unknown item/tag or non-positive quantity");
                delete recipe;
                return false;
            }
            total += raw.ingredients[i].count;
        }
        if(total <= 0)
        {
            recipewarning(raw.id, raw.source, "furnace recipe requires at least one ingredient");
            delete recipe;
            return false;
        }
        if(recipe->ingredients.length() > FURNACE_INPUT_MAX)
        {
            recipewarning(raw.id, raw.source, "furnace recipe has more distinct ingredients than furnace input slots");
            delete recipe;
            return false;
        }
        furnacerecipes.add(recipe);
        return true;
    }

    static bool loadrecipefiles(const char *directory)
    {
        vector<char *> files;
        listfiles(directory, "cfg", files);
        files.sort();
        files.uniquedeletearrays();
        bool success = true;
        loopv(files)
        {
            formatstring(currentsource, "%s/%s.cfg", directory, files[i]);
            if(!execfile(currentsource, false))
            {
                conoutf(CON_WARN, "could not load crafting data file %s", currentsource);
                ++recipeerrors;
                success = false;
            }
        }
        files.deletecontents();
        return success;
    }

    static bool ingredientmatches(const recipeingredientdefinition &ingredient, int item)
    {
        return ingredient.item >= 0 ? ingredient.item == item : itemhastag(item, ingredient.tag);
    }

    static bool matchshaped(const recipedefinition &recipe, const int *items, const int *counts, int gridsize, bool mirrored, int crafts, craftmatch &match)
    {
        int minx = gridsize, miny = gridsize, maxx = -1, maxy = -1;
        loopi(gridsize) loopj(gridsize)
        {
            const int slot = i * gridsize + j;
            if(items[slot] < 0 || counts[slot] <= 0) continue;
            minx = min(minx, j); miny = min(miny, i); maxx = max(maxx, j); maxy = max(maxy, i);
        }
        if(maxx < minx || maxy < miny || maxx - minx + 1 != recipe.width || maxy - miny + 1 != recipe.height) return false;
        loopi(recipe.height) loopj(recipe.width)
        {
            const int gridslot = (miny + i) * gridsize + minx + j,
                      patternx = mirrored ? recipe.width - 1 - j : j,
                      ingredientindex = recipe.slots[i * recipe.width + patternx];
            if(ingredientindex < 0)
            {
                if(items[gridslot] >= 0 && counts[gridslot] > 0) return false;
            }
            else
            {
                if(items[gridslot] < 0 || counts[gridslot] < crafts || !ingredientmatches(recipe.ingredients[ingredientindex], items[gridslot])) return false;
                match.consume[gridslot] = crafts;
            }
        }
        return true;
    }

    static bool distributetag(const recipedefinition &recipe, const recipeingredientdefinition &ingredient, int group, int remaining, int groups, const int *groupitems, int *available, int *assigned, int ingredientindex, int crafts);

    static bool assigntags(const recipedefinition &recipe, int ingredientindex, int groups, const int *groupitems, int *available, int *assigned, int crafts)
    {
        while(ingredientindex < recipe.ingredients.length() && recipe.ingredients[ingredientindex].item >= 0) ++ingredientindex;
        if(ingredientindex >= recipe.ingredients.length()) return true;
        return distributetag(recipe, recipe.ingredients[ingredientindex], 0, recipe.ingredients[ingredientindex].count * crafts, groups, groupitems, available, assigned, ingredientindex, crafts);
    }

    static bool distributetag(const recipedefinition &recipe, const recipeingredientdefinition &ingredient, int group, int remaining, int groups, const int *groupitems, int *available, int *assigned, int ingredientindex, int crafts)
    {
        if(group >= groups)
            return remaining == 0 && assigntags(recipe, ingredientindex + 1, groups, groupitems, available, assigned, crafts);
        const int maximum = ingredientmatches(ingredient, groupitems[group]) ? min(remaining, available[group]) : 0;
        for(int take = maximum; take >= 0; --take)
        {
            available[group] -= take;
            assigned[ingredientindex * CRAFT_GRID_MAX + group] = take;
            if(distributetag(recipe, ingredient, group + 1, remaining - take, groups, groupitems, available, assigned, ingredientindex, crafts)) return true;
            assigned[ingredientindex * CRAFT_GRID_MAX + group] = 0;
            available[group] += take;
        }
        return false;
    }

    static bool matchshapeless(const recipedefinition &recipe, const int *items, const int *counts, int gridsize, int crafts, craftmatch &match)
    {
        int groupitems[CRAFT_GRID_MAX], available[CRAFT_GRID_MAX], assigned[CRAFT_GRID_MAX * CRAFT_GRID_MAX] = { 0 },
            groups = 0, total = 0, required = 0;
        loopv(recipe.ingredients) required += recipe.ingredients[i].count * crafts;
        loopi(gridsize * gridsize) if(items[i] >= 0 && counts[i] > 0)
        {
            total += counts[i];
            int group = -1;
            loopj(groups) if(groupitems[j] == items[i]) { group = j; break; }
            if(group < 0) { group = groups++; groupitems[group] = items[i]; available[group] = 0; }
            available[group] += counts[i];
        }
        if(total < required) return false;
        loopi(groups)
        {
            bool compatible = false;
            loopvj(recipe.ingredients) if(ingredientmatches(recipe.ingredients[j], groupitems[i])) { compatible = true; break; }
            if(!compatible) return false;
        }
        loopv(recipe.ingredients) if(recipe.ingredients[i].item >= 0)
        {
            int group = -1;
            loopj(groups) if(groupitems[j] == recipe.ingredients[i].item) { group = j; break; }
            const int ingredientcount = recipe.ingredients[i].count * crafts;
            if(group < 0 || available[group] < ingredientcount) return false;
            available[group] -= ingredientcount;
            assigned[i * CRAFT_GRID_MAX + group] = ingredientcount;
        }
        if(!assigntags(recipe, 0, groups, groupitems, available, assigned, crafts)) return false;
        int groupconsumed[CRAFT_GRID_MAX] = { 0 };
        loopv(recipe.ingredients) loopj(groups) groupconsumed[j] += assigned[i * CRAFT_GRID_MAX + j];
        loopi(gridsize * gridsize) if(items[i] >= 0 && counts[i] > 0)
        {
            int group = -1;
            loopj(groups) if(groupitems[j] == items[i]) { group = j; break; }
            if(group >= 0)
            {
                match.consume[i] = min(counts[i], groupconsumed[group]);
                groupconsumed[group] -= match.consume[i];
            }
        }
        return true;
    }

    static bool matchcompiledrecipe(const recipedefinition &recipe, const int *items, const int *counts, int gridsize, int crafts, craftmatch &match)
    {
        match = craftmatch();
        bool matched = recipe.type == RECIPE_SHAPED && recipe.width <= gridsize && recipe.height <= gridsize
                     ? matchshaped(recipe, items, counts, gridsize, false, crafts, match) : false;
        if(!matched && recipe.type == RECIPE_SHAPED && recipe.mirror)
        {
            match = craftmatch();
            matched = matchshaped(recipe, items, counts, gridsize, true, crafts, match);
        }
        if(!matched && recipe.type == RECIPE_SHAPELESS)
            matched = matchshapeless(recipe, items, counts, gridsize, crafts, match);
        return matched;
    }

    static bool distributefurnacetag(const furnacerecipedefinition &recipe, const recipeingredientdefinition &ingredient, int group,
                                     int remaining, int groups, const int *groupitems, int *available, int *assigned,
                                     int ingredientindex);

    static bool assignfurnacetags(const furnacerecipedefinition &recipe, int ingredientindex, int groups, const int *groupitems,
                                  int *available, int *assigned)
    {
        while(ingredientindex < recipe.ingredients.length() && recipe.ingredients[ingredientindex].item >= 0) ++ingredientindex;
        if(ingredientindex >= recipe.ingredients.length()) return true;
        return distributefurnacetag(recipe, recipe.ingredients[ingredientindex], 0, recipe.ingredients[ingredientindex].count, groups,
                                    groupitems, available, assigned, ingredientindex);
    }

    static bool distributefurnacetag(const furnacerecipedefinition &recipe, const recipeingredientdefinition &ingredient, int group,
                                     int remaining, int groups, const int *groupitems, int *available, int *assigned,
                                     int ingredientindex)
    {
        if(group >= groups) return remaining == 0 && assignfurnacetags(recipe, ingredientindex + 1, groups, groupitems, available, assigned);
        const int maximum = ingredientmatches(ingredient, groupitems[group]) ? min(remaining, available[group]) : 0;
        for(int take = maximum; take >= 0; --take)
        {
            available[group] -= take;
            assigned[ingredientindex * FURNACE_INPUT_MAX + group] = take;
            if(distributefurnacetag(recipe, ingredient, group + 1, remaining - take, groups, groupitems, available, assigned,
                                    ingredientindex)) return true;
            assigned[ingredientindex * FURNACE_INPUT_MAX + group] = 0;
            available[group] += take;
        }
        return false;
    }

    static bool matchcompiledfurnacerecipe(const furnacerecipedefinition &recipe, const int *items, const int *counts, int slots,
                                           furnacematch &match)
    {
        int groupitems[FURNACE_INPUT_MAX], available[FURNACE_INPUT_MAX],
            assigned[FURNACE_INPUT_MAX * FURNACE_INPUT_MAX] = { 0 }, groups = 0;
        loopi(slots) if(items[i] >= 0 && counts[i] > 0)
        {
            int group = -1;
            loopj(groups) if(groupitems[j] == items[i]) { group = j; break; }
            if(group < 0) { group = groups++; groupitems[group] = items[i]; available[group] = 0; }
            available[group] += counts[i];
        }
        loopi(groups)
        {
            bool compatible = false;
            loopvj(recipe.ingredients) if(ingredientmatches(recipe.ingredients[j], groupitems[i])) { compatible = true; break; }
            if(!compatible) return false;
        }
        loopv(recipe.ingredients) if(recipe.ingredients[i].item >= 0)
        {
            int group = -1;
            loopj(groups) if(groupitems[j] == recipe.ingredients[i].item) { group = j; break; }
            if(group < 0 || available[group] < recipe.ingredients[i].count) return false;
            available[group] -= recipe.ingredients[i].count;
            assigned[i * FURNACE_INPUT_MAX + group] = recipe.ingredients[i].count;
        }
        if(!assignfurnacetags(recipe, 0, groups, groupitems, available, assigned)) return false;
        int groupconsumed[FURNACE_INPUT_MAX] = { 0 };
        loopv(recipe.ingredients) loopj(groups) groupconsumed[j] += assigned[i * FURNACE_INPUT_MAX + j];
        loopi(slots) if(items[i] >= 0 && counts[i] > 0)
        {
            int group = -1;
            loopj(groups) if(groupitems[j] == items[i]) { group = j; break; }
            if(group >= 0)
            {
                match.consume[i] = min(counts[i], groupconsumed[group]);
                groupconsumed[group] -= match.consume[i];
            }
        }
        return true;
    }
}

int numcraftrecipes() { return recipes.length(); }
int numitemtags() { return itemtags.length(); }
const char *getcraftrecipeid(int recipe) { return recipes.inrange(recipe) ? recipes[recipe]->id : ""; }
int getcraftrecipeoutputitem(int recipe) { return recipes.inrange(recipe) ? recipes[recipe]->outputitem : -1; }
int getcraftrecipeoutputcount(int recipe) { return recipes.inrange(recipe) ? recipes[recipe]->outputcount : 0; }
int getcraftrecipeskill(int recipe) { return recipes.inrange(recipe) ? recipes[recipe]->skill : -1; }
int getcraftrecipeskilllevel(int recipe) { return recipes.inrange(recipe) ? recipes[recipe]->skilllevel : 0; }

int finditemtag(const char *id)
{
    loopv(itemtags) if(!cubecasecmp(itemtags[i]->id, id)) return i;
    return -1;
}

bool itemhastag(int item, int tag)
{
    return itemtags.inrange(tag) && itemtags[tag]->members.inrange(item) && itemtags[tag]->members[item] != 0;
}

bool matchcraftrecipe(const int *items, const int *counts, int gridsize, int stationitem, int skill, int skilllevel, int requestedrecipe, craftmatch &match, int maxoutput)
{
    match = craftmatch();
    if(!items || !counts || (gridsize != 2 && gridsize != 3)) return false;
    const int first = requestedrecipe >= 0 ? requestedrecipe : 0, end = requestedrecipe >= 0 ? requestedrecipe + 1 : recipes.length();
    for(int i = first; i < end; ++i)
    {
        if(!recipes.inrange(i)) return false;
        const recipedefinition &recipe = *recipes[i];
        if(recipe.stationitem >= 0 && recipe.stationitem != stationitem) continue;
        if(recipe.skill >= 0 && (recipe.skill != skill || skilllevel < recipe.skilllevel)) continue;
        const int outputlimit = min(max(maxoutput, 0), max(getinventoryitemmaxstack(recipe.outputitem), 1)),
                  maxcrafts = recipe.outputcount > 0 ? outputlimit / recipe.outputcount : 0;
        int low = 1, high = maxcrafts, crafts = 0;
        craftmatch candidate;
        while(low <= high)
        {
            const int attempt = low + (high - low) / 2;
            craftmatch attempted;
            if(matchcompiledrecipe(recipe, items, counts, gridsize, attempt, attempted))
            {
                crafts = attempt;
                candidate = attempted;
                low = attempt + 1;
            }
            else high = attempt - 1;
        }
        if(crafts <= 0) continue;
        candidate.recipe = i;
        candidate.outputitem = recipe.outputitem;
        candidate.outputcount = recipe.outputcount * crafts;
        match = candidate;
        return true;
    }
    return false;
}

int numfurnacerecipes() { return furnacerecipes.length(); }
const char *getfurnacerecipeid(int recipe) { return furnacerecipes.inrange(recipe) ? furnacerecipes[recipe]->id : ""; }
int getfurnacerecipeoutputitem(int recipe) { return furnacerecipes.inrange(recipe) ? furnacerecipes[recipe]->outputitem : -1; }
int getfurnacerecipeoutputcount(int recipe) { return furnacerecipes.inrange(recipe) ? furnacerecipes[recipe]->outputcount : 0; }
int getfurnacerecipeduration(int recipe) { return furnacerecipes.inrange(recipe) ? furnacerecipes[recipe]->duration : 0; }
int getfurnacefuelmillis(int item) { return furnacefuelmillis.inrange(item) ? furnacefuelmillis[item] : 0; }

int getfurnacerecipeindex(const char *id)
{
    loopv(furnacerecipes) if(!cubecasecmp(furnacerecipes[i]->id, id)) return i;
    return -1;
}

bool matchfurnacerecipe(const int *items, const int *counts, int slots, int requestedrecipe, furnacematch &match)
{
    match = furnacematch();
    if(!items || !counts || slots < 1 || slots > FURNACE_INPUT_MAX) return false;
    const int first = requestedrecipe >= 0 ? requestedrecipe : 0,
              end = requestedrecipe >= 0 ? requestedrecipe + 1 : furnacerecipes.length();
    for(int i = first; i < end; ++i)
    {
        if(!furnacerecipes.inrange(i)) return false;
        furnacematch candidate;
        if(!matchcompiledfurnacerecipe(*furnacerecipes[i], items, counts, slots, candidate)) continue;
        candidate.recipe = i;
        candidate.outputitem = furnacerecipes[i]->outputitem;
        candidate.outputcount = furnacerecipes[i]->outputcount;
        match = candidate;
        return true;
    }
    return false;
}

static bool refreshfurnacematch(furnaceinstance &furnace, furnacematch &match, bool &syncchanged)
{
    bool matched = furnace.activerecipe >= 0 &&
                   matchfurnacerecipe(furnace.inputitems, furnace.inputcounts, furnace.inputslots, furnace.activerecipe, match);
    if(!matched) matched = matchfurnacerecipe(furnace.inputitems, furnace.inputcounts, furnace.inputslots, -1, match);
    const int recipe = matched ? match.recipe : -1;
    if(recipe != furnace.activerecipe)
    {
        furnace.activerecipe = recipe;
        furnace.progress = 0;
        furnace.baking = false;
        syncchanged = true;
    }
    return matched;
}

static bool furnaceoutputfits(const furnaceinstance &furnace, const furnacematch &match)
{
    if(match.outputitem < 0 || match.outputcount <= 0) return false;
    if(furnace.outputcount > 0 && furnace.outputitem != match.outputitem) return false;
    return furnace.outputcount + match.outputcount <= max(getinventoryitemmaxstack(match.outputitem), 1);
}

bool startfurnaceinstance(furnaceinstance &furnace)
{
    if(furnace.baking) return false;
    furnacematch match;
    bool syncchanged = false;
    if(!refreshfurnacematch(furnace, match, syncchanged) || !furnaceoutputfits(furnace, match)) return false;
    if(furnace.heat <= 0 && (furnace.fuelcount <= 0 || getfurnacefuelmillis(furnace.fuelitem) <= 0)) return false;
    furnace.baking = true;
    return true;
}

bool updatefurnaceinstance(furnaceinstance &furnace, int elapsed, bool &syncchanged)
{
    syncchanged = false;
    elapsed = max(elapsed, 0);
    furnacematch match;
    bool matched = refreshfurnacematch(furnace, match, syncchanged), changed = syncchanged;
    int remaining = elapsed;
    if(!furnace.baking)
    {
        const int burned = min(furnace.heat, remaining);
        furnace.heat -= burned;
        return changed || burned > 0;
    }
    while(remaining > 0)
    {
        if(!matched || !furnaceoutputfits(furnace, match))
        {
            const int burned = min(furnace.heat, remaining);
            furnace.heat -= burned;
            changed |= burned > 0;
            break;
        }
        if(furnace.heat <= 0)
        {
            const int duration = furnace.fuelcount > 0 ? getfurnacefuelmillis(furnace.fuelitem) : 0;
            if(duration <= 0) break;
            if(--furnace.fuelcount <= 0)
            {
                furnace.fuelitem = -1;
                furnace.fuelcount = furnace.fueldurability = 0;
            }
            furnace.heat = furnace.heatcapacity = duration;
            changed = syncchanged = true;
        }
        const int duration = getfurnacerecipeduration(furnace.activerecipe);
        if(duration <= 0) break;
        const int step = min(remaining, min(furnace.heat, duration - furnace.progress));
        if(step <= 0) break;
        furnace.heat -= step;
        furnace.progress += step;
        remaining -= step;
        changed = true;
        if(furnace.progress < duration) continue;

        furnacematch completed;
        if(!matchfurnacerecipe(furnace.inputitems, furnace.inputcounts, furnace.inputslots, furnace.activerecipe, completed) ||
           !furnaceoutputfits(furnace, completed))
            break;
        loopi(FURNACE_INPUT_MAX) if(completed.consume[i] > 0)
        {
            furnace.inputcounts[i] -= completed.consume[i];
            if(furnace.inputcounts[i] <= 0)
            {
                furnace.inputitems[i] = -1;
                furnace.inputcounts[i] = furnace.inputdurabilities[i] = 0;
            }
        }
        furnace.outputitem = completed.outputitem;
        furnace.outputcount += completed.outputcount;
        furnace.outputdurability = getinventorytoolmaxdurability(completed.outputitem);
        furnace.progress = 0;
        syncchanged = true;
        matched = refreshfurnacematch(furnace, match, syncchanged);
    }
    return changed;
}

bool reloadrecipes(bool report)
{
    recipes.deletecontents();
    itemtags.deletecontents();
    furnacerecipes.deletecontents();
    furnacefuelmillis.shrink(0);
    rawrecipes.deletecontents();
    rawtags.deletecontents();
    rawfurnacefuels.deletecontents();
    rawfurnacerecipes.deletecontents();
    currentrecipe = NULL;
    currentfurnacerecipe = NULL;
    recipeerrors = 0;
    copystring(currentsource, "(console)");
    bool success = loadrecipefiles("config/game/itemtags");
    success = loadrecipefiles("config/game/recipes") && success;
    success = loadrecipefiles("config/game/furnaces") && success;
    compiletags();
    loopv(rawrecipes) compilerecipe(*rawrecipes[i]);
    compilefurnacefuels();
    loopv(rawfurnacerecipes) compilefurnacerecipe(*rawfurnacerecipes[i]);
    if(report)
    {
        conoutf("Loaded %d recipes", recipes.length());
        conoutf("Loaded %d item tags", itemtags.length());
        conoutf("Loaded %d furnace recipes and %d fuels", furnacerecipes.length(), furnacefuelmillis.length());
        conoutf("%d recipe errors", recipeerrors);
    }
    return success && recipeerrors == 0;
}

static void reloadrecipescommand() { reloadrecipes(true); }
COMMANDN(reloadrecipes, reloadrecipescommand, "");

ICOMMAND(itemtag, "ss", (char *id, char *items),
{
    rawtag *tag = rawtags.add(new rawtag(id, currentsource));
    explodelist(items, tag->items);
});

ICOMMAND(craftrecipe, "se", (char *id, uint *body),
{
    if(currentrecipe)
    {
        recipewarning(id, currentsource, "nested craftrecipe blocks are not allowed");
        return;
    }
    currentrecipe = rawrecipes.add(new rawrecipe(id, currentsource));
    execute(body);
    currentrecipe = NULL;
});

ICOMMAND(furnacefuel, "sf", (char *itemortag, float *seconds),
{
    rawfurnacefuels.add(new rawfurnacefuel(itemortag, *seconds, currentsource));
});

ICOMMAND(furnacerecipe, "se", (char *id, uint *body),
{
    if(currentfurnacerecipe)
    {
        recipewarning(id, currentsource, "nested furnacerecipe blocks are not allowed");
        return;
    }
    currentfurnacerecipe = rawfurnacerecipes.add(new rawfurnacerecipe(id, currentsource));
    execute(body);
    currentfurnacerecipe = NULL;
});

ICOMMAND(furnaceoutput, "si", (char *item, int *count),
{
    if(!currentfurnacerecipe) return;
    copystring(currentfurnacerecipe->output, item);
    currentfurnacerecipe->outputcount = *count;
});

ICOMMAND(furnacetime, "f", (float *seconds),
{
    if(currentfurnacerecipe) currentfurnacerecipe->seconds = *seconds;
});

ICOMMAND(furnaceingredient, "si", (char *itemortag, int *count),
{
    if(currentfurnacerecipe) currentfurnacerecipe->ingredients.add(rawingredient(itemortag, "", *count));
});

ICOMMAND(recipetype, "s", (char *type),
{
    if(!currentrecipe) return;
    currentrecipe->type = !cubecasecmp(type, "shaped") ? RECIPE_SHAPED : !cubecasecmp(type, "shapeless") ? RECIPE_SHAPELESS : RECIPE_UNSET;
});

ICOMMAND(recipeoutput, "si", (char *item, int *count),
{
    if(!currentrecipe) return;
    copystring(currentrecipe->output, item);
    currentrecipe->outputcount = *count;
});

ICOMMAND(recipepattern, "s", (char *pattern),
{
    if(!currentrecipe) return;
    currentrecipe->pattern.deletecontents();
    explodelist(pattern, currentrecipe->pattern);
});

ICOMMAND(recipeingredient, "sTN", (char *first, tagval *second, int *numargs),
{
    if(!currentrecipe) return;
    if(currentrecipe->type == RECIPE_SHAPED)
    {
        if(*numargs < 2) currentrecipe->invalid = true;
        else currentrecipe->ingredients.add(rawingredient(first, second->getstr(), 1));
    }
    else
    {
        const int count = *numargs >= 2 ? second->getint() : 1;
        currentrecipe->ingredients.add(rawingredient(first, "", count));
    }
});

ICOMMAND(recipestation, "s", (char *station), if(currentrecipe) copystring(currentrecipe->station, station));
ICOMMAND(recipeskill, "si", (char *skill, int *level),
{
    if(!currentrecipe) return;
    copystring(currentrecipe->skill, skill);
    currentrecipe->skilllevel = *level;
});
ICOMMAND(recipemirror, "i", (int *enabled), if(currentrecipe) currentrecipe->mirror = *enabled != 0);
