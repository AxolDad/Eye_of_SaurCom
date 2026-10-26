# ARDOP (monitor)

ARDOP, the Amateur Radio Digital Open Protocol of Rick Muething (KN6KB), is the HF ARQ data modem used by Winlink gateways, in four channel widths: 200, 500, 1000 and 2000 Hz. Eye of SaurCom listens to it; it never connects, answers or transmits.

## What the mode shows

- Every frame by its type: ID, connect request (200 / 500 / 1000 / 2000 Hz, "max" or "forced"), connect acknowledge, ping and ping acknowledge, ACK and NAK with their quality (38 to 100), break, idle, disconnect, end, connect rejected, and the data frames by name (`4FSK.500.100.E`, `8PSK.200.100.O` and so on; the E / O is the even / odd toggle the ARQ uses to tell a repeat from a new frame).
- Call signs (with SSID) and the grid square of ID frames; the station list.
- The session ID (the second frame type byte is the type XOR the session ID of the two connected stations; `FF` means unconnected or broadcast, shown as "-").
- The leader length reported in a connect acknowledge, the S/N and quality of a ping acknowledge.
- Data bytes of the 4FSK frames at 50 and 100 baud (`4FSK.200.50S`, `4FSK.500.100`, `4FSK.500.100S`) that pass their CRC-16, in a text view. Many Winlink messages are compressed binary; unprintable bytes are shown as dots.
- PSK, QAM and 600 baud 4FSK data frames are listed by their header (type, modulation, bandwidth class, baud) but their data is not read. Only frames of the 4FSK family can be read by this receiver.
- Upper sideband, the signal at 1500 Hz in the audio; the receiver finds a tuning error of up to about 50 Hz.

## How it works

Every ARDOP frame starts with a leader of two tones (1475 and 1525 Hz, 100 to 1000 ms), then one symbol with the leader's phase reversed (sync), then ten 4FSK symbols at 50 baud in a 200 Hz channel (tones 1425, 1475, 1525, 1575 Hz): the frame type byte, the type XOR the session ID, and one parity symbol after each byte (1 XOR the four type symbols). The receiver measures the two leader tones over 20 ms windows every millisecond at seven trial tuning errors; a long run of high energy that ends in the dip where the phase reversal cuts the window in half gives the symbol timing to a millisecond, the leader gives the tuning error to about a hertz, and the frame type is the table entry whose symbols and parity symbols have the smallest soft distance.

Bodies: ID, connect request and ping carry two six-bit packed eight-character fields (call sign and grid square, or caller and target) and four Reed-Solomon bytes over 12 data bytes. A connect acknowledge carries the leader length (in tens of ms) three times; a ping acknowledge the S/N and quality three times. A 4FSK data frame carries a byte count, the data (zero padded), a CRC-16 (seed FFFF, x^16 + x^12 + x^5 + 1, the low byte XORed with the frame type) and Reed-Solomon parity (GF(256), polynomial 0x11D, roots alpha^1 to alpha^n) per carrier; the 100 baud 500 Hz frames use tones 1350, 1450, 1550, 1650 Hz.

## Test signal

The synthetic source (`makeArdopSynth`, `core/include/dect2/ardop/ardop_gen.h`) plays a script with every kind of 4FSK frame (ID, connect request and acknowledge, ping and acknowledge, ACK, NAK, three data frames, a PSK header, disconnect, end) in noise. `ArdopGenerator` takes any frame list, tuning error, sender clock error, leader length and SNR.

## What is verified and what is not

Verified against the generator written for this mode (`tests/ardop`): all frame kinds above, tuning errors from -40 to +38 Hz, a sender mis-tuned inside the audio, 120 ppm of clock error either way, leaders from 100 ms to 1 s, 8 to 20 dB tone SNR in 2.5 kHz, and noise alone. The CRC-16 and Reed-Solomon parity are compared with golden values produced by the reference implementation's algorithms.

**Not verified against a real ARDOP transmission**: no recording of one was at hand. The frame type table, the tone layout (50 baud tones 1425 to 1575 Hz, 100 baud tones 1350 to 1650 Hz), the leader and sync construction, the parity rule, the ID / connect request layout and the data frame layout were taken from the public ARDOP specification (the frame type table of appendix B is a picture in the PDF, so the codes come from the ardopcf header and sources) and re-implemented. Details the sources left open — how a real radio's filters shape the leader, how a real sender's timing drifts, the exact locator packing of an ID frame — were chosen to be self-consistent. If a real recording does not decode, compare those first.

## Credits

ARDOP by Rick Muething KN6KB; ARDOPC by John Wiseman G8BPQ; ardopcf by Peter LaRue KG4JJA (MIT licence). No source file of theirs is part of Eye of SaurCom. See THIRD_PARTY.md.
