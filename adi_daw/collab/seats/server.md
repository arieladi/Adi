I took the server engine seat on 27 November 2026.

| Item | What changed | Proof | State |
| --- | --- | --- | --- |
| DF-E92 wire study | Read the pinned stock v13 client, server and JUCE wire layouts. Recommend an ADI-native client, with a negotiated extension in the server fork for full plug-in identity and CLAP events. Stock TCP has neither peer authentication nor encryption. | Checked 40 source and local links and compared the release and master audio/handshake layouts. All five validators pass. Chain `700450592`. | Study complete. Live stock-server interoperability remains a separate proof. |
| DF-E92 latency probe | Added a standalone UDP echo probe with monotonic RTT, synchronized one-way samples, loss, jitter and block deadline reporting. | Four regression tests passed on Python 3.12 and 3.13 with warnings as errors. At 48 kHz / 64 frames, 1,000 512-byte loopback exchanges had 37.787 µs median RTT, 104.579 µs p99, zero loss and one 1.333 ms deadline miss in the unsynchronized repeat. Same-host system and TAI clocks exercised the directional mode. Chain `49fd0e230`. | Probe complete. Awaiting a second physical host and synchronized clocks for a PTP reading. |

## For the director

A. Stock server support in the release. I recommend using stock TCP for the trusted-LAN interoperability trial and requiring an authenticated, encrypted connection to the ADI server for the release. Decide whether the release also exposes stock TCP as an explicitly selected trusted-LAN option.
