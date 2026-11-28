# Skip rule of the GPU ctest entries (plan F3 Task 1). Only device creation's absent-backend message skips:
# headless / viewer print "backend not available: <reason>" when createDevice exits 3. Any other exit 3 on a
# present backend (e.g. the T0 tier rejection "pipeline needs TIER 1, device is TIER 0") must be reported
# Failed, never Skipped. tests/viewer/run_viewer.py applies the same text (BACKEND_ABSENT).
set(ENG_BACKEND_ABSENT_SKIP_REGEX "backend not available:")
