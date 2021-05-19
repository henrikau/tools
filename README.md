# Tools

Small tools used along the way. Most are one-offs, most are written in C

## Requirements

You need:
- Fairly new Linux kernel/distro. If a particular version is required,
it is listed by the tool
- meson and ninja for building
- gcc (any version shipped with a fairly recent distro should work)


## Building

```
meson build
ninja -C build/
```

Binaries can be found in build/

# Usage
## pcie_clock

Background: I needed to look at how PCIe latency could affect real-time
tasks, and in particular how a PTP synchronized (syntonized) clock must
be connected to the system time. This means moving data across the PCIe
bus. AFAIK, there's no way to prioritize traffic in PCIe, so a NIC clock
accurate to within "a few hundred ns" will not be as accurate for the
system.

pcie_clock is fairly simple, it reads TSC before and after reading
both CLOCK_REALTIME and ptp_fd. It attempts to reduce the interference
from other tasks by running as sched_deadline. For further accuracy, the
task should be shielded using cpuset (cset is useful!).

```
sudo ./build/pcie_clock -i eth2 -l 100000 -o ptp_clock.csv
```

This runs for 100k iterations (1ms period) using PTP clock on eth2 and
stores the output to ptp_clock.csv on the format

``` text
$ head -n5 ptp_clock.csv
clock_realtime_s,tsc_real,tsc_ptp,tsc_tot,clock_diff_s
1621430653.394576669,5334,248738,254092,37.000011
1621430653.395801327,412,210022,210454,37.000017
1621430653.397008828,434,170504,170956,37.000017
1621430653.398176339,448,78544,79012,37.000017
```

- clock_realtime: timestamp of system. Ideally synchronized using
  phc2sys from ptp timestamp.
- tsc_real: Number of CPU cycles from start to end of reading
  CLOCK_REALTIME
- tsc_ptp: Number of CPU cycles from start to end of reading the PTP
  clock via PCIe
- tsc_tot: Number of CPU cycles spent reading '''both''' clock sources
- clock_diff_s: Difference between system clock and PTP time (TAI)
