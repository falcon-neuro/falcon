#!/bin/sh
# Lints every .cpp under src. A file is skipped while it, every file it includes, the lint settings
# and the compile commands are byte for byte what they were at its last clean lint, so the result
# equals a full run. Only includes that lead into src or build/generated are followed: after
# upgrading the compiler or the system's libraries, run with --all.
set -e
cd "$(dirname "$0")/.."

is_linting_everything=false
for argument in "$@"; do
    case "$argument" in
        --all)
            is_linting_everything=true
            ;;
        *)
            echo "usage: sh scripts/tidy.sh [--all]" >&2
            exit 2
            ;;
    esac
done

clang_tidy_path=$(command -v clang-tidy)
clang_tidy_version=$(clang-tidy --version)
printf '%s\n%s\n' "$clang_tidy_path" "$clang_tidy_version"

if [ ! -f build/compile_commands.json ]; then
    echo "tidy.sh: build/compile_commands.json is missing, configure the build first" >&2
    exit 1
fi

clean_directory=build/tidy/clean
mkdir -p "$clean_directory"
run_directory=$(mktemp -d build/tidy/run.XXXXXX)
pending_directory=$run_directory/pending
trap 'rm -rf "$run_directory"' EXIT
trap 'exit 130' INT TERM HUP

{
    find src ! -type d
    # The build writes sources there that it derives from src and that files in src include.
    if [ -d build/generated ]; then
        find build/generated ! -type d
    fi
} > "$run_directory/unsorted_tree"
# find lists a directory in an order that can change between runs, and a manifest must not.
LC_ALL=C sort "$run_directory/unsorted_tree" > "$run_directory/tree"
# The lists below hold one name per line with tab-separated columns, and sha256sum rewrites a name
# that holds a backslash.
if LC_ALL=C grep -q '[[:cntrl:]\\]' "$run_directory/tree"; then
    echo "tidy.sh: a file name holds a backslash or a control character" >&2
    exit 1
fi
awk '/^src\/.*\.cpp$/' "$run_directory/tree" > "$run_directory/roots"

# Prints "root<tab>file" for each root and for every file the root includes, directly or not.
awk '
function fail(message) {
    print "tidy.sh: " message > "/dev/stderr"
    exit 1
}

function directory_of(path) {
    sub("/[^/]*$", "", path)
    return path
}

# One file reached by two routes ("a/../b/x" and "b/x") must get one name.
function normalized(path,    parts, part_count, part_index, kept, kept_count, result) {
    part_count = split(path, parts, "/")
    kept_count = 0
    for (part_index = 1; part_index <= part_count; part_index++) {
        if (parts[part_index] == "" || parts[part_index] == ".") {
            continue
        }
        if (parts[part_index] == ".." && kept_count > 0 && kept[kept_count] != "..") {
            kept_count--
            continue
        }
        kept[++kept_count] = parts[part_index]
    }
    result = ""
    for (part_index = 1; part_index <= kept_count; part_index++) {
        result = result (part_index > 1 ? "/" : "") kept[part_index]
    }
    return result
}

function add_include(file, name, is_quoted,    beside_includer, base_name) {
    if (is_quoted) {
        beside_includer = normalized(directory_of(file) "/" name)
        if (beside_includer in is_in_tree) {
            includes_of[file] = includes_of[file] beside_includer "\n"
            return
        }
    }
    # The compiler finds this name through an include directory, and which one is not known here,
    # so every file of that name counts. A name found nowhere in the tree belongs to a library.
    base_name = name
    sub(".*/", "", base_name)
    if (base_name in paths_by_base_name) {
        includes_of[file] = includes_of[file] paths_by_base_name[base_name]
    }
}

function scan(file,    line, read_status, name, is_quoted) {
    if (file in includes_of) {
        return
    }
    includes_of[file] = ""
    while ((read_status = (getline line < file)) > 0) {
        if (line !~ /^[ \t]*#[ \t]*include/) {
            continue
        }
        if (match(line, /^[ \t]*#[ \t]*include[ \t]*"[^"]+"/)) {
            is_quoted = 1
        } else if (match(line, /^[ \t]*#[ \t]*include[ \t]*<[^>]+>/)) {
            is_quoted = 0
        } else {
            fail(file ": cannot tell which file this line includes: " line)
        }
        name = substr(line, RSTART, RLENGTH - 1)
        sub(/^[^"<]*["<]/, "", name)
        add_include(file, name, is_quoted)
    }
    if (read_status < 0) {
        fail("cannot read " file)
    }
    close(file)
}

function print_closure(root,    pending, pending_count, is_reached, file, targets, target_count, target_index) {
    pending_count = 1
    pending[1] = root
    is_reached[root] = 1
    while (pending_count > 0) {
        file = pending[pending_count--]
        print root "\t" file
        scan(file)
        target_count = split(includes_of[file], targets, "\n")
        for (target_index = 1; target_index <= target_count; target_index++) {
            if (targets[target_index] == "" || (targets[target_index] in is_reached)) {
                continue
            }
            is_reached[targets[target_index]] = 1
            pending[++pending_count] = targets[target_index]
        }
    }
}

is_reading_roots {
    roots[++root_count] = $0
    next
}

{
    is_in_tree[$0] = 1
    base_name = $0
    sub(".*/", "", base_name)
    paths_by_base_name[base_name] = paths_by_base_name[base_name] $0 "\n"
}

END {
    for (root_index = 1; root_index <= root_count; root_index++) {
        print_closure(roots[root_index])
    }
}
' "$run_directory/tree" is_reading_roots=1 "$run_directory/roots" > "$run_directory/dependencies"

{
    printf '%s\n' "$clang_tidy_version"
    # CMakeLists.txt stands in for the libraries whose versions it pins: their headers are not followed.
    sha256sum -- "$clang_tidy_path" .clang-tidy CMakeLists.txt build/compile_commands.json
    grep -E '/(\.clang-tidy|CMakeLists\.txt)$' "$run_directory/tree" | xargs -d '\n' -r sha256sum --
} > "$run_directory/configuration"

: > "$run_directory/stale"
while IFS= read -r root; do
    pending_manifest=$pending_directory/$root.manifest
    mkdir -p "$(dirname "$pending_manifest")"
    {
        cat "$run_directory/configuration"
        root=$root awk -F '\t' '$1 == ENVIRON["root"] { print $2 }' "$run_directory/dependencies" |
            xargs -d '\n' -r sha256sum --
    } > "$pending_manifest"
    if [ "$is_linting_everything" = true ] ||
        ! cmp -s "$pending_manifest" "$clean_directory/$root.manifest"; then
        printf '%s\n' "$root" >> "$run_directory/stale"
    fi
done < "$run_directory/roots"

echo "linting $(wc -l < "$run_directory/stale") of $(wc -l < "$run_directory/roots") files"

export pending_directory clean_directory
xargs -d '\n' -r -P "$(nproc)" -n 1 sh -c '
    root=$1
    clean_manifest=$clean_directory/$root.manifest
    # Under --all the old manifest can still match, so it must not outlive a lint that fails.
    rm -f "$clean_manifest"
    if output=$(clang-tidy -p build --quiet "$root") && [ -z "$output" ]; then
        mkdir -p "$(dirname "$clean_manifest")"
        mv "$pending_directory/$root.manifest" "$clean_manifest"
    else
        if [ -n "$output" ]; then
            printf "%s\n" "$output"
        fi
        exit 1
    fi
' sh < "$run_directory/stale"
