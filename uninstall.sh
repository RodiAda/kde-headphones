#!/bin/bash
set -uo pipefail

systemctl --user disable --now headphonesd.service
rm -f ~/.config/systemd/user/headphonesd.service ~/.local/bin/headphonesd
systemctl --user daemon-reload
kpackagetool6 -t Plasma/Applet -r io.github.rodiada.headphones
