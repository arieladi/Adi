2026-11-27 — agent seat online. Starting the ADI chain from `origin/main`.

| Item | What changed | Proof | State |
|---|---|---|---|
| 1. Marker prompts | The changeset boundary refuses `marker.setPrompt` and prompt writes through marker creation or captured Arrangement edits. The host can mark a marker run for mandatory review, including at Apply. Observe still runs without edit capability. | On the original code the new changeset regression failed seven checks. On `70c475f1b` the Debug build with `ADI_WERROR=ON`, five relevant CTest suites and all five validators pass on Linux. | Pushed. The helper host will supply run origin in item 8. |
