#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"

cmake -B build-user -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-user
cmake --install build-user

systemctl --user daemon-reload
systemctl --user enable headphonesd.service
systemctl --user restart headphonesd.service

echo "Installed. Add the widget via Configure System Tray -> Entries -> Headphones."
