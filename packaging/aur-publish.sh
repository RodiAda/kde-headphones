#!/bin/bash
set -euo pipefail

usage() {
    echo "usage: $0 kde-headphones|kde-headphones-git [commit message]" >&2
    exit 1
}

[ $# -ge 1 ] || usage
name=$1
message=${2:-"Update to $(sed -n 's/^pkgver=//p' "$(dirname "$0")/$( [ "$name" = kde-headphones-git ] && echo arch-git || echo arch )/PKGBUILD")"}
case $name in
    kde-headphones) src="$(dirname "$0")/arch/PKGBUILD" ;;
    kde-headphones-git) src="$(dirname "$0")/arch-git/PKGBUILD" ;;
    *) usage ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git clone "ssh://aur@aur.archlinux.org/$name.git" "$work/$name"
cp "$src" "$work/$name/PKGBUILD"
cd "$work/$name"
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO
if git diff --cached --quiet; then
    echo "Nothing to publish"
    exit 0
fi
git commit -m "$message"
git push origin HEAD:master
