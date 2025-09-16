This document lists the changes in configuration files for OAI gNB starting from 2023.
The aim is to simplify change tracking so that users can keep their private
configuration files up to date with the ones present in the repository.

[[_TOC_]]

# 2025.w37 Rework BWP configuration

Additional BWP configuration reworked. `servingCellConfigDedicated` removed and
replaced with `bwp_list` containing information about each additional BWP.
See [`RUN_NR_multiple_BWPs.md`](../RUN_NR_multiple_BWPs.md) for more information.

# 2025.w37 Rework PTRS configuration

Moved from `servingCellConfigDedicated` section to dedicated `phaseTrackingRS` section.

# 2025.w37 Remove ulsch_max_frame_inactivity

In case the parameter is set to 10 (the default, so not necessary), or to
0 forcing the gNB to schedule the UE all the time, which is only
necessary in specific scenarios.

# 2025.w33 Remove ra_ResponseWindow

If not present in the configuration file, the ra_ResponseWindow will be chosen
automatically.

# 2025.w32 Remove F1 interface names

These options are not taken into account by the code and not even defined in
the configuration module.

# 2025.w32 Remove unused control port number

 he configuration is not actually applied, and the default F1-C port number 38472
is hardcoded in the F1 module.  The configuration option is still available,
in case we want to allow to configure the port numbers in the future.

# 2025.w02 Improvements in PRACH parameters

`ssb_perRACH_OccasionAndCB_PreamblesPerSSB_PR` set to 4 (meaning 1 SSB per RACH occasion)
in all configuration files having a single SSB.

`prach_ConfigurationIndex` set to a value not scheduling PRACH in the last symbol of the slot
to avoid overlapping with PUCCH. See Tables from 6.3.3.2-2 to 6.3.3.2-4 in 38.211

# 2024.w49 Remove RU delay profile 

XRAN E release (and later F) doesn't use them. fhi_72 section in fronthaul config files
updated by removing unnecessary RU delay parameters.

# 2024.w41 Remove bf_weights field

Deprecated `bf_weights` removed from RU section.

# 2024.w38 Remove ssb_PositionsInBurs_BitmapPR

The parameter, determining the maximum number of SSBs, is now set automatically.

# 2024.w32 Cleanup of unused parallel/worker thread

In LTE there is logic that checks the thread configurtion with the
getters `get_thread_parallel_conf()` and `get_thread_worker_conf()`
in NR these getters are not used anymore.

# 2024.w24 7.2 FH: auto-compute FFT size

Size of FFT performed by 7.2 RU is now automatically computed.

# 2024.w15 Remove IPv6/active/preference fields from amf_ip_address array

Not used

# 2024.w15 Remove AMF Network Interface sections's interface names

Not used

# 2023.w44 RFsimulator serverport

Change from a string to a number

# 2023.w43 Rename gNB_CU_ID

Not existing `gNB_CU_ID` renamed to existing gNB_ID

# 2023.w06 Remove duplicate cset0 ss0 configuration

Before this change there was a duplicated configuration for
coreset0 and searchspace0. `pdcch_ConfigSIB1` has been removed
and only `initialDLBWPcontrolResourceSetZero` and `initialDLBWPsearchSpaceZero`
are used.
