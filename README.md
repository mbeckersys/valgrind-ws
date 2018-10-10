# Valgrind-ws
This plugin for valgrind computes the [working set](https://github.com/mbeckersys/valgrind-ws.git) of an application.
That is, it captures the set of references that have been made in the past `tau` time units at each time `t`.
In other words, the working set `WS(t,tau)` is the collection of all memory accesses made in the time interval `(t-tau, t)`,
and quantifies the amount of memory that a process requires over a time interval.

This program measures at page granularity, separately for code and data.

## Limitations
This tool in early development, and might not do what you may expect. Please familiarize yourself with the limitations below.

### Precision
 * The sampling interval is not exactly equidistant, but happens only at the end of superblocks or exit IR statements.
 * Sharing pages between threads is currently ignored, therefore the working set may be overestimated for multi-threaded programs.

## Compiling
### Prerequisites
Valgrind 3.13. Other versions are untested.

### Build Process
 1. Clone this repository into your valgrind sources, such that it resides in the same directory as `lackey`. In the following, we assume the new directoy is called `ws`.
 2. Edit valgrind's `Makefile.am`, adding the new directory `ws` to the `TOOLS` variable
 3. Edit `configure.in`, adding `ws/Makefile`, `ws/docs/Makefile` and `ws/tests/Makefile` to the `AC_OUTPUT` list
 4. Run
```
autogen.sh
./configure # possibly with other parameters
make install
```
It should eventually produce a binary of ws in your `lib` folder

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
The tool dumps the output to a file, one per PID. Filenames can be selected via `--ws-filename`.
First it prints the page counters:
```
Code pages:
 269 entries:
     count                 page  last accessed
1580768124 0x00000000000040B000     4497357144
1026520428 0x00000000000040C000     4103152507
 840914760 0x000000000000421000     4497357138
 551533455 0x000000000000411000     3559503578
 104982644 0x000000000000422000     4117425424
  60066864 0x000000000000410000     3796721408

Data pages:
1474 entries:
    count                 page  last accessed
162444651 0x000000001FFEFFF000     4497362197
 20039552 0x000000001FFEFF1000     4497336740
 20039552 0x000000001FFEFF2000     4497338276
 20039552 0x000000001FFEFF0000     4497335204
 20039552 0x000000001FFEFF3000     4497339812
```

Then it prints the working set size (in number of pages) over time:
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
```
whereas the increments in column `t` are approximately the value of command line parameter `--every`.
