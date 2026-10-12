#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

applet_id=io.github.rodiada.headphones
applet=plasmoid/$applet_id
domain=plasma_applet_$applet_id

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

install -Dm755 build/headphonesd ~/.local/bin/headphonesd
install -Dm644 systemd/headphonesd.service ~/.config/systemd/user/headphonesd.service
systemctl --user daemon-reload
systemctl --user enable headphonesd.service
systemctl --user restart headphonesd.service

for po in po/*.po; do
    lang=$(basename "$po" .po)
    mkdir -p "$applet/contents/locale/$lang/LC_MESSAGES"
    msgfmt -o "$applet/contents/locale/$lang/LC_MESSAGES/$domain.mo" "$po"
done
if kpackagetool6 -t Plasma/Applet -s "$applet_id" >/dev/null 2>&1; then
    kpackagetool6 -t Plasma/Applet -u "$applet"
else
    kpackagetool6 -t Plasma/Applet -i "$applet"
fi
