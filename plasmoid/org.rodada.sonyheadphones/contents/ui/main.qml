import QtQuick
import QtQuick.Layouts
import QtQuick.Window

import org.kde.kirigami as Kirigami
import org.kde.plasma.components as PlasmaComponents3
import org.kde.plasma.core as PlasmaCore
import org.kde.plasma.extras as PlasmaExtras
import org.kde.plasma.plasmoid
import org.kde.plasma.workspace.dbus as DBus

PlasmoidItem {
    id: root

    readonly property string service: "org.rodada.SonyHeadphones"
    readonly property string objectPath: "/org/rodada/SonyHeadphones"

    property var hp: ({})
    readonly property bool daemonUp: watcher.registered
    readonly property bool available: daemonUp && unwrap(hp.Connected, false) === true
    readonly property string deviceName: available ? unwrap(hp.DeviceName, "") : ""
    readonly property string mode: available ? unwrap(hp.Mode, "") : ""
    readonly property int battery: available ? unwrap(hp.Battery, -1) : -1
    readonly property int ambientLevel: available ? unwrap(hp.AmbientLevel, 10) : 10
    readonly property bool focusOnVoice: available && unwrap(hp.FocusOnVoice, false) === true
    readonly property var devices: available ? parseJson(unwrap(hp.Devices, ""), []) : []
    readonly property var connectedDevices: devices.filter(d => d.connected).sort((a, b) => a.order - b.order)
    readonly property var pairedDevices: devices.filter(d => !d.connected)
    readonly property var deviceAction: available ? parseJson(unwrap(hp.DeviceAction, ""), null) : null
    readonly property bool canConnectMore: connectedDevices.length < 2
    property bool showPaired: false

    function mergeProperties(map) {
        const next = Object.assign({}, root.hp)
        for (const k in map)
            next[k] = unwrap(map[k], undefined)
        root.hp = next
    }

    function refresh() {
        const reply = DBus.SessionBus.asyncCall({
            service: root.service,
            path: root.objectPath,
            iface: "org.freedesktop.DBus.Properties",
            member: "GetAll",
            arguments: [new DBus.string(root.service)]
        })
        reply.finished.connect(() => {
            if (reply.isError) {
                console.warn("sony-headphones: GetAll", reply.error.message)
                return
            }
            root.hp = {}
            mergeProperties(reply.value)
        })
    }

    Component.onCompleted: refresh()

    function unwrap(v, fallback) {
        if (v !== null && typeof v === "object" && "value" in v)
            v = v.value
        return v === undefined || v === null ? fallback : v
    }

    function parseJson(text, fallback) {
        if (!text)
            return fallback
        try {
            return JSON.parse(text)
        } catch (e) {
            return fallback
        }
    }

    function deviceStatus(d) {
        const a = root.deviceAction
        if (a && a.mac === d.mac) {
            if (a.state === "pending" || a.state === "in-progress")
                return a.action === "connect" ? i18n("Connecting…") : i18n("Disconnecting…")
            if (a.state === "error" || a.state === "busy")
                return a.action === "connect" ? i18n("Could not connect") : i18n("Could not disconnect")
        }
        if (d.self)
            return d.order === 1 ? i18n("This computer · connected first") : i18n("This computer · connected second")
        if (d.connected)
            return d.order === 1 ? i18n("Connected first") : i18n("Connected second")
        return i18n("Paired, not connected")
    }

    function deviceBusy(d) {
        const a = root.deviceAction
        return !!a && a.mac === d.mac && (a.state === "pending" || a.state === "in-progress")
    }

    function modeLabel(m) {
        switch (m) {
        case "noise-cancelling": return i18n("Noise cancelling")
        case "ambient": return i18n("Ambient sound")
        case "off": return i18n("Off")
        }
        return ""
    }

    function batteryIcon(level) {
        if (level < 0)
            return "battery-missing"
        const step = Math.min(100, Math.round(level / 10) * 10)
        return "battery-" + String(step).padStart(3, "0")
    }

    function call(member, args) {
        const reply = DBus.SessionBus.asyncCall({
            service: root.service,
            path: root.objectPath,
            iface: root.service,
            member: member,
            arguments: args || []
        })
        reply.finished.connect(() => {
            if (reply.isError)
                console.warn("sony-headphones:", member, reply.error.message)
        })
    }

    DBus.DBusServiceWatcher {
        id: watcher
        busType: DBus.BusType.Session
        watchedService: root.service
        onRegisteredChanged: {
            root.hp = {}
            if (registered)
                root.refresh()
        }
    }

    DBus.Properties {
        id: props
        busType: DBus.BusType.Session
        service: root.service
        path: root.objectPath
        iface: root.service
        onPropertiesChanged: (interfaceName, changed, invalidated) => {
            if (interfaceName === root.service)
                root.mergeProperties(changed)
        }
    }

    Plasmoid.icon: "audio-headphones"
    Plasmoid.status: available || expanded ? PlasmaCore.Types.ActiveStatus : PlasmaCore.Types.HiddenStatus

    toolTipMainText: available ? deviceName : i18n("Sony Headphones")
    toolTipSubText: {
        if (!daemonUp)
            return i18n("sony-headphonesd is not running")
        if (!available)
            return i18n("Not connected")
        const parts = [modeLabel(mode)]
        if (battery >= 0)
            parts.push(i18n("battery %1%", battery))
        return parts.join(" · ") + "\n" + i18n("Middle-click to cycle the mode")
    }

    compactRepresentation: MouseArea {
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        hoverEnabled: true
        property bool wasExpanded: false
        onPressed: wasExpanded = root.expanded
        onClicked: mouse => {
            if (mouse.button === Qt.MiddleButton) {
                if (root.available)
                    root.call("CycleMode")
            } else {
                root.expanded = !wasExpanded
            }
        }

        Kirigami.Icon {
            anchors.fill: parent
            source: Plasmoid.icon
            active: parent.containsMouse
            opacity: root.available ? 1 : 0.5
        }

        Rectangle {
            visible: root.mode === "noise-cancelling" || root.mode === "ambient"
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: badge.implicitWidth + Kirigami.Units.smallSpacing
            height: badge.implicitHeight
            radius: height / 2
            color: Kirigami.Theme.highlightColor
            PlasmaComponents3.Label {
                id: badge
                anchors.centerIn: parent
                text: root.mode === "noise-cancelling" ? "NC" : "A"
                font.pixelSize: Math.max(Kirigami.Theme.smallFont.pixelSize * 0.8, 7)
                font.bold: true
                color: Kirigami.Theme.highlightedTextColor
            }
        }
    }

    fullRepresentation: PlasmaExtras.Representation {
        id: popup
        readonly property real contentFitHeight: Math.min(
            (heading.visible ? heading.implicitHeight : 0) + content.implicitHeight + Kirigami.Units.largeSpacing * 2,
            Screen.desktopAvailableHeight * 0.8)
        Layout.minimumWidth: Kirigami.Units.gridUnit * 22
        Layout.preferredWidth: Layout.minimumWidth
        Layout.minimumHeight: root.available ? contentFitHeight : Kirigami.Units.gridUnit * 10
        Layout.preferredHeight: Layout.minimumHeight
        Layout.maximumHeight: Layout.minimumHeight

        header: PlasmaExtras.PlasmoidHeading {
            id: heading
            visible: root.available
            RowLayout {
                anchors.fill: parent
                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 3
                    text: root.deviceName
                    elide: Text.ElideRight
                }
                Kirigami.Icon {
                    visible: root.battery >= 0
                    source: root.batteryIcon(root.battery)
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: Kirigami.Units.iconSizes.small
                }
                PlasmaComponents3.Label {
                    visible: root.battery >= 0
                    text: i18n("%1%", root.battery)
                }
            }
        }

        PlasmaExtras.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: !root.available
            iconName: "audio-headphones"
            text: root.daemonUp ? i18n("Headphones not connected") : i18n("Service not running")
            explanation: root.daemonUp
                ? i18n("Connect the headphones over Bluetooth. If they are also connected to a phone, close Sound Connect there.")
                : i18n("Start it with:\nsystemctl --user start sony-headphonesd")
        }

        PlasmaComponents3.ScrollView {
            id: scroll
            anchors.fill: parent
            visible: root.available
            contentWidth: availableWidth
            contentHeight: content.implicitHeight + Kirigami.Units.largeSpacing * 2
            PlasmaComponents3.ScrollBar.horizontal.policy: PlasmaComponents3.ScrollBar.AlwaysOff

        ColumnLayout {
            id: content
            x: Kirigami.Units.largeSpacing
            y: Kirigami.Units.largeSpacing
            width: scroll.availableWidth - Kirigami.Units.largeSpacing * 2
            spacing: Kirigami.Units.largeSpacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                Repeater {
                    model: [
                        { mode: "noise-cancelling", icon: "audio-volume-muted" },
                        { mode: "ambient", icon: "audio-input-microphone" },
                        { mode: "off", icon: "system-shutdown" }
                    ]
                    delegate: PlasmaComponents3.Button {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        icon.name: modelData.icon
                        text: root.modeLabel(modelData.mode)
                        display: PlasmaComponents3.AbstractButton.TextUnderIcon
                        checkable: true
                        checked: root.mode === modelData.mode
                        onClicked: {
                            checked = Qt.binding(() => root.mode === modelData.mode)
                            root.call("SetMode", [new DBus.string(modelData.mode)])
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.mode === "ambient"
                spacing: Kirigami.Units.smallSpacing

                RowLayout {
                    Layout.fillWidth: true
                    PlasmaComponents3.Label {
                        Layout.fillWidth: true
                        text: i18n("Ambient sound level")
                    }
                    PlasmaComponents3.Label {
                        text: ambientSlider.value
                        font.features: { "tnum": 1 }
                    }
                }
                PlasmaComponents3.Slider {
                    id: ambientSlider
                    Layout.fillWidth: true
                    from: 1
                    to: 20
                    stepSize: 1
                    snapMode: PlasmaComponents3.Slider.SnapAlways
                    value: root.ambientLevel
                    onMoved: sendLevel.restart()
                    Timer {
                        id: sendLevel
                        interval: 150
                        onTriggered: {
                            root.call("SetAmbientLevel", [new DBus.int32(ambientSlider.value)])
                            ambientSlider.value = Qt.binding(() => root.ambientLevel)
                        }
                    }
                }
                PlasmaComponents3.CheckBox {
                    text: i18n("Focus on voice")
                    checked: root.focusOnVoice
                    onToggled: {
                        root.call("SetFocusOnVoice", [new DBus.bool(checked)])
                        checked = Qt.binding(() => root.focusOnVoice)
                    }
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            Kirigami.Heading {
                level: 4
                text: i18n("Devices")
            }

            Repeater {
                model: root.connectedDevices.concat(root.showPaired ? root.pairedDevices : [])
                delegate: RowLayout {
                    id: deviceRow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    opacity: modelData.connected ? 1 : 0.75

                    Kirigami.Icon {
                        source: modelData.self ? "computer" : "network-bluetooth"
                        implicitWidth: Kirigami.Units.iconSizes.smallMedium
                        implicitHeight: Kirigami.Units.iconSizes.smallMedium
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        PlasmaComponents3.Label {
                            Layout.fillWidth: true
                            text: deviceRow.modelData.name
                            font.bold: deviceRow.modelData.connected
                            elide: Text.ElideRight
                        }
                        PlasmaComponents3.Label {
                            Layout.fillWidth: true
                            text: root.deviceStatus(deviceRow.modelData)
                            font: Kirigami.Theme.smallFont
                            opacity: 0.7
                            elide: Text.ElideRight
                        }
                    }

                    PlasmaComponents3.BusyIndicator {
                        visible: root.deviceBusy(deviceRow.modelData)
                        implicitWidth: Kirigami.Units.iconSizes.small
                        implicitHeight: Kirigami.Units.iconSizes.small
                    }

                    PlasmaComponents3.ToolButton {
                        visible: deviceRow.modelData.connected && root.connectedDevices.length > 1
                        icon.name: deviceRow.modelData.active ? "media-playback-playing" : "media-playback-start"
                        checked: deviceRow.modelData.active
                        onClicked: root.call("SwitchPlayback", [new DBus.string(deviceRow.modelData.mac)])
                        PlasmaComponents3.ToolTip.text: i18n("Play audio from this device")
                        PlasmaComponents3.ToolTip.visible: hovered
                        PlasmaComponents3.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }

                    PlasmaComponents3.ToolButton {
                        visible: !deviceRow.modelData.self
                        enabled: !root.deviceBusy(deviceRow.modelData) && (deviceRow.modelData.connected || root.canConnectMore)
                        icon.name: deviceRow.modelData.connected ? "network-disconnect" : "network-connect"
                        onClicked: root.call(deviceRow.modelData.connected ? "DisconnectDevice" : "ConnectDevice",
                                             [new DBus.string(deviceRow.modelData.mac)])
                        PlasmaComponents3.ToolTip.text: deviceRow.modelData.connected ? i18n("Disconnect from the headphones")
                            : root.canConnectMore ? i18n("Connect to the headphones")
                            : i18n("Two devices are already connected; disconnect one first")
                        PlasmaComponents3.ToolTip.visible: hovered
                        PlasmaComponents3.ToolTip.delay: Kirigami.Units.toolTipDelay
                    }
                }
            }

            PlasmaComponents3.ToolButton {
                visible: root.pairedDevices.length > 0
                Layout.fillWidth: true
                icon.name: root.showPaired ? "arrow-up" : "arrow-down"
                text: root.showPaired ? i18n("Hide other devices")
                                      : i18n("Other devices (%1)", root.pairedDevices.length)
                onClicked: root.showPaired = !root.showPaired
            }
        }
        }
    }
}
