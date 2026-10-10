#!/bin/sh
# install.sh: install rmp, the one command of raylib_multiplatform.
#
#   curl -fsSL https://omardev29.github.io/rmp-docs/install.sh | sh    Linux, macOS
#   wget -qO- https://omardev29.github.io/rmp-docs/install.sh | sh     Linux without curl
#   fetch -o - https://omardev29.github.io/rmp-docs/install.sh | sh    FreeBSD
#   ftp -o - https://omardev29.github.io/rmp-docs/install.sh | sh      OpenBSD, NetBSD
#
# None of the three BSDs has curl in its base system: fetch and ftp are what
# each of them has. Windows is install.ps1, from PowerShell:
#
#   irm https://omardev29.github.io/rmp-docs/install.ps1 | iex
#
# What it does: clones the framework into ~/.local/share/rmp (with git,
# without the file contents of old commits) and runs `rmp install`, which links
# ~/.local/bin/rmp to it and adds ~/.local/bin to PATH in your shell's startup
# file only if it is not on PATH already. Run again, it updates the clone with
# `rmp update` instead. It needs git and a Python 3.11+ and stops, saying how
# to get them, without either. It never uses sudo and never installs a
# package: what a build still needs -- CMake, Ninja, a C++ compiler, the X11
# headers on Linux -- is listed at the end with the command for this system.
#
# RMP_INSTALL_SOURCE and RMP_INSTALL_BRANCH clone from somewhere else: the
# tests install from a local clone.
#
# Written for `curl ... | sh`, where this file IS the shell's stdin. All of it
# is one function, called on the last line, so a download cut short runs
# nothing; and every command it starts reads /dev/null, so none of them can
# eat what is left of the script (a game did exactly that to the BSD jobs:
# CLAUDE.md, "A game started inside the BSD VM's script"). POSIX sh, ASCII,
# LF: it runs under dash, bash, zsh, the BSDs' sh and macOS's bash 3.2.

# SC2217 says uname and rmdir do not read stdin. Every command gets /dev/null
# all the same: one rule with no exceptions to remember, and what a command
# does with stdin can change under it (InstallTest runs each one through a
# stand-in that reads all of its stdin).
# shellcheck disable=SC2217

main() {
    url=https://github.com/omardev29/raylib_multiplatform
    source=${RMP_INSTALL_SOURCE:-$url}
    branch=${RMP_INSTALL_BRANCH:-main}

    say() { printf '%s\n' "$*"; }
    fail() { printf 'FAIL: %s\n' "$*"; }
    have() { command -v "$1" > /dev/null 2>&1; }

    system=$(uname -s < /dev/null 2> /dev/null) || system=unknown
    case $system in
        MINGW* | MSYS* | CYGWIN* | Windows_NT)
            fail "this is Windows. Install rmp from PowerShell instead:"
            say "  irm https://omardev29.github.io/rmp-docs/install.ps1 | iex"
            return 1 ;;
    esac
    if [ -z "${HOME:-}" ]; then
        fail "HOME is not set, so there is no home folder to install rmp into."
        return 1
    fi
    dest=$HOME/.local/share/rmp

    # What to tell somebody to type. The package manager is the system's, and
    # the command is run by them, as root, never by this script.
    manager=
    case $system in
        Darwin) manager=brew ;;
        FreeBSD) manager=pkg ;;
        OpenBSD) manager=pkg_add ;;
        NetBSD) manager=pkgin ;;
        *)
            for m in apt-get dnf pacman zypper; do
                if have "$m"; then
                    manager=$m
                    break
                fi
            done ;;
    esac
    root=
    if have sudo; then
        root='sudo '
    elif have doas; then
        root='doas '
    fi
    # One package name per need; the X11 lists are the README's (apt) and the
    # FreeBSD job's (.github/workflows/_bsd.yml). OpenBSD and NetBSD ship X11
    # and a compiler in their base sets.
    package() {
        case $manager:$1 in
            apt-get:python) say python3 ;;
            apt-get:python-new) say python3.11 ;;
            apt-get:ninja) say ninja-build ;;
            apt-get:cxx) say g++ ;;
            apt-get:x11) say libx11-dev libxrandr-dev libxi-dev libxcursor-dev \
                libxinerama-dev libgl1-mesa-dev ;;
            dnf:python) say python3 ;;
            dnf:python-new) say python3.11 ;;
            dnf:ninja) say ninja-build ;;
            dnf:cxx) say gcc-c++ ;;
            dnf:x11) say libX11-devel libXrandr-devel libXi-devel libXcursor-devel \
                libXinerama-devel mesa-libGL-devel ;;
            pacman:python | pacman:python-new) say python ;;
            pacman:cxx) say gcc ;;
            pacman:x11) say libx11 libxrandr libxi libxcursor libxinerama mesa ;;
            zypper:python | zypper:python-new) say python311 ;;
            zypper:cxx) say gcc-c++ ;;
            zypper:x11) say libX11-devel libXrandr-devel libXi-devel libXcursor-devel \
                libXinerama-devel Mesa-libGL-devel ;;
            brew:python | brew:python-new) say python ;;
            pkg:python | pkg:python-new) say python311 ;;
            pkg:x11) say libX11 libXrandr libXi libXcursor libXinerama libglvnd mesa-libs ;;
            pkg_add:python | pkg_add:python-new) say python%3.12 ;;
            pkgin:python | pkgin:python-new) say python312 ;;
            pkgin:ninja) say ninja-build ;;
            *:git | *:cmake | *:ninja) say "$1" ;;
        esac
    }
    installer() {
        case $manager in
            apt-get) say "${root}apt install" ;;
            dnf) say "${root}dnf install" ;;
            pacman) say "${root}pacman -S --needed" ;;
            zypper) say "${root}zypper install" ;;
            brew) say "brew install" ;;
            pkg) say "${root}pkg install" ;;
            pkg_add) say "${root}pkg_add" ;;
            pkgin) say "${root}pkgin install" ;;
        esac
    }
    # How to get $1 (a need from package()), as the lines to print.
    hint() {
        names=$(package "$1")
        if [ -n "$manager" ] && [ -n "$names" ]; then
            if [ -z "$root" ] && [ "$manager" != brew ]; then
                say "Install it, as root, with:"
            else
                say "Install it with:"
            fi
            say "  $(installer) $names"
        else
            say "Install it with your system's package manager."
        fi
        if [ "$manager" = brew ] && ! have brew; then
            say "Homebrew first, if you do not have it: https://brew.sh"
        fi
    }

    # git: the clone, and every update after it. On a Mac without the
    # command line tools /usr/bin/git is only a stub that offers them.
    if [ "$system" = Darwin ] && ! xcode-select -p < /dev/null > /dev/null 2>&1; then
        fail "the Xcode command line tools are not installed; they bring git and the C++"
        say "compiler. Install them, then run this again:"
        say "  xcode-select --install"
        return 1
    fi
    if ! have git; then
        fail "rmp needs git, and there is none on PATH. Nothing was installed."
        hint git
        return 1
    fi

    # The same names, in the same order, as the rmp launcher, and each one
    # ASKED for 3.11: macOS's own python3 is 3.9.
    python=
    old=
    for py in python3 python3.14 python3.13 python3.12 python3.11 python; do
        if have "$py"; then
            if "$py" -c 'import sys; sys.exit(sys.version_info < (3, 11))' \
                < /dev/null > /dev/null 2>&1; then
                python=$py
                break
            fi
            if [ -z "$old" ]; then
                old=$("$py" -c 'import sys; print("%d.%d" % sys.version_info[:2])' \
                    < /dev/null 2> /dev/null)
                old="$py (${old:-an unknown version})"
            fi
        fi
    done
    if [ -z "$python" ]; then
        if [ -n "$old" ]; then
            fail "rmp needs Python 3.11 or newer, and $old is older."
            say "Nothing was installed."
            hint python-new
        else
            fail "rmp needs Python 3.11 or newer, and there is none on PATH."
            say "Nothing was installed."
            hint python
        fi
        return 1
    fi

    if [ -d "$dest/.git" ]; then
        say "rmp: updating the framework in $dest"
        if ! "$dest/rmp" update < /dev/null; then
            fail "$dest could not be updated (see above). Fix it, or move it away and"
            say "run this again."
            return 1
        fi
    else
        # An empty folder is fine; rmdir removes nothing else.
        if [ -e "$dest" ] && ! rmdir "$dest" < /dev/null 2> /dev/null; then
            fail "$dest is there and is not a clone of the framework. Move it away and"
            say "run this again. Nothing was installed."
            return 1
        fi
        mkdir -p "$HOME/.local/share" < /dev/null || return 1
        say "rmp: cloning $source ($branch) into $dest"
        if ! GIT_TERMINAL_PROMPT=0 git clone --quiet --filter=blob:none --branch "$branch" \
            "$source" "$dest" < /dev/null; then
            fail "git clone failed (see above). Nothing was installed."
            return 1
        fi
    fi
    if ! "$dest/rmp" install < /dev/null; then
        fail "rmp install did not finish (see above)."
        return 1
    fi

    # What a build needs beyond what rmp itself does. Said, never installed.
    missing=
    packages=
    need() {
        missing="$missing${missing:+, }$1"
        if [ -n "$2" ]; then packages="$packages $2"; fi
    }
    if ! have cmake; then need CMake "$(package cmake)"; fi
    if ! have ninja && ! have ninja-build && ! have samu; then
        need Ninja "$(package ninja)"
    fi
    if ! have c++ && ! have g++ && ! have clang++; then
        need "a C++ compiler" "$(package cxx)"
    fi
    case $system in
        Linux | FreeBSD)
            x11=yes
            if have pkg-config; then
                pkg-config --exists x11 xrandr xi xcursor xinerama gl < /dev/null || x11=
            else
                for h in X11/Xlib.h X11/extensions/Xrandr.h X11/extensions/XInput2.h \
                    X11/Xcursor/Xcursor.h X11/extensions/Xinerama.h GL/gl.h; do
                    if [ ! -f "/usr/include/$h" ] && [ ! -f "/usr/local/include/$h" ]; then
                        x11=
                    fi
                done
            fi
            if [ -z "$x11" ]; then need "the X11 development headers" "$(package x11)"; fi ;;
    esac
    say ""
    if [ -z "$missing" ]; then
        say "Everything a build needs is here: CMake, Ninja and a C++ compiler."
        return 0
    fi
    say "To build a game, this machine still needs: $missing."
    if [ -n "$manager" ] && [ -n "$packages" ]; then
        if [ -z "$root" ] && [ "$manager" != brew ]; then
            say "Install them, as root, with:"
        else
            say "Install them with:"
        fi
        say "  $(installer)$packages"
    else
        say "Install them with your system's package manager."
    fi
    return 0
}

main "$@"
