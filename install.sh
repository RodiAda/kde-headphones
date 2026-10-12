#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

install -Dm755 build/sony-headphonesd ~/.local/bin/sony-headphonesd
install -Dm644 systemd/sony-headphonesd.service ~/.config/systemd/user/sony-headphonesd.service
systemctl --user daemon-reload
systemctl --user enable sony-headphonesd.service
systemctl --user restart sony-headphonesd.service

applet=plasmoid/org.rodada.sonyheadphones
domain=plasma_applet_org.rodada.sonyheadphones
for po in po/*.po; do
    lang=$(basename "$po" .po)
    mkdir -p "$applet/contents/locale/$lang/LC_MESSAGES"
    msgfmt -o "$applet/contents/locale/$lang/LC_MESSAGES/$domain.mo" "$po"
done
if kpackagetool6 -t Plasma/Applet -s org.rodada.sonyheadphones >/dev/null 2>&1; then
    kpackagetool6 -t Plasma/Applet -u "$applet"
else
    kpackagetool6 -t Plasma/Applet -i "$applet"
fi
