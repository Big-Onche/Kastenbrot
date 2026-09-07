#include <atomic>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include "cube.h"
#undef main

uint randomMT()
{
    return 0;
}

static Uint64 testticks = 0;
static bool failthread = false;
static std::atomic<bool> workerpaused(false), workerentered(false);

static Uint64 testcounter()
{
    return ++testticks;
}

static Uint64 testfrequency()
{
    return 1000000;
}

static SDL_Thread *testcreatethread(SDL_ThreadFunction function, const char *name, void *argument)
{
    return failthread ? NULL : SDL_CreateThread(function, name, argument);
}

static void testwaitworker()
{
    workerentered = true;
    while(workerpaused.load()) SDL_Delay(1);
}

#undef SDL_CreateThread
#define SDL_CreateThread testcreatethread
#define SDL_GetPerformanceCounter testcounter
#define SDL_GetPerformanceFrequency testfrequency

namespace supporttest
{
    enum { CREATIVE_GRID = 16 };
    static int playerstorage, *player1 = &playerstorage;
    static int totalmillis = 0, localsupportlasttick = 0, localsupportlastdistance = -1, supportdecaymillis = 3000;
    static ivec localsupportlastsection(INT_MIN, INT_MIN, INT_MIN);
    static float supportupdatebudget = 1;
    static bool inrange = true;
    static int samplecost = 0, scancost = 0;
    static int world[32][32][32];

    static bool waitforserveredit()
    {
        return false;
    }

    static bool islocalworld()
    {
        return true;
    }

    static bool localsupportinrange(const ivec &cell)
    {
        return inrange;
    }

    static int getworldcubesupportdistance(int index)
    {
        return index == 1 ? 3 : 0;
    }

    static bool localblockworldindex(const ivec &cell, int &index)
    {
        testticks += samplecost;
        const ivec block = ivec(cell).div(CREATIVE_GRID);
        index = block.x >= 0 && block.y >= 0 && block.z >= 0 && block.x < 32 && block.y < 32 && block.z < 32
              ? world[block.x][block.y][block.z] : -1;
        if(index != -2) return true;
        index = -1;
        return false;
    }

    static void discoverlocalsupportblocks(Uint64 deadline)
    {
        testticks += scancost;
    }

    static void randomticklocalsupportblock(const ivec &cell)
    {
        assert(false); // These tests must never reach the separately scheduled decay tick.
    }

    #include "support-production.h"

    static void waitbatch()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while(localsupportbatchpending && !supportbatchcomplete(*localsupportbatchpending))
        {
            assert(std::chrono::steady_clock::now() < deadline);
            SDL_Delay(1);
        }
    }

    static void reset()
    {
        workerpaused = false;
        resetlocalsupportblocks();
        workerentered = false;
        failthread = false;
        inrange = true;
        testticks = samplecost = scancost = 0;
        loopi(32) loopj(32) loopk(32) world[i][j][k] = -1;
    }

    static void testasync()
    {
        reset();
        const ivec cell(128, 128, 128);
        world[8][8][8] = 1;
        world[8][8][7] = 2;
        queuesupportcheck(cell, 0);
        workerpaused = true;
        updatesupportblocks();
        assert(localsupportbatchpending && localsupportbatchpending->count == 1);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while(!workerentered)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            SDL_Delay(1);
        }
        loopi(10) updatesupportblocks();
        assert(localsupportchecks.size() == 1 && !localsupportcells.access(cell));
        queuesupportcheck(cell, 3);
        assert(localsupportchecks.size() == 1 && localsupportpending[cell] == 3);
        world[8][8][7] = -1;
        workerpaused = false;
        waitbatch();
        updatesupportblocks();
        assert(!localsupportbatchpending && localsupportchecks.size() == 1 && !localsupportcells.access(cell));
        updatesupportblocks();
        waitbatch();
        updatesupportblocks();
        assert(localsupportcells.access(cell) && localsupportcells.access(cell)->distance == 0);
        assert(localsupportchecks.size() == 6); // Increased reach propagated after the deferred commit.
        reset();
    }

    static void testinvalidations()
    {
        const ivec cell(128, 128, 128);
        for(int change = 0; change < 3; ++change)
        {
            reset();
            world[8][8][8] = 1;
            queuesupportcheck(cell, 0);
            updatesupportblocks();
            waitbatch();
            if(change == 0) world[8][8][7] = 2; // A previously empty cell becomes an anchor.
            if(change == 1) world[8][8][7] = -2; // A consulted neighbour unloads.
            if(change == 2) world[8][8][8] = 2; // Target definition changes.
            updatesupportblocks();
            assert(!localsupportbatchpending && localsupportchecks.size() == 1 && !localsupportcells.access(cell));
        }
        reset();
        world[8][8][8] = 1;
        queuesupportcheck(cell, 0);
        updatesupportblocks();
        waitbatch();
        inrange = false;
        updatesupportblocks();
        assert(localsupportchecks.size() == 1 && !localsupportcells.access(cell));
        reset();
    }

    static void testbudgetandfallback()
    {
        reset();
        samplecost = 1200; // One live query already exhausts a 1 ms allowance.
        loopi(100) queuesupportcheck(ivec(i * 16, 0, 0), 0);
        updatesupportblocks();
        assert(localsupportbatchpending->count == 1 && localsupportchecks.size() == 100);
        samplecost = 0;
        for(int frame = 0; !localsupportchecks.empty() && frame < 250; ++frame)
        {
            waitbatch();
            updatesupportblocks();
        }
        assert(localsupportchecks.empty() && localsupportpending.empty());
        reset();
        failthread = true;
        world[8][8][8] = 1;
        world[8][8][7] = 2;
        const ivec cell(128, 128, 128);
        queuesupportcheck(cell, 0);
        updatesupportblocks();
        assert(!localsupportworker && localsupportbatchpending && supportbatchcomplete(*localsupportbatchpending));
        updatesupportblocks();
        assert(localsupportchecks.empty() && localsupportcells.access(cell)->distance == 1);
        reset();
    }

    static void testshutdown()
    {
        loopi(20)
        {
            reset();
            loopj(16)
            {
                world[j + 4][8][8] = 1;
                queuesupportcheck(ivec((j + 4) * 16, 128, 128), 0);
            }
            updatesupportblocks();
            // Shutdown with both queued and potentially active jobs.
            resetlocalsupportblocks();
            assert(!localsupportworker && !localsupportbatchpending && localsupportjobs.empty() && localsupportchecks.empty());
        }
    }
}

namespace watertest
{
    struct fluidcell
    {
        int update;
        bool queued;

        fluidcell() : update(0), queued(true)
        {
        }
    };
    static vector<ivec> fluidupdates;
    static hashtable<ivec, fluidcell> fluidcells;
    static int fluidupdatecursor = 0, fluidupdatespertick = 1024, authoritativewaterupdates = 1024, totalmillis = 0;
    static bool authoritativewatersettings = false;
    static float fluidupdatebudget = 1;
    static std::unordered_set<int> visited;

    static bool waterinsimulationrange(const ivec &position)
    {
        return position.x >= 0;
    }

    static void updatewatercell(const ivec &position)
    {
        testticks += 600;
        assert(visited.insert(position.x).second);
    }

    #include "water-production.h"

    static void testbudget()
    {
        loopi(20)
        {
            const ivec cell(i, 0, 0);
            fluidupdates.add(cell);
            fluidcells.access(cell, fluidcell());
        }
        updatewatersimulation();
        assert(visited.size() == 2 && fluidupdates.length() == 18);
        loopi(9) updatewatersimulation();
        assert(visited.size() == 20 && fluidupdates.empty());
        loopi(1000) fluidupdates.add(ivec(i + 1000, 0, 0)); // Stale work counts against inspection admission.
        fluidupdatespertick = 1;
        updatewatersimulation();
        assert(fluidupdates.length() == 744);
        fluidupdates.setsize(0);
        const ivec delayed(99, 0, 0), outside(-1, 0, 0);
        fluidcells.access(delayed, fluidcell()).update = 100;
        fluidcells.access(outside, fluidcell());
        fluidupdates.add(delayed);
        fluidupdates.add(outside);
        updatewatersimulation();
        assert(fluidupdates.length() == 2);
        totalmillis = 100;
        updatewatersimulation();
        assert(visited.count(99) && fluidupdates.length() == 1 && fluidupdates[0] == outside);
    }
}

int vatilesize = 64;
#include "streaminggeometry.h"

static void testmeshqueue()
{
    const int section = 256, worldsize = 2048;
    const ivec origin(768, 768, 768), maximum = ivec(origin).add(section);
    for(int size = 16; size <= 256; size *= 2)
    {
        vatilesize = size;
        streaminggeometryqueue queue;
        queue.changed(origin, maximum, section, worldsize);
        const int tiles = section / size, expected = tiles * tiles * tiles + 6 * tiles * tiles;
        assert(queue.length() == expected && queue.pending(origin));
        queue.changed(origin, maximum, section, worldsize);
        assert(queue.length() == expected);
        std::unordered_set<int> seen;
        while(queue.length())
        {
            const ivec cell = queue.pop(section);
            assert(!(cell.x % size) && !(cell.y % size) && !(cell.z % size));
            assert(seen.insert(cell.x + worldsize * (cell.y + worldsize * (cell.z / size))).second);
        }
        assert(!queue.pending(origin) && int(seen.size()) == expected);
        queue.changed(origin, maximum, section, worldsize);
        queue.pop(section);
        queue.clear();
        assert(!queue.length() && !queue.pending(origin));
    }
}

int main()
{
    supporttest::testasync();
    supporttest::testinvalidations();
    supporttest::testbudgetandfallback();
    supporttest::testshutdown();
    watertest::testbudget();
    testmeshqueue();
    puts("PASS: asynchronous support, snapshot invalidation, budget continuation, worker failure/shutdown, water and mesh queues");
    return 0;
}
