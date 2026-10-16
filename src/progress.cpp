#include "progress.h"
#include "zip.h"
#include "file_work.h"
#include <QFile>
#include <QFileInfo>
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
#include <functional>
#include <QElapsedTimer>
#include <QTimer>
#include <memory>

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
constexpr qint64 epubWorkBytes=32*1024*1024;
// Includes input, UTF-16 conversion and conservative node overhead, before DOM allocation.
bool scanXml(const QByteArray &bytes,int *nodeCount=nullptr,qint64 *text=nullptr,qint64 budget=0) {
    if (bytes.isEmpty()) return false;
    QXmlStreamReader reader(bytes); int depth=0, nodes=0; qint64 length=0;
    if (budget && bytes.size()*6>budget) return false;
    while (!reader.atEnd()) {
        if (fileTaskCancelled()) return false;
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
        if (reader.isStartElement()) nodes+=reader.attributes().size()*2;
        if (token==QXmlStreamReader::EntityReference || ++nodes>200000 ||
            (budget && bytes.size()*6+qint64(nodes)*256>budget)) return false;
        if (reader.isCharacters()) length+=reader.text().size();
        if (reader.isStartElement() && ++depth>128) return false;
        if (reader.isEndElement()) --depth;
    }
    if (nodeCount) *nodeCount=nodes;
    if (text) *text=length;
    return !reader.hasError() && !fileTaskCancelled();
}
bool xml(const QByteArray &bytes, QDomDocument &doc,int *nodeCount=nullptr,qint64 budget=epubWorkBytes) {
    return scanXml(bytes,nodeCount,nullptr,budget) &&
        bool(doc.setContent(bytes,QDomDocument::ParseOption::PreserveSpacingOnlyNodes));
}
qint64 textSize(const QDomNode &node) {
    if (fileTaskCancelled()) return 0;
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
    int nodes=0;
};

bool loadPackage(Zip &zip, Package &package) {
    QDomDocument container;
    if (!xml(zip.read("META-INF/container.xml",1024*1024),container,nullptr,8*1024*1024) || tag(container.documentElement())!="container") return false;
    const auto roots=childElement(container.documentElement(),"rootfiles");
    // Standard CFIs address the default rendition: the first rootfile.
    const auto rootfile=childElement(roots,"rootfile");
    const QString opf=archivePath({},rootfile.attribute("full-path"));
    if (opf.isEmpty() || !xml(zip.read(opf,2*1024*1024),package.document,&package.nodes,8*1024*1024)) return false;
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

QList<int> cfiOrder(const QString &path) {
    QList<int> result;
    for (auto step:path.mid(1).split('/')) {
        step.remove(QRegularExpression("\\[[^\\]]*\\]"));
        for (const auto &number:step.split(':')) result.append(number.toInt());
    }
    return result;
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

namespace {
constexpr qint64 epubCacheBytes=16*1024*1024,epubLifetime=120000;
constexpr int epubCacheNodes=100000;
#ifdef BOOKORBIT_TEST_HTTP
thread_local ProgressWork progressCounters;
#endif
struct EpubIndex;
void reserveEpubWork(EpubIndex *index);
struct EpubIndex {
    struct Chapter { QDomDocument document; qint64 cost=0,used=0; int nodes=0; };
    struct Length { qint64 text=0,bytes=0; };
    FileStamp stamp;
    Zip zip;
    Package package;
    QList<QDomElement> spine;
    QHash<QString,Chapter> chapters;
    QHash<QString,Length> lengths;
    QList<qint64> offsets;
    qint64 total=0,used=0,sequence=0,packageBytes=0,baseCost=0;
    bool valid=false,complete=false;
    explicit EpubIndex(const QString &path,const FileStamp &identity):stamp(identity),zip(path) {
#ifdef BOOKORBIT_TEST_HTTP
        ++progressCounters.packages;
#endif
        if (!loadPackage(zip,package) || fileTaskCancelled()) return;
        packageBytes=32*1024*1024-zip.budget;
        // shortcut: conservative DOM accounting, measure allocator overhead if the cache budget grows.
        baseCost=packageBytes*4+qint64(package.nodes)*256;
        for (auto it=zip.entries.begin();it!=zip.entries.end();++it) baseCost+=it.key().size()*2+it.value().size()+256;
        for (auto it=package.resources.begin();it!=package.resources.end();++it) baseCost+=(it.key().size()+it.value().size())*2+256;
        for (auto item=package.spine.firstChildElement();!item.isNull();item=item.nextSiblingElement()) spine.append(item);
        valid=fileStamp(path)==stamp && !spine.isEmpty();
    }
    qint64 cost() const {
        qint64 result=baseCost+qint64(lengths.size())*512+qint64(offsets.size())*8;
        for (const auto &chapter:chapters) result+=chapter.cost;
        return result;
    }
    int nodes() const { int result=package.nodes; for (const auto &chapter:chapters) result+=chapter.nodes; return result; }
    QDomDocument chapter(const QString &resource) {
        if (fileTaskCancelled() || resource.isEmpty()) return {};
        auto existing=chapters.find(resource);
        if (existing!=chapters.end()) { existing->used=++sequence; return existing->document; }
        reserveEpubWork(this);
        // Extraction and DOM have a separate working budget from retained cache storage.
        zip.budget=32*1024*1024;
        const auto bytes=zip.read(resource); QDomDocument document; int count=0;
#ifdef BOOKORBIT_TEST_HTTP
        ++progressCounters.chapters;
#endif
        if (!xml(bytes,document,&count,epubWorkBytes-baseCost) || fileTaskCancelled()) return {};
        const auto length=textSize(document.documentElement());
        if (fileTaskCancelled()) return {};
        lengths[resource]={length,bytes.size()};
        const auto charge=qint64(bytes.size())*4+qint64(count)*256;
        if (charge<=epubCacheBytes && count<=epubCacheNodes) {
            chapters[resource]={document,charge,++sequence,count};
            while (!chapters.isEmpty() && (chapters.size()>2 || cost()>epubCacheBytes || nodes()>epubCacheNodes)) {
                auto oldest=chapters.begin();
                for (auto it=chapters.begin();it!=chapters.end();++it) if (it->used<oldest->used) oldest=it;
                chapters.erase(oldest);
            }
        }
        return document;
    }
    bool estimate() {
        if (complete) return true;
#ifdef BOOKORBIT_TEST_HTTP
        ++progressCounters.lengthPasses;
#endif
        QList<qint64> prefix; qint64 size=packageBytes,sum=0;
        for (const auto &item:spine) {
            if (fileTaskCancelled()) return false;
            const auto resource=package.resources.value(item.attribute("idref"));
            if (!lengths.contains(resource)) {
                reserveEpubWork(this); zip.budget=32*1024*1024;
                const auto bytes=zip.read(resource); qint64 text=0;
#ifdef BOOKORBIT_TEST_HTTP
                ++progressCounters.lengthReads;
#endif
                if (!scanXml(bytes,nullptr,&text)) return false;
                lengths[resource]={text,bytes.size()};
            }
            const auto length=lengths.value(resource); size+=length.bytes;
            if (size>32*1024*1024) return false;
            prefix.append(sum); sum+=length.text;
        }
        if (fileTaskCancelled() || sum<=0) return false;
        offsets=prefix; total=sum; complete=true; return true;
    }
};
struct EpubCache {
    QElapsedTimer clock;
    QTimer timer;
    QHash<QString,std::shared_ptr<EpubIndex>> books;
    EpubCache() {
        clock.start(); timer.setSingleShot(true);
        QObject::connect(&timer,&QTimer::timeout,[this] { trim(); });
    }
    void trim() {
        qint64 bytes=0; int nodes=0;
        for (auto it=books.begin();it!=books.end();) {
            if (clock.elapsed()-it.value()->used>=epubLifetime) it=books.erase(it);
            else { bytes+=it.value()->cost(); nodes+=it.value()->nodes(); ++it; }
        }
        while (books.size()>4 || bytes>epubCacheBytes || nodes>epubCacheNodes) {
            auto oldest=books.begin();
            for (auto it=books.begin();it!=books.end();++it) if (it.value()->used<oldest.value()->used) oldest=it;
            bytes-=oldest.value()->cost(); nodes-=oldest.value()->nodes(); books.erase(oldest);
#ifdef BOOKORBIT_TEST_HTTP
            ++progressCounters.evictions;
#endif
        }
        timer.stop();
        if (!books.isEmpty()) {
            qint64 next=epubLifetime;
            for (const auto &book:books) next=qMin(next,epubLifetime-(clock.elapsed()-book->used));
            timer.start(int(qMax(qint64(1),next)));
        }
    }
    std::shared_ptr<EpubIndex> get(const QString &path) {
        trim(); const auto key=QFileInfo(path).absoluteFilePath(); const auto stamp=fileStamp(path);
        if (!stamp.valid) { books.remove(key); return {}; }
        auto result=books.value(key);
        if (result && result->stamp==stamp) {
#ifdef BOOKORBIT_TEST_HTTP
            ++progressCounters.hits;
#endif
        } else {
            // No caller retains a chapter DOM across another package construction.
#ifdef BOOKORBIT_TEST_HTTP
            progressCounters.evictions+=books.size();
#endif
            books.clear(); result=std::make_shared<EpubIndex>(path,stamp);
            if (!result->valid) return {};
            books[key]=result;
        }
        result->used=clock.elapsed(); return result;
    }
};
thread_local EpubCache epubCache;
void reserveEpubWork(EpubIndex *index) {
    for (auto it=epubCache.books.begin();it!=epubCache.books.end();) {
        if (it.value().get()!=index) it=epubCache.books.erase(it);
        else ++it;
    }
    index->chapters.clear();
}
}
void clearFb2Cache();
void clearEpubCache() { epubCache.books.clear(); epubCache.timer.stop(); clearFb2Cache(); }
#ifdef BOOKORBIT_TEST_HTTP
ProgressWork progressWork() {
    auto result=progressCounters; result.cachedBooks=epubCache.books.size();
    for (const auto &book:epubCache.books) { result.cachedBytes+=book->cost(); result.cachedNodes+=book->nodes(); }
    return result;
}
void resetProgressWork() { clearEpubCache(); progressCounters={}; }
void expireEpubCache() { for (const auto &book:epubCache.books) book->used=epubCache.clock.elapsed()-epubLifetime; epubCache.trim(); }
#endif

QString nativeCfi(const QString &position) {
    for (const auto &prefix : {QString("pbr:/webkit?##"), QString("pbr:/word?")}) {
        if (!position.startsWith(prefix)) continue;
        const int start=position.indexOf("epubcfi(");
        if (start>=0 && position.endsWith(')') && position.size()<=4096) return position.mid(start);
    }
    return {};
}

bool epubPosition(const QString &path, const QString &cfi, double *percentage, QString *point, QVariantMap *context,PositionInfo *resolved) {
    if (context) context->clear();
    if (resolved) *resolved={};
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
    const auto index=epubCache.get(path);
    if (!index) return false;
    struct Trim { ~Trim() { epubCache.trim(); } } trim;
    const auto root=index->package.document.documentElement();
    QDomNode ref; qint64 unused=0; int offset=0;
    if (!resolve(root,parts[0],ref,unused,offset) || tag(ref.toElement())!="itemref" || ref.parentNode()!=index->package.spine) return false;
    const int chapter=index->spine.indexOf(ref.toElement());
    if (chapter<0) return false;
    const auto resource=index->package.resources.value(ref.toElement().attribute("idref"));
    auto content=index->chapter(resource);
    if (content.isNull()) return false;
    QDomNode target; qint64 before=0;
    if (!resolve(content.documentElement(),start,target,before,offset)) return false;
    if (!end.isEmpty()) {
        QDomNode last; qint64 endBefore=0; int endOffset=0;
        if (!resolve(content.documentElement(),end,last,endBefore,endOffset) || endBefore<before) return false;
        const auto firstOrder=cfiOrder(start),lastOrder=cfiOrder(end);
        if (std::lexicographical_compare(lastOrder.begin(),lastOrder.end(),firstOrder.begin(),firstOrder.end())) return false;
    }
    if (context || resolved) {
        QString heading; precedingHeading(content.documentElement(),target,heading);
        resolvedContext={{"chapter",heading},{"excerpt",content.documentElement().text().mid(before,160).simplified()}};
    }
    target.clear(); content.clear();
    if (percentage) {
        if (!index->estimate()) return false;
        const qint64 position=index->offsets[chapter]+before;
        if (position<0 || position>index->total) return false;
        if (fileTaskCancelled() || fileStamp(path)!=index->stamp) { epubCache.books.remove(QFileInfo(path).absoluteFilePath()); return false; }
        *percentage=100.0*double(position)/double(index->total);
    }
    if (fileTaskCancelled() || fileStamp(path)!=index->stamp) { epubCache.books.remove(QFileInfo(path).absoluteFilePath()); return false; }

    if (point) *point="epubcfi("+parts[0]+"!"+start+")";
    if (context) *context=resolvedContext;
    if (resolved) *resolved={"epubcfi("+parts[0]+"!"+start+")",resolvedContext,chapter,before,0,index->stamp};
    return true;
}

bool positionEstimate(const QString &path,const PositionInfo &resolved,double *percentage) {
    if (!percentage || !resolved.file.valid || fileStamp(path)!=resolved.file) return false;
    if (path.endsWith(".fb2",Qt::CaseInsensitive)) {
        if (resolved.total<=0 || resolved.offset<0 || resolved.offset>resolved.total) return false;
        *percentage=100.0*double(resolved.offset)/double(resolved.total); return true;
    }
    const auto index=epubCache.get(path);
    if (!index || index->stamp!=resolved.file) return false;
    struct Trim { ~Trim() { epubCache.trim(); } } trim;
    if (resolved.chapter<0 || resolved.chapter>=index->spine.size() || resolved.offset<0 || !index->estimate()) return false;
    const auto resource=index->package.resources.value(index->spine[resolved.chapter].attribute("idref"));
    if (resolved.offset>index->lengths.value(resource).text || fileTaskCancelled() || fileStamp(path)!=index->stamp) return false;
    *percentage=100.0*double(index->offsets[resolved.chapter]+resolved.offset)/double(index->total);
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
        if (fileTaskCancelled()) return false;
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
namespace {
struct FbParagraph { QDomElement node; int section, page; qint64 offset; };
struct FbBook { QList<QDomDocument> sections; QList<FbParagraph> paragraphs; qint64 total=0; };

// shortcut: PB634 prose only; body images, tables and poetry need native-model calibration.
bool fbConvert(const QDomElement &source, QDomElement parent, FbBook &book,
               int section, int page, qint64 &offset, bool top=false, bool inlineText=false) {
    const QString name=tag(source);
    if (source.tagName()!=name || (source.hasAttribute("xmlns") &&
        source.attribute("xmlns")!="http://www.gribuser.ru/xml/fictionbook/2.0")) return false;
    QString output;
    bool paragraph=false, childrenInline=inlineText;
    if (inlineText) {
        static const QMap<QString,QString> inlineTags{{"strong","strong"},{"emphasis","em"},{"style","span"},
            {"a","a"},{"strikethrough","s"},{"sub","sub"},{"sup","sup"},{"code","code"}};
        output=inlineTags.value(name);
    } else if (name=="p" || name=="subtitle" || name=="text-author") {
        output=name=="subtitle" ? "h2" : tag(parent)=="header" || (top && name=="p") ? "h1" : "p";
        paragraph=true; childrenInline=true;
    } else if (name=="empty-line") { output="br"; paragraph=true; }
    else if (name=="section") output="section";
    else if (name=="title") output=top ? "section" : "header";
    else if (name=="epigraph" || name=="cite") output=top ? "section" : "blockquote";
    if (output.isEmpty()) return false;
    auto doc=parent.ownerDocument(); auto node=doc.createElement(output);
    if (source.hasAttribute("id")) node.setAttribute("id",source.attribute("id"));
    parent.appendChild(node);
    if (paragraph) book.paragraphs.append({node,section,page,offset});
    for (auto child=source.firstChild();!child.isNull();child=child.nextSibling()) {
        if (child.isElement()) {
            if (name=="title" && tag(child.toElement())!="p" && tag(child.toElement())!="empty-line") return false;
            if (name=="empty-line" || !fbConvert(child.toElement(),node,book,section,page,offset,
                    top && name=="title",childrenInline)) return false;
        } else if (child.isText() || child.isCDATASection() || child.isComment()) {
            if (name=="empty-line" && !child.isComment() && !child.nodeValue().isEmpty()) return false;
            if (!paragraph && !inlineText && !child.isComment() && !child.nodeValue().trimmed().isEmpty()) return false;
            node.appendChild(doc.importNode(child,true));
        } else if (!child.isProcessingInstruction()) return false;
    }
    if (paragraph) {
        offset+=textSize(node)+1;
        book.total+=textSize(node)+1;
        // shortcut: keep oversized paragraphs blocked until native paragraph splitting is calibrated.
        if (textSize(node)>60000) return false;
    }
    return true;
}

QDomElement fbDocument(FbBook &book) {
    QDomDocument doc; auto html=doc.createElement("html"); doc.appendChild(html);
    html.appendChild(doc.createTextNode("\n    "));
    html.appendChild(doc.createElement("head"));
    html.appendChild(doc.createTextNode("\n    "));
    auto body=doc.createElement("body"); html.appendChild(body);
    html.appendChild(doc.createTextNode("\n"));
    book.sections.append(doc); return body;
}

bool loadFb(const QString &path, FbBook &book) {
    QFile file(path); QDomDocument source;
    if (!file.open(QIODevice::ReadOnly) || file.size()>16*1024*1024 || !xml(file.readAll(),source,nullptr,0)) return false;
    const auto root=source.documentElement();
    if (tag(root)!="FictionBook" || root.tagName()!= "FictionBook" ||
        root.attribute("xmlns")!="http://www.gribuser.ru/xml/fictionbook/2.0") return false;
    int bodyIndex=0,page=-1; qint64 offset=0;
    for (auto body=root.firstChildElement();!body.isNull();body=body.nextSiblingElement()) {
        if (tag(body)!="body") continue;
        QDomElement notes;
        if (bodyIndex) { auto wrapper=fbDocument(book); notes=wrapper.ownerDocument().createElement("body"); wrapper.appendChild(notes); }
        bool started=false,sectionsStarted=false;
        for (auto child=body.firstChild();!child.isNull();child=child.nextSibling()) {
            if (!child.isElement()) {
                if ((child.isText() || child.isCDATASection()) && !child.nodeValue().trimmed().isEmpty()) return false;
                if (bodyIndex && (child.isText() || child.isCDATASection() || child.isComment())) notes.appendChild(notes.ownerDocument().importNode(child,true));
                continue;
            }
            const auto element=child.toElement(); const QString name=tag(element);
            if (element.tagName()!=name || (name!="section" && name!="title" && name!="epigraph") ||
                (sectionsStarted && name!="section")) return false;
            if (!started || name=="section") { ++page; offset=0; started=true; }
            sectionsStarted |= name=="section";
            auto parent=bodyIndex ? notes : fbDocument(book);
            const int before=book.paragraphs.size();
            if (!fbConvert(element,parent,book,book.sections.size()-1,page,offset,true) || before==book.paragraphs.size()) return false;
        }
        if (!started) return false;
        ++bodyIndex;
    }
    // U634 word pages split before the paragraph that reaches 2048 text units.
    // Paragraph delimiters affect offsets, but do not count toward the split.
    int storage=-1,nativePage=-1;
    qint64 pageText=0,pageStart=0;
    for (auto &paragraph:book.paragraphs) {
        const auto length=textSize(paragraph.node);
        if (paragraph.page!=storage) {
            storage=paragraph.page; ++nativePage; pageText=0; pageStart=paragraph.offset;
        } else if (pageText+length>=2048) {
            ++nativePage; pageText=0; pageStart=paragraph.offset;
        }
        paragraph.page=nativePage;
        paragraph.offset-=pageStart;
        pageText+=length;
    }
    return bodyIndex>0 && book.total>0;
}

}
struct Fb2Index {
    FbBook book;
    FileStamp stamp;
    int version=1;
    qint64 cost=0;
};
namespace {
constexpr int fb2ConverterVersion=1;
struct Fb2Cache {
    QString path;
    Fb2Snapshot index;
    QTimer timer;
    Fb2Cache() {
        timer.setSingleShot(true);
        QObject::connect(&timer,&QTimer::timeout,[this] { index.reset(); path.clear(); });
    }
};
thread_local Fb2Cache fb2Cache;
}
void clearFb2Cache() { fb2Cache.index.reset(); fb2Cache.path.clear(); fb2Cache.timer.stop(); }
Fb2Snapshot fb2Index(const QString &path) {
    if (!path.endsWith(".fb2",Qt::CaseInsensitive) || fileTaskCancelled()) return {};
    const auto stamp=fileStamp(path); const auto key=QFileInfo(path).absoluteFilePath();
    if (!stamp.valid) return {};
    if (fb2Cache.path==key && fb2Cache.index && fb2Cache.index->stamp==stamp && fb2Cache.index->version==fb2ConverterVersion) {
        fb2Cache.timer.start(120000); return fb2Cache.index;
    }
    fb2Cache.index.reset(); fb2Cache.path.clear(); fb2Cache.timer.stop();
    auto index=std::make_shared<Fb2Index>(); index->stamp=stamp; index->version=fb2ConverterVersion;
#ifdef BOOKORBIT_TEST_HTTP
    ++progressCounters.fb2Parses;
#endif
    if (!loadFb(path,index->book) || fileTaskCancelled() || fileStamp(path)!=stamp) return {};
    // Count converted DOM storage; the source DOM is already gone.
    std::function<void(QDomNode)> charge=[&](QDomNode node) {
        index->cost+=256+(node.nodeName().size()+node.nodeValue().size())*2;
        const auto attributes=node.attributes();
        for (int i=0;i<attributes.size();++i) {
            const auto attribute=attributes.item(i);
            index->cost+=256+(attribute.nodeName().size()+attribute.nodeValue().size())*2;
        }
        for (auto child=node.firstChild();!child.isNull();child=child.nextSibling()) charge(child);
    };
    for (const auto &doc:index->book.sections) charge(doc);
    index->cost+=index->book.paragraphs.size()*sizeof(FbParagraph);
    if (index->cost<=8*1024*1024) { fb2Cache.path=key; fb2Cache.index=index; fb2Cache.timer.start(120000); }
    return index;
}
namespace {
bool currentFb(const QString &path,const Fb2Snapshot &index) {
    return index && !fileTaskCancelled() && index->version==fb2ConverterVersion && fileStamp(path)==index->stamp;
}

bool within(QDomNode node,const QDomNode &ancestor) {
    while (!node.isNull()) { if (node==ancestor) return true; node=node.parentNode(); } return false;
}
qint64 fbBefore(QDomNode node) {
    qint64 result=0;
    while (!node.parentNode().isNull()) {
        for (auto prev=node.previousSibling();!prev.isNull();prev=prev.previousSibling()) result+=textSize(prev);
        node=node.parentNode();
    }
    return result;
}
QString fbPath(QDomNode node) {
    QString path;
    while (!node.parentNode().isDocument()) {
        int elements=0;
        for (auto prev=node.previousSibling();!prev.isNull();prev=prev.previousSibling()) if (prev.isElement()) ++elements;
        path="/"+QString::number(node.isElement() ? (elements+1)*2 : elements*2+1)+path;
        node=node.parentNode();
    }
    return path;
}
QString fbTextPoint(const FbParagraph &p,qint64 offset) {
    // One CFI text step covers adjacent text/CDATA nodes separated by comments.
    std::function<QString(QDomNode)> walk=[&](QDomNode parent)->QString {
        for (auto node=parent.firstChild();!node.isNull();) {
            if (node.isElement()) { auto result=walk(node); if (!result.isEmpty()) return result; node=node.nextSibling(); }
            else {
                QDomNode first; qint64 length=0;
                while (!node.isNull() && !node.isElement()) {
                    if (fileTaskCancelled()) return {};
                    if (node.isText() || node.isCDATASection()) { if (first.isNull()) first=node; length+=node.nodeValue().size(); }
                    node=node.nextSibling();
                }
                if (!first.isNull() && offset<=length) return fbPath(first)+":"+QString::number(offset);
                offset-=length;
            }
        }
        return {};
    };
    QString point=walk(p.node);
    if (point.isEmpty() && textSize(p.node)==0 && offset==0) point=fbPath(p.node);
    return point.isEmpty() ? QString{} : "epubcfi(/6/"+QString::number((p.section+1)*2)+"!"+point+")";
}
QString fbNative(const FbParagraph &p,qint64 offset) {
    return "pbr:/word?page="+QString::number(p.page)+"&offs="+QString::number(p.offset+offset);
}
struct FbLocation { int paragraph=-1; qint64 offset=0; };
bool fbResolve(const FbBook &book,int section,const QString &path,FbLocation &location) {
    if (section<0 || section>=book.sections.size()) return false;
    QDomNode target; qint64 before=0; int unused=0;
    if (!resolve(book.sections[section].documentElement(),path,target,before,unused)) return false;
    for (int i=0;i<book.paragraphs.size();++i) {
        const auto &p=book.paragraphs[i]; if (p.section!=section) continue;
        if (within(target,p.node)) {
            const auto offset=before-fbBefore(p.node);
            if (offset<0 || offset>textSize(p.node)) return false;
            location={i,offset}; return true;
        }
        if (target.isElement() && within(p.node,target)) { location={i,0}; return true; }
    }
    return false;
}

bool fbPosition(const QString &path,const QString &cfi,double *percentage,QString *point,QVariantMap *context,QString *native,PositionInfo *resolved,const Fb2Snapshot &snapshot) {
    if (context) context->clear();
    static const QRegularExpression base("^/6/([1-9][0-9]*)$");
    if (cfi.size()>4096 || !cfi.startsWith("epubcfi(") || !cfi.endsWith(')')) return false;
    auto ranges=cfi.mid(8,cfi.size()-9).split(',');
    if (ranges.size()!=1 && ranges.size()!=3) return false;
    auto parts=ranges[0].split('!'); if (parts.size()!=2) return false;
    auto match=base.match(parts[0]); bool ok=false; int index=match.captured(1).toInt(&ok);
    if (!match.hasMatch() || !ok || index%2) return false;
    QString start=parts[1],end;
    if (ranges.size()==3) {
        if (start.isEmpty() || (!ranges[1].startsWith('/') && !ranges[1].startsWith(':')) ||
            (!ranges[2].startsWith('/') && !ranges[2].startsWith(':'))) return false;
        start+=ranges[1]; end=parts[1]+ranges[2];
    }
    const auto fb=snapshot ? snapshot : fb2Index(path);
    if (!currentFb(path,fb)) return false;
    const auto &book=fb->book; FbLocation first,last;
    if (!fbResolve(book,index/2-1,start,first)) return false;
    if (!end.isEmpty() && (!fbResolve(book,index/2-1,end,last) || last.paragraph<first.paragraph ||
        (last.paragraph==first.paragraph && last.offset<first.offset))) return false;
    if (!end.isEmpty()) {
        const auto a=cfiOrder(start),b=cfiOrder(end);
        if (std::lexicographical_compare(b.begin(),b.end(),a.begin(),a.end())) return false;
    }
    const auto &p=book.paragraphs[first.paragraph];
    if (native) *native=fbNative(p,first.offset);
    if (point) *point=fbTextPoint(p,first.offset);
    if (percentage || resolved) {
        qint64 before=first.offset;
        for (int i=0;i<first.paragraph;++i) before+=textSize(book.paragraphs[i].node)+1;
        if (percentage) *percentage=100.0*double(before)/double(book.total);
        if (resolved) { resolved->offset=before; resolved->total=book.total; }
    }
    if (context) {
        QString heading; precedingHeading(book.sections[p.section].documentElement(),p.node,heading);
        *context={{"chapter",heading},{"excerpt",p.node.text().mid(first.offset,160).simplified()}};
    }
    return currentFb(path,fb);
}
}

bool syncFormat(const QString &format) { return format=="epub" || format=="fb2"; }

bool bookPosition(const QString &path,const QString &cfi,double *percentage,QString *point,QVariantMap *context,QString *native,PositionInfo *resolved,const Fb2Snapshot &fb) {
    if (resolved) *resolved={};
    if (path.endsWith(".fb2",Qt::CaseInsensitive)) {
        const auto stamp=fileStamp(path);
        QString location; QVariantMap preview;
        if (!fbPosition(path,cfi,percentage,&location,context ? context : resolved ? &preview : nullptr,native,resolved,fb) ||
            (resolved && (!stamp.valid || fileStamp(path)!=stamp))) return false;
        if (point) *point=location;
        if (resolved) { resolved->point=location; resolved->context=context ? *context : preview; resolved->file=stamp; }
        return true;
    }
    if (!path.endsWith(".epub",Qt::CaseInsensitive)) return false;
    QString result;
    if (!epubPosition(path,cfi,percentage,&result,context,resolved)) return false;
    if (point) *point=result;
    if (native) *native="pbr:/webkit?##"+result;
    return true;
}

QString bookCfi(const QString &path,const QString &position,const Fb2Snapshot &snapshot) {
    if (!path.endsWith(".fb2",Qt::CaseInsensitive)) return nativeCfi(position);
    static const QRegularExpression pattern("^pbr:/word\\?page=([0-9]+)(?:&offs=([0-9]+))?$");
    auto match=pattern.match(position); bool ok=false;
    const int page=match.captured(1).toInt(&ok); if (!match.hasMatch() || !ok) return {};
    const qint64 offset=match.captured(2).isEmpty()?0:match.captured(2).toLongLong(&ok);
    if (!ok) return {};
    const auto fb=snapshot ? snapshot : fb2Index(path);
    if (!currentFb(path,fb)) return {};
    for (const auto &p:fb->book.paragraphs)
        if (p.page==page && offset>=p.offset && offset<=p.offset+textSize(p.node)) {
            const auto point=fbTextPoint(p,offset-p.offset); return currentFb(path,fb) ? point : QString{};
        }
    return {};
}

bool sameBookPosition(const QString &path,const QString &first,const QString &second,bool compareRange,const Fb2Snapshot &snapshot) {
    if (!path.endsWith(".fb2",Qt::CaseInsensitive)) return sameEpubPosition(path,first,second,compareRange);
    if (first.isEmpty() || second.isEmpty()) return first.isEmpty() && second.isEmpty();
    const auto fb=snapshot ? snapshot : fb2Index(path);
    QString a,b;
    if (!bookPosition(path,first,nullptr,nullptr,nullptr,&a,nullptr,fb) || !bookPosition(path,second,nullptr,nullptr,nullptr,&b,nullptr,fb) || a!=b) return false;
    if (!compareRange) return true;
    const auto endpoint=[](const QString &cfi) {
        const auto range=cfi.mid(8,cfi.size()-9).split(',');
        return range.size()==3 ? "epubcfi("+range[0]+range[2]+")" : cfi;
    };
    return bookPosition(path,endpoint(first),nullptr,nullptr,nullptr,&a,nullptr,fb) &&
        bookPosition(path,endpoint(second),nullptr,nullptr,nullptr,&b,nullptr,fb) && a==b;
}
