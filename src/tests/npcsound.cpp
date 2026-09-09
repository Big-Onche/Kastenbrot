// g++ -std=c++11 -O2 -Wall src/tests/npcsound.cpp -o bin64/npcsound-tests.exe
#include "../game/npcsound.h"
#include <cassert>
#include <cstdio>

struct voices
{
    struct sound { int minmillis, maxmillis; } sounds[3];

    voices(int minimum, int maximum)
    {
        for(int i = 0; i < 3; ++i) { sounds[i].minmillis = minimum; sounds[i].maxmillis = maximum; }
    }

    int length() const
    {
        return 3;
    }

    bool empty() const
    {
        return false;
    }

    const sound &operator[](int i) const
    {
        assert(i >= 0 && i < length());
        return sounds[i];
    }
};

static void intervals(const voices &sounds)
{
    npcvoice::schedule a, b;
    assert(a.update(0, 12345, sounds) == -1);
    assert(b.update(0, 12345, sounds) == -1);
    unsigned long long previous = 0;
    int counts[3] = { 0, 0, 0 };
    bool varied = false;
    const unsigned long long first = a.next;
    for(int i = 0; i < 10000; ++i)
    {
        assert(a.next == b.next && a.variant == b.variant);
        const unsigned long long due = a.next, delay = due - previous;
        assert(delay >= static_cast<unsigned int>(sounds[a.variant].minmillis));
        assert(delay <= static_cast<unsigned int>(sounds[a.variant].maxmillis));
        varied |= delay != first;
        const int choice = a.variant;
        assert(a.update(static_cast<unsigned int>(due - 1), 12345, sounds) == -1);
        assert(a.update(static_cast<unsigned int>(due), 12345, sounds) == choice);
        assert(b.update(static_cast<unsigned int>(due), 12345, sounds) == choice);
        assert(a.update(static_cast<unsigned int>(due), 12345, sounds) == -1);
        ++counts[choice];
        previous = due;
    }
    assert(varied);
    for(int i = 0; i < 3; ++i) assert(counts[i] > 2500 && counts[i] < 4200);
}

int main()
{
    const voices cows(10000, 20000), zombies(15000, 40000);
    intervals(cows);
    intervals(zombies);

    npcvoice::schedule fast, slow, other;
    fast.update(0, 7, cows);
    slow.update(0, 7, cows);
    other.update(0, 8, cows);
    assert(fast.next != other.next || fast.variant != other.variant);
    int fastcalls = 0, slowcalls = 0;
    for(unsigned int now = 1; now <= 1000000; ++now)
    {
        if(now % 16 == 0 && fast.update(now, 7, cows) >= 0) ++fastcalls;
        if(now % 40 == 0 && slow.update(now, 7, cows) >= 0) ++slowcalls;
        other.update(now, 8, cows); // Unrelated mobs never perturb either stream.
    }
    assert(fast.next == slow.next && fast.serial == slow.serial && fastcalls == slowcalls);

    npcvoice::schedule stalled;
    stalled.update(0, 7, cows);
    assert(stalled.update(1000000, 7, cows) == -1); // Skip stale calls, never replay a backlog.
    assert(stalled.next == fast.next && stalled.serial == fast.serial);
    assert(stalled.update(1000000, 7, cows) == -1);
    assert(stalled.update(0, 7, cows) == -1); // Map clock reset restarts the same deterministic sequence.
    npcvoice::schedule reset;
    reset.update(0, 7, cows);
    assert(stalled.next == reset.next && stalled.variant == reset.variant);

    voices fixed(10000, 10000);
    npcvoice::schedule exact;
    exact.update(0, 1, fixed);
    for(unsigned int due = 10000; due <= 100000; due += 10000)
    {
        assert(exact.next == due);
        assert(exact.update(due, 1, fixed) >= 0);
    }
    std::puts("NPC sound tests passed: interval bounds, variants, independent seeds, frame rates, missed events, and clock reset.");
    return 0;
}
