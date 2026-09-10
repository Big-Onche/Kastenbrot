#ifndef GAME_NPCDEF_H
#define GAME_NPCDEF_H

// NPC registry and block configuration for client and dedicated server.
namespace game
{
    static vector<npcdefinition *> npcdefinitions;

    npcdefinition *findnpcdefinition(const char *id)
    {
        loopv(npcdefinitions) if(!cubecasecmp(npcdefinitions[i]->id, id)) return npcdefinitions[i];
        return NULL;
    }

    int numnpcdefinitions()
    {
        return npcdefinitions.length();
    }

    npcdefinition *getnpcdefinition(int index)
    {
        return npcdefinitions.inrange(index) ? npcdefinitions[index] : NULL;
    }

    static int parseattitude(const char *name)
    {
        if(!cubecasecmp(name, "aggressive")) return NPC_AGGRESSIVE;
        if(!cubecasecmp(name, "neutral")) return NPC_NEUTRAL;
        if(!cubecasecmp(name, "friendly")) return NPC_FRIENDLY;
        if(!cubecasecmp(name, "scared")) return NPC_SCARED;
        return -1;
    }

    static int parsebehavior(const char *name)
    {
        if(!cubecasecmp(name, "wandering") || !cubecasecmp(name, "wander")) return NPC_WANDERING;
        if(!cubecasecmp(name, "chase")) return NPC_CHASE;
        if(!cubecasecmp(name, "flee")) return NPC_FLEE;
        return -1;
    }

    static int parsemodeltype(const char *name)
    {
        if(!cubecasecmp(name, "humanoid")) return NPC_MODEL_HUMANOID;
        if(!cubecasecmp(name, "quadruped")) return NPC_MODEL_QUADRUPED;
        return -1;
    }

    static int parsebiome(const char *name)
    {
        if(!cubecasecmp(name, "plains")) return WORLD_BIOME_PLAINS;
        if(!cubecasecmp(name, "forest")) return WORLD_BIOME_FOREST;
        if(!cubecasecmp(name, "desert")) return WORLD_BIOME_DESERT;
        if(!cubecasecmp(name, "snow")) return WORLD_BIOME_SNOW;
        return -1;
    }

    enum { NPCDEF_ROOT, NPCDEF_MODEL, NPCDEF_NATURAL, NPCDEF_CAVES, NPCDEF_HITFLEE, NPCDEF_WANDERSOUND };
    static npcdefinition *currentnpcdefinition = NULL;
    static int currentnpccomponent = NPCDEF_ROOT;
    static bool npcdefinitionfailed = false;
    static uint npcdefinitioncomponents = 0;
    static string soundprefix;
    static int soundvariants, soundminimum, soundmaximum;

    static void npcdefinitionerror(const char *message)
    {
        conoutf(CON_ERROR, "npcdef \"%s\": %s", currentnpcdefinition ? currentnpcdefinition->id : "<none>", message);
        npcdefinitionfailed = true;
    }

    static const char *npcdefinitioncommand(const char *command, int component)
    {
        if(component == NPCDEF_ROOT)
        {
            if(!strcmp(command, "name")) return "npcdef_root_name";
            if(!strcmp(command, "attitude")) return "npcdef_root_attitude";
            if(!strcmp(command, "behavior")) return "npcdef_root_behavior";
            if(!strcmp(command, "health")) return "npcdef_root_health";
            if(!strcmp(command, "damage")) return "npcdef_root_damage";
            if(!strcmp(command, "speed")) return "npcdef_root_speed";
            if(!strcmp(command, "attackmillis")) return "npcdef_root_attackmillis";
            if(!strcmp(command, "wanderradius")) return "npcdef_root_wanderradius";
            if(!strcmp(command, "aggrodist")) return "npcdef_root_aggrodist";
            if(!strcmp(command, "fleedist")) return "npcdef_root_fleedist";
            if(!strcmp(command, "model")) return "npcdef_root_model";
            if(!strcmp(command, "natural")) return "npcdef_root_natural";
            if(!strcmp(command, "caves")) return "npcdef_root_caves";
            if(!strcmp(command, "hitflee")) return "npcdef_root_hitflee";
            if(!strcmp(command, "wandersound")) return "npcdef_root_wandersound";
            if(!strcmp(command, "drop")) return "npcdef_root_drop";
        }
        if(component == NPCDEF_MODEL)
        {
            if(!strcmp(command, "path")) return "npcdef_model_path";
            if(!strcmp(command, "type")) return "npcdef_model_type";
            if(!strcmp(command, "radius")) return "npcdef_model_radius";
            if(!strcmp(command, "height")) return "npcdef_model_height";
            if(!strcmp(command, "rootheight")) return "npcdef_model_rootheight";
        }
        if(component == NPCDEF_NATURAL)
        {
            if(!strcmp(command, "biome")) return "npcdef_natural_biome";
            if(!strcmp(command, "chance")) return "npcdef_natural_chance";
            if(!strcmp(command, "group")) return "npcdef_natural_group";
        }
        if(component == NPCDEF_CAVES)
        {
            if(!strcmp(command, "bands")) return "npcdef_caves_bands";
            if(!strcmp(command, "group")) return "npcdef_caves_group";
        }
        if(component == NPCDEF_HITFLEE)
        {
            if(!strcmp(command, "speed")) return "npcdef_hitflee_speed";
            if(!strcmp(command, "herdradius")) return "npcdef_hitflee_herdradius";
            if(!strcmp(command, "duration")) return "npcdef_hitflee_duration";
        }
        if(component == NPCDEF_WANDERSOUND)
        {
            if(!strcmp(command, "prefix")) return "npcdef_wandersound_prefix";
            if(!strcmp(command, "variants")) return "npcdef_wandersound_variants";
            if(!strcmp(command, "interval")) return "npcdef_wandersound_interval";
        }
        return NULL;
    }

    static void executenpcdefinitionbody(const char *body, int component)
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
                const char *replacement = npcdefinitioncommand(command, component);
                if(!replacement)
                {
                    defformatstring(message, "unknown %s command \"%s\"", component == NPCDEF_ROOT ? "npcdef" : "component", command);
                    npcdefinitionerror(message);
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

    ICOMMAND(npcdef_root_model, "S", (char *body),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT || (npcdefinitioncomponents & (1U << NPCDEF_MODEL)))
        {
            npcdefinitionerror("model must occur once, directly inside npcdef");
            return;
        }
        npcdefinitioncomponents |= 1U << NPCDEF_MODEL;
        currentnpccomponent = NPCDEF_MODEL;
        executenpcdefinitionbody(body, currentnpccomponent);
        currentnpccomponent = NPCDEF_ROOT;
    });

    ICOMMAND(npcdef_root_natural, "S", (char *body),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT || (npcdefinitioncomponents & (1U << NPCDEF_NATURAL)))
        {
            npcdefinitionerror("natural must occur once, directly inside npcdef");
            return;
        }
        npcdefinitioncomponents |= 1U << NPCDEF_NATURAL;
        currentnpccomponent = NPCDEF_NATURAL;
        executenpcdefinitionbody(body, currentnpccomponent);
        currentnpccomponent = NPCDEF_ROOT;
    });

    ICOMMAND(npcdef_root_caves, "S", (char *body),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT || (npcdefinitioncomponents & (1U << NPCDEF_CAVES)))
        {
            npcdefinitionerror("caves must occur once, directly inside npcdef");
            return;
        }
        npcdefinitioncomponents |= 1U << NPCDEF_CAVES;
        currentnpccomponent = NPCDEF_CAVES;
        executenpcdefinitionbody(body, currentnpccomponent);
        currentnpccomponent = NPCDEF_ROOT;
    });

    ICOMMAND(npcdef_root_hitflee, "S", (char *body),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT || (npcdefinitioncomponents & (1U << NPCDEF_HITFLEE)))
        {
            npcdefinitionerror("hitflee must occur once, directly inside npcdef");
            return;
        }
        npcdefinitioncomponents |= 1U << NPCDEF_HITFLEE;
        currentnpccomponent = NPCDEF_HITFLEE;
        executenpcdefinitionbody(body, currentnpccomponent);
        currentnpccomponent = NPCDEF_ROOT;
    });

    ICOMMAND(npcdef_root_wandersound, "S", (char *body),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT || (npcdefinitioncomponents & (1U << NPCDEF_WANDERSOUND)))
        {
            npcdefinitionerror("wandersound must occur once, directly inside npcdef");
            return;
        }
        npcdefinitioncomponents |= 1U << NPCDEF_WANDERSOUND;
        currentnpccomponent = NPCDEF_WANDERSOUND;
        executenpcdefinitionbody(body, currentnpccomponent);
        currentnpccomponent = NPCDEF_ROOT;
    });

    ICOMMAND(npcdef_root_name, "s", (char *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("name outside its component"); return; }
        copystring(currentnpcdefinition->name, value);
    });
    ICOMMAND(npcdef_root_attitude, "s", (char *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("attitude outside its component"); return; }
        currentnpcdefinition->attitude = parseattitude(value);
    });
    ICOMMAND(npcdef_root_behavior, "s", (char *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("behavior outside its component"); return; }
        currentnpcdefinition->behavior = parsebehavior(value);
    });
    ICOMMAND(npcdef_root_health, "i", (int *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("health outside its component"); return; }
        currentnpcdefinition->health = *value;
    });
    ICOMMAND(npcdef_root_damage, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("damage outside its component"); return; }
        currentnpcdefinition->damage = *value;
    });
    ICOMMAND(npcdef_root_speed, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("speed outside its component"); return; }
        currentnpcdefinition->speed = *value;
    });
    ICOMMAND(npcdef_root_attackmillis, "i", (int *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("attackmillis outside its component"); return; }
        currentnpcdefinition->attackmillis = *value;
    });
    ICOMMAND(npcdef_root_wanderradius, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("wanderradius outside its component"); return; }
        currentnpcdefinition->wanderradius = *value;
    });
    ICOMMAND(npcdef_root_aggrodist, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("aggrodist outside its component"); return; }
        currentnpcdefinition->aggrodist = *value;
    });
    ICOMMAND(npcdef_root_fleedist, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("fleedist outside its component"); return; }
        currentnpcdefinition->fleedist = *value;
    });
    ICOMMAND(npcdef_model_path, "s", (char *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_MODEL) { npcdefinitionerror("path outside its component"); return; }
        copystring(currentnpcdefinition->model, value);
    });
    ICOMMAND(npcdef_model_type, "s", (char *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_MODEL) { npcdefinitionerror("type outside its component"); return; }
        currentnpcdefinition->modeltype = parsemodeltype(value);
    });
    ICOMMAND(npcdef_model_radius, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_MODEL) { npcdefinitionerror("radius outside its component"); return; }
        currentnpcdefinition->radius = *value;
    });
    ICOMMAND(npcdef_model_height, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_MODEL) { npcdefinitionerror("height outside its component"); return; }
        currentnpcdefinition->height = *value;
    });
    ICOMMAND(npcdef_model_rootheight, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_MODEL) { npcdefinitionerror("rootheight outside its component"); return; }
        currentnpcdefinition->rootheight = *value;
    });
    ICOMMAND(npcdef_natural_biome, "s", (char *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_NATURAL) { npcdefinitionerror("biome outside its component"); return; }
        currentnpcdefinition->naturalbiome = parsebiome(value);
    });
    ICOMMAND(npcdef_natural_chance, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_NATURAL) { npcdefinitionerror("chance outside its component"); return; }
        currentnpcdefinition->spawnchance = *value;
    });
    ICOMMAND(npcdef_caves_bands, "i", (int *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_CAVES) { npcdefinitionerror("bands outside its component"); return; }
        currentnpcdefinition->cavebands = *value;
    });
    ICOMMAND(npcdef_hitflee_speed, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_HITFLEE) { npcdefinitionerror("speed outside its component"); return; }
        currentnpcdefinition->fleespeed = *value;
    });
    ICOMMAND(npcdef_hitflee_herdradius, "f", (float *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_HITFLEE) { npcdefinitionerror("herdradius outside its component"); return; }
        currentnpcdefinition->herdradius = *value;
    });
    ICOMMAND(npcdef_hitflee_duration, "i", (int *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_HITFLEE) { npcdefinitionerror("duration outside its component"); return; }
        currentnpcdefinition->fleeonhitmillis = *value;
    });
    ICOMMAND(npcdef_wandersound_prefix, "s", (char *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_WANDERSOUND) { npcdefinitionerror("prefix outside its component"); return; }
        copystring(soundprefix, value);
    });
    ICOMMAND(npcdef_wandersound_variants, "i", (int *value),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_WANDERSOUND) { npcdefinitionerror("variants outside its component"); return; }
        soundvariants = *value;
    });
    ICOMMAND(npcdef_natural_group, "ii", (int *minimum, int *maximum),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_NATURAL) { npcdefinitionerror("group outside its component"); return; }
        currentnpcdefinition->groupmin = *minimum;
        currentnpcdefinition->groupmax = *maximum;
    });
    ICOMMAND(npcdef_caves_group, "ii", (int *minimum, int *maximum),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_CAVES) { npcdefinitionerror("group outside its component"); return; }
        currentnpcdefinition->cavegroupmin = *minimum;
        currentnpcdefinition->cavegroupmax = *maximum;
    });
    ICOMMAND(npcdef_wandersound_interval, "ii", (int *minimum, int *maximum),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_WANDERSOUND) { npcdefinitionerror("interval outside its component"); return; }
        soundminimum = *minimum;
        soundmaximum = *maximum;
    });

    ICOMMAND(npcdef_root_drop, "siifN", (char *itemid, int *minimum, int *maximum, float *chance, int *numargs),
    {
        if(!currentnpcdefinition || currentnpccomponent != NPCDEF_ROOT) { npcdefinitionerror("drop outside npcdef"); return; }
        const float probability = *numargs >= 4 ? *chance : 1.0f;
        if(!itemid[0] || *minimum <= 0 || *maximum < *minimum || !(probability > 0 && probability <= 1))
        {
            npcdefinitionerror("invalid drop item, count range, or chance");
            return;
        }
        currentnpcdefinition->drops.add(npcdropdefinition(itemid, *minimum, *maximum, probability));
    });

    ICOMMAND(npcdef, "sS", (char *id, char *body),
    {
        if(currentnpcdefinition) { npcdefinitionerror("nested npcdef blocks are not allowed"); return; }
        if(!id[0] || strlen(id) >= sizeof(string) || findnpcdefinition(id))
        {
            conoutf(CON_ERROR, "duplicate, empty, or oversized NPC id: %s", id);
            return;
        }
        npcdefinition definition(id);
        currentnpcdefinition = &definition;
        currentnpccomponent = NPCDEF_ROOT;
        npcdefinitionfailed = false;
        npcdefinitioncomponents = 0;
        soundprefix[0] = '\0';
        soundvariants = soundminimum = soundmaximum = 0;
        executenpcdefinitionbody(body, NPCDEF_ROOT);
        if(!definition.name[0]) copystring(definition.name, id);
        if(!definition.model[0] || definition.attitude < 0 || definition.behavior < 0 || definition.health <= 0 ||
           !(definition.damage >= 0 && definition.speed > 0 && definition.wanderradius >= 0 && definition.aggrodist >= 0 &&
             definition.fleedist >= 0) || definition.attackmillis <= 0)
            npcdefinitionerror("invalid name, model path, attitude, behavior, or stats");
        if(definition.modeltype < 0 || !(definition.radius > 0 && definition.height > 0 && definition.rootheight > 0 &&
                                       definition.rootheight < definition.height))
            npcdefinitionerror("invalid model type or dimensions");
        if((npcdefinitioncomponents & (1U << NPCDEF_NATURAL)) &&
           (definition.naturalbiome < 0 || definition.groupmin <= 0 || definition.groupmax < definition.groupmin || definition.groupmax > 16 ||
            !(definition.spawnchance > 0 && definition.spawnchance <= 1)))
            npcdefinitionerror("natural spawning requires a biome, group range within 1-16, and chance in (0, 1]");
        if((npcdefinitioncomponents & (1U << NPCDEF_CAVES)) &&
           (definition.attitude != NPC_AGGRESSIVE || definition.cavebands < 0 || definition.cavebands > NPC_WORLD_HEIGHT_BLOCKS ||
            definition.cavegroupmin < 1 || definition.cavegroupmax < definition.cavegroupmin || definition.cavegroupmax > 4))
            npcdefinitionerror("caves requires an aggressive NPC, 0-512 bands, and group range within 1-4");
        if((npcdefinitioncomponents & (1U << NPCDEF_HITFLEE)) &&
           (!(definition.fleespeed >= 1 && definition.herdradius >= 0) || definition.fleeonhitmillis <= 0))
            npcdefinitionerror("hitflee requires speed >= 1, herdradius >= 0, and duration > 0");
        if(npcdefinitioncomponents & (1U << NPCDEF_WANDERSOUND))
        {
            if(!soundprefix[0] || strlen(soundprefix) + 2 >= sizeof(string) || soundvariants < 1 || soundvariants > 64 ||
               soundminimum < 1 || soundmaximum < soundminimum || soundmaximum > 3600)
                npcdefinitionerror("wandersound requires a prefix, 1-64 variants, and interval within 1-3600 seconds");
            else
            {
                loopi(soundvariants)
                {
                    defformatstring(sample, "%s%d", soundprefix, i + 1);
                    definition.wandersounds.add(npcwandersounddefinition(sample, soundminimum * 1000, soundmaximum * 1000));
                }
                ++definition.wandersoundrevision;
            }
        }
        currentnpcdefinition = NULL;
        currentnpccomponent = NPCDEF_ROOT;
        if(!npcdefinitionfailed) *npcdefinitions.add(new npcdefinition(id)) = definition;
    });
}

#endif
