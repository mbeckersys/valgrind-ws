#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>

#ifdef DEBUG
  #define DPRINT(fmt, ...) do { if (DEBUG) fprintf(stderr, fmt, __VA_ARGS__); } while (0)
#else
  #define DPRINT(fmt, ...) /* nix */
#endif

static long PS = 4096;

typedef struct pagetab_s {
    int    num;
    void **tab;
} pagetab_t;

static inline void* _alloc_page(void) {
    return mmap(NULL, PS, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
}

static inline void _free_page(void*pg) {
    munmap(pg, PS);
}

static void claim_page(pagetab_t *pt) {
    pt->tab[pt->num++] = _alloc_page();
}

static void release_page(pagetab_t *pt) {
    _free_page(pt->tab[pt->num]);
    pt->tab[pt->num--] = NULL;
}

static int touch_pages(pagetab_t *pt) {
    for (int p = 0; p < pt->num; ++p) {
        char*first = (char*)pt->tab[p];
        *first = p;
    }
}

int main(int argc, char**argv) {
    // arguments
    int maxp = 1024, cycles = 10;
    if (argc > 1) {
        maxp = atoi(argv[1]);
    }
    if (argc > 2) {
        cycles = atoi(argv[2]);
    }
    if (maxp < 0) maxp = -maxp;
    if (cycles < 0) cycles=-cycles;

    // set up page table
    PS = sysconf(_SC_PAGESIZE);
    pagetab_t pt;
    pt.num = 0;
    pt.tab = (void*) malloc(sizeof(void*) * maxp);
    DPRINT("Ramping up to %d pages, %d times\n", maxp, cycles);

    for (int c = 0; c < cycles; ++c) {
        // ramp up
        DPRINT("Cycle %d up...\n", c);
        for (int p = 0; p < maxp; ++p) {
            claim_page(&pt);
            touch_pages(&pt);
        }
        // ramp down
        DPRINT("Cycle %d down...\n", c);
        for (int p = 0; p < maxp; ++p) {
            release_page(&pt);
            touch_pages(&pt);
        }
    }

    // cleanup
    free(pt.tab);
    DPRINT("DONE\n");
}
