# Key Performance Indicators for the OpenAirInterface Code Base

This document summarizes the main throughput KPIs 

## 1. `nr-softmodem` Performance in `oai-gNB` and `oai-gNB-du` Modes for FR1 bands

### Test Profile

The following results apply to the TDD configuration below:

| Parameter | Value         |
|-----------|---------------|
| Band      | `n78` / `n77` |
| SCS       | `30 kHz`      |

### 1.1 20 MHz Bandwidth (`51 PRBs`)

#### Downlink

Configuration: `DDDSU`, mixed slot `6D4U`

| Layers | DL Throughput (Mbps) |
|--------|----------------------|
| 1      | 72                   |
| 2      | 143                  |
| 4      | 258                  |

#### Uplink

Configuration: `DDSUU`, mixed slot `XX`

| Layers | UL Throughput (Mbps) |
|--------|----------------------|
| 1      | 39                   |
| 2      | 65                   |

### 1.2 40 MHz Bandwidth (`106 PRBs`)

#### Downlink

Configuration: `DDDSU`, mixed slot `6D4U`

| Layers | DL Throughput (Mbps) |
|--------|----------------------|
| 1      | 152                  |
| 2      | 304                  |
| 4      | 550                  |

#### Uplink

Configuration: `DDSUU`, mixed slot `XX`

| Layers | UL Throughput (Mbps) |
|--------|----------------------|
| 1      | 81                   |
| 2      | 154                  |

### 1.3 60 MHz Bandwidth (`162 PRBs`)

#### Downlink

Configuration: `2.5 ms DDDSU`, mixed slot `6D4U`

| Layers | DL Throughput (Mbps) |
|--------|----------------------|
| 1      | 233                  |
| 2      | 466                  |
| 4      | 730                  |

#### Uplink

Configuration: `DDSUU`, mixed slot `XX`

| Layers | UL Throughput (Mbps) |
|--------|----------------------|
| 1      | 123                  |
| 2      | 175                  |

### 1.4 80 MHz Bandwidth (`217 PRBs`)

#### Downlink

Configuration: `2.5 ms DDDSU`, mixed slot `6D4U`

| Layers | DL Throughput (Mbps) |
|--------|----------------------|
| 1      | 310                  |
| 2      | 622                  |

#### Uplink

Configuration: `DDSUU`, mixed slot `XX`

| Layers | UL Throughput (Mbps) |
|--------|----------------------|
| 1      | 80                   |
| 2      | 140                  |

### 1.5 100 MHz Bandwidth (`273 PRBs`)

#### Downlink

Configuration: `2.5 ms DDDSU`, mixed slot `6D4U`

| Layers | DL Throughput (Mbps) |
|--------|----------------------|
| 1      | Not available        |
| 2      | Not available        |
| 4      | 1400                 |

#### Uplink

Configuration: `2.5 ms DDDSU`, mixed slot `6D4U`

| Layers | UL Throughput (Mbps) |
|--------|----------------------|
| 1      | 100                  |
| 2      | 170                  |

## 1. `nr-softmodem` Performance in `oai-gNB` and `oai-gNB-du` Modes for FR2 bands

### Test Profile

The following results apply to the TDD configuration below:

| Parameter | Value         |
|-----------|---------------|
| Band      | `257`         |
| SCS       | `120 kHz`     |

### 1.1 200 MHz Bandwidth (`132 PRBs`)

O-RU: MicroAmp

#### Downlink

Configuration: `DDDSU`, 0.625ms, Mix slot 10D2U

|Layers|DL(Mbps)|
|------|--------|
|1     |500     |
|2     |890     |

#### Uplink

Configuration: `DDDSU`, 0.625ms, Mix slot 10D2U

|Layers|UL (Mbps)|
|------|---------|
|1     |86       |

### RTT

With `ulsch_max_frame_inactivity= 0;`

Measured using ping: 4.526 ms

## 2. Performance Metrics for OAI Block Tests

### 2.1 Physical Simulators (`physims`)

For execution details, see [physical-simulators.md](./physical-simulators.md).

Results:


### 2.2 `phytest`

Needs to update

## 3. `nr-uesoftmodem`

### Test Profile

The following results apply to the TDD configuration below:

| Parameter | Value         |
|-----------|---------------|
| Band      | `n78` / `n77` |
| SCS       | `30 kHz`      |

The UE was in SISO mode.

Testbed Architecture: 

UE <--> Over the Air 1.5m to 2m distance <--> USRP/RU <--> gNB/DU server

| Platform    | UE-Radio  | Bandwidth | DL Throughput | UL Throughput |
| ----------- | --------- | --------- | ------------- | ------------- |
| Jetson Orin | B210      | 10 MHz    | 12 Mbps       | 7.5 Mbps      |
| Jetson Orin | B210      | 20 MHz    | 20 Mbps       | 9.5 Mbps      |
| Jetson Orin | B210      | 30 MHz    | 61 Mbps       | 33 Mbps       |
| Jetson Orin | B210      | 40 MHz    | 69 Mbps       | 46 Mbps       |
| DGX Spark   | B210      | 40 MHz    | 86 Mbps       | 46 Mbps       |
| DGX Spark   | N310/x410 | 100 MHz   | 231 Mbps      | 118 Mbps      |
