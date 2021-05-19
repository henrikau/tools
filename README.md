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
$ head ptp_clock.csv
clock_realtime_s,tsc_real,tsc_ptp,clock_diff_s
1621414876.139971814,25536,32338,37.000005
1621414876.141000278,342,27322,37.000004
1621414876.142012783,228,27132,37.000004
1621414876.143024578,190,27246,37.000004
1621414876.144036753,494,27208,37.000004
1621414876.145048237,228,27018,37.000004
1621414876.146059562,190,27132,37.000004
1621414876.147072207,532,32072,37.000005
1621414876.148542319,3876,40660,37.000007
```
