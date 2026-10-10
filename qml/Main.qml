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
    readonly property var updater: typeof updateManager === "undefined" ? null : updateManager
    Connections {
        target: window.updater
        function onChanged() {
            if (window.settings && window.updater.state === "ready")
                Qt.callLater(function() {
                    settingsScroll.contentItem.contentY = Math.max(0, settingsScroll.contentHeight - settingsScroll.height)
                })
        }
    }
    property bool fullDescription: false
    property bool editionExpanded: false
    property bool coverGrid: false
    readonly property var activeCatalog: coverGrid && !client.offlineOnly ? coverCatalog : catalog
    property real savedCatalogY: 0
    property real savedLibraryY: 0
    property bool libraryAtBeginning: true
    property real pageOffset: 0
    property int pageDirection: 0
    property bool catalogTransition: false
    property int pendingFileId: 0
    property bool formatDownload: false
    readonly property var languageCodes: ["system"].concat(client.languageCodes)
    property bool settings: false
    property bool syncView: false
    property string syncFilter: "all"
    property string syncBookKey: ""
    readonly property var catalogBooks: client.books
    readonly property var detailData: client.detail
    readonly property var syncData: client.syncSummary
    readonly property var detailFormat: (detailData.files || []).find(file => file.id === detailData.fileId) || ({})
    readonly property var pendingFormat: (detailData.files || []).find(file => file.id === pendingFileId) || ({})
    readonly property var selectedSyncBook: window.syncData.books.find(book => book.key === syncBookKey) || ({})
    readonly property bool loginRequired: !client.authenticated && !client.hasSavedSession
    property bool addingConnection: false
    property bool browsingFolders: false
    property string folderPath: "/mnt/ext1"
    property real keyboardSpace: Qt.inputMethod.visible ? Math.max(Qt.inputMethod.keyboardRectangle.height, height * 0.4) : 0
    font.pixelSize: 18 * u

    readonly property string uiContext: settings ? (addingConnection ? "connection" : "settings") : client.detailVisible ? "book" : syncView ? "sync" : "catalog"
    readonly property var uiMessage: client.feedback
    readonly property bool messageVisible: (uiMessage.context === uiContext || (settings && !addingConnection && uiMessage.context === "connection")) && (!uiMessage.bookId || !client.detailVisible || uiMessage.bookId === detailData.bookId) && (!uiMessage.fileId || !client.detailVisible || uiMessage.fileId === detailData.fileId)
    onUiContextChanged: Qt.callLater(function() { client.setUiContext(window.uiContext) })
    readonly property string activeAccount: client.server + "\n" + client.username
    onActiveAccountChanged: { savedLibraryY = 0; libraryAtBeginning = true; if (client.offlineOnly) search.text = client.localQuery }
    onSyncViewChanged: if (syncView) syncFilter = "all"
    Connections { target: Qt.application; function onStateChanged() { if (Qt.application.state === Qt.ApplicationActive) client.refreshRecents() } }
    function showFile(fileId) {
        savedCatalogY = activeCatalog.contentY
        fullDescription = false; editionExpanded = false
        client.showSyncFile(fileId)
        detailScroll.contentItem.contentY = 0
    }
    function primaryLabel(book) {
        if (!book.downloaded) return book.needsRepair ? qsTranslate("BookOrbit", "Download again") : qsTranslate("BookOrbit", "Download ") + (book.format || "")
        if (!book.readable) return qsTranslate("BookOrbit", "Downloaded")
        if (book.remoteFileChanged) return qsTranslate("BookOrbit", "Read locally")
        if (book.hasConflict) return qsTranslate("BookOrbit", "Choose position")
        if (book.pendingProgress) return qsTranslate("BookOrbit", "Read from reader position")
        return qsTranslate("BookOrbit", "Read ") + book.format
    }
    function quickAction(book, index) {
        if (book.downloaded && book.readable && (book.remoteFileChanged || (!book.pendingProgress && !book.hasConflict))) client.openFile(book.fileId, false)
        else if (book.downloaded || book.needsRepair) showFile(book.fileId)
        else if (!client.authenticated) editConnection()
        else if (book.fileCount > 1) { showBook(book.bookId); chooseFormat(true) }
        else client.download(index)
    }
    function submitSearch() {
        Qt.inputMethod.hide()
        if (client.offlineOnly) { libraryAtBeginning = true; client.searchDownloaded(search.text); catalog.positionViewAtBeginning(); savedLibraryY = 0 }
        else { pageOffset = activeCatalog.contentY; pageDirection = 0; catalogTransition = true; client.refresh(0, search.text) }
    }
    function clearSearch() { search.text = ""; submitSearch() }
    function showBook(id) {
        savedCatalogY = activeCatalog.contentY
        fullDescription = false; editionExpanded = false
        client.showDetail(id)
        detailScroll.contentItem.contentY = 0
    }
    function chooseFormat(download) {
        pendingFileId = window.detailData.fileId || 0
        formatDownload = download
        formatDialog.open()
    }
    function readStatusLabel(book) {
        const status = book.readStatus ? book.readStatus.status : "unread"
        const labels = { unread: qsTranslate("BookOrbit", "Unread"), want_to_read: qsTranslate("BookOrbit", "Want to read"), reading: qsTranslate("BookOrbit", "Reading"), on_hold: qsTranslate("BookOrbit", "On hold"), rereading: qsTranslate("BookOrbit", "Rereading"), read: qsTranslate("BookOrbit", "Read"), skimmed: qsTranslate("BookOrbit", "Skimmed"), abandoned: qsTranslate("BookOrbit", "Abandoned") }
        let label = labels[status] || ""
        if (label) label = qsTranslate("BookOrbit", "In BookOrbit: ") + label.charAt(0).toLowerCase() + label.slice(1)
        const progress = book.readingProgress
        if ((status === "reading" || status === "rereading") && typeof progress === "number" && isFinite(progress) && progress >= 0 && progress <= 100)
            label += " · " + progress.toLocaleString(Qt.locale(), "f", progress % 1 === 0 ? 0 : 1) + " %"
        return label
    }
    function editionText() {
        const d = window.detailData, lines = []
        function field(name, value) { if (value) lines.push(name + ": " + value) }
        field(qsTranslate("BookOrbit", "Title"), d.title)
        field(qsTranslate("BookOrbit", "Subtitle"), d.subtitle)
        field(qsTranslate("BookOrbit", "Authors"), d.author)
        field(qsTranslate("BookOrbit", "Series"), d.seriesName)
        field(qsTranslate("BookOrbit", "My rating"), d.rating ? d.rating + " / 5" : "")
        field(qsTranslate("BookOrbit", "Publisher"), d.publisher)
        field(qsTranslate("BookOrbit", "Published"), d.publishedDate || d.publishedYear)
        field(qsTranslate("BookOrbit", "Language"), ({ru:qsTranslate("BookOrbit", "Russian"), en:qsTranslate("BookOrbit", "English"), de:qsTranslate("BookOrbit", "German"), fr:qsTranslate("BookOrbit", "French")})[d.language] || d.language)
        field(qsTranslate("BookOrbit", "Page count"), d.pageCount ? d.pageCount + qsTranslate("BookOrbit", " pages") : "")
        field("ISBN", d.isbn13 || d.isbn10)
        field(qsTranslate("BookOrbit", "Library"), d.libraryName)
        field(qsTranslate("BookOrbit", "Genres"), (d.genres || []).join(", "))
        field(qsTranslate("BookOrbit", "Tags"), (d.tags || []).join(", "))
        const series = d.seriesMemberships || []
        if (series.length > 0) field(qsTranslate("BookOrbit", "Series"), series.map(s => s.seriesName + (s.seriesIndex ? " · № " + s.seriesIndex : "")).join("; "))
        for (const rating of d.communityRatings || []) field(qsTranslate("BookOrbit", "Rating: ") + rating.provider, rating.rating)
        return lines.length ? lines.join("\n") : qsTranslate("BookOrbit", "No additional details")
    }
    function pageContent(direction) {
        if (formatDialog.visible || progressDialog.visible || Qt.inputMethod.visible || settings || client.busy) return
        if (client.offlineOnly && !client.detailVisible && !syncView) libraryAtBeginning = false
        const target = client.detailVisible ? detailScroll.contentItem : syncView ? (syncBookKey ? syncDetails.contentItem : syncList) : client.collectionsView ? collectionList : activeCatalog
        const top = target.originY === undefined ? 0 : target.originY
        const bottom = top + Math.max(0, target.contentHeight-target.height)
        const edge = direction > 0 ? target.contentY >= bottom-1 : target.contentY <= top+1
        if (edge && !client.detailVisible && !syncView && !client.collectionsView && !client.offlineOnly &&
                (direction > 0 ? (client.page+1)*10 < client.total : client.page > 0)) {
            pageOffset = target.contentY; pageDirection = direction; catalogTransition = true
            client.refresh(client.page + direction, client.catalogQuery)
        } else target.contentY = Math.max(top, Math.min(bottom, target.contentY + direction*target.height*0.85))
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
        implicitHeight: action.icon.source.toString().length ? 48 * window.u : Math.max(48 * window.u, contentItem.implicitHeight + padding * 2)
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
            implicitHeight: action.icon.source.toString().length ? 28 * window.u : Math.max(buttonText.implicitHeight, 28 * window.u)
            Text {
                id: buttonText
                objectName: action.objectName + "Label"
                anchors.fill: parent
                visible: !action.icon.source.toString().length
                text: action.text
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
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
    component Choice: ComboBox {
        id: choice
        implicitHeight: 48 * window.u
        font.pixelSize: 17 * window.u
        padding: 10 * window.u
        background: Rectangle { color: "white"; border.color: "black"; border.width: choice.activeFocus ? 3 : 1; radius: 3 * window.u }
        delegate: ItemDelegate {
            required property var modelData
            required property int index
            width: choice.width; implicitHeight: 48 * window.u
            text: modelData; font.pixelSize: 16 * window.u
            highlighted: choice.highlightedIndex === index
            Accessible.name: text
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
            objectName: "header"
            Layout.fillWidth: true
            Text { text: window.browsingFolders ? qsTranslate("BookOrbit", "Book folder") : (window.addingConnection ? qsTranslate("BookOrbit", "Connection") : (window.settings ? qsTranslate("BookOrbit", "Settings") : client.detailVisible ? qsTranslate("BookOrbit", "About this book") : window.syncView ? (window.syncBookKey ? qsTranslate("BookOrbit", "Reading position") : qsTranslate("BookOrbit", "Sync")) : "BookOrbit")); font.pixelSize: 29 * window.u; font.bold: true; Layout.fillWidth: true }
            Action {
                objectName: "syncButton"
                visible: !window.settings && !window.syncView && !client.detailVisible
                text: qsTranslate("BookOrbit", "Sync")
                icon.source: "qrc:/icons/sync.svg"
                Accessible.name: qsTranslate("BookOrbit", "Open sync for downloaded books")
                enabled: !client.busy && client.authenticated
                onClicked: { window.syncView = true; window.syncBookKey = "" }
            }
            Action {
                objectName: "loginButton"
                text: qsTranslate("BookOrbit", "Sign in")
                visible: !window.browsingFolders && (window.loginRequired || window.addingConnection && (server.text.trim().replace(/\/+$/, "") !== client.server || username.text.trim() !== client.username))
                enabled: !client.busy && (!window.addingConnection || username.text.length > 0 && (password.text.length > 0 || client.hasSavedSession && username.text === client.username && server.text === client.server))
                onClicked: {
                    if (window.addingConnection) {
                        client.login(server.text, username.text, password.text)
                        password.text = ""
                        Qt.inputMethod.hide()
                    } else if (window.settings) window.connectionForm(false)
                    else window.editConnection()
                }
            }
            Action {
                objectName: "navigationButton"
                text: window.settings || client.detailVisible || window.syncView ? qsTranslate("BookOrbit", "Back") : qsTranslate("BookOrbit", "Settings")
                icon.source: window.settings || client.detailVisible || window.syncView ? "" : "qrc:/icons/settings.svg"
                enabled: !client.busy
                onClicked: window.settings || client.detailVisible || window.syncView ? window.back() : window.editConnection()
            }
            Action { objectName: "exitButton"; visible: !window.settings && !window.syncView && !client.detailVisible; text: qsTranslate("BookOrbit", "Close application"); icon.source: "qrc:/icons/exit.svg"; enabled: !client.busy; onClicked: Qt.quit() }
        }
        Action {
            objectName: "updateNotification"
            visible: !window.settings && !!window.updater && window.updater.state === "available"
            text: qsTranslate("BookOrbit", "Available version: %1").arg(window.updater ? window.updater.availableVersion : "")
            Layout.fillWidth: true; enabled: !client.busy
            onClicked: { window.settings = true; Qt.callLater(function() { settingsScroll.contentItem.contentY = Math.max(0, settingsScroll.contentHeight - settingsScroll.height) }) }
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
            maximumLineCount: client.busy ? 2 : 3
            elide: Text.ElideRight
            objectName: "operationMessage"
            text: window.messageVisible ? (window.uiMessage.text || "") : ""
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
                Text { text: qsTranslate("BookOrbit", "Server address"); font.pixelSize: 16 * window.u }
                Field { id: server; inputMethodHints: Qt.ImhUrlCharactersOnly; Accessible.name: qsTranslate("BookOrbit", "Server address") }
                Text { text: qsTranslate("BookOrbit", "Username"); font.pixelSize: 16 * window.u }
                Field { id: username; inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText; Accessible.name: qsTranslate("BookOrbit", "Username") }
                Text { text: qsTranslate("BookOrbit", "Sign-in password"); font.pixelSize: 16 * window.u }
                Field { id: password; objectName: "passwordField"; echoMode: showPassword.checked ? TextInput.Normal : TextInput.Password; inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText; Accessible.name: qsTranslate("BookOrbit", "Password") }
                CheckBox {
                    id: showPassword
                    objectName: "showPassword"
                    text: qsTranslate("BookOrbit", "Show password")
                    font.pixelSize: 16 * window.u
                    implicitHeight: 44 * window.u
                    enabled: !client.busy
                    onVisibleChanged: if (!visible) checked = false
                }
                Text {
                    text: client.hasSavedSession ? qsTranslate("BookOrbit", "A token is saved on this device to restore your session. Your password is not saved.") : qsTranslate("BookOrbit", "After sign-in, the app will try to save a session token. Your password is not saved.")
                    Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444"
                }
                }
                ColumnLayout {
                    visible: !window.addingConnection
                    Layout.fillWidth: true
                    spacing: 8 * window.u
                    Text { text: qsTranslate("BookOrbit", "Interface language"); font.pixelSize: 18 * window.u; font.bold: true }
                    Choice {
                        objectName: "languageSelector"
                        Layout.fillWidth: true
                        model: [qsTranslate("BookOrbit", "Follow system")].concat(client.languageNames)
                        currentIndex: window.languageCodes.indexOf(client.language)
                        Accessible.name: qsTranslate("BookOrbit", "Interface language")
                        enabled: !client.busy
                        onActivated: index => { client.setLanguage(window.languageCodes[index]); currentIndex = Qt.binding(function() { return window.languageCodes.indexOf(client.language) }) }
                    }
                    Item { Layout.preferredHeight: 10 * window.u }
                    Text { text: qsTranslate("BookOrbit", "Saved connections"); font.pixelSize: 18 * window.u; font.bold: true }
                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            Layout.fillWidth: true
                            implicitHeight: 48 * window.u
                            font.pixelSize: 15 * window.u
                            model: client.accounts
                            currentIndex: client.accounts.indexOf(client.username + " · " + client.server)
                            enabled: !client.busy && count > 0
                            Accessible.name: qsTranslate("BookOrbit", "Saved connections")
                            displayText: count > 0 ? currentText : qsTranslate("BookOrbit", "No connections yet")
                            onActivated: client.selectAccount(currentIndex)
                        }
                        Action { objectName: "addConnection"; text: qsTranslate("BookOrbit", "Add"); enabled: !client.busy; onClicked: window.connectionForm(true) }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8 * window.u
                        Action {
                            objectName: "connectButton"
                            text: qsTranslate("BookOrbit", "Connect"); Layout.fillWidth: true; Layout.preferredWidth: 1
                            enabled: !client.busy && client.accounts.length > 0 && !client.authenticated
                            onClicked: client.hasSavedSession ? client.restoreSession() : window.connectionForm(false)
                        }
                        Action {
                            objectName: "logoutButton"
                            text: qsTranslate("BookOrbit", "Sign out"); Layout.fillWidth: true; Layout.preferredWidth: 1
                            enabled: !client.busy && (client.authenticated || client.hasSavedSession)
                            onClicked: client.logout()
                        }
                    }
                    Item { Layout.preferredHeight: 10 * window.u }
                Text { text: qsTranslate("BookOrbit", "Book download folder"); font.pixelSize: 18 * window.u; font.bold: true }
                Text { text: client.downloadDirectory; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 14 * window.u }
                Action { text: qsTranslate("BookOrbit", "Choose folder…"); Layout.fillWidth: true; enabled: !client.busy; onClicked: { Qt.inputMethod.hide(); window.browsingFolders = true } }
                Text { text: qsTranslate("BookOrbit", "For new downloads. Books already downloaded will stay where they are."); Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
                Action {
                    objectName: "verifyLibraryButton"
                    text: qsTranslate("BookOrbit", "Verify library files")
                    Layout.fillWidth: true
                    enabled: !client.busy && client.authenticated
                    onClicked: client.verifyLibrary()
                }
                Text {
                    text: qsTranslate("BookOrbit", "Compares downloaded files with BookOrbit by downloading their contents. Books and reading positions are kept unchanged")
                    Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444"
                }
                CheckBox {
                    text: qsTranslate("BookOrbit", "Write diagnostic log")
                    checked: client.diagnosticLogging
                    enabled: !client.busy
                    onClicked: client.setDiagnosticLogging(checked)
                }
                Text { objectName: "diagnosticPath"; text: client.diagnosticLogPath; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 14 * window.u; color: "#444444" }
                Text {
                    text: client.diagnosticError
                    visible: text.length > 0; textFormat: Text.PlainText
                    Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u
                }
                ColumnLayout {
                    visible: window.updater !== null
                    Layout.fillWidth: true
                    spacing: 8 * window.u
                    Item { Layout.preferredHeight: 12 * window.u }
                    Text { text: qsTranslate("BookOrbit", "Application updates"); font.pixelSize: 18 * window.u; font.bold: true }
                    Text {
                        text: qsTranslate("BookOrbit", "Installed version: %1").arg(window.updater ? window.updater.version : "")
                        Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u
                    }
                    Text {
                        visible: !!window.updater && window.updater.lastChecked.length > 0
                        text: qsTranslate("BookOrbit", "Last checked: %1").arg(window.updater ? window.updater.lastChecked : "")
                        Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u
                    }
                    CheckBox {
                        text: qsTranslate("BookOrbit", "Check at startup when connected")
                        checked: !!window.updater && window.updater.automatic
                        enabled: !client.busy
                        font.pixelSize: 16 * window.u; implicitHeight: 48 * window.u
                        onClicked: window.updater.automatic = checked
                    }
                    Text {
                        text: window.updater ? window.updater.message : ""
                        visible: text.length > 0; textFormat: Text.PlainText
                        Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u
                        Accessible.role: Accessible.StaticText; Accessible.name: text
                    }
                    Action {
                        objectName: "checkUpdateButton"
                        text: window.updater && window.updater.state === "checking" ? qsTranslate("BookOrbit", "Checking updates…") : qsTranslate("BookOrbit", "Check for updates")
                        Layout.fillWidth: true
                        enabled: !client.busy && !!window.updater && ["idle", "error", "available"].indexOf(window.updater.state) >= 0
                        onClicked: window.updater.check()
                    }
                    Text {
                        text: qsTranslate("BookOrbit", "Available version: %1").arg(window.updater ? window.updater.availableVersion : "")
                        visible: !!window.updater && window.updater.availableVersion.length > 0
                        Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u; font.bold: true
                    }
                    Text {
                        text: window.updater ? window.updater.notes : ""
                        visible: text.length > 0; textFormat: Text.PlainText
                        Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 15 * window.u
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        visible: !!window.updater && window.updater.state === "downloading"
                        value: window.updater ? window.updater.progress : 0
                        Accessible.name: qsTranslate("BookOrbit", "Update download progress")
                    }
                    Text {
                        visible: !!window.updater && window.updater.availableVersion.length > 0
                        text: window.updater && window.updater.state === "downloading"
                            ? qsTranslate("BookOrbit", "%1% · %2 / %3 KB").arg(Math.round(window.updater.progress * 100)).arg(Math.round(window.updater.progress * window.updater.archiveBytes / 1024)).arg(Math.ceil(window.updater.archiveBytes / 1024))
                            : qsTranslate("BookOrbit", "Download size: %1 KB").arg(window.updater ? Math.ceil(window.updater.archiveBytes / 1024) : 0)
                        Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u
                    }
                    Action {
                        objectName: "downloadUpdateButton"
                        text: qsTranslate("BookOrbit", "Download update"); Layout.fillWidth: true
                        visible: !!window.updater && window.updater.state === "available"
                        enabled: !client.busy
                        onClicked: window.updater.download()
                    }
                    Action {
                        text: qsTranslate("BookOrbit", "Cancel update download"); Layout.fillWidth: true
                        visible: !!window.updater && ["checking", "downloading", "verifying"].indexOf(window.updater.state) >= 0
                        onClicked: window.updater.cancel()
                    }
                    Action {
                        objectName: "installUpdateButton"
                        text: qsTranslate("BookOrbit", "Install and close"); Layout.fillWidth: true
                        visible: !!window.updater && window.updater.canInstall
                        enabled: !client.busy
                        onClicked: window.updater.install()
                    }
                }
                }
            }
        }
        ColumnLayout {
            visible: window.browsingFolders
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 12 * window.u
            Text { text: qsTranslate("BookOrbit", "Internal storage"); font.pixelSize: 18 * window.u; font.bold: true }
            Text { text: window.folderPath; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 15 * window.u; Accessible.name: qsTranslate("BookOrbit", "Current folder: ") + text }
            Action {
                text: qsTranslate("BookOrbit", "↑ Parent folder"); Layout.fillWidth: true
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
                    Accessible.name: qsTranslate("BookOrbit", "Open folder ") + modelData.name
                    onClicked: window.folderPath = modelData.path
                    contentItem: Text {
                        text: parent.text; font: parent.font; color: parent.down ? "white" : "black"
                        elide: Text.ElideMiddle; verticalAlignment: Text.AlignVCenter
                    }
                }
                Text { anchors.centerIn: parent; width: parent.width; visible: folders.count === 0; text: qsTranslate("BookOrbit", "No accessible subfolders"); wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 17 * window.u }
            }
            Action {
                text: qsTranslate("BookOrbit", "Download to this folder"); Layout.fillWidth: true
                onClicked: { if (client.setDownloadDirectory(window.folderPath)) window.browsingFolders = false }
            }
            Action { text: qsTranslate("BookOrbit", "Cancel"); Layout.fillWidth: true; onClicked: window.browsingFolders = false }
        }
        RowLayout {
            visible: !window.settings && !window.syncView && !client.detailVisible
            Layout.fillWidth: true
            spacing: 8 * window.u
            Action { objectName: "downloadsTab"; text: qsTranslate("BookOrbit", "Downloads"); checked: client.offlineOnly; Layout.fillWidth: true; enabled: !client.busy; onClicked: { window.libraryAtBeginning = window.savedLibraryY === 0; client.showDownloaded(true); search.text = client.localQuery; Qt.callLater(function() { if (window.libraryAtBeginning) catalog.positionViewAtBeginning(); else catalog.contentY = window.savedLibraryY }) } }
            Action { objectName: "catalogTab"; text: qsTranslate("BookOrbit", "Catalog"); checked: !client.offlineOnly && !client.collectionsView && client.collectionId === 0; Layout.fillWidth: true; enabled: !client.busy; onClicked: { if (client.offlineOnly) window.savedLibraryY = window.libraryAtBeginning ? 0 : catalog.contentY; search.text = ""; window.pageDirection = 0; window.pageOffset = 0; window.catalogTransition = true; client.showDownloaded(false) } }
            Action { objectName: "collectionsTab"; text: qsTranslate("BookOrbit", "Collections"); checked: client.collectionsView || client.collectionId > 0; Layout.fillWidth: true; enabled: !client.busy; onClicked: { if (client.offlineOnly) window.savedLibraryY = window.libraryAtBeginning ? 0 : catalog.contentY; search.text = ""; client.showCollections() } }
        }
        RowLayout {
            visible: !window.settings && !window.syncView && !client.detailVisible && client.collectionId > 0
            Layout.fillWidth: true
            Action { text: qsTranslate("BookOrbit", "‹ Back"); enabled: !client.busy; onClicked: window.backCollection() }
            Text { text: client.collectionName; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; font.bold: true; font.pixelSize: 20 * window.u }
        }
        RowLayout {
            visible: !window.settings && !window.syncView && !client.collectionsView && !client.detailVisible
            Layout.fillWidth: true
            Field { id: search; objectName: "searchField"; placeholderText: qsTranslate("BookOrbit", "Title, author or series"); onAccepted: window.submitSearch() }
            Action { objectName: "searchButton"; text: qsTranslate("BookOrbit", "Search"); enabled: !client.busy && (client.offlineOnly || client.authenticated); onClicked: window.submitSearch() }
            Action { objectName: "clearSearch"; text: qsTranslate("BookOrbit", "Clear"); visible: search.text.length > 0; enabled: !client.busy; onClicked: window.clearSearch() }
            RowLayout {
                visible: !client.offlineOnly
                spacing: 0
                Action {
                    objectName: "listViewButton"
                    text: qsTranslate("BookOrbit", "List"); icon.source: "qrc:/icons/list.svg"
                    checkable: true; autoExclusive: true; checked: !window.coverGrid
                    Accessible.role: Accessible.RadioButton
                    Accessible.checked: checked
                    enabled: !client.busy
                    onClicked: { Qt.inputMethod.hide(); window.coverGrid = false }
                }
                Action {
                    objectName: "coverViewButton"
                    text: qsTranslate("BookOrbit", "Covers"); icon.source: "qrc:/icons/grid.svg"
                    checkable: true; autoExclusive: true; checked: window.coverGrid
                    Accessible.role: Accessible.RadioButton
                    Accessible.checked: checked
                    enabled: !client.busy
                    onClicked: { Qt.inputMethod.hide(); window.coverGrid = true }
                }
            }
        }
        Text {
            objectName: "localCount"
            visible: client.offlineOnly && !window.settings && !window.syncView && !client.detailVisible && client.localQuery.length > 0
            text: qsTranslate("BookOrbit", "Found: ") + window.catalogBooks.length
            font.pixelSize: 14 * window.u
        }
        ColumnLayout {
            visible: window.syncView && !window.settings && !client.detailVisible && !window.syncBookKey
            Layout.fillWidth: true
            spacing: 10 * window.u
            Text { text: client.username + " · " + client.server; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 14 * window.u }
            Text { objectName: "syncSummaryText"; text: qsTranslate("BookOrbit", "Synced: ") + window.syncData.synced + qsTranslate("BookOrbit", " of ") + window.syncData.total; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 22 * window.u; font.bold: true }
            Text { text: qsTranslate("BookOrbit", "Downloaded book positions · last sync results"); Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
            Text { text: qsTranslate("BookOrbit", "Waiting: ") + window.syncData.waiting + qsTranslate("BookOrbit", " · needs attention: ") + window.syncData.attention; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 15 * window.u }
            RowLayout {
                Layout.fillWidth: true
                Text { text: qsTranslate("BookOrbit", "Show:"); font.pixelSize: 16 * window.u }
                Choice {
                    objectName: "syncFilter"; Layout.fillWidth: true
                    model: [qsTranslate("BookOrbit", "All (") + window.syncData.total + ")", qsTranslate("BookOrbit", "Waiting (") + window.syncData.waiting + ")", qsTranslate("BookOrbit", "Needs attention (") + window.syncData.attention + ")", qsTranslate("BookOrbit", "Synced (") + window.syncData.synced + ")"]
                    currentIndex: ["all", "waiting", "attention", "synced"].indexOf(window.syncFilter)
                    Accessible.name: qsTranslate("BookOrbit", "Filter sync status")
                    onActivated: index => { window.syncFilter = ["all", "waiting", "attention", "synced"][index]; syncList.contentY = 0 }
                }
            }
        }
        ListView {
            id: syncList
            objectName: "syncList"
            visible: window.syncView && !window.settings && !client.detailVisible && !window.syncBookKey
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: window.syncData.books.filter(book => window.syncFilter === "all" || book.group === window.syncFilter)
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
                        Text { text: modelData.label + " · " + (modelData.files.length > 1 ? qsTranslate("BookOrbit", "Files: ") + modelData.files.length : modelData.files[0].format); Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                    }
                    Text { text: "›"; font.pixelSize: 22 * window.u; Accessible.ignored: true }
                }
            }
            Text {
                anchors.centerIn: parent; width: parent.width
                visible: syncList.count === 0
                text: window.syncData.total === 0 ? qsTranslate("BookOrbit", "Nothing to sync yet.\nDownload an EPUB or FB2 from the catalog.") : qsTranslate("BookOrbit", "No more books in this group.")
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
                Text { text: window.selectedSyncBook.title || qsTranslate("BookOrbit", "Book unavailable"); textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 24 * window.u; font.bold: true }
                Repeater {
                    model: window.selectedSyncBook.files || []
                    ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true; spacing: 12 * window.u
                        Text { text: modelData.filename || modelData.format; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; font.pixelSize: 15 * window.u; color: "#444444" }
                        Text { objectName: "syncState-" + modelData.fileId; text: modelData.label; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 21 * window.u; font.bold: true }
                        Text { text: modelData.reason; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 * window.u }
                        Text { text: modelData.checkedAt ? qsTranslate("BookOrbit", "Last attempt: ") + modelData.checkedAt : ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
                        Action { objectName: "choosePosition-" + modelData.fileId; visible: modelData.state === "conflict"; text: qsTranslate("BookOrbit", "Choose position"); Layout.fillWidth: true; enabled: !client.busy; onClicked: client.inspectConflict(modelData.fileId) }
                        Action { visible: modelData.state === "network"; text: qsTranslate("BookOrbit", "Connect to network"); Layout.fillWidth: true; enabled: !client.busy; onClicked: client.connectForSync() }
                        Action { objectName: "retrySync-" + modelData.fileId; text: modelData.state === "synced" ? qsTranslate("BookOrbit", "Check again") : qsTranslate("BookOrbit", "Retry sync"); visible: modelData.state !== "file"; Layout.fillWidth: true; enabled: !client.busy && client.authenticated; onClicked: client.syncFile(modelData.fileId) }
                        Action { text: modelData.state === "file" ? qsTranslate("BookOrbit", "Open file details") : qsTranslate("BookOrbit", "About this book"); Layout.fillWidth: true; enabled: !client.busy; onClicked: client.showSyncFile(modelData.fileId) }
                        Action { text: qsTranslate("BookOrbit", "Read locally"); visible: modelData.available; Layout.fillWidth: true; enabled: !client.busy; onClicked: { client.showSyncFile(modelData.fileId); client.openSelected(false) } }
                        Rectangle { Layout.fillWidth: true; height: 1; color: "#aaaaaa" }
                    }
                }
            }
        }
        Text { visible: window.syncView && !window.settings && !client.detailVisible && !window.syncBookKey && window.syncData.unsupported > 0; text: qsTranslate("BookOrbit", "Other books without position sync support: ") + window.syncData.unsupported; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
        Action { objectName: "syncAllStatusButton"; visible: window.syncView && !window.settings && !client.detailVisible && !window.syncBookKey; text: client.busy ? qsTranslate("BookOrbit", "Syncing books…") : qsTranslate("BookOrbit", "Sync now"); Layout.fillWidth: true; checked: true; enabled: !client.busy && client.authenticated && window.syncData.total > 0; onClicked: client.syncAll() }
        Text { objectName: "batchProgress"; visible: client.syncBatch.running; text: qsTranslate("BookOrbit", "Files completed: ") + client.syncBatch.completed + qsTranslate("BookOrbit", " of ") + client.syncBatch.total; Layout.fillWidth: true; font.pixelSize: 16 * window.u }
        Action {
            objectName: "stopSync"; visible: client.syncBatch.running; Layout.fillWidth: true
            text: client.syncBatch.stopRequested ? qsTranslate("BookOrbit", "Stopping after the current file…") : qsTranslate("BookOrbit", "Stop after current file")
            enabled: !client.syncBatch.stopRequested; onClicked: client.stopSyncAfterCurrent()
        }
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
                        Text { text: qsTranslate("BookOrbit", "Shared collection"); visible: !modelData.isOwner; font.pixelSize: 14 * window.u; color: "#444444" }
                    }
                    Text { text: modelData.bookCount + "  ›"; font.pixelSize: 18 * window.u }
                }
                Accessible.name: modelData.name + qsTranslate("BookOrbit", ", books: ") + modelData.bookCount
            }
            Text { anchors.centerIn: parent; width: parent.width; visible: collectionList.count === 0 && !client.busy; text: client.authenticated ? (client.canRetry ? qsTranslate("BookOrbit", "Could not load collections.\nTap Retry.") : qsTranslate("BookOrbit", "No collections available yet")) : qsTranslate("BookOrbit", "Sign in to view collections"); wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 19 * window.u }
        }
        ListView {
            id: catalog
            objectName: "catalog"
            onMovementStarted: window.libraryAtBeginning = false
            onOriginYChanged: if (client.offlineOnly && window.libraryAtBeginning)
                Qt.callLater(function() { if (client.offlineOnly && window.libraryAtBeginning) catalog.positionViewAtBeginning() })
            visible: !window.settings && !window.syncView && !client.detailVisible && !client.collectionsView && (!window.coverGrid || client.offlineOnly || count === 0)
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: window.catalogBooks
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
                Accessible.name: qsTranslate("BookOrbit", "About this book: ") + modelData.title + ", " + window.readStatusLabel(modelData)
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
                        Text { anchors.centerIn: parent; visible: cover.status !== Image.Ready; text: qsTranslate("BookOrbit", "Book"); font.pixelSize: 13 * window.u; color: "#555555" }
                        Image { id: cover; anchors.fill: parent; source: { const revision = client.coverRevision; return client.coverUrl(modelData.bookId) }
                            fillMode: Image.PreserveAspectFit; verticalAlignment: Image.AlignTop; cache: false; sourceSize.width: 240; sourceSize.height: 320 }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.minimumHeight: 105 * window.u; Layout.alignment: Qt.AlignTop; spacing: 4 * window.u
                        Text { objectName: "title-" + modelData.bookId; text: modelData.title; Layout.fillWidth: true; textFormat: Text.PlainText; font.pixelSize: 21 * window.u; font.bold: true; maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight }
                        Text { objectName: "author-" + modelData.bookId; text: modelData.author; visible: text.trim().length > 0; textFormat: Text.PlainText; Layout.fillWidth: true; font.pixelSize: 15 * window.u; elide: Text.ElideRight }
                        Text { objectName: "reading-" + modelData.bookId; text: window.readStatusLabel(modelData); visible: text.length > 0; Layout.fillWidth: true; font.pixelSize: 14 * window.u; elide: Text.ElideRight }
                        Text { text: qsTranslate("BookOrbit", "Details saved on this device"); visible: client.offlineOnly; Layout.fillWidth: true; font.pixelSize: 12 * window.u; wrapMode: Text.Wrap }
                        Item { Layout.fillHeight: true }
                        Text {
                            text: modelData.hasConflict ? qsTranslate("BookOrbit", "Choose a position") : modelData.pendingProgress ? qsTranslate("BookOrbit", "Position waiting to be applied") : modelData.localFormats ? modelData.localFormats + qsTranslate("BookOrbit", " · on device") : modelData.formats || qsTranslate("BookOrbit", "No book files")
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
                            text: !modelData.downloaded ? (modelData.needsRepair ? qsTranslate("BookOrbit", "Retry") : qsTranslate("BookOrbit", "Download")) : !modelData.readable ? qsTranslate("BookOrbit", "About this book") : modelData.remoteFileChanged ? qsTranslate("BookOrbit", "Local") : modelData.hasConflict ? qsTranslate("BookOrbit", "Choose") : modelData.pendingProgress ? qsTranslate("BookOrbit", "Check") : qsTranslate("BookOrbit", "Read")
                            icon.source: modelData.downloaded ? "" : "qrc:/icons/download.svg"
                            Accessible.name: (modelData.downloaded ? text : modelData.needsRepair ? qsTranslate("BookOrbit", "Download again") : qsTranslate("BookOrbit", "Download")) + ": " + modelData.title
                            enabled: !client.busy && (modelData.downloaded || modelData.supported && client.authenticated)
                            onClicked: window.quickAction(modelData, index)
                        }
                    }
                }
            }
            Column {
                anchors.centerIn: parent; width: parent.width - 20 * window.u; spacing: 14 * window.u
                visible: catalog.count === 0 && !client.busy
                Text {
                    width: parent.width
                    text: client.offlineOnly ? (client.localQuery.length ? qsTranslate("BookOrbit", "No results for your search.") : qsTranslate("BookOrbit", "No downloaded books yet.\nChoose a book from the catalog.")) : !client.authenticated ? qsTranslate("BookOrbit", "Sign in to download books.") : client.canRetry ? qsTranslate("BookOrbit", "Could not load books.\nTap Retry.") : search.text.length ? qsTranslate("BookOrbit", "No results for your search.") : client.collectionId > 0 ? qsTranslate("BookOrbit", "No books available in this collection yet.") : qsTranslate("BookOrbit", "No books available in the catalog yet.")
                    wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 19 * window.u; color: "#444444"
                }
                Action { visible: search.text.length > 0; anchors.horizontalCenter: parent.horizontalCenter; text: qsTranslate("BookOrbit", "Clear search"); enabled: !client.busy; onClicked: window.clearSearch() }
            }
        }
        GridView {
            id: coverCatalog
            objectName: "coverCatalog"
            visible: !window.settings && !window.syncView && !client.detailVisible && !client.collectionsView && !client.offlineOnly && window.coverGrid && count > 0
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            model: window.catalogBooks
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
                Accessible.name: qsTranslate("BookOrbit", "About this book: ") + modelData.title + ", " + modelData.author + ", " + window.readStatusLabel(modelData) + (modelData.downloaded ? qsTranslate("BookOrbit", ", downloaded") : "")
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
                            text: qsTranslate("BookOrbit", "No cover"); wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter
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
                            Text { anchors.centerIn: parent; text: qsTranslate("BookOrbit", "Downloaded"); font.pixelSize: 12 * window.u; font.bold: true }
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
                        Text { anchors.centerIn: parent; visible: detailCover.status !== Image.Ready; text: qsTranslate("BookOrbit", "No cover"); font.pixelSize: 15 * window.u }
                        Image { id: detailCover; anchors.fill: parent; anchors.margins: 1
                            source: { const revision = client.coverRevision; return client.coverUrl(window.detailData.bookId || 0) }
                            fillMode: Image.PreserveAspectFit; cache: false
                            Accessible.name: qsTranslate("BookOrbit", "Cover: ") + (window.detailData.title || "")
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        Layout.minimumHeight: 280 * window.u; Layout.alignment: Qt.AlignTop; spacing: 8 * window.u
                        Text { objectName: "detailTitle"; text: window.detailData.title || ""; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight; font.bold: true; font.pixelSize: 23 * window.u }
                        Text { text: window.detailData.subtitle || ""; maximumLineCount: 1; elide: Text.ElideRight; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                        Text { text: window.detailData.author || ""; maximumLineCount: 2; elide: Text.ElideRight; visible: text.length > 0; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 17 * window.u }
                        Text { text: (window.detailData.seriesName || "") + (window.detailData.seriesIndex ? " · № " + window.detailData.seriesIndex : ""); textFormat: Text.PlainText; visible: !!window.detailData.seriesName; Layout.fillWidth: true; maximumLineCount: 1; elide: Text.ElideRight; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
                        Item { Layout.fillHeight: true; Layout.minimumHeight: 8 * window.u }
                        Text { objectName: "detailReading"; text: window.readStatusLabel(window.detailData); visible: text.length > 0; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; font.bold: true; font.pixelSize: 14 * window.u; wrapMode: Text.Wrap }
                        ProgressBar {
                            id: detailProgress
                            objectName: "detailProgress"
                            Layout.fillWidth: true; implicitHeight: 7 * window.u
                            from: 0; to: 100
                            visible: typeof window.detailData.readingProgress === "number" && isFinite(window.detailData.readingProgress) && window.detailData.readingProgress >= 0 && window.detailData.readingProgress <= 100
                            value: visible ? window.detailData.readingProgress : 0
                            Accessible.name: qsTranslate("BookOrbit", "Reading progress in BookOrbit")
                            background: Rectangle { color: "#cccccc"; radius: 3 * window.u }
                            contentItem: Item {
                                Rectangle { width: parent.width * detailProgress.position; height: parent.height; color: "black"; radius: 3 * window.u }
                            }
                        }
                        Action {
                            Layout.fillWidth: true
                            objectName: "formatButton"
                            font.pixelSize: 14 * window.u
                            visible: (window.detailData.files || []).length > 1
                            text: qsTranslate("BookOrbit", "Format: ") + (window.detailFormat.format || qsTranslate("BookOrbit", "no files")) + (window.detailFormat.sizeLabel ? " · " + window.detailFormat.sizeLabel : "") + "  ▾"
                            enabled: !client.busy
                            onClicked: window.chooseFormat(false)
                        }
                        Text { text: window.detailData.downloaded ? qsTranslate("BookOrbit", "Downloaded to this device") : window.detailData.needsRepair ? qsTranslate("BookOrbit", "Local file missing or damaged") : qsTranslate("BookOrbit", "File not downloaded yet"); Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
                        Text { text: window.detailData.remoteFileChanged ? qsTranslate("BookOrbit", "Server file differs") : window.detailData.hasConflict ? qsTranslate("BookOrbit", "Positions differ") : window.detailData.pendingProgress ? qsTranslate("BookOrbit", "Incoming position has not been applied yet") : ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; font.bold: true }
                        Action {
                            Layout.fillWidth: true
                            objectName: "downloadButton"
                            font.bold: true; font.pixelSize: 18 * window.u
                            text: window.primaryLabel(window.detailData)
                            visible: !window.detailData.downloaded || !!window.detailData.readable
                            enabled: !client.busy && (window.detailData.downloaded || !!window.detailData.supported)
                            onClicked: {
                                if (!window.detailData.downloaded) { if (client.authenticated) client.downloadSelected(); else window.editConnection() }
                                else if (window.detailData.hasConflict && !window.detailData.remoteFileChanged) client.inspectConflict(window.detailData.fileId)
                                else client.openSelected(false)
                            }
                        }
                    }
                }
                Text { text: !window.detailData.supported && window.detailData.format ? qsTranslate("BookOrbit", "The file exceeds the 100 MiB download limit.") : !window.detailData.readable && window.detailData.format ? qsTranslate("BookOrbit", "Available to download; opening this format from the app is not supported yet.") : window.detailData.format && window.detailData.format !== "EPUB" && window.detailData.format !== "FB2" ? qsTranslate("BookOrbit", "Position sync is not supported for this format yet.") : window.detailData.format === "FB2" ? qsTranslate("BookOrbit", "Experimental FB2 sync: text-only books with short sections. Unsupported structures keep their progress unchanged.") : ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
                Text { objectName: "fileCheckResult"; text: window.detailData.remoteFileChanged ? qsTranslate("BookOrbit", "The server file differs. Progress sync is paused") : window.detailData.syncResult || ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                Text { text: window.detailData.hasConflict ? qsTranslate("BookOrbit", "The reader and BookOrbit have different positions. Check again and choose the one to use.") : window.detailData.pendingProgress ? qsTranslate("BookOrbit", "The incoming position has not been applied yet. Close all books in the built-in reader and check again.") : ""; visible: !window.detailData.remoteFileChanged && text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                Action { objectName: "redownloadButton"; text: qsTranslate("BookOrbit", "Download again"); visible: !!window.detailData.remoteFileChanged; Layout.fillWidth: true; enabled: !client.busy && client.authenticated; onClicked: client.downloadSelected() }
                Action { objectName: "readReaderPosition"; text: qsTranslate("BookOrbit", "Read from reader position"); visible: !!window.detailData.hasConflict && !!window.detailData.readable && !!window.detailData.downloaded; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.openSelected(false) }
                Action { objectName: "reconcilePositions"; text: qsTranslate("BookOrbit", "Compare positions"); visible: !!window.detailData.canSync; Layout.fillWidth: true; enabled: !client.busy && client.authenticated; onClicked: client.syncSelected() }
                Text { text: window.detailData.notice || ""; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
                Text {
                    id: description
                    objectName: "detailDescription"
                    text: window.detailData.description || (window.detailData.detailed ? qsTranslate("BookOrbit", "No description in BookOrbit") : qsTranslate("BookOrbit", "Details not loaded yet"))
                    textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 * window.u
                    maximumLineCount: window.fullDescription ? 100000 : 6; elide: Text.ElideRight
                }
                Action { objectName: "descriptionToggle"; text: window.fullDescription ? qsTranslate("BookOrbit", "Collapse description") : qsTranslate("BookOrbit", "Read more"); visible: window.fullDescription || description.truncated; onClicked: window.fullDescription = !window.fullDescription }
                Action { text: window.detailData.detailed ? qsTranslate("BookOrbit", "Refresh details") : qsTranslate("BookOrbit", "Load details"); visible: client.offlineOnly || !!window.detailData.notice; enabled: !client.busy && client.authenticated; onClicked: client.refreshDetail() }
                Action { objectName: "editionToggle"; text: qsTranslate("BookOrbit", "Edition details  ") + (window.editionExpanded ? "▾" : "›"); Layout.fillWidth: true; onClicked: window.editionExpanded = !window.editionExpanded }
                Text { objectName: "detailEdition"; text: window.editionText(); textFormat: Text.PlainText; visible: window.editionExpanded; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 17 * window.u }
                Text { text: qsTranslate("BookOrbit", "Personal note"); visible: !!window.detailData.personalNote; font.bold: true; font.pixelSize: 20 * window.u }
                Text { text: window.detailData.personalNote || ""; textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 17 * window.u }
                Text { text: qsTranslate("BookOrbit", "In my collections"); visible: (window.detailData.collections || []).length > 0; font.bold: true; font.pixelSize: 20 * window.u }
                Repeater {
                    model: window.detailData.collections || []
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
            Action { objectName: "previousPage"; text: "‹"; Layout.preferredWidth: 100 * window.u; Accessible.name: qsTranslate("BookOrbit", "Previous page"); enabled: !client.busy && client.authenticated && (client.page > 0 || activeCatalog.contentY > 1); onClicked: window.pageContent(-1) }
            Text { objectName: "catalogRange"; text: client.total === 0 ? qsTranslate("BookOrbit", "Books: 0") : (client.catalogQuery ? qsTranslate("BookOrbit", "Found: ") + client.total + qsTranslate("BookOrbit", " · showing ") : qsTranslate("BookOrbit", "Books ")) + (client.page*10+1) + "–" + Math.min((client.page+1)*10, client.total) + (client.catalogQuery ? "" : qsTranslate("BookOrbit", " of ") + client.total); Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 14 * window.u }
            Action { objectName: "nextPage"; text: "›"; Layout.preferredWidth: 100 * window.u; Accessible.name: qsTranslate("BookOrbit", "Next page"); enabled: !client.busy && client.authenticated && ((client.page+1)*10 < client.total || activeCatalog.contentY < Math.max(0, activeCatalog.contentHeight-activeCatalog.height)-1); onClicked: window.pageContent(1) }
        }
        Action { visible: client.downloading; text: qsTranslate("BookOrbit", "Cancel download"); Layout.fillWidth: true; onClicked: client.cancelDownload() }
        Action { objectName: "cancelVerificationButton"; visible: client.verifyingLibrary; text: qsTranslate("BookOrbit", "Cancel verification"); Layout.fillWidth: true; onClicked: client.cancelLibraryVerification() }
        Action { objectName: "retryOperation"; visible: client.canRetry && window.messageVisible; text: qsTranslate("BookOrbit", "Retry"); Layout.fillWidth: true; onClicked: client.retry() }
    }
    Dialog {
        id: formatDialog
        objectName: "formatDialog"
        anchors.centerIn: parent
        width: parent.width - 40 * window.u
        modal: true
        closePolicy: Popup.CloseOnEscape
        header: Text { text: qsTranslate("BookOrbit", "Choose a format"); padding: 16 * window.u; font.pixelSize: 22 * window.u; font.bold: true }
        contentItem: ColumnLayout {
            spacing: 12 * window.u
            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, window.height * 0.5)
                clip: true; spacing: 6 * window.u
                model: window.detailData.files || []
                ScrollBar.vertical: ScrollBar { }
                delegate: Action {
                    required property var modelData
                    objectName: "file-" + modelData.id
                    width: ListView.view.width
                    implicitHeight: Math.max(64 * window.u, contentItem.implicitHeight + 20 * window.u)
                    text: modelData.format + (modelData.sizeLabel ? " · " + modelData.sizeLabel : "") + (modelData.filename ? "\n" + modelData.filename : "") + "\n" + ((modelData.format === "EPUB" || modelData.format === "FB2") ? qsTranslate("BookOrbit", "Reading and position sync") : modelData.readable ? qsTranslate("BookOrbit", "Reading without position sync") : qsTranslate("BookOrbit", "Download only")) + (!modelData.supported ? qsTranslate("BookOrbit", "\nExceeds the 100 MiB download limit") : "")
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: text
                    Accessible.checked: checked
                    checked: window.pendingFileId === modelData.id
                    enabled: !client.busy
                    onClicked: window.pendingFileId = modelData.id
                    contentItem: Text {
                        text: (parent.checked ? "●  " : "○  ") + parent.text + (modelData.downloaded ? qsTranslate("BookOrbit", "\nOn device") : "")
                        textFormat: Text.PlainText; font.pixelSize: 16 * window.u; wrapMode: Text.Wrap; verticalAlignment: Text.AlignVCenter
                        color: parent.checked ? "white" : "black"
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Action { objectName: "formatCancel"; text: qsTranslate("BookOrbit", "Cancel"); onClicked: formatDialog.close() }
                Action {
                    Layout.fillWidth: true; checked: true
                    objectName: "formatConfirm"
                    text: !window.formatDownload ? qsTranslate("BookOrbit", "Choose") : window.pendingFormat.downloaded ? qsTranslate("BookOrbit", "Open details") : qsTranslate("BookOrbit", "Download ") + (window.pendingFormat.format || "")
                    enabled: !client.busy && window.pendingFileId > 0 && (!window.formatDownload || window.pendingFormat.downloaded || window.pendingFormat.supported)
                    onClicked: { client.selectFile(window.pendingFileId); formatDialog.close(); if (window.formatDownload && !window.detailData.downloaded) client.downloadSelected() }
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
        header: RowLayout {
            Text { text: qsTranslate("BookOrbit", "Choose reading position"); Layout.fillWidth: true; Layout.margins: 16 * window.u; font.pixelSize: 22 * window.u; font.bold: true }
            Action { objectName: "conflictLoginButton"; text: qsTranslate("BookOrbit", "Sign in"); visible: window.loginRequired; Layout.rightMargin: 16 * window.u; enabled: !client.busy; onClicked: { client.dismissConflict(); window.editConnection() } }
        }
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
                Text { text: qsTranslate("BookOrbit", "The reader and BookOrbit have different saved positions. Where would you like to continue?"); Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 18 * window.u }
                Text { text: qsTranslate("BookOrbit", "Chapter and excerpt are from the book on this device."); Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444" }
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
                            Text { text: modelData.excerpt ? "«" + modelData.excerpt + "»" : modelData.chapter ? "" : qsTranslate("BookOrbit", "Chapter and excerpt unavailable"); textFormat: Text.PlainText; visible: text.length > 0; Layout.fillWidth: true; Layout.leftMargin: 40 * window.u; Layout.rightMargin: 10 * window.u; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight; font.pixelSize: 17 * window.u }
                            Text { text: modelData.percentage; Layout.fillWidth: true; Layout.leftMargin: 40 * window.u; Layout.rightMargin: 10 * window.u; wrapMode: Text.Wrap; font.pixelSize: 15 * window.u; color: "#444444" }
                            Text { text: modelData.notice || ""; visible: text.length > 0; Layout.fillWidth: true; Layout.leftMargin: 40 * window.u; Layout.rightMargin: 10 * window.u; wrapMode: Text.Wrap; font.pixelSize: 15 * window.u }
                        }
                    }
                }
                Text { text: progressDialog.choice === 0 ? qsTranslate("BookOrbit", "The reader position will replace the position in BookOrbit.") : progressDialog.choice === 1 ? qsTranslate("BookOrbit", "The BookOrbit position will replace the reader position. All books in the built-in reader must be closed.") : qsTranslate("BookOrbit", "Both positions are saved. Choose one or decide later."); Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
                Text { text: client.status; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 16 * window.u }
            }
        }
        footer: ColumnLayout {
            spacing: 10 * window.u
            Action { objectName: "applyConflict"; text: client.busy ? qsTranslate("BookOrbit", "Checking positions…") : progressDialog.choice === 0 ? qsTranslate("BookOrbit", "Use reader position") : progressDialog.choice === 1 ? qsTranslate("BookOrbit", "Use BookOrbit position") : qsTranslate("BookOrbit", "Choose a position"); Layout.fillWidth: true; checked: true; enabled: !client.busy && client.authenticated && progressDialog.choice >= 0; onClicked: client.resolveProgress(progressDialog.choice === 0) }
            Action { objectName: "deferConflict"; text: qsTranslate("BookOrbit", "Decide later"); Layout.fillWidth: true; enabled: !client.busy; onClicked: client.dismissConflict() }
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
        function onSyncBatchChanged() {
            if (client.syncBatch.running && !window.settings && !client.detailVisible) window.syncView = true
        }
        function onCompleted(operation, success) {
            if (window.catalogTransition && window.uiMessage.context === "catalog") {
                const direction = window.pageDirection, offset = window.pageOffset
                Qt.callLater(function() { activeCatalog.contentY = success ? (direction < 0 ? Math.max(0,activeCatalog.contentHeight-activeCatalog.height) : 0) : offset })
                if (success) { window.pageDirection = 0; window.catalogTransition = false }
                else search.text = client.catalogQuery
            }

            if (operation === "sync" && window.syncData.total > 0 && !window.settings) {
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
