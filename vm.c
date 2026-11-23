#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>

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

typedef struct {
    TLB *tlb;
    int page;
    int index;
    int found;
    int frame;
} TlbSearchArg;

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

static bool is_valid_number_line(const char *line) {
    const char *p = line;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p == '\0') return false;
    if (*p == '-') p++;
    if (!isdigit((unsigned char)*p)) return false;
    long v = strtol(line, NULL, 10);
    if (v < 0 || v >= LOGICAL_MAX) return false;
    return true;
}

static void init_all(void) {
    for (int i = 0; i < PAGE_TABLE_SIZE; i++) {
        page_table[i].frame = -1;
        page_table[i].valid = false;
        page_table[i].last_use = 0;
    }
    for (int i = 0; i < TLB_SIZE; i++) {
        tlb.entries[i].page = -1;
        tlb.entries[i].frame = -1;
        tlb.entries[i].valid = false;
        tlb.entries[i].last_use = 0;
        tlb.fifo_order[i] = -1;
    }
    tlb.fifo_head = 0;
    tlb.fifo_count = 0;
    for (int i = 0; i < NUM_FRAMES; i++) {
        frame_used[i] = false;
        for (int j = 0; j < FRAME_SIZE; j++) physical_memory[i][j] = 0;
        page_fifo[i] = -1;
    }
    next_free_frame = 0;
    page_fifo_head = 0;
    page_fifo_count = 0;
    clock_counter = 0;
    page_faults = 0;
    tlb_hits = 0;
    tlb_misses = 0;
}

static void *tlb_search_worker(void *p) {
    TlbSearchArg *a = (TlbSearchArg *)p;
    TLBEntry *e = &a->tlb->entries[a->index];
    if (e->valid && e->page == a->page) {
        a->found = 1;
        a->frame = e->frame;
    }
    return NULL;
}

static int tlb_lookup(int page, int *hit_index) {
    pthread_t threads[TLB_SIZE];
    TlbSearchArg args[TLB_SIZE];
    for (int i = 0; i < TLB_SIZE; i++) {
        args[i].tlb = &tlb;
        args[i].page = page;
        args[i].index = i;
        args[i].found = 0;
        args[i].frame = -1;
        pthread_create(&threads[i], NULL, tlb_search_worker, &args[i]);
    }
    int frame = -1;
    *hit_index = -1;
    for (int i = 0; i < TLB_SIZE; i++) {
        pthread_join(threads[i], NULL);
        if (args[i].found && frame == -1) {
            frame = args[i].frame;
            *hit_index = i;
        }
    }
    return frame;
}

static void tlb_fifo_push(int index) {
    int slot = (tlb.fifo_head + tlb.fifo_count) % TLB_SIZE;
    tlb.fifo_order[slot] = index;
    if (tlb.fifo_count < TLB_SIZE) tlb.fifo_count++;
    else tlb.fifo_head = (tlb.fifo_head + 1) % TLB_SIZE;
}

static void tlb_fifo_remove_index(int index) {
    int new_order[TLB_SIZE];
    int n = 0;
    for (int i = 0; i < tlb.fifo_count; i++) {
        int idx = tlb.fifo_order[(tlb.fifo_head + i) % TLB_SIZE];
        if (idx != index) new_order[n++] = idx;
    }
    tlb.fifo_head = 0;
    tlb.fifo_count = n;
    for (int i = 0; i < n; i++) tlb.fifo_order[i] = new_order[i];
}

static void tlb_insert(int page, int frame) {
    int victim = -1;
    for (int i = 0; i < TLB_SIZE; i++) {
        if (!tlb.entries[i].valid) { victim = i; break; }
    }
    if (victim == -1) {
        if (tlb_algo == ALGO_FIFO) {
            victim = tlb.fifo_order[tlb.fifo_head];
            tlb_fifo_remove_index(victim);
        } else {
            unsigned long oldest = (unsigned long)-1;
            victim = 0;
            for (int i = 0; i < TLB_SIZE; i++) {
                if (tlb.entries[i].last_use < oldest) {
                    oldest = tlb.entries[i].last_use;
                    victim = i;
                }
            }
        }
    }
    tlb.entries[victim].page = page;
    tlb.entries[victim].frame = frame;
    tlb.entries[victim].valid = true;
    clock_counter++;
    tlb.entries[victim].last_use = clock_counter;
    if (tlb_algo == ALGO_FIFO) tlb_fifo_push(victim);
}

static void tlb_touch(int index) {
    clock_counter++;
    tlb.entries[index].last_use = clock_counter;
    if (tlb_algo == ALGO_FIFO) {
        tlb_fifo_remove_index(index);
        tlb_fifo_push(index);
    }
}

static int pick_page_victim(void) {
    if (page_algo == ALGO_FIFO) {
        int f = page_fifo[page_fifo_head];
        page_fifo_head = (page_fifo_head + 1) % NUM_FRAMES;
        page_fifo_count--;
        return f;
    }
    int victim_frame = -1;
    unsigned long oldest = (unsigned long)-1;
    for (int p = 0; p < PAGE_TABLE_SIZE; p++) {
        if (!page_table[p].valid) continue;
        if (page_table[p].last_use < oldest) {
            oldest = page_table[p].last_use;
            victim_frame = page_table[p].frame;
        }
    }
    return victim_frame;
}

static void page_fifo_push(int frame) {
    int slot = (page_fifo_head + page_fifo_count) % NUM_FRAMES;
    page_fifo[slot] = frame;
    page_fifo_count++;
}

static void page_fifo_remove_frame(int frame) {
    int tmp[NUM_FRAMES];
    int n = 0;
    for (int i = 0; i < page_fifo_count; i++) {
        int f = page_fifo[(page_fifo_head + i) % NUM_FRAMES];
        if (f != frame) tmp[n++] = f;
    }
    page_fifo_head = 0;
    page_fifo_count = n;
    for (int i = 0; i < n; i++) page_fifo[i] = tmp[i];
}

static int handle_page_fault(int page) {
    int frame = -1;
    if (next_free_frame < NUM_FRAMES) {
        frame = next_free_frame++;
        frame_used[frame] = true;
    } else {
        frame = pick_page_victim();
        for (int p = 0; p < PAGE_TABLE_SIZE; p++) {
            if (page_table[p].valid && page_table[p].frame == frame) {
                page_table[p].valid = false;
                page_table[p].frame = -1;
                break;
            }
        }
    }
    page_table[page].frame = frame;
    page_table[page].valid = true;
    clock_counter++;
    page_table[page].last_use = clock_counter;
    if (page_algo == ALGO_FIFO) {
        if (next_free_frame <= NUM_FRAMES && page_fifo_count < NUM_FRAMES) {
            page_fifo_push(frame);
        } else {
            page_fifo_remove_frame(frame);
            page_fifo_push(frame);
        }
    }
    for (int i = 0; i < FRAME_SIZE; i++) physical_memory[frame][i] = 0;
    page_faults++;
    return frame;
}

static void page_touch(int page) {
    clock_counter++;
    page_table[page].last_use = clock_counter;
    if (page_algo == ALGO_FIFO) {
        int f = page_table[page].frame;
        page_fifo_remove_frame(f);
        page_fifo_push(f);
    }
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
    
        FILE *in = fopen(argv[1], "r");
    if (!in) {
        fprintf(stderr, "Erro: nao foi possivel abrir o arquivo '%s'.\n", argv[1]);
        return 1;
    }

    unsigned int *addresses = NULL;
    size_t count = 0, capacity = 0;
    char line[256];
    while (fgets(line, sizeof(line), in)) {
        if (!is_valid_number_line(line)) {
            fprintf(stderr, "Erro: linha invalida no arquivo de enderecos: '%s'\n", line);
            free(addresses);
            fclose(in);
            return 1;
        }
        long v = strtol(line, NULL, 10);
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 64;
            unsigned int *tmp = realloc(addresses, capacity * sizeof(unsigned int));
            if (!tmp) { free(addresses); fclose(in); fprintf(stderr, "Erro: realloc.\n"); return 1; }
            addresses = tmp;
        }
        addresses[count++] = (unsigned int)v;
    }
    fclose(in);

    if (count == 0) {
        fprintf(stderr, "Erro: arquivo de enderecos vazio.\n");
        free(addresses);
        return 1;
    }

    init_all();

    printf("lidos %zu enderecos\n", count);
    free(addresses);
    return 0;

    return 0;
}