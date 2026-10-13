#!/bin/sh
# Installs the prebuilt AArchX from GitHub releases into ~/.aarchx and puts
# ocerz on PATH.  Nothing outside ~/.aarchx is touched except one line in the
# shell profile (~/.zprofile or ~/.bash_profile), and no password is asked for.
#
#   curl -fsSL https://mont127.github.io/AArchX/install.sh | sh
#   curl -fsSL https://mont127.github.io/AArchX/install.sh | sh -s -- --uninstall
#
# AARCHX_HOME           where to install (default ~/.aarchx)
# AARCHX_VERSION        a release tag such as v0.7 (default: the latest release)
# AARCHX_NO_MODIFY_PATH set to 1 to leave the shell profile alone
# AARCHX_URL            the archive to install instead of the release's
#
# The archive is fetched with curl, so macOS does not quarantine it and
# Gatekeeper does not stop the unsigned ocerz binary.

set -eu

REPO=mont127/AArchX
ASSET=aarchx-macos-arm64.tar.gz
MARK='# added by the AArchX installer'

say() { printf '%s\n' "$*"; }
die() { printf 'aarchx install: %s\n' "$*" >&2; exit 1; }

profile_file() {
    case "${SHELL:-/bin/zsh}" in
        */bash) printf '%s\n' "$HOME/.bash_profile" ;;
        */zsh) printf '%s\n' "$HOME/.zprofile" ;;
        *) printf '\n' ;;
    esac
}

uninstall() {
    home=${AARCHX_HOME:-$HOME/.aarchx}
    if [ -x "$home/ocerz" ]; then
        rm -rf "$home"
        say "removed $home"
    else
        say "no AArchX install at $home"
    fi
    for f in "$HOME/.zprofile" "$HOME/.bash_profile"; do
        if [ -f "$f" ] && grep -qF "$MARK" "$f"; then
            sed -i '' "/$MARK/d" "$f"
            say "removed the PATH line from $f"
        fi
    done
    say "the translation cache in ~/Library/Caches/ocerz is left; delete it too if you want the space back"
}

install() {
    [ "$(uname -s)" = Darwin ] || die "AArchX runs on macOS only"
    [ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = 1 ] ||
        die "AArchX needs an Apple silicon Mac"
    command -v curl >/dev/null || die "curl is missing"

    home=${AARCHX_HOME:-$HOME/.aarchx}
    case "$home" in
        /*) ;;
        *) die "AARCHX_HOME must be an absolute path" ;;
    esac
    if [ -n "${AARCHX_URL:-}" ]; then
        url=$AARCHX_URL
    elif [ -n "${AARCHX_VERSION:-}" ]; then
        url=https://github.com/$REPO/releases/download/$AARCHX_VERSION/$ASSET
    else
        url=https://github.com/$REPO/releases/latest/download/$ASSET
    fi

    parent=$(dirname "$home")
    mkdir -p "$parent"
    tmp=$(mktemp -d "$parent/.aarchx-install.XXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    trap 'exit 1' INT TERM

    say "downloading $url"
    curl -fL --progress-bar -o "$tmp/$ASSET" "$url" || die "download failed"
    if [ -z "${AARCHX_URL:-}" ]; then
        curl -fsSL -o "$tmp/$ASSET.sha256" "$url.sha256" || die "checksum download failed"
        want=$(awk '{ print $1; exit }' "$tmp/$ASSET.sha256")
        got=$(shasum -a 256 "$tmp/$ASSET" | awk '{ print $1 }')
        [ -n "$want" ] && [ "$want" = "$got" ] || die "checksum mismatch: expected $want, got $got"
    fi

    mkdir "$tmp/x"
    tar -xzf "$tmp/$ASSET" -C "$tmp/x" || die "could not unpack the archive"
    [ -x "$tmp/x/aarchx/ocerz" ] || die "the archive has no aarchx/ocerz"
    xattr -dr com.apple.quarantine "$tmp/x/aarchx" 2>/dev/null || true
    mkdir -p "$tmp/x/aarchx/bin"
    ln -s ../ocerz "$tmp/x/aarchx/bin/ocerz"
    "$tmp/x/aarchx/ocerz" version >/dev/null 2>&1 || die "the downloaded ocerz does not run on this Mac"

    if [ -e "$home" ]; then
        [ -x "$home/ocerz" ] || die "$home exists and is not an AArchX install; set AARCHX_HOME"
        mv "$home" "$tmp/old"
    fi
    mv "$tmp/x/aarchx" "$home"
    say "installed $("$home/ocerz" version) in $home"

    case "$home" in
        "$HOME"/*) dir="\$HOME${home#"$HOME"}/bin" ;;
        *) dir="$home/bin" ;;
    esac
    line="export PATH=\"$dir:\$PATH\"  $MARK"
    profile=$(profile_file)
    case ":$PATH:" in
        *":$home/bin:"*) ;;
        *)
            if [ "${AARCHX_NO_MODIFY_PATH:-0}" = 1 ] || [ -z "$profile" ]; then
                say "add $home/bin to PATH to run ocerz by name"
            elif [ -f "$profile" ] && grep -qF "$MARK" "$profile"; then
                say "PATH is set in $profile; open a new terminal window to use it"
            else
                printf '\n%s\n' "$line" >> "$profile"
                say "added $home/bin to PATH in $profile; open a new terminal window to use it"
            fi
            ;;
    esac

    found=0
    for d in /System/Volumes/Preboot/Cryptexes/Rosetta /System/Volumes/Preboot/Cryptexes/OS; do
        if [ -f "$d/System/Library/dyld/dyld_shared_cache_x86_64" ]; then
            found=1
        fi
    done
    if [ "$found" = 0 ]; then
        say ""
        say "Rosetta is not installed, so only native mode (ocerz -native) will work."
        say "Cache mode reads Apple's x86-64 shared cache, which ships with Rosetta:"
        say "  softwareupdate --install-rosetta --agree-to-license"
    fi

    say ""
    say "Run an Intel program:"
    say "  ocerz /Applications/Some.app"
    say "  ocerz -native ./some_tool"
    say "Uninstall: curl -fsSL https://mont127.github.io/AArchX/install.sh | sh -s -- --uninstall"
}

case "${1:-}" in
    --uninstall) uninstall ;;
    "") install ;;
    *) die "unknown option $1 (the only one is --uninstall)" ;;
esac
