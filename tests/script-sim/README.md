# script-sim

Runs a Checkpoint picoc script on the build machine with the real script
bindings from `common/script/` (JSON, HTTP via libcurl, zip, device_seal,
progress, heap), a fake title catalog, folder-backed save archives and scripted
dialog answers. See `host_sim.cpp`.

- `build.sh [outdir]` — builds `host_sim` (needs g++ with C++23, libcurl and
  mbedtls development packages).
- `run_dropbox.sh` — end-to-end test of `scripts/common/universal/dropbox.c`
  against `mock_dropbox.py`, a strict local stand-in for the Dropbox API.
