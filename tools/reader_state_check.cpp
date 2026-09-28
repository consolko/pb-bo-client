// VM-only check of the same InkView task query used before replacing an EPUB.
#include "device.h"
#include <cstdio>

int main(int argc, char **argv) {
    if (argc != 2) return 3;
    setupDevice();
    const auto state = readerFileState(QString::fromLocal8Bit(argv[1]));
    const char *name = state == ReaderFileState::Open ? "open" :
                       state == ReaderFileState::Closed ? "closed" : "unknown";
    std::puts(name);
    return state == ReaderFileState::Open ? 1 : state == ReaderFileState::Unknown ? 2 : 0;
}
