#ifndef ENGINE_RENDERBATCH_H
#define ENGINE_RENDERBATCH_H

#include <algorithm>

// Collect draw records in O(1), sort their indices once in O(n log n), then
// link equal render states in one linear pass. Sorting indices also supports
// records containing references, which cannot be assigned by std::sort.
struct renderbatchorder
{
    vector<int> order;
    int first, count;
    bool sorted;

    renderbatchorder() : first(-1), count(0), sorted(false)
    {
    }

    void clear()
    {
        order.setsize(0);
        first = -1;
        count = 0;
        sorted = false;
    }

    template<class T> void sort(vector<T> &batches)
    {
        if(sorted) return;
        clear();
        sorted = true;
        if(batches.empty()) return;
        order.reserve(batches.length());
        loopv(batches) order.add(i);
        std::sort(order.getbuf(), order.getbuf() + order.length(), [&batches](int a, int b)
        {
            const int result = batches[a].compare(batches[b]);
            return result < 0 || (!result && a < b);
        });
        int head = -1, tail = -1;
        loopv(order)
        {
            const int index = order[i];
            T &b = batches[index];
            b.next = b.batch = -1;
            if(head >= 0 && !b.compare(batches[head])) batches[tail].batch = index;
            else
            {
                if(head < 0) first = index;
                else batches[head].next = index;
                head = index;
                ++count;
            }
            tail = index;
        }
    }
};

#endif
