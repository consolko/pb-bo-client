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
#include <functional>

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
        if (token==QXmlStreamReader::EntityReference || ++nodes>200000) return false;
        if (reader.isStartElement() && ++depth>128) return false;
        if (reader.isEndElement()) --depth;
    }
    return !reader.hasError() && bool(doc.setContent(bytes, QDomDocument::ParseOption::PreserveSpacingOnlyNodes));
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
        if (fileTaskCancelled()) return false;
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
                const auto firstOrder=cfiOrder(start), lastOrder=cfiOrder(end);
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
    if (fileTaskCancelled() || position<0 || (percentage && (total<=0 || position>total))) return false;
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

// ponytail: PB634 prose only. Images, tables, poetry and long native sections
// need separate native-model calibration before their positions can be written.
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
        // Below the firmware's 63000 UTF-16 split threshold, including delimiters.
        if (offset>60000) return false;
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
    if (!file.open(QIODevice::ReadOnly) || file.size()>16*1024*1024 || !xml(file.readAll(),source)) return false;
    const auto root=source.documentElement();
    if (tag(root)!="FictionBook" || root.tagName()!= "FictionBook" ||
        root.attribute("xmlns")!="http://www.gribuser.ru/xml/fictionbook/2.0" ||
        !root.elementsByTagName("image").isEmpty()) return false;
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
    return bodyIndex>0 && book.total>0;
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

bool fbPosition(const QString &path,const QString &cfi,double *percentage,QString *point,QVariantMap *context,QString *native) {
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
    FbBook book; FbLocation first,last;
    if (!loadFb(path,book) || !fbResolve(book,index/2-1,start,first)) return false;
    if (!end.isEmpty() && (!fbResolve(book,index/2-1,end,last) || last.paragraph<first.paragraph ||
        (last.paragraph==first.paragraph && last.offset<first.offset))) return false;
    if (!end.isEmpty()) {
        const auto a=cfiOrder(start),b=cfiOrder(end);
        if (std::lexicographical_compare(b.begin(),b.end(),a.begin(),a.end())) return false;
    }
    const auto &p=book.paragraphs[first.paragraph];
    if (native) *native=fbNative(p,first.offset);
    if (point) *point=fbTextPoint(p,first.offset);
    if (percentage) {
        qint64 before=first.offset;
        for (int i=0;i<first.paragraph;++i) before+=textSize(book.paragraphs[i].node)+1;
        *percentage=100.0*double(before)/double(book.total);
    }
    if (context) {
        QString heading; precedingHeading(book.sections[p.section].documentElement(),p.node,heading);
        *context={{"chapter",heading},{"excerpt",p.node.text().mid(first.offset,160).simplified()}};
    }
    return true;
}
}

bool syncFormat(const QString &format) { return format=="epub" || format=="fb2"; }

bool bookPosition(const QString &path,const QString &cfi,double *percentage,QString *point,QVariantMap *context,QString *native) {
    if (path.endsWith(".fb2",Qt::CaseInsensitive)) return fbPosition(path,cfi,percentage,point,context,native);
    if (!path.endsWith(".epub",Qt::CaseInsensitive)) return false;
    QString result;
    if (!epubPosition(path,cfi,percentage,&result,context)) return false;
    if (point) *point=result;
    if (native) *native="pbr:/webkit?##"+result;
    return true;
}

QString bookCfi(const QString &path,const QString &position) {
    if (!path.endsWith(".fb2",Qt::CaseInsensitive)) return nativeCfi(position);
    static const QRegularExpression pattern("^pbr:/word\\?page=([0-9]+)(?:&offs=([0-9]+))?$");
    auto match=pattern.match(position); bool ok=false;
    const int page=match.captured(1).toInt(&ok); if (!match.hasMatch() || !ok) return {};
    const qint64 offset=match.captured(2).isEmpty()?0:match.captured(2).toLongLong(&ok);
    if (!ok) return {};
    FbBook book; if (fileTaskCancelled() || !loadFb(path,book)) return {};
    for (const auto &p:book.paragraphs)
        if (p.page==page && offset>=p.offset && offset<=p.offset+textSize(p.node)) return fbTextPoint(p,offset-p.offset);
    return {};
}

bool sameBookPosition(const QString &path,const QString &first,const QString &second,bool compareRange) {
    if (!path.endsWith(".fb2",Qt::CaseInsensitive)) return sameEpubPosition(path,first,second,compareRange);
    if (first.isEmpty() || second.isEmpty()) return first.isEmpty() && second.isEmpty();
    QString a,b;
    if (!bookPosition(path,first,nullptr,nullptr,nullptr,&a) || !bookPosition(path,second,nullptr,nullptr,nullptr,&b) || a!=b) return false;
    if (!compareRange) return true;
    const auto endpoint=[](const QString &cfi) {
        const auto range=cfi.mid(8,cfi.size()-9).split(',');
        return range.size()==3 ? "epubcfi("+range[0]+range[2]+")" : cfi;
    };
    return bookPosition(path,endpoint(first),nullptr,nullptr,nullptr,&a) &&
        bookPosition(path,endpoint(second),nullptr,nullptr,nullptr,&b) && a==b;
}
