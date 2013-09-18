#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <ctype.h>
#include <fcntl.h>
#include "stats.h"
#include "trace.h"
#include "pscanf.h"
#include "cpu_is_snb.h"

// Sandy Bridge microarchitectures have signatures 06_2a and 06_2d with non-architectural events
// listed in Table 19-7, 19-8, and 19-9.  19-8 is 06_2a specific, 19-9 is 06_2d specific.  Stampede
// is 06_2d but no 06_2d specific events are used here.

// $ ls -l /dev/cpu/0
// total 0
// crw-------  1 root root 203, 0 Oct 28 18:47 cpuid
// crw-------  1 root root 202, 0 Oct 28 18:47 msr

// Uncore events are in this file.  Uncore events are found in the MSR and PCI config space.
// C-Box, PCU, and U-box counters are all in the MSR file
// This stuff is all in: 
//Intel Xeon Processor E5-2600 Product Family Uncore Performance Monitoring Guide

// Uncore MSR addresses
// C-Box control and counter registers 
// 8 C-Boxes, 4 counters each.  Each counter has a different restriction on what it counts. Addresses in Table 2-8.
/* Box Control - Cn_MSR_PMON_BOX_CTL - Defs in Table 2-9 */
#define CBOX_CTL0 0xD04
#define CBOX_CTL1 0xD24
#define CBOX_CTL2 0xD44
#define CBOX_CTL3 0xD64
#define CBOX_CTL4 0xD84
#define CBOX_CTL5 0xDA4
#define CBOX_CTL6 0xDC4
#define CBOX_CTL7 0xDE4
/* Counter Filters - Cn_MSR_PMON_BOX_FILTER - Defs in 2-12, 2-13 */
#define CBOX_FILTER0 0xD14
#define CBOX_FILTER1 0xD34
#define CBOX_FILTER2 0xD54
#define CBOX_FILTER3 0xD74
#define CBOX_FILTER4 0xD94
#define CBOX_FILTER5 0xDB4
#define CBOX_FILTER6 0xDD4
#define CBOX_FILTER7 0xDF4
/* Counter Control - Cn_MSR_PMON_CTL{0-3} - Defs in 2-10 */
/* Box CTL and CTR increments by 1  intra box */ 
/* Box CTL and CTR increments by 32 inter box */ 
#define CTL0 0xD10 /* Base to start from */
#define CTL1 0xD11 /* Base to start from */
#define CTL2 0xD12 /* Base to start from */
#define CTL3 0xD13 /* Base to start from */

/* Counter Registers 64 bit but only 44 bit for counting */
#define CTR0 0xD16 /* Base to count from */
#define CTR1 0xD17 /* Base to count from */
#define CTR2 0xD18 /* Base to count from */
#define CTR3 0xD19 /* Base to count from */

// Width of 44 for C-Boxes
#define KEYS \
    X(CTL0, "C", ""), \
    X(CTL1, "C", ""), \
    X(CTL2, "C", ""), \
    X(CTL3, "C", ""),  \
    X(CTR0, "E,W=44", ""), \
    X(CTR1, "E,W=44", ""), \
    X(CTR2, "E,W=44", ""), \
    X(CTR3, "E,W=44", "")
  
/* Defs in Table 2-12 */
/* Event filters in C-Box
opcode             [31:23]
state filter       [22:18]
node filter        [17:10]
tid: core filter   [3:1] 
     thread filter [0]
*/
#define CBOX_FILTER(...)			\
  ( (0x0ULL << 0)   /* Count all events */	\
  | (0x00ULL << 10) /* Don't filter on node */  \
  | (1ULL << 18)  /* Track I state. */ \
  | (1ULL << 19)  /* Track S state. */ \
  | (1ULL << 20)  /* Track E state. */ \
  | (1ULL << 21)  /* Track M state. */ \
  | (1ULL << 22)  /* Track F state. */ \
  | (0x000ULL << 23) /* Opcode */ \
  )

/* Events in C-Box
threshhold        [31:24]
invert threshold  [23]
enable            [22]
tid filter enable [19]
edge detect       [18]
clear counter     [17]
umask             [15:8]
event select      [7:0]
*/

/* Defs in Table 2-10 */
#define CBOX_PERF_EVENT(event, umask) \
  ( (event) \
  | (umask << 8) \
  | (0ULL << 17) /* Reset Counters. */ \
  | (0ULL << 18) /* Edge Detection. */ \
  | (0ULL << 19) /* TID Filter. */ \
  | (1ULL << 22) /* Enable. */ \
  | (0ULL << 23) /* Invert */ \
  | (0x01ULL << 24) /* Threshold */ \
  )

/* Definitions in Table 2-14 */
#define CLOCK_TICKS         CBOX_PERF_EVENT(0x00, 0x00) /* Ctrs 0-3 */
#define RxR_OCCUPANCY       CBOX_PERF_EVENT(0x11, 0x01)  /* Ctrs 0   */
#define COUNTER0_OCCUPANCY  CBOX_PERF_EVENT(0x1F, 0x00) /* Ctrs 1-3 */
#define LLC_LOOKUP          CBOX_PERF_EVENT(0x34, 0x03) /* Ctrs 0-1 */

static int intel_snb_cbo_begin_box(char *cpu, int box, uint64_t *events, size_t nr_events)
{
  int rc = -1;
  char msr_path[80];
  int msr_fd = -1;
  uint64_t ctl;
  //uint64_t filter;
  int offset = box*32;

  snprintf(msr_path, sizeof(msr_path), "/dev/cpu/%s/msr", cpu);
  msr_fd = open(msr_path, O_RDWR);
  if (msr_fd < 0) {
    ERROR("cannot open `%s': %m\n", msr_path);
    goto out;
  }

  ctl = 0x10100ULL; // enable freeze (bit 16), freeze (bit 8)
  /* C-box ctrl registers are 32-bits apart */
  if (pwrite(msr_fd, &ctl, sizeof(ctl), CBOX_CTL0 + offset) < 0) {
    ERROR("cannot enable freeze of C-box counter: %m\n");
    goto out;
  }
  
  /* Ignore C-Box filter for now */
  /* The filters are part of event selection */
  /*
  filter = CBOX_FILTER();
  if (pwrite(msr_fd, &filter, sizeof(filter), CBOX_FILTER0 + offset) < 0) {
    ERROR("cannot modify C-box filters: %m\n");
    goto out;
  }
  */

  /* Select Events for this C-Box */
  int i;
  for (i = 0; i < nr_events; i++) {
    TRACE("MSR %08X, event %016llX\n", CTL0 + offset + i, (unsigned long long) events[i]);
    if (pwrite(msr_fd, &events[i], sizeof(events[i]), CTL0 + offset + i) < 0) { 
      ERROR("cannot write event %016llX to MSR %08X through `%s': %m\n", 
            (unsigned long long) events[i],
            (unsigned) CTL0 + offset + i,
            msr_path);
      goto out;
    }
  }

  ctl |= 1ULL << 1; // reset counter
  /* C-box ctrl registers are 32-bits apart */
  if (pwrite(msr_fd, &ctl, sizeof(ctl), CBOX_CTL0 + offset) < 0) {
    ERROR("cannot reset C-box counter: %m\n");
    goto out;
  }
  
  /* Unfreeze C-box counter (64-bit) */
  ctl = 0x10000ULL; // unfreeze counter
  if (pwrite(msr_fd, &ctl, sizeof(ctl), CBOX_CTL0 + offset) < 0) {
    ERROR("cannot unfreeze C-box counters: %m\n");
    goto out;
  }

  rc = 0;

 out:
  if (msr_fd >= 0)
    close(msr_fd);

  return rc;
}

static int intel_snb_cbo_begin(struct stats_type *type)
{
  int nr = 0;

  uint64_t cbo_events[8][4] = {
    { RxR_OCCUPANCY, LLC_LOOKUP, COUNTER0_OCCUPANCY, CLOCK_TICKS, },
    { RxR_OCCUPANCY, LLC_LOOKUP, COUNTER0_OCCUPANCY, CLOCK_TICKS, },
    { RxR_OCCUPANCY, LLC_LOOKUP, COUNTER0_OCCUPANCY, CLOCK_TICKS, },
    { RxR_OCCUPANCY, LLC_LOOKUP, COUNTER0_OCCUPANCY, CLOCK_TICKS, },
    { RxR_OCCUPANCY, LLC_LOOKUP, COUNTER0_OCCUPANCY, CLOCK_TICKS, },
    { RxR_OCCUPANCY, LLC_LOOKUP, COUNTER0_OCCUPANCY, CLOCK_TICKS, },
    { RxR_OCCUPANCY, LLC_LOOKUP, COUNTER0_OCCUPANCY, CLOCK_TICKS, },
    { RxR_OCCUPANCY, LLC_LOOKUP, COUNTER0_OCCUPANCY, CLOCK_TICKS, },
  };

  int i;
  for (i = 0; i < nr_cpus; i++) {
    char cpu[80];
    char core_id_path[80];
    int core_id = -1;
    int box;
    /* Only program uncore counters on core 0 of a socket. */

    snprintf(core_id_path, sizeof(core_id_path), "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
    if (pscanf(core_id_path, "%d", &core_id) != 1) {
      ERROR("cannot read core id file `%s': %m\n", core_id_path); /* errno */
      continue;
    }

    if (core_id != 0)
      continue;

    snprintf(cpu, sizeof(cpu), "%d", i);
    
    if (cpu_is_sandybridge(cpu))      
      {
	for (box = 0; box < 8; box++)
	  if (intel_snb_cbo_begin_box(cpu, box, cbo_events[box], 4) == 0)
	    nr++; /* HARD */
      }
  }

  return nr > 0 ? 0 : -1;
}

static void intel_snb_cbo_collect_box(struct stats_type *type, char *cpu, char* cpu_box, int box)
{
  struct stats *stats = NULL;
  char msr_path[80];
  int msr_fd = -1;
  int offset;
  offset = 32*box;

  stats = get_current_stats(type, cpu_box);
  if (stats == NULL)
    goto out;

  TRACE("cpu %s\n", cpu);
  TRACE("socket/box %s\n", cpu_box);

  snprintf(msr_path, sizeof(msr_path), "/dev/cpu/%s/msr", cpu);
  msr_fd = open(msr_path, O_RDONLY);
  if (msr_fd < 0) {
    ERROR("cannot open `%s': %m\n", msr_path);
    goto out;
  }

#define X(k,r...) \
  ({ \
    uint64_t val = 0; \
    if (pread(msr_fd, &val, sizeof(val), k + offset) < 0) \
      ERROR("cannot read `%s' (%08X) through `%s': %m\n", #k, k + offset, msr_path); \
    else \
      stats_set(stats, #k, val); \
  })
  KEYS;
#undef X

 out:
  if (msr_fd >= 0)
    close(msr_fd);
}

static void intel_snb_cbo_collect(struct stats_type *type)
{

  int i;
  for (i = 0; i < nr_cpus; i++) {
    char cpu[80];
    char core_id_path[80];

    char socket[80];
    char socket_id_path[80];

    char cpu_box[80];

    int socket_id = -1;
    int core_id = -1;
    int box;

    /* Only collect uncore counters on core 0 of a socket. */
    snprintf(core_id_path, sizeof(core_id_path), 
	     "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
    if (pscanf(core_id_path, "%d", &core_id) != 1) {
      ERROR("cannot read core id file `%s': %m\n", core_id_path);
      continue;
    }
    if (core_id != 0)
      continue;

    /* Get socket number. */
    snprintf(socket_id_path, sizeof(socket_id_path), 
	     "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
    if (pscanf(socket_id_path, "%d", &socket_id) != 1) {
      ERROR("cannot read socket id file `%s': %m\n", socket_id_path);
      continue;
    }

    snprintf(cpu, sizeof(cpu), "%d", i);
    snprintf(socket, sizeof(socket), "%d", socket_id);

    if (cpu_is_sandybridge(cpu))
      {
	for (box = 0; box < 8; box++)
	  {
	    snprintf(cpu_box, sizeof(cpu_box), "%d/%d", socket_id, box);
	    intel_snb_cbo_collect_box(type, cpu, cpu_box, box);
	  }
      }
  }
}

struct stats_type intel_snb_cbo_stats_type = {
  .st_name = "intel_snb_cbo",
  .st_begin = &intel_snb_cbo_begin,
  .st_collect = &intel_snb_cbo_collect,
#define X SCHEMA_DEF
  .st_schema_def = JOIN(KEYS),
#undef X
};