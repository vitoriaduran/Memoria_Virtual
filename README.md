# Gerenciador de Memória Virtual (Implementação 5 – SO)
 
Implementação em C do projeto *Designing a Virtual Memory Manager*
(Silberschatz et al., *Operating System Concepts*, 10ª ed., p. P-51) com as
modificações do enunciado: **128 frames**, substituição de página **FIFO/LRU**,
TLB de 16 entradas com substituição **FIFO/LRU** e busca na TLB com **uma
thread por entrada**.
 
## Requisitos
- Linux, Unix ou macOS com `gcc` (ou `cc`), `make` e `pthreads`.
- `BACKING_STORE.bin` (65 536 bytes, fornecido pelo professor) **no diretório
  em que o programa é executado**.
## Compilar e limpar
```sh
make          # gera o executável ./vm
make clean    # remove ./vm e arquivos objeto (não apaga correct.txt)
```
 
## Executar
```sh
./vm <arquivo_de_enderecos> <fifo|lru> <fifo|lru>
#                             |          |
#          substituição de página    substituição da TLB
./vm address.txt lru fifo     # exemplo do enunciado
```
A saída é gravada em **`correct.txt`** no diretório atual, no mesmo formato do
arquivo do professor (linhas terminadas em `\r\n`, sem quebra de linha após a
última linha, idêntico byte a byte ao `correct.txt` de referência):
```
Virtual address: 16916 TLB: 0 Physical address: 20 Value: 0
...
Number of Translated Addresses = 1000
Page Faults = 539
Page Fault Rate = 0.539
TLB Hits = 54
TLB Hit Rate = 0.054
```
O campo `TLB:` informa o **índice (0–15) da entrada da TLB** onde a página foi
encontrada (hit) ou inserida (miss).
 
> ⚠️ O programa **sobrescreve `correct.txt`** a cada execução. Guarde a cópia
> do professor com outro nome (ex.: `esperado.txt`) antes de executar.
 
## Decisões de projeto
| Tema | Decisão |
|---|---|
| Endereço | 16 bits: 8 bits de página + 8 bits de deslocamento (256 páginas de 256 B) |
| Memória física | 128 frames, preenchidos de 0 a 127; cheia → algoritmo de substituição |
| Substituição de página | **FIFO**: frame carregado há mais tempo. **LRU**: frame usado há mais tempo (um *hit* na TLB também conta como uso) |
| TLB | 16 entradas; ocupa primeiro os slots livres 0..15; depois **FIFO** (entrada mais antiga) ou **LRU** (menos recentemente usada) |
| Consistência | quando uma página é expulsa da memória, suas entradas na TLB são invalidadas |
| Threads | 16 threads permanentes (uma por entrada). Em cada consulta todas comparam, em paralelo, a página buscada com a sua entrada; sincronização por `mutex` + variáveis de condição (portável: funciona também no macOS, que não tem `pthread_barrier`) |
| Valor lido | `signed char` da memória física (`frame*256 + deslocamento`), carregada do backing store por `fseek`/`fread` a cada falta de página |
 
## Tratamento de erros
O programa valida **tudo antes de simular** e, em caso de erro, imprime mensagem
em `stderr`, retorna código `1` e **não gera `correct.txt`**:
1. número de argumentos ≠ 3;
2. 2º ou 3º argumento diferente de `fifo`/`lru` (minúsculas);
3. arquivo de endereços inexistente, ilegível ou diretório;
4. formato inválido: arquivo vazio, linha não numérica, negativo, decimal,
   hexadecimal, mais de um número por linha, linha vazia fora do final do
   arquivo, endereço fora de `0..65535`;
5. `BACKING_STORE.bin` ausente ou com menos de 65 536 bytes.
Aceita arquivos com fim de linha `\n` ou `\r\n`, com/sem `\n` final e com espaços/tabs no fim das linhas.
 
## Organização do código (`vm.c`)
| Função | Papel |
|---|---|
| `interpretar_algoritmo()` | valida `fifo`/`lru` (2º e 3º argumentos) |
| `ler_enderecos()` | lê e valida todo o arquivo de endereços antes de simular |
| `trabalhador_tlb()` | corpo de cada uma das 16 threads (compara só a sua entrada) |
| `iniciar_trabalhadores()` / `parar_todos_trabalhadores()` | criam e encerram o pool de threads |
| `buscar_tlb_threads()` / `buscar_tlb_sequencial()` | busca na TLB com e sem threads |
| `escolher_slot_tlb()` | slot livre ou vítima da TLB (FIFO/LRU) |
| `escolher_frame_vitima()` | frame vítima da memória física (FIFO/LRU) |
| `carregar_pagina()` | lê 256 bytes do `BACKING_STORE.bin` para o frame |
 
## Testes
```sh
cp <correct.txt do professor> esperado.txt   # opcional: comparação byte a byte
make test                                    # roda run_tests.sh (≈ 75 verificações)
```
Cobrem: comparação com o `correct.txt`, as 4 combinações de algoritmos,
determinismo, threads × sem threads, os fluxos da rubrica, semântica de
FIFO/LRU (páginas e TLB), limites válidos (0, 65535, fronteira 255/256, CRLF…)
e todos os casos de erro acima.
 
## Fluxos da rubrica (variáveis de ambiente opcionais)
O enunciado fixa 3 argumentos; para demonstrar os outros fluxos sem alterar a
interface existem variáveis de ambiente (sem elas, o comportamento é o do enunciado):
```sh
VM_FRAMES=256 VM_TLB=off ./vm address.txt fifo fifo   # sem TLB, 256 frames
VM_TLB=off ./vm address.txt fifo fifo                 # sem TLB, 128 frames, FIFO
VM_TLB=off ./vm address.txt lru fifo                  # sem TLB, 128 frames, LRU
VM_TLB_THREADS=off ./vm address.txt lru fifo          # TLB sem threads
./vm address.txt lru fifo                             # TLB com threads
```
 
## Evidências
```sh
script -a evidencias.log
sh evidencias.sh
exit
```
 
## Estrutura
`vm.c` · `Makefile` · `run_tests.sh` · `evidencias.sh` · `README.md` · `evidencias.log` · relatório (PDF)