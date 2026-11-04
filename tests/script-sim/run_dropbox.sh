#!/usr/bin/env bash
# End-to-end test of scripts/common/universal/dropbox.c against mock_dropbox.py,
# with the real script bindings (see host_sim.cpp). Run from anywhere:
#   tests/script-sim/run_dropbox.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
work="${TMPDIR:-/tmp}/dropbox-sim"
build="$work/build"
port=18765
rm -rf "$work"
mkdir -p "$work"
"$here/build.sh" "$build" >/dev/null

# the script, pointed at the mock instead of dropbox.com
sed -e "s|https://api.dropboxapi.com/oauth2/token|http://127.0.0.1:$port/oauth2/token|" \
    -e "s|https://api.dropboxapi.com/2/|http://127.0.0.1:$port/api/2/|" \
    -e "s|https://content.dropboxapi.com/2/files/|http://127.0.0.1:$port/content/2/files/|" \
    "$root/scripts/common/universal/dropbox.c" > "$work/dropbox.c"
grep -q "127.0.0.1:$port/content/2/files/download" "$work/dropbox.c"

sd="$work/sd"
mkdir -p "$sd"
# sd_mkdirs prefixes "sdmc:" as on the console; from inside $work that name is
# a link to /, so "sdmc:/tmp/..." lands on the same folder as "/tmp/..."
ln -s / "$work/sdmc:"
cat > "$sd/titles.txt" <<'EOF'
000400000011C500|ポケットモンスター アルファサファイア|1|1
0004000000055D00|Pokemon X|1|0
0004000000AAAA00|No Save Game|0|0
EOF
mkdir -p "$sd/archives/000400000011C500_0/sub/deep" "$sd/archives/0004000000055D00_0"
head -c 491520 /dev/urandom > "$sd/archives/000400000011C500_0/main"   # ORAS-sized save
echo "deep file" > "$sd/archives/000400000011C500_0/sub/deep/x.bin"
echo "x save" > "$sd/archives/0004000000055D00_0/main"
mkdir -p "$sd/saves/0004000000055D00 Pokemon X/old-backup"
echo "old" > "$sd/saves/0004000000055D00 Pokemon X/old-backup/main"
mkdir -p "$sd/extdata/000400000011C500 ポケットモンスター アルファサファイア/ext1"
echo "ext" > "$sd/extdata/000400000011C500 ポケットモンスター アルファサファイア/ext1/file"

state="$work/state.json"
python3 "$here/mock_dropbox.py" $port "$state" &
mock=$!
trap 'kill $mock 2>/dev/null || true' EXIT
sleep 1

fail() { echo "FAIL: $*"; exit 1; }
run() { # name, selected title, answers... (EXPECT=1 when the run should fail)
    local name="$1" sel="$2"
    shift 2
    printf '%s\n' "$@" > "$sd/answers.txt"
    echo "=== $name"
    local rc=0
    (cd "$work" && "$build/host_sim" "$sd" "$work/dropbox.c" "$sel") > "$work/$name.out" 2> "$work/$name.err" || rc=$?
    if [ "$rc" != "${EXPECT:-0}" ]; then
        tail -40 "$work/$name.out"
        tail -20 "$work/$name.err"
        fail "$name: script failed"
    fi
    grep -E "^\[sim\]" "$work/$name.out"
}
remote() { python3 -c "import json,sys; print('\n'.join(sorted(json.load(open('$state'))['files'])))"; }

# 1. wrong code first: refused cleanly
EXPECT=1 run 01-badcode "" ok KEY SECRET ok WRONG ok
grep -q "did not accept that code" "$work/01-badcode.out" || fail "bad code not explained"
[ ! -f "$sd/config/dropbox.vault" ] || fail "vault written after a failed sign-in"

# 2. sign in, back up everything, sync
run 02-signin-backup "" ok KEY SECRET ok GOODCODE n ok 1 ok
[ -f "$sd/config/dropbox.vault" ] || fail "no vault"
grep -q "Backed up: 2" "$work/02-signin-backup.out" || fail "expected 2 fresh backups"
grep -q "Uploaded: 4" "$work/02-signin-backup.out" || fail "expected 4 uploads (2 new + old + extdata)"
remote | tee "$work/remote1.txt"
grep -q "^/3ds/saves/000400000011C500 ポケットモンスター アルファサファイア/.*\.zip$" "$work/remote1.txt" || fail "ORAS backup not in Dropbox"
grep -q "^/3ds/saves/0004000000055D00 Pokemon X/old-backup.zip$" "$work/remote1.txt" || fail "old backup not uploaded"
grep -q "^/3ds/extdata/000400000011C500 .*/ext1.zip$" "$work/remote1.txt" || fail "extdata not uploaded"
# the fresh ORAS backup holds the full save tree, byte for byte
oras_b=$(ls -d "$sd/saves/000400000011C500 "*/2* | head -1)
cmp "$oras_b/main" "$sd/archives/000400000011C500_0/main" || fail "backup main differs"
cmp "$oras_b/sub/deep/x.bin" "$sd/archives/000400000011C500_0/sub/deep/x.bin" || fail "nested file differs"

# 3. another console uploaded a backup: it comes down, nothing goes up again
python3 - "$state" <<'EOF'
import json, sys
s = json.load(open(sys.argv[1]))
src = [p for p in s["files"] if p.endswith("old-backup.zip")][0]
s["files"]["/3ds/saves/0004000000055D00 Pokemon X/from-other.zip"] = s["files"][src]
json.dump(s, open(sys.argv[1], "w"), ensure_ascii=False)
EOF
kill $mock; wait $mock 2>/dev/null || true
python3 "$here/mock_dropbox.py" $port "$state" &
mock=$!
sleep 1
run 03-sync-down "" 0 ok
grep -q "Downloaded: 1" "$work/03-sync-down.out" || fail "expected 1 download"
grep -q "Uploaded: 0" "$work/03-sync-down.out" || fail "nothing should be re-uploaded"
cmp "$sd/saves/0004000000055D00 Pokemon X/from-other/main" "$sd/saves/0004000000055D00 Pokemon X/old-backup/main" || fail "downloaded backup content differs"

# 4. run again: everything already in sync
run 04-idle "" 0 ok
grep -q "Uploaded: 0" "$work/04-idle.out" && grep -q "Downloaded: 0" "$work/04-idle.out" || fail "second sync not idle"

# 5. highlighted title: back up + sync just that one
run 05-this-title 0004000000055D00 2 ok
grep -q "Backed up: 1" "$work/05-this-title.out" && grep -q "Uploaded: 1" "$work/05-this-title.out" || fail "this-title run"

# 6. settings: account check, then sign out (revokes, deletes the vault)
run 06-account "" 2 0 ok
grep -q "tester@example.com" "$work/06-account.out" || fail "account not shown"
run 07-signout "" 2 3 y ok
[ ! -f "$sd/config/dropbox.vault" ] || fail "vault still there after sign out"
grep -q "auth/token/revoke" "$state" || fail "token not revoked"

# 8. sign in again from a setup file holding only the app key (secret typed)
echo '{"app_key":"KEY"}' > "$sd/config/dropbox.json"
run 08-setup-file "" SECRET ok GOODCODE n ok 2 0 ok
[ ! -f "$sd/config/dropbox.json" ] || fail "setup file not deleted"
[ -f "$sd/config/dropbox.vault" ] || fail "no vault after file sign-in"
grep -q "tester@example.com" "$work/08-setup-file.out" || fail "account not reachable after file sign-in"

echo "ALL DROPBOX SIM TESTS PASSED"
