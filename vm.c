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


//tratamento de erros
//validaçao de argumentos da cli e do arquivo de endereços
static void mostrar_uso(const char *prog){
    fprintf(stderr, "Uso: %s <arquivo_de_enderecos> <fifo|lru> <fifo|lru>\n" "2o argumento: algoritmo de substituicao de pagina\n" "3o argumento: algoritmo de substituicao da TLB\n", prog);
}

static int interpretar_algoritmo(const char *s, alg_t *saida){

    if (strcmp(s, "fifo") == 0){
        *saida = ALG_FIFO; return 0;
    }
    if(strcmp(s, "lru")  == 0){
         *saida = ALG_LRU;
           return 0;
     }

    return -1;
}

// Le e valida todos os enderecos antes de iniciar a simulacao
static int *ler_enderecos(const char *caminho, size_t *total_saida){
    FILE *arq = fopen(caminho, "rb");
    if (!arq){
        fprintf(stderr, "Erro: nao foi possivel abrir o arquivo de enderecos '%s': %s\n", caminho, strerror(errno));
        return NULL;
    }

    size_t capacidade = 1024, total = 0;
    int *vetor = malloc(capacidade * sizeof *vetor);
    if (!vetor){
        fclose(arq); fprintf(stderr, "Erro: memoria insuficiente\n"); 
        return NULL;
    }

    char *linha = NULL;
    size_t cap_linha = 0;
    ssize_t tam;
    size_t num_linha = 0, linhas_vazias = 0;
    int valido = 1;

    errno = 0;
    while ((tam = getline(&linha, &cap_linha, arq)) != -1){
        num_linha++;

        //remove \n, \r e espacos em branco finais
        while (tam > 0 && (linha[tam - 1] == '\n' || linha[tam - 1] == '\r' ||linha[tam - 1] == ' '  || linha[tam - 1] == '\t')){
            
            linha[--tam] = '\0';
        }

        // linha vazia: so aceita no fim do arquivo 
        if (tam == 0){                 
            linhas_vazias++;
            continue;
        }
          // se havia linha vazia no meio dos dados 
        if(linhas_vazias > 0){      

            fprintf(stderr, "Erro: linha %zu: linha vazia no meio do arquivo de enderecos\n", num_linha - 1);
            valido = 0; 
            break;
        }
        // aceita apenas digitos decimais sem sinal
        long long valor = 0;
        for (ssize_t k = 0; k < tam; k++) {
            if (!isdigit((unsigned char)linha[k])) {
                fprintf(stderr, "Erro: linha %zu: formato invalido ('%s'); esperado um inteiro decimal sem sinal\n",num_linha, linha);
                valido = 0;
                 break;
            }
            valor = valor * 10 + (linha[k] - '0');
            if (valor > ADDR_MAX){ 
                break;

            }
        }
        if(!valido){
            break;
        } 

        if (valor > ADDR_MAX){
            fprintf(stderr, "Erro: linha %zu: endereco fora do intervalo 0..%d\n", num_linha, ADDR_MAX);
            valido = 0;
            break;
        }

        if (total == capacidade) {
            capacidade *= 2;
            int *tmp = realloc(vetor, capacidade * sizeof *vetor);
            if (!tmp){
                fprintf(stderr, "Erro: memoria insuficiente\n");
                valido = 0;
                break;
            }
            vetor = tmp;
        }
        vetor[total++] = (int)valor;
    }
    if (valido && ferror(arq)) {
        fprintf(stderr, "Erro: falha ao ler '%s': %s\n", caminho, strerror(errno));
        valido = 0;
    }


    if(valido && total == 0) {
        fprintf(stderr, "Erro: arquivo de enderecos '%s' esta vazio\n", caminho);
        valido = 0;

    }
    free(linha);
    fclose(arq);

    if (!valido){
        free(vetor);
        return NULL;
    }

    *total_saida = total;
    return vetor;
}


static int variavel_ambiente_igual(const char *nome, const char *valor){
    const char *env = getenv(nome);

    return env && strcmp(env, valor) == 0;
}

//função principal main
int main(int argc, char **argv)
{
    // Validacao de quantidade e tipo dos argumentos da linha de comando 
    if (argc != 4) {
        fprintf(stderr, "Erro: numero de argumentos invalido (recebidos %d, esperados 3)\n", argc - 1);
        mostrar_uso(argv[0]);
        return 1;


    }

    if (interpretar_algoritmo(argv[2], &page_alg) != 0){
        fprintf(stderr, "Erro: algoritmo de pagina invalido '%s' (use fifo ou lru)\n", argv[2]);
        mostrar_uso(argv[0]);
        return 1;

    }
    if (interpretar_algoritmo(argv[3], &tlb_alg) != 0){
        fprintf(stderr, "Erro: algoritmo da TLB invalido '%s' (use fifo ou lru)\n", argv[3]);
        mostrar_uso(argv[0]);
        return 1;

    }

    // Configuracao de modos via variaveis de ambiente
    const char *env_frames = getenv("VM_FRAMES");
    if (env_frames && *env_frames){

        char *fim; long v = strtol(env_frames, &fim, 10);
        if(*fim || v < 1 || v > MAX_FRAMES){
            fprintf(stderr, "Erro: VM_FRAMES invalido ('%s'); use 1..%d\n", env_frames, MAX_FRAMES);
            return 1;

        }
        num_frames=(int)v;
    }

    if (variavel_ambiente_igual("VM_TLB", "off")){
        use_tlb = 0;
    }

    if (variavel_ambiente_igual("VM_TLB_THREADS", "off")){
        use_threads = 0;
    }


    //Leitura antecipada dos enderecos e abertura dos arquivos 
    size_t total_enderecos = 0;
    int *enderecos = ler_enderecos(argv[1], &total_enderecos);

    if (!enderecos){
        return 1;
    }

    apoio = fopen(BACKING_NAME, "rb");
    if (!apoio) {
        fprintf(stderr, "Erro: nao foi possivel abrir '%s': %s\n", BACKING_NAME, strerror(errno));
        free(enderecos);
        return 1;
    }
    if(fseek(apoio, 0, SEEK_END) != 0 || ftell(apoio) < (long)NUM_PAGES * PAGE_SIZE){

        fprintf(stderr, "Erro: '%s' invalido (esperados %d bytes)\n", BACKING_NAME, NUM_PAGES * PAGE_SIZE);
        fclose(apoio); free(enderecos);
        return 1;
    }

    //Paginaçao e tradução sem TLB

    FILE *saida = fopen(OUTPUT_NAME, "wb");
    if (!saida) {
        fprintf(stderr, "Erro: nao foi possivel criar '%s': %s\n", OUTPUT_NAME, strerror(errno));
        fclose(apoio); free(enderecos);
        return 1;
    }

    // Inicializa tabela de paginas e frames como vazios (-1) 
    for (int p = 0; p < NUM_PAGES; p++){
         page_table[p] = -1;
    }
    for (int f = 0; f < MAX_FRAMES; f++){
        frame_page[f] = -1;
    }


    // inicia o pool de 16 threads da TLB, se ativado 
    int threads_ativas = use_tlb && use_threads;
    if (threads_ativas && iniciar_trabalhadores() != 0){
        fprintf(stderr, "Aviso: falha ao criar threads da TLB; usando busca sequencial\n");
        parar_todos_trabalhadores();
        threads_ativas = 0;
    }

    long faltas_pagina = 0;
    long acertos_tlb = 0;
    int codigo_retorno = 0;

    //loop principal de traducao com TLB (threads ou sequencial) + Paginacao 
    for (size_t k = 0; k < total_enderecos; k++) {

        int endereco = enderecos[k];
        int pagina       = (endereco >> 8) & 0xFF;
        int deslocamento = endereco & 0xFF;
        int frame, slot = -1;
        clock_tick++;

        //passo A: Consulta a TLB (com threads ou sequencial) 
        int indice_tlb = -1;
        if (use_tlb){
            indice_tlb = threads_ativas ? buscar_tlb_threads(pagina) : buscar_tlb_sequencial(pagina);

        }
        //tlb hit
        if (indice_tlb >= 0){

            acertos_tlb++;
            slot = indice_tlb;
            frame = tlb[indice_tlb].frame;
            tlb[indice_tlb].usado = clock_tick;
            frame_used[frame] = clock_tick;
        
         //TLB Miss (ou TLB desligada) 
        }else{               
            //Acerto na Tabela de Paginas                         
            if (page_table[pagina] >= 0) {              
                frame = page_table[pagina];
                frame_used[frame] = clock_tick;

            //falta de pagina
            } else {                                   
                faltas_pagina++;

                //Ainda ha frames livres 
                if (next_free < num_frames) {           
                    frame = next_free++;
                
                }
                //Memoria cheia: substitui pagina vitima 
                else{                               
                    frame = escolher_frame_vitima();
                    int pagina_antiga = frame_page[frame];
                    page_table[pagina_antiga] = -1;

                 
                    pthread_mutex_lock(&mtx);
                    for (int i = 0; i < TLB_SIZE; i++)
                        if (tlb[i].valido && tlb[i].pagina == pagina_antiga)
                            tlb[i].valido = 0;
                    pthread_mutex_unlock(&mtx);
                    
                }
                if (carregar_pagina(pagina, frame) != 0) {
                    fprintf(stderr, "Erro: falha ao ler a pagina %d de '%s'\n", pagina, BACKING_NAME);
                    codigo_retorno = 1;
                    break;
                }
                page_table[pagina]  = frame;
                frame_page[frame]   = pagina;
                frame_loaded[frame] = clock_tick;
                frame_used[frame]   = clock_tick;
            }
            //Atualiza a TLB com a nova traducao pagina - frame
            if (use_tlb) {

                pthread_mutex_lock(&mtx);
                slot = escolher_slot_tlb();
                tlb[slot].valido    = 1;
                tlb[slot].pagina    = pagina;
                tlb[slot].frame     = frame;
                tlb[slot].carregado = clock_tick;
                tlb[slot].usado     = clock_tick;
                pthread_mutex_unlock(&mtx);
            }
        }

        //calcula o endereco fisico e le o byte da memoria fisica 
        int endereco_fisico = frame * PAGE_SIZE + deslocamento;
        int valor = phys_mem[endereco_fisico];
        fprintf(saida, "Virtual address: %d TLB: %d Physical address: %d Value: %d" EOL, endereco, use_tlb ? slot : 0, endereco_fisico, valor);
    }

    if (threads_ativas){
        parar_todos_trabalhadores();
    
    }

    //Imprime as estatisticas finais 
    if (codigo_retorno == 0) {
        fprintf(saida, "Number of Translated Addresses = %zu" EOL, total_enderecos);
        fprintf(saida, "Page Faults = %ld" EOL, faltas_pagina);
        fprintf(saida, "Page Fault Rate = %.3f" EOL, (double)faltas_pagina / (double)total_enderecos);
        fprintf(saida, "TLB Hits = %ld" EOL, acertos_tlb);
        fprintf(saida, "TLB Hit Rate = %.3f", (double)acertos_tlb / (double)total_enderecos);
    }

    if (fclose(saida) != 0) {
        fprintf(stderr, "Erro: falha ao gravar '%s': %s\n", OUTPUT_NAME, strerror(errno));
        codigo_retorno = 1;
    }
    fclose(apoio);
    free(enderecos);
    return codigo_retorno;


}