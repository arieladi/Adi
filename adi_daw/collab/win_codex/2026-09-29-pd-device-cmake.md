# Pd device CMake modules

Checklist before migration: preserve the six devices and all test names; discover one module per device with one CONFIGURE_DEPENDS glob; build with Pd enabled and disabled; prove a newly added module is discovered without editing the root.

Branched from main, then fast-forwarded to Shifter #178 as the authorized stacked prerequisite. The five pending device claims are carried here so their rebases do not compete at the same claims-table line. No device DSP changes or README count edits.

Validation: executable and CTest inventories match before/after. MSVC /WX built all six cores and their five Pd suites plus the engine suite; all twelve CTests passed. A separate ADI_WITH_PD=OFF build compiled all six cores. Adding a temporary module and requesting its target triggered automatic reconfiguration and succeeded, with no root edit; the temporary file was removed. The combined color-bass Pd test remains in Color Cab's module and still exercises both externals.
