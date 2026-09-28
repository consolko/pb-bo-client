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
    property bool settings: false
    font.pixelSize: 18 * u

    function editConnection() {
        server.text = client.server
        username.text = client.username
        password.text = client.demo ? "demo" : ""
        demo.checked = client.demo
        settings = true
    }
    function back() {
        if (Qt.inputMethod.visible) { Qt.inputMethod.hide(); return }
        if (settings) { settings = false; password.text = ""; Qt.inputMethod.hide() }
        else Qt.quit()
    }
    component Action: Button {
        id: action
        implicitHeight: 48 * window.u
        font.pixelSize: 17 * window.u
        padding: 10 * window.u
        background: Rectangle {
            color: action.down || action.checked ? "black" : "white"
            border.color: action.enabled ? "black" : "#999999"
            border.width: action.activeFocus ? 3 : 1
            radius: 3 * window.u
        }
        contentItem: Text {
            text: action.text
            font: action.font
            color: !action.enabled ? "#777777" : (action.down || action.checked ? "white" : "black")
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
    component Field: TextField {
        Layout.fillWidth: true
        implicitHeight: 48 * window.u
        font.pixelSize: 18 * window.u
        enabled: !client.busy
        selectByMouse: true
        onAccepted: Qt.inputMethod.hide()
        color: "black"
        background: Rectangle { color: "white"; border.color: "black"; border.width: parent.activeFocus ? 3 : 1 }
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20 * window.u
        spacing: 12 * window.u
        focus: true
        Keys.onEscapePressed: window.back()
        Keys.onBackPressed: window.back()
        Keys.onPressed: event => {
            if (!window.settings && (event.key === Qt.Key_PageDown || event.key === Qt.Key_Right)) {
                catalog.contentY = Math.min(Math.max(0, catalog.contentHeight-catalog.height), catalog.contentY+catalog.height*0.8)
                event.accepted = true
            } else if (!window.settings && (event.key === Qt.Key_PageUp || event.key === Qt.Key_Left)) {
                catalog.contentY = Math.max(0, catalog.contentY-catalog.height*0.8)
                event.accepted = true
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Text { text: window.settings ? "Подключение" : "BookOrbit"; font.pixelSize: 29 * window.u; font.bold: true; Layout.fillWidth: true }
            Action {
                text: window.settings ? "Назад" : "Настройки"
                enabled: !client.busy
                onClicked: window.settings ? window.back() : window.editConnection()
            }
            Action { visible: !window.settings; text: "Закрыть"; enabled: !client.busy; onClicked: Qt.quit() }
        }
        Text {
            Layout.fillWidth: true
            text: client.demo ? "Локальный стенд · тестовые книги" : client.username + " · " + client.server
            font.pixelSize: 14 * window.u
            color: "#444444"
            maximumLineCount: 2
            elide: Text.ElideRight
            wrapMode: Text.Wrap
        }
        ScrollView {
            visible: window.settings
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth
            ColumnLayout {
                width: parent.width
                spacing: 8 * window.u
                Text { text: "Сохранённые подключения"; font.pixelSize: 16 * window.u; visible: client.accounts.length > 0 }
                ComboBox {
                    Layout.fillWidth: true
                    implicitHeight: 48 * window.u
                    font.pixelSize: 15 * window.u
                    model: client.accounts
                    currentIndex: client.accounts.indexOf(client.username + " · " + client.server)
                    visible: count > 0
                    enabled: !client.busy
                    onActivated: { client.selectAccount(currentIndex); window.editConnection() }
                }
                CheckBox {
                    id: demo
                    indicator: Rectangle {
                        implicitWidth: 24 * window.u; implicitHeight: 24 * window.u
                        x: demo.leftPadding; y: (demo.height-height)/2
                        color: demo.checked ? "black" : "white"; border.color: "black"
                        Text { anchors.centerIn: parent; text: "✓"; visible: demo.checked; color: "white"; font.pixelSize: 19 * window.u }
                    }
                    leftPadding: 0
                    contentItem: Text {
                        text: demo.text; font: demo.font; color: "black"
                        leftPadding: 34 * window.u; verticalAlignment: Text.AlignVCenter
                    }
                    text: "Локальное демо"
                    font.pixelSize: 17 * window.u
                    enabled: !client.busy
                    onClicked: {
                        server.text = checked ? demoServerUrl : "https://"
                        username.text = checked ? "demo" : ""
                        password.text = checked ? "demo" : ""
                    }
                }
                Text { text: "Адрес сервера"; font.pixelSize: 16 * window.u }
                Field { id: server; readOnly: demo.checked; inputMethodHints: Qt.ImhUrlCharactersOnly; Accessible.name: "Адрес сервера" }
                Text { text: "Имя пользователя"; font.pixelSize: 16 * window.u }
                Field { id: username; readOnly: demo.checked; inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText; Accessible.name: "Имя пользователя" }
                Text { text: "Пароль для входа"; font.pixelSize: 16 * window.u }
                Field { id: password; echoMode: TextInput.Password; inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText; Accessible.name: "Пароль" }
                Text {
                    text: "Адрес и имя сохраняются. Пароль и сессия — только до закрытия приложения."
                    Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u; color: "#444444"
                }
                Action {
                    text: "Сохранить без входа"; Layout.fillWidth: true; enabled: !client.busy
                    onClicked: { if (client.configure(server.text, username.text, demo.checked)) window.back() }
                }
                Action {
                    text: "Войти"; Layout.fillWidth: true
                    enabled: !client.busy && username.text.length > 0 && password.text.length > 0
                    onClicked: {
                        if (client.configure(server.text, username.text, demo.checked)) {
                            client.login(username.text, password.text)
                            password.text = ""
                            Qt.inputMethod.hide()
                        }
                    }
                }
                Action {
                    text: "Выйти из аккаунта"; Layout.fillWidth: true
                    visible: client.authenticated; enabled: !client.busy
                    onClicked: { client.logout(); window.back() }
                }
            }
        }
        RowLayout {
            visible: !window.settings
            Layout.fillWidth: true
            Action { text: "Скачанные"; checked: client.offlineOnly; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.showDownloaded(true) }
            Action { text: "Каталог"; checked: !client.offlineOnly; Layout.fillWidth: true; enabled: !client.busy; onClicked: client.showDownloaded(false) }
        }
        RowLayout {
            visible: !window.settings && !client.offlineOnly
            Layout.fillWidth: true
            Field { id: search; placeholderText: "Поиск по названию"; onAccepted: client.refresh(0, text) }
            Action { text: "Найти"; enabled: !client.busy && client.authenticated; onClicked: client.refresh(0, search.text) }
        }
        RowLayout {
            visible: !window.settings && !client.authenticated
            Layout.fillWidth: true
            Text { text: "Скачанные книги доступны без входа"; Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 14 * window.u }
            Action { text: "Войти"; enabled: !client.busy; onClicked: window.editConnection() }
        }
        ListView {
            id: catalog
            visible: !window.settings
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: client.books
            spacing: 12 * window.u
            ScrollBar.vertical: ScrollBar { }
            delegate: Rectangle {
                required property var modelData
                required property int index
                width: catalog.width
                height: 150 * window.u
                color: "white"
                border.color: "#999999"
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10 * window.u
                    spacing: 12 * window.u
                    Rectangle {
                        Layout.preferredWidth: 84 * window.u
                        Layout.preferredHeight: 126 * window.u
                        color: "#eeeeee"
                        border.color: "#aaaaaa"
                        Text { anchors.centerIn: parent; visible: cover.status !== Image.Ready; text: "Нет\nобложки"; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 13 * window.u; color: "#555555" }
                        Image {
                            id: cover
                            anchors.fill: parent
                            anchors.margins: 1
                            source: { const revision = client.coverRevision; return client.coverUrl(modelData.bookId) }
                            fillMode: Image.PreserveAspectFit
                            cache: false
                            sourceSize.width: 240
                            sourceSize.height: 320
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 6 * window.u
                        Text { text: modelData.title; Layout.fillWidth: true; font.pixelSize: 21 * window.u; font.bold: true; maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight }
                        Text { text: modelData.author; Layout.fillWidth: true; font.pixelSize: 14 * window.u; elide: Text.ElideRight }
                        Item { Layout.fillHeight: true }
                        RowLayout {
                            Text { text: modelData.downloaded ? "Без сети" : (modelData.needsRepair ? "Нужна загрузка" : (modelData.supported ? "EPUB" : "Нет EPUB")); font.pixelSize: 13 * window.u; Layout.fillWidth: true }
                            Action {
                                text: modelData.downloaded ? "Читать" : (modelData.needsRepair ? "Заново" : "Скачать")
                                enabled: !client.busy && modelData.supported && (modelData.downloaded || client.authenticated)
                                onClicked: modelData.downloaded ? client.open(index) : client.download(index)
                            }
                        }
                    }
                }
            }
            Text {
                anchors.centerIn: parent; width: parent.width-30*window.u
                visible: catalog.count === 0
                text: client.offlineOnly ? "Скачанных книг пока нет.\nВыберите книгу в каталоге." : (client.authenticated ? "Книги не найдены.\nИзмените запрос и нажмите «Найти»." : "Войдите, чтобы загрузить каталог.")
                wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 19 * window.u; color: "#444444"
            }
        }
        RowLayout {
            visible: !window.settings && !client.offlineOnly
            Layout.fillWidth: true
            Action { text: "Назад"; enabled: !client.busy && client.authenticated && client.page > 0; onClicked: client.refresh(client.page-1, search.text) }
            Text { text: (client.total === 0 ? "Книг: 0" : "Страница " + (client.page+1) + " · книг: " + client.total); Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter; font.pixelSize: 14 * window.u }
            Action { text: "Далее"; enabled: !client.busy && client.authenticated && (client.page+1)*10 < client.total; onClicked: client.refresh(client.page+1, search.text) }
        }
        Text {
            Layout.fillWidth: true
            text: client.status
            font.pixelSize: 15 * window.u
            font.bold: client.busy
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
        Action { visible: client.downloading; text: "Отменить загрузку"; Layout.fillWidth: true; onClicked: client.cancelDownload() }
        Action { visible: client.canRetry; text: "Повторить"; Layout.fillWidth: true; onClicked: client.retry() }
    }
    Connections {
        target: client
        function onCompleted(operation, success) {
            if (operation === "catalog" && client.authenticated && window.settings) {
                window.settings = false
                client.showDownloaded(false)
            }
        }
    }
}
