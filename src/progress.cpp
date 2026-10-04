#include "progress.h"
#include "zip.h"
#include <QFile>
#include <QUrl>
#include <QDir>
#include <QDomDocument>
#include <QXmlStreamReader>
#include <QRegularExpression>
#include <QMap>
#include <QSet>
#include <QtEndian>
#include <zlib.h>
#include <algorithm>

namespace {
QString tag(const QDomElement &e) { return e.tagName().section(':',-1); }
bool precedingHeading(const QDomNode &node, const QDomNode &target, QString &heading) {
    const QString name=tag(node.toElement());
    if (name.size()==2 && name[0]=='h' && name[1]>='1' && name[1]<='6')
        heading=node.toElement().text().simplified().left(120);
    if (node==target) return true;
    for (auto child=node.firstChild(); !child.isNull(); child=child.nextSibling())
        if (precedingHeading(child,target,heading)) return true;
    return false;
}
bool xml(const QByteArray &bytes, QDomDocument &doc) {
    if (bytes.isEmpty()) return false;
    QXmlStreamReader reader(bytes); int depth=0, nodes=0;
    while (!reader.atEnd()) {
        const auto token=reader.readNext();
        if (token==QXmlStreamReader::DTD) {
            // Permit only a declaration without a subset or external identifiers.
            // The same vetted bytes are then passed to QDomDocument below.
            static const QRegularExpression simpleDoctype(
                "^<!DOCTYPE\\s+[A-Za-z_][A-Za-z0-9_.:-]*\\s*>$");
            if (!reader.dtdPublicId().isEmpty() || !reader.dtdSystemId().isEmpty() ||
                !reader.entityDeclarations().isEmpty() || !reader.notationDeclarations().isEmpty() ||
                !simpleDoctype.match(reader.text().toString()).hasMatch()) return false;
        }
        if (token==QXmlStreamReader::EntityReference || ++nodes>200000) return false;
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
// ponytail: simple steps and ID assertions only; extended assertions need a tested resolver.
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
            // CFI addresses a logical text chunk between elements. Comments and
            // processing instructions do not split it; text and CDATA are joined.
            const int wanted=index/2;
            int n=0;
            qint64 length=0;
            QDomNode found;
            for (auto child=target.firstChild();!child.isNull();child=child.nextSibling()) {
                if (child.isElement()) {
                    if (n==wanted) break;
                    before+=textSize(child);
                    ++n;
                } else if (child.isText() || child.isCDATASection()) {
                    if (n==wanted) {
                        if (found.isNull()) found=child;
                        length+=child.nodeValue().size();
                    } else before+=child.nodeValue().size();
                }
            }
            if (n!=wanted) return false;
            offset=m.captured(3).isEmpty()?0:m.captured(3).toInt(&ok);
            if ((!m.captured(3).isEmpty() && !ok) || offset>length) return false;
            // Empty chunks have no DOM node. The parent is sufficient for context;
            // the validated CFI retains the exact logical chunk and offset.
            if (!found.isNull()) target=found;
            before+=offset;
        }
    }
    return true;
}

QDomElement childElement(const QDomElement &parent, const QString &name) {
    for (auto e=parent.firstChildElement();!e.isNull();e=e.nextSiblingElement())
        if (tag(e)==name) return e;
    return {};
}

QString archivePath(const QString &base, const QString &href) {
    const QUrl url(href, QUrl::StrictMode);
    if (href.isEmpty() || !url.isValid() || !url.isRelative() || !url.host().isEmpty() ||
        url.hasQuery() || url.hasFragment()) return {};
    const QString decoded=QUrl::fromPercentEncoding(href.toUtf8());
    if (decoded.startsWith('/') || decoded.contains('\\') || decoded.contains(QChar::Null)) return {};
    const QString path=QDir::cleanPath(base+decoded);
    return path==".." || path.startsWith("../") || path=="." ? QString{} : path;
}

struct Package {
    QDomDocument document;
    QDomElement spine;
    QMap<QString,QString> resources;
};

bool loadPackage(Zip &zip, Package &package) {
    QDomDocument container;
    if (!xml(zip.read("META-INF/container.xml"),container) || tag(container.documentElement())!="container") return false;
    const auto roots=childElement(container.documentElement(),"rootfiles");
    // Standard CFIs address the default rendition: the first rootfile.
    const auto rootfile=childElement(roots,"rootfile");
    const QString opf=archivePath({},rootfile.attribute("full-path"));
    if (opf.isEmpty() || !xml(zip.read(opf),package.document)) return false;
    const auto root=package.document.documentElement();
    if (tag(root)!="package") return false;
    const auto manifest=childElement(root,"manifest");
    package.spine=childElement(root,"spine");
    if (manifest.isNull() || package.spine.isNull()) return false;
    const QString directory=opf.contains('/')?opf.left(opf.lastIndexOf('/')+1):QString{};
    for (auto e=manifest.firstChildElement();!e.isNull();e=e.nextSiblingElement()) {
        if (tag(e)!="item") continue;
        const QString id=e.attribute("id");
        if (id.isEmpty() || package.resources.contains(id)) return false;
        const QString href=e.attribute("href"), resource=archivePath(directory,href);
        const QUrl url(href,QUrl::StrictMode);
        if (resource.isEmpty() && (!url.isValid() || url.host().isEmpty() ||
            (url.scheme()!="https" && url.scheme()!="http"))) return false;
        package.resources.insert(id,resource);
    }
    return true;
}

// Called only after both endpoints and all assertions have been validated.
QString locationKey(QString point) {
    point.remove(QRegularExpression("\\[[^\\]]*\\]"));
    const int bang=point.indexOf('!');
    const int slash=point.lastIndexOf('/');
    if (slash>bang) {
        const auto last=point.mid(slash+1,point.size()-slash-2);
        if (!last.contains(':') && last.toInt()%2) point.insert(point.size()-1,":0");
    }
    return point;
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

bool epubPosition(const QString &path, const QString &cfi, double *percentage, QString *point, QVariantMap *context) {
    if (context) context->clear();
    QVariantMap resolvedContext;
    if (cfi.size()>4096 || !cfi.startsWith("epubcfi(") || !cfi.endsWith(')')) return false;
    const auto range=cfi.mid(8,cfi.size()-9).split(',');
    if (range.size()!=1 && range.size()!=3) return false;
    const auto parts=range[0].split('!');
    if (parts.size()!=2) return false;
    QString start=parts[1], end;
    if (range.size()==3) {
        if (start.isEmpty() ||
            (!range[1].startsWith('/') && !range[1].startsWith(':')) ||
            (!range[2].startsWith('/') && !range[2].startsWith(':'))) return false;
        start+=range[1]; end=parts[1]+range[2];
    }
    Zip zip(path); Package package;
    if (!loadPackage(zip,package)) return false;
    const auto root=package.document.documentElement();
    QDomNode ref; qint64 unused=0; int offset=0;
    if (!resolve(root,parts[0],ref,unused,offset) || tag(ref.toElement())!="itemref" || ref.parentNode()!=package.spine) return false;
    qint64 total=0, position=-1;
    for (auto item=package.spine.firstChildElement();!item.isNull();item=item.nextSiblingElement()) {
        // Coordinate-only checks must not parse unrelated chapters or depend on
        // the availability of a whole-book text-length estimate.
        if (!percentage && item!=ref) continue;
        const QString resource=package.resources.value(item.attribute("idref"));
        QDomDocument content;
        if (resource.isEmpty() || !xml(zip.read(resource),content)) return false;
        if (item==ref) {
            QDomNode target; qint64 before=0;
            if (!resolve(content.documentElement(),start,target,before,offset)) return false;
            if (!end.isEmpty()) {
                QDomNode last; qint64 endBefore=0; int endOffset=0;
                if (!resolve(content.documentElement(),end,last,endBefore,endOffset) || endBefore<before) return false;
                // Text offsets alone cannot order empty elements: compare validated CFI steps too.
                const auto order=[](const QString &path) {
                    QList<int> result;
                    for (auto step : path.mid(1).split('/')) {
                        step.remove(QRegularExpression("\\[[^\\]]*\\]"));
                        for (const auto &number : step.split(':')) result.append(number.toInt());
                    }
                    return result;
                };
                const auto firstOrder=order(start), lastOrder=order(end);
                if (std::lexicographical_compare(lastOrder.begin(),lastOrder.end(),firstOrder.begin(),firstOrder.end())) return false;
            }
            position=total+before;
            if (context) {
                QString heading;
                precedingHeading(content.documentElement(),target,heading);
                resolvedContext={{"chapter",heading},
                    {"excerpt",content.documentElement().text().mid(before,160).simplified()}};
            }
        }
        total+=textSize(content.documentElement());
    }
    if (position<0 || (percentage && (total<=0 || position>total))) return false;
    if (percentage) *percentage=100.0*double(position)/double(total);
    if (point) *point="epubcfi("+parts[0]+"!"+start+")";
    if (context) *context=resolvedContext;
    return true;
}


bool sameEpubPosition(const QString &path, const QString &first, const QString &second, bool compareRange) {
    if (first.isEmpty() || second.isEmpty()) return first.isEmpty() && second.isEmpty();
    QString left, right;
    if (!epubPosition(path,first,nullptr,&left) || !epubPosition(path,second,nullptr,&right)) return false;
    if (locationKey(left)!=locationKey(right)) return false;
    if (!compareRange) return true;
    const auto endpoint=[](const QString &cfi) {
        const auto range=cfi.mid(8,cfi.size()-9).split(',');
        return range.size()==3 ? "epubcfi("+range[0]+range[2]+")" : cfi;
    };
    // Preserve range-end changes even when both ranges start at the same point.
    return epubPosition(path,endpoint(first),nullptr,&left) &&
        epubPosition(path,endpoint(second),nullptr,&right) && locationKey(left)==locationKey(right);
}

bool validEpub(const QString &path) {
    Zip zip(path);
    if (zip.read("mimetype")!="application/epub+zip") return false;
    Package package;
    if (!loadPackage(zip,package)) return false;
    int spineItems=0;
    for (auto item=package.spine.firstChildElement();!item.isNull();item=item.nextSiblingElement()) {
        if (tag(item)!="itemref") return false;
        const QString resource=package.resources.value(item.attribute("idref"));
        if (resource.isEmpty() || !zip.contains(resource)) return false;
        ++spineItems;
    }
    if (!spineItems) return false;
    // Local manifest resources must actually exist. Remote optional resources
    // are never fetched; this is structural validation, not EPUBCheck.
    for (const auto &resource:package.resources)
        if (!resource.isEmpty() && !zip.contains(resource)) return false;
    return true;
}
