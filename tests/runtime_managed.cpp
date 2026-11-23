#include "managed.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "managed test: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

struct Value {
    int64_t number;
};

struct Closure {
    int64_t (*invoke)(const Value *);
    Value *capture;
};

struct Collection {
    Value *entries[2];
    Closure *closure;
    char *bytes;
};

int64_t readCapture(const Value *value) {
    return value->number;
}

[[gnu::noinline]] LyraGcRoot *makeGraph() {
    auto *collection = static_cast<Collection *>(lyraGcAlloc(sizeof(Collection)));
    check(!collection->entries[0] && !collection->entries[1] && !collection->closure && !collection->bytes, "traced allocation was not zeroed");
    collection->entries[0] = static_cast<Value *>(lyraGcAlloc(sizeof(Value)));
    collection->entries[1] = collection->entries[0];
    collection->closure = static_cast<Closure *>(lyraGcAlloc(sizeof(Closure)));
    collection->closure->invoke = readCapture;
    collection->closure->capture = collection->entries[0];
    collection->closure->capture->number = 314;
    collection->bytes = static_cast<char *>(lyraGcAllocAtomic(6));
    memcpy(collection->bytes, "alive", 6);
    return lyraGcRootCreate(collection);
}

}

int main() {
    auto *empty = lyraGcAlloc(0);
    check(empty != lyraGcAlloc(0), "distinct empty objects must have distinct identities");
    auto *local = static_cast<Value *>(lyraGcAlloc(sizeof(Value)));
    local->number = 41;
    auto *root = makeGraph();
    for (int round = 0; round < 4; ++round) {
        for (int index = 0; index < 4096; ++index) lyraGcAlloc(192);
        lyraGcCollect();
    }
    auto *collection = static_cast<Collection *>(lyraGcRootGet(root));
    check(local->number == 41, "local was lost across collection");
    check(collection->entries[1] == collection->entries[0] && collection->entries[0] == collection->closure->capture, "shared object identity changed");
    check(collection->closure->invoke(collection->closure->capture) == 314, "captured value was lost");
    check(strcmp(collection->bytes, "alive") == 0, "pointer-free byte allocation was lost");
    lyraGcRootDrop(root);
}
