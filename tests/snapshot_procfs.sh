#!/usr/bin/env sh

if [ $# -lt 1 ] || [ -z "$1" ]; then
    cat >&2 <<END
usage: $0 <output-dir>

Take a copy of relevant files from /procfs.
END
    exit 1
fi

if [ $UID -ne 0 ]; then
    echo "$0: Root privileges needed" >&2
    exit 1
fi

DEST=$1

# This amount twice - once for regular processes and once for others
: "${PROC_SUBSET_SIZE:=30}"

regular_processes_to_copy=$PROC_SUBSET_SIZE
other_processes_to_copy=$PROC_SUBSET_SIZE

(cd /proc && find . -maxdepth 1 -type d -name '[0-9]*') \
    |while read pid_dir; do
        counter=
        if [ "$pid_dir" == 1 ]; then
            : # always copy diretory of PID 1
        elif [ "$pid_dir" == 2 ]; then
            : # always copy directory of the "kthreadd" process
        # Note that under procfs the "-e" test must be used, not "-L" as is used
        # in test_compact.sh which works on the snapshot (real file system)
        elif [ -e "/proc/$pid_dir/exe" ]; then
            [ "$regular_processes_to_copy" -gt 0 ] || continue
            counter=regular_processes_to_copy
        else
            [ "$other_processes_to_copy" -gt 0 ] || continue
            counter=other_processes_to_copy
        fi

        mkdir -p "$DEST/$pid_dir" || exit
        for file in exe cmdline status smaps; do
            file_abs="/proc/$pid_dir/$file"
            # Kernel processes...
            if [ "$file" == exe ] && ! ls -l "$file_abs" &>/dev/null; then
                touch "$DEST/$pid_dir/exe"
                continue
            fi
            if ! cp -a "$file_abs" "$DEST/$pid_dir/"; then
                # Process exited meanwhile
                rm -r "$DEST/$pid_dir"
                break
            fi
        done

        if [ -e "$DEST/$pid_dir" ] && [ "$counter" ]; then
            let "$counter"--
        fi
    done

selected_pids_count=$(ls "$DEST" |wc -l)

if [ "$selected_pids_count" -lt "$((PROC_SUBSET_SIZE * 2))" ]; then
    echo "Error: Not enough processes to test with" >&2
    exit 1
fi
