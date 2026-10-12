# Headphones for KDE Plasma

A Plasma 6 system tray widget and a small user daemon for controlling wireless headphones:
Sony (tested with the WH-1000XM5) and Samsung Galaxy Buds2.

- Noise cancelling / ambient sound / off, ambient level, focus on voice
- Battery level, codec, firmware
- Paired devices: connection order, connect / disconnect, switch the playback source
- Playback control and volume
- Equalizer presets, 5-band custom equalizer, Clear Bass, DSEE Extreme
- Speak-to-Chat with sensitivity and timeout
- NC/AMB button function, touch panel, multipoint, automatic source switching,
  connection quality priority
- Pause or power off when taken off, automatic power off, voice guidance
- Pairing mode and powering the headphones off

Only the features the headphones support are shown.

Sony: both protocol generations handled by libmdr are supported: MDR V2 (WH-1000XM5/XM6, WF-1000XM5/XM6,
WH-CH720N, WF-C510, WF-C700N, LinkBuds) and the legacy MDR V1 (WH-1000XM3/XM4, WF-1000XM3/XM4),
picked by the Sony control service the device exposes. Only the WH-1000XM5 has been tested with this
widget; see the [libmdr support matrix](https://github.com/mos9527/SonyHeadphonesClient/tree/main/docs/device-support).
Paired device management, the connection order and the connect/disconnect results are available
on MDR V2 only. BLE-only devices are not supported.

Samsung Galaxy Buds2: own implementation of the Galaxy Buds RFCOMM protocol (service
`2E73A4AD-332D-41FC-90E2-16BEF06523F2`): earbud and case battery, noise cancelling / ambient / off,
ambient level, equalizer presets, touch lock and seamless connection. Other Galaxy Buds models
that use the same service may partly work; the status layout is model specific.

```
headphonesd  ── RFCOMM ──  headphones
   │  backends: Sony (libmdr), Samsung Galaxy Buds
   │ session D-Bus: org.rodada.Headphones
plasmoid org.rodada.headphones (pure QML)
```

The Sony protocol is handled by [libmdr](https://github.com/mos9527/SonyHeadphonesClient/tree/main/libmdr)
from SonyHeadphonesClient, pulled in as a git submodule.

## Install

Requirements: CMake 3.31+, a C++20 compiler, Qt 6 (Core, DBus), BlueZ and D-Bus development files,
gettext, Plasma 6.

```
git clone --recurse-submodules <this repo>
./install.sh
```

The script builds the daemon, installs it to `~/.local/bin`, enables the
`headphonesd.service` user unit, compiles the translations and installs the widget.
Add the widget via *Configure System Tray → Entries → Headphones*. If it is not listed,
restart plasmashell: `systemctl --user restart plasma-plasmashell`.

Middle-click the tray icon to cycle the noise mode.

## Translations

The widget follows the system language. Strings are written in English and translated through
gettext catalogs in `po/` (domain `plasma_applet_org.rodada.headphones`); `install.sh` compiles
them into the widget package. Currently available: Russian.

To update the template after changing strings and merge it into a catalog:

```
xgettext --from-code=UTF-8 -C -kde -ki18n:1 -ki18nc:1c,2 -o po/plasma_applet_org.rodada.headphones.pot \
    plasmoid/org.rodada.headphones/contents/ui/*.qml
msgmerge -U po/ru.po po/plasma_applet_org.rodada.headphones.pot
```

A new language is a new `po/<lang>.po`.

## D-Bus

```
busctl --user introspect org.rodada.Headphones /org/rodada/Headphones
busctl --user call org.rodada.Headphones /org/rodada/Headphones org.rodada.Headphones CycleMode
```

Properties: `Connected`, `Vendor` (sony | samsung), `DeviceName`, `Address`, `Battery` (-1 when unknown; the lower earbud on
true wireless models), `Batteries` (JSON array of `part`: main|left|right|case, `level`, `charging`),
`Modes` (JSON array of the noise modes the model supports), `Mode`
(`noise-cancelling` | `ambient` | `off`), `AmbientLevel` (1..20), `FocusOnVoice`,
`Devices` (JSON array of `mac`, `name`, `connected`, `order`, `active`, `self`),
`DeviceAction` (JSON describing the last connect/disconnect: `pending` → `in-progress` → `ok` | `error` | `busy`).

`Settings` (JSON with every supported setting, see below; `ambient` gives the level range), `Alert` (non-empty while the headphones
wait for a confirmation).

Methods: `SetMode(s)`, `SetAmbientLevel(i)`, `SetFocusOnVoice(b)`, `CycleMode()`,
`SwitchPlayback(s mac)`, `ConnectDevice(s mac)`, `DisconnectDevice(s mac)`,
`Apply(s json)`, `Playback(s play|pause|next|previous)`, `PowerOff()`, `RespondToAlert(b)`.

`Apply` takes a partial object with the same shape as `Settings`:

```
busctl --user call org.rodada.Headphones /org/rodada/Headphones org.rodada.Headphones \
    Apply s '{"speakToChat":{"enabled":true,"timeout":"long"},"dsee":true}'
```

Keys: `speakToChat` {`enabled`, `sensitivity`: auto|high|low, `timeout`: short|medium|long|manual},
`equalizer` {`preset`, `bands`, `clearBass`}, `dsee`, `noiseButton` (nc-ambient-off | nc-ambient | nc-off |
ambient-off), `power` {`autoPowerOff`: 0|5|15|30|60|180, `offWhenRemoved`, `autoPause`},
`voiceGuidance` {`enabled`, `volume`}, `connectionMode` (quality | stability), `autoSwitch`, `pairing`,
`volume` (0..30), `general` {"<index>": bool}. Galaxy Buds: `equalizer` {`preset`}, `touchLock`, `seamless`.

`CycleMode` works well as a global shortcut (*System Settings → Shortcuts → Add Command*).

## Notes

- The daemon picks the first connected BlueZ device that exposes a known control service
  and retries every 5 seconds.
- The headphones accept a single control connection. If a phone holds it (Sound Connect open),
  the daemon cannot connect.
- The daemon refuses to disconnect the computer it runs on, since that would drop the control link.
- libmdr does not expose the connection order or the result of connect/disconnect; the daemon reads
  both from raw packets (`PERI_RET/NTFY_PARAM` type 2 and `PERI_NTFY_EXTENDED_PARAM`).
- Connecting a device reports `in-progress` first and `ok` or `error` later; `error` means the
  device itself refused.
- The last Sony ambient level is kept in `~/.config/rodada/headphonesd.conf`, because the headphones
  do not report it while noise cancelling is on.
- `DBus.Properties` in plasmashell does not expose all properties before the first
  `PropertiesChanged`, so the widget reads the initial state with its own `GetAll`.
- After reinstalling the widget, restart plasmashell; `kpackagetool6 -u` only updates the files.
- Logs: `journalctl --user -u headphonesd`.

## Tests

```
cmake -B build-test -DHEADPHONES_TESTS=ON && cmake --build build-test --target galaxy-test && build-test/galaxy-test
```

The Galaxy backend test replays an extended status frame captured from a Galaxy Buds2.

## License

MIT, see [LICENSE](LICENSE). libmdr is MIT licensed by its authors.
