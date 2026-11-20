#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SIZE 256
#define NUM_PAGES 256
#define MAX_FRAMES 256
#define DEFAULT_FRAMES 128
#define TLB_SIZE 16
#define ADDR_MAX 65535
#define BACKING_NAME "BACKING_STORE.bin"
#define OUTPUT_NAME "correct.txt"
#define EOL "\r\n"  

typedef enum { ALG_FIFO, ALG_LRU } alg_t;

//estado global
static int  num_frames = DEFAULT_FRAMES;
static int  use_tlb = 1;
static int  use_threads = 1;
static alg_t page_alg, tlb_alg;

static signed char phys_mem[MAX_FRAMES * PAGE_SIZE];
static int page_table[NUM_PAGES];          
static int frame_page[MAX_FRAMES];         
static long frame_loaded[MAX_FRAMES];       
static long frame_used[MAX_FRAMES];        
static int  next_free = 0;

typedef struct {
    int valido;
    int pagina;
    int frame;
    long carregado;
    long usado;
}tbl_entrada tbl[TLB_SIZE];

static long clock_tick = 0;
static long *apoio = NULL;


//pool de threads: uma thread permanente por entrada tlb
static pthread_t trabalhos[TLB_SIZE];
static int trabalho_comecou[TLB_SIZE];
static int trabalho_idx[TLB_SIZE];
static int match[TLB_SIZE];
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_comecou = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  cv_terminou= PTHREAD_COND_INITIALIZER;
static long geracao = 0; //incrementa a cada busca
static int pendente = 0; //threads que ainda nao terminaram 
static int pagina_alvo = 0;
static int parar_trabalho = 0;






