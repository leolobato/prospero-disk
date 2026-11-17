# Prospero Disk

A disk-usage explorer for jailbroken PS5s (title `PPSA93475`), forked from
ps5-homebrew-ui (remote `kit`). What to build is in
[docs/PRD.md](docs/PRD.md); the MVP scope is its section 11 and the milestones section 12.

The kit's instructions for coding agents are in [AGENTS.md](AGENTS.md); they
apply here and are imported below. Its gallery designs were removed: the app's
screens live in `src/concepts/disk*.cpp`, and the Component Library
(`components`) stays as a reference until milestone M3.

The one habit that matters most here: **preview on the PC and look at the
pictures before calling any UI change done.**

```bash
tools/host-snapshots.sh build/snapshots <design id|all>   # PNG frames in build/snapshots/
make test-unit                                            # tests, sanitizers on
make                                                      # the PS5 app folder in dist/
make lint                                                 # format, static analysis, metadata
```

The preview renders the same UI code as the console, off-screen, with no
console and no GPU ("Local preview" in AGENTS.md has the options, the output
names and what it cannot show). Never launch on a console without asking the
owner first.

@AGENTS.md
