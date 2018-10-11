# Valgrind-ws
This plugin for valgrind computes the [working set](https://github.com/mbeckersys/valgrind-ws.git) of an application.
That is, it captures the set of references that have been made in the past `tau` time units at each time `t`.
In other words, the working set `WS(t,tau)` is the collection of all memory accesses made in the time interval `(t-tau, t)`,
and quantifies the amount of memory that a process requires over a time interval.

This program measures at page granularity, separately for code and data.

## Limitations
This tool in early development, and might not do what you may expect. Please familiarize yourself with the limitations below.

### Problems
Untested for multi-threaded programs.

### Precision
 * The sampling interval is not exactly equidistant, but happens only at the end of superblocks or exit IR statements.
 * Sharing pages between threads is currently ignored, therefore the working set may be overestimated for multi-threaded programs.

## Compiling
### Prerequisites
Valgrind 3.13. Other versions are untested.

### Build Process
 1. Clone this repository into your valgrind sources, such that it resides in the same directory as `lackey`. In the following, we assume the new directory is called `ws`.
 2. Edit valgrind's `Makefile.am`, adding the new directory `ws` to the `TOOLS` variable
 3. Edit `configure.in`, adding `ws/Makefile`, `ws/docs/Makefile` and `ws/tests/Makefile` to the `AC_OUTPUT` list
 4. Run
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

### Output
The tool dumps the output to a file, one per PID. The file name can be chosen using `--ws-file`.
First it prints the page access counters:
```
Working Set Measurement by valgrind-ws-0.1

Command: stress-ng --memrate 1 -t 2
Instructions: 1044362
Page size: 4096 B
Time Unit: instructions
Every: 10000 units
Tau: 1000000 units

Code pages:
 180 entries:
   count                 page  last-accessed location
  247131 0x000000000005A25000        1037277 0x5A25DC0: strcasecmp (strcmp.S:114)
  122343 0x000000000004008000        1044254 0x4008100: _dl_map_object (dl-load.c:2317)
   76825 0x000000000004009000        1044316 0x4009810: _dl_lookup_symbol_x (dl-lookup.c:714)
   74795 0x000000000004017000        1044245 0x4017B50: strlen (rtld-strlen.S:26)
   56045 0x000000000005ABD000         386040 0x5ABD640: _dl_addr (dl-addr.c:126)
       .                    .              . .
       .                    .              . .
       .                    .              . .

Data pages:
 722 entries:
   count                 page  last-accessed
   62284 0x000000001FFEFFE000        1037716
   61885 0x000000001FFEFFF000        1044344
   15906 0x000000000004223000        1042794
   12566 0x000000000004050000        1044329
   11725 0x00000000000404F000        1043695
    9604 0x000000001FFEFFD000        1037649
       .                    .              .
       .                    .              .
       .                    .              .
```
The location info for code pages is the debug info belonging to
the instruction at the lowest address in the given page. This has not necessarily been executed.

Then it prints the working set size (number of pages) over time:
```
Working sets:
           t WSS_insn WSS_data
           0        0        0
       10003       14       16
       20012       30       16
       30013       26       13
       40015       35       13
       50018       37       13
       60018       37       13
       70020       39       14
       80021       25       10
       90033       28       11
      100035       27        5
      110039       23       11
      120041       10        1
      130041       50        4
      140042       43        5
      150042       50        5
      160055       43        4
      170061       43        4
      180063       51        5
           .        .        .
           .        .        .
           .        .        .

Insn peak: 107 pages (428 kB)
Data peak: 238 pages (952 kB)
```
whereas the increments in column `t` are approximately the value of command line parameter `--every`.

## Visualization
TODO
