
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
#include "pub_tool_machine.h"     // VG_(fnptr_to_fnentry)
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
   #define WS_EPOCH 1
#else
   #undef WS_EPOCH
   #error "This valgrind version is too old"
#endif

#ifdef WS_EPOCH
   #define EPOCH_ARG_NOCOMMA(x) x
   #define EPOCH_ARG(x) EPOCH_ARG_NOCOMMA(x),
#else
   #define EPOCH_ARG_NOCOMMA(x) /* nix */
   #define EPOCH_ARG(x) EPOCH_ARG_NOCOMMA(x)
#endif

/*------------------------------------------------------------*/
/*--- tool info                                            ---*/
/*------------------------------------------------------------*/

#define WS_NAME "ws"
#define WS_VERSION "0.1"
#define WS_DESC "compute working set for data and instructions"

/*------------------------------------------------------------*/
/*--- macros                                               ---*/
/*------------------------------------------------------------*/
#define FABS(x) ((x < 0.f) ? -x : x)

/*------------------------------------------------------------*/
/*--- type definitions                                     ---*/
/*------------------------------------------------------------*/

typedef unsigned long pagecount;

struct map_pageaddr
{
  VgHashNode        top;  // page address, must be first
  unsigned long int count;
  Time              last_access;
  #ifdef WS_EPOCH
  DiEpoch           ep;
  #endif
};

#define vgPlain_malloc(size) vgPlain_malloc ((const char *) __func__, size)

typedef enum { TimeI, TimeMS } TimeUnit;

typedef
   IRExpr
   IRAtom;

/* --- Operations --- */

typedef enum { OpLoad=0, OpStore=1, OpAlu=2 } Op;

#define MAX_DSIZE    512

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
      Time t;
      pagecount pages_insn;
      pagecount pages_data;
      //int       stackid;    ///< if peak-detect is on and this wset is a peak, then this is the ID of the according call stack
   #ifdef DEBUG
      float     mAvg, mVar;
   #endif
   }
   WorkingSet;

/**
 * @brief details for a WSS peak
 */
typedef
   struct {
      unsigned int  id;
      unsigned int  cnt;
      HChar        *callstack;
   }
   PeakInfo;

/**
 * @brief element in hash table ExeContext -> PeakInfo.
 */
struct map_peakinfo
{
  VgHashNode top;   // ExeContext ECU, must be first
  PeakInfo   info;
};


/**
 * @brief element in list of WSS peaks
 */
typedef
   struct {
      Time        t;
      ExeContext *ec;
   }
   WorkingSetPeak;


/**
 * @brief internal data of peak detector
 */
typedef
   struct {
      // states
      unsigned  k;
      short int peak_pre;
      float     filt_pre;
      float     movingAvg;
      float     movingVar;
      // params
      unsigned  window;
      float     thresh;     ///< min. z-score to be counted as peak
      float     influence;  ///< coefficient for filtering out peaks
      float     exp_alpha;  ///< coefficient for exponential moving filters
   }
   PeakDetect;

/*------------------------------------------------------------*/
/*--- prototypes                                           ---*/
/*------------------------------------------------------------*/

static void maybe_compute_ws (void);

/*------------------------------------------------------------*/
/*--- globals                                              ---*/
/*------------------------------------------------------------*/

static Bool postmortem = False;  ///< certain actions we cannot do after process terminated
static Long guest_instrs_executed = 0;
static unsigned long num_samples = 0;
static unsigned long drop_samples = 0;

// page access tables
static VgHashTable *ht_data;
static VgHashTable *ht_insn;
static VgHashTable *ht_ec2peakinfo;

// working set at each point in time
static XArray *ws_at_time;

// peak detect results
static XArray *ws_peak_list;

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
#define WS_DEFAULT_PEAKT 3
#define WS_DEFAULT_PEAKW 30
#define WS_DEFAULT_PEAKINFL 0.5f ///< default value for peak filter

// user inputs:
static Bool  clo_locations  = True;
static Bool  clo_listpages  = False;
static Bool  clo_peakdetect = False;
static Int   clo_peakthresh = WS_DEFAULT_PEAKT;  // FIXME: float?
static Int   clo_peakwindow = WS_DEFAULT_PEAKW;
static Float clo_peakinfl   = WS_DEFAULT_PEAKINFL;  // FIXME: from clo
static Int   clo_pagesize   = WS_DEFAULT_PS;
static Int   clo_every      = WS_DEFAULT_EVERY;
static Int   clo_tau        = 0;
static Int   clo_time_unit  = TimeI;
//static Int   clo_stackdepth = 30;

/* The name of the function of which the number of calls (under
 * --basic-counts=yes) is to be counted, with default. Override with command
 * line option --fnname. */
static const HChar* clo_fnname = "main";
static const HChar* clo_filename = "ws.out.%p";
static HChar* int_filename;

static Bool ws_process_cmd_line_option(const HChar* arg)
{
   if VG_BOOL_CLO(arg, "--ws-locations", clo_locations) {}
   else if VG_BOOL_CLO(arg, "--ws-list-pages", clo_listpages) {}
   else if VG_STR_CLO(arg, "--ws-file", clo_filename) {}
   else if VG_INT_CLO(arg, "--ws-pagesize", clo_pagesize) {}
   else if VG_INT_CLO(arg, "--ws-every", clo_every) {}
   else if VG_INT_CLO(arg, "--ws-tau", clo_tau) { tl_assert(clo_tau > 0); }
   else if VG_XACT_CLO(arg, "--ws-time-unit=i", clo_time_unit, TimeI)  {}
   else if VG_XACT_CLO(arg, "--ws-time-unit=ms", clo_time_unit, TimeMS) {}
   else if VG_BOOL_CLO(arg, "--ws-peak-detect", clo_peakdetect) {}
   else if VG_INT_CLO(arg, "--ws-peak-window", clo_peakwindow) { tl_assert(clo_peakwindow > 0); }
   else if VG_INT_CLO(arg, "--ws-peak-thresh", clo_peakthresh) { tl_assert(clo_peakthresh > 0); }
   else return False;

   tl_assert(clo_fnname);
   tl_assert(clo_fnname[0]);
   tl_assert(clo_pagesize > 0);
   tl_assert(clo_every > 0);
   return True;
}

static void ws_print_usage(void)
{
   VG_(printf)(
"    --ws-file=<string>        file name to write results\n"
"    --ws-list-pages=no|yes    print list of all accessed pages [no]\n"
"    --ws-locations=no|yes     collect location info for insn pages in listing [yes]\n"
"    --ws-peak-detect=no|yes   collect info for peaks in working set [no]\n"
"    --ws-peak-window=<int>    window length (in samples) for peak detection [%d]\n"
"    --ws-peak-thresh=<int>    threshold in multiples of variance for peaks [%d]\n"
"    --ws-pagesize=<int>       size of VM pages in bytes [%d]\n"
"    --ws-time-unit=i|ms       time unit: instructions executed (default), milliseconds\n"
"    --ws-every=<int>          sample WS every <int> time units [%d]\n"
"    --ws-tau=<int>            consider all accesses made in the last tau time units [%d]\n",
   WS_DEFAULT_PEAKW,
   WS_DEFAULT_PEAKT,
   WS_DEFAULT_PS,
   WS_DEFAULT_EVERY,
   WS_DEFAULT_TAU
   );
}

static void ws_print_debug_usage(void)
{
   VG_(printf)(
"    (none)\n"
   );
}

static const HChar* TimeUnit_to_string(TimeUnit time_unit)
{
   switch (time_unit) {
   case TimeI:  return "instructions";
   case TimeMS: return "ms";
   default:     tl_assert2(0, "TimeUnit_to_string: unrecognised TimeUnit");
   }
}

static void init_peakd(PeakDetect *pd) {
   pd->filt_pre = 0.f;
   pd->peak_pre = 0;
   pd->k = 0;
   pd->movingAvg = 0.f;
   pd->movingVar = 0.f;

   pd->window = clo_peakwindow;
   pd->exp_alpha = 2.f / (clo_peakwindow + 1);
   pd->influence = (float) clo_peakinfl;
   pd->thresh = (float) clo_peakthresh;
}

static Bool peak_detect(PeakDetect *pd, pagecount y) {
   short int pk = 0;
   float filt = (float) y;

   // detect peaks and filter them
   float y0 = y - pd->movingAvg;
   const Bool is_peak = FABS(y0) > (pd->thresh * pd->movingVar);
   if (pd->k >= pd->window && is_peak) {
      pk = (y > pd->movingAvg) ? 1 : -1;
      filt = pd->influence * y + (1 - pd->influence) * pd->filt_pre;
   }

   // moving variance (must be calc'd first)
   if (0 < pd->k) {
      const float diff = ((float) filt) - pd->movingAvg;  // XXX: important, previous average.
      pd->movingVar = (1.f - pd->exp_alpha) * (pd->movingVar + pd->exp_alpha * diff * diff);
   } else {
      pd->movingVar = 0.f;
   }

   // moving avg
   if (0 < pd->k) {
      pd->movingAvg = pd->exp_alpha * filt + (1.f - pd->exp_alpha) * pd->movingAvg;
   } else {
      pd->movingAvg = (float) filt;
   }

   if (pd->k < pd->window) pd->k++;
   pd->filt_pre = filt;

   Bool ret = pk != pd->peak_pre;
   pd->peak_pre = pk;
   return ret;
}

static Time get_time(void)
{
   // Get current time, in whatever time unit we're using.
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

static inline Addr pageaddr(Addr addr)
{
   return addr & ~(clo_pagesize-1);
}

// TODO: pages shared between processes?
static void pageaccess(Addr pageaddr, VgHashTable *ht) {

   struct map_pageaddr *page = VG_(HT_lookup) (ht, pageaddr);
   if (page == NULL) {
      page = VG_(malloc) (sizeof (*page));
      page->top.key = pageaddr;
      page->count = 0;
      VG_(HT_add_node) (ht, (VgHashNode *) page);
      //VG_(dmsg)("New page: %p\n", pageaddr);
   }

   page->count++;
   page->last_access = (long) get_time();
   #ifdef WS_EPOCH
   page->ep = VG_(current_DiEpoch)();
   #endif

   maybe_compute_ws();
}

static VG_REGPARM(2) void trace_data(Addr addr, SizeT size)
{
   const Addr pa = pageaddr(addr);
   pageaccess(pa, ht_data);
}

static VG_REGPARM(2) void trace_instr(Addr addr, SizeT size)
{
   const Addr pa = pageaddr(addr);
   pageaccess(pa, ht_insn);
}

static void flushEvents(IRSB* sb)
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

      // Add the helper.
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

static void addEvent_Ir ( IRSB* sb, IRAtom* iaddr, UInt isize )
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


/*------------------------------------------------------------*/
/*--- Basic tool functions                                 ---*/
/*------------------------------------------------------------*/

static void ws_post_clo_init(void)
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

   // peak filters
   init_peakd(&pd_data);
   init_peakd(&pd_insn);

   // verbose a bit
   VG_(umsg)("Page size = %d bytes\n", clo_pagesize);
   VG_(umsg)("Computing WS every %d %s\n", clo_every,
             TimeUnit_to_string(clo_time_unit));
   VG_(umsg)("Considering references in past %d %s\n", clo_tau,
             TimeUnit_to_string(clo_time_unit));
}

static void add_counter_update(IRSB* sbOut, Int n)
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

typedef
   struct {
      HChar *str;
      int    rem;
   }
   callstack_string;

/**
 * @brief actually assemble callstack string
 * * XXX: do not change this function without according changes in strstack_maxlen()
 */
static void strstack_make
(UInt n, EPOCH_ARG(DiEpoch ep) Addr ip, void* uu_opaque)
{
   callstack_string *cs = (callstack_string*) uu_opaque;

   // inlining
   InlIPCursor *iipc = VG_(new_IIPC)(EPOCH_ARG(ep) ip);
   do {
      const HChar *fname;
      UInt         line;
      if (VG_(get_filename_linenum)(ep, ip, &fname, NULL, &line)) {
         // format: "%s:%u"
         VG_(strcat) (cs->str, fname);
         VG_(strcat) (cs->str, ":");
         cs->rem -= VG_(strlen) (fname) + 1;
         cs->rem -= VG_(snprintf) (cs->str + VG_(strlen) (cs->str), cs->rem-1, "%u", line);
      }
      // separator
      VG_(strcat) (cs->str, "|");
      cs->rem -= 1;
   } while (VG_(next_IIPC)(iipc));
}

/**
 * @brief determine max. length of callstack string
 * XXX: do not change this function without according changes in strstack_make()
 */
static void strstack_maxlen
(UInt n, EPOCH_ARG(DiEpoch ep) Addr ip, void* uu_opaque)
{
   callstack_string *cs = (callstack_string*) uu_opaque;

   // inlining
   InlIPCursor *iipc = VG_(new_IIPC)(EPOCH_ARG(ep) ip);
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
static HChar* get_callstack(ExeContext *ec) {
   callstack_string cs;

   const DiEpoch ep = VG_(get_ExeContext_epoch) (ec);

   // pre-calculate length of string
   cs.rem = 0;
   VG_(apply_ExeContext) (strstack_maxlen, (void*)&cs, ep, ec);

   cs.str = (HChar*) VG_(malloc) ((cs.rem + 1) * sizeof(HChar));
   cs.str[0] = 0;

   // assemble string
   VG_(apply_ExeContext) (strstack_make, (void*)&cs, ep, ec);
   cs.str[VG_(strlen) (cs.str) - 1] = 0;  // remove trailing separator

   return cs.str;
}

#if 0
static const HChar* get_current_callstack(void) {
   Addr*ips = (Addr*) VG_(malloc) (sizeof(Addr)*clo_stackdepth);

   // After this call, the IPs we want are in ips[0]..ips[n_ips-1].
   Int n_ips = VG_(get_StackTrace)(VG_(get_running_tid)(),
                                   ips, clo_stackdepth,
                                   NULL /*array to dump SP values in*/,
                                   NULL /*array to dump FP values in*/,
                                   0 /*first_ip_delta*/);

   DiEpoch ep = VG_(current_DiEpoch)();

   ExeContext* ec = VG_(make_ExeContext_from_StackTrace)(ips, n_ips);
   const HChar *str = get_callstack(ips, ep, n_ips);
   //VG_(free) (ips); assuming ec now owns the ptr.
   return str;
}
#endif

static
void compute_ws(Time now_time)
{
   WorkingSet *ws = VG_(malloc) (sizeof(WorkingSet));
   if (!ws) {
      drop_samples++;
      return;
   }

   ws->t = now_time;
   ws->pages_insn = recently_used_pages (ht_insn, now_time);
   ws->pages_data = recently_used_pages (ht_data, now_time);
   num_samples++;

   if (clo_peakdetect) {
      #ifdef DEBUG
         ws->mAvg = pd_data.movingAvg;
         ws->mVar = pd_data.movingVar;
      #endif
      if (peak_detect(&pd_data, ws->pages_data) || peak_detect(&pd_insn, ws->pages_insn)) {
         WorkingSetPeak *wsp = VG_(malloc) (sizeof(*wsp));
         if (wsp) {
            wsp->t = now_time;
            if (!postmortem) {
               wsp->ec = VG_(record_ExeContext)(VG_(get_running_tid)(), 0);
            } else {
               wsp->ec = VG_(null_ExeContext)();
            }
            VG_(addToXA) (ws_peak_list, &wsp);
         }
         //VG_(umsg) ("WSS peak (insn=%lu, data=%lu) at: \n", ws->pages_insn, ws->pages_data);
      }
   }

   VG_(addToXA) (ws_at_time, &ws);
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
static Int
map_pageaddr_compare (const void *p1, const void *p2)
{
   const struct map_pageaddr * const *a1 = (const struct map_pageaddr * const *) p1;
   const struct map_pageaddr * const *a2 = (const struct map_pageaddr * const *) p2;

   if ((*a1)->count > (*a2)->count) return -1;
   if ((*a1)->count < (*a2)->count) return 1;
   return 0;
}

/**
 * @brief sort peakinfo by id
 */
static Int
map_peakinfo_compare (const void *p1, const void *p2)
{
   const struct map_peakinfo * const *a1 = (const struct map_peakinfo * const *) p1;
   const struct map_peakinfo * const *a2 = (const struct map_peakinfo * const *) p2;

   if ((*a1)->info.id > (*a2)->info.id) return 1;
   if ((*a1)->info.id < (*a2)->info.id) return -1;
   return 0;
}

static void print_pagestats(VgHashTable *ht, VgFile *fp)
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
         #ifdef WS_EPOCH
         const HChar *where = VG_(describe_IP) (res[i]->ep, res[i]->top.key, NULL);
         #else
         const HChar *where = VG_(describe_IP) (res[i]->top.key, NULL);
         #endif
         VG_(fprintf) (fp, " %s", where);
      }
   }
   VG_(fprintf) (fp, "\n");
   VG_(free) (res);
}

static void print_ws_peak_info(VgHashTable *ht, VgFile *fp) {

   int nres = 0;
   int nentry = VG_(HT_count_nodes) (ht);
   // sort
   struct map_peakinfo **res;
   res = VG_(malloc) (nentry * sizeof (*res));  // ptr to ptr
   VG_(HT_ResetIter)(ht);
   VgHashNode *nd;
   while ((nd = VG_(HT_Next)(ht)))
      res[nres++] = (struct map_peakinfo *) nd;
   VG_(ssort) (res, nres, sizeof (res[0]), map_peakinfo_compare);

   // print sorted
   for (int i = 0; i < nres; ++i) {
      struct map_peakinfo *pi = (struct map_peakinfo*) res[i];
      VG_(fprintf) (fp, "[%4d] refs=%u, loc=%s\n", pi->info.id, pi->info.cnt, pi->info.callstack);
   }
   VG_(free) (res);
}

static void print_ws_over_time(XArray *xa, VgHashTable *peakinfo, VgFile *fp)
{
   // header
   VG_(fprintf) (fp, "%12s %8s %8s", "t", "WSS_insn", "WSS_data");
   if (clo_peakdetect) {
      VG_(fprintf) (fp, " peak");
      #ifdef DEBUG
         VG_(fprintf) (fp, " %12s %12s", "mAvg", "mVar");
      #endif
   }

   VG_(fprintf) (fp, "\n");

   // peak info
   const int n_peaks = VG_(sizeXA)(ws_peak_list);
   int peak_id = 0;
   WorkingSetPeak **next_peak = (n_peaks > 0) ? VG_(indexXA)(ws_peak_list, peak_id++) : NULL;

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

      VG_(fprintf) (fp, "%12lu %8lu %8lu", t, pi, pd);
      if (clo_peakdetect) {
         char strpeak[5];
         if (next_peak && (*next_peak)->t == t) {
            // this sample was classified as peak; lookup peak info
            const UInt ecid = VG_(get_ECU_from_ExeContext)((*next_peak)->ec);
            struct map_peakinfo *pki = VG_(HT_lookup) (ht_ec2peakinfo, ecid);
            tl_assert(pki != NULL);
            VG_(snprintf) (strpeak, sizeof(strpeak), "%d", pki->info.id);
            next_peak = (peak_id < n_peaks) ? VG_(indexXA)(ws_peak_list, peak_id++) : NULL;
         } else {
            VG_(snprintf) (strpeak, sizeof(strpeak), "-");
         }
         VG_(fprintf) (fp, " %4s", strpeak);
         #ifdef DEBUG
            VG_(fprintf) (fp, " %10.1f %10.1f", (*ws)->mAvg, (*ws)->mVar);
         #endif
      }
      VG_(fprintf) (fp, "\n");
   }

   const long unsigned int total_i = (long unsigned int) VG_(HT_count_nodes) (ht_insn);
   const long unsigned int total_d = (long unsigned int) VG_(HT_count_nodes) (ht_data);
   const float avg_i = ((float)sum_i) / (num_t - 1);
   const float avg_d = ((float)sum_d) / (num_t - 1);
   VG_(fprintf) (fp, "\nInsn avg/peak/total:  %'.1f/%'lu/%'lu pages (%'u/%'u/%'u kB)",
                 avg_i, peak_i, total_i,
                 (unsigned int)((avg_i * clo_pagesize) / 1024.f),
                 (unsigned int)((peak_i * clo_pagesize) / 1024.f),
                 (unsigned int)((total_i * clo_pagesize) / 1024.f));
   VG_(fprintf) (fp, "\nData avg/peak/total:  %'.1f/%'lu/%'lu pages (%'u/%'u/%'u kB)",
                 avg_d, peak_d, total_d,
                 (unsigned int)((avg_d * clo_pagesize) / 1024.f),
                 (unsigned int)((peak_d * clo_pagesize) / 1024.f),
                 (unsigned int)((total_d * clo_pagesize) / 1024.f));
}

/**
 * @brief go over list of peaks and determine peak info (callstack)
 * @return number of unique peaks
 */
static unsigned long
compute_peakinfo(XArray *xa) {
   unsigned long num_unique = 0;

   const int num_t = VG_(sizeXA)(xa);
   for (int i = 0; i < num_t; i++) {
      WorkingSetPeak **wsp = VG_(indexXA)(xa, i);
      const UInt ecid = VG_(get_ECU_from_ExeContext)((*wsp)->ec);
      struct map_peakinfo *pi = VG_(HT_lookup) (ht_ec2peakinfo, ecid);
      if (pi == NULL) {
         num_unique++;
         HChar *strcs = get_callstack((*wsp)->ec);
         pi = VG_(malloc) (sizeof(*pi));
         pi->top.key = ecid;
         pi->info.id = VG_(HT_count_nodes)(ht_ec2peakinfo);
         pi->info.callstack = strcs;
         pi->info.cnt = 1;
         //VG_(umsg)("peak [%u] @%s\n", pi->info.id, pi->info.callstack);
         VG_(HT_add_node) (ht_ec2peakinfo, (VgHashNode *) pi);

      } else {
         pi->info.cnt++;
         //VG_(umsg)("peak [%d] visited %u times\n", pi->info.id, pi->info.cnt);
      }
   }
   return num_unique;
}

static void free_peakinfo(void *arg) {
   struct map_peakinfo *pi = (struct map_peakinfo*) arg;
   VG_(free) (pi->info.callstack);
   VG_(free) (arg);
}

static void ws_fini(Int exitcode)
{
   // force one last data point
   postmortem = True;
   compute_ws(get_time());

   tl_assert(clo_fnname);
   tl_assert(clo_fnname[0]);

   VG_(umsg)("Number of instructions: %'lu\n", (unsigned long) guest_instrs_executed);
   VG_(umsg)("Number of WS samples:   %'lu\n", num_samples);
   VG_(umsg)("Dropped WS samples:     %'lu\n", drop_samples);

   HChar* outfile = VG_(expand_file_name)("--ws-file", int_filename);
   VG_(umsg)("Writing results to file '%s'\n", outfile);
   VgFile *fp = VG_(fopen)(outfile, VKI_O_CREAT|VKI_O_TRUNC|VKI_O_WRONLY,
                                    VKI_S_IRUSR|VKI_S_IWUSR);
   if (fp == NULL) {
      // If the file can't be opened for whatever reason (conflict
      // between multiple cachegrinded processes?), give up now.
      VG_(umsg)("error: can't open simulation output file '%s'\n",
                outfile );
      VG_(umsg)("       ... so simulation results will be missing.\n");
      VG_(free)(outfile);
      return;
   } else {
      VG_(free)(outfile);
   }

   if (fp != NULL) {
       // preamble
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
         VG_(fprintf) (fp, "Peak influence: %.1f\n", clo_peakinfl);
      }
      VG_(fprintf) (fp, "--\n\n");

      // page listing
      if (clo_listpages) {
         VG_(fprintf) (fp, "Code pages, ");
         print_pagestats (ht_insn, fp);
         VG_(fprintf) (fp, "\nData pages, ");
         print_pagestats (ht_data, fp);
         VG_(fprintf) (fp, "\n--\n\n");
      }

      unsigned long upk = 0;
      if (clo_peakdetect) {
         upk = compute_peakinfo(ws_peak_list);
         VG_(umsg)("Number of peaks/unique: %lu/%lu\n", VG_(sizeXA)(ws_peak_list), upk);
      }

      // working set data
      VG_(fprintf) (fp, "Working sets:\n");
      print_ws_over_time (ws_at_time, ht_ec2peakinfo, fp);
      VG_(fprintf) (fp, "\n--\n\n");

      if (clo_peakdetect) {
         VG_(fprintf) (fp, "Peak info:\n");
         print_ws_peak_info (ht_ec2peakinfo, fp);
         VG_(fprintf) (fp, "\n");
         VG_(fprintf) (fp, "Number of peaks/unique: %lu/%lu", VG_(sizeXA)(ws_peak_list), upk);
         VG_(fprintf) (fp, "\n--\n\n");
      }
   }

   // cleanup
   VG_(fclose)(fp);
   VG_(HT_destruct) (ht_data, VG_(free));
   VG_(HT_destruct) (ht_insn, VG_(free));
   VG_(HT_destruct) (ht_ec2peakinfo, free_peakinfo);
   VG_(deleteXA) (ws_at_time);
   VG_(deleteXA) (ws_peak_list);
   if (int_filename != clo_filename) VG_(free) ((void*)int_filename);
   VG_(umsg)("ws finished\n");
}

// DONE
static void ws_pre_clo_init(void)
{
   VG_(details_name)            (WS_NAME);
   VG_(details_version)         (WS_VERSION);
   VG_(details_description)     (WS_DESC);
   VG_(details_copyright_author)("Copyright (C) 2018, and GNU GPL'd, by Martin Becker.");
   VG_(details_bug_reports_to)  (VG_BUGS_TO);
   VG_(details_avg_translation_sizeB) ( 200 );

   VG_(basic_tool_funcs)          (ws_post_clo_init,
                                   ws_instrument,
                                   ws_fini);
   VG_(needs_command_line_options)(ws_process_cmd_line_option,
                                   ws_print_usage,
                                   ws_print_debug_usage);

   ht_data        = VG_(HT_construct) ("ht_data");
   ht_insn        = VG_(HT_construct) ("ht_insn");
   ht_ec2peakinfo = VG_(HT_construct) ("ht_ec2peakinfo");
   ws_at_time     = VG_(newXA) (VG_(malloc), "arr_ws",   VG_(free), sizeof(WorkingSet*));
   ws_peak_list   = VG_(newXA) (VG_(malloc), "arr_peak", VG_(free), sizeof(WorkingSetPeak*));
}

VG_DETERMINE_INTERFACE_VERSION(ws_pre_clo_init)

/*--------------------------------------------------------------------*/
/*--- end                                                ws_main.c ---*/
/*--------------------------------------------------------------------*/
