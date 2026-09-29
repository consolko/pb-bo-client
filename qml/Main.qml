import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    visible: true
    width: screenWidth
    height: screenHeight
    title: "BookOrbit"
    color: "white"
    property real u: width / 600
    property bool fullDescription: false
    property bool editionExpanded: false
    property real savedCatalogY: 0
    property int pendingFileId: 0
    property bool formatDownload: false
    property bool settings: false
    property bool addingConnection: false
    property bool browsingFolders: false
    property string folderPath: "/mnt/ext1"
    property real keyboardSpace: Qt.inputMethod.visible ? Math.max(Qt.inputMethod.keyboardRectangle.height, height * 0.4) : 0
    font.pixelSize: 18 * u

    function showBook(id) {
        savedCatalogY = catalog.contentY
        fullDescription = false; editionExpanded = false
        client.showDetail(id)
        detailScroll.contentItem.contentY = 0
    }
    function selectedFormat(id) {
        const files = client.detail.files || []
        const wanted = id === undefined ? client.detail.fileId : id
        for (let i = 0; i < files.length; ++i) if (files[i].id === wanted) return files[i]
        return {}
    }
    function chooseFormat(download) {
        pendingFileId = client.detail.fileId || 0
        formatDownload = download
        formatDialog.open()
    }
    function readStatusLabel(book) {
        const status = book.readStatus ? book.readStatus.status : "unread"
        const labels = { unread: "Не прочитано", want_to_read: "Хочу прочитать", reading: "Читаю", on_hold: "Отложено", rereading: "Перечитываю", read: "Прочитано", skimmed: "Просмотрено", abandoned: "Чтение прекращено" }
        let label = labels[status] || ""
        const progress = book.readingProgress
        if ((status === "reading" || status === "rereading") && typeof progress === "number" && isFinite(progress) && progress >= 0 && progress <= 100)
            label += " · " + progress.toFixed(1).replace(/\.0$/, "").replace(".", ",") + " %"
        return label
    }
    function editionText() {
        const d = client.detail, lines = []
        function field(name, value) { if (value) lines.push(name + ": " + value) }
        field("Издательство", d.publisher)
        field("Издано", d.publishedDate || d.publishedYear)
        field("Язык", ({ru:"Русский", en:"Английский", de:"Немецкий", fr:"Французский"})[d.language] || d.language)
        field("Объём издания", d.pageCount ? d.pageCount + " стр." : "")
        field("ISBN", d.isbn13 || d.isbn10)
        field("Библиотека", d.libraryName)
        field("Жанры", (d.genres || []).join(", "))
        field("Теги", (d.tags || []).join(", "))
        const series = d.seriesMemberships || []
        if (series.length > 0) field("Серии", series.map(s => s.seriesName + (s.seriesIndex ? " · № " + s.seriesIndex : "")).join("; "))
        for (const rating of d.communityRatings || []) field("Оценка " + rating.provider, rating.rating)
        return lines.length ? lines.join("\n") : "Дополнительные сведения не указаны"
    }
    function pageContent(direction) {
        const target = client.detailVisible ? detailScroll.contentItem : client.collectionsView ? collectionList : catalog
        target.contentY = Math.max(0, Math.min(Math.max(0, target.contentHeight-target.height), target.contentY + direction*target.height*0.85))
    }
    function backCollection() {
        const saved = client.backFromCollection()
        if (saved.query !== undefined) {
            search.text = saved.query; savedCatalogY = saved.catalogY
            Qt.callLater(function() { detailScroll.contentItem.contentY = saved.detailY })
        } else { search.text = ""; client.showCollections() }
    }
    function revealField(field) {
        if (!settings || !field || !field.activeFocus || !field.mapToItem) return
        const y = field.mapToItem(settingsScroll, 0, 0).y
        const bottom = y + field.height + 12 * u
        if (bottom > settingsScroll.height)
            settingsScroll.contentItem.contentY += bottom - settingsScroll.height
        else if (y < 12 * u)
            settingsScroll.contentItem.contentY = Math.max(0, settingsScroll.contentItem.contentY + y - 12 * u)
    }

    function editConnection() {
        addingConnection = false
        settings = true
    }
    function connectionForm(fresh) {
        server.text = fresh ? "https://" : client.server
        username.text = fresh ? "" : client.username
        password.text = ""
        addingConnection = true
        settingsScroll.contentItem.contentY = 0
    }
    function back() {
        if (formatDialog.visible) { formatDialog.close(); return }
        if (Qt.inputMethod.visible) { Qt.inputMethod.hide(); return }
        if (browsingFolders) { browsingFolders = false; return }
        if (addingConnection) { addingConnection = false; password.text = ""; return }
        if (settings) { settings = false; password.text = ""; Qt.inputMethod.hide() }
        else if (client.detailVisible) { client.closeDetail(); Qt.callLater(function() { catalog.contentY = savedCatalogY }) }
        else if (client.collectionId > 0 && !client.busy) window.backCollection()
        else if (!client.busy) Qt.quit()
    }
    component Action: Button {
        id: action
        implicitHeight: 48 * window.u
        font.pixelSize: 17 * window.u
        padding: 10 * window.u
        Accessible.name: text
        ToolTip.text: text
        ToolTip.visible: hovered && icon.source.toString().length > 0
        ToolTip.delay: 600
        background: Rectangle {
            color: action.down || action.checked ? (action.icon.source.toString().length ? "#dddddd" : "black") : "white"
            border.color: action.enabled ? "black" : "#999999"
            border.width: action.activeFocus ? 3 : 1
            radius: 3 * window.u
        }
        contentItem: Item {
            implicitWidth: action.icon.source.toString().length ? 28 * window.u : buttonText.implicitWidth
            implicitHeight: Math.max(buttonText.implicitHeight, 28 * window.u)
            Text {
                id: buttonText
                anchors.fill: parent
                visible: !action.icon.source.toString().length
                text: action.text
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font: action.font
                color: !action.enabled ? "#777777" : (action.down || action.checked ? "white" : "black")
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            Image {
                objectName: action.objectName + "Icon"
                anchors.centerIn: parent
                width: 28 * window.u; height: width
                source: action.icon.source
                visible: source.toString().length > 0
                sourceSize.width: width; sourceSize.height: height
                opacity: action.enabled ? 1 : 0.4
                Accessible.ignored: true
            }
        }
    }
    component Field: TextField {
        Layout.fillWidth: true
        implicitHeight: 48 * window.u
        font.pixelSize: 18 * window.u
        enabled: !client.busy
        selectByMouse: true
        onAccepted: Qt.inputMethod.hide()
        onActiveFocusChanged: if (activeFocus) Qt.callLater(window.revealField, this)
        color: "black"
        background: Rectangle { color: "white"; border.color: "black"; border.width: parent.activeFocus ? 3 : 1 }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Math.round(20 * window.u)
        spacing: 12 * window.u
        focus: true
        Keys.onEscapePressed: window.back()
        Keys.onBackPressed: window.back()
        Keys.onPressed: event => {
            if (!window.settings && (event.key === Qt.Key_PageDown || event.key === Qt.Key_Right)) {
                window.pageContent(1)
                event.accepted = true
            } else if (!window.settings && (event.key === Qt.Key_PageUp || event.key === Qt.Key_Left)) {
                window.pageContent(-1)
                event.accepted = true
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { text: window.browsingFolders ? "Папка для книг" : (window.addingConnection ? "Подключение" : (window.settings ? "Настройки" : client.detailVisible ? "О книге" : "BookOrbit")); font.pixelSize: 29 * window.u; font.bold: true; Layout.fillWidth: true }
            Action {
                objectName: "syncButton"
                visible: !window.settings && !client.detailVisible
                text: "Синхронизировать"
                icon.source: "qrc:/icons/sync.svg"
                Accessible.name: "Синхронизировать прогресс скачанных EPUB"
                enabled: !client.busy && client.authenticated
                onClicked: client.syncAll()
            }
            Action {
                objectName: "navigationButton"
                text: window.settings || client.detailVisible ? "Назад" : "Настройки"
                icon.source: window.settings || client.detailVisible ? "" : "qrc:/icons/settings.svg"
                enabled: !client.busy
                onClicked: window.settings || client.detailVisible ? window.back() : window.editConnection()
            }
            Action { objectName: "exitButton"; visible: !window.settings && !client.detailVisible; text: "Закрыть приложение"; icon.source: "qrc:/icons/exit.svg"; enabled: !client.busy; onClicked: Qt.quit() }
        }
        Text {
            Layout.fillWidth: true
            visible: window.settings
            text: client.username + " · " + client.server
            font.pixelSize: 14 * window.u
            color: "#444444"
            maximumLineCount: 2
            elide: Text.ElideRight
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            text: client.status
            visible: text.length > 0
            font.pixelSize: 15 * window.u
            font.bold: client.busy
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
        ScrollView {
            id: settingsScroll
            visible: window.settings && !window.browsingFolders
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.bottomMargin: window.keyboardSpace
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: 8 * window.u
                ColumnLayout {
                    visible: window.addingConnection
                    Layout.fillWidth: true
                    spacing: 8 * window.u
                Text { text: "Адрес сервера"; font.pixelSize: 16 * window.u }
                Field { id: server; inputMethodHints: Qt.ImhUrlCharactersOnly; Accessible.name: "Адрес сервера" }
                Text { text: "Имя пользователя"; font.pixelSize: 16 * window.u }
                Field { id: username; inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText; Accessible.name: "Имя пользователя" }
                Text { text: "Пароль для входа"; font.pixelSize: 16 * window.u }
                Field { id: password; objectName: "passwordField"; echoMode: showPassword.checked ? TextInput.Normal : TextInput.Password; inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText; Accessible.name: "Пароль" }
                CheckBox {
                    id: showPassword
                    objectName: "showPassword"
                    text: "Показать пароль"
                    font.pixelSize: 16 * window.u
                    implicitHeight: 44 * window.u
                    enabled: !client.busy
                    onVisibleChanged: if (!visible) checked = false
                }
                Text {
                    text: client.hasSavedPassword ? "Вход сохранён на устройстве до выхода из аккаунта." : "После входа пароль сохранится на устройстве до выхода из аккаунта."
                    Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444"
                }
                Action {
                    text: "Войти"; Layout.fillWidth: true
                    enabled: !client.busy && username.text.length > 0 && (password.text.length > 0 || client.hasSavedPassword && username.text === client.username && server.text === client.server)
                    onClicked: {
                        if (client.configure(server.text, username.text)) {
                            client.login(username.text, password.text)
                            password.text = ""
                            Qt.inputMethod.hide()
                        }
                    }
                }
                }
                ColumnLayout {
                    visible: !window.addingConnection
                    Layout.fillWidth: true
                    spacing: 8 * window.u
                    Text { text: "Сохранённые подключения"; font.pixelSize: 18 * window.u; font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            Layout.fillWidth: true
                            implicitHeight: 48 * window.u
                            font.pixelSize: 15 * window.u
                            model: client.accounts
                            currentIndex: client.accounts.indexOf(client.username + " · " + client.server)
                            enabled: !client.busy && count > 0
                            Accessible.name: "Сохранённые подключения"
                            displayText: count > 0 ? currentText : "Подключений пока нет"
                            onActivated: client.selectAccount(currentIndex)
                        }
                        Action { objectName: "addConnection"; text: "Добавить"; enabled: !client.busy; onClicked: window.connectionForm(true) }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8 * window.u
                        Action {
                            objectName: "connectButton"
                            text: "Подключиться"; Layout.fillWidth: true; Layout.preferredWidth: 1
                            enabled: !client.busy && client.accounts.length > 0 && !client.authenticated
                            onClicked: client.hasSavedPassword ? client.restoreSession() : window.connectionForm(false)
                        }
                        Action {
                            objectName: "logoutButton"
                            text: "Выйти из аккаунта"; Layout.fillWidth: true; Layout.preferredWidth: 1
                            enabled: !client.busy && (client.authenticated || client.hasSavedPassword)
                            onClicked: client.logout()
                        }
                    }
                    Item { Layout.preferredHeight: 10 * window.u }
                Text { text: "Папка для скачивания книг"; font.pixelSize: 18 * window.u; font.bold: true }
                Text { text: client.downloadDirectory; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 14 * window.u }
                Action { text: "Выбрать папку…"; Layout.fillWidth: true; enabled: !client.busy; onClicked: { Qt.inputMethod.hide(); window.browsingFolders = true } }
                Text { text: "Для новых загрузок. Уже скачанные книги останутся на своих местах."; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
                CheckBox {
                    text: "Записывать диагностический журнал"
                    checked: client.diagnosticLogging
                    enabled: !client.busy
                    onClicked: client.setDiagnosticLogging(checked)
                }
                Text { objectName: "diagnosticPath"; text: client.diagnosticLogPath; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 14 * window.u; color: "#444444" }
                }
            }
        }
        ColumnLayout {
            visible: window.browsingFolders
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12 * window.u
            Text { text: "Внутренняя память"; font.pixelSize: 18 * window.u; font.bold: true }
            Text { text: window.folderPath; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 15 * window.u; Accessible.name: "Текущая папка: " + text }
            Action {
                text: "↑ На уровень выше"; Layout.fillWidth: true
                enabled: window.folderPath !== "/mnt/ext1"
                onClicked: window.folderPath = window.folderPath.slice(0, window.folderPath.lastIndexOf("/"))
            }
            ListView {
                id: folders
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 8 * window.u
                model: client.directories(window.folderPath)
                ScrollBar.vertical: ScrollBar { }
                delegate: Action {
                    required property var modelData
                    width: folders.width
                    text: modelData.name + "  ›"
                    Accessible.name: "Открыть папку " + modelData.name
                    onClicked: window.folderPath = modelData.path
                    contentItem: Text {
                        text: parent.text; font: parent.font; color: parent.down ? "white" : "black"
                        elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter
                    }
                }
                Text { anchors.centerIn: parent; width: parent.width; visible: folders.count === 0; text: "Нет доступных вложенных папок"; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 17 * window.u }
            }
            Action {
                text: "Скачивать в эту папку"; Layout.fillWidth: true
                onClicked: { if (client.setDownloadDirectory(window.folderPath)) window.browsingFolders = false }
            }
            Action { text: "Отмена"; Layout.fillWidth: true; onClicked: window.browsingFolders = false }
        }
        RowLayout {
            visible: !window.settings && !client.detailVisible
            Layout.fillWidth: true
            spacing: 8 * window.u
            Action { objectName: "downloadsTab"; text: "Скачанные"; checked: client.offlineOnly; Layout.fillWidth: true; enabled: !client.busy; onClicked: { search.text = ""; client.showDownloaded(true) } }
            Action { objectName: "catalogTab"; text: "Каталог"; checked: !client.offlineOnly && !client.collectionsView && client.collectionId === 0; Layout.fillWidth: true; enabled: !client.busy; onClicked: { search.text = ""; client.showDownloaded(false) } }
            Action { objectName: "collectionsTab"; text: "Коллекции"; checked: client.collectionsView || client.collectionId > 0; Layout.fillWidth: true; enabled: !client.busy; onClicked: { search.text = ""; client.showCollections() } }
        }
        RowLayout {
            visible: !window.settings && !client.detailVisible && client.collectionId > 0
            Layout.fillWidth: true
            Action { text: "‹ Назад"; enabled: !client.busy; onClicked: window.backCollection() }
            Text { text: client.collectionName; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; font.bold: true; font.pixelSize: 20 * window.u }
        }
        RowLayout {
            visible: !window.settings && !client.offlineOnly && !client.collectionsView && !client.detailVisible
            Layout.fillWidth: true
            Field { id: search; objectName: "searchField"; placeholderText: "Название, автор или серия"; onAccepted: { Qt.inputMethod.hide(); client.refresh(0, text) } }
            Action { objectName: "searchButton"; text: "Найти"; enabled: !client.busy && client.authenticated; onClicked: { Qt.inputMethod.hide(); client.refresh(0, search.text) } }
        }
        RowLayout {
            visible: !window.settings && !client.authenticated
            Layout.fillWidth: true
            Text { text: "Скачанные книги доступны без входа"; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
            Action { text: "Войти"; enabled: !client.busy; onClicked: window.editConnection() }
        }
        ListView {
            id: collectionList
            objectName: "collectionList"
            visible: !window.settings && client.collectionsView && !client.detailVisible
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: client.collections
            ScrollBar.vertical: ScrollBar { }
            delegate: ItemDelegate {
                required property var modelData
                objectName: "collection-" + modelData.id
                width: collectionList.width
                height: (modelData.isOwner ? 66 : 84) * window.u
                onClicked: { search.text = ""; catalog.contentY = 0; client.openCollection(modelData.id, modelData.name) }
                enabled: !client.busy
                background: Rectangle { color: parent.down ? "#dddddd" : "white"; Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#aaaaaa" } }
                contentItem: RowLayout {
                    ColumnLayout {
                        Layout.fillWidth: true
                        Text { text: modelData.name; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; font.pixelSize: 21 * window.u }
                        Text { text: "Общая коллекция"; visible: !modelData.isOwner; font.pixelSize: 14 * window.u; color: "#444444" }
                    }
                    Text { text: modelData.bookCount + "  ›"; font.pixelSize: 18 * window.u }
                }
                Accessible.name: modelData.name + ", книг: " + modelData.bookCount
            }
            Text { anchors.centerIn: parent; width: parent.width; visible: collectionList.count === 0 && !client.busy; text: client.authenticated ? (client.canRetry ? "Не удалось загрузить коллекции.\nНажмите «Повторить»." : "Доступных коллекций пока нет") : "Войдите для просмотра коллекций"; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 19 * window.u }
        }
        ListView {
            id: catalog
            objectName: "catalog"
            visible: !window.settings && !client.detailVisible && !client.collectionsView
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: client.books
            spacing: Math.round(6 * window.u)
            ScrollBar.vertical: ScrollBar { }
            delegate: ItemDelegate {
                id: bookRow
                required property var modelData
                required property int index
                objectName: "book-" + modelData.bookId
                width: catalog.width
                // Fractional row edges leave hairlines in the firmware's software renderer.
                height: Math.ceil(Math.max(130 * window.u, implicitContentHeight + topPadding + bottomPadding))
                padding: 0; topPadding: Math.round(8 * window.u); bottomPadding: Math.round(10 * window.u)
                enabled: !client.busy
                onClicked: window.showBook(modelData.bookId)
                Accessible.name: "О книге: " + modelData.title + ", " + window.readStatusLabel(modelData)
                background: Rectangle {
                    color: bookRow.down ? "#dddddd" : "white"
                    Rectangle { anchors.fill: parent; visible: bookRow.visualFocus; color: "transparent"; border.color: "black"; border.width: 2 }
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#aaaaaa" }
                }
                contentItem: RowLayout {
                    spacing: 14 * window.u
                    Rectangle {
                        objectName: "cover-" + modelData.bookId
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 70 * window.u; Layout.preferredHeight: 105 * window.u
                        color: "#eeeeee"
                        Text { anchors.centerIn: parent; visible: cover.status !== Image.Ready; text: "Книга"; font.pixelSize: 13 * window.u; color: "#555555" }
                        Image { id: cover; anchors.fill: parent; source: { const revision = client.coverRevision; return client.coverUrl(modelData.bookId) }
                            fillMode: Image.PreserveAspectFit; verticalAlignment: Image.AlignTop; cache: false; sourceSize.width: 240; sourceSize.height: 320 }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.minimumHeight: 105 * window.u; Layout.alignment: Qt.AlignTop; spacing: 4 * window.u
                        Text { objectName: "title-" + modelData.bookId; text: modelData.title; Layout.fillWidth: true; textFormat: Text.PlainText; font.pixelSize: 21 * window.u; font.bold: true; maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight }
                        Text { objectName: "author-" + modelData.bookId; text: modelData.author; visible: text.trim().length > 0; textFormat: Text.PlainText; Layout.fillWidth: true; font.pixelSize: 15 * window.u; elide: Text.ElideRight }
                        Text { objectName: "reading-" + modelData.bookId; text: window.readStatusLabel(modelData); visible: text.length > 0; Layout.fillWidth: true; font.pixelSize: 14 * window.u; elide: Text.ElideRight }
                        Item { Layout.fillHeight: true }
                        Text {
                            text: modelData.hasConflict ? "Выберите позицию" : modelData.pendingProgress ? "Позиция ожидает применения" : modelData.localFormats ? modelData.localFormats + " · на устройстве" : modelData.formats || "Нет файлов книги"
                            Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; font.pixelSize: 13 * window.u
                        }
                    }
                    Item {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: bookAction.implicitWidth
                        Layout.preferredHeight: 105 * window.u
                        Action {
                            id: bookAction
                            anchors.bottom: parent.bottom
                            objectName: "bookAction-" + modelData.bookId
                            text: modelData.downloaded ? (modelData.readable ? "Читать" : "О книге") : modelData.needsRepair ? "Заново" : "Скачать"
                            icon.source: modelData.downloaded ? "" : "qrc:/icons/download.svg"
                            Accessible.name: (modelData.downloaded ? text : modelData.needsRepair ? "Скачать заново" : "Скачать") + ": " + modelData.title
                            enabled: !client.busy && (modelData.downloaded || modelData.supported && client.authenticated)
                            onClicked: {
                                if (modelData.downloaded && modelData.readable && !modelData.pendingProgress) client.open(index)
                                else if (modelData.downloaded) window.showBook(modelData.bookId)
                                else if (modelData.fileCount > 1) { window.showBook(modelData.bookId); window.chooseFormat(true) }
                                else client.download(index)
                            }
                        }
                    }
                }
            }
            Column {
                anchors.centerIn: parent; width: parent.width - 20 * window.u; spacing: 14 * window.u
                visible: catalog.count === 0 && !client.busy
                Text {
                    width: parent.width
                    text: client.offlineOnly ? "Скачанных книг пока нет.\nВыберите книгу в каталоге." : !client.authenticated ? "Войдите, чтобы загрузить книги." : client.canRetry ? "Не удалось загрузить книги.\nНажмите «Повторить»." : search.text.length ? "По вашему запросу ничего не найдено." : client.collectionId > 0 ? "В этой коллекции пока нет доступных книг." : "В каталоге пока нет доступных книг."
                    wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 19 * window.u; color: "#444444"
                }
                Action { visible: search.text.length > 0; anchors.horizontalCenter: parent.horizontalCenter; text: "Сбросить поиск"; enabled: !client.busy; onClicked: { search.text = ""; client.refresh() } }
            }
        }
        ScrollView {
            id: detailScroll
            objectName: "detailScroll"
            visible: !window.settings && client.detailVisible
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: detailScroll.availableWidth
                spacing: 14 * window.u
                RowLayout {
                    Layout.fillWidth: true; spacing: 18 * window.u
                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 108 * window.u; Layout.preferredHeight: 162 * window.u
                        color: "#eeeeee"
                        Text { anchors.centerIn: parent; visible: detailCover.status !== Image.Ready; text: "Книга"; font.pixelSize: 16 * window.u }
                        Image { id: detailCover; anchors.fill: parent; source: { const revision = client.coverRevision; return client.coverUrl(client.detail.bookId || 0) }
                            fillMode: Image.PreserveAspectFit; cache: false }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; spacing: 8 * window.u
                        Text { text: client.detail.title || ""; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.bold: true; font.pixelSize: 25 * window.u }
                        Text { text: client.detail.subtitle || ""; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 17 * window.u }
                        Text { text: client.detail.author || ""; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 * window.u }
                        Text { objectName: "detailReading"; text: window.readStatusLabel(client.detail); visible: text.length > 0; Layout.fillWidth: true; font.pixelSize: 15 * window.u; wrapMode: Text.Wrap }
                        Text { text: (client.detail.seriesName || "") + (client.detail.seriesIndex ? " · № " + client.detail.seriesIndex : ""); textFormat: Text.PlainText; visible: !!client.detail.seriesName; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 15 * window.u }
                    }
                }
                Action {
                    Layout.fillWidth: true
                    objectName: "formatButton"
                    text: "Формат: " + (window.selectedFormat().format || "нет файлов") + (window.selectedFormat().sizeLabel ? " · " + window.selectedFormat().sizeLabel : "") + ((client.detail.files || []).length > 1 ? "  ▾" : "")
                    enabled: !client.busy && (client.detail.files || []).length > 1
                    onClicked: window.chooseFormat(false)
                }
                Action {
                    Layout.fillWidth: true; checked: true
                    objectName: "downloadButton"
                    text: client.detail.downloaded ? (client.detail.readable ? "Читать " + client.detail.format : "Скачано") : client.detail.needsRepair ? "Скачать заново" : "Скачать " + (client.detail.format || "")
                    enabled: !client.busy && (client.detail.downloaded ? !!client.detail.readable : !!client.detail.supported && client.authenticated)
                    onClicked: client.detail.downloaded ? client.openSelected() : client.downloadSelected()
                }
                Text { text: client.detail.downloaded ? "Скачано на устройство" : ""; visible: text.length > 0; Layout.fillWidth: true; font.pixelSize: 14 * window.u }
                Text { text: !client.detail.supported && client.detail.format ? "Размер файла превышает лимит загрузки 100 МБ." : !client.detail.readable && client.detail.format ? "Можно скачать; открытие этого формата из клиента пока не поддерживается." : client.detail.format && client.detail.format !== "EPUB" ? "Обмен позицией для этого формата пока не поддерживается." : ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
                Text { text: client.detail.hasConflict ? "На ридере и в BookOrbit разные позиции. Повторите сверку и выберите нужную." : client.detail.pendingProgress ? "Входящая позиция ещё не применена. Закройте книгу в читалке и повторите сверку." : client.detail.syncResult || ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                Action { text: "Читать локально"; visible: !!client.detail.pendingProgress; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.openSelected(false) }
                Action { text: "Синхронизировать позицию"; visible: !!client.detail.canSync; Layout.fillWidth: true; enabled: !client.busy && client.authenticated; onClicked: client.syncSelected() }
                Text { text: client.detail.notice || ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
                Action { text: client.detail.detailed ? "Обновить сведения" : "Загрузить сведения"; visible: client.offlineOnly || !!client.detail.notice; enabled: !client.busy && client.authenticated; onClicked: client.refreshDetail() }
                Text { text: "Аннотация"; font.bold: true; font.pixelSize: 20 * window.u }
                Text {
                    text: client.detail.description || (client.detail.detailed ? "Аннотации в BookOrbit нет" : "Сведения ещё не загружены")
                    textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 * window.u
                    maximumLineCount: window.fullDescription ? 100000 : 6; elide: Text.ElideRight
                }
                Action { text: window.fullDescription ? "Свернуть аннотацию" : "Читать полностью"; visible: !!client.detail.description; onClicked: window.fullDescription = !window.fullDescription }
                Action { text: "Об издании  " + (window.editionExpanded ? "▾" : "›"); Layout.fillWidth: true; onClicked: window.editionExpanded = !window.editionExpanded }
                Text { text: window.editionText(); textFormat: Text.PlainText; visible: window.editionExpanded; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 17 * window.u }
                Text { text: "Моя оценка: " + client.detail.rating + " из 5"; visible: !!client.detail.rating; font.pixelSize: 17 * window.u }
                Text { text: "Личная заметка"; visible: !!client.detail.personalNote; font.bold: true; font.pixelSize: 20 * window.u }
                Text { text: client.detail.personalNote || ""; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 17 * window.u }
                Text { text: "В моих коллекциях"; visible: (client.detail.collections || []).length > 0; font.bold: true; font.pixelSize: 20 * window.u }
                Repeater {
                    model: client.detail.collections || []
                    Action {
                        required property var modelData
                        text: modelData.name + "  ›"; Layout.fillWidth: true; enabled: !client.busy
                        onClicked: { client.openCollection(modelData.id, modelData.name, window.savedCatalogY, detailScroll.contentItem.contentY); search.text = "" }
                    }
                }
            }
        }
        RowLayout {
            visible: !window.settings && !client.offlineOnly && !client.collectionsView && !client.detailVisible
            Layout.fillWidth: true
            Action { objectName: "previousPage"; text: "‹"; Layout.preferredWidth: 100 * window.u; Accessible.name: "Предыдущая страница"; enabled: !client.busy && client.authenticated && client.page > 0; onClicked: { catalog.contentY = 0; client.refresh(client.page-1, search.text) } }
            Text { text: client.total === 0 ? "Книг: 0" : (client.page+1) + " / " + Math.ceil(client.total/10) + " · " + (search.text ? "Найдено: " : "Книг: ") + client.total; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 14 * window.u }
            Action { objectName: "nextPage"; text: "›"; Layout.preferredWidth: 100 * window.u; Accessible.name: "Следующая страница"; enabled: !client.busy && client.authenticated && (client.page+1)*10 < client.total; onClicked: { catalog.contentY = 0; client.refresh(client.page+1, search.text) } }
        }
        Action { visible: client.downloading; text: "Отменить загрузку"; Layout.fillWidth: true; onClicked: client.cancelDownload() }
        Action { visible: client.canRetry; text: "Повторить"; Layout.fillWidth: true; onClicked: client.retry() }
    }
    Dialog {
        id: formatDialog
        objectName: "formatDialog"
        anchors.centerIn: parent
        width: parent.width - 40 * window.u
        modal: true
        closePolicy: Popup.CloseOnEscape
        header: Text { text: "Выберите формат"; padding: 16 * window.u; font.pixelSize: 22 * window.u; font.bold: true }
        contentItem: ColumnLayout {
            spacing: 12 * window.u
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, window.height * 0.5)
                clip: true; spacing: 6 * window.u
                model: client.detail.files || []
                ScrollBar.vertical: ScrollBar { }
                delegate: Action {
                    required property var modelData
                    objectName: "file-" + modelData.id
                    width: ListView.view.width
                    implicitHeight: 78 * window.u
                    checked: window.pendingFileId === modelData.id
                    enabled: !client.busy
                    onClicked: window.pendingFileId = modelData.id
                    contentItem: Text {
                        text: (parent.checked ? "●  " : "○  ") + modelData.format + (modelData.sizeLabel ? " · " + modelData.sizeLabel : "") + (modelData.downloaded ? " · на устройстве" : "") + (modelData.filename ? "\n" + modelData.filename : "")
                        textFormat: Text.PlainText; font.pixelSize: 16 * window.u; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter
                        color: parent.checked ? "white" : "black"
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Action { objectName: "formatCancel"; text: "Отмена"; onClicked: formatDialog.close() }
                Action {
                    Layout.fillWidth: true; checked: true
                    objectName: "formatConfirm"
                    text: !window.formatDownload ? "Выбрать" : window.selectedFormat(window.pendingFileId).downloaded ? "Открыть карточку" : "Скачать " + (window.selectedFormat(window.pendingFileId).format || "")
                    enabled: !client.busy && window.pendingFileId > 0 && (!window.formatDownload || window.selectedFormat(window.pendingFileId).downloaded || window.selectedFormat(window.pendingFileId).supported)
                    onClicked: { client.selectFile(window.pendingFileId); formatDialog.close(); if (window.formatDownload && !client.detail.downloaded) client.downloadSelected() }
                }
            }
        }
    }
    Dialog {
        id: progressDialog
        anchors.centerIn: parent
        width: parent.width - 40 * window.u
        modal: true
        title: "Позиция чтения"
        visible: client.progressConflict
        closePolicy: Popup.NoAutoClose
        contentItem: ColumnLayout {
            spacing: 12 * window.u
            Text { text: client.conflictDescription; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 * window.u }
            Action { text: "Использовать позицию ридера"; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.resolveProgress(true) }
            Action { text: "Использовать позицию BookOrbit"; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.resolveProgress(false) }
            Action { text: "Отмена"; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.dismissConflict() }
        }
    }
    Connections {
        target: settingsScroll.contentItem
        function onMovementStarted() {
            if (window.settings && Qt.inputMethod.visible) {
                Qt.inputMethod.hide()
                settingsScroll.forceActiveFocus()
            }
        }
    }
    Connections {
        target: Qt.inputMethod
        function onVisibleChanged() { Qt.callLater(window.revealField, window.activeFocusItem) }
        function onKeyboardRectangleChanged() { Qt.callLater(window.revealField, window.activeFocusItem) }
    }
    Connections {
        target: client
        function onCompleted(operation, success) {
            if (success && operation === "catalog" && client.authenticated && window.addingConnection) {
                window.addingConnection = false
                window.settings = false
            }
        }
    }
}
