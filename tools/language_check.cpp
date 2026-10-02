#include "client.h"
#include "device.h"
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
        engine.retranslate();
    });
    engine.rootContext()->setContextProperty("client", &client);
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
    snapshot("settings-ru");
    {
        Client restarted(QUrl("https://example.invalid"), storage.path());
        require(restarted.language() == "ru", "explicit choice survives restart");
        require(restarted.setDiagnosticLogging(true), "other preference can be saved");
        require(!restarted.setLanguage("de") && restarted.language() == "ru", "unsupported manual choice rejected");
    }
    require(client.setLanguage("en"), "switch to English");
    settle();
    require(item("catalogTab")->property("text").toString() == "Catalog" && client.status() == "Language saved.", "English restored without restart");
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
