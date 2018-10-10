# Valgrind-ws
This plugin for valgrind computes the [working set](https://github.com/mbeckersys/valgrind-ws.git) of an application.
That is, it captures the set of references have been made in the past `tau` time units at each time `t`. 
In other words, the working set `WS(t,tau)` is all collection of all memory accesses made in the time interval `(t-tau, t)`,
and quantifies the amount of memory that a process requires over a time interval.

This program measures at page granularity, separately for code and data.

## Compiling
### Prerequisites
Valgrind 3.13. Other versions are untested.

### Build Process
 1. clone this repository into your valgrind souces, such that it resides in the same directory as `lackey`. We assume the new directoy is called `ws` in the following
 2. edit valgrind's `Makefile.am`, adding the new directory `ws` to the `TOOLS` variable
 3. edit `configure.in`, adding `ws/Makefile`, `ws/docs/Makefile` and `ws/tests/Makefile` to the `AC_OUTPUT` list
 4. run 
```
autogen.sh
./configure # possibly with other parameters
make install
```
It should eventually produce a binary of ws in your `lib` folder
 5. test with: `valgrind --tool=ws ls`

## Usage
TODO
