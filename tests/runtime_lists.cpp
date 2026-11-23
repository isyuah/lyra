#include "lists.h"
#include "managed.h"

#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

void check(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "lists: %s\n", message); std::exit(EXIT_FAILURE); }
}

const int* box(int value) {
    auto* object = static_cast<int*>(lyraGcAlloc(sizeof(value)));
    *object = value;
    return object;
}

int load(const LyraList* list, int index) {
    return *static_cast<const int*>(lyraListGet(list, index, __FILE__, __LINE__, 1));
}

}

int main() {
    auto* first = lyraListCreate();
    auto* second = lyraListCreate();
    lyraListAdd(first, box(5));
    lyraListExtend(second, first);
    lyraListSet(first, 0, box(9));
    check(load(second, 0) == 5, "spreading aliased a replaceable source slot");
    lyraListExtend(second, second);
    check(lyraListSize(second) == 2 && load(second, 1) == 5, "self append failed");
    std::thread workers[4];
    for (auto& worker : workers) worker = std::thread([first] {
        auto registration = lyraGcEnterWorker();
        for (int index = 0; index < 64; ++index) lyraListAdd(first, box(index));
        lyraGcLeaveWorker(registration);
    });
    for (auto& worker : workers) worker.join();
    lyraGcCollect();
    check(lyraListSize(first) == 257, "concurrent additions lost slots");
    int total = 0;
    for (int index = 0; index < 257; ++index) total += load(first, index);
    check(total == 9 + 4 * 63 * 64 / 2, "GC lost a boxed list element");
}
