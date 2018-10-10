# Valgrind-ws
This plugin for valgrind computes the [working set](https://github.com/mbeckersys/valgrind-ws.git) of an application.
That is, it captures the set of references that have been made in the past `tau` time units at each time `t`.
In other words, the working set `WS(t,tau)` is the collection of all memory accesses made in the time interval `(t-tau, t)`,
and quantifies the amount of memory that a process requires over a time interval.

This program measures at page granularity, separately for code and data.

## Limitations
This tool in early development, and not do what you might expect. Please familiarize yourself with the limitations below.

### Precision
 * The sampling interval is not exactly equidistant, but happens only at the and of superblocks or exit IRs
 * Sharing pages between threads is currently ignored, therefore the working set may be too large for multi-threaded programs.

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
TODO
