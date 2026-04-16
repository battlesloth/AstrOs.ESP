# scripts/

Build-support scripts that run outside the normal C++ build.

## version_gen.py

Called automatically by PlatformIO before every build via `extra_scripts = pre:scripts/version_gen.py` in `platformio.ini`. Resolves the firmware version from `../VERSION` + git state and writes `../lib_native/AstrOsUtility/src/version_generated.hpp` (gitignored).

Can also be run standalone for debugging:

```
python3 scripts/version_gen.py
```

See `.docs/plans/20260411-0905-ci-pipeline-design.md` for the full versioning strategy.
