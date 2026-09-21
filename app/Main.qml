import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import ServoRig

ApplicationWindow {
    id: win
    visible: true
    width: 420
    height: 880
    title: qsTr("Servo rig")
    color: theme.panel

    // --- design tokens -----------------------------------------------------
    // A machined instrument panel: blue-black body, one hairline bezel, and
    // a single recessed actuator whose ring is also the hold timer.
    QtObject {
        id: theme
        readonly property color panel:    "#131a22"
        readonly property color deck:     "#1d2631"
        readonly property color recess:   "#0c1117"
        readonly property color bezel:    "#2e3a48"
        readonly property color ink:      "#e6e3dc"
        readonly property color muted:    "#8a96a4"
        readonly property color actuator: "#ce4a34"
        readonly property color countdown:"#e8a33d"
        readonly property color live:     "#5fbf7f"
    }

    // --- rig ---------------------------------------------------------------

    ServoLink {
        id: rig
    }

    MjpegClient {
        id: video
        url: snapshotMode ? rig.snapshotUrl : rig.streamUrl
    }

    Component.onCompleted: {
        rig.startPolling()
        video.start()
    }

    // Fraction of the hold still to run, 0 to 1. Drives the actuator ring.
    readonly property real holdProgress:
        rig.remainingMs > 0
            ? Math.min(1, rig.remainingMs / Math.max(1, rig.holdMs))
            : 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ===== camera, the hero ============================================
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: 240

            Rectangle {
                anchors.fill: parent
                color: theme.recess
            }

            VideoSurface {
                id: surface
                anchors.fill: parent
                source: video
            }

            // Shown until the first frame lands, and whenever it stalls.
            Text {
                anchors.centerIn: parent
                width: parent.width * 0.7
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                visible: !video.receiving
                color: theme.muted
                font.pixelSize: 15
                text: video.message !== ""
                          ? video.message
                          : qsTr("Waiting for the camera")
            }

            // Feed health, bottom left of the image so it sits over the
            // darkest part of most scenes rather than the subject.
            Row {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 14
                spacing: 8
                opacity: 0.9

                Rectangle {
                    width: 7; height: 7; radius: 3.5
                    anchors.verticalCenter: parent.verticalCenter
                    color: video.receiving ? theme.live : theme.muted
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    color: theme.muted
                    font.pixelSize: 13
                    text: video.receiving
                              ? qsTr("%1 fps").arg(video.fps.toFixed(0))
                              : qsTr("no feed")
                }
            }

            // Settings opener, kept visually quiet.
            AbstractButton {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 8
                width: 44; height: 44
                onClicked: settings.open()

                contentItem: Text {
                    anchors.centerIn: parent
                    color: theme.muted
                    font.pixelSize: 20
                    text: "\u2699"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 22
                    color: parent.pressed ? Qt.rgba(0, 0, 0, 0.35) : "transparent"
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: theme.bezel
        }

        // ===== control deck ================================================
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 330
            color: theme.deck

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 22
                spacing: 18

                // telemetry: what the servo is doing right now
                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        color: theme.ink
                        font.pixelSize: 16
                        text: rig.online
                                  ? qsTr("Servo at %1\u00B0").arg(rig.angle)
                                  : qsTr("Rig not responding")
                    }

                    Item { Layout.fillWidth: true }

                    Text {
                        color: rig.holding ? theme.countdown : theme.muted
                        font.pixelSize: 16
                        text: {
                            if (!rig.online) return rig.host
                            if (rig.holding) return qsTr("holding")
                            if (rig.moving)  return qsTr("moving")
                            return qsTr("at rest")
                        }
                    }
                }

                // the actuator: one button, and its ring is the hold timer
                Item {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 176
                    Layout.preferredHeight: 176

                    Canvas {
                        id: ring
                        anchors.fill: parent
                        antialiasing: true

                        property real progress: win.holdProgress

                        onProgressChanged: requestPaint()
                        onPaint: {
                            const ctx = getContext("2d")
                            const cx = width / 2
                            const cy = height / 2
                            const r  = Math.min(cx, cy) - 5
                            const lw = 4

                            ctx.reset()

                            ctx.beginPath()
                            ctx.arc(cx, cy, r, 0, Math.PI * 2)
                            ctx.lineWidth = lw
                            ctx.strokeStyle = theme.bezel
                            ctx.stroke()

                            if (progress <= 0)
                                return

                            // Drains clockwise from the top as the hold runs out.
                            ctx.beginPath()
                            ctx.arc(cx, cy, r, -Math.PI / 2,
                                    -Math.PI / 2 + Math.PI * 2 * progress)
                            ctx.lineWidth = lw
                            ctx.lineCap = "round"
                            ctx.strokeStyle = theme.countdown
                            ctx.stroke()
                        }
                    }

                    AbstractButton {
                        id: actuator
                        anchors.centerIn: parent
                        width: 142
                        height: 142
                        enabled: rig.online && !rig.holding && !rig.moving
                        focus: true
                        Keys.onReturnPressed: clicked()
                        Keys.onSpacePressed: clicked()
                        onClicked: rig.rotate()

                        background: Rectangle {
                            radius: width / 2
                            color: actuator.enabled
                                       ? (actuator.pressed
                                              ? Qt.darker(theme.actuator, 1.3)
                                              : theme.actuator)
                                       : Qt.rgba(0.3, 0.35, 0.42, 0.4)
                            border.width: actuator.activeFocus ? 2 : 0
                            border.color: theme.ink

                            // Motion only in answer to a press.
                            scale: actuator.pressed ? 0.96 : 1.0
                            Behavior on scale {
                                NumberAnimation { duration: 90 }
                            }
                        }

                        contentItem: Column {
                            spacing: 2

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                color: theme.ink
                                font.pixelSize: rig.holding ? 40 : 19
                                font.weight: rig.holding ? Font.Light : Font.Medium
                                horizontalAlignment: Text.AlignHCenter
                                // Fixed width stops the digits from shuffling
                                // the layout as the countdown ticks.
                                width: rig.holding ? 96 : implicitWidth
                                text: rig.holding
                                          ? (rig.remainingMs / 1000).toFixed(1)
                                          : qsTr("Rotate")
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                color: Qt.rgba(1, 1, 1, 0.65)
                                font.pixelSize: 13
                                horizontalAlignment: Text.AlignHCenter
                                text: rig.holding
                                          ? qsTr("seconds left")
                                          : qsTr("to %1\u00B0, hold %2 s")
                                                .arg(180)
                                                .arg((rig.holdMs / 1000).toFixed(0))
                            }
                        }
                    }
                }

                // quiet escape hatch, for cutting a hold short
                AbstractButton {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: 200
                    Layout.preferredHeight: 46
                    enabled: rig.online
                    onClicked: rig.goHome()

                    background: Rectangle {
                        radius: 23
                        color: parent.pressed ? Qt.rgba(1, 1, 1, 0.06)
                                              : "transparent"
                        border.width: 1
                        border.color: theme.bezel
                    }
                    contentItem: Text {
                        color: parent.enabled ? theme.muted
                                              : Qt.rgba(0.54, 0.59, 0.64, 0.4)
                        font.pixelSize: 15
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Return to 0 now")
                    }
                }

                Text {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 20
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    color: theme.muted
                    font.pixelSize: 13
                    text: rig.message
                }
            }
        }
    }

    // ===== settings ========================================================

    Drawer {
        id: settings
        edge: Qt.BottomEdge
        width: win.width
        height: Math.min(win.height * 0.8, 470)
        dim: true

        background: Rectangle {
            color: theme.panel
            border.width: 1
            border.color: theme.bezel
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            spacing: 16

            Text {
                color: theme.ink
                font.pixelSize: 18
                font.weight: Font.Medium
                text: qsTr("Rig settings")
            }

            Text {
                color: theme.muted
                font.pixelSize: 13
                text: qsTr("Address of the ESP32-S3, host or host:port")
            }

            TextField {
                id: hostField
                Layout.fillWidth: true
                text: rig.host
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                color: theme.ink
                font.pixelSize: 16
                background: Rectangle {
                    color: theme.recess
                    radius: 4
                    border.width: 1
                    border.color: hostField.activeFocus ? theme.actuator
                                                        : theme.bezel
                }
                onEditingFinished: rig.host = text
            }

            Text {
                color: theme.muted
                font.pixelSize: 13
                text: qsTr("Hold for %1 seconds before returning")
                          .arg((rig.holdMs / 1000).toFixed(1))
            }

            Slider {
                Layout.fillWidth: true
                from: 1000
                to: 60000
                stepSize: 500
                value: rig.holdMs
                onMoved: rig.holdMs = value
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: theme.muted
                    font.pixelSize: 13
                    text: qsTr("Poll still images instead of streaming video. Choppier, but survives a weak signal.")
                }

                Switch {
                    checked: video.snapshotMode
                    onToggled: video.snapshotMode = checked
                }
            }

            Item { Layout.fillHeight: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                AbstractButton {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 46
                    onClicked: rig.pushHold()

                    background: Rectangle {
                        radius: 23
                        color: parent.pressed ? Qt.rgba(1, 1, 1, 0.06)
                                              : "transparent"
                        border.width: 1
                        border.color: theme.bezel
                    }
                    contentItem: Text {
                        color: theme.muted
                        font.pixelSize: 15
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Save to the Arduino")
                    }
                }

                AbstractButton {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 46
                    onClicked: {
                        rig.host = hostField.text
                        video.restart()
                        settings.close()
                    }

                    background: Rectangle {
                        radius: 23
                        color: parent.pressed ? Qt.darker(theme.actuator, 1.3)
                                              : theme.actuator
                    }
                    contentItem: Text {
                        color: theme.ink
                        font.pixelSize: 15
                        font.weight: Font.Medium
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("Apply")
                    }
                }
            }
        }
    }
}
