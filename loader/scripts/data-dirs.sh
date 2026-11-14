# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by shot.sh and drive.sh: where the fixture player finds map previews
# (it answers `pk3:` image refs from folders of pk3s, or of unpacked maps/*.jpg),
# and the sandbox arguments that let the GNOME runtime read those folders.
#
#   GODNOTIC_DATA_DIRS   colon-separated folders, if set
#   otherwise            the folders the app keeps its game data in
#                        (crates/core/src/paths.rs: ~/.xonotic/data, the
#                        Flatpak's and a plain install's godnotic/data, the
#                        Flatpak's /app/data) and an installed Xonotic
#                        Flatpak's stock pk3s, those that exist
#
# Sets GN_DATA_DIRS (colon-separated) and GN_DATA_FS (--filesystem=...:ro
# arguments for `flatpak run`). Symbolic links inside a folder must point
# somewhere the sandbox also sees (under home, or one of these folders).

gn_exists() { # a folder here, or on the host when flatpak is distrobox-host-exec
	[ -d "$1" ] || [ -d "/run/host$1" ]
}

GN_DATA_DIRS=""
GN_DATA_FS=()
if [ -n "${GODNOTIC_DATA_DIRS:-}" ]; then
	_gn_dirs="$GODNOTIC_DATA_DIRS"
else
	_gn_dirs=""
	for _gn_d in "$HOME/.xonotic/data" \
		"$HOME/.var/app/io.github.dixonSolutions.Godnotic/data/godnotic/data" \
		"${XDG_DATA_HOME:-$HOME/.local/share}/godnotic/data" \
		"$HOME/.local/share/flatpak/app/io.github.dixonSolutions.Godnotic/current/active/files/data" \
		/var/lib/flatpak/app/io.github.dixonSolutions.Godnotic/current/active/files/data \
		"$HOME/.local/share/flatpak/app/org.xonotic.Xonotic/current/active/files/share/xonotic/data" \
		/var/lib/flatpak/app/org.xonotic.Xonotic/current/active/files/share/xonotic/data; do
		gn_exists "$_gn_d" && _gn_dirs="$_gn_dirs:$_gn_d"
	done
fi
IFS=: read -r -a _gn_list <<< "$_gn_dirs"
for _gn_d in "${_gn_list[@]}"; do
	[ -n "$_gn_d" ] || continue
	GN_DATA_DIRS="${GN_DATA_DIRS:+$GN_DATA_DIRS:}$_gn_d"
	case "$_gn_d" in
		"$HOME"/*) ;; # the sandbox has home already
		*) GN_DATA_FS+=("--filesystem=$_gn_d:ro") ;;
	esac
done
unset _gn_dirs _gn_d _gn_list
