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
}tlb_entrada;
static tlb_entrada tlb[TLB_SIZE];

static long clock_tick = 0;
static FILE *apoio = NULL;


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
        match[indice] = tlb[indice].valido && tlb[indice].pagina == pagina_alvo;

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


//Rubrica: fluxo tlb sem threads
//busca sequencial na tlb usada quando VM_TLB_THREADS=off
static int buscar_tlb_sequencial(int pagina){
    for (int i = 0; i < TLB_SIZE; i++){
        if (tlb[i].valido && tlb[i].pagina == pagina){
             return i;
        }
    }
    return -1;
}

//substituicao da tlb por fifo ou lru
//escolhe o slot livre (0..15) ou aplica FIFO/LRU para substituir uma entrada
static int escolher_slot_tlb(void){
    int melhor = -1;
     //primeiro, preenche slots livres 0..15 
    for (int i = 0; i < TLB_SIZE; i++)  {    
        if (!tlb[i].valido){
            return i;
        }
    }

    for (int i = 0; i < TLB_SIZE; i++) {
        long chave;
        if (tlb_alg == ALG_FIFO) {
            chave = tlb[i].carregado;
        }else {
            chave = tlb[i].usado;
        }

        long melhor_chave;
        if (melhor < 0) {
            melhor_chave = 0;
        }else if(tlb_alg == ALG_FIFO){
             melhor_chave = tlb[melhor].carregado;
        }else{
            melhor_chave = tlb[melhor].usado;
        }

        if (melhor < 0 || chave < melhor_chave){
            melhor = i;
        }
    }
    return melhor;
}

//Le 256 bytes da pagina no disco e grava no frame da memoria fisica
static int carregar_pagina(int pagina, int frame){
    if (fseek(apoio, (long)pagina * PAGE_SIZE, SEEK_SET) != 0 ||
        fread(&phys_mem[frame * PAGE_SIZE], 1, PAGE_SIZE, apoio) != PAGE_SIZE){
        return -1;
    }

    return 0;
}

//Rubrica: Substituicao de pagina com FIFO (10%) ou LRU (15%)
//escolhe o frame vitima com o menor tempo de carga (FIFO) ou ultimo acesso (LRU) 
static int escolher_frame_vitima(void){
    int melhor = 0;

    for (int f = 1; f < num_frames; f++) {
        long chave;

        if (page_alg == ALG_FIFO) {
            chave = frame_loaded[f];

        }else{

            chave = frame_used[f];
        }

        long melhor_chave;
        if(page_alg == ALG_FIFO){
            melhor_chave = frame_loaded[melhor];
        }else{

            melhor_chave = frame_used[melhor];
        }

        if (chave<melhor_chave){
            melhor = f;
        }
    }
    return melhor;
}
