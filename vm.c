#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define TLB_SIZE 16
#define PAGE_TABLE_SIZE 256
#define FRAME_SIZE 256
#define NUM_FRAMES 128
#define LOGICAL_MAX 65536

#define ALGO_NONE -1
#define ALGO_FIFO 0
#define ALGO_LRU 1

typedef struct {
    int frame;
    bool valid;
    unsigned long last_use;
} PageTableEntry;

typedef struct {
    int page;
    int frame;
    bool valid;
    unsigned long last_use;
} TLBEntry;

typedef struct {
    TLBEntry entries[TLB_SIZE];
    int fifo_order[TLB_SIZE];
    int fifo_head;
    int fifo_count;
} TLB;

static PageTableEntry page_table[PAGE_TABLE_SIZE];
static TLB tlb;
static signed char physical_memory[NUM_FRAMES][FRAME_SIZE];
static bool frame_used[NUM_FRAMES];
static int next_free_frame = 0;
static int page_fifo[NUM_FRAMES];
static int page_fifo_head = 0;
static int page_fifo_count = 0;
static unsigned long clock_counter = 0;
static unsigned long page_faults = 0;
static unsigned long tlb_hits = 0;
static unsigned long tlb_misses = 0;
static int page_algo = ALGO_NONE;
static int tlb_algo = ALGO_NONE;
static FILE *out = NULL;