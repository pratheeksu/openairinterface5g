# OAI 5G NR-UE: Complete Troubleshooting & CFAR Implementation Documentation

**Date:** May 2026  
**Subject:** USRP X310 Gain Mapping, SSS Detection Bug, and CFAR Fix  
**Setup:** gNB + UE on USRP X310 (UBX-160 daughterboards), Band 78, 3619.2 MHz, 106 PRB, 5G SA

---

## Part 1: Understanding OAI Gain Parameters and the USRP X310

### 1.1 The Starting Point

The investigation began with a need to understand how the gain parameters in OAI software map to the physical hardware of the USRP X310. The active configuration file was `gnb.sa.band78.fr1.106PRB.usrpb210.conf`, which uses the `RUs` section to configure radio unit hardware.

### 1.2 gNB Gain Mapping

On the gNB side, gains are configured via `att_tx`, `att_rx`, and `max_rxgain` in the `RUs` block.

**Transmit Side (`att_tx`)**
OAI treats `att_tx` as **attenuation**, not raw gain. It is subtracted from the USRP's maximum hardware Tx gain capability:

```
Actual USRP Tx Gain = USRP_Max_Tx_Gain - att_tx
```

The UBX-160 daughterboard's software-addressable maximum internal Tx gain is **31.5 dB**. With `att_tx = 6`, the effective hardware gain is `25.5 dB`.

**Receive Side (`att_rx` / `max_rxgain`)**
Similarly, the Rx gain is subtracted from the maximum hardware capability:

```
Actual USRP Rx Gain = USRP_Max_Rx_Gain - att_rx
```

The UBX-160 supports a maximum Rx gain of up to ~115 dB. With `att_rx = 6`, the hardware Rx gain is set to ~109 dB.

### 1.3 UE Gain Mapping

On the UE side, the `--ue-rxgain <value>` command-line flag directly sets the hardware Rx gain in dB. There is no inversion or attenuation calculation.

**OAI AGC is Disabled by Default**

OAI's Automatic Gain Control (AGC) is inactive unless you explicitly pass `--agc` at startup. Without it:
- OAI calculates an ideal gain offset based on measured RSRP and prints `adjust_rxgain: X dB` to the console.
- The hardware gain is **NOT changed** — it is purely advisory.
- When `--agc` is active, OAI adjusts the hardware gain but only after PBCH decoding (post-sync). It cannot help during the initial blind cell search.

The AGC target is hardcoded at:
```c
#define TARGET_RX_POWER     50  // Target digital power (dB), value found by simulation
#define TARGET_RX_POWER_MAX 65  // Upper limit before clipping risk
#define TARGET_RX_POWER_MIN 35  // Lower limit before noise floor risk
```
Notably, `TARGET_RX_POWER_MAX` and `TARGET_RX_POWER_MIN` are **never used** in the OAI codebase — they are ghost macros defined but never enforced.

---

## Part 2: Physical Transmit Power and the PAPR Problem

### 2.1 Hardware Maximum from the Datasheet

The Ettus UBX-160 datasheet "TX Frequency Response at Maximum Gain" graph shows the maximum Continuous Wave (CW) output power across frequency. At **3.6 GHz (Band 78)**, the hardware is capable of **+20 dBm** peak CW output at maximum gain.

### 2.2 Why 5G Signals Must Back Off from the CW Maximum

A 5G NR signal with 106 PRBs contains 1,272 active OFDM subcarriers. Each subcarrier is independently modulated. By random statistical chance, at certain moments many subcarriers align in phase and add constructively, creating a brief, massive voltage spike. This is the **Peak-to-Average Power Ratio (PAPR)**, which is typically **10–12 dB** for a standard 5G NR signal.

**Your intuition was exactly right:**
> *"If 20 dBm is the max power the X310 supports, and the average power is 20 dBm, then the OFDM peaks would be much higher and would clip. So if the peaks have to be at 20 dBm, the average has to be ~12 dB lower."*

This is the correct understanding. The USRP cannot output more than +20 dBm instantaneously. If you set the average power to +20 dBm, the 12 dB PAPR spikes would require +32 dBm peaks, which is physically impossible — those peaks get **clipped**. Clipping truncates the waveform into a square wave, producing massive spectral splatter and corrupting the signal.

### 2.3 Practical Power Budget

| Parameter | Value |
|---|---|
| Hardware CW ceiling (3.6 GHz) | +20 dBm |
| 5G NR OFDM PAPR | ~12 dB |
| Maximum safe average transmit power | **+8 dBm** |
| With `att_tx = 6` dB applied | **+2 dBm** |

---

## Part 3: Interpreting OAI Log Metrics

### 3.1 The 90.3 dB Digital Scale

The USRP X310 has **14-bit ADCs** internally (effective range ±8192), but delivers samples to the host as **16-bit signed integers** (sc16 format, range: -32768 to +32767). OAI treats these as full 16-bit values, setting the absolute digital ceiling:

$$\text{Max Energy} = 32767^2 \approx 10^9$$
$$10 \times \log_{10}(10^9) = \mathbf{90.3 \text{ dB}}$$

OAI hardcodes this as `SQ15_SQUARED_NORM_FACTOR_DB = 90.3089986992` in `common/utils/nr/nr_common.h`.

**This creates two different "dB" scales that appear in OAI logs:**

| Scale | Example Log | Meaning |
|---|---|---|
| Digital dB (raw ADC) | `rsrp: 47 dB/RE` (during initial sync) | Raw energy relative to the 90.3 dB maximum |
| Physical dBm | `RSRP: -54 dBm` (during connected mode) | Calibrated physical power at the antenna |

### 3.2 Connected Mode RSRP Conversion

When connected, OAI converts the digital energy to physical dBm by explicitly subtracting the configured `--ue-rxgain`. In `openair1/PHY/NR_UE_ESTIMATION/nr_ue_measurements.c`:

```c
double rx_gain = openair0_cfg[ue->rf_map.card].rx_gain[0];
double rx_gain_offset = openair0_cfg[ue->rf_map.card].rx_gain_offset[0];
// ...
const int nf_usrp = ... - ((int)rx_gain - (int)rx_gain_offset) - ...
```

This is why the UE correctly reports `-54 dBm` regardless of whether `--ue-rxgain` is 70 or 110. The math subtracts your hardware gain to reconstruct the true physical voltage at the antenna.

### 3.3 PSS Correlation Metrics

During cell search, OAI logs:
```
Peak found at pos 3472, val = 3177988313 (90 dB power over signal avg 61 dB)
```

- **Peak (90 dB):** The squared correlation value at the timing offset matching the PSS. This can exceed the 90.3 dB single-sample ceiling because it is the **sum** of many correlated samples.
- **Average (61 dB):** The background noise floor — the mean energy over the rest of the frame where no PSS is present.
- **Ratio (29 dB):** This is your PSS SNR. OAI uses a dynamic threshold of ~5–7 dB ratio to declare a PSS valid.

A 29 dB ratio (as observed at Rx Gain 98) is an excellent PSS signal that passes detection comfortably.

### 3.4 RACH Metrics (gNB Side)

```
[NR_PHY] Initiating RA procedure with preamble 59, energy 54.0 dB (I0 99, thres 120)
```

- **`energy 54.0 dB`**: OAI divides the raw preamble energy by 10 before printing. Actual score = `540`.
- **`I0 99`**: The IIR-filtered interference/noise floor on the PRACH resource, continuously updated.
- **`thres 120`**: The configured `prach_dtx_threshold` in the gNB config.
- **Detection condition** (from `nr_prach_procedures.c`): `energy > I0 + thres`, i.e., `540 > 99 + 120 = 219` → RACH preamble detected successfully.

**Power Headroom (PH):**
```
[NR_MAC] UE rnti ... Power Headroom 48 dB
```
This is `P_CMAX - P_PUSCH`. A value of 48 dB means the UE is transmitting at 48 dB below its maximum capability, confirming it has enormous uplink power reserve.

---

## Part 4: Discovering the SSS Bug

### 4.1 The Observation

At Rx Gain 110, the UE connected perfectly every time. At Rx Gain 98, the UE intermittently failed to connect — even when the PSS correlation showed an excellent 23 dB SNR. This made no physical sense.

### 4.2 The Debug Log Breakthrough

Enabling PHY debug logs (`--log_config.phy_log_level debug`) revealed the exact failure point:

```
[PHY] Peak found at pos 3472 (90 dB power over signal avg 61 dB)  ← PSS PASSED
[PHY] Failed to detect SSS after PSS, metric of SSS 28756, threshold to consider SSS valid 30000  ← SSS FAILED
```

**The PSS passed. The SSS failed.** Not because the signal was bad, but because the SSS used a completely different gating mechanism: a **hardcoded absolute integer floor**.

### 4.3 The SSS Floor Constant

In `openair1/PHY/NR_REFSIG/sss_nr.h`:
```c
#define SSS_METRIC_FLOOR_NR (30000)
```

And in `sss_nr.c`, the detection gate:
```c
if (tot_metric > SSS_METRIC_FLOOR_NR) {
    // Accept this SSS candidate
}
```

The SSS metric is a **coherent cross-correlation** (voltage domain, not power domain). It scales linearly with the incoming signal amplitude. At Rx Gain 110, the metric easily exceeded 30,000. At Rx Gain 98, the lower signal amplitude caused the metric to produce `28,756` — just below the arbitrary gate — even though the signal was objectively pristine.

This is in stark contrast to the PSS detection, which uses a **dynamic threshold** proportional to the measured noise floor (a proper CFAR approach).

### 4.4 The 8-Year Git Forensic Trail

A `git blame` and `git log -p` trace revealed a remarkable history:

**May 30, 2018 — Original commit by Hongzhi Wang:**
```c
#ifdef DEBUG_SSS_NR
#define SSS_METRIC_FLOOR_NR (30000)
if (*tot_metric > SSS_METRIC_FLOOR_NR) {
    // gate
}
#endif
```
At this point it was purely a debug diagnostic, disabled in production builds.

**June 5, 2018 — "UE nr parms init bug fix" commit (same author):**
```diff
-#ifdef DEBUG_SSS_NR
+//#ifdef DEBUG_SSS_NR

 #define SSS_METRIC_FLOOR_NR (30000)
 if (*tot_metric > SSS_METRIC_FLOOR_NR) {
```
By commenting out `#ifdef` with `//`, the entire block was immediately activated for all production builds. This was almost certainly an accidental "quick hack" that was never caught.

**March 2023:** A refactoring commit (Sidelink work) moved `SSS_METRIC_FLOOR_NR` cleanly into `sss_nr.h`, cementing its permanent production status — nearly 5 years after the original accidental promotion.

**The constant has sat untouched for ~8 years.**

### 4.5 Why the Two Signals Scale Differently

A common question was: "Why is the SSS metric lower when the PSS is so good?"

The two signals are measured in fundamentally different mathematical domains:

| Signal | Detection Method | Domain | Sensitivity to Amplitude |
|---|---|---|---|
| PSS | Non-coherent, blind correlation | **Power** (amplitude²) | Quadratic — drops by 4x when amplitude halves |
| SSS | Coherent, channel-compensated correlation | **Voltage** (amplitude) | Linear — drops by 2x when amplitude halves |

**PSS is non-coherent** because when the UE first wakes up, it has no knowledge of the signal's phase. It must use squared magnitudes to eliminate the unknown phase.

**SSS is coherent** because by the time the SSS is checked, the PSS has already established both timing and a channel estimate. The channel estimate removes the phase ambiguity, allowing a direct signed dot product (voltage domain correlation), which is both more sensitive and more efficient.

The absolute scales of the two metrics are therefore not comparable. The PSS "peak" in dB and the SSS metric in raw integer units measure fundamentally different things.

---

## Part 5: The CFAR Mathematical Solution

### 5.1 Design Goals

The replacement algorithm must be:
1. **Gain-invariant:** The detection decision cannot depend on the absolute amplitude of the signal.
2. **Robust against noise:** Should not accept genuine noise as a valid SSS.
3. **Coherent:** Must stay in the voltage domain to match the existing SSS correlator structure.

### 5.2 The Mathematical Derivation (Cauchy-Schwarz)

The SSS correlation metric for a given phase hypothesis `φ` and candidate `Nid1` is:
```
M = Σᵢ d[i] · Re{rot · sss[i]}
```
where `d[i]` are the 127 channel-compensated received samples and `sss[i]` are the ±1 reference symbols.

The Cauchy-Schwarz inequality gives the theoretical maximum:
```
M² ≤ (Σᵢ d[i]²) · (Σᵢ sss[i]²) = E_sss · 127
```

where `E_sss = Σᵢ d[i]²` is the total energy of the received SSS samples.

**The key insight:** Both `M` and `E_sss` contain the same signal amplitude factor `A`. When we form the ratio `M² / (127 · E_sss)`, the amplitude cancels identically:

```
A²·m² / (127 · A²·e) = m²/(127·e)   ← amplitude A² cancels out
```

This ratio is **gain-invariant** by construction. It equals 1.0 for a perfect match and approaches 0 for pure noise.

### 5.3 The Implemented Threshold (α = 0.25)

Requiring the metric to achieve at least 50% of its theoretical maximum correlation:
```
M/√(127 · E_sss) ≥ 0.5
M² ≥ 0.25 · 127 · E_sss
```

This is the implemented CFAR check (from `sss_nr.c`):
```c
// CFAR: M² >= (ALPHA_NUM/ALPHA_DEN) * LENGTH_SSS_NR * sss_energy
const int64_t metric_sq = (int64_t)(*tot_metric) * (*tot_metric);
const int64_t cfar_threshold = SSS_CFAR_ALPHA_NUM * (int64_t)LENGTH_SSS_NR * sss_energy 
                               / SSS_CFAR_ALPHA_DEN;
if (metric_sq >= cfar_threshold) {
    // Accept SSS candidate
}
```
Note: The multiplication is done before the division (`NUM * 127 * E / DEN`) to avoid premature integer truncation.

**Why α = 0.25 (50% correlation)?**
- At α = 0.25, a 50% match is required. This tolerates moderate channel estimation errors while maintaining strong noise rejection.
- For random noise, the expected ratio is `1/127 ≈ 0.008`, meaning noise will almost never pass with a 25% threshold.
- The 24-bit CRC on the PBCH (`~1 in 16 million` false positive rate) acts as the final backstop.

### 5.4 Overflow Safety Analysis

With 16-bit IQ samples and 127 symbols:

| Quantity | Maximum Value | Fits in int64_t? |
|---|---|---|
| `E_sss` (max) | 127 × 32767² ≈ 1.36 × 10¹¹ | ✅ Yes (max 9.2 × 10¹⁸) |
| `metric_sq` (max) | (127 × 32767)² ≈ 1.74 × 10¹³ | ✅ Yes |
| `energy_thr` (max) | (127/4) × 1.36 × 10¹¹ ≈ 4.3 × 10¹² | ✅ Yes |

Using `int64_t` for both sides of the comparison is safe with significant headroom.

---

## Part 6: Post-Fix Testing and Log Analysis

### 6.1 Observed Behavior After CFAR Patch

After implementing the CFAR fix:
- At **Rx Gain 70:** UE found and decoded the cell (SSS and PBCH passed), but could not maintain the RACH procedure.
- At **Rx Gain 75:** UE eventually connected to the gNB, but exhibited high Uplink BLER and persistent RACH loops.

The CFAR patch worked correctly. However, it exposed the next layer of system limitations.

### 6.2 The True Cause of Uplink BLER: DTX, Not Power Control

Initial analysis suggested that low Rx Gain caused the RSRP measurement to be inaccurate, corrupting the UE's open-loop uplink power control calculation. However, this was refuted by a third-party review of the raw log data.

**The data contradicts the power control hypothesis:**
- **UL SNR: 32.2 dB** — When the UE *did* transmit, the gNB received it perfectly cleanly.
- **Power Headroom: 48 dB** — The UE was nowhere near its power ceiling.
- **`ulsch_DTX`: 123 → 196 → 272 → 353** — The dominant metric. DTX means the gNB scheduled uplink slots but the UE **never transmitted at all**.

**The actual failure mode is PDCCH failure:**
At Rx Gain 70–75, the signal is too weak for the UE to reliably decode the **Physical Downlink Control Channel (PDCCH)**. The PDCCH carries the DCI (Downlink Control Information) uplink scheduling grants. When the UE misses a DCI grant, it does not know it was supposed to transmit, so it stays silent. The gNB expects an uplink packet, receives nothing, logs a DTX, and counts it as an error in the BLER statistics.

The power control explanation was a narrative built on plausible physics, but the raw integer data (excellent UL SNR on received transmissions, massive DTX count) clearly points to PDCCH decoding failure as the root cause.

### 6.3 ADC Quantization at Extreme Low Gain

At `rsrp: 27 dB/RE` (Rx Gain 75), the average signal amplitude is approximately:
$$\sqrt{10^{27/10}} \approx 22 \text{ ADC counts out of ±8192 (14-bit X310 ADC)}$$

This corresponds to roughly **4–5 effective bits** of resolution. While this sounds severe, it is still sufficient to decode QPSK modulations (PBCH, MCS 0), as the successful PBCH decode proves. The PSS correlation ratio (Peak 72 dB, Average 60 dB → ratio 12 dB) also remained healthy, demonstrating that both signal and noise dropped proportionally when gain was reduced — consistent with the ADC still operating in its linear region.

### 6.4 RACH Energy Sawtooth Pattern

The gNB logs showed the incoming RACH energy ramping up in steps (38 dB → 40.3 dB → 43.2 dB → 44.2 dB), then suddenly dropping back to 38 dB before the pattern repeated.

**Explanation (confirmed by `nr_ue_power_procedures.c`):**

OAI implements standard 3GPP power ramping in `NR_PRACH_RESOURCES_t`:
```c
float DELTA_P_rampup = (prach_res->preamble_power_ramping_cnt - 1)
                       * prach_res->preamble_power_ramping_step;
```

Each failed RACH attempt increments `preamble_power_ramping_cnt`, stepping up the transmit power. When the MAC layer exhausts its maximum retry count, it abandons the RACH procedure entirely and restarts. Upon restart, `preamble_power_ramping_cnt` resets to 1:

```
DELTA_P_rampup = (1 - 1) * step = 0
```

The energy drops back to the initial PRACH target power, creating the "sawtooth" pattern observed in the gNB logs.

The brief I0 (noise floor) jump in the gNB logs after each energy drop is explained by the IIR filter absorbing the outgoing PRACH preamble energy into the noise floor estimate momentarily.

---

## Part 7: Expert Review and Final Assessment

A third-party expert review of the post-fix analysis identified several corrections:

| Claim | Assessment |
|---|---|
| UL BLER caused by bad power control | ❌ **Wrong.** DTX data proves the UE wasn't transmitting at all — PDCCH failure is the correct root cause. |
| USRP has 16-bit ADCs | ❌ **Wrong.** The X310 has 14-bit ADCs (±8192 range). |
| "2–3 effective bits" at low gain | ❌ **Overstated.** At 27 dB/RE, effective resolution is ~4–5 bits. |
| PBCH decoded = quantization is tolerable | ✅ **Correct.** Successful PBCH proves the signal was intact. |
| PSS ratio collapse as evidence of ADC floor | ❌ **Contradicted by data.** The 12 dB Peak-to-Average ratio is healthy; both signal and noise dropped proportionally. |
| RSRP calibration uses rx_gain subtraction | ✅ **Directionally correct.** |
| RACH power sawtooth = procedure reset | ✅ **Correct.** |

**Final Conclusion from the expert review:**
> "The alternative analysis has a narrative problem: it builds a story around quantization noise being catastrophic, but the actual data (32.2 dB UL SNR, successful PBCH, preserved PSS peak-to-average ratio) contradicts that narrative at every turn."

---

## Summary: The Complete Chain of Discovery

```
Start: UE fails to connect at Rx Gain < 110
  │
  ├─► PHY Debug Logs → SSS metric 28,756 < threshold 30,000
  │
  ├─► Git Forensics → 8-year-old "debug code accidentally promoted to production" in 2018
  │
  ├─► CFAR Math (Cauchy-Schwarz) → Gain-invariant normalized check
  │         M² ≥ (1/4) · 127 · E_sss
  │
  ├─► CFAR Patch Applied → UE connects at Rx Gain 70/75
  │
  ├─► Post-fix Observations → High UL BLER, RACH loops
  │
  ├─► Log Analysis (1st pass) → Blamed power control (WRONG)
  │
  ├─► Expert Review → DTX is the smoking gun → PDCCH failure at low SNR
  │
  └─► Final Understanding:
        - CFAR patch solved the synchronization bottleneck (SSS gate)
        - The new bottleneck is PDCCH decoding at Rx Gain 70–75
        - System is "signal-limited", not "threshold-limited" — the patch worked correctly
```
