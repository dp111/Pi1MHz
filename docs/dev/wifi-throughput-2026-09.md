# WiFi throughput — what the 2026-09 campaign established

> **STATUS 2026-09-18: the ~2.3 MB/s ceiling is NOT explained.** Everything on
> the host side is eliminated by measurement; the one tuning gain found is
> `wifi_ampdu_rts=0` at +6.2%. Read "What is still open" before planning work.

The question: a Pi Zero W on 2.4 GHz should manage 27–34 Mbit/s (published
iperf3 figures); we measure 18–20 Mbit/s. This records what was ruled out, so
none of it is re-investigated, and what remains.

## Eliminated, each by measurement

| candidate | verdict | evidence |
|---|---|---|
| Our driver | no | equals brcmfmac within 5–8% on the same board, firmware and AP, measured in the same hour with raw TCP |
| Too few SDIO RX descriptors | no | `bus:credall=1` saturates the dongle's queue (host at zero credits) and throughput does not move |
| Host servicing / interrupts | no | the card interrupt line is one MMIO read per poll-loop pass, ~20 µs; RX gate service latency <50 µs on 94 % of samples |
| Our poll loop | no | ICMP request-in to reply-out is 14–16 µs (measured 2026-07, `WIFI_LWIP_ICMP_PROBE_DIAG`) |
| PHY rate | no | pinning MCS7 changes nothing; pinning MCS3 halves throughput, so the pin works and we were already at MCS7 |
| SDIO bus | no | 12 % busy during a transfer, ~75 µs/frame |
| Retransmission | no | `delta_stats` during a transfer: 24 retries per ~1600 frames (1.5 %), zero failures |
| Aggregation disabled | no | it is on and worth ~2.5× — `ampdu=0` costs 61 % |
| Credit announcement policy | no | `credall` neutral, `rxsequpdthresh` worse at 6 and 12 |
| Firmware aggregate cap | no | `ampdu_release` reads 32, `ampdu_mpdu` is auto, ceiling is 11 |

Knobs measured and rejected: `txlazydelay`, `rxsequpdthresh`, `txglom` (0/8/16),
`credall`, `maxtxpktglom`, `acksizethsd`, `ampdu_tx_density`, `ampdu_mpdu`,
`txswqlen`.

## The one gain: `wifi_ampdu_rts=0`, +6.2 %

Interleaved A/B, n=6 against n=12, with the value read back out of the chip.
Not the default: RTS/CTS earns its keep where hidden nodes exist, and this is
one board against one AP.

## What is still open

**Aggregates run ~3 MPDUs deep where the firmware would allow 11.** Depth is
inferred from the RTS timing delta, not read directly — the per-aggregate
counter exists at `ampdu+0x250+0x0C` but no iovar exposes it, and the
`ampdu_txq_prof_*` iovars are compiled-out stubs returning `BCME_UNSUPPORTED`.

The firmware's cap chain (confirmed against the published Broadcom
`brcmsmac/ampdu.c`, same lineage) is:

```
release = min(ampdunummpdu, 11, ampdu_mpdu_if_set,
              max_ampdu_bytes / 1600,          /* max_ampdu_bytes = 8192 << AP's exponent */
              fifo_tb[BE].mcs2ampdu_table[FFPLD_MAX_MCS])
```

Two things in there are worth raising with Infineon independently of our
problem: the byte budget is divided by a **hard-coded 1600** rather than the
actual MPDU length, so smaller frames under-aggregate proportionally; and the
last term is the **ffpld** table, lowered permanently by TX-FIFO underflow with
no reset path.

ffpld is *not* what sets our steady-state level: it is a ratchet, and
throughput was seen to recover 30 % within one continuous boot.

## Site factors, and why they are hard to measure here

The AP advertises 802.11b basic rates (`1.0* 2.0* 5.5* 11.0*`), so every beacon
goes out at 1 Mbps — roughly 3.4 ms of airtime each, against ~0.2 ms at
24 Mbps — multiplied by 3–5 BSSIDs every 102.4 ms. That is a real airtime tax;
its size was **not** established, because throughput at this site varies ±45 %
within a single set of runs. A scan shows no neighbouring networks at all, so
the variance is other traffic on the same AP.

`iw dev wlan0 survey dump` returns nothing on this FullMAC part, so channel
occupancy cannot be read from the hardware.

## Measuring anything here

- Interleave legs and use ≥12 samples per arm. A block comparison 20 minutes
  apart produced a phantom +15 % that a proper interleave reduced to +1.6 %.
- Record the PHY `Link rate` on every run: the chip's rate control can sit at
  6.5 Mbit/s for minutes after a 3 dB signal change.
- Never trust an iovar SET without reading it back. Two silent failures were
  found this way — commands falling through the ioctl map to `WLC_GET_VERSION`,
  and a readback that ran before the SET.
- `build-all.sh` leaves a **debug** `kernel.img`, and a plain `build.sh rpi`
  afterwards returns success while relinking nothing. Only `build.sh rpi clean`
  produces the release image.

## Tooling this left behind

`wifi_test_iovars` in `Pi1MHz.cfg` sends up to four firmware iovars at bring-up
and reports slot 0 read back from the chip on `/status`, so a knob can be A/B'd
by editing the cfg and rebooting rather than rebuilding. An entry with no
`=value` is a read-only probe. Unset, nothing is sent and the join is
byte-identical.

`delta_stats` is sampled on demand (non-blocking) when armed with
`wifi_test_iovars=delta_stats_interval=1`, giving txframe / retransmits /
failures / `rxcrsglitch` on `/status`.

**Known limitation:** reading the 848-byte `statistics` iovar needs the control
payload limit raised above 164, and doing so **stops the chip associating** —
bisected, cause not found (making the frame buffer static did not fix it). That
blocks reading the ucode `txfunfl` counters, which would settle whether the MAC
is underrunning mid-aggregate.
