# SPDX-License-Identifier: AGPL-3.0-only
# Entry point for ADI's external build driver; use the existing adaptation,
# not a second copy of the ZL DSP. ZL_SOURCE is revision-checked there.
cmake_minimum_required(VERSION 3.24.1)
project(adi_dynamic_eq_external LANGUAGES C CXX)
enable_testing()
add_subdirectory(../../dynamic-eq dynamic-eq)
