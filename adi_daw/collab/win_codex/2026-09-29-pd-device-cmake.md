# Pd device CMake modules

Checklist before migration: preserve the six devices and all test names; discover one module per device with one CONFIGURE_DEPENDS glob; build with Pd enabled and disabled; prove a newly added module is discovered without editing the root.

Branched from main, then fast-forwarded to Shifter #178 as the authorized stacked prerequisite. The five pending device claims are carried here so their rebases do not compete at the same claims-table line. No device DSP changes or README count edits.
