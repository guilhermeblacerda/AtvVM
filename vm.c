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

#include <string.h>
#include <ctype.h>

static int parse_algo(const char *s) {
    char buf[16];
    size_t n = strlen(s);
    if (n >= sizeof(buf)) return ALGO_NONE;
    for (size_t i = 0; i <= n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    if (strcmp(buf, "fifo") == 0) return ALGO_FIFO;
    if (strcmp(buf, "lru") == 0) return ALGO_LRU;
    return ALGO_NONE;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <arquivo_enderecos> <fifo|lru> <fifo|lru>\n", argv[0]);
        return 1;
    }
    page_algo = parse_algo(argv[2]);
    tlb_algo = parse_algo(argv[3]);
    if (page_algo == ALGO_NONE) {
        fprintf(stderr, "Erro: algoritmo de pagina invalido (%s). Use fifo ou lru.\n", argv[2]);
        return 1;
    }
    if (tlb_algo == ALGO_NONE) {
        fprintf(stderr, "Erro: algoritmo de TLB invalido (%s). Use fifo ou lru.\n", argv[3]);
        return 1;
    }
    printf("args ok: page=%s tlb=%s\n", argv[2], argv[3]);
    return 0;
}