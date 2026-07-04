# OAI and USRP X310: Gain Mapping, Transmit Power, and Log Interpretation

This document details how OpenAirInterface (OAI) gain parameters map to the physical hardware of the USRP X310 (with UBX-160 daughterboards), how to calculate true physical transmit powers, and how to interpret the power-related metrics printed in OAI logs.

## 1. Gain Mapping (OAI to USRP Hardware)

OAI configures the USRP hardware gains through parameters in the gNB configuration file (under the `RUs` section) and command-line arguments for the UE.

### Transmit Gain (gNB `att_tx` and UE `--ue-txgain`)
In OAI, the transmit configuration parameter `att_tx` represents **attenuation** from the USRP's maximum hardware Tx gain. OAI passes `att_tx` to the USRP driver, which internally subtracts it from the hardware maximum:

```
OAI config:   att_tx = 6
USRP driver:  Actual Tx Gain = gain_range - att_tx = 31.5 - 6 = 25.5 dB
```

This is confirmed by the USRP driver log:
```
[HW] USRP TX_GAIN:25.50 gain_range:31.50 tx_gain:6.00
```

*   **Formula:** `Actual USRP Tx Gain = USRP_Max_Tx_Gain - att_tx`
*   **Example:** The USRP X310 (UBX daughterboard) has a maximum Tx gain of `31.5 dB`. With `att_tx = 6`, the hardware Tx gain is set to `25.5 dB`.

### Receive Gain (gNB `att_rx` and UE `--ue-rxgain`)
Similarly, `att_rx` is attenuation from the maximum Rx gain. Unlike the TX path (where the USRP driver does the subtraction), OAI performs the subtraction itself before passing to the driver. From `executables/nr-ru.c:646`:
```c
cfg->rx_gain[i] = ru->max_rxgain - ru->att_rx;  // OAI subtracts before passing to driver
```

*   **Formula (before calibration):** `cfg->rx_gain = max_rxgain - att_rx`
*   **Example:** If `max_rxgain = 115` and `att_rx = 6`, OAI computes `cfg->rx_gain = 109`. However, an additional **calibration offset** (77 dB for X310 at 3.5 GHz) is subtracted before the USRP hardware is set — see the [RX Gain Calibration Offset](#rx-gain-calibration-offset-the-77-for-x310) section below.
*   On the UE side, `--ue-rxgain 98` sets `cfg->rx_gain = 98` directly, but the same calibration offset still applies.

> [!NOTE]
> Both `att_tx` and `att_rx` are attenuation values (higher = quieter). The subtraction from max happens at different layers: the USRP driver handles TX, while OAI handles RX. The end result is the same.

### RX Gain Calibration Offset (the "77" for X310)

There is a **hidden intermediate step** between the OAI config and the actual USRP hardware gain. OAI maintains a per-device, per-frequency **RX gain calibration table** in `radio/USRP/usrp_lib.cpp:908`:

```c
rx_gain_calib_table_t calib_table_x310[] = {
  {3500000000.0, 77.0},   // 3.5 GHz → offset = 77 dB
  {2660000000.0, 81.0},
  {2300000000.0, 81.0},
  {1880000000.0, 82.0},
  {816000000.0,  85.0},
  {-1, 0}
};
```

At initialization, OAI looks up the closest frequency and subtracts this offset before setting the USRP hardware gain (`usrp_lib.cpp:1360`):

```c
double gain = cfg->rx_gain[i] - cfg->rx_gain_offset[i];
s->usrp->set_rx_gain(gain);
```

**The full RX gain chain for the gNB is therefore:**

```
Actual USRP RX Gain = (max_rxgain - att_rx) - rx_gain_offset
                    = (115 - 6) - 77
                    = 32 dB  ← what the USRP VGA is actually set to
```

**What does the 77 represent?** It is an empirical calibration factor that accounts for the X310's fixed-gain stages (antenna → PCB traces → front-end filters → LNA bias). These stages provide roughly 77 dB of "free" gain that is always present. When OAI says "I want 109 dB of total system gain," it subtracts the 77 dB of inherent fixed gain and asks the USRP's variable-gain amplifier (VGA) for only 32 dB.

**Calibration tables for other devices:**

| Device | Offset @ 3.5 GHz | Notes |
|---|---|---|
| X310 | 77.0 dB | Highest fixed-gain chain |
| B210 | 44.0 dB | Lower fixed gain |
| N310 | 0.0 dB | No calibration applied |
| X410 | 0.0 dB | No calibration applied |

> [!WARNING]
> For the **UE** side (`--ue-rxgain`), the same calibration offset is applied. If you pass `--ue-rxgain 98` on an X310, the actual USRP hardware gain will be `98 - 77 = 21 dB`, not 98. The 77 dB offset means OAI's gain values are "virtual" system gains, not raw hardware gains.

> [!NOTE]
> The TX path has **no equivalent calibration table**. The USRP driver subtracts `att_tx` from `gain_range.stop()` directly (`usrp_lib.cpp:1387`), with no frequency-dependent offset.

> [!NOTE]
> OAI's Automatic Gain Control (AGC) is disabled by default. Unless you pass `--agc` to the UE, the hardware Rx gain stays rigidly fixed at the value you configure, regardless of what `adjust_rxgain` the UE prints to the console.

## 2. Calculating True Physical Transmit Power

To determine how loud your radio actually transmits over the air, you must combine the OAI configuration with the physical hardware limits found in the Ettus UBX-160 datasheet.

### The Hardware Maximums
The absolute maximum physical output power of the UBX-160 is frequency-dependent. Based on the Ettus datasheet's "TX Frequency Response at Maximum Gain" graph:
*   At **3.6 GHz (Band 78)**, the hardware is capable of pushing a maximum Continuous Wave (CW) power of **+20 dBm**.
*   This physical ceiling occurs when OAI sets `att_tx = 0` (zero attenuation = maximum gain).

### The OFDM PAPR Backoff
5G NR uses OFDM, which consists of thousands of independent subcarriers. When these subcarriers temporarily align in phase, they create massive instantaneous voltage spikes. The difference between the normal "average" signal power and these massive spikes is the **Peak-to-Average Power Ratio (PAPR)**, which is typically **10 to 12 dB** for a 5G signal.

If you push the average power too high, these OFDM spikes will hit the USRP's +20 dBm ceiling and clip (causing severe signal distortion and BLER). Therefore, you must "back off" the average power to leave headroom for the spikes.

### The Power Calculation Example
If you use `att_tx = 0` (zero attenuation = max gain of 31.5 dB):
1.  **Peak Power:** Your signal peaks will hit the hardware ceiling of **+20 dBm**.
2.  **Average Power:** Because of the 12 dB PAPR, your average transmit power will be bounded at `20 - 12 = ` **+8 dBm**.

If you use `att_tx = 6` (6 dB attenuation → 25.5 dB gain):
1.  **Peak Power:** The ceiling drops by 6 dB to **+14 dBm**.
2.  **Average Power:** Your average transmit power drops to `14 - 12 = ` **+2 dBm**.

## 3. Interpreting OAI Log Metrics

One of the most confusing aspects of OAI is that it frequently logs signal power using an internal **Digital dB Scale**, not physical dBm. 

### The 90.3 dB Digital Ceiling
The USRP X310 has **14-bit ADCs** internally (effective range ±8192), but delivers samples to the host as **16-bit signed integers** (sc16 format, ranging from `-32768` to `+32767`). OAI treats these as full 16-bit values:
*   The maximum possible sample amplitude is `32767`.
*   The maximum possible sample energy is $32767^2 \approx 10^9$.
*   In decibels, $10 \times \log_{10}(10^9) = \mathbf{90.3 \text{ dB}}$.

OAI mathematically anchors its internal calculations to this `90.3 dB` ceiling (defined as `SQ15_SQUARED_NORM_FACTOR_DB`). Any raw metric logged in OAI that approaches ~90 dB means the digital signal is physically clipping the ADC.

> [!NOTE]
> Because the physical ADC is 14-bit, the true dynamic range is ~84 dB (6.02 × 14), not 90.3 dB. The bottom ~2 bits of the 16-bit representation are zero-padded by the USRP hardware.

### Initial Sync Logs (`dB/RE`)
When the UE first boots up and searches for the cell, you will see logs like:
`pbch rx ok. rsrp:47 dB/RE`

*   This `47` is **not dBm**. It is a raw measurement on the digital scale (relative to the 90.3 dB maximum). 
*   It means the digital energy of the PBCH resource elements is `47 dB`.

### Connected Mode RSRP (`dBm`)
Once the UE connects, the MAC and RRC layers require the UE to report its RSRP in standard physical **dBm** (e.g., `-54 dBm`).
*   To do this, OAI uses a conversion math equation. It takes the raw digital energy (e.g., `47 dB/RE`), factors in the 90.3 dB anchor, and **explicitly subtracts your configured `--ue-rxgain`**.
*   This is why the connected-mode RSRP looks accurate (e.g., `-54 dBm`) regardless of whether you set your Rx Gain to 110 or 98; the math artificially reconstructs the physical antenna voltage by canceling out your software gain.

### RACH Metrics (gNB Side)
When the UE attempts to connect, the gNB logs the incoming Random Access (RACH) attempt:
`Initiating RA procedure with preamble 59, energy 54.0 dB (I0 99, thres 120)`

*   **`energy 54.0 dB`**: The gNB detected the UE's preamble with a digital energy score of `540` (OAI prints the value divided by 10).
*   **`I0 99`**: The IIR-filtered noise floor estimate on the PRACH resources, continuously updated as a running exponential average.
*   **`thres 120`**: The configured `prach_dtx_threshold` (default: 150 in OAI).

**Detection condition** (from `nr_prach_procedures.c:54`):
```c
res.max_preamble_energy > gNB->measurements.prach_I0 + gNB->prach_thres
// i.e., 540 > 99 + 120 = 219 → DETECTED
```
The preamble energy must exceed `I0 + thres` (not just `thres` alone). Here, `540 > 219`, so the gNB successfully detects the RACH attempt.

---

## 4. AGC: Behavior, Caveats, and the 50 dB Target

### AGC is a One-Shot During Initial Sync

OAI's `--agc` flag enables gain adjustment, but it only changes the **hardware** gain in two scenarios:

1. **Sync failure:** If initial cell search fails, AGC bumps gain by 3 dB (`INCREASE_IN_RXGAIN`) and retries.
2. **Sync success:** After PBCH decode, AGC adjusts gain once to bring the signal to `TARGET_RX_POWER = 50 dB`.

After this initial adjustment, the hardware gain is **locked for the duration of the connection**.

### Connected-Mode AGC Only Touches a Software Variable

In connected mode, `phy_adjust_gain_nr()` runs on every slot 2 (in `phy_procedures_nr_ue.c:430`), but it only modifies `ue->rx_total_gain_dB` — an internal software variable used for power calculations. It **never calls** `trx_set_gains_func()` to change the actual USRP hardware gain. This means:

- If channel conditions change after initial sync (e.g., UE moves, interference appears), the UE **cannot adapt** its hardware gain.
- The UE is stuck at whatever gain was set during initial sync until it loses sync and re-acquires.

### Why 50 dB?

The AGC target of `TARGET_RX_POWER = 50 dB` is a midpoint on the 90.3 dB digital scale:
- **Headroom above:** `90.3 - 50 = 40.3 dB` before clipping. OFDM PAPR peaks of 10-12 dB are safely contained.
- **Headroom below:** `50 dB` above the quantization floor. This provides ~44 dB of effective digital SNR.

Raising the target (e.g., to 60 dB) would improve SNR slightly but risk clipping on PAPR peaks. Lowering it would waste ADC resolution.

### Energy Efficiency is Not a Concern

On a USRP (or any radio receiver), the LNA/mixer chain draws roughly **constant power regardless of gain setting**. The LNA is always powered on and biased at the same operating point — gain control works by adjusting feedback networks or attenuator paths, not by "working harder." There is no energy cost difference between targeting 30 dB vs 50 dB.

---

## 5. gNB Receiver Configuration

### The gNB Has No Receiver AGC

The gNB's Rx gain is **static**, set once at startup via `max_rxgain - att_rx`. There is no dynamic gain adjustment during operation. This is intentional — the gNB relies on **uplink power control** (TPC commands) to keep received UE signals in the right range. A gNB AGC would change the noise floor, breaking the `prach_I0` measurement that PRACH detection depends on.

### Why `att_rx` Exists on the gNB

Even though the gNB controls UE transmit power via TPC, `att_rx` is needed for several scenarios:

| Scenario | Why `att_rx` matters |
|---|---|
| **Lab/testbed** | UE is physically inches from the gNB. Even at minimum UE Tx power, the signal can saturate the gNB ADC. `att_rx` prevents clipping. |
| **PRACH (initial access)** | During initial access, the gNB has *no* power control over the UE yet. The first preamble is transmitted at whatever the UE decides (open-loop). A nearby UE can send an extremely loud preamble. |
| **Multi-UE** | Near and far UEs transmit simultaneously. Power control keeps them roughly balanced, but combined signals can clip. |

### gNB Transmit Power is Fixed

The gNB transmits at a **fixed power** set by `att_tx` in the config. There is no per-slot power adaptation. `att_tx` is loaded once at startup and passed to the USRP. For maximum coverage in a testbed, minimize `att_tx` (= maximize gain).

### Key RACH Configuration Parameters

These are in the gNB config file and directly affect coverage and initial access success:

| Parameter | Your Value | Default | Effect |
|---|---|---|---|
| `preambleReceivedTargetPower` | -96 dBm | -118 dBm | Target power level the gNB wants to receive preambles at. Lower = UE transmits louder. |
| `powerRampingStep` | 1 (2 dB) | — | How much UE increases Tx power per failed RACH attempt. |
| `preambleTransMax` | 6 (10 attempts) | — | Max RACH attempts before giving up. |
| `p0_NominalWithGrant` | -90 dBm | — | Target UE Rx power at gNB for PUSCH data. |
| `prach_dtx_threshold` | (L1 config) | 150 | How far above I0 a preamble must be for detection. |
| `msg3_DeltaPreamble` | 1 | — | Power offset between preamble and Msg3. |

> [!TIP]
> Your `preambleReceivedTargetPower = -96` is 22 dB higher than the default (-118). This tells the UE that the gNB expects a relatively strong preamble — causing the UE to compute a *lower* open-loop Tx power. For extended range, consider lowering this (e.g., -110), which makes the UE transmit louder.

---

## 6. Practical Recommendations for Testbed Operation

### Manual Gain vs AGC

For a static testbed (fixed gNB-UE placement), **manual gain is more predictable**:

| Aspect | Manual (`--ue-rxgain 98`) | AGC (`--agc --ue-rxgain 75`) |
|---|---|---|
| Sync | Works (with CFAR patch) | Works, then adjusts gain once |
| Connected mode | Fixed at 98 | Fixed at whatever AGC set |
| Channel changes | Can't adapt | Can't adapt either |
| Predictability | Known gain value | Depends on initial RSRP measurement |

Since AGC is a one-shot anyway, use it only when you don't know the right gain for a new environment. For a known setup, set the gain directly.

### Finding the Right Gain

Start high and lower until performance degrades:
```bash
# Confirm it works at high gain
nr-uesoftmodem --ue-rxgain 110 ...

# Lower incrementally
nr-uesoftmodem --ue-rxgain 98 ...
```

Pick the lowest gain that gives clean RACH (<3 attempts) and acceptable DL BLER (<10%). From testing:
- **98-100 dB:** Clean sync, stable connection, good BLER.
- **75 dB:** Marginal — RACH struggles, high DTX, ~20% BLER (PDCCH-limited, not sync-limited after CFAR patch).
- **70 dB:** Cell found but cannot maintain RACH.

### Maximizing Coverage

| Knob | Action | Effect |
|---|---|---|
| gNB `att_tx` | Minimize (e.g., 0) | Maximize gNB transmit power |
| UE `--ue-rxgain` | Maximize (e.g., 110) | Maximize UE receive sensitivity |
| `preambleReceivedTargetPower` | Lower (e.g., -110) | UE transmits louder preambles |
| `p0_NominalWithGrant` | Lower (e.g., -100) | UE transmits louder PUSCH data |
| UE `--agc` | Enable | Auto-adjusts gain after sync |
