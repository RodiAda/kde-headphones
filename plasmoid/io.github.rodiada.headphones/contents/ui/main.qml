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

    readonly property string service: "io.github.rodiada.Headphones"
    readonly property string objectPath: "/io/github/rodiada/Headphones"

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
    readonly property var cfg: available ? parseJson(unwrap(hp.Settings, ""), {}) : {}
    readonly property string alert: available ? unwrap(hp.Alert, "") : ""
    readonly property var batteries: available ? parseJson(unwrap(hp.Batteries, ""), []) : []
    readonly property bool hasMainBattery: batteries.some(b => b.part === "main")
    readonly property var modes: available ? parseJson(unwrap(hp.Modes, ""), []) : []
    property bool showPaired: false
    property int currentTab: 0

    function eqBandLabels(count) {
        if (count === 5)
            return ["400", "1k", "2.5k", "6.3k", "16k"]
        if (count === 8)
            return ["100", "200", "400", "800", "1.6k", "3.2k", "6.4k", "12.8k"]
        if (count === 10)
            return ["31", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"]
        return []
    }

    function batteryPartLabel(part) {
        switch (part) {
        case "left": return i18nc("left earbud", "L")
        case "right": return i18nc("right earbud", "R")
        case "case": return i18nc("charging case", "Case")
        }
        return ""
    }

    function batterySummary() {
        if (root.hasMainBattery || root.batteries.length === 0)
            return root.battery >= 0 ? i18n("battery %1%", root.battery) : ""
        return root.batteries.map(b => i18n("%1 %2%", root.batteryPartLabel(b.part), b.level)).join(", ")
    }
    readonly property var ambientRange: cfg.ambient || null
    readonly property string eqPresetKey: {
        const eq = cfg.equalizer
        if (!eq)
            return ""
        const preset = (eq.presets || []).find(p => p.id === eq.preset)
        return preset ? preset.key : ""
    }

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
        case "adaptive": return i18n("Adaptive")
        }
        return ""
    }

    function presetLabel(key) {
        switch (key) {
        case "off": return i18n("Off")
        case "rock": return i18n("Rock")
        case "pop": return i18n("Pop")
        case "jazz": return i18n("Jazz")
        case "dance": return i18n("Dance")
        case "edm": return i18n("EDM")
        case "rnb-hiphop": return i18n("R&B/Hip-Hop")
        case "acoustic": return i18n("Acoustic")
        case "bright": return i18n("Bright")
        case "excited": return i18n("Excited")
        case "mellow": return i18n("Mellow")
        case "relaxed": return i18n("Relaxed")
        case "vocal": return i18n("Vocal")
        case "treble-boost": return i18n("Treble Boost")
        case "bass-boost": return i18n("Bass Boost")
        case "speech": return i18n("Speech")
        case "custom": return i18n("Custom")
        case "soft": return i18n("Soft")
        case "dynamic": return i18n("Dynamic")
        case "clear": return i18n("Clear")
        case "balanced": return i18n("Balanced")
        case "more-bass": return i18n("More bass")
        case "more-treble": return i18n("More treble")
        case "voice": return i18n("Voice")
        case "electronic": return i18n("Electronic")
        case "classical": return i18n("Classical")
        case "enhance-vocals": return i18n("Enhance vocals")
        case "dirac": return i18n("Dirac Opteo")
        case "default": return i18n("Default")
        case "lively": return i18n("Lively")
        case "movie": return i18n("Movie")
        case "gaming": return i18n("Gaming")
        case "podcast": return i18n("Podcast")
        case "punchy": return i18n("Punchy")
        case "adaptive": return i18n("Adaptive")
        case "natural": return i18n("Natural")
        case "concert": return i18n("Concert")
        case "shooter": return i18n("Shooter")
        case "standard": return i18n("Standard")
        case "volume-boost": return i18n("Volume boost")
        case "soundcore-signature": return i18n("Soundcore Signature")
        case "bass-booster": return i18n("Bass booster")
        case "bass-reducer": return i18n("Bass reducer")
        case "deep": return i18n("Deep")
        case "flat": return i18n("Flat")
        case "hip-hop": return i18n("Hip-hop")
        case "latin": return i18n("Latin")
        case "lounge": return i18n("Lounge")
        case "piano": return i18n("Piano")
        case "rnb": return i18n("R&B")
        case "small-speakers": return i18n("Small speakers")
        case "spoken-word": return i18n("Spoken word")
        case "treble-booster": return i18n("Treble booster")
        case "treble-reducer": return i18n("Treble reducer")
        case "smooth": return i18n("Smooth")
        case "energize": return i18n("Energize")
        }
        const generic = /^preset-(\d+)$/.exec(key)
        if (generic)
            return i18n("Preset %1", generic[1])
        const user = /^user-(\d+)$/.exec(key)
        if (user)
            return i18n("User Setting %1", user[1])
        return key
    }

    function humanize(key) {
        const words = String(key).replace(/([a-z0-9])([A-Z])/g, "$1 $2").replace(/[-_]+/g, " ").toLowerCase()
        return words.charAt(0).toUpperCase() + words.slice(1)
    }

    function controlLabel(key) {
        switch (key) {
        case "conversationAwareness": return i18n("Conversation awareness")
        case "earDetection": return i18n("Automatic ear detection")
        case "personalizedVolume": return i18n("Personalized volume")
        case "adaptiveAudio": return i18n("Adaptive audio")
        case "ancStrength": return i18n("Noise cancelling strength")
        case "ambientStrength": return i18n("Ambient sound strength")
        case "multipoint": return i18n("Connect to 2 devices simultaneously")
        case "lowLatency": return i18n("Low latency mode")
        case "voicePrompts": return i18n("Voice prompts")
        case "touchControls": return i18n("Touch controls")
        case "wearDetection": return i18n("Wear detection")
        case "spatialAudio": return i18n("Spatial audio")
        case "headTracking": return i18n("Head tracking")
        case "bassBoost": return i18n("Bass boost")
        case "autoPowerOff": return i18n("Automatic power off")
        case "volumeEq": return i18n("Volume EQ")
        case "monoAudio": return i18n("Mono audio")
        case "balance": return i18n("Balance")
        case "eqLowBass": return i18n("Low bass")
        case "eqBass": return i18n("Bass")
        case "eqMid": return i18n("Mid")
        case "eqTreble": return i18n("Treble")
        case "eqUpperTreble": return i18n("Upper treble")
        case "offListeningMode": return i18n("Allow Off listening mode")
        case "autoAnswer": return i18n("Answer calls automatically")
        case "windBlock": return i18n("Wind noise reduction")
        case "ancLevel": return i18n("Noise cancelling level")
        case "sidetone": return i18n("Hear your own voice on calls")
        case "audioMode": return i18n("Audio mode")
        case "voiceBoost": return i18n("Voice boost")
        case "touchTone": return i18n("Touch tones")
        case "surroundSound": return i18n("Surround sound")
        case "adaptiveSound": return i18n("Adaptive sound")
        case "noiseCancellingMode": return i18n("Noise cancelling scene")
        case "transparencyMode": return i18n("Transparency mode")
        case "smartPause": return i18n("Smart pause")
        case "comfortCall": return i18n("Comfort call")
        case "batteryProtection": return i18n("Battery protection")
        case "windNoiseReduction": return i18n("Wind noise reduction")
        case "muteReminder": return i18n("Mute reminder")
        case "autoRejectCalls": return i18n("Reject incoming calls automatically")
        case "buttonSounds": return i18n("Button sounds")
        case "sidetoneLevel": return i18n("Own voice level on calls")
        case "callEqualizer": return i18n("Call equalizer")
        }
        const band = /^eqBand(\d+)$/.exec(key)
        if (band)
            return i18n("Band %1", band[1])
        return humanize(key)
    }

    function optionLabel(option) {
        switch (option) {
        case "off": return i18n("Off")
        case "on": return i18n("On")
        case "low": return i18n("Low")
        case "medium": return i18n("Medium")
        case "high": return i18n("High")
        case "auto": return i18n("Auto")
        case "wind": return i18n("Wind")
        case "comfort": return i18n("Comfort")
        case "normal": return i18n("Normal")
        case "ultra": return i18n("Ultra")
        case "dynamic": return i18n("Dynamic")
        case "dual-engine": return i18n("Dual engine")
        case "regular": return i18n("Regular")
        case "enhance-voice": return i18n("Enhance voices")
        case "enhance-ambient": return i18n("Enhance ambient sound")
        case "still": return i18n("Fixed")
        case "motion": return i18n("Head tracking")
        case "never": return i18n("Never")
        case "transport": return i18n("Transport")
        case "outdoor": return i18n("Outdoor")
        case "indoor": return i18n("Indoor")
        case "full": return i18n("Full")
        case "vocal": return i18n("Vocal")
        case "talk": return i18n("Talk")
        case "manual": return i18n("Manual")
        case "tones": return i18n("Tones")
        case "voice": return i18n("Voice")
        case "neutral": return i18n("Neutral")
        case "bass": return i18n("Bass")
        case "treble": return i18n("Treble")
        }
        const minutes = /^(\d+)-min$/.exec(option)
        if (minutes)
            return i18n("%1 min", minutes[1])
        return humanize(option)
    }

    function generalLabel(subject) {
        switch (subject) {
        case "TOUCH_PANEL_SETTING": return i18n("Touch sensor control panel")
        case "MULTIPOINT_SETTING": return i18n("Connect to 2 devices simultaneously")
        }
        return subject
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

    function apply(change) {
        call("Apply", [new DBus.string(JSON.stringify(change))])
    }

    function applyEqualizer(change) {
        const eq = root.cfg.equalizer
        if (eq && eq.customPreset !== undefined && (eq.preset < eq.customPreset || eq.preset > eq.customPreset + 5))
            change.preset = eq.customPreset
        apply({ equalizer: change })
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

    toolTipMainText: available ? deviceName : i18n("Headphones")
    toolTipSubText: {
        if (!daemonUp)
            return i18n("headphonesd is not running")
        if (!available)
            return i18n("Not connected")
        if (cfg.untested === true)
            return [modeLabel(mode), batterySummary()].filter(s => s.length > 0).join(" · ") + "\n" + i18n("Support for this model is untested")
        const parts = [modeLabel(mode), batterySummary()].filter(s => s.length > 0)
        let text = parts.join(" · ")
        if (modes.length > 1)
            text += "\n" + i18n("Middle-click to cycle the mode")
        return text
    }

    component SettingSwitch: RowLayout {
        id: settingSwitch
        property string text
        property string subtitle
        property bool checked
        signal toggled(bool on)
        Layout.fillWidth: true
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            PlasmaComponents3.Label {
                Layout.fillWidth: true
                text: settingSwitch.text
                wrapMode: Text.WordWrap
            }
            PlasmaComponents3.Label {
                Layout.fillWidth: true
                visible: settingSwitch.subtitle.length > 0
                text: settingSwitch.subtitle
                font: Kirigami.Theme.smallFont
                opacity: 0.7
                wrapMode: Text.WordWrap
            }
        }
        PlasmaComponents3.Switch {
            id: toggle
            checked: settingSwitch.checked
            onToggled: {
                settingSwitch.toggled(checked)
                checked = Qt.binding(() => settingSwitch.checked)
            }
        }
    }

    component SettingChoice: ColumnLayout {
        id: settingChoice
        property string text
        property var options: []
        property var value
        signal chosen(var value)
        Layout.fillWidth: true
        spacing: Kirigami.Units.smallSpacing
        PlasmaComponents3.Label {
            Layout.fillWidth: true
            text: settingChoice.text
        }
        PlasmaComponents3.ComboBox {
            Layout.fillWidth: true
            model: settingChoice.options
            textRole: "text"
            valueRole: "value"
            currentIndex: settingChoice.options.findIndex(o => o.value === settingChoice.value)
            onActivated: index => settingChoice.chosen(settingChoice.options[index].value)
        }
    }

    compactRepresentation: MouseArea {
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
        hoverEnabled: true
        property bool wasExpanded: false
        onPressed: wasExpanded = root.expanded
        onClicked: mouse => {
            if (mouse.button === Qt.MiddleButton) {
                if (root.available && root.modes.length > 1)
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
            visible: root.mode === "noise-cancelling" || root.mode === "ambient" || root.mode === "adaptive"
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            width: badge.implicitWidth + Kirigami.Units.smallSpacing
            height: badge.implicitHeight
            radius: height / 2
            color: Kirigami.Theme.highlightColor
            PlasmaComponents3.Label {
                id: badge
                anchors.centerIn: parent
                text: root.mode === "noise-cancelling" ? "NC" : root.mode === "adaptive" ? "AD" : "A"
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
        Layout.minimumHeight: root.available ? contentFitHeight : placeholder.implicitHeight + Kirigami.Units.gridUnit * 2
        Layout.preferredHeight: Layout.minimumHeight
        Layout.maximumHeight: Layout.minimumHeight

        header: PlasmaExtras.PlasmoidHeading {
            id: heading
            visible: root.available
            ColumnLayout {
                anchors.fill: parent
                spacing: Kirigami.Units.smallSpacing
                RowLayout {
                    Layout.fillWidth: true
                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 3
                        text: root.deviceName
                        elide: Text.ElideRight
                    }
                    PlasmaComponents3.Label {
                        visible: !!root.cfg.codec
                        text: root.cfg.codec || ""
                        opacity: 0.7
                    }
                    Repeater {
                        model: root.hasMainBattery || root.batteries.length === 0
                            ? (root.battery >= 0 ? [{ part: "main", level: root.battery }] : [])
                            : root.batteries
                        delegate: RowLayout {
                            required property var modelData
                            spacing: Kirigami.Units.smallSpacing
                            PlasmaComponents3.Label {
                                visible: modelData.part !== "main"
                                text: root.batteryPartLabel(modelData.part)
                                opacity: 0.7
                            }
                            Kirigami.Icon {
                                source: root.batteryIcon(modelData.level)
                                implicitWidth: Kirigami.Units.iconSizes.small
                                implicitHeight: Kirigami.Units.iconSizes.small
                            }
                            PlasmaComponents3.Label {
                                text: i18n("%1%", modelData.level)
                            }
                        }
                    }
                }
                PlasmaComponents3.TabBar {
                    id: tabs
                    Layout.fillWidth: true
                    currentIndex: root.currentTab
                    onCurrentIndexChanged: root.currentTab = currentIndex
                    PlasmaComponents3.TabButton {
                        text: i18n("Main")
                    }
                    PlasmaComponents3.TabButton {
                        text: i18n("Sound")
                    }
                    PlasmaComponents3.TabButton {
                        text: i18n("Settings")
                    }
                }
            }
        }

        PlasmaExtras.PlaceholderMessage {
            id: placeholder
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: !root.available
            iconName: "audio-headphones"
            text: root.daemonUp ? i18n("Headphones not connected") : i18n("Service not running")
            explanation: root.daemonUp
                ? i18n("Connect the headphones over Bluetooth. If they are also connected to a phone, close the headphones app there (Sound Connect, Galaxy Wearable).")
                : i18n("Start it with:\nsystemctl --user start headphonesd")
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

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: root.cfg.untested === true
                type: Kirigami.MessageType.Information
                text: i18n("Support for these headphones has not been tested on real hardware yet.")
                actions: [
                    Kirigami.Action {
                        text: i18n("Report how it works")
                        icon.name: "tools-report-bug"
                        onTriggered: Qt.openUrlExternally("https://github.com/RodiAda/kde-headphones/issues")
                    }
                ]
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: root.alert.length > 0
                type: Kirigami.MessageType.Warning
                text: i18n("The headphones ask to confirm this change. They may disconnect for a moment.")
                actions: [
                    Kirigami.Action {
                        text: i18n("Confirm")
                        icon.name: "dialog-ok"
                        onTriggered: root.call("RespondToAlert", [new DBus.bool(true)])
                    },
                    Kirigami.Action {
                        text: i18n("Cancel")
                        icon.name: "dialog-cancel"
                        onTriggered: root.call("RespondToAlert", [new DBus.bool(false)])
                    }
                ]
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.currentTab === 0
                spacing: Kirigami.Units.largeSpacing

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.modes.length > 0
                    spacing: Kirigami.Units.smallSpacing
                    Repeater {
                        model: [
                            { mode: "noise-cancelling", icon: "audio-volume-muted" },
                            { mode: "ambient", icon: "audio-input-microphone" },
                            { mode: "adaptive", icon: "audio-headphones" },
                            { mode: "off", icon: "system-shutdown" }
                        ].filter(m => root.modes.includes(m.mode))
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

                Kirigami.InlineMessage {
                    Layout.fillWidth: true
                    visible: root.cfg.oneEarbudNoise === false && !!root.cfg.wearing
                             && !(root.cfg.wearing.left && root.cfg.wearing.right)
                    type: Kirigami.MessageType.Information
                    text: i18n("Noise control works only with both earbuds in your ears.")
                    actions: [
                        Kirigami.Action {
                            text: i18n("Allow with one earbud")
                            onTriggered: root.apply({ oneEarbudNoise: true })
                        }
                    ]
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.ambientRange !== null && root.modes.includes("ambient")
                    enabled: root.mode === "ambient"
                    opacity: enabled ? 1 : 0.5
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
                        from: root.ambientRange ? root.ambientRange.min : 1
                        to: root.ambientRange ? root.ambientRange.max : 20
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
                        visible: !!root.ambientRange && root.ambientRange.focusOnVoice === true
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
                    visible: root.devices.length > 0
                }

                Kirigami.Heading {
                    level: 4
                    visible: root.devices.length > 0
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

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.currentTab === 1
                spacing: Kirigami.Units.largeSpacing

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: !!root.cfg.playback
                    spacing: Kirigami.Units.smallSpacing

                    PlasmaComponents3.Label {
                        Layout.fillWidth: true
                        readonly property var pb: root.cfg.playback || {}
                        visible: !!pb.title
                        text: pb.artist ? i18n("%1 — %2", pb.artist, pb.title) : (pb.title || "")
                        elide: Text.ElideRight
                        font.bold: true
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        readonly property var pb: root.cfg.playback || {}
                        PlasmaComponents3.ToolButton {
                            visible: !!parent.pb.control
                            icon.name: "media-skip-backward"
                            onClicked: root.call("Playback", [new DBus.string("previous")])
                        }
                        PlasmaComponents3.ToolButton {
                            visible: !!parent.pb.control
                            icon.name: parent.pb.status === "playing" ? "media-playback-pause" : "media-playback-start"
                            onClicked: root.call("Playback", [new DBus.string(parent.pb.status === "playing" ? "pause" : "play")])
                        }
                        PlasmaComponents3.ToolButton {
                            visible: !!parent.pb.control
                            icon.name: "media-skip-forward"
                            onClicked: root.call("Playback", [new DBus.string("next")])
                        }
                        Kirigami.Icon {
                            visible: parent.pb.volume !== undefined && parent.pb.volume !== null
                            source: "audio-volume-medium"
                            implicitWidth: Kirigami.Units.iconSizes.small
                            implicitHeight: Kirigami.Units.iconSizes.small
                        }
                        PlasmaComponents3.Slider {
                            id: volumeSlider
                            Layout.fillWidth: true
                            visible: parent.pb.volume !== undefined && parent.pb.volume !== null
                            from: 0
                            to: 30
                            stepSize: 1
                            value: parent.pb.volume || 0
                            onMoved: sendVolume.restart()
                            Timer {
                                id: sendVolume
                                interval: 150
                                onTriggered: {
                                    root.apply({ volume: volumeSlider.value })
                                    volumeSlider.value = Qt.binding(() => (root.cfg.playback || {}).volume || 0)
                                }
                            }
                        }
                    }
                    Kirigami.Separator {
                        Layout.fillWidth: true
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: !!root.cfg.equalizer
                    spacing: Kirigami.Units.smallSpacing
                    readonly property var eq: root.cfg.equalizer || { presets: [], bands: [], preset: 0, clearBass: 0 }

                    Kirigami.Heading {
                        level: 4
                        text: i18n("Equalizer")
                    }
                    PlasmaComponents3.ComboBox {
                        Layout.fillWidth: true
                        model: parent.eq.presets.map(p => ({ text: root.presetLabel(p.key), value: p.id }))
                        textRole: "text"
                        valueRole: "value"
                        currentIndex: parent.eq.presets.findIndex(p => p.id === parent.eq.preset)
                        onActivated: index => root.apply({ equalizer: { preset: parent.eq.presets[index].id } })
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 7
                        visible: root.eqPresetKey !== "off" && (parent.eq.bands || []).length > 0
                        Repeater {
                            model: (parent.parent.eq.bands || []).length
                            delegate: ColumnLayout {
                                id: bandColumn
                                required property int index
                                readonly property int bandValue: (root.cfg.equalizer || { bands: [] }).bands[index] || 0
                                Layout.fillWidth: true
                                Layout.preferredWidth: 1
                                Layout.maximumWidth: Infinity
                                Layout.fillHeight: true
                                spacing: Kirigami.Units.smallSpacing
                                PlasmaComponents3.Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: bandSlider.value > 0 ? "+" + bandSlider.value : bandSlider.value
                                    font: Kirigami.Theme.smallFont
                                }
                                PlasmaComponents3.Slider {
                                    id: bandSlider
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.fillHeight: true
                                    orientation: Qt.Vertical
                                    from: -10
                                    to: 10
                                    stepSize: 1
                                    snapMode: PlasmaComponents3.Slider.SnapAlways
                                    value: bandColumn.bandValue
                                    onMoved: sendBand.restart()
                                    Timer {
                                        id: sendBand
                                        interval: 200
                                        onTriggered: {
                                            const bands = ((root.cfg.equalizer || { bands: [] }).bands).slice()
                                            bands[bandColumn.index] = bandSlider.value
                                            root.applyEqualizer({ bands: bands })
                                            bandSlider.value = Qt.binding(() => bandColumn.bandValue)
                                        }
                                    }
                                }
                                PlasmaComponents3.Label {
                                    Layout.alignment: Qt.AlignHCenter
                                    text: root.eqBandLabels((root.cfg.equalizer || { bands: [] }).bands.length)[bandColumn.index] || ""
                                    font: Kirigami.Theme.smallFont
                                    opacity: 0.7
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.eqPresetKey !== "off" && parent.eq.clearBass !== undefined
                        PlasmaComponents3.Label {
                            text: i18n("Clear Bass")
                        }
                        PlasmaComponents3.Slider {
                            id: clearBassSlider
                            Layout.fillWidth: true
                            from: -10
                            to: 10
                            stepSize: 1
                            snapMode: PlasmaComponents3.Slider.SnapAlways
                            value: (root.cfg.equalizer || {}).clearBass || 0
                            onMoved: sendClearBass.restart()
                            Timer {
                                id: sendClearBass
                                interval: 200
                                onTriggered: {
                                    root.applyEqualizer({ clearBass: clearBassSlider.value })
                                    clearBassSlider.value = Qt.binding(() => (root.cfg.equalizer || {}).clearBass || 0)
                                }
                            }
                        }
                        PlasmaComponents3.Label {
                            text: clearBassSlider.value > 0 ? "+" + clearBassSlider.value : clearBassSlider.value
                            font.features: { "tnum": 1 }
                        }
                    }
                }

                SettingSwitch {
                    visible: root.cfg.dsee !== undefined
                    text: i18n("DSEE Extreme")
                    subtitle: i18n("Restores detail lost in compressed audio")
                    checked: root.cfg.dsee === true
                    onToggled: on => root.apply({ dsee: on })
                }

                Kirigami.Separator {
                    Layout.fillWidth: true
                    visible: !!root.cfg.speakToChat
                }

                SettingSwitch {
                    visible: !!root.cfg.speakToChat
                    text: i18n("Speak-to-Chat")
                    subtitle: i18n("Pauses playback and lets ambient sound in when you start talking")
                    checked: (root.cfg.speakToChat || {}).enabled === true
                    onToggled: on => root.apply({ speakToChat: { enabled: on } })
                }
                SettingChoice {
                    visible: (root.cfg.speakToChat || {}).enabled === true
                    text: i18n("Voice detection sensitivity")
                    options: [
                        { text: i18n("Auto"), value: "auto" },
                        { text: i18n("High"), value: "high" },
                        { text: i18n("Low"), value: "low" }
                    ]
                    value: (root.cfg.speakToChat || {}).sensitivity
                    onChosen: v => root.apply({ speakToChat: { sensitivity: v } })
                }
                SettingChoice {
                    visible: (root.cfg.speakToChat || {}).enabled === true
                    text: i18n("End Speak-to-Chat")
                    options: [
                        { text: i18n("Short (about 15 s)"), value: "short" },
                        { text: i18n("Standard (about 30 s)"), value: "medium" },
                        { text: i18n("Long (about 1 min)"), value: "long" },
                        { text: i18n("Don't end automatically"), value: "manual" }
                    ]
                    value: (root.cfg.speakToChat || {}).timeout
                    onChosen: v => root.apply({ speakToChat: { timeout: v } })
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: root.currentTab === 2
                spacing: Kirigami.Units.largeSpacing

                Repeater {
                    model: root.cfg.toggles || []
                    delegate: SettingSwitch {
                        required property var modelData
                        text: root.controlLabel(modelData.key)
                        checked: modelData.value === true
                        onToggled: on => {
                            const change = {}
                            change[modelData.key] = on
                            root.apply({ toggles: change })
                        }
                    }
                }

                Repeater {
                    model: root.cfg.choices || []
                    delegate: SettingChoice {
                        required property var modelData
                        text: root.controlLabel(modelData.key)
                        options: (modelData.options || []).map(o => ({ text: root.optionLabel(o), value: o }))
                        value: modelData.value
                        onChosen: v => {
                            const change = {}
                            change[modelData.key] = v
                            root.apply({ choices: change })
                        }
                    }
                }

                Repeater {
                    model: root.cfg.sliders || []
                    delegate: ColumnLayout {
                        id: sliderSetting
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        RowLayout {
                            Layout.fillWidth: true
                            PlasmaComponents3.Label {
                                Layout.fillWidth: true
                                text: root.controlLabel(sliderSetting.modelData.key)
                            }
                            PlasmaComponents3.Label {
                                text: genericSlider.value
                                font.features: { "tnum": 1 }
                            }
                        }
                        PlasmaComponents3.Slider {
                            id: genericSlider
                            Layout.fillWidth: true
                            from: sliderSetting.modelData.min
                            to: sliderSetting.modelData.max
                            stepSize: 1
                            snapMode: PlasmaComponents3.Slider.SnapAlways
                            value: sliderSetting.modelData.value
                            onMoved: sendGeneric.restart()
                            Timer {
                                id: sendGeneric
                                interval: 200
                                onTriggered: {
                                    const change = {}
                                    change[sliderSetting.modelData.key] = genericSlider.value
                                    root.apply({ sliders: change })
                                    genericSlider.value = Qt.binding(() => sliderSetting.modelData.value)
                                }
                            }
                        }
                    }
                }

                SettingSwitch {
                    visible: root.cfg.touchLock !== undefined
                    text: i18n("Lock touch controls")
                    subtitle: i18n("Ignore taps on the earbuds")
                    checked: root.cfg.touchLock === true
                    onToggled: on => root.apply({ touchLock: on })
                }
                SettingSwitch {
                    visible: root.cfg.oneEarbudNoise !== undefined
                    text: i18n("Noise control with one earbud")
                    subtitle: i18n("Allow noise cancelling and ambient sound when only one earbud is worn")
                    checked: root.cfg.oneEarbudNoise === true
                    onToggled: on => root.apply({ oneEarbudNoise: on })
                }
                SettingSwitch {
                    visible: root.cfg.seamless !== undefined
                    text: i18n("Seamless earbud connection")
                    subtitle: i18n("Switch automatically between devices signed in to the same Samsung account")
                    checked: root.cfg.seamless === true
                    onToggled: on => root.apply({ seamless: on })
                }

                SettingChoice {
                    visible: !!root.cfg.noiseButton
                    text: i18n("NC/AMB button")
                    options: [
                        { text: i18n("Noise cancelling → Ambient sound → Off"), value: "nc-ambient-off" },
                        { text: i18n("Noise cancelling ↔ Ambient sound"), value: "nc-ambient" },
                        { text: i18n("Noise cancelling ↔ Off"), value: "nc-off" },
                        { text: i18n("Ambient sound ↔ Off"), value: "ambient-off" }
                    ]
                    value: root.cfg.noiseButton
                    onChosen: v => root.apply({ noiseButton: v })
                }

                Repeater {
                    model: (root.cfg.general || []).filter(g => g.writable)
                    delegate: SettingSwitch {
                        required property var modelData
                        text: root.generalLabel(modelData.subject)
                        checked: modelData.value === true
                        onToggled: on => {
                            const change = {}
                            change[modelData.index] = on
                            root.apply({ general: change })
                        }
                    }
                }

                SettingSwitch {
                    visible: root.cfg.autoSwitch !== undefined
                    text: i18n("Switch audio between devices automatically")
                    subtitle: i18n("When playback starts on the other connected device")
                    checked: root.cfg.autoSwitch === true
                    onToggled: on => root.apply({ autoSwitch: on })
                }

                SettingChoice {
                    visible: !!root.cfg.connectionMode
                    text: i18n("Bluetooth connection quality")
                    options: [
                        { text: i18n("Priority on sound quality"), value: "quality" },
                        { text: i18n("Priority on stable connection"), value: "stability" }
                    ]
                    value: root.cfg.connectionMode
                    onChosen: v => root.apply({ connectionMode: v })
                }

                Kirigami.Separator {
                    Layout.fillWidth: true
                    visible: !!root.cfg.power || !!root.cfg.voiceGuidance
                }

                SettingSwitch {
                    visible: (root.cfg.power || {}).autoPause !== undefined
                    text: i18n("Pause when headphones are taken off")
                    checked: (root.cfg.power || {}).autoPause === true
                    onToggled: on => root.apply({ power: { autoPause: on } })
                }
                SettingSwitch {
                    visible: (root.cfg.power || {}).offWhenRemoved !== undefined
                    text: i18n("Turn off when headphones are taken off")
                    checked: (root.cfg.power || {}).offWhenRemoved === true
                    onToggled: on => root.apply({ power: { offWhenRemoved: on } })
                }
                SettingChoice {
                    visible: (root.cfg.power || {}).autoPowerOff !== undefined
                    text: i18n("Automatic power off when idle")
                    options: [
                        { text: i18n("Don't turn off"), value: 0 },
                        { text: i18n("After 5 minutes"), value: 5 },
                        { text: i18n("After 15 minutes"), value: 15 },
                        { text: i18n("After 30 minutes"), value: 30 },
                        { text: i18n("After 1 hour"), value: 60 },
                        { text: i18n("After 3 hours"), value: 180 }
                    ]
                    value: (root.cfg.power || {}).autoPowerOff
                    onChosen: v => root.apply({ power: { autoPowerOff: v } })
                }
                SettingSwitch {
                    visible: !!root.cfg.voiceGuidance
                    text: i18n("Voice guidance")
                    subtitle: i18n("Spoken notifications such as battery level and mode changes")
                    checked: (root.cfg.voiceGuidance || {}).enabled === true
                    onToggled: on => root.apply({ voiceGuidance: { enabled: on } })
                }

                Kirigami.Separator {
                    Layout.fillWidth: true
                    visible: root.cfg.pairing !== undefined || root.cfg.canPowerOff === true
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.cfg.pairing !== undefined
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        PlasmaComponents3.Label {
                            Layout.fillWidth: true
                            text: i18n("Pair a new device")
                        }
                        PlasmaComponents3.Label {
                            Layout.fillWidth: true
                            text: root.cfg.pairing === true ? i18n("Pairing mode is on; the headphones are discoverable")
                                                            : i18n("Makes the headphones discoverable over Bluetooth")
                            font: Kirigami.Theme.smallFont
                            opacity: 0.7
                            wrapMode: Text.WordWrap
                        }
                    }
                    PlasmaComponents3.Button {
                        text: root.cfg.pairing === true ? i18n("Stop") : i18n("Start")
                        onClicked: root.apply({ pairing: root.cfg.pairing !== true })
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.cfg.canPowerOff === true
                    PlasmaComponents3.Label {
                        Layout.fillWidth: true
                        text: powerOffButton.armed ? i18n("Click again to turn the headphones off") : i18n("Turn off the headphones")
                        wrapMode: Text.WordWrap
                    }
                    PlasmaComponents3.Button {
                        id: powerOffButton
                        property bool armed: false
                        icon.name: "system-shutdown"
                        text: armed ? i18n("Turn off") : i18n("Turn off…")
                        onClicked: {
                            if (armed) {
                                armed = false
                                root.call("PowerOff")
                            } else {
                                armed = true
                                disarm.restart()
                            }
                        }
                        Timer {
                            id: disarm
                            interval: 4000
                            onTriggered: powerOffButton.armed = false
                        }
                    }
                }

                PlasmaComponents3.Label {
                    Layout.fillWidth: true
                    text: [root.cfg.model, root.cfg.firmware ? i18n("firmware %1", root.cfg.firmware) : "", root.cfg.codec]
                        .filter(s => !!s).join(" · ")
                    font: Kirigami.Theme.smallFont
                    opacity: 0.6
                    wrapMode: Text.WordWrap
                }
            }
        }
        }
    }
}
