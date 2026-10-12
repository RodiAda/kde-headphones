#!/bin/bash
set -uo pipefail
cd "$(dirname "$0")"

systemctl --user disable --now headphonesd.service
if [ -f build-user/install_manifest.txt ]; then
    xargs rm -fv < build-user/install_manifest.txt
else
    rm -fv ~/.local/bin/headphonesd \
        ~/.local/share/systemd/user/headphonesd.service \
        ~/.local/share/dbus-1/services/io.github.rodiada.Headphones.service
    rm -rfv ~/.local/share/plasma/plasmoids/io.github.rodiada.headphones
fi
systemctl --user daemon-reload
