#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build

if systemctl --user list-unit-files sony-headphonesd.service >/dev/null 2>&1; then
    systemctl --user disable --now sony-headphonesd.service 2>/dev/null || true
    rm -f ~/.config/systemd/user/sony-headphonesd.service ~/.local/bin/sony-headphonesd
fi

install -Dm755 build/headphonesd ~/.local/bin/headphonesd
install -Dm644 systemd/headphonesd.service ~/.config/systemd/user/headphonesd.service
systemctl --user daemon-reload
systemctl --user enable headphonesd.service
systemctl --user restart headphonesd.service

applet=plasmoid/org.rodada.headphones
domain=plasma_applet_org.rodada.headphones
for po in po/*.po; do
    lang=$(basename "$po" .po)
    mkdir -p "$applet/contents/locale/$lang/LC_MESSAGES"
    msgfmt -o "$applet/contents/locale/$lang/LC_MESSAGES/$domain.mo" "$po"
done
if kpackagetool6 -t Plasma/Applet -s org.rodada.headphones >/dev/null 2>&1; then
    kpackagetool6 -t Plasma/Applet -u "$applet"
else
    kpackagetool6 -t Plasma/Applet -i "$applet"
fi
