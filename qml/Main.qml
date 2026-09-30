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
    property bool coverGrid: false
    readonly property var activeCatalog: coverGrid && !client.offlineOnly ? coverCatalog : catalog
    property real savedCatalogY: 0
    property int pendingFileId: 0
    property bool formatDownload: false
    property bool settings: false
    property bool syncView: false
    property string syncFilter: "all"
    property string syncBookKey: ""
    readonly property var selectedSyncBook: client.syncBooks.find(book => book.key === syncBookKey) || ({})
    property bool addingConnection: false
    property bool browsingFolders: false
    property string folderPath: "/mnt/ext1"
    property real keyboardSpace: Qt.inputMethod.visible ? Math.max(Qt.inputMethod.keyboardRectangle.height, height * 0.4) : 0
    font.pixelSize: 18 * u

    function showBook(id) {
        savedCatalogY = activeCatalog.contentY
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
        const target = client.detailVisible ? detailScroll.contentItem : syncView ? (syncBookKey ? syncDetails.contentItem : syncList) : client.collectionsView ? collectionList : activeCatalog
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
        if (client.progressConflict) { if (!client.busy) client.dismissConflict(); return }
        if (formatDialog.visible) { formatDialog.close(); return }
        if (Qt.inputMethod.visible) { Qt.inputMethod.hide(); return }
        if (browsingFolders) { browsingFolders = false; return }
        if (addingConnection) { addingConnection = false; password.text = ""; return }
        if (syncView && !settings && !client.detailVisible) {
            if (syncBookKey) syncBookKey = ""
            else syncView = false
            return
        }
        if (settings) { settings = false; password.text = ""; Qt.inputMethod.hide() }
        else if (client.detailVisible) { client.closeDetail(); Qt.callLater(function() { activeCatalog.contentY = savedCatalogY }) }
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
                objectName: action.objectName + "Label"
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
            Text { text: window.browsingFolders ? "Папка для книг" : (window.addingConnection ? "Подключение" : (window.settings ? "Настройки" : client.detailVisible ? "О книге" : window.syncView ? (window.syncBookKey ? "Позиция чтения" : "Синхронизация") : "BookOrbit")); font.pixelSize: 29 * window.u; font.bold: true; Layout.fillWidth: true }
            Action {
                objectName: "syncButton"
                visible: !window.settings && !window.syncView && !client.detailVisible
                text: "Синхронизировать"
                icon.source: "qrc:/icons/sync.svg"
                Accessible.name: "Синхронизировать прогресс скачанных EPUB"
                enabled: !client.busy && client.authenticated
                onClicked: client.syncAll()
            }
            Action {
                objectName: "navigationButton"
                text: window.settings || client.detailVisible || window.syncView ? "Назад" : "Настройки"
                icon.source: window.settings || client.detailVisible || window.syncView ? "" : "qrc:/icons/settings.svg"
                enabled: !client.busy
                onClicked: window.settings || client.detailVisible || window.syncView ? window.back() : window.editConnection()
            }
            Action { objectName: "exitButton"; visible: !window.settings && !window.syncView && !client.detailVisible; text: "Закрыть приложение"; icon.source: "qrc:/icons/exit.svg"; enabled: !client.busy; onClicked: Qt.quit() }
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
        Text {
            Layout.fillWidth: true
            visible: client.sessionWarning.length > 0
            text: client.sessionWarning
            wrapMode: Text.Wrap
            font.pixelSize: 14 * window.u
            Accessible.name: text
        }
        ScrollView {
            id: settingsScroll
            objectName: "settingsScroll"
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
                    text: client.hasSavedSession ? "На устройстве сохранён токен для восстановления входа. Пароль не сохраняется." : "После входа клиент попробует сохранить токен. Пароль не сохраняется."
                    Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444"
                }
                Action {
                    text: "Войти"; Layout.fillWidth: true
                    enabled: !client.busy && username.text.length > 0 && (password.text.length > 0 || client.hasSavedSession && username.text === client.username && server.text === client.server)
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
                            onClicked: client.hasSavedSession ? client.restoreSession() : window.connectionForm(false)
                        }
                        Action {
                            objectName: "logoutButton"
                            text: "Выйти из аккаунта"; Layout.fillWidth: true; Layout.preferredWidth: 1
                            enabled: !client.busy && (client.authenticated || client.hasSavedSession)
                            onClicked: client.logout()
                        }
                    }
                    Item { Layout.preferredHeight: 10 * window.u }
                Text { text: "Папка для скачивания книг"; font.pixelSize: 18 * window.u; font.bold: true }
                Text { text: client.downloadDirectory; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 14 * window.u }
                Action { text: "Выбрать папку…"; Layout.fillWidth: true; enabled: !client.busy; onClicked: { Qt.inputMethod.hide(); window.browsingFolders = true } }
                Text { text: "Для новых загрузок. Уже скачанные книги останутся на своих местах."; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
                Action {
                    objectName: "verifyLibraryButton"
                    text: "Полная проверка библиотеки"
                    Layout.fillWidth: true
                    enabled: !client.busy && client.authenticated
                    onClicked: client.verifyLibrary()
                }
                Text {
                    text: "Сверяет скачанные файлы с BookOrbit. Загружает их содержимое для проверки; книги и позиции чтения не заменяет"
                    Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444"
                }
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
            visible: !window.settings && !window.syncView && !client.detailVisible
            Layout.fillWidth: true
            spacing: 8 * window.u
            Action { objectName: "downloadsTab"; text: "Скачанные"; checked: client.offlineOnly; Layout.fillWidth: true; enabled: !client.busy; onClicked: { search.text = ""; client.showDownloaded(true) } }
            Action { objectName: "catalogTab"; text: "Каталог"; checked: !client.offlineOnly && !client.collectionsView && client.collectionId === 0; Layout.fillWidth: true; enabled: !client.busy; onClicked: { search.text = ""; client.showDownloaded(false) } }
            Action { objectName: "collectionsTab"; text: "Коллекции"; checked: client.collectionsView || client.collectionId > 0; Layout.fillWidth: true; enabled: !client.busy; onClicked: { search.text = ""; client.showCollections() } }
        }
        RowLayout {
            visible: !window.settings && !window.syncView && !client.detailVisible && client.collectionId > 0
            Layout.fillWidth: true
            Action { text: "‹ Назад"; enabled: !client.busy; onClicked: window.backCollection() }
            Text { text: client.collectionName; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; font.bold: true; font.pixelSize: 20 * window.u }
        }
        RowLayout {
            visible: !window.settings && !window.syncView && !client.offlineOnly && !client.collectionsView && !client.detailVisible
            Layout.fillWidth: true
            Field { id: search; objectName: "searchField"; placeholderText: "Название, автор или серия"; onAccepted: { Qt.inputMethod.hide(); client.refresh(0, text) } }
            Action { objectName: "searchButton"; text: "Найти"; enabled: !client.busy && client.authenticated; onClicked: { Qt.inputMethod.hide(); client.refresh(0, search.text) } }
            Action {
                objectName: "coverGridToggle"
                text: "Обложки"; checkable: true; checked: window.coverGrid
                Accessible.name: "Отображать каталог сеткой обложек"
                enabled: !client.busy
                onClicked: { Qt.inputMethod.hide(); window.coverGrid = checked }
            }
        }
        RowLayout {
            visible: !window.settings && !client.authenticated
            Layout.fillWidth: true
            Text { text: "Скачанные книги доступны без входа"; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
            Action { text: "Войти"; enabled: !client.busy; onClicked: window.editConnection() }
        }
        Action {
            objectName: "syncStatusButton"
            visible: !window.settings && !window.syncView && !client.detailVisible && client.offlineOnly
            Layout.fillWidth: true
            text: "Синхронизация: " + client.syncSummary.synced + " из " + client.syncSummary.total + " · открыть"
            enabled: !client.busy
            onClicked: { window.syncView = true; window.syncBookKey = "" }
        }
        ColumnLayout {
            visible: window.syncView && !window.settings && !client.detailVisible && !window.syncBookKey
            Layout.fillWidth: true
            spacing: 10 * window.u
            Text { text: client.username + " · " + client.server; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 14 * window.u }
            Text { objectName: "syncSummaryText"; text: "Синхронизировано: " + client.syncSummary.synced + " из " + client.syncSummary.total; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 22 * window.u; font.bold: true }
            Text { text: "Позиции скачанных EPUB · по последней сверке"; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
            RowLayout {
                Layout.fillWidth: true; spacing: 6 * window.u
                Repeater {
                    model: [{key:"all",label:"Все",count:client.syncSummary.total}, {key:"attention",label:"Требуют действия",count:client.syncSummary.attention}, {key:"synced",label:"Синхронизированы",count:client.syncSummary.synced}]
                    Action {
                        required property var modelData
                        objectName: "syncFilter-" + modelData.key
                        Layout.fillWidth: true; Layout.preferredWidth: 1
                        implicitHeight: 64 * window.u
                        checked: window.syncFilter === modelData.key
                        text: modelData.label + "\n" + modelData.count
                        onClicked: { window.syncFilter = modelData.key; syncList.contentY = 0 }
                        contentItem: Text { text: parent.text; font.pixelSize: 14 * window.u; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; color: parent.checked ? "white" : "black" }
                    }
                }
            }
        }
        ListView {
            id: syncList
            objectName: "syncList"
            visible: window.syncView && !window.settings && !client.detailVisible && !window.syncBookKey
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: client.syncBooks.filter(book => window.syncFilter === "all" || (window.syncFilter === "synced" ? book.state === "synced" : book.state !== "synced"))
            ScrollBar.vertical: ScrollBar { }
            delegate: ItemDelegate {
                required property var modelData
                objectName: "syncBook-" + modelData.key
                width: syncList.width; height: syncRow.implicitHeight + 28 * window.u
                enabled: !client.busy
                onClicked: { window.syncBookKey = modelData.key; syncDetails.contentItem.contentY = 0 }
                Accessible.name: modelData.title + ", " + modelData.label
                background: Rectangle { color: parent.down ? "#dddddd" : "white"; Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#aaaaaa" } }
                contentItem: RowLayout {
                    id: syncRow
                    spacing: 14 * window.u
                    Text { text: modelData.state === "synced" ? "✓" : modelData.state === "conflict" ? "?" : "!"; font.pixelSize: 24 * window.u; Layout.preferredWidth: 25 * window.u; Accessible.ignored: true }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 4 * window.u
                        Text { text: modelData.title; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; font.pixelSize: 21 * window.u; font.bold: true }
                        Text { text: modelData.author; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 1; elide: Text.ElideRight; font.pixelSize: 14 * window.u; color: "#444444" }
                        Text { text: modelData.label + " · " + (modelData.files.length > 1 ? "файлов EPUB: " + modelData.files.length : "EPUB"); Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                    }
                    Text { text: "›"; font.pixelSize: 22 * window.u; Accessible.ignored: true }
                }
            }
            Text {
                anchors.centerIn: parent; width: parent.width
                visible: syncList.count === 0
                text: client.syncSummary.total === 0 ? "Пока нечего синхронизировать.\nСкачайте EPUB из каталога." : "В этой группе больше нет книг."
                wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 19 * window.u
            }
        }
        ScrollView {
            id: syncDetails
            objectName: "syncDetails"
            visible: window.syncView && !window.settings && !client.detailVisible && window.syncBookKey.length > 0
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: syncDetails.availableWidth; spacing: 16 * window.u
                Text { text: window.selectedSyncBook.title || "Книга недоступна"; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 24 * window.u; font.bold: true }
                Repeater {
                    model: window.selectedSyncBook.files || []
                    ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true; spacing: 12 * window.u
                        Text { text: modelData.filename || "EPUB"; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 15 * window.u; color: "#444444" }
                        Text { objectName: "syncState-" + modelData.fileId; text: modelData.label; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 21 * window.u; font.bold: true }
                        Text { text: modelData.reason; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 * window.u }
                        Text { text: modelData.checkedAt ? "Последняя попытка: " + modelData.checkedAt : ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
                        Action { objectName: "choosePosition-" + modelData.fileId; visible: modelData.state === "conflict"; text: "Выбрать позицию"; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.inspectConflict(modelData.fileId) }
                        Action { visible: modelData.state === "network"; text: "Подключить сеть"; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.connectForSync() }
                        Action { objectName: "retrySync-" + modelData.fileId; text: modelData.state === "synced" ? "Сверить ещё раз" : "Повторить сверку"; visible: modelData.state !== "file"; Layout.fillWidth: true; enabled: !client.busy && client.authenticated; onClicked: client.syncFile(modelData.fileId) }
                        Action { text: modelData.state === "file" ? "Открыть карточку файла" : "О книге"; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.showSyncFile(modelData.fileId) }
                        Action { text: "Читать локально"; visible: modelData.available; Layout.fillWidth: true; enabled: !client.busy; onClicked: { client.showSyncFile(modelData.fileId); client.openSelected(false) } }
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#aaaaaa" }
                    }
                }
            }
        }
        Text { visible: window.syncView && !window.settings && !client.detailVisible && !window.syncBookKey && client.syncSummary.unsupported > 0; text: "Ещё книг без поддержки синхронизации позиции: " + client.syncSummary.unsupported; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
        Action { objectName: "syncAllStatusButton"; visible: window.syncView && !window.settings && !client.detailVisible && !window.syncBookKey; text: client.busy ? "Сверяем книги…" : "Синхронизировать"; Layout.fillWidth: true; checked: true; enabled: !client.busy && client.authenticated && client.syncSummary.total > 0; onClicked: client.syncAll() }
        ListView {
            id: collectionList
            objectName: "collectionList"
            visible: !window.settings && !window.syncView && client.collectionsView && !client.detailVisible
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: client.collections
            ScrollBar.vertical: ScrollBar { }
            delegate: ItemDelegate {
                required property var modelData
                objectName: "collection-" + modelData.id
                width: collectionList.width
                height: (modelData.isOwner ? 66 : 84) * window.u
                onClicked: { search.text = ""; activeCatalog.contentY = 0; client.openCollection(modelData.id, modelData.name) }
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
            visible: !window.settings && !window.syncView && !client.detailVisible && !client.collectionsView && (!window.coverGrid || client.offlineOnly || count === 0)
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
        GridView {
            id: coverCatalog
            objectName: "coverCatalog"
            visible: !window.settings && !window.syncView && !client.detailVisible && !client.collectionsView && !client.offlineOnly && window.coverGrid && count > 0
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: client.books
            cellWidth: Math.floor(width / 3)
            cellHeight: Math.round(270 * window.u)
            ScrollBar.vertical: ScrollBar { }
            delegate: ItemDelegate {
                id: coverTile
                required property var modelData
                objectName: "gridBook-" + modelData.bookId
                width: coverCatalog.cellWidth; height: coverCatalog.cellHeight
                padding: 8 * window.u
                enabled: !client.busy
                onClicked: window.showBook(modelData.bookId)
                Accessible.name: "О книге: " + modelData.title + ", " + modelData.author + ", " + window.readStatusLabel(modelData) + (modelData.downloaded ? ", скачано" : "")
                background: Rectangle {
                    color: coverTile.down ? "#dddddd" : "white"
                    border.width: coverTile.visualFocus ? 2 : 0; border.color: "black"
                }
                contentItem: Column {
                    spacing: 8 * window.u
                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: Math.min(parent.width, 140 * window.u); height: 210 * window.u
                        color: "#eeeeee"; border.color: "#aaaaaa"
                        Text {
                            objectName: "gridPlaceholder-" + modelData.bookId
                            anchors.centerIn: parent; width: parent.width - 16 * window.u
                            visible: gridCover.status !== Image.Ready
                            text: "Нет обложки"; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter
                            font.pixelSize: 15 * window.u; color: "#444444"
                        }
                        Image {
                            id: gridCover
                            objectName: "gridCover-" + modelData.bookId
                            anchors.fill: parent; anchors.margins: 1
                            source: { const revision = client.coverRevision; return client.coverUrl(modelData.bookId) }
                            fillMode: Image.PreserveAspectFit; cache: false
                            sourceSize.width: 300; sourceSize.height: 450
                            Accessible.ignored: true
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom; width: parent.width; height: 24 * window.u
                            visible: modelData.downloaded
                            color: "white"; border.color: "black"
                            Text { anchors.centerIn: parent; text: "Скачано"; font.pixelSize: 12 * window.u; font.bold: true }
                        }
                    }
                    Text {
                        width: parent.width; height: 36 * window.u
                        text: modelData.title; textFormat: Text.PlainText; font.pixelSize: 15 * window.u
                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                        maximumLineCount: 2; elide: Text.ElideRight
                    }
                }
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
                    Layout.fillWidth: true; spacing: 16 * window.u
                    Rectangle {
                        objectName: "detailCoverFrame"
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 180 * window.u; Layout.preferredHeight: 280 * window.u
                        color: "white"; border.color: "#777777"
                        Text { anchors.centerIn: parent; visible: detailCover.status !== Image.Ready; text: "Нет обложки"; font.pixelSize: 15 * window.u }
                        Image { id: detailCover; anchors.fill: parent; anchors.margins: 1
                            source: { const revision = client.coverRevision; return client.coverUrl(client.detail.bookId || 0) }
                            fillMode: Image.PreserveAspectFit; cache: false
                            Accessible.name: "Обложка: " + (client.detail.title || "")
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        Layout.minimumHeight: 280 * window.u; Layout.alignment: Qt.AlignTop; spacing: 8 * window.u
                        Text { objectName: "detailTitle"; text: client.detail.title || ""; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.bold: true; font.pixelSize: 23 * window.u }
                        Text { text: client.detail.subtitle || ""; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                        Text { text: client.detail.author || ""; visible: text.length > 0; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 17 * window.u }
                        Text {
                            objectName: "detailEdition"
                            text: [client.detail.publishedYear || client.detail.publishedDate, client.detail.publisher, client.detail.pageCount ? client.detail.pageCount + " стр." : ""].filter(v => !!v).join(" · ")
                            visible: text.length > 0; textFormat: Text.PlainText; Layout.fillWidth: true
                            wrapMode: Text.Wrap; font.pixelSize: 14 * window.u
                        }
                        RowLayout {
                            objectName: "detailRating"
                            visible: typeof client.detail.rating === "number" && client.detail.rating > 0 && client.detail.rating <= 5
                            spacing: 8 * window.u
                            Accessible.role: Accessible.StaticText
                            Accessible.name: "Моя оценка: " + client.detail.rating + " из 5"
                            Text {
                                text: { const stars = Math.max(0, Math.min(5, Math.round(client.detail.rating || 0))); return "★".repeat(stars) + "☆".repeat(5 - stars) }
                                font.pixelSize: 26 * window.u; Accessible.ignored: true
                            }
                            Text { text: (client.detail.rating || 0) + "/5"; font.pixelSize: 14 * window.u; Accessible.ignored: true }
                        }
                        Flow {
                            id: detailGenres
                            objectName: "detailGenres"
                            Layout.fillWidth: true
                            visible: (client.detail.genres || []).length > 0
                            spacing: 6 * window.u
                            Repeater {
                                model: client.detail.genres || []
                                Rectangle {
                                    required property string modelData
                                    width: Math.min(genreLabel.implicitWidth + 16 * window.u, detailGenres.width)
                                    height: genreLabel.height + 10 * window.u
                                    color: "white"; border.color: "black"; radius: 6 * window.u
                                    Text {
                                        id: genreLabel
                                        x: 8 * window.u; y: 5 * window.u; width: parent.width - 16 * window.u
                                        text: modelData; textFormat: Text.PlainText; wrapMode: Text.Wrap
                                        font.pixelSize: 12 * window.u; font.bold: true
                                    }
                                }
                            }
                        }
                        Text { text: (client.detail.seriesName || "") + (client.detail.seriesIndex ? " · № " + client.detail.seriesIndex : ""); textFormat: Text.PlainText; visible: !!client.detail.seriesName; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
                        Item { Layout.fillHeight: true; Layout.minimumHeight: 8 * window.u }
                        Text { objectName: "detailReading"; text: window.readStatusLabel(client.detail); visible: text.length > 0; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; font.bold: true; font.pixelSize: 14 * window.u; wrapMode: Text.Wrap }
                        ProgressBar {
                            id: detailProgress
                            objectName: "detailProgress"
                            Layout.fillWidth: true; implicitHeight: 7 * window.u
                            from: 0; to: 100
                            visible: typeof client.detail.readingProgress === "number" && isFinite(client.detail.readingProgress) && client.detail.readingProgress >= 0 && client.detail.readingProgress <= 100
                            value: visible ? client.detail.readingProgress : 0
                            Accessible.name: "Прогресс чтения в BookOrbit"
                            background: Rectangle { color: "#cccccc"; radius: 3 * window.u }
                            contentItem: Item {
                                Rectangle { width: parent.width * detailProgress.position; height: parent.height; color: "black"; radius: 3 * window.u }
                            }
                        }
                        Action {
                            Layout.fillWidth: true
                            objectName: "formatButton"
                            font.pixelSize: 14 * window.u
                            visible: (client.detail.files || []).length > 1
                            text: "Формат: " + (window.selectedFormat().format || "нет файлов") + (window.selectedFormat().sizeLabel ? " · " + window.selectedFormat().sizeLabel : "") + "  ▾"
                            enabled: !client.busy
                            onClicked: window.chooseFormat(false)
                        }
                        Action {
                            Layout.fillWidth: true
                            objectName: "downloadButton"
                            font.bold: true; font.pixelSize: 18 * window.u
                            text: client.detail.downloaded ? (client.detail.readable ? "Читать (" + client.detail.format + ")" : "Скачано") : client.detail.needsRepair ? "Скачать заново" : "Скачать" + (client.detail.format ? " (" + client.detail.format + ")" : "")
                            enabled: !client.busy && (client.detail.downloaded ? !!client.detail.readable : !!client.detail.supported && client.authenticated)
                            onClicked: client.detail.downloaded ? client.openSelected() : client.downloadSelected()
                        }
                    }
                }
                Text {
                    id: description
                    objectName: "detailDescription"
                    text: client.detail.description || (client.detail.detailed ? "Аннотации в BookOrbit нет" : "Сведения ещё не загружены")
                    textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 * window.u
                    maximumLineCount: window.fullDescription ? 100000 : 6; elide: Text.ElideRight
                }
                Action { objectName: "descriptionToggle"; text: window.fullDescription ? "Свернуть аннотацию" : "Читать полностью"; visible: window.fullDescription || description.truncated; onClicked: window.fullDescription = !window.fullDescription }
                Text { text: client.detail.downloaded ? "Скачано на устройство" : ""; visible: text.length > 0; Layout.fillWidth: true; font.pixelSize: 14 * window.u }
                Text { text: !client.detail.supported && client.detail.format ? "Размер файла превышает лимит загрузки 100 МБ." : !client.detail.readable && client.detail.format ? "Можно скачать; открытие этого формата из клиента пока не поддерживается." : client.detail.format && client.detail.format !== "EPUB" ? "Обмен позицией для этого формата пока не поддерживается." : ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
                Text { objectName: "fileCheckResult"; text: client.detail.remoteFileChanged ? "Файл на сервере отличается. Обмен прогрессом приостановлен" : client.detail.syncResult || ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                Text { text: client.detail.hasConflict ? "На ридере и в BookOrbit разные позиции. Повторите сверку и выберите нужную." : client.detail.pendingProgress ? "Входящая позиция ещё не применена. Закройте книгу в читалке и повторите сверку." : ""; visible: !client.detail.remoteFileChanged && text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                Action { objectName: "redownloadButton"; text: "Скачать заново"; visible: !!client.detail.remoteFileChanged; Layout.fillWidth: true; enabled: !client.busy && client.authenticated; onClicked: client.downloadSelected() }
                Action { text: "Читать локально"; visible: !!client.detail.pendingProgress; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.openSelected(false) }
                Action { text: "Синхронизировать позицию"; visible: !!client.detail.canSync; Layout.fillWidth: true; enabled: !client.busy && client.authenticated; onClicked: client.syncSelected() }
                Text { text: client.detail.notice || ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
                Action { text: client.detail.detailed ? "Обновить сведения" : "Загрузить сведения"; visible: client.offlineOnly || !!client.detail.notice; enabled: !client.busy && client.authenticated; onClicked: client.refreshDetail() }
                Action { text: "Об издании  " + (window.editionExpanded ? "▾" : "›"); Layout.fillWidth: true; onClicked: window.editionExpanded = !window.editionExpanded }
                Text { text: window.editionText(); textFormat: Text.PlainText; visible: window.editionExpanded; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 17 * window.u }
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
            visible: !window.settings && !window.syncView && !client.offlineOnly && !client.collectionsView && !client.detailVisible
            Layout.fillWidth: true
            Action { objectName: "previousPage"; text: "‹"; Layout.preferredWidth: 100 * window.u; Accessible.name: "Предыдущая страница"; enabled: !client.busy && client.authenticated && client.page > 0; onClicked: { activeCatalog.contentY = 0; client.refresh(client.page-1, search.text) } }
            Text { text: client.total === 0 ? "Книг: 0" : (client.page+1) + " / " + Math.ceil(client.total/10) + " · " + (search.text ? "Найдено: " : "Книг: ") + client.total; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 14 * window.u }
            Action { objectName: "nextPage"; text: "›"; Layout.preferredWidth: 100 * window.u; Accessible.name: "Следующая страница"; enabled: !client.busy && client.authenticated && (client.page+1)*10 < client.total; onClicked: { activeCatalog.contentY = 0; client.refresh(client.page+1, search.text) } }
        }
        Action { visible: client.downloading; text: "Отменить загрузку"; Layout.fillWidth: true; onClicked: client.cancelDownload() }
        Action { objectName: "cancelVerificationButton"; visible: client.verifyingLibrary; text: "Отменить проверку"; Layout.fillWidth: true; onClicked: client.cancelLibraryVerification() }
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
        objectName: "progressDialog"
        property int choice: -1
        readonly property int revision: client.conflictRevision
        onRevisionChanged: choice = -1
        onOpened: choice = -1
        anchors.centerIn: parent
        width: parent.width - 40 * window.u
        height: parent.height - 40 * window.u
        modal: true
        title: "Выбор позиции"
        visible: client.progressConflict
        closePolicy: Popup.NoAutoClose
        background: Rectangle { color: "white"; border.color: "black"; radius: 3 * window.u }
        contentItem: ScrollView {
            id: conflictScroll
            clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: conflictScroll.availableWidth
                spacing: 14 * window.u
                Text { text: client.conflictDescription; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 22 * window.u; font.bold: true }
                Text { text: "На ридере и в BookOrbit сохранены разные позиции. Где продолжить чтение?"; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 * window.u }
                Text { text: "Глава и фрагмент — из EPUB на устройстве."; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
                Repeater {
                    model: client.conflictPositions
                    RadioButton {
                        required property var modelData
                        required property int index
                        objectName: modelData.local ? "conflictLocal" : "conflictRemote"
                        Layout.fillWidth: true
                        implicitHeight: positionContent.implicitHeight + 24 * window.u
                        checked: progressDialog.choice === index
                        enabled: !client.busy && modelData.canUse
                        onClicked: progressDialog.choice = index
                        Accessible.name: modelData.source + ", " + (modelData.chapter || "") + ", " + modelData.percentage
                        indicator: Rectangle {
                            x: 10 * window.u; y: 16 * window.u
                            width: 22 * window.u; height: width; radius: width / 2
                            color: "white"; border.color: "black"
                            Rectangle { anchors.centerIn: parent; width: 12 * window.u; height: width; radius: width / 2; color: "black"; visible: progressDialog.choice === index }
                        }
                        background: Rectangle { color: parent.checked ? "#eeeeee" : "white"; border.color: "black"; border.width: parent.checked ? 3 : 1; radius: 3 * window.u }
                        contentItem: ColumnLayout {
                            id: positionContent
                            spacing: 6 * window.u
                            Text { text: modelData.source; Layout.fillWidth: true; Layout.leftMargin: 40 * window.u; Layout.rightMargin: 10 * window.u; font.pixelSize: 20 * window.u; font.bold: true }
                            Text { text: modelData.chapter || ""; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; Layout.leftMargin: 40 * window.u; Layout.rightMargin: 10 * window.u; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; font.pixelSize: 18 * window.u }
                            Text { text: modelData.excerpt ? "«" + modelData.excerpt + "»" : modelData.chapter ? "" : "Глава и фрагмент недоступны"; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; Layout.leftMargin: 40 * window.u; Layout.rightMargin: 10 * window.u; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight; font.pixelSize: 17 * window.u }
                            Text { text: modelData.percentage; Layout.fillWidth: true; Layout.leftMargin: 40 * window.u; Layout.rightMargin: 10 * window.u; wrapMode: Text.Wrap; font.pixelSize: 15 * window.u; color: "#444444" }
                            Text { text: modelData.notice || ""; visible: text.length > 0; Layout.fillWidth: true; Layout.leftMargin: 40 * window.u; Layout.rightMargin: 10 * window.u; wrapMode: Text.Wrap; font.pixelSize: 15 * window.u }
                        }
                    }
                }
                Text { text: progressDialog.choice === 0 ? "Позиция ридера заменит позицию в BookOrbit." : progressDialog.choice === 1 ? "Позиция BookOrbit заменит позицию на ридере. Все книги штатной читалки должны быть закрыты." : "Обе позиции сохранены. Выберите одну или решите позже."; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                Text { text: client.status; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
            }
        }
        footer: ColumnLayout {
            spacing: 10 * window.u
            Action { objectName: "applyConflict"; text: client.busy ? "Проверяем позиции…" : progressDialog.choice === 0 ? "Использовать позицию ридера" : progressDialog.choice === 1 ? "Использовать позицию BookOrbit" : "Выберите позицию"; Layout.fillWidth: true; checked: true; enabled: !client.busy && client.authenticated && progressDialog.choice >= 0; onClicked: client.resolveProgress(progressDialog.choice === 0) }
            Action { text: "Войти для применения позиции"; visible: !client.authenticated; Layout.fillWidth: true; enabled: !client.busy; onClicked: { client.dismissConflict(); window.editConnection() } }
            Action { objectName: "deferConflict"; text: "Решить позже"; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.dismissConflict() }
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
            if (operation === "sync" && client.syncSummary.total > 0 && !window.settings) {
                window.syncView = true
                window.syncBookKey = ""
            }
            if (success && operation === "catalog" && client.authenticated && window.addingConnection) {
                window.addingConnection = false
                window.settings = false
            }
        }
    }
}
