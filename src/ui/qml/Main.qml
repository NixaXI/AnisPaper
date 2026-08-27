import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    visible: true
    width: 1320
    height: 860
    minimumWidth: 1080
    minimumHeight: 720
    title: "ANISPAPER · STARLIGHT"
    color: "#0a0a0c"
    font.family: "Chakra Petch"

    palette {
        window: "#14110d"
        windowText: "#fff8e7"
        base: "#0a0a0c"
        text: "#fff8e7"
        button: "#1c1810"
        buttonText: "#fff8e7"
        highlight: "#ffd54a"
        highlightedText: "#171204"
    }

    readonly property color cBg: "#0a0a0c"
    readonly property color cPanel: "#14110d"
    readonly property color cPanel2: "#1c1810"
    readonly property color cCard: "#171310"
    readonly property color cCardHi: "#211a12"
    readonly property color cLine: "#332a19"
    readonly property color cLineSoft: "#4a3d20"
    readonly property color cCream: "#fff8e7"
    readonly property color cMuted: "#8d8471"
    readonly property color cAccent: "#ffd54a"
    readonly property color cAccent2: "#ffb300"
    readonly property color cBright: "#ffea8f"
    readonly property color cDanger: "#c45c5c"
    readonly property color cOk: "#7ee787"

    property string view: "catalog"
    property string heroTitle: ""
    property string heroType: ""
    property string heroPreview: ""
    property bool stageLive: false
    property bool atmoOn: true
    property bool gamingOn: false
    property bool curtainClosing: false
    property int wsPage: 1
    property bool wsBusy: false
    property string wsError: ""
    property string typeChip: ""

    ListModel { id: wsModel }

    function typeColor(t) {
        if (t === "scene") return "#ffd54a"
        if (t === "video") return "#6ecbff"
        if (t === "web") return "#7ee787"
        return "#d3b8ff"
    }
    function typeName(t) {
        return t === "static" ? "IMAGE" : String(t).toUpperCase()
    }
    function fileUrl(path) {
        if (!path || !path.length)
            return ""
        if (path.indexOf("file:") === 0 || path.indexOf("http") === 0 || path.indexOf("qrc:") === 0)
            return path
        return "file://" + path
    }
    function selectCard(id, title, type, preview) {
        client.selectedId = id
        heroTitle = title
        heroType = type
        heroPreview = preview || ""
    }
    function setTypeChip(value) {
        typeChip = value
        searchField.text = value
        client.filter = value
    }
    function shineNow() {
        if (!client.online || client.applying || client.selectedId === "")
            return
        stageLive = true
        curtainClosing = false
        client.applySelected()
        flashAnim.restart()
    }
    function curtainNow() {
        if (!client.online || client.applying)
            return
        curtainClosing = true
        curtainTimer.restart()
    }

    Timer {
        id: curtainTimer
        interval: 360
        onTriggered: {
            client.stopSelected()
            stageLive = false
            curtainClosing = false
        }
    }

    function wsSearch(page) {
        if (wsBusy || page < 1)
            return
        wsPage = page
        wsModel.clear()
        wsError = ""
        wsBusy = true
        var q = wsField.text.trim()
        var url = "https://steamcommunity.com/workshop/browse/?appid=431960"
                + "&browsesort=textsearch&section=readytouseitems&p=" + page
                + (q.length ? "&searchtext=" + encodeURIComponent(q) : "")
        var xhr = new XMLHttpRequest()
        xhr.open("GET", url, true)
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE)
                return
            if (xhr.status !== 200) {
                wsBusy = false
                wsError = "HTTP " + xhr.status + " contactando Steam Community."
                return
            }
            var re = /sharedfiles\/filedetails\/\?id=(\d+)/g
            var m
            var seen = {}
            var ids = []
            while ((m = re.exec(xhr.responseText)) !== null) {
                if (!seen[m[1]]) {
                    seen[m[1]] = true
                    ids.push(m[1])
                }
            }
            if (ids.length === 0) {
                wsBusy = false
                wsError = "Sin resultados en esta página."
                return
            }
            wsDetails(ids.slice(0, 30))
        }
        try { xhr.send() } catch (e) { wsBusy = false; wsError = "Red no disponible." }
    }

    function wsDetails(ids) {
        var body = ""
        for (var i = 0; i < ids.length; i++)
            body += (i ? "&" : "") + "publishedfileids%5B" + i + "%5D=" + ids[i]
        var xhr = new XMLHttpRequest()
        xhr.open("POST", "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/?format=json", true)
        xhr.setRequestHeader("Content-Type", "application/x-www-form-urlencoded;charset=UTF-8")
        xhr.onreadystatechange = function() {
            if (xhr.readyState !== XMLHttpRequest.DONE)
                return
            wsBusy = false
            if (xhr.status !== 200) {
                wsError = "HTTP " + xhr.status + " en GetPublishedFileDetails."
                return
            }
            try {
                var data = JSON.parse(xhr.responseText)
                var arr = (data.response && data.response.publishedfiledetails) || []
                for (var j = 0; j < arr.length; j++) {
                    var d = arr[j]
                    if (!d || !d.publishedfileid)
                        continue
                    wsModel.append({
                        "wsid": String(d.publishedfileid),
                        "wtitle": d.title || String(d.publishedfileid),
                        "wpreview": d.preview_url || ""
                    })
                }
                if (wsModel.count === 0)
                    wsError = "Steam no devolvió detalles para esta página."
            } catch (e) {
                wsError = "Respuesta inválida de Steam."
            }
        }
        try { xhr.send(body) } catch (e) { wsBusy = false; wsError = "Red no disponible." }
    }

    Connections {
        target: client
        function onSelectedIdChanged() {
            if (client.selectedId === "") {
                root.heroTitle = ""
                root.heroType = ""
                root.heroPreview = ""
            }
        }
    }

    // ---- ambient backdrop (few items, opacity only) ----
    Rectangle {
        id: aurora
        anchors.fill: parent
        visible: atmoOn
        opacity: gamingOn ? 0.42 : 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "#22ffd54a" }
            GradientStop { position: 0.45; color: "#000a0a0c" }
            GradientStop { position: 1.0; color: "#149682ff" }
        }
        z: 0
    }

    Repeater {
        model: atmoOn ? 12 : 0
        Text {
            z: 0
            text: index % 3 === 0 ? "✦" : "✧"
            color: cAccent
            font.pixelSize: 7 + (index % 5)
            x: (index * 97) % Math.max(80, root.width - 20)
            y: 20 + ((index * 53) % Math.max(80, root.height - 40))
            SequentialAnimation on opacity {
                loops: Animation.Infinite
                NumberAnimation { from: 0; to: 0.45; duration: 1600 + index * 180 }
                NumberAnimation { from: 0.45; to: 0.08; duration: 1400 + index * 90 }
                NumberAnimation { from: 0.08; to: 0; duration: 1200 }
                PauseAnimation { duration: 400 + index * 70 }
            }
        }
    }

    Rectangle {
        id: sidebar
        z: 2
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 252
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#14110d" }
            GradientStop { position: 1.0; color: "#0a0a0c" }
        }
        Rectangle {
            anchors.right: parent.right
            width: 1
            height: parent.height
            color: cLine
        }

        Column {
            anchors.fill: parent
            anchors.leftMargin: 22
            anchors.rightMargin: 14
            anchors.topMargin: 18
            anchors.bottomMargin: 16
            spacing: 5

            Row {
                spacing: 11
                Rectangle {
                    width: 48
                    height: 48
                    radius: 13
                    color: "#1c1810"
                    clip: true
                    Image {
                        anchors.fill: parent
                        source: "qrc:/anis-star.png"
                        fillMode: Image.PreserveAspectCrop
                    }
                }
                Column {
                    spacing: 2
                    anchors.verticalCenter: parent.verticalCenter
                    Text {
                        text: "ANISPAPER"
                        color: cCream
                        font.pixelSize: 17
                        font.bold: true
                        font.letterSpacing: 3
                    }
                    Text {
                        text: "STARLIGHT · KDE PLASMA"
                        color: cAccent
                        font.pixelSize: 9
                        font.letterSpacing: 2
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 5
                radius: 3
                clip: true
                opacity: gamingOn ? 0.45 : 0.9
                color: "#101010"
                Row {
                    spacing: 0
                    Repeater {
                        model: 24
                        Rectangle {
                            width: 9
                            height: 18
                            y: -6
                            rotation: -45
                            color: index % 2 === 0 ? cAccent : "#101010"
                        }
                    }
                }
            }

            Item { width: 1; height: 8 }

            Repeater {
                model: [
                    { "key": "catalog", "ico": "▦", "label": "Catalog" },
                    { "key": "workshop", "ico": "◉", "label": "Workshop" },
                    { "key": "settings", "ico": "⚙", "label": "Settings" }
                ]
                Rectangle {
                    width: sidebar.width - 36
                    height: 42
                    radius: 11
                    color: root.view === modelData.key ? "#21ffd54a" : "transparent"
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 13
                        spacing: 10
                        Text {
                            text: modelData.ico
                            width: 20
                            color: root.view === modelData.key ? cAccent : cMuted
                            font.pixelSize: 15
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text {
                            text: modelData.label
                            color: root.view === modelData.key ? cAccent : cMuted
                            font.pixelSize: 13
                            font.bold: true
                            font.letterSpacing: 0.8
                        }
                    }
                    Text {
                        visible: root.view === modelData.key
                        anchors.right: parent.right
                        anchors.rightMargin: 11
                        anchors.verticalCenter: parent.verticalCenter
                        text: "✦"
                        color: cBright
                        font.pixelSize: 11
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.view = modelData.key
                    }
                }
            }

            Text {
                topPadding: 12
                text: "STAGES"
                color: cMuted
                font.pixelSize: 10
                font.bold: true
                font.letterSpacing: 2.6
            }

            Repeater {
                model: client.monitorNames
                Rectangle {
                    width: sidebar.width - 36
                    height: 48
                    radius: 11
                    color: client.selectedOutput === modelData ? "#21ffd54a" : cPanel2
                    border.width: 1
                    border.color: client.selectedOutput === modelData ? cAccent : cLine
                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 9
                        Rectangle {
                            width: 7
                            height: 7
                            radius: 4
                            anchors.verticalCenter: parent.verticalCenter
                            color: client.selectedOutput === modelData ? cOk : cMuted
                        }
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2
                            Text {
                                text: modelData
                                color: cCream
                                font.pixelSize: 12
                                font.bold: true
                            }
                            Text {
                                text: client.selectedOutput === modelData ? "MAIN STAGE" : "standby"
                                color: client.selectedOutput === modelData ? cAccent : cMuted
                                font.pixelSize: 8
                                font.letterSpacing: 1.6
                            }
                        }
                        Item { width: 8; height: 1 }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: client.selectedOutput === modelData ? client.measuredFps : "— fps"
                            color: client.selectedOutput === modelData ? cAccent : cMuted
                            font.pixelSize: 10
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: client.selectedOutput = modelData
                    }
                }
            }

            Text {
                visible: client.monitorNames.length === 0
                text: client.online ? "sin salidas detectadas" : "esperando anis-paperd…"
                color: cMuted
                font.pixelSize: 11
            }

            Item { width: 1; height: 16 }

            Rectangle {
                width: sidebar.width - 36
                height: 58
                radius: 13
                color: cPanel2
                border.width: 1
                border.color: cLine
                Row {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text { text: "ATMOSPHERE"; color: cCream; font.pixelSize: 11; font.letterSpacing: 1.4; font.bold: true }
                        Text { text: "Starfall & ambient light"; color: cMuted; font.pixelSize: 9 }
                    }
                    Item { width: 8; height: 1 }
                    Rectangle {
                        width: 44
                        height: 23
                        radius: 12
                        anchors.verticalCenter: parent.verticalCenter
                        color: atmoOn ? cAccent : cBg
                        border.width: 1
                        border.color: atmoOn ? cAccent : cLine
                        Rectangle {
                            width: 17
                            height: 17
                            radius: 9
                            y: 2
                            x: atmoOn ? 22 : 2
                            color: atmoOn ? "#120e0a" : cMuted
                        }
                        MouseArea { anchors.fill: parent; onClicked: atmoOn = !atmoOn }
                    }
                }
            }

            Rectangle {
                width: sidebar.width - 36
                height: 58
                radius: 13
                color: cPanel2
                border.width: 1
                border.color: cLine
                Row {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Text { text: "GAMING MODE"; color: cCream; font.pixelSize: 11; font.letterSpacing: 1.4; font.bold: true }
                        Text { text: "Stage dimmed while playing"; color: cMuted; font.pixelSize: 9 }
                    }
                    Rectangle {
                        width: 44
                        height: 23
                        radius: 12
                        anchors.verticalCenter: parent.verticalCenter
                        color: gamingOn ? cAccent : cBg
                        border.width: 1
                        border.color: gamingOn ? cAccent : cLine
                        Rectangle {
                            width: 17
                            height: 17
                            radius: 9
                            y: 2
                            x: gamingOn ? 22 : 2
                            color: gamingOn ? "#120e0a" : cMuted
                        }
                        MouseArea { anchors.fill: parent; onClicked: gamingOn = !gamingOn }
                    }
                }
            }

            Text {
                width: parent.width
                topPadding: 10
                horizontalAlignment: Text.AlignHCenter
                text: "✦  \"You just watch.\nThe desktop performs itself.\"  ✦"
                color: cMuted
                font.pixelSize: 10
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "ANISPAPER · STARLIGHT · ZERO ELECTRON"
                color: cMuted
                opacity: 0.55
                font.pixelSize: 9
                font.letterSpacing: 1.4
            }
        }
    }

    Rectangle {
        id: mainArea
        z: 2
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        color: "transparent"

        Rectangle {
            id: topbar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 62
            color: "#9e14110d"
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: cLine
            }
            Row {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 16
                spacing: 12

                TextField {
                    id: searchField
                    width: 330
                    height: 38
                    anchors.verticalCenter: parent.verticalCenter
                    placeholderText: "Search scenes, videos, stars…"
                    color: cCream
                    placeholderTextColor: cMuted
                    font.pixelSize: 13
                    leftPadding: 32
                    background: Rectangle {
                        radius: 11
                        color: cBg
                        border.width: 1
                        border.color: searchField.activeFocus ? cAccent : cLine
                    }
                    Text {
                        text: "⌕"
                        color: cMuted
                        font.pixelSize: 16
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    onTextChanged: client.filter = text
                }

                Repeater {
                    model: [
                        { "l": "ALL", "v": "" },
                        { "l": "SCENE", "v": "scene" },
                        { "l": "VIDEO", "v": "video" },
                        { "l": "WEB", "v": "web" },
                        { "l": "IMAGE", "v": "static" }
                    ]
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        height: 32
                        width: chipLab.implicitWidth + 26
                        radius: 99
                        color: root.typeChip === modelData.v ? cAccent : "transparent"
                        border.width: 1
                        border.color: root.typeChip === modelData.v ? cAccent : cLine
                        Text {
                            id: chipLab
                            anchors.centerIn: parent
                            text: modelData.l
                            font.pixelSize: 11
                            font.bold: true
                            font.letterSpacing: 1
                            color: root.typeChip === modelData.v ? "#141005" : cMuted
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.setTypeChip(modelData.v)
                        }
                    }
                }

                Item { width: Math.max(8, mainArea.width - 980); height: 1 }

                Button {
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 38
                    implicitHeight: 38
                    onClicked: client.refreshCatalog()
                    contentItem: Text {
                        text: "↻"
                        color: cMuted
                        font.pixelSize: 16
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 11
                        color: "transparent"
                        border.width: 1
                        border.color: cLine
                    }
                }
            }
        }

        // ================= CATALOG =================
        Item {
            id: catalogView
            visible: root.view === "catalog"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: topbar.bottom
            anchors.bottom: playerbar.top

            Column {
                anchors.fill: parent
                anchors.margins: 18
                spacing: 14

                // cinematic main stage
                Item {
                    id: stageFrame
                    width: parent.width
                    height: 300

                    Rectangle {
                        anchors.fill: parent
                        radius: 21
                        color: "transparent"
                        border.width: 1
                        border.color: "#4affd54a"
                    }
                    Rectangle {
                        id: orbitSeg
                        width: 70
                        height: 2
                        radius: 2
                        color: cBright
                        y: 0
                        visible: atmoOn
                        SequentialAnimation on x {
                            loops: Animation.Infinite
                            NumberAnimation { from: -70; to: stageFrame.width; duration: 14000; easing.type: Easing.Linear }
                        }
                    }

                    Rectangle {
                        id: stage
                        anchors.fill: parent
                        anchors.margins: 2
                        radius: 20
                        clip: true
                        color: cCard
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: "#211a12" }
                            GradientStop { position: 0.55; color: "#171310" }
                            GradientStop { position: 1.0; color: "#0a0a0c" }
                        }

                        Image {
                            anchors.fill: parent
                            source: fileUrl(heroPreview)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: true
                            opacity: heroPreview.length ? (stageLive ? 0.72 : 0.42) : 0
                            visible: heroPreview.length > 0
                        }

                        Rectangle {
                            anchors.fill: parent
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0.0; color: "#eb0a0a0c" }
                                GradientStop { position: 0.42; color: "#a80a0a0c" }
                                GradientStop { position: 0.72; color: "#2e0a0a0c" }
                                GradientStop { position: 1.0; color: "#610a0a0c" }
                            }
                        }
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: parent.height * 0.55
                            gradient: Gradient {
                                GradientStop { position: 0.0; color: "#00000000" }
                                GradientStop { position: 1.0; color: "#e00a0a0c" }
                            }
                        }

                        Rectangle {
                            width: parent.width * 0.56
                            height: parent.height * 0.85
                            x: -parent.width * 0.06
                            y: parent.height * 0.32
                            radius: width
                            color: "#21ffd54a"
                            opacity: stageLive ? 0.8 : 0.45
                        }

                        Rectangle {
                            visible: gamingOn
                            anchors.fill: parent
                            color: "#000000"
                            opacity: 0.34
                        }

                        Image {
                            id: stageAnis
                            width: Math.min(300, parent.width * 0.38)
                            height: parent.height * 0.92
                            anchors.right: parent.right
                            anchors.rightMargin: -28
                            anchors.verticalCenter: parent.verticalCenter
                            source: "qrc:/anis-star.png"
                            fillMode: Image.PreserveAspectCrop
                            opacity: stageLive ? 0.22 : (heroPreview.length ? 0.32 : 0.5)
                        }

                        Rectangle {
                            id: sheen
                            width: parent.width * 0.30
                            height: parent.height
                            color: "transparent"
                            opacity: atmoOn ? 0.55 : 0
                            gradient: Gradient {
                                orientation: Gradient.Horizontal
                                GradientStop { position: 0.0; color: "#00000000" }
                                GradientStop { position: 0.45; color: "#14ffffff" }
                                GradientStop { position: 0.60; color: "#21ffd54a" }
                                GradientStop { position: 1.0; color: "#00000000" }
                            }
                            SequentialAnimation on x {
                                loops: Animation.Infinite
                                running: atmoOn
                                PauseAnimation { duration: 6800 }
                                NumberAnimation { from: -200; to: 1100; duration: 3200; easing.type: Easing.InOutQuad }
                            }
                        }

                        Text {
                            visible: !heroTitle.length && !stageLive
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: parent.height * 0.34
                            text: "✦"
                            color: cBright
                            font.pixelSize: 22
                            SequentialAnimation on opacity {
                                loops: Animation.Infinite
                                NumberAnimation { from: 0.35; to: 1; duration: 1500 }
                                NumberAnimation { from: 1; to: 0.35; duration: 1500 }
                            }
                        }

                        Rectangle {
                            id: wash
                            anchors.fill: parent
                            color: "#000000"
                            opacity: curtainClosing ? 0.72 : 0
                            Behavior on opacity { NumberAnimation { duration: 280 } }
                        }
                        Rectangle {
                            width: parent.width
                            height: parent.height / 2
                            color: "#0b0b0d"
                            y: curtainClosing ? 0 : -height - 2
                            Behavior on y { NumberAnimation { duration: 340; easing.type: Easing.InCubic } }
                        }
                        Rectangle {
                            width: parent.width
                            height: parent.height / 2
                            color: "#131017"
                            y: curtainClosing ? parent.height / 2 : parent.height + 2
                            Behavior on y { NumberAnimation { duration: 340; easing.type: Easing.InCubic } }
                        }

                        SequentialAnimation {
                            id: flashAnim
                            running: false
                            PropertyAnimation { target: stage; property: "opacity"; to: 1.0; duration: 1 }
                            PropertyAnimation { target: stageFrame; property: "scale"; to: 1.01; duration: 180 }
                            PropertyAnimation { target: stageFrame; property: "scale"; to: 1.0; duration: 280 }
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            anchors.leftMargin: 28
                            anchors.bottomMargin: 22
                            width: parent.width * 0.62
                            spacing: 8
                            z: 4

                            Row {
                                spacing: 12
                                Text {
                                    text: "✦  " + (client.selectedOutput.length ? client.selectedOutput : "MAIN STAGE") + " · MAIN STAGE"
                                    color: cAccent
                                    font.pixelSize: 11
                                    font.bold: true
                                    font.letterSpacing: 3
                                }
                                Rectangle {
                                    height: 22
                                    width: modeLab.implicitWidth + 20
                                    radius: 99
                                    color: stageLive && !gamingOn ? cAccent : "transparent"
                                    border.width: 1
                                    border.color: gamingOn && stageLive ? cDanger
                                                  : stageLive ? cAccent
                                                  : heroTitle.length ? cLineSoft : cLineSoft
                                    Text {
                                        id: modeLab
                                        anchors.centerIn: parent
                                        text: !client.online ? "OFFLINE"
                                              : client.applying ? "APPLYING…"
                                              : gamingOn && stageLive ? "STAGE DIMMED"
                                              : stageLive ? "NOW SHINING"
                                              : heroTitle.length ? "READY IN THE WINGS"
                                              : "WAITING IN THE WINGS"
                                        font.pixelSize: 9
                                        font.bold: true
                                        font.letterSpacing: 1.4
                                        color: !client.online ? cDanger
                                               : gamingOn && stageLive ? cDanger
                                               : stageLive && !gamingOn ? "#141005"
                                               : heroTitle.length ? cAccent : cMuted
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                text: heroTitle.length ? heroTitle
                                      : (client.selectedOutput.length ? client.selectedOutput : "MAIN STAGE")
                                color: cCream
                                font.pixelSize: 34
                                font.bold: true
                                elide: Text.ElideRight
                            }
                            Text {
                                width: Math.min(430, parent.width)
                                text: stageLive
                                      ? (gamingOn ? "Holding the last frame — the stage rests while you play."
                                                  : "Now shining. The desktop performs itself.")
                                      : (heroTitle.length ? "Ready in the wings. Press SHINE to put it on stage."
                                                          : "Waiting in the wings — choose a wallpaper below and make it shine.")
                                color: cCream
                                opacity: 0.82
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }
                            Text {
                                text: (heroType.length ? typeName(heroType) + " · " : "")
                                      + client.measuredFps + " · cap " + client.fpsCap + " FPS"
                                color: cMuted
                                font.pixelSize: 11
                                font.letterSpacing: 1.4
                            }
                            Row {
                                spacing: 8
                                Rectangle {
                                    height: 26
                                    width: fpsChip.implicitWidth + 24
                                    radius: 99
                                    color: "#8c0a0a0c"
                                    border.width: 1
                                    border.color: cLineSoft
                                    Row {
                                        anchors.centerIn: parent
                                        spacing: 7
                                        Rectangle {
                                            width: 6
                                            height: 6
                                            radius: 3
                                            anchors.verticalCenter: parent.verticalCenter
                                            color: stageLive && !gamingOn ? cOk : cDanger
                                        }
                                        Text {
                                            id: fpsChip
                                            text: client.measuredFps
                                            color: cAccent
                                            font.pixelSize: 10
                                            font.bold: true
                                        }
                                    }
                                }
                                Rectangle {
                                    height: 26
                                    width: 92
                                    radius: 99
                                    color: "#8c0a0a0c"
                                    border.width: 1
                                    border.color: cLineSoft
                                    Text {
                                        anchors.centerIn: parent
                                        text: gamingOn ? "GAMING" : "RUNNING"
                                        color: cCream
                                        font.pixelSize: 10
                                        font.bold: true
                                    }
                                }
                            }
                            Row {
                                spacing: 10
                                Button {
                                    id: shineBtn
                                    enabled: client.online && !client.applying && client.selectedId !== ""
                                    implicitWidth: 168
                                    implicitHeight: 46
                                    onClicked: root.shineNow()
                                    contentItem: Text {
                                        text: client.applying ? "APPLYING…" : "★ SHINE"
                                        color: "#171204"
                                        font.pixelSize: 14
                                        font.bold: true
                                        font.letterSpacing: 1.6
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        radius: 11
                                        color: shineBtn.enabled ? cBright : cLine
                                    }
                                }
                                Button {
                                    id: curtainBtn
                                    enabled: client.online && !client.applying && stageLive
                                    implicitHeight: 46
                                    onClicked: root.curtainNow()
                                    contentItem: Text {
                                        text: "CURTAIN"
                                        color: curtainBtn.enabled ? cMuted : "#555555"
                                        font.pixelSize: 12
                                        font.bold: true
                                        font.letterSpacing: 1.2
                                        horizontalAlignment: Text.AlignHCenter
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    background: Rectangle {
                                        radius: 11
                                        color: "transparent"
                                        border.width: 1
                                        border.color: curtainBtn.enabled ? cLine : "#222222"
                                    }
                                }
                            }
                        }
                    }
                }

                Text {
                    text: "CATALOG — " + client.catalog.count + " WALLPAPERS"
                    color: cMuted
                    font.pixelSize: 11
                    font.bold: true
                    font.letterSpacing: 2.4
                }

                GridView {
                    id: grid
                    width: parent.width
                    height: parent.height - 348
                    clip: true
                    cellWidth: 210
                    cellHeight: 196
                    cacheBuffer: 400
                    model: client.catalog
                    ScrollBar.vertical: ScrollBar { }

                    delegate: Item {
                        width: grid.cellWidth
                        height: grid.cellHeight
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 7
                            radius: 16
                            color: client.selectedId === itemId ? cCardHi : cCard
                            border.width: client.selectedId === itemId ? 2 : 1
                            border.color: client.selectedId === itemId ? cAccent : cLine
                            clip: true

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.selectCard(itemId, title, type, preview)
                                onDoubleClicked: {
                                    root.selectCard(itemId, title, type, preview)
                                    root.shineNow()
                                }
                            }

                            Image {
                                id: thumb
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                height: 128
                                source: preview && preview.length ? fileUrl(preview) : "qrc:/anis-star.png"
                                fillMode: Image.PreserveAspectCrop
                                asynchronous: true
                                cache: true
                                sourceSize.width: 320
                                sourceSize.height: 180
                            }
                            Rectangle {
                                anchors.left: thumb.left
                                anchors.right: thumb.right
                                anchors.bottom: thumb.bottom
                                height: 46
                                gradient: Gradient {
                                    GradientStop { position: 0.0; color: "#00000000" }
                                    GradientStop { position: 1.0; color: "#a80a0a0c" }
                                }
                            }
                            Rectangle {
                                x: 9
                                y: 9
                                height: 19
                                width: badgeTxt.implicitWidth + 18
                                radius: 99
                                color: root.typeColor(type)
                                Text {
                                    id: badgeTxt
                                    anchors.centerIn: parent
                                    text: root.typeName(type)
                                    color: "#0c0c0c"
                                    font.pixelSize: 9
                                    font.bold: true
                                    font.letterSpacing: 1
                                }
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 8
                                text: "★"
                                font.pixelSize: 15
                                color: favorite ? cAccent : "#66ffffff"
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: 11
                                text: title && title.length ? title : "Sin título"
                                color: cCream
                                elide: Text.ElideRight
                                font.pixelSize: 12
                                font.bold: true
                            }
                        }
                    }
                }
            }
        }

        // ================= WORKSHOP =================
        Column {
            id: workshopView
            visible: root.view === "workshop"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: topbar.bottom
            anchors.bottom: playerbar.top
            anchors.margins: 18
            spacing: 12

            Text {
                text: "WORKSHOP — WALLPAPER ENGINE 431960"
                color: cMuted
                font.pixelSize: 11
                font.bold: true
                font.letterSpacing: 2.4
            }
            Rectangle {
                width: parent.width
                height: 58
                radius: 14
                color: cCard
                border.width: 1
                border.color: cLine
                Text {
                    anchors.fill: parent
                    anchors.margins: 14
                    wrapMode: Text.WordWrap
                    text: "Busca y suscribite desde Steam sin publicar AnisPaper. Tras descargar, ↻ biblioteca recarga el catálogo local."
                    color: cCream
                    opacity: 0.85
                    font.pixelSize: 12
                }
            }
            Row {
                spacing: 8
                TextField {
                    id: wsField
                    width: 360
                    height: 38
                    placeholderText: "Buscar en la Workshop de WE…"
                    color: cCream
                    placeholderTextColor: cMuted
                    font.pixelSize: 13
                    background: Rectangle {
                        radius: 11
                        color: cBg
                        border.width: 1
                        border.color: wsField.activeFocus ? cAccent : cLine
                    }
                    onAccepted: wsSearch(1)
                }
                Button {
                    enabled: !wsBusy
                    implicitWidth: 120
                    implicitHeight: 38
                    onClicked: wsSearch(1)
                    contentItem: Text {
                        text: "BUSCAR"
                        color: "#171204"
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle { radius: 11; color: parent.enabled ? cAccent : cLine }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: wsBusy
                    text: "buscando…"
                    color: cAccent
                    font.pixelSize: 11
                }
                Button {
                    enabled: wsPage > 1 && !wsBusy
                    implicitWidth: 36
                    implicitHeight: 36
                    onClicked: wsSearch(wsPage - 1)
                    contentItem: Text { text: "‹"; color: enabled ? cCream : "#555555"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 9; color: "transparent"; border.width: 1; border.color: cLine }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "pág " + wsPage
                    color: cMuted
                }
                Button {
                    enabled: !wsBusy
                    implicitWidth: 36
                    implicitHeight: 36
                    onClicked: wsSearch(wsPage + 1)
                    contentItem: Text { text: "›"; color: enabled ? cCream : "#555555"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 9; color: "transparent"; border.width: 1; border.color: cLine }
                }
                Button {
                    implicitHeight: 36
                    onClicked: client.refreshCatalog()
                    contentItem: Text { text: "↻ biblioteca"; color: cMuted; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; font.pixelSize: 11 }
                    background: Rectangle { radius: 9; color: "transparent"; border.width: 1; border.color: cLine }
                }
            }
            Text {
                visible: wsError !== ""
                text: "⚠ " + wsError
                color: cDanger
            }
            GridView {
                id: wsGrid
                width: parent.width
                height: parent.height - 160
                clip: true
                cellWidth: 216
                cellHeight: 248
                cacheBuffer: 200
                model: wsModel
                ScrollBar.vertical: ScrollBar { }
                delegate: Item {
                    width: wsGrid.cellWidth
                    height: wsGrid.cellHeight
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 7
                        radius: 16
                        color: cCard
                        border.width: 1
                        border.color: cLine
                        clip: true
                        Image {
                            id: wsThumb
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: 132
                            source: wpreview
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            cache: true
                        }
                        Column {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: wsThumb.bottom
                            anchors.margins: 10
                            spacing: 8
                            Text {
                                width: parent.width
                                text: wtitle
                                color: cCream
                                elide: Text.ElideRight
                                font.pixelSize: 12
                                font.bold: true
                            }
                            Rectangle {
                                height: 24
                                width: subTxt.implicitWidth + 16
                                radius: 99
                                color: cBright
                                Text {
                                    id: subTxt
                                    anchors.centerIn: parent
                                    text: "+ SUSCRIBIRSE"
                                    color: "#171204"
                                    font.pixelSize: 9
                                    font.bold: true
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Qt.openUrlExternally("steam://url/CommunityFilePage/" + wsid)
                                }
                            }
                        }
                    }
                }
            }
        }

        // ================= SETTINGS =================
        Column {
            visible: root.view === "settings"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: topbar.bottom
            anchors.bottom: playerbar.top
            anchors.margins: 18
            spacing: 14
            width: 560

            Text {
                text: "SETTINGS"
                color: cMuted
                font.pixelSize: 11
                font.bold: true
                font.letterSpacing: 2.4
            }
            Rectangle {
                width: 560
                height: 150
                radius: 15
                color: cCard
                border.width: 1
                border.color: cLine
                Column {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 12
                    Text { text: "PERFORMANCE"; color: cAccent; font.pixelSize: 11; font.letterSpacing: 2.4 }
                    Row {
                        spacing: 12
                        Text { width: 90; text: "Volume"; color: cMuted; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                        Slider {
                            id: setVol
                            width: 280
                            from: 0; to: 100; stepSize: 1; live: true
                            Binding on value { when: !setVol.pressed; value: client.volumePercent }
                            onMoved: client.volumePercent = Math.round(value)
                        }
                        Text { text: Math.round(setVol.value) + "%"; color: cCream; width: 44 }
                    }
                    Row {
                        spacing: 12
                        Text { width: 90; text: "FPS cap"; color: cMuted; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                        Slider {
                            id: setFps
                            width: 280
                            from: 15; to: 60; stepSize: 1; live: true
                            Binding on value { when: !setFps.pressed; value: client.fpsCap }
                            onMoved: client.fpsCap = Math.round(value)
                        }
                        Text { text: Math.round(setFps.value); color: cCream; width: 44 }
                    }
                }
            }
            Rectangle {
                width: 560
                height: 110
                radius: 15
                color: cCard
                border.width: 1
                border.color: cLine
                Column {
                    anchors.fill: parent
                    anchors.margins: 18
                    spacing: 8
                    Text { text: "STEAM / WORKSHOP"; color: cAccent; font.pixelSize: 11; font.letterSpacing: 2.4 }
                    Text {
                        width: parent.width
                        wrapMode: Text.WordWrap
                        color: cMuted
                        font.pixelSize: 11
                        text: "AnisPaper lee tu biblioteca local de Wallpaper Engine (app 431960). La pestaña Workshop busca en Steam Community sin publicar AnisPaper ni pagar Steam Direct."
                    }
                }
            }
        }

        Rectangle {
            id: playerbar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 62
            color: cPanel
            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: cLine
            }
            Row {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 16
                spacing: 16

                Rectangle {
                    width: 56
                    height: 37
                    radius: 8
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#222222"
                    clip: true
                    Image {
                        anchors.fill: parent
                        source: fileUrl(heroPreview)
                        fillMode: Image.PreserveAspectCrop
                        visible: heroPreview.length > 0
                        asynchronous: true
                    }
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Text {
                        text: heroTitle.length ? heroTitle : "Nothing on stage"
                        color: cCream
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        width: 180
                    }
                    Text {
                        text: "output: " + (client.selectedOutput.length ? client.selectedOutput : "—")
                              + (stageLive ? (gamingOn ? " · dimmed" : " · now shining") : " · idle")
                        color: cMuted
                        font.pixelSize: 10
                    }
                }

                Item { width: 20; height: 1 }

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Text { text: "VOL"; color: cMuted; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.8; anchors.verticalCenter: parent.verticalCenter }
                    Slider {
                        id: volSlider
                        width: 100
                        from: 0; to: 100; stepSize: 1; live: true
                        Binding on value { when: !volSlider.pressed; value: client.volumePercent }
                        onMoved: client.volumePercent = Math.round(value)
                    }
                    Text { text: Math.round(volSlider.value) + "%"; color: cMuted; font.pixelSize: 11; width: 34; anchors.verticalCenter: parent.verticalCenter }
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Text { text: "FPS"; color: cMuted; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.8; anchors.verticalCenter: parent.verticalCenter }
                    Slider {
                        id: fpsSlider
                        width: 100
                        from: 15; to: 60; stepSize: 1; live: true
                        Binding on value { when: !fpsSlider.pressed; value: client.fpsCap }
                        onMoved: client.fpsCap = Math.round(value)
                    }
                    Text { text: Math.round(fpsSlider.value); color: cMuted; font.pixelSize: 11; width: 28; anchors.verticalCenter: parent.verticalCenter }
                }

                Item { width: Math.max(8, mainArea.width - 980); height: 1 }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: client.statusLine
                    color: client.online ? cOk : cDanger
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    width: 220
                }
                Button {
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: client.online && !client.applying
                    implicitHeight: 38
                    onClicked: root.curtainNow()
                    contentItem: Text {
                        text: "CURTAIN"
                        color: parent.enabled ? cMuted : "#555555"
                        font.bold: true
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 11
                        color: "transparent"
                        border.width: 1
                        border.color: cLine
                    }
                }
                Button {
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: client.online && !client.applying && client.selectedId !== ""
                    implicitWidth: 140
                    implicitHeight: 42
                    onClicked: root.shineNow()
                    contentItem: Text {
                        text: "★ SHINE"
                        color: "#171204"
                        font.bold: true
                        font.pixelSize: 13
                        font.letterSpacing: 1.4
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 11
                        color: parent.enabled ? cBright : cLine
                    }
                }
            }
        }
    }

    Rectangle {
        visible: client.toast.length > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 78
        width: Math.min(380, Math.max(280, toastText.implicitWidth + 48))
        height: 52
        radius: 12
        color: cPanel2
        border.width: 1
        border.color: cLine
        z: 50
        Rectangle {
            width: 2
            height: parent.height
            color: cAccent
            radius: 1
        }
        Text {
            id: toastText
            anchors.centerIn: parent
            text: client.toast
            color: cCream
            font.pixelSize: 12
        }
        MouseArea { anchors.fill: parent; onClicked: client.dismissToast() }
        Timer {
            interval: 5200
            running: client.toast.length > 0
            onTriggered: client.dismissToast()
        }
    }
}
