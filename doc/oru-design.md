# O-RU solution design

## Overview

OAI O-RU shall implement a set of O-RU CAT-A and CAT-B on COTS hardware.

## Procedure requirements for CAT-A

The procedures executed by the O-RU are defined in O-RAN.WG4.TS.CUS Section 4.2.1

### DL procedures

For downlink, the O-RU processing chain is as follows:

1. Receive frequency domain IQ data from DU
2. Symbol rotation (if configured)
3. DFT
4. Transmission

### PRACH procedures

TBD

### PUSCH procedures

TBD

### SRS procedures

TBD

## Procedure requirements for CAT-B

TBD

## Architecture

O-RU shall have two main threads are `north_read` and `south_read`. The threads shall read IQ data
from the respective interfaces. The north interface is handled by xran and is a frequency domain
interface The south interface is a split8 time domain device.

There are two processing triggers in the O-RU process:

 - `oran_tx_read` - used to indicate to the application that the downlink UP data is available in
xran buffers. Triggers DL FH processing and transmission.
 - `trx_read_func` - indicates to the application that samples are available to on the south. Triggers
PRACH, SRS and PUSCH FH processing.

## North interface

The north interface shall be handles by using OAI split 7.2 library, which uses xran library.

### Changes to xran adapter

The OAI xran adapter library has to be modified to adapt ot for usage with the O-RU. The existing
function signatures which are used by the O-DU do not fulfill the processing time requirements of
O-RU.

The proposed solution is to add a new set of sub-slot APIs to OAI xran adapter. This would ensure
on-time transmission and delivery of frequency-domain IQ to xran library buffers.

## Timing requirements

The timing requirements on O-RU processing come from the ORAN specification O-RAN.WG4.TS.CUS
More directly the standard is implemented by the xran library used in OAI split7.2 adapter 
(radio/fhi_72) which shall be used by O-RU.

### Timing requirements for DL

The main requirement for DL comes in the form of a timer: T1a_up min, max. The minimum value is
the downlink UP reception window closing time. This value is given in uS before OTA. Therefore
the downlink procedures must complete within this minimum timer value.

An additional requirement comes from the time domain split8 device. This requirement is due to 
the device requiring the samples to be provided before transmission.

### Timing requirements for PRACH/PUSCH/SRS

TBD

## Synchronization

xran library provides the main timing callback in the form of downlink UP reception window closing time.
This  infromation is received in `north_read` thread and should be communicated to `south_read` thread
in order to determine slot and frame start sample. From there the `south_read` follows this synchronization
information to produce samples for each slot and symbol.

## Thread structure

The O-RU shall use threadPool and/or actors to parallelize the workload. The exact details are up
to the implementation.

Actors are dedicated threads for a single job.

TheadPool is a pool of threads that can be shared for different tasks.

The OAI threadpool implementation disallows usage from another threadPool thread. Therefore if using
threadpool, another form of parallelism is required.

### DL

Each job shall process a subset of symbols in a given slot. The symbols processed by each job depend on
the `sense_of_time_t` struct returned by xran call.

### UL

UL should parallelize each task. This is due to the fact that `south_read` thread needs to poll the south
interface for new IQ samples continuously.

#### PRACH

Each job should process at least 1 PRACH occasion. The job shall be triggered from `south_read` thread
when the samples for the PRACH occasion are received

#### PUSCH

Each job should process a subset of RX antennas. The job shall be triggered from `south_read` thread once
samples for at least one symbol are received.

## Configuration

The O-RU shall use OAIs configuration subsystem and have its own configuration section. The changes to
existing sections shall be kept to a minimum

Configuration elements required:

|name|type|comment|
|---|---|---|
|phase_compensation|bool|Whether to perform symbol rotation.|
|prach_config_index|int|PRACH configuration index.|
