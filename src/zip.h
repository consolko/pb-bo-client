#pragma once
#include "file_executor.h"
#include <QFile>
#include <QMap>
#include <QtEndian>
#include <zlib.h>
#include <algorithm>
#include <QSaveFile>
#include <QFileInfo>

inline quint16 u16(const QByteArray &b, int p) { return qFromLittleEndian<quint16>(b.constData()+p); }
inline quint32 u32(const QByteArray &b, int p) { return qFromLittleEndian<quint32>(b.constData()+p); }
// Bounded ZIP reading for EPUB; streaming extraction to a fixed destination for OTA.
struct Zip {
    QFile file;
    QMap<QString, QByteArray> entries;
    qint64 budget = 32 * 1024 * 1024;
    qint64 centralOffset = 0;
    explicit Zip(const QString &path) : file(path) {
        if (!file.open(QIODevice::ReadOnly) || file.size() > 100 * 1024 * 1024) return;
        file.seek(qMax(qint64(0), file.size()-65557));
        const auto tail = file.readAll();
        int end = tail.size()-22;
        for (; end >= 0; --end)
            if (u32(tail,end) == 0x06054b50 && end+22+u16(tail,end+20) == tail.size()) break;
        if (end < 0 || u16(tail,end+4) || u16(tail,end+6) || u16(tail,end+8) != u16(tail,end+10)) return;
        const auto size = u32(tail,end+12), offset = u32(tail,end+16);
        if (size > 4*1024*1024 || quint64(offset)+size > quint64(file.size())) return;
        centralOffset=offset;
        file.seek(offset); const auto dir = file.read(size);
        int p = 0;
        for (int i=0; i<u16(tail,end+10); ++i) {
            if (p+46 > dir.size() || u32(dir,p) != 0x02014b50) { entries.clear(); return; }
            const int n = u16(dir,p+28), extra = u16(dir,p+30), comment = u16(dir,p+32);
            if (p+46+n+extra+comment > dir.size() || u16(dir,p+34)) { entries.clear(); return; }
            const QString name = QString::fromUtf8(dir.mid(p+46,n));
            if (entries.contains(name)) { entries.clear(); return; }
            entries[name] = dir.mid(p,46);
            p += 46+n+extra+comment;
        }
        if (p!=dir.size()) entries.clear();
    }
    bool contains(const QString &name) {
        const auto h = entries.value(name);
        if (h.size()!=46 || (u16(h,8)&1) || (u16(h,10)!=0 && u16(h,10)!=8)) return false;
        const auto offset=u32(h,42), compressed=u32(h,20);
        if (quint64(offset)+30 > quint64(file.size()) || !file.seek(offset)) return false;
        const auto local=file.read(30);
        if (local.size()!=30 || u32(local,0)!=0x04034b50 || u16(local,8)!=u16(h,10) || (u16(local,6)&1)) return false;
        const auto filename=file.read(u16(local,26));
        return QString::fromUtf8(filename)==name &&
            quint64(offset)+30+u16(local,26)+u16(local,28)+compressed<=quint64(centralOffset);
    }
    QByteArray read(const QString &name, qint64 limit = 8 * 1024 * 1024) {
        const auto h = entries.value(name);
        if (!contains(name)) return {};
        const auto compressed=u32(h,20), size=u32(h,24), offset=u32(h,42);
        const int method=u16(h,10);
        if (size>limit || compressed>8*1024*1024 || size>budget || (method!=0 && method!=8)) return {};
        if (!file.seek(offset)) return {};
        const auto local=file.read(30);
        if (local.size()!=30 || u32(local,0)!=0x04034b50 || u16(local,8)!=method || (u16(local,6)&1)) return {};
        if (!file.seek(quint64(offset)+30+u16(local,26)+u16(local,28))) return {};
        const auto data=file.read(compressed);
        if (quint32(data.size())!=compressed) return {};
        QByteArray out;
        if (method==0) out=data;
        else {
            out.resize(size);
            z_stream z{};
            z.next_in=reinterpret_cast<Bytef *>(const_cast<char *>(data.constData())); z.avail_in=data.size();
            z.next_out=reinterpret_cast<Bytef *>(out.data()); z.avail_out=out.size();
            if (inflateInit2(&z,-MAX_WBITS)!=Z_OK) return {};
            const int rc=inflate(&z,Z_FINISH); inflateEnd(&z);
            if (rc!=Z_STREAM_END || z.total_out!=size || z.total_in!=compressed) return {};
        }
        if (quint32(out.size())!=size || crc32(0,reinterpret_cast<const Bytef *>(out.constData()),out.size())!=u32(h,16)) return {};
        budget-=size;
        return out;
    }
    bool extract(const QString &name, const QString &destination, qint64 expected, QString *error) {
        auto fail=[&](const QString &text) { if(error) *error=text; return false; };
        for(auto it=entries.begin();it!=entries.end();++it) {
            const auto parts=it.key().split('/');
            if(it.key().startsWith('/') || it.key().contains('\\') || parts.contains("..") ||
               parts.contains(".") || (u32(it.value(),38)>>16 & 0170000)==0120000)
                return fail("Unsafe ZIP entry");
        }
        if(!contains(name) || expected<1 || expected>32*1024*1024 || QFileInfo(destination).isSymLink()) return fail("Invalid ZIP member");
        const auto h=entries.value(name); const int method=u16(h,10);
        const auto size=u32(h,24),compressed=u32(h,20),offset=u32(h,42);
        if(size!=expected || compressed>32*1024*1024) return fail("Invalid ZIP size");
        file.seek(offset); const auto local=file.read(30);
        if(!file.seek(quint64(offset)+30+u16(local,26)+u16(local,28))) return fail("Cannot seek ZIP member");
        QSaveFile out(destination); out.setDirectWriteFallback(false);
        if(!out.open(QIODevice::WriteOnly)) return fail(out.errorString());
        z_stream z{}; if(method==8 && inflateInit2(&z,-MAX_WBITS)!=Z_OK) return fail("Cannot initialize ZIP decoder");
        qint64 remaining=compressed,written=0; uLong crc=crc32(0,nullptr,0); int rc=Z_OK; bool ok=true;
        char buffer[65536];
        while(remaining>0 && ok) {
            if(fileTaskCancelled()) { ok=false; break; }
            const auto input=file.read(qMin(remaining,qint64(65536)));
            if(input.isEmpty()) { ok=false; break; }
            remaining-=input.size();
            z.next_in=reinterpret_cast<Bytef*>(const_cast<char*>(input.constData())); z.avail_in=input.size();
            do {
                qint64 count=input.size(); const char *data=input.constData();
                if(method==8) {
                    const auto available=z.avail_in;
                    z.next_out=reinterpret_cast<Bytef*>(buffer); z.avail_out=sizeof(buffer);
                    rc=inflate(&z,Z_NO_FLUSH); count=sizeof(buffer)-z.avail_out; data=buffer;
                    if(rc==Z_BUF_ERROR && !z.avail_in && !count && remaining>0) break;
                    if(rc!=Z_OK && rc!=Z_STREAM_END) { ok=false; break; }
                    if(rc!=Z_STREAM_END && !count && z.avail_in==available) { ok=false; break; }
                }
                if(written+count>expected || out.write(data,count)!=count) { ok=false; break; }
                written+=count; crc=crc32(crc,reinterpret_cast<const Bytef*>(data),count);
                if(method==0) { z.avail_in=0; break; }
                if(rc==Z_STREAM_END) { if(z.avail_in || remaining) ok=false; break; }
            } while(z.avail_in || z.avail_out==0);
        }
        if(method==8) { ok=ok && rc==Z_STREAM_END && z.total_in==compressed; inflateEnd(&z); }
        if(!ok || written!=expected || crc!=u32(h,16)) { out.cancelWriting(); return fail("Damaged ZIP member"); }
        if(fileTaskCancelled()) { out.cancelWriting(); return fail("Cancelled"); }
        return out.commit() || fail(out.errorString());
    }
};
