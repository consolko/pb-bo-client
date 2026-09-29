#include "progress.h"
#include <QFile>
#include <QUrl>
#include <QDir>
#include <QDomDocument>
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QMap>
#include <QtEndian>
#include <zlib.h>

namespace {
quint16 u16(const QByteArray &b, int p) { return qFromLittleEndian<quint16>(b.constData()+p); }
quint32 u32(const QByteArray &b, int p) { return qFromLittleEndian<quint32>(b.constData()+p); }
// Read bounded ZIP members in place. No extraction, external process, or new dependency.
struct Zip {
    QFile file;
    QMap<QString, QByteArray> entries;
    qint64 budget = 32 * 1024 * 1024;
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
    }
    QByteArray read(const QString &name) {
        const auto h = entries.value(name);
        if (h.size()!=46 || u16(h,8)&1) return {};
        const auto compressed=u32(h,20), size=u32(h,24), offset=u32(h,42);
        const int method=u16(h,10);
        if (size>8*1024*1024 || compressed>8*1024*1024 || size>budget || (method!=0 && method!=8)) return {};
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
};
QString tag(const QDomElement &e) { return e.tagName().section(':',-1); }
bool xml(const QByteArray &bytes, QDomDocument &doc) {
    if (bytes.isEmpty()) return false;
    QXmlStreamReader reader(bytes); int depth=0, nodes=0;
    while (!reader.atEnd()) {
        const auto token=reader.readNext();
        if (token==QXmlStreamReader::DTD || token==QXmlStreamReader::EntityReference || ++nodes>200000) return false;
        if (reader.isStartElement() && ++depth>128) return false;
        if (reader.isEndElement()) --depth;
    }
    return !reader.hasError() && bool(doc.setContent(bytes, QDomDocument::ParseOption::PreserveSpacingOnlyNodes));
}
qint64 textSize(const QDomNode &node) {
    if (node.isText() || node.isCDATASection()) return node.nodeValue().size();
    qint64 n=0; for (auto child=node.firstChild(); !child.isNull(); child=child.nextSibling()) n+=textSize(child);
    return n;
}
// ponytail: point CFIs only; add tested range/assertion support before accepting more forms.
// They must gain a tested resolver before they may be applied to the native reader.
bool resolve(QDomElement root, const QString &path, QDomNode &target, qint64 &before, int &offset) {
    static const QRegularExpression step("^([1-9][0-9]*)(?:\\[([A-Za-z0-9_.:-]+)\\])?(?::([0-9]+))?$");
    if (!path.startsWith('/')) return false;
    const auto steps=path.mid(1).split('/');
    target=root; before=0; offset=0;
    for (int i=0;i<steps.size();++i) {
        auto m=step.match(steps[i]); bool ok=false;
        const int index=m.captured(1).toInt(&ok);
        if (!m.hasMatch() || !ok || index>1000000 || !target.isElement()) return false;
        if (index%2==0) {
            if (!m.captured(3).isEmpty()) return false;
            int n=0; QDomNode found;
            for (auto child=target.firstChild();!child.isNull();child=child.nextSibling()) {
                if (child.isElement() && ++n==index/2) { found=child; break; }
                before+=textSize(child);
            }
            if (found.isNull() || (!m.captured(2).isEmpty() && found.toElement().attribute("id")!=m.captured(2))) return false;
            target=found;
        } else {
            if (i!=steps.size()-1 || !m.captured(2).isEmpty()) return false;
            int n=0; QDomNode found;
            for (auto child=target.firstChild();!child.isNull();child=child.nextSibling()) {
                if ((child.isText() || child.isCDATASection()) && n==index/2) { found=child; break; }
                if (child.isElement()) ++n;
                before+=textSize(child);
            }
            if (found.isNull()) return false;
            offset=m.captured(3).isEmpty()?0:m.captured(3).toInt(&ok);
            if ((!m.captured(3).isEmpty() && !ok) || offset>found.nodeValue().size()) return false;
            target=found; before+=offset;
        }
    }
    return true;
}
}

QString nativeCfi(const QString &position) {
    for (const auto &prefix : {QString("pbr:/webkit?##"), QString("pbr:/word?")}) {
        if (!position.startsWith(prefix)) continue;
        const int start=position.indexOf("epubcfi(");
        if (start>=0 && position.endsWith(')') && position.size()<=4096) return position.mid(start);
    }
    return {};
}

bool epubPosition(const QString &path, const QString &cfi, double *percentage) {
    if (cfi.size()>4096 || !cfi.startsWith("epubcfi(") || !cfi.endsWith(')')) return false;
    const auto parts=cfi.mid(8,cfi.size()-9).split('!');
    if (parts.size()!=2) return false;
    Zip zip(path); QDomDocument container, package;
    if (!xml(zip.read("META-INF/container.xml"),container)) return false;
    const auto roots=container.elementsByTagName("rootfile");
    if (roots.size()!=1) return false;
    const QString opf=roots.at(0).toElement().attribute("full-path");
    if (!xml(zip.read(opf),package)) return false;
    const auto root=package.documentElement();
    QDomElement manifest,spine;
    for (auto e=root.firstChildElement();!e.isNull();e=e.nextSiblingElement()) {
        if (tag(e)=="manifest") manifest=e;
        if (tag(e)=="spine") spine=e;
    }
    QDomNode ref; qint64 unused=0; int offset=0;
    if (!resolve(root,parts[0],ref,unused,offset) || tag(ref.toElement())!="itemref" || ref.parentNode()!=spine) return false;
    qint64 total=0, position=-1;
    const QString directory=opf.contains('/')?opf.left(opf.lastIndexOf('/')+1):QString{};
    for (auto item=spine.firstChildElement();!item.isNull();item=item.nextSiblingElement()) {
        QString href;
        for (auto entry=manifest.firstChildElement();!entry.isNull();entry=entry.nextSiblingElement())
            if (entry.attribute("id")==item.attribute("idref")) href=entry.attribute("href");
        if (href.isEmpty() || href.contains(':') || href.contains('#')) return false;
        QDomDocument content;
        if (!xml(zip.read(QDir::cleanPath(directory+QUrl::fromPercentEncoding(href.toUtf8()))),content)) return false;
        if (item==ref) {
            QDomNode target; qint64 before=0;
            if (!resolve(content.documentElement(),parts[1],target,before,offset)) return false;
            position=total+before;
        }
        total+=textSize(content.documentElement());
    }
    if (position<0 || total<=0 || position>total) return false;
    if (percentage) *percentage=100.0*double(position)/double(total);
    return true;
}
