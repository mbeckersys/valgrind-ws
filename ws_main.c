
/*-----------------------------------------------------------------------*/
/*--- A Valgrind tool to compute working sets of a process. ws_main.c ---*/
/*-----------------------------------------------------------------------*/

/*
   This file is part of ws.

   Copyright (C) 2018 Martin Becker

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/
#include <sys/types.h>

#include "pub_tool_basics.h"
#include "pub_tool_tooliface.h"
#include "pub_tool_libcassert.h"
#include "pub_tool_libcprint.h"
#include "pub_tool_libcbase.h"
#include "pub_tool_libcfile.h"
#include "pub_tool_libcproc.h"
#include "pub_tool_options.h"
#include "pub_tool_machine.h"      // VG_(fnptr_to_fnentry)
#include "pub_tool_clientstate.h"  // args + exe name
#include "pub_tool_hashtable.h"
#include "pub_tool_mallocfree.h"
#include "pub_tool_debuginfo.h"
#include "pub_tool_stacktrace.h"
#include "pub_tool_execontext.h"
#include "pub_tool_threadstate.h"
#include "pub_tool_xtree.h"
#include "pub_tool_xarray.h"
#include "valgrind.h"

/*------------------------------------------------------------*/
/*--- version-specific defs                                ---*/
/*------------------------------------------------------------*/

//#define DEBUG

#if defined(__VALGRIND_MAJOR__) && defined(__VALGRIND_MINOR__) \
    && (__VALGRIND_MAJOR__ >= 3 && __VALGRIND_MINOR__ >= 14)
   // all okay
#else
   #error "This valgrind version is too old"
#endif

/*------------------------------------------------------------*/
/*--- tool info                                            ---*/
/*------------------------------------------------------------*/

#define WS_NAME    "ws"
#define WS_VERSION "0.4"
#define WS_DESC    "compute working set for data and instructions"

/*------------------------------------------------------------*/
/*--- type definitions                                     ---*/
/*------------------------------------------------------------*/

typedef unsigned long pagecount;

struct map_pageaddr
{
  VgHashNode        top;  // page address, must be first
  unsigned long int count;
  Time              last_access;
  DiEpoch           ep;  // FIXME: opt: we do not use debug info for data pages, remove ep for data?
};

#define vgPlain_malloc(size) vgPlain_malloc ((const char *) __func__, size)

typedef enum { TimeI, TimeMS } TimeUnit;

typedef
   IRExpr
   IRAtom;

/* --- Operations --- */

typedef enum { OpLoad=0, OpStore=1, OpAlu=2 } Op;

#define MAX_DSIZE 512

typedef
   enum { Event_Ir, Event_Dr, Event_Dw, Event_Dm }
   EventKind;

typedef
   struct {
      EventKind  ekind;
      IRAtom*    addr;
      Int        size;
      IRAtom*    guard; /* :: Ity_I1, or NULL=="always True" */
   }
   Event;

typedef
   struct {
      Time t; // FIXME: opt: we could store only the delta to the intended point in time.
      pagecount pages_insn;
      pagecount pages_data;
   #ifdef DEBUG
      Float     mAvg, mVar;
   #endif
   }
   WorkingSet;

/**
 * @brief details for a single working set sample
 */
typedef
   struct {
      unsigned int  id;
      unsigned int  cnt;
      HChar        *callstack;
   }
   SampleInfo;

/**
 * @brief element in hash table ExeContext -> SampleInfo.
 */
struct map_context2sampleinfo
{
  VgHashNode top;   // ExeContext ECU, must be first
  SampleInfo info;
};

/**
 * @brief element in list ws_context_list
 */
typedef
   struct {
      Time        t;
      ExeContext *ec;
   }
   SampleContext;

/**
 * @brief internal data of peak detector
 */
typedef
   struct {
      // states
      unsigned  k;
      short int peak_pre;
      Float     filt_pre;
      Float     movingAvg;
      Float     movingVar;
      // params
      unsigned  window;
      Float     threshgain;
      Float     adaptrate;  ///< coefficient for filtering out peaks
      Float     exp_alpha;  ///< coefficient for exponential moving filters
   }
   PeakDetect;

typedef
   struct {
      HChar *str;
      int    rem;
   }
   callstack_string;

typedef
   struct {
      unsigned long n;
      unsigned long sum;
      Addr          pre;
   }
   LocalityInfo;

/*------------------------------------------------------------*/
/*--- prototypes                                           ---*/
/*------------------------------------------------------------*/

static void maybe_compute_ws (void);

/*------------------------------------------------------------*/
/*--- globals                                              ---*/
/*------------------------------------------------------------*/

static Bool postmortem = False;  ///< certain actions we cannot do after process terminated
static Long guest_instrs_executed = 0;

// page access tables
static VgHashTable *ht_data;
static VgHashTable *ht_insn;
static VgHashTable *ht_ec2sampleinfo;

// list of user-defined points in time where sample info shall be recorded
static XArray *ws_info_times;
static int     next_user_time_idx = -1;

// working set at each point in time
static XArray        *ws_at_time;
static unsigned long  drop_samples = 0;

// list of sample contexts; on termination converted to SampleInfo
static XArray *ws_context_list;

// locality info
LocalityInfo locality_insn, locality_data;

/* Up to this many unnotified events are allowed.  Must be at least two,
   so that reads and writes to the same address can be merged into a modify.
   Beyond that, larger numbers just potentially induce more spilling due to
   extending live ranges of address temporaries. */
#define N_EVENTS 4

/* Maintain an ordered list of memory events which are outstanding, in
   the sense that no IR has yet been generated to do the relevant
   helper calls.  The SB is scanned top to bottom and memory events
   are added to the end of the list, merging with the most recent
   notified event where possible (Dw immediately following Dr and
   having the same size and EA can be merged).

   This merging is done so that for architectures which have
   load-op-store instructions (x86, amd64), the instr is treated as if
   it makes just one memory reference (a modify), rather than two (a
   read followed by a write at the same address).

   At various points the list will need to be flushed, that is, IR
   generated from it.  That must happen before any possible exit from
   the block (the end, or an IRStmt_Exit).  Flushing also takes place
   when there is no space to add a new event, and before entering a
   RMW (read-modify-write) section on processors supporting LL/SC.

   If we require the simulation statistics to be up to date with
   respect to possible memory exceptions, then the list would have to
   be flushed before each memory reference.  That's a pain so we don't
   bother.

   Flushing the list consists of walking it start to end and emitting
   instrumentation IR for each event, in the order in which they
   appear. */

static Event events[N_EVENTS];
static Int   events_used = 0;

PeakDetect   pd_data, pd_insn;

/*------------------------------------------------------------*/
/*--- Command line options                                 ---*/
/*------------------------------------------------------------*/

/* Command line options controlling instrumentation kinds, as described at
 * the top of this file. */

#define WS_DEFAULT_PS 4096
#define WS_DEFAULT_EVERY 100000
#define WS_DEFAULT_TAU WS_DEFAULT_EVERY
#define WS_DEFAULT_PEAKT 5
#define WS_DEFAULT_PEAKW 30
#define WS_DEFAULT_PEAKADP 0.25f ///< default value for peak filter. lower=more robust to bursts

// user inputs:
static Bool  clo_locations  = True;
static Bool  clo_listpages  = False;
static Bool  clo_peakdetect = False;
static Bool  clo_localitytr = False;
static Int   clo_peakthresh = WS_DEFAULT_PEAKT;  // FIXME: Float?
static Int   clo_peakwindow = WS_DEFAULT_PEAKW;
static Float clo_peakadapt  = WS_DEFAULT_PEAKADP;  // FIXME: from clo
static Int   clo_pagesize   = WS_DEFAULT_PS;
static Int   clo_every      = WS_DEFAULT_EVERY;
static Int   clo_tau        = 0;
static Int   clo_time_unit  = TimeI;

/* The name of the function of which the number of calls (under
 * --basic-counts=yes) is to be counted, with default. Override with command
 * line option --fnname. */
//static const HChar* clo_fnname = "main";
static const HChar* clo_filename = "ws.out.%p";
static const HChar* clo_info_at = "";
static HChar* int_filename;

/*------------------------------------------------------------*/
/*--- libc replacements                                    ---*/
/*------------------------------------------------------------*/
#define FABS(x) ((x < 0.f) ? -x : x)

static inline
Float exp_approx(Float x)
{
  x = 1.0 + x / 256.0;
  x *= x; x *= x; x *= x; x *= x;
  x *= x; x *= x; x *= x; x *= x;
  return x;
}

/*------------------------------------------------------------*/
/*--- all other functions                                  ---*/
/*------------------------------------------------------------*/

static
Bool ws_process_cmd_line_option(const HChar* arg)
{
   if VG_BOOL_CLO(arg, "--ws-locations", clo_locations) {}
   else if VG_BOOL_CLO(arg, "--ws-list-pages", clo_listpages) {}
   else if VG_STR_CLO(arg, "--ws-file", clo_filename) {}
   else if VG_STR_CLO(arg, "--ws-info-at", clo_info_at) {}
   else if VG_INT_CLO(arg, "--ws-pagesize", clo_pagesize) {}
   else if VG_INT_CLO(arg, "--ws-every", clo_every) {}
   else if VG_INT_CLO(arg, "--ws-tau", clo_tau) { tl_assert(clo_tau > 0); }
   else if VG_XACT_CLO(arg, "--ws-time-unit=i", clo_time_unit, TimeI)  {}
   else if VG_XACT_CLO(arg, "--ws-time-unit=ms", clo_time_unit, TimeMS) {}
   else if VG_BOOL_CLO(arg, "--ws-peak-detect", clo_peakdetect) {}
   else if VG_BOOL_CLO(arg, "--ws-track-locality", clo_localitytr) {}
   else if VG_INT_CLO(arg, "--ws-peak-window", clo_peakwindow) { tl_assert(clo_peakwindow > 0); }
   else if VG_INT_CLO(arg, "--ws-peak-thresh", clo_peakthresh) { tl_assert(clo_peakthresh > 0); }
   else return False;

   tl_assert(clo_filename);
   tl_assert(clo_filename[0]);
   tl_assert(clo_pagesize > 0);
   tl_assert(clo_every > 0);
   return True;
}

static
void ws_print_usage(void)
{
   VG_(printf)(
"    --ws-file=<string>            file name to write results\n"
"    --ws-list-pages=no|yes        print list of all accessed pages [no]\n"
"    --ws-locations=no|yes         collect location info for insn pages in listing [yes]\n"
"    --ws-peak-detect=no|yes       collect info for peaks in working set [no]\n"
"    --ws-peak-window=<int>        window length (in samples) for peak detection [%d]\n"
"    --ws-peak-thresh=<int>        threshold for peaks. Lower is more sensitive [%d]\n"
"    --ws-info-at=<int>(,<int>)*   list of points in time where additional information shall be recorded\n"
"    --ws-track-locality=no|yes    compute locality of access\n"
"    --ws-pagesize=<int>           size of VM pages in bytes [%d]\n"
"    --ws-time-unit=i|ms           time unit: instructions executed (default), milliseconds\n"
"    --ws-every=<int>              sample working set every <int> time units [%d]\n"
"    --ws-tau=<int>                consider all accesses made in the last tau time units [%d]\n",
   WS_DEFAULT_PEAKW,
   WS_DEFAULT_PEAKT,
   WS_DEFAULT_PS,
   WS_DEFAULT_EVERY,
   WS_DEFAULT_TAU
   );
}

static
void ws_print_debug_usage(void)
{
   VG_(printf)(
"    (none)\n"
   );
}

static
const HChar* TimeUnit_to_string(TimeUnit time_unit)
{
   switch (time_unit) {
   case TimeI:  return "instructions";
   case TimeMS: return "ms";
   default:     tl_assert2(0, "TimeUnit_to_string: unrecognised TimeUnit");
   }
}

static
void init_locality(LocalityInfo *li)
{
   li->sum = 0;
   li->n = 0;
   li->pre = 0;
}

static
void init_peakd(PeakDetect *pd)
{
   pd->filt_pre = 0.f;
   pd->peak_pre = 0;
   pd->k = 0;
   pd->movingAvg = 0.f;
   pd->movingVar = 0.f;

   pd->window = clo_peakwindow;
   pd->exp_alpha = 2.f / (clo_peakwindow + 1);
   pd->adaptrate = (Float) clo_peakadapt;
   pd->threshgain = (Float) clo_peakthresh;
}

/**
 * @brief detects peaks in working set size
 * Implements a moving average (avg) and a moving variance (var) filter.
 * Window length is given by --ws-peak-window.
 * Peak is detected if signal properties change such that a certain
 * metric becomes higher than a threshold.
 *
 * The Fano factor F (var/avg) is used to decide how peaks are detected:
 *  - high F: compare changes to current ver
 *  - low F: compare changes to current avg
 *  - in between: smooth transition between both
 *
 * Additionally, the signal is exponentially filtered iff peaks are detected,
 * before it is taken into the moving windows. Full filtering (1.0) means
 * the signal is assumed stationary, and thresholds do not react to peaks. Vice
 * versa, 0.0 suppresses filtering, taking signal into account as it is.
 */
static
Bool peak_detect(PeakDetect *pd, pagecount y)
{
   short int pk = 0;
   Float filt = (Float) y;

   // detect peaks and filter them
   Float coeff = 1.0;
   if (pd->movingAvg > 0.f) {
      const Float fano = pd->movingVar / pd->movingAvg;
      coeff = 1.0 - exp_approx(-fano / 2.);  // fano = 1.0 -> 60% weight to variance
   }
   const Float thresh = coeff * pd->threshgain * pd->movingVar +
                        (1.f - coeff) * pd->threshgain/10. * pd->movingAvg;
   Float y0 = y - pd->movingAvg;
   const Bool is_peak = FABS(y0) > thresh;
   if (pd->k >= pd->window && is_peak) {
      pk = (y > pd->movingAvg) ? 1 : -1;
      filt = pd->adaptrate * y + (1 - pd->adaptrate) * pd->filt_pre;
   }

   // moving variance (must be calc'd first)
   if (0 < pd->k) {
      const Float diff = ((Float) filt) - pd->movingAvg;  // XXX: important, previous average.
      pd->movingVar = (1.f - pd->exp_alpha) * (pd->movingVar + pd->exp_alpha * diff * diff);
   } else {
      pd->movingVar = 0.f;
   }

   // moving avg
   if (0 < pd->k) {
      pd->movingAvg = pd->exp_alpha * filt + (1.f - pd->exp_alpha) * pd->movingAvg;
   } else {
      pd->movingAvg = (Float) filt;
   }

   if (pd->k < pd->window) pd->k++;
   pd->filt_pre = filt;

   Bool ret = pk != pd->peak_pre;
   pd->peak_pre = pk;
   return ret;
}

/**
 * @brief Get current time, in whatever time unit we're using.
 */
static
Time get_time(void)
{
   if (clo_time_unit == TimeI) {
      return guest_instrs_executed;
   } else if (clo_time_unit == TimeMS) {
      // Some stuff happens between the millisecond timer being initialised
      // to zero and us taking our first snapshot.  We determine that time
      // gap so we can subtract it from all subsequent times so that our
      // first snapshot is considered to be at t = 0ms.  Unfortunately, a
      // bunch of symbols get read after the first snapshot is taken but
      // before the second one (which is triggered by the first allocation),
      // so when the time-unit is 'ms' we always have a big gap between the
      // first two snapshots.  But at least users won't have to wonder why
      // the first snapshot isn't at t=0.
      static Bool is_first_get_time = True;
      static Time start_time_ms;
      if (is_first_get_time) {
         start_time_ms = VG_(read_millisecond_timer)();
         is_first_get_time = False;
         return 0;
      } else {
         return VG_(read_millisecond_timer)() - start_time_ms;
      }
   } else {
      tl_assert2(0, "bad --ws-time-unit value");
   }
}

static
inline Addr pageaddr(Addr addr)
{
   return addr & ~(clo_pagesize-1);
}

static
inline void track_locality(LocalityInfo *li, Addr addr)
{
   if (li->pre != addr) {
      const unsigned long d = li->pre > addr? li->pre - addr : addr - li->pre;
      li->sum += d;
      li->pre = addr;
   }
   li->n++;  ///< technically, we could derive this from #page accesses. But it's ~no overhead.
}

// TODO: pages shared between processes?
static
inline void pageaccess(Addr pageaddr, VgHashTable *ht)
{
   // this is a one-item cache, exploiting locality and speeding up sim dramatically
   static Addr                 lastaddr = 0;
   static struct map_pageaddr *lastpage = NULL;
   struct map_pageaddr        *page;
   if (pageaddr == lastaddr) {
      page = lastpage;
   } else {
      page = VG_(HT_lookup) (ht, pageaddr);
      if (page == NULL) {
         page = VG_(malloc) (sizeof (*page));
         page->top.key = pageaddr;
         page->count = 0;
         page->ep = VG_(current_DiEpoch)();
         VG_(HT_add_node) (ht, (VgHashNode *) page);
      }
      lastaddr = pageaddr;
      lastpage = page;
   }
   page->count++;
   page->last_access = (long) get_time();

   maybe_compute_ws();
}

static
VG_REGPARM(2) void trace_data(Addr addr, SizeT size)
{
   const Addr pa = pageaddr(addr);
   pageaccess(pa, ht_data);
   if (clo_localitytr) track_locality(&locality_data, addr);
}

static
VG_REGPARM(2) void trace_instr(Addr addr, SizeT size)
{
   const Addr pa = pageaddr(addr);
   pageaccess(pa, ht_insn);
   if (clo_localitytr) track_locality(&locality_insn, addr);
}

static
void flushEvents(IRSB* sb)
{
   Int        i;
   const HChar* helperName;
   void*      helperAddr;
   IRExpr**   argv;
   IRDirty*   di;
   Event*     ev;

   for (i = 0; i < events_used; i++) {

      ev = &events[i];

      // Decide on helper fn to call and args to pass it.
      switch (ev->ekind) {
         case Event_Ir: helperName = "trace_instr";
                        helperAddr =  trace_instr;  break;

         case Event_Dr:
         case Event_Dw:
         case Event_Dm: helperName = "trace_data";
                        helperAddr =  trace_data; break;
         default:
            tl_assert(0);
      }

      // Add the helper. FIXME: help the branch predictor here?
      argv = mkIRExprVec_2( ev->addr, mkIRExpr_HWord( ev->size ));
      di   = unsafeIRDirty_0_N( /*regparms*/2,
                                helperName, VG_(fnptr_to_fnentry)( helperAddr ),
                                argv );
      if (ev->guard) {
         di->guard = ev->guard;
      }
      addStmtToIRSB( sb, IRStmt_Dirty(di) );
   }

   events_used = 0;
}

static
void addEvent_Ir ( IRSB* sb, IRAtom* iaddr, UInt isize )
{
   Event* evt;
   tl_assert( (VG_MIN_INSTR_SZB <= isize && isize <= VG_MAX_INSTR_SZB)
            || VG_CLREQ_SZB == isize );
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Ir;
   evt->addr  = iaddr;
   evt->size  = isize;
   evt->guard = NULL;
   events_used++;
}

/* Add a guarded read event. */
static
void addEvent_Dr_guarded ( IRSB* sb, IRAtom* daddr, Int dsize, IRAtom* guard )
{
   Event* evt;
   tl_assert(isIRAtom(daddr));
   tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Dr;
   evt->addr  = daddr;
   evt->size  = dsize;
   evt->guard = guard;
   events_used++;
}

/* Add an ordinary read event, by adding a guarded read event with an
   always-true guard. */
static
void addEvent_Dr ( IRSB* sb, IRAtom* daddr, Int dsize )
{
   addEvent_Dr_guarded(sb, daddr, dsize, NULL);
}

/* Add a guarded write event. */
static
void addEvent_Dw_guarded ( IRSB* sb, IRAtom* daddr, Int dsize, IRAtom* guard )
{
   Event* evt;
   tl_assert(isIRAtom(daddr));
   tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Dw;
   evt->addr  = daddr;
   evt->size  = dsize;
   evt->guard = guard;
   events_used++;
}

/* Add an ordinary write event.  Try to merge it with an immediately
   preceding ordinary read event of the same size to the same
   address. */
static
void addEvent_Dw ( IRSB* sb, IRAtom* daddr, Int dsize )
{
   Event* lastEvt;
   Event* evt;
   tl_assert(isIRAtom(daddr));
   tl_assert(dsize >= 1 && dsize <= MAX_DSIZE);

   // Is it possible to merge this write with the preceding read?
   lastEvt = &events[events_used-1];
   if (events_used > 0
       && lastEvt->ekind == Event_Dr
       && lastEvt->size  == dsize
       && lastEvt->guard == NULL
       && eqIRAtom(lastEvt->addr, daddr))
   {
      lastEvt->ekind = Event_Dm;
      return;
   }

   // No.  Add as normal.
   if (events_used == N_EVENTS)
      flushEvents(sb);
   tl_assert(events_used >= 0 && events_used < N_EVENTS);
   evt = &events[events_used];
   evt->ekind = Event_Dw;
   evt->size  = dsize;
   evt->addr  = daddr;
   evt->guard = NULL;
   events_used++;
}

/**
 * @brief sort Time
 */
static
Int time_compare (const Time **t1, const Time **t2)
{
   if (((long)**t1) > ((long)**t2)) return 1;
   if (((long)**t1) < ((long)**t2)) return -1;
   return 0;
}

static
void ws_post_clo_init(void)
{
   // ensure we have a separate file for every process
   const HChar *append = ".%p";
   const int appendlen = (VG_(strstr) (clo_filename, "%p") == NULL) ? VG_(strlen) (append) : 0;
   int_filename = (HChar*) VG_(malloc) (VG_(strlen) (clo_filename) + 1 + appendlen);
   VG_(strcpy) (int_filename, clo_filename);
   if (appendlen > 0) VG_(strcat) (int_filename, append);
   VG_(umsg)("Output file: %s\n", int_filename);

   // check intervals and times
   if (clo_tau == 0) clo_tau = clo_every;
   if (clo_time_unit != TimeI) {
      VG_(umsg)("Warning: time unit %s not implemented, yet. Fallback to instructions",
                TimeUnit_to_string(clo_time_unit));
      clo_time_unit = TimeI;
   }

   // user list of times for sample info
   {
      // parse
      const HChar *pt = clo_info_at;
      UInt num;
      while (VG_(parse_UInt) (&pt, &num)) {
         Time *it = VG_(malloc) (sizeof(Time));
         if (it) {
            *it = (Time)num;
            VG_(addToXA) (ws_info_times, &it);
         }
         while (*pt == ',' || *pt == ' ') pt++;
      }
      // sort and un-dupe
      VG_(setCmpFnXA) (ws_info_times, (XACmpFn_t) time_compare);
      VG_(sortXA) (ws_info_times);
      int num_t = VG_(sizeXA) (ws_info_times);
      Time pre;
      for (int i = 0; i < num_t; /* in loop*/) {
         Time **cur = VG_(indexXA)(ws_info_times, i);
         if (i > 0 && pre == **cur) {
            pre = **cur;
            VG_(removeIndexXA) (ws_info_times, i);
            num_t--;
         } else {
            pre = **cur;
            ++i;
         }
      }
      // summarize
      num_t = VG_(sizeXA) (ws_info_times);
      if (num_t > 0) {
         next_user_time_idx = 0;
         VG_(umsg) ("Recording info at user times: ");
         for (int i = 0; i < num_t; i++) {
            Time **t = VG_(indexXA)(ws_info_times, i);
            VG_(umsg) ("%ld ", (long)**t);
         }
         VG_(umsg) ("\n");
      }
   }

   // peak filters
   init_peakd(&pd_data);
   init_peakd(&pd_insn);

   // locality trackers
   init_locality(&locality_data);
   init_locality(&locality_insn);

   // verbose a bit
   VG_(umsg)("Page size = %d bytes\n", clo_pagesize);
   VG_(umsg)("Computing WS every %d %s\n", clo_every,
             TimeUnit_to_string(clo_time_unit));
   VG_(umsg)("Considering references in past %d %s\n", clo_tau,
             TimeUnit_to_string(clo_time_unit));
}

static
void add_counter_update(IRSB* sbOut, Int n)
{
   #if defined(VG_BIGENDIAN)
   # define END Iend_BE
   #elif defined(VG_LITTLEENDIAN)
   # define END Iend_LE
   #else
   # error "Unknown endianness"
   #endif
   // Add code to increment 'guest_instrs_executed' by 'n', like this:
   //   WrTmp(t1, Load64(&guest_instrs_executed))
   //   WrTmp(t2, Add64(RdTmp(t1), Const(n)))
   //   Store(&guest_instrs_executed, t2)
   IRTemp t1 = newIRTemp(sbOut->tyenv, Ity_I64);
   IRTemp t2 = newIRTemp(sbOut->tyenv, Ity_I64);
   IRExpr* counter_addr = mkIRExpr_HWord( (HWord)&guest_instrs_executed );

   IRStmt* st1 = IRStmt_WrTmp(t1, IRExpr_Load(END, Ity_I64, counter_addr));
   IRStmt* st2 =
      IRStmt_WrTmp(t2,
                   IRExpr_Binop(Iop_Add64, IRExpr_RdTmp(t1),
                                           IRExpr_Const(IRConst_U64(n))));
   IRStmt* st3 = IRStmt_Store(END, counter_addr, IRExpr_RdTmp(t2));

   addStmtToIRSB( sbOut, st1 );
   addStmtToIRSB( sbOut, st2 );
   addStmtToIRSB( sbOut, st3 );
}

// iterate pages and count those accessed within (now_time - tau, now_time)
static
unsigned long recently_used_pages(VgHashTable *ht, Time now_time)
{
   unsigned long cnt = 0;

   Time tmin = 0;
   if (clo_tau < now_time) tmin = now_time - clo_tau;

   VG_(HT_ResetIter)(ht);
   const VgHashNode *nd;
   while ((nd = VG_(HT_Next)(ht))) {
      const struct map_pageaddr *page = (const struct map_pageaddr *) nd;
      if (page->last_access > tmin) cnt++;
   }
   return cnt;
}

/**
 * @brief actually assemble callstack string
 * * XXX: do not change this function without according changes in strstack_maxlen()
 */
static
void strstack_make
(UInt n, DiEpoch ep, Addr ip, void* uu_opaque)
{
   callstack_string *cs = (callstack_string*) uu_opaque;

   // inlining
   InlIPCursor *iipc = VG_(new_IIPC)(ep, ip);
   do {
      const HChar *fname;
      UInt         line;
      if (VG_(get_filename_linenum)(ep, ip, &fname, NULL, &line)) {
         // format: "%s:%u"
         VG_(strncat) (cs->str, fname, cs->rem); cs->rem -= VG_(strlen)(fname);
         VG_(strncat) (cs->str, ":", cs->rem); cs->rem -= 1;
         cs->rem -= VG_(snprintf) (cs->str + VG_(strlen) (cs->str), cs->rem, "%u", line);
      }
      // separator
      VG_(strncat) (cs->str, "|", cs->rem); cs->rem -= 1;
   } while (VG_(next_IIPC)(iipc));
}

/**
 * @brief determine max. length of callstack string
 * XXX: do not change this function without according changes in strstack_make()
 */
static
void strstack_maxlen(UInt n, DiEpoch ep, Addr ip, void* uu_opaque)
{
   callstack_string *cs = (callstack_string*) uu_opaque;

   // inlining
   InlIPCursor *iipc = VG_(new_IIPC)(ep, ip);
   do {
      const HChar *fname;
      UInt         line;
      if (VG_(get_filename_linenum)(ep, ip, &fname, NULL, &line)) {
         // format: "%s:%u"
         HChar buf[8];
         cs->rem += VG_(strlen) (fname);
         cs->rem += 1;
         cs->rem += VG_(snprintf) (buf, sizeof(buf), "%u", line);
      }
      cs->rem += 1;  // trailing separator or NULL terminator.
   } while (VG_(next_IIPC)(iipc));
   VG_(delete_IIPC)(iipc);
}

/**
 * @brief get callstack as string
 */
static
HChar* get_callstack(ExeContext *ec)
{
   callstack_string cs;
   const DiEpoch ep = VG_(get_ExeContext_epoch) (ec);

   // pre-calculate length of string
   cs.rem = 0;
   VG_(apply_ExeContext) (strstack_maxlen, (void*)&cs, ep, ec);

   cs.rem += 1; // better than sorry
   cs.str = (HChar*) VG_(malloc) (cs.rem * sizeof(HChar));
   cs.str[0] = 0;

   // assemble string
   VG_(apply_ExeContext) (strstack_make, (void*)&cs, ep, ec);
   cs.str[VG_(strlen) (cs.str) - 1] = 0;  // remove trailing separator

   return cs.str;
}

/**
 * @brief record additional information about process right now
 */
static
void record_sample_info(Time now_time)
{
   SampleContext *wsp = VG_(malloc) (sizeof(*wsp));
   if (wsp) {
      wsp->t = now_time;
      if (!postmortem) {
         ThreadId tid = VG_(get_running_tid)();
         wsp->ec = VG_(record_ExeContext)(tid, 0);
      } else {
         wsp->ec = VG_(null_ExeContext)();
      }
      VG_(addToXA) (ws_context_list, &wsp);
   }
}

static
void compute_ws(Time now_time)
{
   /*********
    * WSS
    *********/
   WorkingSet *ws = VG_(malloc) (sizeof(WorkingSet));
   if (!ws) {
      drop_samples++;
      return;
   }
   ws->t = now_time;
   ws->pages_insn = recently_used_pages (ht_insn, now_time);
   ws->pages_data = recently_used_pages (ht_data, now_time);
   VG_(addToXA) (ws_at_time, &ws);

   /*********
    * INFO
    *********/
   Bool have_info = False;

   if (next_user_time_idx >= 0) {
      Time **nextt= VG_(indexXA) (ws_info_times, next_user_time_idx);
      if (now_time >= **nextt) {
         record_sample_info (now_time);
         have_info = True;
         // go to next one that is in the future (they might be too dense for --ws-every)
         do {
            if (next_user_time_idx < VG_(sizeXA)(ws_info_times) - 1) {
               next_user_time_idx++;
            } else {
               next_user_time_idx = -1;
            }
            if (next_user_time_idx < 0) break;
            nextt = VG_(indexXA) (ws_info_times, next_user_time_idx);
         } while (**nextt < now_time);
      }
   }

   if (clo_peakdetect) {
      // both peak detect have to run every sample for uniformity, thus no short-circuit eval
      const Bool pk_data = peak_detect(&pd_data, ws->pages_data);
      const Bool pk_insn = peak_detect(&pd_insn, ws->pages_insn);
      if (!have_info && (pk_data || pk_insn)) {
         record_sample_info (now_time);
      }
      #ifdef DEBUG
         ws->mAvg = pd_data.movingAvg;
         ws->mVar = pd_data.movingVar;
      #endif
   }
}

static
void maybe_compute_ws (void)
{
   tl_assert(clo_time_unit == TimeI);

   // if 'every' time units have passed, determine working set again
   static Time earliest_possible_time_of_next_ws = 0;
   Time now_time = get_time();
   if (now_time < earliest_possible_time_of_next_ws) return;

   compute_ws (now_time);

   earliest_possible_time_of_next_ws = now_time + clo_every;
}

// We increment the instruction count in two places:
// - just before any Ist_Exit statements;
// - just before the IRSB's end.
// In the former case, we zero 'n' and then continue instrumenting.
static
IRSB* ws_instrument ( VgCallbackClosure* closure,
                      IRSB* sbIn,
                      const VexGuestLayout* layout,
                      const VexGuestExtents* vge,
                      const VexArchInfo* archinfo_host,
                      IRType gWordTy, IRType hWordTy )
{
   Int        i, ninsn;
   IRSB*      sbOut;
   IRTypeEnv* tyenv = sbIn->tyenv;

   if (gWordTy != hWordTy) {
      /* We don't currently support this case. */
      VG_(tool_panic)("host/guest word size mismatch");
   }

   sbOut = deepCopyIRSBExceptStmts(sbIn);

   // Copy verbatim any IR preamble preceding the first IMark
   i = 0;
   while (i < sbIn->stmts_used && sbIn->stmts[i]->tag != Ist_IMark) {
      addStmtToIRSB( sbOut, sbIn->stmts[i] );
      i++;
   }

   events_used = ninsn = 0;
   // instrument accesses and insn counter, if needed
   for (/*use current i*/; i < sbIn->stmts_used; i++) {
      IRStmt* st = sbIn->stmts[i];
      if (!st || st->tag == Ist_NoOp) continue;

      switch (st->tag) {
         case Ist_NoOp:
         case Ist_AbiHint:
         case Ist_Put:
         case Ist_PutI:
         case Ist_MBE:
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_IMark:
            if (clo_time_unit == TimeI) ninsn++;
            addEvent_Ir( sbOut, mkIRExpr_HWord( (HWord)st->Ist.IMark.addr ),
                         st->Ist.IMark.len );
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_WrTmp:
            {
               IRExpr* data = st->Ist.WrTmp.data;
               if (data->tag == Iex_Load) {
                   addEvent_Dr( sbOut, data->Iex.Load.addr,
                                sizeofIRType(data->Iex.Load.ty) );
               }
            }
            addStmtToIRSB( sbOut, st );
            break;

         case Ist_Store: {
            IRExpr* data = st->Ist.Store.data;
            IRType  type = typeOfIRExpr(tyenv, data);
            tl_assert(type != Ity_INVALID);
            addEvent_Dw( sbOut, st->Ist.Store.addr,
                         sizeofIRType(type) );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_StoreG: {
            IRStoreG* sg   = st->Ist.StoreG.details;
            IRExpr*   data = sg->data;
            IRType    type = typeOfIRExpr(tyenv, data);
            tl_assert(type != Ity_INVALID);
            addEvent_Dw_guarded( sbOut, sg->addr,
                                 sizeofIRType(type), sg->guard );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LoadG: {
            IRLoadG* lg       = st->Ist.LoadG.details;
            IRType   type     = Ity_INVALID; /* loaded type */
            IRType   typeWide = Ity_INVALID; /* after implicit widening */
            typeOfIRLoadGOp(lg->cvt, &typeWide, &type);
            tl_assert(type != Ity_INVALID);
            addEvent_Dr_guarded( sbOut, lg->addr,
                                 sizeofIRType(type), lg->guard );
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_Dirty: {
            {
               Int      dsize;
               IRDirty* d = st->Ist.Dirty.details;
               if (d->mFx != Ifx_None) {
                  // This dirty helper accesses memory.  Collect the details.
                  tl_assert(d->mAddr != NULL);
                  tl_assert(d->mSize != 0);
                  dsize = d->mSize;
                  if (d->mFx == Ifx_Read || d->mFx == Ifx_Modify)
                     addEvent_Dr( sbOut, d->mAddr, dsize );
                  if (d->mFx == Ifx_Write || d->mFx == Ifx_Modify)
                     addEvent_Dw( sbOut, d->mAddr, dsize );
               } else {
                  tl_assert(d->mAddr == NULL);
                  tl_assert(d->mSize == 0);
               }
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_CAS: {
            /* We treat it as a read and a write of the location.  I
               think that is the same behaviour as it was before IRCAS
               was introduced, since prior to that point, the Vex
               front ends would translate a lock-prefixed instruction
               into a (normal) read followed by a (normal) write. */
            Int    dataSize;
            IRType dataTy;
            IRCAS* cas = st->Ist.CAS.details;
            tl_assert(cas->addr != NULL);
            tl_assert(cas->dataLo != NULL);
            dataTy   = typeOfIRExpr(tyenv, cas->dataLo);
            dataSize = sizeofIRType(dataTy);
            if (cas->dataHi != NULL)
               dataSize *= 2; /* since it's a doubleword-CAS */
            addEvent_Dr( sbOut, cas->addr, dataSize );
            addEvent_Dw( sbOut, cas->addr, dataSize );

            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_LLSC: {
            IRType dataTy;
            if (st->Ist.LLSC.storedata == NULL) {
               /* LL */
               dataTy = typeOfIRTemp(tyenv, st->Ist.LLSC.result);
               addEvent_Dr( sbOut, st->Ist.LLSC.addr,
                                   sizeofIRType(dataTy) );
               /* flush events before LL, helps SC to succeed */
               flushEvents(sbOut);
            } else {
               /* SC */
               dataTy = typeOfIRExpr(tyenv, st->Ist.LLSC.storedata);
               addEvent_Dw( sbOut, st->Ist.LLSC.addr,
                            sizeofIRType(dataTy) );
            }
            addStmtToIRSB( sbOut, st );
            break;
         }

         case Ist_Exit:
            if (clo_time_unit == TimeI && ninsn > 0) {
               // Add an increment before the Exit statement, then reset 'n'.
               add_counter_update(sbOut, ninsn);
               ninsn = 0;
            }
            flushEvents(sbOut);
            addStmtToIRSB( sbOut, st );      // Original statement
            break;

         default:
            ppIRStmt(st);
            tl_assert(0);
      }
   }

   /* At the end of the sbIn.  Flush outstandings. */
   if (clo_time_unit == TimeI && ninsn > 0) {
      add_counter_update(sbOut, ninsn);
   }
   flushEvents(sbOut);

   return sbOut;
}

/**
 * @brief sort page addr by ref count
 */
static
Int map_pageaddr_compare (const void *p1, const void *p2)
{
   const struct map_pageaddr * const *a1 = (const struct map_pageaddr * const *) p1;
   const struct map_pageaddr * const *a2 = (const struct map_pageaddr * const *) p2;

   if ((*a1)->count > (*a2)->count) return -1;
   if ((*a1)->count < (*a2)->count) return 1;
   return 0;
}

/**
 * @brief sort SampleInfo by id
 */
static
Int map_context2sampleinfo_compare (const void *p1, const void *p2)
{
   const struct map_context2sampleinfo * const *a1 = (const struct map_context2sampleinfo * const *) p1;
   const struct map_context2sampleinfo * const *a2 = (const struct map_context2sampleinfo * const *) p2;

   if ((*a1)->info.id > (*a2)->info.id) return 1;
   if ((*a1)->info.id < (*a2)->info.id) return -1;
   return 0;
}

static
void print_page_list(VgHashTable *ht, VgFile *fp)
{
   int nentry = VG_(HT_count_nodes) (ht);
   VG_(fprintf) (fp, "%'d entries:\n", nentry);

   // sort
   struct map_pageaddr **res;
   int nres = 0;
   res = VG_(malloc) (nentry * sizeof (*res));
   VG_(HT_ResetIter)(ht);
   VgHashNode *nd;
   while ((nd = VG_(HT_Next)(ht)))
      res[nres++] = (struct map_pageaddr *) nd;
   VG_(ssort) (res, nres, sizeof (res[0]), map_pageaddr_compare);

   // print
   VG_(fprintf) (fp, "%8s %20s %14s", "count", "page", "last-accessed");
   if (ht == ht_insn && clo_locations) VG_(fprintf) (fp, " location");
   for (int i = 0; i < nres; ++i)
   {
      VG_(fprintf) (fp, "\n%8lu %018p %14llu",
                    res[i]->count,
                    (void*)res[i]->top.key,
                    res[i]->last_access);
      if (ht == ht_insn && clo_locations) {
         const HChar *where = VG_(describe_IP) (res[i]->ep, res[i]->top.key, NULL);
         VG_(fprintf) (fp, " %s", where);
      }
   }
   VG_(fprintf) (fp, "\n");
   VG_(free) (res);
}

static
void print_sample_info(VgHashTable *ht, VgFile *fp)
{
   int nres = 0;
   int nentry = VG_(HT_count_nodes) (ht);
   // sort
   struct map_context2sampleinfo **res;
   res = VG_(malloc) (nentry * sizeof (*res));  // ptr to ptr
   VG_(HT_ResetIter)(ht);
   VgHashNode *nd;
   while ((nd = VG_(HT_Next)(ht)))
      res[nres++] = (struct map_context2sampleinfo *) nd;
   VG_(ssort) (res, nres, sizeof (res[0]), map_context2sampleinfo_compare);

   // print sorted
   for (int i = 0; i < nres; ++i) {
      struct map_context2sampleinfo *pi = (struct map_context2sampleinfo*) res[i];
      VG_(fprintf) (fp, "[%4d] refs=%u, loc=%s\n", pi->info.id, pi->info.cnt, pi->info.callstack);
   }
   VG_(free) (res);
}

static
void print_access_stats(VgHashTable *ht, VgFile *fp)
{
   const long unsigned int num = (long unsigned int) VG_(HT_count_nodes) (ht);
   VG_(HT_ResetIter)(ht);
   VgHashNode *nd;
   unsigned long long access = 0;
   while ((nd = VG_(HT_Next)(ht))) {
      struct map_pageaddr *pg = (struct map_pageaddr *) nd;
      access += pg->count;
   }

   UInt kB = (UInt)((num * clo_pagesize) / 1024.f);
   Float acc =  ((Float) access) / num;
   VG_(fprintf) (fp, "pages/access:  %'lu pages (%'u kB)/%'u accesses per page",
                 num, kB, (UInt) acc);
}

static
void print_ws_over_time(XArray *xa, VgHashTable *ht_sampleinfo, VgFile *fp)
{
   // header
   VG_(fprintf) (fp, "%12s %8s %8s", "t", "WSS_insn", "WSS_data");
   if (VG_(HT_count_nodes) (ht_sampleinfo) > 0) {
      VG_(fprintf) (fp, " info");
   }
   if (clo_peakdetect) {
      #ifdef DEBUG
         VG_(fprintf) (fp, " %12s %12s", "mAvg", "mVar");
      #endif
   }
   VG_(fprintf) (fp, "\n");

   // sample info
   const int n_info = VG_(sizeXA)(ws_context_list);
   int info_id = 0;
   SampleContext **next_info = (n_info > 0) ? VG_(indexXA)(ws_context_list, info_id++) : NULL;

   // data points
   const int num_t = VG_(sizeXA)(xa);
   unsigned long peak_i = 0, peak_d = 0;
   unsigned long long sum_i = 0, sum_d = 0;
   for (int i = 0; i < num_t; i++) {
      WorkingSet **ws = VG_(indexXA)(xa, i);
      const unsigned long t = (unsigned long)(*ws)->t;
      const unsigned long pi = (*ws)->pages_insn;
      const unsigned long pd = (*ws)->pages_data;

      // track stats
      sum_i += pi;
      sum_d += pd;
      if (pi > peak_i) peak_i = pi;
      if (pd > peak_d) peak_d = pd;

      // sample info, if present
      if (VG_(HT_count_nodes) (ht_sampleinfo) > 0) {
         char strinfo[5];
         if (next_info && (*next_info)->t == t) {
            const UInt ecid = VG_(get_ECU_from_ExeContext)((*next_info)->ec);
            struct map_context2sampleinfo *pki = VG_(HT_lookup) (ht_ec2sampleinfo, ecid);
            tl_assert(pki != NULL);
            VG_(snprintf) (strinfo, sizeof(strinfo), "%d", pki->info.id);
            next_info = (info_id < n_info) ? VG_(indexXA)(ws_context_list, info_id++) : NULL;
         } else {
            VG_(snprintf) (strinfo, sizeof(strinfo), "-");
         }
         VG_(fprintf) (fp, "%12lu %8lu %8lu %4s", t, pi, pd, strinfo);

      } else {
         VG_(fprintf) (fp, "%12lu %8lu %8lu", t, pi, pd);
      }

      if (clo_peakdetect) {
         #ifdef DEBUG
            VG_(fprintf) (fp, " %10.1f %10.1f", (*ws)->mAvg, (*ws)->mVar);
         #endif
      }
      VG_(fprintf) (fp, "\n");
   }

   const Float avg_i = ((Float)sum_i) / (num_t - 1);
   const Float avg_d = ((Float)sum_d) / (num_t - 1);
   VG_(fprintf) (fp, "\nInsn WSS avg/peak:  %'.1f/%'lu pages (%'u/%'u kB)",
                 avg_i, peak_i,
                 (unsigned int)((avg_i * clo_pagesize) / 1024.f),
                 (unsigned int)((peak_i * clo_pagesize) / 1024.f));
   VG_(fprintf) (fp, "\nData WSS avg/peak:  %'.1f/%'lu pages (%'u/%'u kB)",
                 avg_d, peak_d,
                 (unsigned int)((avg_d * clo_pagesize) / 1024.f),
                 (unsigned int)((peak_d * clo_pagesize) / 1024.f));

   VG_(fprintf) (fp, "\nInsn ");
   print_access_stats (ht_insn, fp);
   VG_(fprintf) (fp, "\nData ");
   print_access_stats (ht_data, fp);
}

/**
 * @brief go over list of sample info and make list of unique info
 * @return number of unique information
 */
static
unsigned long compute_sample_info(XArray *xa)
{
   unsigned long num_unique = 0;

   const int num_t = VG_(sizeXA)(xa);
   for (int i = 0; i < num_t; i++) {
      SampleContext **wsp = VG_(indexXA)(xa, i);
      const UInt ecid = VG_(get_ECU_from_ExeContext)((*wsp)->ec);
      struct map_context2sampleinfo *pi = VG_(HT_lookup) (ht_ec2sampleinfo, ecid);
      if (pi == NULL) {
         num_unique++;
         HChar *strcs = get_callstack((*wsp)->ec);
         pi = VG_(malloc) (sizeof(*pi));
         pi->top.key = ecid;
         pi->info.id = VG_(HT_count_nodes)(ht_ec2sampleinfo);
         pi->info.callstack = strcs;
         pi->info.cnt = 1;
         VG_(HT_add_node) (ht_ec2sampleinfo, (VgHashNode *) pi);

      } else {
         pi->info.cnt++;
      }
   }
   return num_unique;
}

static
void free_sample_info(void *arg)
{
   struct map_context2sampleinfo *pi = (struct map_context2sampleinfo*) arg;
   VG_(free) (pi->info.callstack);
   VG_(free) (arg);
}

static
void print_locality_stats(VgFile *fp)
{
   VG_(fprintf) (fp, "Insn refs/avg dist: %'lu/%'lu\n",
                 locality_insn.n,
                 (unsigned long) (locality_insn.sum / ((Float)locality_insn.n)));
   VG_(fprintf) (fp, "Data refs/avg dist: %'lu/%'lu\n",
                 locality_data.n,
                 (unsigned long) (locality_data.sum / ((Float)locality_data.n)));
}

static
void ws_fini(Int exitcode)
{
   // force one last data point
   postmortem = True;
   compute_ws(get_time());

   VG_(umsg)("Number of instructions: %'lu\n", (unsigned long) guest_instrs_executed);
   VG_(umsg)("Number of samples:      %'lu\n", VG_(sizeXA) (ws_at_time));
   VG_(umsg)("Dropped samples:        %'lu\n", drop_samples);

   HChar* outfile = VG_(expand_file_name)("--ws-file", int_filename);
   VG_(umsg)("Writing results to file '%s'\n", outfile);
   VgFile *fp = VG_(fopen)(outfile, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY,
                                    VKI_S_IRUSR|VKI_S_IWUSR);
   if (fp == NULL) {
      // If the file can't be opened for whatever reason, give up now.
      VG_(umsg)("error: can't open simulation output file '%s'\n",
                outfile );
      VG_(umsg)("       ... so simulation results will be missing.\n");
      VG_(free)(outfile);
      return;
   } else {
      VG_(free)(outfile);
   }

   if (fp != NULL) {
      // show preamble
      VG_(fprintf) (fp, "Working Set Measurement by valgrind-%s-%s\n\n", WS_NAME, WS_VERSION);
      VG_(fprintf) (fp, "Command:        %s", VG_(args_the_exename));
      for (int i = 0; i < VG_(sizeXA)( VG_(args_for_client) ); i++) {
         HChar* arg = * (HChar**) VG_(indexXA)( VG_(args_for_client), i );
         VG_(fprintf)(fp, " %s", arg);
      }
      VG_(fprintf) (fp, "\nInstructions:   %'lu\n", (unsigned long) guest_instrs_executed);
      VG_(fprintf) (fp, "Page size:      %d B\n", clo_pagesize);
      VG_(fprintf) (fp, "Time Unit:      %s\n", TimeUnit_to_string(clo_time_unit));
      VG_(fprintf) (fp, "Every:          %'d units\n", clo_every);
      VG_(fprintf) (fp, "Tau:            %'d units\n\n", clo_tau);
      if (clo_peakdetect) {
         VG_(fprintf) (fp, "Peak window:    %'d\n", clo_peakwindow);
         VG_(fprintf) (fp, "Peak threshold: %d\n", clo_peakthresh);
         VG_(fprintf) (fp, "Peak adaptrate: %.1f\n", clo_peakadapt);
      }
      VG_(fprintf) (fp, "--\n\n");

      // show page listing
      if (clo_listpages) {
         VG_(fprintf) (fp, "Code pages, ");
         print_page_list (ht_insn, fp);
         VG_(fprintf) (fp, "\nData pages, ");
         print_page_list (ht_data, fp);
         VG_(fprintf) (fp, "\n--\n\n");
      }

      // compute sample info
      const unsigned long ninfo = compute_sample_info(ws_context_list);
      VG_(umsg)("Number of info/unique: %lu/%lu\n", VG_(sizeXA)(ws_context_list), ninfo);

      // show working set data
      VG_(fprintf) (fp, "Working sets:\n");
      print_ws_over_time (ws_at_time, ht_ec2sampleinfo, fp);
      VG_(fprintf) (fp, "\n--\n\n");

      // show sample info.
      if (VG_(HT_count_nodes) (ht_ec2sampleinfo) > 0) {
         VG_(fprintf) (fp, "Sample info:\n");
         print_sample_info (ht_ec2sampleinfo, fp);
         VG_(fprintf) (fp, "\n");
         VG_(fprintf) (fp, "Number of info/unique: %lu/%lu", VG_(sizeXA)(ws_context_list), ninfo);
         VG_(fprintf) (fp, "\n--\n\n");
      }

      // locality info
      if (clo_localitytr) {
         VG_(fprintf) (fp, "Locality statistics:\n");
         print_locality_stats (fp);
         VG_(fprintf) (fp, "\n--\n\n");
      }
   }

   // cleanup
   VG_(fclose)(fp);
   VG_(HT_destruct) (ht_data, VG_(free));
   VG_(HT_destruct) (ht_insn, VG_(free));
   VG_(HT_destruct) (ht_ec2sampleinfo, free_sample_info);
   VG_(deleteXA) (ws_at_time);
   VG_(deleteXA) (ws_context_list);
   VG_(deleteXA) (ws_info_times);
   if (int_filename != clo_filename) VG_(free) ((void*)int_filename);
   VG_(umsg)("ws finished\n");
}

static
void ws_pre_clo_init(void)
{
   VG_(details_name)            (WS_NAME);
   VG_(details_version)         (WS_VERSION);
   VG_(details_description)     (WS_DESC);
   VG_(details_copyright_author)("Copyright (C) 2018, and GNU GPL'd, by Martin Becker.");
   VG_(details_bug_reports_to)  (VG_BUGS_TO);
   VG_(details_avg_translation_sizeB) (200);

   VG_(basic_tool_funcs)          (ws_post_clo_init,
                                   ws_instrument,
                                   ws_fini);
   VG_(needs_command_line_options)(ws_process_cmd_line_option,
                                   ws_print_usage,
                                   ws_print_debug_usage);

   ht_data          = VG_(HT_construct) ("ht_data");
   ht_insn          = VG_(HT_construct) ("ht_insn");
   ht_ec2sampleinfo = VG_(HT_construct) ("ht_ec2sampleinfo");
   ws_at_time       = VG_(newXA) (VG_(malloc), "arr_ws",   VG_(free), sizeof(WorkingSet*));
   ws_context_list  = VG_(newXA) (VG_(malloc), "arr_info", VG_(free), sizeof(SampleContext*));
   ws_info_times    = VG_(newXA) (VG_(malloc), "arr_time", VG_(free), sizeof(Time*));
}

VG_DETERMINE_INTERFACE_VERSION(ws_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                ws_main.c ---*/
/*--------------------------------------------------------------------*/
