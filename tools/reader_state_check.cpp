// VM-only check of the same InkView task query used before replacing an EPUB.
#include "device.h"
#include "progress.h"
#include <cstdio>
#include <QCoreApplication>

int main(int argc, char **argv) {
    qputenv("QT_PLUGIN_PATH", "/ebrmain/plugins");
    QCoreApplication app(argc,argv);
    if (argc != 2 && argc != 3 && argc != 4) return 3;
    setupDevice();
    if (argc==3 && QString::fromLocal8Bit(argv[2])=="--scan") {
        scanBook(QString::fromLocal8Bit(argv[1])); return 0;
    }
    if (argc==3 && QString::fromLocal8Bit(argv[2])=="--position") {
        QString position;
        if (!readerPosition(QString::fromLocal8Bit(argv[1]),&position)) return 4;
        std::printf("%s\n",qPrintable(position)); return 0;
    }
    if (argc==3 && QString::fromLocal8Bit(argv[2])=="--cfi") {
        const QString path=QString::fromLocal8Bit(argv[1]); QString position;
        if (!readerPosition(path,&position)) return 4;
        const QString cfi=bookCfi(path,position);
        std::puts(qPrintable(cfi)); return cfi.isEmpty() ? 5 : 0;
    }
    if (argc==4 && QString::fromLocal8Bit(argv[2])=="--save-cfi") {
        QString previous,error;
        const QString path=QString::fromLocal8Bit(argv[1]);
        if (!readerPosition(path,&previous)) return 4;
        const bool ok=saveReaderPosition(path,previous,QString::fromLocal8Bit(argv[3]),readerProfile(),&error);
        std::puts(ok ? "saved" : qPrintable(error));
        return ok ? 0 : 5;
    }
    const auto state = readerFileState(QString::fromLocal8Bit(argv[1]));
    const char *name = state == ReaderFileState::Open ? "open" :
                       state == ReaderFileState::Closed ? "closed" : "unknown";
    std::puts(name);
    return state == ReaderFileState::Open ? 1 : state == ReaderFileState::Unknown ? 2 : 0;
}
