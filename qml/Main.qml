import QtQuick 2.12
import QtQuick.Window 2.2
import Ubuntu.Components 1.3
import xonotic 1.0

MainView {
    id: root
    applicationName: "xonotic.ratrad"
    width: Screen.width
    height: Screen.height
    anchorToKeyboard: false

    OrientationHelper {
        automaticOrientationAngle: false
        orientationAngle: Orientation.Landscape

        Item {
            id: gameRoot
            anchors.fill: parent

            property int hudHp: 100
            property int hudAmmo: 50
            property int hudScore: 0

            Connections {
                target: Launcher
                onHpChanged: gameRoot.hudHp = hp
                onAmmoChanged: gameRoot.hudAmmo = ammo
            }

            // ─── LOOK ZONE ────────────────────────────────────────────────
            MouseArea {
                id: lookZone
                anchors.fill: parent
                z: 0

                property real lastX: 0
                property real lastY: 0

                onPressed: {
                    lastX = mouse.x
                    lastY = mouse.y
                }
                onPositionChanged: {
                    var dx = mouse.x - lastX
                    var dy = mouse.y - lastY
                    lastX = mouse.x
                    lastY = mouse.y
                    InputDevice.sendLook(dx, dy)
                }
                onDoubleClicked: {
                    InputDevice.pressButton(0, true)
                    flashAnim.restart()
                    btnReleaseTimer.start()
                }
            }

            // Releases BTN_SOUTH after a brief press window
            Timer {
                id: btnReleaseTimer
                interval: 80
                repeat: false
                onTriggered: InputDevice.pressButton(0, false)
            }

            // ─── CROSSHAIR ────────────────────────────────────────────────
            Item {
                anchors.centerIn: parent
                z: 5
                width: 32
                height: 32
                enabled: false

                Rectangle {
                    width: 24; height: 2
                    color: Qt.rgba(1, 1, 1, 0.8)
                    anchors.centerIn: parent
                }
                Rectangle {
                    width: 2; height: 24
                    color: Qt.rgba(1, 1, 1, 0.8)
                    anchors.centerIn: parent
                }
                Rectangle {
                    width: 12; height: 12; radius: 6
                    color: "transparent"
                    border.color: Qt.rgba(1, 1, 1, 0.6)
                    border.width: 1
                    anchors.centerIn: parent
                }
            }

            // ─── DPAD ─────────────────────────────────────────────────────
            Item {
                id: dpad
                width: 140; height: 140
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.margins: 18
                z: 20

                property bool btnUpPressed: false
                property bool btnDownPressed: false
                property bool btnLeftPressed: false
                property bool btnRightPressed: false

                Rectangle {
                    anchors.fill: parent; radius: 70
                    color: Qt.rgba(1, 1, 1, 0.07)
                    border.color: Qt.rgba(1, 1, 1, 0.18)
                    border.width: 0.5
                }

                // Up
                Rectangle {
                    width: 40; height: 40; radius: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top: parent.top; anchors.topMargin: 4
                    color: dpad.btnUpPressed ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(1, 1, 1, 0.10)
                    border.color: Qt.rgba(1, 1, 1, 0.22); border.width: 0.5
                    Rectangle {
                        width: parent.width * 0.7; height: 1
                        color: Qt.rgba(1, 1, 1, 0.3)
                        anchors.top: parent.top; anchors.topMargin: 3
                        anchors.horizontalCenter: parent.horizontalCenter
                        radius: 1
                    }
                    Text { text: "▲"; color: Qt.rgba(1, 1, 1, 0.8); font.pixelSize: 13; anchors.centerIn: parent }
                }

                // Down
                Rectangle {
                    width: 40; height: 40; radius: 20
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 4
                    color: dpad.btnDownPressed ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(1, 1, 1, 0.10)
                    border.color: Qt.rgba(1, 1, 1, 0.22); border.width: 0.5
                    Rectangle {
                        width: parent.width * 0.7; height: 1
                        color: Qt.rgba(1, 1, 1, 0.3)
                        anchors.top: parent.top; anchors.topMargin: 3
                        anchors.horizontalCenter: parent.horizontalCenter
                        radius: 1
                    }
                    Text { text: "▼"; color: Qt.rgba(1, 1, 1, 0.8); font.pixelSize: 13; anchors.centerIn: parent }
                }

                // Left
                Rectangle {
                    width: 40; height: 40; radius: 20
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left; anchors.leftMargin: 4
                    color: dpad.btnLeftPressed ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(1, 1, 1, 0.10)
                    border.color: Qt.rgba(1, 1, 1, 0.22); border.width: 0.5
                    Rectangle {
                        width: parent.width * 0.7; height: 1
                        color: Qt.rgba(1, 1, 1, 0.3)
                        anchors.top: parent.top; anchors.topMargin: 3
                        anchors.horizontalCenter: parent.horizontalCenter
                        radius: 1
                    }
                    Text { text: "◀"; color: Qt.rgba(1, 1, 1, 0.8); font.pixelSize: 13; anchors.centerIn: parent }
                }

                // Right
                Rectangle {
                    width: 40; height: 40; radius: 20
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right; anchors.rightMargin: 4
                    color: dpad.btnRightPressed ? Qt.rgba(1, 1, 1, 0.25) : Qt.rgba(1, 1, 1, 0.10)
                    border.color: Qt.rgba(1, 1, 1, 0.22); border.width: 0.5
                    Rectangle {
                        width: parent.width * 0.7; height: 1
                        color: Qt.rgba(1, 1, 1, 0.3)
                        anchors.top: parent.top; anchors.topMargin: 3
                        anchors.horizontalCenter: parent.horizontalCenter
                        radius: 1
                    }
                    Text { text: "▶"; color: Qt.rgba(1, 1, 1, 0.8); font.pixelSize: 13; anchors.centerIn: parent }
                }

                Rectangle {
                    width: 14; height: 14; radius: 7
                    anchors.centerIn: parent
                    color: Qt.rgba(1, 1, 1, 0.15)
                    border.color: Qt.rgba(1, 1, 1, 0.2)
                    border.width: 0.5
                }

                MultiPointTouchArea {
                    anchors.fill: parent
                    minimumTouchPoints: 0
                    maximumTouchPoints: 1

                    /**
                     * Converts a touch point into normalised analog axis values
                     * and updates the visual direction indicators.
                     *
                     * Axis 0 (ABS_X): horizontal strafe, range [-1, 1]
                     * Axis 1 (ABS_Y): vertical forward/back, range [-1, 1]
                     */
                    function processTouch(points) {
                        if (points.length === 0) {
                            dpad.btnUpPressed    = false
                            dpad.btnDownPressed  = false
                            dpad.btnLeftPressed  = false
                            dpad.btnRightPressed = false
                            InputDevice.setAxis(0, 0.0)
                            InputDevice.setAxis(1, 0.0)
                            return
                        }

                        var pt  = points[0]
                        var cx  = dpad.width  / 2
                        var cy  = dpad.height / 2

                        // Normalise displacement to [-1, 1] and clamp to unit circle
                        var dx  = (pt.x - cx) / cx
                        var dy  = (pt.y - cy) / cy
                        var mag = Math.sqrt(dx * dx + dy * dy)
                        if (mag > 1.0) { dx /= mag; dy /= mag; mag = 1.0 }

                        // Update visual highlights using cardinal heading
                        if (mag < 0.1) {
                            dpad.btnUpPressed = dpad.btnDownPressed =
                                dpad.btnLeftPressed = dpad.btnRightPressed = false
                        } else {
                            var heading = Math.atan2(dx, -dy) * 180 / Math.PI
                            if (heading < 0) heading += 360
                            dpad.btnUpPressed    = (heading >= 315 || heading < 45)
                            dpad.btnRightPressed = (heading >= 45  && heading < 135)
                            dpad.btnDownPressed  = (heading >= 135 && heading < 225)
                            dpad.btnLeftPressed  = (heading >= 225 && heading < 315)
                        }

                        // Send analog values — dx = strafe, dy = forward/back
                        InputDevice.setAxis(0, dx)
                        InputDevice.setAxis(1, dy)
                    }

                    onTouchUpdated: processTouch(touchPoints)
                    onReleased: {
                        dpad.btnUpPressed    = false
                        dpad.btnDownPressed  = false
                        dpad.btnLeftPressed  = false
                        dpad.btnRightPressed = false
                        InputDevice.setAxis(0, 0.0)
                        InputDevice.setAxis(1, 0.0)
                    }
                }
            }

            // ─── HUD PILLS ────────────────────────────────────────────────
            Row {
                id: hudRow
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                anchors.margins: 18
                spacing: 8
                z: 20

                Rectangle {
                    width: 72; height: 30; radius: 15
                    color: Qt.rgba(0, 0, 0, 0.45)
                    border.color: Qt.rgba(1, 1, 1, 0.18); border.width: 0.5
                    Row {
                        anchors.centerIn: parent; spacing: 4
                        Text { text: "❤"; color: Qt.rgba(1, 0.3, 0.3, 0.9); font.pixelSize: 11 }
                        Text { text: gameRoot.hudHp; color: "white"; font.pixelSize: 12; font.bold: true }
                    }
                }

                Rectangle {
                    width: 72; height: 30; radius: 15
                    color: Qt.rgba(0, 0, 0, 0.45)
                    border.color: Qt.rgba(1, 1, 1, 0.18); border.width: 0.5
                    Row {
                        anchors.centerIn: parent; spacing: 4
                        Text { text: "◎"; color: Qt.rgba(1, 0.85, 0.2, 0.9); font.pixelSize: 11 }
                        Text { text: gameRoot.hudAmmo; color: "white"; font.pixelSize: 12; font.bold: true }
                    }
                }

                Rectangle {
                    width: 72; height: 30; radius: 15
                    color: Qt.rgba(0, 0, 0, 0.45)
                    border.color: Qt.rgba(1, 1, 1, 0.18); border.width: 0.5
                    Row {
                        anchors.centerIn: parent; spacing: 4
                        Text { text: "★"; color: Qt.rgba(0.3, 0.7, 1, 0.9); font.pixelSize: 11 }
                        Text { text: gameRoot.hudScore; color: "white"; font.pixelSize: 12; font.bold: true }
                    }
                }
            }

            // ─── SHOOT FLASH ──────────────────────────────────────────────
            Rectangle {
                id: shootFlash
                anchors.fill: parent
                z: 25
                color: "white"
                opacity: 0
                visible: opacity > 0

                SequentialAnimation {
                    id: flashAnim
                    NumberAnimation {
                        target: shootFlash; property: "opacity"
                        from: 0; to: 0.08; duration: 75
                        easing.type: Easing.OutQuad
                    }
                    NumberAnimation {
                        target: shootFlash; property: "opacity"
                        from: 0.08; to: 0; duration: 75
                        easing.type: Easing.InQuad
                    }
                }
            }
        }
    }
}
