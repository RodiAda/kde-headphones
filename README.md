# Sony Headphones for KDE Plasma

A Plasma 6 system tray widget and a small user daemon for controlling Sony headphones
(tested with the WH-1000XM5).

- Noise cancelling / ambient sound / off, ambient level, focus on voice
- Battery level
- Paired devices: connection order, connect / disconnect, switch the playback source

```
sony-headphonesd  ── RFCOMM ──  headphones
      │ session D-Bus: org.rodada.SonyHeadphones
plasmoid org.rodada.sonyheadphones (pure QML)
```

The protocol is handled by [libmdr](https://github.com/mos9527/SonyHeadphonesClient/tree/main/libmdr)
from SonyHeadphonesClient, pulled in as a git submodule.

## Install

Requirements: CMake 3.31+, a C++20 compiler, Qt 6 (Core, DBus), BlueZ and D-Bus development files,
gettext, Plasma 6.

```
git clone --recurse-submodules <this repo>
./install.sh
```

The script builds the daemon, installs it to `~/.local/bin`, enables the
`sony-headphonesd.service` user unit, compiles the translations and installs the widget.
Add the widget via *Configure System Tray → Entries → Sony Headphones*. If it is not listed,
restart plasmashell: `systemctl --user restart plasma-plasmashell`.

Middle-click the tray icon to cycle the noise mode.

## Translations

The widget follows the system language. Strings are written in English and translated through
gettext catalogs in `po/` (domain `plasma_applet_org.rodada.sonyheadphones`); `install.sh` compiles
them into the widget package. Currently available: Russian.

To update the template after changing strings and merge it into a catalog:

```
xgettext --from-code=UTF-8 -C -kde -ki18n:1 -ki18nc:1c,2 -o po/plasma_applet_org.rodada.sonyheadphones.pot \
    plasmoid/org.rodada.sonyheadphones/contents/ui/*.qml
msgmerge -U po/ru.po po/plasma_applet_org.rodada.sonyheadphones.pot
```

A new language is a new `po/<lang>.po`.

## D-Bus

```
busctl --user introspect org.rodada.SonyHeadphones /org/rodada/SonyHeadphones
busctl --user call org.rodada.SonyHeadphones /org/rodada/SonyHeadphones org.rodada.SonyHeadphones CycleMode
```

Properties: `Connected`, `DeviceName`, `Address`, `Battery` (-1 when unknown), `Mode`
(`noise-cancelling` | `ambient` | `off`), `AmbientLevel` (1..20), `FocusOnVoice`,
`Devices` (JSON array of `mac`, `name`, `connected`, `order`, `active`, `self`),
`DeviceAction` (JSON describing the last connect/disconnect: `pending` → `in-progress` → `ok` | `error` | `busy`).

Methods: `SetMode(s)`, `SetAmbientLevel(i)`, `SetFocusOnVoice(b)`, `CycleMode()`,
`SwitchPlayback(s mac)`, `ConnectDevice(s mac)`, `DisconnectDevice(s mac)`.

`CycleMode` works well as a global shortcut (*System Settings → Shortcuts → Add Command*).

## Notes

- The daemon picks the first connected BlueZ device that exposes the Sony control service
  `956C7B26-D49A-4BA8-B03F-B17D393CB6E2` and retries every 5 seconds.
- The headphones accept a single control connection. If a phone holds it (Sound Connect open),
  the daemon cannot connect.
- The daemon refuses to disconnect the computer it runs on, since that would drop the control link.
- libmdr does not expose the connection order or the result of connect/disconnect; the daemon reads
  both from raw packets (`PERI_RET/NTFY_PARAM` type 2 and `PERI_NTFY_EXTENDED_PARAM`).
- Connecting a device reports `in-progress` first and `ok` or `error` later; `error` means the
  device itself refused.
- The last ambient level is kept in `~/.config/rodada/sony-headphonesd.conf`, because the headphones
  do not report it while noise cancelling is on.
- `DBus.Properties` in plasmashell does not expose all properties before the first
  `PropertiesChanged`, so the widget reads the initial state with its own `GetAll`.
- After reinstalling the widget, restart plasmashell; `kpackagetool6 -u` only updates the files.
- Logs: `journalctl --user -u sony-headphonesd`.

## License

MIT, see [LICENSE](LICENSE). libmdr is MIT licensed by its authors.
