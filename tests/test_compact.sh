#!/usr/bin/env sh

if [ $# -lt 1 ] || ! [ -d "$1" ]; then
    cat >&2 <<END
usage: $0 <procfs-snapshot-dir>

Run 'sp_smaps_snapshot' on (a subset of) the given <procfs-snapshot-dir>, let it
produce the output in both the normal and compact formats, expand the compact
formatted output wirh 'sp_smaps_expand' and compare it with the normal output
ignoring whitespace differences.
END
    exit 1
fi

PROC_ROOT=$1
PROC_ROOT=$(readlink -f "$PROC_ROOT") || exit


TMP_DIR=

# Take a copy of the given PROC_ROOT to deal with two issues:
#
# 1. Ensure that the directory for PID 1 comes first in directory listing. At
# least ext4 is unusable in this respect, so copy it to a tmpfs.
#
# 2. Copying/symlinking the procfs snapshot to tmpfs is incredibly slow (why??)
# and comparing the resulting files with "diff" running on (my) device is even
# slower. Therefore, limit the test data.
PROC_ROOT_COPY=

# This amount twice - once for regular processes and once for others
: "${PROC_SUBSET_SIZE:=30}"

cleanup() (
    trap 'echo cleaning up...' INT TERM HUP
    if [ "$TMP_DIR" ]; then
        rm -rf "$TMP_DIR"
    fi
    if [ "$PROC_ROOT_COPY" ]; then
        rm -rf "$PROC_ROOT_COPY"
    fi
)
trap 'cleanup' EXIT
trap 'exit 1' INT TERM HUP

TMP_DIR=$(mktemp -d "sp-smaps-test.XXXXX") || exit
PROC_ROOT_COPY=$(mktemp -d "sp-smaps-test.XXXXX" --tmpdir) || exit

if [ "$(stat --file-system --format="%T" "$PROC_ROOT_COPY")" != tmpfs ]; then
    echo "\$TMPDIR is not on 'tmpfs'." >&2
    exit 1
fi

echo "+ Copying data out of '$PROC_ROOT'..." >&2

regular_processes_to_copy=$PROC_SUBSET_SIZE
other_processes_to_copy=$PROC_SUBSET_SIZE

# Copy it in reverse version (numeric) order. Last touched entry comes first in
# directory listing on tmpfs.
ls -r -v "$PROC_ROOT" |while read pid_dir; do
    if [ "$pid_dir" == 1 ]; then
        : # always copy diretory of PID 1
    elif [ "$pid_dir" == 2 ]; then
        : # always copy directory of the "kthreadd" process
    elif [ -L "$PROC_ROOT/$pid_dir/exe" ]; then
        [ "$regular_processes_to_copy" -gt 0 ] || continue
        let regular_processes_to_copy--
    else
        [ "$other_processes_to_copy" -gt 0 ] || continue
        let other_processes_to_copy--
    fi

    mkdir -p "$PROC_ROOT_COPY/$pid_dir/" || exit
    for file in $(ls "$PROC_ROOT/$pid_dir"); do
        # Symlink regular files, copy symlinks (there is just the "exe"
        # symlink).
        if [ -L "$PROC_ROOT/$pid_dir/$file" ]; then
            cp -a "$PROC_ROOT/$pid_dir/$file" "$PROC_ROOT_COPY/$pid_dir/" || exit
        else
            ln -s "$PROC_ROOT/$pid_dir/$file" "$PROC_ROOT_COPY/$pid_dir/" || exit
        fi
    done
done || exit

selected_pids_count=$(ls "$PROC_ROOT_COPY" |wc -l)

if [ "$selected_pids_count" -lt "$((PROC_SUBSET_SIZE * 2))" ]; then
    echo "+ Error: Not enough processes to test with" >&2
    exit 1
fi

set -e

echo "+ Taking normal snapshot..." >&2
sp_smaps_snapshot --root="$PROC_ROOT_COPY" >"$TMP_DIR/smaps.cap"
echo "+ Taking compact snapshot..." >&2
sp_smaps_snapshot --root="$PROC_ROOT_COPY" --compact >"$TMP_DIR/smaps.ccap"
echo "+ Expanding compact snapshot..." >&2
sp_smaps_expand <"$TMP_DIR/smaps.ccap" >"$TMP_DIR/smaps-expanded.cap"

echo "+ Comparing..." >&2
if diff -q -b "$TMP_DIR/smaps.cap" "$TMP_DIR/smaps-expanded.cap"; then
    echo "PASS"
else
    echo "FAIL"
    exit 1
fi
