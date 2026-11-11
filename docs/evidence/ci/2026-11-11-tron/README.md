# hw-test.yml on tron, 2026-11-11

This is the first green run of `.github/workflows/hw-test.yml` on `main`:
[run 43033512607](https://github.com/tfcollins/libdatax/actions/runs/43033512607),
dispatched with `place=tron` and `image=both` at libdatax `d054231`. The lab
runner (`hw-nemo`) booted tron twice over JTAG, once per boot set pinned in
`tests/hardware/images.json`, and ran `tests/hardware` against each. The files
here are that run's uploaded artifacts. `scripts/hw/redact.py` redacted them
on the runner before upload.

| Image | Files | Result | What it covers |
|---|---|---|---|
| `stock` (recovery PL HDL `main` `085a6ed7`, Kuiper 2023_R2 kernel and device tree) | `evidence-tron-stock.json` / `junit-tron-stock.xml` | 12 passed, 15 skipped (the P3a tests) | P2 streaming: RX, one-shot and cyclic TX, start/stop, digital loopback bit-exact on ch0-ch3 |
| `hopcap-sfp` (hopcap SFP image with the Tick PPS selector, XSA `6292e762…`) | `evidence-tron-hopcap-sfp.json` / `junit-tron-hopcap-sfp.xml` | 21 passed, 6 skipped (TX and loopback: TX DMA feeds vrt49's deframer) | P3a: GPIO staircase, hop schedule and priming, dense hops, late drop, flush, readback across the band, PPS alignment, tune after alignment, MCS verification, PHASE_COHERENT false; P2 RX |

The libiio pin is `6534f14215f4738cec224bf50f13f475fe5264a6`. The image
hashes are in each evidence JSON (`image_hashes`).

The results match the controller-run sessions in [p2](../../p2/README.md) and
[p3a](../../p3a/README.md). The P3a figures and the measured limits are
described there.
