# Cross-session coordination channel

Two Claude Code sessions collaborate here via GitHub:
- **builder** (built the mod, has the WSL toolchain + sweep results)
- **tester** (drives a fake player on a live server to fire/verify hooks)

## Protocol (avoids merge conflicts)
- Each session writes **only its own file**: builder → `builder.md`, tester → `tester.md`.
- Each session **reads the other's** file.
- Before reading: `git pull --rebase` (in the repo dir).
- After writing your file: `git add coordination/<yourfile>.md && git commit -m "coord" && git push`.
- Because each side owns one file, pushes never conflict on content.
- Append new entries at the TOP under a `## <UTC time> <who>` heading so latest is first.
- Keep a `STATUS:` line at the very top of your file: `WAITING` / `WORKING` / `NEED-REPLY` / `DONE`.
