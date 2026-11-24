#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>

#define TLB_SIZE 16
#define PAGE_TABLE_SIZE 256
#define FRAME_SIZE 256
#define MAX_FRAMES 256
#define DEFAULT_FRAMES 128
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
    int fifo_head;
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
static signed char physical_memory[MAX_FRAMES][FRAME_SIZE];
static signed char backing_store[PAGE_TABLE_SIZE][FRAME_SIZE];
static bool frame_used[MAX_FRAMES];
static int next_free_frame = 0;
static int page_fifo[MAX_FRAMES];
static int page_fifo_head = 0;
static int page_fifo_count = 0;
static unsigned long clock_counter = 0;
static unsigned long tlb_clock = 0;
static unsigned long page_faults = 0;
static unsigned long tlb_hits = 0;
static unsigned long tlb_misses = 0;
static int page_algo = ALGO_NONE;
static int tlb_algo = ALGO_NONE;
static FILE *out = NULL;

static int num_frames = DEFAULT_FRAMES;
static int use_tlb = 1;
static int use_threads = 1;

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

static void load_backing_store(void) {
    FILE *f = fopen("BACKING_STORE.bin", "rb");
    if (!f) {
        fprintf(stderr, "Erro: BACKING_STORE.bin nao encontrado.\n");
        exit(1);
    }
    if (fread(backing_store, sizeof(signed char), sizeof(backing_store), f)
            != sizeof(backing_store)) {
        fprintf(stderr, "Erro: BACKING_STORE.bin incompleto.\n");
        fclose(f);
        exit(1);
    }
    fclose(f);
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
    }
    tlb.fifo_head = 0;
    for (int i = 0; i < MAX_FRAMES; i++) {
        frame_used[i] = false;
        for (int j = 0; j < FRAME_SIZE; j++) physical_memory[i][j] = 0;
        page_fifo[i] = -1;
    }
    next_free_frame = 0;
    page_fifo_head = 0;
    page_fifo_count = 0;
    clock_counter = 1;
    tlb_clock = 1;
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

static int tlb_lookup_threaded(int page, int *hit_index) {
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

static int tlb_lookup_seq(int page, int *hit_index) {
    for (int i = 0; i < TLB_SIZE; i++) {
        if (tlb.entries[i].valid && tlb.entries[i].page == page) {
            *hit_index = i;
            return tlb.entries[i].frame;
        }
    }
    *hit_index = -1;
    return -1;
}

static int tlb_lookup(int page, int *hit_index) {
    if (!use_tlb) {
        *hit_index = -1;
        return -1;
    }
    return use_threads ? tlb_lookup_threaded(page, hit_index)
                       : tlb_lookup_seq(page, hit_index);
}

static void tlb_insert(int page, int frame) {
    if (!use_tlb) return;
    int victim = -1;
    for (int i = 0; i < TLB_SIZE; i++) {
        if (!tlb.entries[i].valid) { victim = i; break; }
    }
    if (victim == -1) {
        if (tlb_algo == ALGO_FIFO) {
            victim = tlb.fifo_head;
            tlb.fifo_head = (tlb.fifo_head + 1) % TLB_SIZE;
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
    tlb_clock++;
    tlb.entries[victim].last_use = tlb_clock;
}

static void tlb_touch(int index) {
    if (!use_tlb) return;
    tlb_clock++;
    tlb.entries[index].last_use = tlb_clock;
}

static int pick_page_victim(void) {
    if (page_algo == ALGO_FIFO) {
        int f = page_fifo[page_fifo_head];
        page_fifo_head = (page_fifo_head + 1) % num_frames;
        page_fifo_count--;
        return f;
    }

    int victim_frame = -1;
    unsigned long oldest = (unsigned long)-1;

    for (int f = 0; f < num_frames; f++) {
        if (!frame_used[f]) continue;

        int page = -1;
        for (int p = 0; p < PAGE_TABLE_SIZE; p++) {
            if (page_table[p].valid && page_table[p].frame == f) {
                page = p;
                break;
            }
        }
        if (page == -1) continue;

        if (page_table[page].last_use < oldest) {
            oldest = page_table[page].last_use;
            victim_frame = f;
        }
    }
    return victim_frame;
}

static void page_fifo_push(int frame) {
    int slot = (page_fifo_head + page_fifo_count) % num_frames;
    page_fifo[slot] = frame;
    page_fifo_count++;
}

static int handle_page_fault(int page) {
    int frame = -1;
    if (next_free_frame < num_frames) {
        frame = next_free_frame++;
        frame_used[frame] = true;
    } else {
        frame = pick_page_victim();
        for (int p = 0; p < PAGE_TABLE_SIZE; p++) {
            if (page_table[p].valid && page_table[p].frame == frame) {
                page_table[p].valid = false;
                page_table[p].frame = -1;
                for (int t = 0; t < TLB_SIZE; t++) {
                    if (tlb.entries[t].valid && tlb.entries[t].page == p)
                        tlb.entries[t].valid = false;
                }
                break;
            }
        }
    }
    page_table[page].frame = frame;
    page_table[page].valid = true;
    clock_counter++;
    page_table[page].last_use = clock_counter;
    if (page_algo == ALGO_FIFO) {
        page_fifo_push(frame);
    }
    for (int i = 0; i < FRAME_SIZE; i++) {
        physical_memory[frame][i] = backing_store[page][i];
    }
    page_faults++;
    return frame;
}

static void page_touch(int page) {
    clock_counter++;
    page_table[page].last_use = clock_counter;
}

static void process_address(unsigned int logical) {
    int page = (int)(logical >> 8);
    int offset = (int)(logical & 0xFF);
    int tlb_index = -1;
    int frame = tlb_lookup(page, &tlb_index);
    signed char value;

    if (frame != -1) {
        tlb_hits++;
        value = physical_memory[frame][offset];
        tlb_touch(tlb_index);
        page_touch(page);
    } else {
        tlb_misses++;
        if (!page_table[page].valid) {
            frame = handle_page_fault(page);
        } else {
            frame = page_table[page].frame;
            page_touch(page);
        }
        tlb_insert(page, frame);
        value = physical_memory[frame][offset];
    }

    int physical = frame * FRAME_SIZE + offset;
    int slot_in_tlb = -1;
    if (use_tlb) {
        for (int i = 0; i < TLB_SIZE; i++) {
            if (tlb.entries[i].valid && tlb.entries[i].page == page) {
                slot_in_tlb = i;
                break;
            }
        }
    }
    fprintf(out, "Virtual address: %u TLB: %d Physical address: %d Value: %d\n",
            logical, slot_in_tlb, physical, (int)value);
}

int main(int argc, char *argv[]) {
    if (getenv("VM_NOTLB") != NULL)    use_tlb = 0;
    if (getenv("VM_NOTHREAD") != NULL) use_threads = 0;
    if (getenv("VM_256") != NULL)      num_frames = 256;

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

    out = fopen("correct.txt", "w");
    if (!out) {
        free(addresses);
        fprintf(stderr, "Erro: nao foi possivel criar correct.txt\n");
        return 1;
    }
    load_backing_store();
    init_all();

    for (size_t i = 0; i < count; i++) {
        process_address(addresses[i]);
    }

    int total = (int)count;
    fprintf(out, "Number of Translated Addresses = %d\n", total);
    fprintf(out, "Page Faults = %lu\n", page_faults);
    fprintf(out, "Page Fault Rate = %.3f\n", (double)page_faults / total);
    fprintf(out, "TLB Hits = %lu\n", tlb_hits);
    fprintf(out, "TLB Hit Rate = %.3f\n", (double)tlb_hits / total);

    fclose(out);
    free(addresses);

    printf("Execucao concluida. Saida em correct.txt\n");
    printf("Page faults: %lu | TLB hits: %lu | TLB misses: %lu\n",
           page_faults, tlb_hits, tlb_misses);
    return 0;
}