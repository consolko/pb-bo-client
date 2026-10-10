#include "client.h"
#include "device.h"
#include "update.h"
#include "check_wait.h"
#include <QJsonArray>
#include <QCryptographicHash>
#include <QJSValue>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <QDir>
#include <QFont>
#include <QImage>
#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char *label) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
    if (!ok) std::exit(1);
}
static void settle() {
    QEventLoop loop;
    QTimer::singleShot(350, &loop, &QEventLoop::quit);
    loop.exec();
}
int main(int argc, char **argv) {
#ifdef POCKETBOOK_DEVICE
    qputenv("QT_PLUGIN_PATH", "/ebrmain/plugins");
    qputenv("QT_QPA_PLATFORM", "pocketbook2");
    QCoreApplication::setSetuidAllowed(true);
#endif
    const auto screen = setupDevice();
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    QGuiApplication::setFont(QFont(deviceFont()));
    require(!screen.isEmpty(), "device initialized");
    std::printf("System language: %s\n", qPrintable(deviceLanguage()));
    for (const QString language : {"ru", "ru_RU", "ru-RU", "ru_UA", "RUSSIAN"})
        require(interfaceLanguage(language, "system") == "ru", "Russian system language");
    for (const QString language : {"en", "en_US", "en-GB", "de_DE", "fr", "uk_UA", "C", "", "invalid"})
        require(interfaceLanguage(language, "system") == "en", "English or unsupported system language defaults to English");
    require(interfaceLanguage("ru", "en") == "en" && interfaceLanguage("de", "ru") == "ru", "manual language overrides system");
    require(interfaceLanguage("ru_RU", "invalid") == "ru", "invalid preference uses system language");
    QTemporaryDir storage;
    require(storage.isValid(), "isolated preferences");
    InterfaceTranslation translation;
    translation.apply("de_DE", "system");
    Client client(QUrl("https://example.invalid"), storage.path());
    require(client.language() == "system", "first launch follows system without persisting a manual choice");
    const auto scope=ClientWorkerCheck::scope(client);
    const QByteArray content("language fixture");
    for(int id=1;id<=7;++id) {
        const QJsonObject file{{"id",id},{"format","epub"},{"role","primary"}};
        const QJsonObject book{{"id",id},{"title","Message "+QString::number(id)},{"selectedFile",file},{"files",QJsonArray{file}}};
        QJsonObject status{{"state","error"},{"profile",readerProfile()}};
        if(id==2) status["text"]="The server returned HTTP 503. Check the address and try again.";
        if(id==3) status["text"]=QString::fromUtf8("Сервер ответил HTTP 503. Проверьте адрес и повторите.");
        if(id==4) status["text"]=QString::fromUtf8("Прогресс синхронизирован");
        if(id==5) status["text"]="Unrecognized old dynamic result";
        if(id==6) status["message"]=QJsonObject{{"code","unknown-code"},{"params",QJsonArray{}}};
        if(id==7) status["message"]=QJsonObject{{"code","server_http_error"},{"params",QJsonArray{"not a status"}}};
        const QJsonObject record{{"bookId",id},{"fileId",id},{"filename",QString::number(id)+".epub"},{"format","epub"},
            {"sha256",QString::fromLatin1(QCryptographicHash::hash(content,QCryptographicHash::Sha256).toHex())},
            {"bytes",content.size()},{"book",book},{"syncStatus",status}};
        QFile fileData(scope+"/"+QString::number(id)+".epub"),metadata(scope+"/records/"+QString::number(id)+".json");
        require(fileData.open(QIODevice::WriteOnly) && fileData.write(content)==content.size() && metadata.open(QIODevice::WriteOnly),"language record fixture");
        const auto bytes=QJsonDocument(record).toJson(QJsonDocument::Compact);
        require(metadata.write(bytes)==bytes.size(),"persist language record fixture");
    }
    ClientWorkerCheck::reload(client);
    const auto http=uiMessage("server_http_error").arg(503);
    ClientWorkerCheck::saveSyncMessage(client,"1",http);
    QFile stored(scope+"/records/1.json"); require(stored.open(QIODevice::ReadOnly),"read status written by finish");
    const auto savedStatus=QJsonDocument::fromJson(stored.readAll()).object()["syncStatus"].toObject(); stored.close();
    require(savedStatus["message"].toObject()["code"]=="server_http_error" && savedStatus["message"].toObject()["params"].toArray()[0]==503 && !savedStatus.contains("text"),"finish persists code and parameters without translated text");
    auto verifyStatuses=[&](Client &subject,const QString &expected,const QString &fallback,const QString &synced) {
        QMap<int,QString> reasons;
        for(const auto &book:subject.syncBooks()) for(const auto &value:book.toMap()["files"].toList()) {
            const auto file=value.toMap(); reasons[file["fileId"].toInt()]=file["reason"].toString();
        }
        require(reasons[1]==expected && reasons[2]==expected && reasons[3]==expected,"coded and legacy HTTP statuses render in the current language");
        require(reasons[4]==synced && reasons[5]==fallback && reasons[6]==fallback && reasons[7]==fallback,"legacy static text and invalid messages use safe localization");
    };
    UpdateManager updater(&client,storage.path(),QCoreApplication::applicationFilePath());
    updater.install();
    QQmlApplicationEngine engine;
#ifdef POCKETBOOK_DEVICE
    engine.addImportPath("/ebrmain/qml");
#endif
    int warnings = 0;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app, [&](const QList<QQmlError> &errors) {
        warnings += errors.size();
        for (const auto &error : errors) std::fprintf(stderr, "%s\n", qPrintable(error.toString()));
    });
    QObject::connect(&client, &Client::languageChanged, &engine, [&] {
        translation.apply("de_DE", client.language());
        client.retranslate(); updater.retranslate(); engine.retranslate();
    });
    engine.rootContext()->setContextProperty("client", &client);
    engine.rootContext()->setContextProperty("updateManager", &updater);
    engine.rootContext()->setContextProperty("screenWidth", screen.width());
    engine.rootContext()->setContextProperty("screenHeight", screen.height());
    engine.load(QUrl("qrc:/Main.qml"));
    require(!engine.rootObjects().isEmpty(), "production QML loaded");
    auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    require(window, "application window");
    auto item = [&](const char *name) {
        auto object = window->findChild<QObject *>(name);
        require(object, name);
        return object;
    };
    auto snapshot = [&](const QString &name) {
        settle();
        if (argc > 1) {
            const auto image = window->grabWindow();
            require(!image.isNull() && image.save(QString::fromLocal8Bit(argv[1])+"/"+name+".png"), "rendered screenshot saved");
        }
    };
    require(item("catalogTab")->property("text").toString() == "Catalog", "unsupported system renders English");
    snapshot("catalog-en");
    window->setProperty("settings", true);
    snapshot("settings-en");
    require(QMetaObject::invokeMethod(item("languageSelector"), "activated", Q_ARG(int, 2)), "choose Russian through settings");
    settle();
    require(client.language() == "ru" && item("catalogTab")->property("text").toString() == "Каталог", "settings change translates QML immediately");
    require(client.status() == "Язык сохранён.", "C++ feedback translated immediately");
    require(translatedText("Progress synced") == "Прогресс синхронизирован", "saved English sync result translated to Russian");
    const auto russianHttp=QString::fromUtf8("Сервер ответил HTTP 503. Проверьте адрес и повторите.");
    const auto russianFallback=QString::fromUtf8("Подробности предыдущего результата недоступны. Повторите операцию.");
    verifyStatuses(client,russianHttp,russianFallback,QString::fromUtf8("Прогресс синхронизирован"));
    require(item("updateMessage")->property("text").toString()==updateDeviceError().text(),"stored OTA status retranslates in actual QML");
    ClientWorkerCheck::memoryMessages(client,http); settle();
    require(window->property("uiMessage").value<QJSValue>().toVariant().toMap()["text"]==russianHttp && client.sessionWarning()==russianHttp && client.diagnosticError()==russianHttp,"memory messages and QML feedback use the current language");
    translation.apply("de_DE","en"); client.retranslate(); updater.retranslate(); engine.retranslate(); settle();
    const QString englishHttp="The server returned HTTP 503. Check the address and try again.";
    require(client.status()==englishHttp && client.sessionWarning()==englishHttp && client.diagnosticError()==englishHttp && window->property("uiMessage").value<QJSValue>().toVariant().toMap()["text"]==englishHttp,"translation change updates existing memory messages without recreating them");
    translation.apply("de_DE","ru"); client.retranslate(); updater.retranslate(); engine.retranslate(); settle();
    require(client.status()==russianHttp,"switching back restores the same dynamic message");
    require(uiMessage("server_not_found_1_check_wi_fi_and_dns").arg("host%2").text().contains("host%2"),"parameter percent signs are not interpreted as placeholders");
    const auto summary=messageForSource("%1. Files: %2; matching: %3; different: %4; errors: %5; unchecked: %6. See book details for results.")
        .arg(uiMessage("verification_cancelled")).arg(3).arg(1).arg(1).arg(0).arg(1);
    require(summary.text().contains(QString::fromUtf8("Проверка отменена")) && UiMessage::fromJson(summary.toJson()).text()==summary.text(),"nested summary message and counters survive serialization");
    snapshot("settings-ru");
    {
        Client restarted(QUrl("https://example.invalid"), storage.path());
        require(restarted.language() == "ru", "explicit choice survives restart");
        verifyStatuses(restarted,russianHttp,russianFallback,QString::fromUtf8("Прогресс синхронизирован"));
        require(restarted.setDiagnosticLogging(true), "other preference can be saved");
        require(!restarted.setLanguage("de") && restarted.language() == "ru", "unsupported manual choice rejected");
    }
    require(client.setLanguage("en"), "switch to English");
    settle();
    require(item("catalogTab")->property("text").toString() == "Catalog" && client.status() == "Language saved.", "English restored without restart");
    verifyStatuses(client,englishHttp,"Previous result details are unavailable. Retry the operation.","Progress synced");
    { Client restarted(QUrl("https://example.invalid"),storage.path()); verifyStatuses(restarted,englishHttp,"Previous result details are unavailable. Retry the operation.","Progress synced"); }
    require(translatedText("Прогресс синхронизирован") == "Progress synced", "legacy Russian sync result translated to English");
    QFile preferences(storage.path()+"/preferences.json");
    require(preferences.open(QIODevice::ReadOnly), "saved preferences readable");
    const auto saved = QJsonDocument::fromJson(preferences.readAll()).object();
    require(saved["language"] == "en" && saved["diagnosticLogging"].toBool(), "language save preserves other preferences");
    preferences.close();
    window->setProperty("addingConnection", true);
    snapshot("connection-en");
    window->setProperty("addingConnection", false);
    require(client.setLanguage("system") && client.language() == "system", "manual override can be cleared");
    require(item("catalogTab")->property("text").toString() == "Catalog", "return to unsupported system uses English");
    require(QFile::remove(storage.path()+"/preferences.json") && QDir().mkdir(storage.path()+"/preferences.json"), "simulate preference write failure");
    require(!client.setLanguage("ru") && client.language() == "system", "failed save keeps previous language");
    require(client.status() == "Could not save the language setting.", "language save error is visible");
    require(warnings == 0, "no QML warnings in either language");
    std::puts("PASS: language checks");
}
