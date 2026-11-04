#!/usr/bin/env bash
# End-to-end test of scripts/common/universal/github_downloader.c against mock_github.py.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
work="${TMPDIR:-/tmp}/github-sim"
build="$work/build"
port=18766
rm -rf "$work"
mkdir -p "$work"
"$here/build.sh" "$build" >/dev/null
sd="$work/sd"
mkdir -p "$sd"
ln -s / "$work/sdmc:"
# point the script at the mock, and its SD-root prefix at the test card
sed -e "s|https://api.github.com/repos/|http://127.0.0.1:$port/api/repos/|" \
    -e "s|https://github.com/|http://127.0.0.1:$port/|" \
    -e "s|        g_sd\[0\] = '\\\\0';|        strcpy(g_sd, \"$sd\");|" \
    "$root/scripts/common/universal/github_downloader.c" > "$work/gh.c"
grep -q "strcpy(g_sd, \"$sd\")" "$work/gh.c"
echo "000400000011C500|Test|1|0" > "$sd/titles.txt"
python3 "$here/mock_github.py" $port &
mock=$!
trap 'kill $mock 2>/dev/null || true' EXIT
sleep 1
fail() { echo "FAIL: $*"; exit 1; }
run() {
    local name="$1"; shift
    printf '%s\n' "$@" > "$sd/answers.txt"
    echo "=== $name"
    local rc=0
    (cd "$work" && "$build/host_sim" "$sd" "$work/gh.c" "") > "$work/$name.out" 2> "$work/$name.err" || rc=$?
    if [ "$rc" != "${EXPECT:-0}" ]; then tail -30 "$work/$name.out"; tail -15 "$work/$name.err"; fail "$name exited $rc"; fi
    grep -E "^\[sim\]" "$work/$name.out"
}
run 01-preset-dropbox 0 y ok
[ "$(cat "$sd/scripts/universal/dropbox.c")" = "/* dropbox */" ] || fail "dropbox.c not saved"
run 02-preset-firm 1 y ok
[ -f "$sd/luma/payloads/GodMode9_RefFix.firm" ] || fail "firm not saved"
[ ! -f "$sd/luma/payloads/GodMode9_RefFix.firm.part" ] || fail ".part left behind"
run 03-self-update 2 y ok
[ -f "$sd/scripts/universal/github_downloader.c" ] || fail "self-update not saved"
run 04-custom-firm 3 test/proj 0 0 ok
grep -q "\[PICK\] test/proj v1" "$work/04-custom-firm.out" || fail "asset list not shown"
cmp <(python3 -c "import sys; sys.stdout.buffer.write(b'FIRM'+bytes(range(256))*40)") "$sd/luma/payloads/tool.firm" || fail "custom firm content"
run 05-custom-script-replace 3 test/proj 1 0 ok
run 06-custom-replace-again 3 test/proj 1 0 y ok
[ -f "$sd/scripts/universal/script.c" ] || fail "script.c not saved to scripts"
EXPECT=0 run 07-bad-name 3 "not a repo" ok
grep -q "owner/repo" "$work/07-bad-name.out" || fail "bad name not explained"
EXPECT=1 run 08-missing-repo 3 nobody/none ok
grep -q "Not found (404)" "$work/08-missing-repo.out" || fail "404 not explained"
EXPECT=0 run 09-cancel 3 "<cancel>"
echo "ALL GITHUB SIM TESTS PASSED"
