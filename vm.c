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
}tbl_entrada;
static tbl_entrada tbl[TLB_SIZE];

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


//funçao executada por cada uma das 16 threads da tlb
static void *trabalhador_tlb(void *arg) {
    int indice = *(int *)arg;
    long vista = 0;

    pthread_mutex_lock(&mtx);
    for (;;) {
        // Aguarda a thread principal disparar uma nova busca 
        while (geracao == vista && !parar_trabalho){

            pthread_cond_wait(&cv_comecou, &mtx);
        }
        if (parar_trabalho){
            break;
        
        }

        vista = geracao;
        //Cada thread verifica EXCLUSIVAMENTE a sua propria entrada na TLB 
        match[indice] = tbl[indice].valido && tbl[indice].pagina == pagina_alvo;

        //A ultima thread a terminar sinaliza a thread principal 
        pendente --;
        if (pendente == 0){
            pthread_cond_signal(&cv_terminou);
        
        }
    }
    pthread_mutex_unlock(&mtx);
    return NULL;
}


//cria e inicializa as 16 threads trabalhadoras antes do loop de endereços
static int iniciar_trabalhadores(void) {
    for (int i = 0; i < TLB_SIZE; i++){

        trabalho_idx[i] = i;
        if (pthread_create(&trabalhos[i], NULL, trabalhador_tlb, &trabalho_idx[i]) != 0){
            return -1;

        }

        trabalho_comecou[i] = 1;
    }

    return 0;
}

//encerra todas as threads de forma limpa ao final da execucao
static void parar_todos_trabalhadores(void) {

    pthread_mutex_lock(&mtx);
    parar_trabalho = 1;
    pthread_cond_broadcast(&cv_comecou);
    pthread_mutex_unlock(&mtx);

    for (int i = 0; i < TLB_SIZE; i++){
        if (trabalho_comecou[i]) {

            pthread_join(trabalhos[i], NULL);
            trabalho_comecou[i] = 0;
        }
    }

}

//dispara uma busca paralela nas 16 entradas da TLB e retorna o slot ou -1 
static int buscar_tlb_threads(int pagina) {
    int encontrado = -1;

    pthread_mutex_lock(&mtx);
    pagina_alvo = pagina;
    pendente = TLB_SIZE;
    geracao++;
    pthread_cond_broadcast(&cv_comecou);

    while (pendente > 0){
        pthread_cond_wait(&cv_terminou, &mtx);

    }

    for (int i = 0; i < TLB_SIZE; i++){

        if (match[i]){
             encontrado = i; 
             break;
        }
    }
    
    pthread_mutex_unlock(&mtx);
    return encontrado;
}
