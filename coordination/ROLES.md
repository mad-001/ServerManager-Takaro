# Roles — who does what (avoid both sessions editing the same files)

## builder (this session) — owns the CODE and the repo as source of truth
Sole editor/committer of:
- `core/src/*` (C++), and rebuilds `winmm.dll` / `version.dll`
- `mod/TakaroConnector/Scripts/*` (universal Lua)
- `profiles/*.lua` (per-game profiles)
- docs, README, releases, and the bridge-game folders
Reacts to the tester's findings within ~1 min and pushes fixes.

## tester (other session) — owns TESTING & DISCOVERY on the live rig
- Boots servers, drives the fake player, dumps UFunctions, injects `ipc/req` to test actions.
- REPORTS in `tester.md`: what fires / breaks, real hook paths, param signatures, Lua errors,
  the contents of `ipc/evt`/`ipc/res`, screenshots.
- May tweak its OWN local test tree to keep testing unblocked, but does **not** commit changes
  to `core/`, `mod/`, or `profiles/` — instead report the need and builder implements it in the
  repo (so there's one source of truth and no merge collisions).

## Channel rule
Each session writes ONLY its own file (`builder.md` / `tester.md`), reads the other's,
`git pull` before reading, `git push` after writing.
