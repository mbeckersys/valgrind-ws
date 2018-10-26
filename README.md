# Valgrind-ws
This plugin for valgrind computes the [working set](https://en.wikipedia.org/wiki/Working_set) of an application.
That is, it captures the set of references that have been made in the past `tau` time units at each time `t`.
In other words, the working set `WS(t,tau)` is the collection of all memory accesses made in the time interval `(t-tau, t)`,
and quantifies the amount of memory that a process requires over a time interval.

This program measures at page granularity, separately for code and data.

## General Caveats
Valgrind slows down the execution significantly, depending on the workload.
Therefore, keep in mind that comparing time-controlled workloads (e.g., those which terminate after a fixed amount of time),
may not show the expected results.

## Tool Limitations
This tool is in early development, and might not do what you may expect. Please familiarize yourself with the limitations below.

 * The sampling interval is not exactly equidistant, but happens only at the end of superblocks or exit IR statements.
 * Sharing pages between threads is currently ignored, therefore the working set may be overestimated for multi-threaded programs.
 * If pages are unmapped and a new page is later mapped under the same address, they are counted as the same page, even if the contents may be different. This is less critical for the working set size, but affects the total given in the end.
 * Only pages which are actually accessed are counted. For example, readahead or prefetching are not considered.

## Compiling
### Prerequisites
Valgrind 3.14. Other versions are untested.

### Build Process
 1. Clone this repository into your valgrind sources, such that it resides in the same directory as `lackey`. In the following, we assume the new directory is called `ws`.
 2. some valgrind versions need a patch (to walk stack traces from ExeContext):
   * valgrind 3.14: apply [this patch](https://bugsfiles.kde.org/attachment.cgi?id=115712)
 3. Edit valgrind's `Makefile.am`, adding the new directory `ws` to the `TOOLS` variable
 4. Edit `configure.ac`, adding `ws/Makefile` to the `AC_CONFIG_FILES` list
 5. Run
```
autogen.sh
./configure # possibly with other parameters
make install
```
It should eventually produce a binary of ws in your `lib` folder.

## Usage
Basic usage:
```
valgrind --tool=ws <executable>
```
This computes the working set every 100,000 cycles. To calculate, say, every 50,000 cycles:
```
valgrind --tool=ws --ws-every=50000 <executable>
```

The page size is assumed to be 4kB by default, and can be changed with `--ws-pagesize`.

For a full list of options, use `--help`.

### Output
A summary is printed on standard output:
```
==5931== ws-0.1, compute working set for data and instructions
==5931== Copyright (C) 2018, and GNU GPL'd, by Martin Becker.
==5931== Using Valgrind-3.13.0 and LibVEX; rerun with -h for copyright info
==5931== Command: stress-ng -c 1 --cpu-ops=10
==5931==
==5931== Page size = 4096 bytes
==5931== Computing WS every 100000 instructions
==5931== Considering references in past 100000 instructions
stress-ng: info:  [5931] defaulting to a 86400 second (1 day, 0.00 secs) run per stressor
stress-ng: info:  [5931] dispatching hogs: 1 cpu
==5932==
==5932== Number of instructions: 604,599,465
==5932== Number of WS samples:   6,047
==5932== Dropped WS samples:     0
==5932== Writing results to file '/tmp/ws.out.5932'
==5932== ws finished
stress-ng: info:  [5931] successful run completed in 15.33s
==5931==
==5931== Number of instructions: 1,003,719
==5931== Number of WS samples:   12
==5931== Dropped WS samples:     0
==5931== Writing results to file '/tmp/ws.out.5931'
==5931== ws finished
```

The working set data is written to a file, one per PID. The file name can be chosen using `--ws-file`.
First it prints the header:
```
Working Set Measurement by valgrind-ws-0.3

Command:        stress-ng -c 1 --cpu-ops=10
Instructions:   604,599,465
Page size:      4096 B
Time Unit:      instructions
Every:          100,000 units
Tau:            100,000 units
```

Then, if `--ws-list-pages=yes` is specified, all used pages are listed (note that this could be a lot of output):
```
Code pages, 224 entries:
   count                 page  last-accessed location
382536864 0x00000000000040B000      604594200 0x40B000: djb2a (stress-cpu.c:645)
163118903 0x000000000000411000      550704581 0x411000: stress_cpu_nsqrt (stress-cpu.c:402)
45078797 0x000000000000421000      595770869 0x421000: stress_context.part.1 (stress-context.c:118)
 2434333 0x0000000000004A6000      600862075 0x4A6000: __bid64_mul (in /usr/bin/stress-ng)
 1272222 0x000000000000470000      387122000 0x470000: parse_opts (stress-ng.c:2044)
 1198336 0x000000000000488000      604594113 0x488000: __bid64_add (in /usr/bin/stress-ng)
 1169869 0x000000000000471000      604594130 0x471000: __divsc3 (in /usr/bin/stress-ng)
       .                    .              . .
       .                    .              . .
       .                    .              . .

Data pages, 792 entries:
   count                 page  last-accessed
 5243008 0x000000001FFEFF1000      550650144
 5243008 0x000000001FFEFF2000      550655264
 5243008 0x000000001FFEFF4000      550665504
 5243008 0x000000001FFEFF0000      550645024
 5243008 0x000000001FFEFF5000      550670624
 5243008 0x000000001FFEFEF000      550639904
 5243008 0x000000001FFEFF3000      550660384
 5226628 0x000000001FFEFF6000      550675744
 5187150 0x000000001FFEFEE000      550634784
       .                    .              .
       .                    .              .
       .                    .              .
```
The location info for code pages is taken from the debug info belonging to
the instruction at the lowest address in the given page. This has not necessarily been executed.

Finally, the tool prints the working set size (number of pages) over time:
```
Working sets:
           t WSS_insn WSS_data
           0        0        0
      100000       21       80
      200000       20       78
      300002       25       79
      400006       85       93
      500013      105      103
      600014       59      177
      700016        1      224
      800018        1      224
      900027       68       97
     1000027       94       95
     1100031       39       54
     1200033        1        2
     1300035        1        2
     1400039        1        2
     1500039        1        1
     1600042        1        2
           .        .        .
           .        .        .
           .        .        .

Insn WSS avg/var/peak:  1.4/11.7/105 pages (5/46/420 kB)
Data WSS avg/var/peak:  18.9/102.1/224 pages (75/408/896 kB)
Insn pages/access:      224 pages (896 kB)/2,699,104 accesses per page
Data pages/access:      792 pages (3,168 kB)/159,631 accesses per page
```
whereas the increments in column `t` are approximately the value of command line parameter `--every`.
The avg/var/total values are interpreted as follows:
 * `avg` is the average number of the working set size over all samples in `t`.
 * `total` is the working set size over the entire life time of the process, i.e., t=tau=inf.
 * `var` is the variance of the working set size over all `t`.
Finally, the tool shows the total number of pages that have been accessed, and how often they
have been accessed in average.

### Stride Analysis (experimental)
With option `--ws-track-locality`.


### Additional Information for Samples
Additional information, such as the current call stack, can be collected for some samples. Currently,
there are two ways to determine for which samples detailed information is recorded:
 1. at user-defined points in time. Use command line argument `--ws-info-at`
 2. automatically, when peaksin the working set size are detected. Use command line argument `--ws-peak-detect=yes`. More information about peak detection is given below

#### Peak Detection
With option `--ws-peak-detect=yes`, the tool tries to detect sudden jumps in the working set sizes,
and records additional sample information to allow for further debugging.
Towards this, the working set table is augmented with a column `info`, which contains an ID number
referencing further information about the sample:

```
...
Working sets:
           t WSS_insn WSS_data info
           0        0        0    -
      100002       21       79    -
           .        .        .    .
           .        .        .    .
           .        .        .    .
    96101775        1        2    0
           .        .        .    .
           .        .        .    .
           .        .        .    .
   380931690       27       86    3
--
```

The references are elaborated below:
```

Sample info:
[   0] refs=4, loc=stress-cpu.c:1262|stress-cpu.c:1267|stress-cpu.c:1267|stress-cpu.c:1267|...
...
[   3] refs=1, loc=
```
In this example, it means that the call stack with ID=0 was encountered at four different peaks (e.g.,
at t=96101775, see table above), and the source location is given as a string thereafter
("stress-cpu.c:1262" called by "stress-cpu.c:1267" ...).

Furthermore, this example demonstrates a special case: the process produced a peak during exit
(the last sample is always taken at exit), with reference ID=3. However, at process exit, we no
longer have any location info, thus `loc` is empty.

##### Detection Parameters
We use a moving average/variance and compare the dispersion of new samples towards the current
average. A peak is detected if the current sample deviates more than a certain threshold, multiplied
by `--ws-peak-thresh`. The threshold itself depends on the local memory characteristics: If the
ratio variance over peak is high, then the threshold is defined by how many "variances" the new
 sample deviates from the average. If it is low, then it is defined by how much in comparison to
 the average it deviates from the average. In between, both factors are considered.

Additionally, every time a peak is detected, the signal is exponentially filtered, that is, since
the current working set size is currently peaky, we do not fully include it into the window statistics,
but only with exponential dampening. The gain parameter is currently not available via the command line.

Tuning tips:
 * larger windows assume more stationary behavior, and take longer to track
 * higher thresholds require bigger jumps before a peak is detected

The accompanying Python script can be used to experiment with values.

### Interpretation of the example
In the example above, it can be seen that the workload initially requires around hundred instruction pages,
which however decreases to an average of 1.4 pages later on (this is not surprising, since this particular
workload -- stress-ng -- is designed to consume as little memory as possible when stationary).
The peak working set size for instructions is at the program initialization, and about half of the
total number of pages used over the entire life time.

The picture is similar for data, where on average 18.9 pages have been referenced every 100,000
instructions, and at most 224 pages.

In summary, this process requires on average 80kB of memory every 100,000 instructions, which is
much less than the total amount of memory shown by other tools, e.g., by the `time` command:

```
\time -v stress-ng -c 1 --cpu-ops=10
        Command being timed: "stress-ng -c 1 --cpu-ops=10"
        .
        .
        .
        Maximum resident set size (kbytes): 4900
        Average resident set size (kbytes): 0
```

## Visualization
To generate a plot of the working set, you may use the Python script `valgrind-ws-plot.py` in the
folder tools. It uses matplotlib to generate an interactive plot. Example:
```
./valgrind-ws-plot.py --yscale=symlog ../tests/data/ws.peak.out
```
gives
![Alt text](/tests/data/ws.peak.out.png?raw=true "Plot")

The y-axis is in units of pages. The green annotations mark peaks, if `--ws-peak-detect=yes` is used,
and the numbers are the IDs of the call stacks. The plot can also be exported to a file with
command line option `--output=myfile.png`
