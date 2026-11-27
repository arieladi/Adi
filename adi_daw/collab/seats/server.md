I took the server engine seat on 27 November 2026.

| Item | What changed | Proof | State |
| --- | --- | --- | --- |
| DF-E92 wire study | Read the pinned stock v13 client, server and JUCE wire layouts. Recommend an ADI-native client, with a negotiated extension in the server fork for full plug-in identity and CLAP events. Stock TCP has neither peer authentication nor encryption. | Checked 40 source and local links and compared the release and master audio/handshake layouts. All five validators pass. Chain `700450592`. | Study complete. Live stock-server interoperability remains a separate proof. |

## For the director

A. Stock server support in the release. I recommend using stock TCP for the trusted-LAN interoperability trial and requiring an authenticated, encrypted connection to the ADI server for the release. Decide whether the release also exposes stock TCP as an explicitly selected trusted-LAN option.
