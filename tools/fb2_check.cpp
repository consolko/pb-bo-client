#include "progress.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <cstdio>
#include <cstdlib>
static void require(bool ok,const char *what) { if (!ok) { std::fprintf(stderr,"FAIL: %s\n",what); std::exit(1); } }
static void write(const QString &path,const QByteArray &data) { QFile f(path); require(f.open(QIODevice::WriteOnly) && f.write(data)==data.size(),"write fixture"); }
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    // Optional export compares this implementation with the real native model / Foliate in research.
    if (argc==2) {
        QString path=QString::fromLocal8Bit(argv[1]); QJsonArray points;
        for (int page=0;page<100;++page) {
            for (int offset=0;offset<60001;++offset) {
                auto native=QString("pbr:/word?page=%1&offs=%2").arg(page).arg(offset);
                auto cfi=bookCfi(path,native); if (cfi.isEmpty()) break;
                QString back; require(bookPosition(path,cfi,nullptr,nullptr,nullptr,&back) && back==native,"export roundtrip");
                points.append(QJsonObject{{"native",native},{"cfi",cfi}});
            }
        }
        std::puts(QJsonDocument(points).toJson().constData()); return 0;
    }
    QTemporaryDir dir; require(dir.isValid(),"temporary directory"); const auto path=dir.path()+"/test.fb2";
    const QByteArray prefix="<?xml version=\"1.0\" encoding=\"utf-8\"?><FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\">";
    const QByteArray body=R"(<body><title><p>Title</p></title><epigraph><p>Front</p></epigraph><section id="ch"><title><p>Chapter</p></title>
<p id="text">A😀<strong id="b">Б е́</strong> tail<![CDATA[ data]]><!--comment--> end</p><empty-line/>
<section><title><p>Nested</p></title><p>Last</p></section></section></body>
<body name="notes"><section id="n1"><p>Note one</p></section><section id="n2"><p>Note two</p></section></body>)";
    write(path,prefix+body+"</FictionBook>");
    // Native PB634 model: title + epigraph share page 0, nested section stays in page 1;
    // each notes section has its own native page but shares one Foliate document.
    const QList<QPair<QString,QString>> cases{
        {"pbr:/word?page=0", "epubcfi(/6/2!/4/2/2/1:0)"},
        {"pbr:/word?page=0&offs=6", "epubcfi(/6/4!/4/2/2/1:0)"},
        {"pbr:/word?page=1", "epubcfi(/6/6!/4/2/2/2/1:0)"},
        {"pbr:/word?page=1&offs=9", "epubcfi(/6/6!/4/2/4/1:1)"},
        {"pbr:/word?page=1&offs=13", "epubcfi(/6/6!/4/2/4/2/1:2)"},
        {"pbr:/word?page=2", "epubcfi(/6/8!/4/2/2/2/1:0)"},
        {"pbr:/word?page=3", "epubcfi(/6/8!/4/2/4/2/1:0)"}
    };
    resetProgressWork();
    const auto snapshot=fb2Index(path); require(bool(snapshot),"build immutable FB2 snapshot");
    for (const auto &[native,cfi]:cases) {
        require(bookCfi(path,native)==cfi,"native coordinate -> expected Foliate CFI");
        QString back,point; double percent=-1; QVariantMap context;
        require(bookPosition(path,cfi,&percent,&point,&context,&back),"Foliate coordinate accepted");
        require(back==(native.contains("offs=")?native:native+"&offs=0") && point==cfi,"exact reverse coordinate");
        require(percent>=0 && percent<=100 && context.contains("excerpt"),"display estimate and context");
    }
    require(progressWork().fb2Parses==1,"all conversions share one cached FB2 parse");
    clearEpubCache();
    require(bookCfi(path,cases[0].first,snapshot)==cases[0].second &&
        bookPosition(path,cases[4].second,nullptr,nullptr,nullptr,nullptr,nullptr,snapshot) &&
        sameBookPosition(path,cases[4].second,cases[4].second,true,snapshot) && progressWork().fb2Parses==1,
        "operation snapshot survives cache eviction across conversion and comparisons");
    const QString asserted="epubcfi(/6/6!/4/2[ch]/4[text]/2[b]/1:2)";
    require(sameBookPosition(path,asserted,cases[4].second),"ID assertions compare after validation");
    require(!bookPosition(path,QString(asserted).replace("[b]","[wrong]"),nullptr),"wrong ID rejected");
    const QString range="epubcfi(/6/6!/4/2/4,/1:1,/2/1:2)";
    QString point;
    require(bookPosition(path,range,nullptr,&point) && point==cases[3].second,"range uses validated start");
    require(sameBookPosition(path,range,cases[3].second),"point and range start equivalent");
    require(!sameBookPosition(path,range,cases[3].second,true),"range end change retained");
    require(!bookPosition(path,"epubcfi(/6/6!/4/2/4,/2/1:2,/1:1)",nullptr),"reversed range rejected");
    require(!bookPosition(path,"epubcfi(/6/6!/4/2/4,/2/1:0,/1:3)",nullptr),"reversed equivalent boundary rejected");
    require(!bookPosition(path,"epubcfi(/6/6!/4/2/4,/1:1,/2/1:999)",nullptr),"invalid range end rejected");
    for (const auto &native:{"pbr:/word?page=-1","pbr:/word?page=100","pbr:/word?page=1&offs=999999999999999999999","pbr:/word?page=1&offs=999","pbr:/word?page=1&offs=2&extra=1"})
        require(bookCfi(path,native).isEmpty(),"invalid native coordinates rejected");
    for (const auto &extra:{"<image/>","<table><tr><td>Cell</td></tr></table>","<poem><stanza><v>Verse</v></stanza></poem>","<unknown>Text</unknown>","<p><image/></p>"}) {
        write(path,prefix+"<body><section><p>Text</p>"+extra+"</section></body></FictionBook>");
        require(bookCfi(path,"pbr:/word?page=0").isEmpty(),"uncalibrated structure blocked");
    }
    write(path,prefix+"<description><title-info><coverpage><image/></coverpage></title-info></description>"+body+"</FictionBook>");
    require(bookCfi(path,cases[4].first,snapshot).isEmpty(),"old snapshot rejects a changed file");
    require(bookCfi(path,cases[4].first)==cases[4].second,"metadata cover does not shift native text or Foliate CFI");
    // U634 starts the next word page before the paragraph reaching 2048 UTF-16 units.
    QByteArray longBody="<body><section><p>"+QByteArray(1024,'a')+"</p><empty-line/><p>"+QByteArray(1023,'b')+"</p>";
    longBody+="<p>c</p><p>"+QByteArray(2047,'d')+"</p><p>e</p></section><section><p>Tail</p></section></body>";
    write(path,prefix+longBody+"</FictionBook>");
    for (const auto &[native,cfi]:QList<QPair<QString,QString>>{
        {"pbr:/word?page=0&offs=1026","epubcfi(/6/2!/4/2/6/1:0)"},
        {"pbr:/word?page=1&offs=0","epubcfi(/6/2!/4/2/8/1:0)"},
        {"pbr:/word?page=2&offs=2046","epubcfi(/6/2!/4/2/10/1:2046)"},
        {"pbr:/word?page=3&offs=0","epubcfi(/6/2!/4/2/12/1:0)"},
        {"pbr:/word?page=4&offs=0","epubcfi(/6/4!/4/2/2/1:0)"}}) {
        require(bookCfi(path,native)==cfi,"word-page boundary matches native calibration");
        QString back; require(bookPosition(path,cfi,nullptr,nullptr,nullptr,&back) && back==native,"word-page boundary restores exactly");
    }
    QByteArray paragraphs;
    for (int i=0;i<300;++i) paragraphs+="<p>"+QByteArray(1000,'x')+"</p>";
    write(path,prefix+"<body><section>"+paragraphs+"</section></body></FictionBook>");
    require(bookCfi(path,"pbr:/word?page=149&offs=1002")=="epubcfi(/6/2!/4/2/600/1:1)","long prose section spans native word pages");
    write(path,prefix+"<body><section><p>"+QByteArray(60001,'x')+"</p></section></body></FictionBook>");
    require(bookCfi(path,"pbr:/word?page=0").isEmpty(),"uncalibrated oversized paragraph blocked");
    write(path,prefix+"<body><section><p id=\""+QByteArray(4*1024*1024,'a')+"\">Text</p></section></body></FictionBook>");
    const auto large=fb2Index(path);
    require(large && fb2Index(path)!=large,"large converted attributes do not enter the small FB2 cache");
    write(path,"<!DOCTYPE FictionBook [<!ENTITY x SYSTEM 'file:///etc/passwd'>]>"+prefix+body+"</FictionBook>");
    require(bookCfi(path,"pbr:/word?page=0").isEmpty(),"entity declaration blocked");
    std::puts("PASS: FB2 coordinates, ranges, UTF-16, notes and unsupported structures");
}
