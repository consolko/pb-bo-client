#pragma once
#include <QCoreApplication>
#include <QLocale>
#include <QTranslator>
#include <QMap>

// Add each supported language here and embed its Qt catalog in qml/app.qrc.
inline const QMap<QString, QString> &interfaceLanguages() {
    static const QMap<QString, QString> languages{{"en", "English"}, {"ru", "Русский"}};
    return languages;
}

inline QString interfaceLanguage(QString systemLanguage, const QString &preference) {
    if (interfaceLanguages().contains(preference)) return preference;
    systemLanguage = systemLanguage.trimmed().toLower().replace('-', '_');
    // PocketBook uses ISO codes; accept locale names and the older firmware name too.
    if (systemLanguage == "russian") systemLanguage = "ru";
    const auto code = systemLanguage.section('_', 0, 0).section('.', 0, 0).section('@', 0, 0);
    return interfaceLanguages().contains(code) ? code : QStringLiteral("en");
}

inline QString translatedText(const QString &text) {
    static QTranslator legacy;
    static const bool loaded = legacy.load(":/translations/bookorbit_en.qm");
    const QString source = loaded ? legacy.translate("Legacy", text.toUtf8().constData()) : QString{};
    return QCoreApplication::translate("BookOrbit", (source.isEmpty() ? text : source).toUtf8().constData());
}

class InterfaceTranslation {
public:
    void apply(const QString &systemLanguage, const QString &preference) {
        QCoreApplication::removeTranslator(&translator);
        const QString language = interfaceLanguage(systemLanguage, preference);
        if (language != "en") {
            if (!translator.load(":/translations/bookorbit_"+language+".qm")) qFatal("Missing translation catalog");
            QCoreApplication::installTranslator(&translator);
        }
        QLocale::setDefault(QLocale(language));
    }
private:
    QTranslator translator;
};
