# Shared authored ADI-Q processing provenance

This component extracts the authored processing shipped in ADI-Q 0.3.18. The ZLEqualizer-derived source retains its copyright notices and AGPL-3.0-or-later terms; the complete original license is retained at detail/ThirdParty/ZLEqualizer/LICENSE.md. The nested zldsp_fft code retains its Apache-2.0 license at detail/ThirdParty/ZLEqualizer/zldsp_fft/LICENSE.md.

The processing closure is isolated with adi_eq_zlp / adi_eq_dsp / adi_eq_chore namespaces, unique FFT include guards and unique extension class names. Only framework parameter factories and AudioProcessor/AsyncUpdater latency notification are separated from processing. Prepared-plan read access and a standard C++ typed facade are additive. No private research reports, proprietary implementation or new native-reference observations are added by this extraction.

Highway 1.4.0 is a separate Apache-2.0 dependency. Supply the existing pinned hwy target or ADI_MIX_EQ_HIGHWAY_SOURCE_DIR; do not substitute an FFT or dispatch configuration without rerunning the authored-engine equivalence gate. The original build defines HWY_COMPILE_ONLY_STATIC and ZL_EQ_BAND_NUM=24. No Eigen or xsimd dependency is used by this closure. Source attribution does not replace the combined application's distribution-license review.
