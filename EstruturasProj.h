#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timeb.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#define PIPE_NAME "input_pipe"
#define MAX_BUFFER  10
#define MAX 1000
#define MUTEX 0


typedef struct dados_config{ //struct para os dados da config
  int unit_temp; //em milisegundos
  int dur_descolagem; //duracao da dur_descolagem
  int int_descolagem; //intervalos entre descolagens
  int dur_aterragem; //duracao da dur_aterragem
  int int_aterragem; //intervalos entre aterragens
  int min_holding; //holding mínimo
  int max_holding; //holding máximo
  int max_partidas; //maximo de partidas
  int max_chegadas; //maximo de chegadas
}Dados_config;

typedef struct voo_aterrar *aterrar;
typedef struct voo_descolar * descolar;

typedef struct voo_aterrar{ //struct para os voos que aterram
  int muda;
  int state;
  char nome[MAX_BUFFER];
  int init; //instante inicial do voo
  int eta; //estimated time of arrival aka tempo que demora a chegar
  int fuel;//pode chegar até 0 e se chegar é desviado para outro aeroporto
  int id;
  aterrar next;
}Voo_aterrar;

typedef struct voo_descolar{ //struct para os voos que descolam
  int muda;
  int state;
  char nome[MAX_BUFFER];
  int init; //instante inicial do voo
  int takeoff; //instante desejado de descolagem
  int id;
  descolar next;
}Voo_descolar;

typedef struct message{
  long mytype;
  int tipo;
  int voo;
  char sms[1024];
  Voo_aterrar ms_aterra;
  Voo_descolar ms_descola;
}Message;

typedef struct pista{
  char nome[MAX_BUFFER];
  long final;
}Pista;

typedef struct estatisticas{
  int total_voos;
  int total_voos_aterra;
  int total_voos_descola;
  int total_hold;
  int total_pocrl;
  int total_rejeitados;
  int total_temp_aterra;
  int total_temp_descola;
  double t_med_aterra;
  double t_med_descola;
}Estatisticas;


//INFORMAÇÃO SOBRE O PIPE
char* myfifo = "/tmp/input_pipe";

//INFORMAÇÕES DOS PROCESSOS
pid_t Torre_Controlo;
pid_t processo_manager;

//INFORMAÇÃO DA CONFIG
Dados_config *dados_config;
char mensagem[MAX];
long pos;

//PISTAS
pthread_t thr_28L, thr_28R, thr_01L, thr_01R;

//INFORMAÇÃO SOBRE AS PISTAS
Pista *pista;
sem_t semid_01L, semid_01R, semid_28L, semid_28R;
int value01L, value01R, value28L, value28R;

//INFORMAÇÃO DAS ATERRAGENS
Voo_aterrar *dados_aterrar;

//INFORMAÇÃO DAS DESCOLAGENS
Voo_descolar *dados_descolar;

//INFORMAÇÕES MEM PARTILHADA
int shmid_aterra, shmid_descola;
Voo_aterrar *voo_aterra_shm;
Voo_descolar *voo_descola_shm;
int valor;

//INFORMAÇÃO SOBRE MEM PARTILHADA DA ESTATISTICA
Estatisticas *estat_shm;
int shmid_estat;

//INFORMAÇÕES DA MESSAGE QUEUE
int mqs_thread_to_ct, mqs_ct_to_thread;
struct message rcmessage;
struct message messagesnt;

//INFORMAÇÕES DO PIPE
int fd;

//INFORMAÇÕES SOBRE THREADS
pthread_t thr_aterra[MAX], thr_descola[MAX];

//INFORMAÇÕES DO MUTEX
pthread_mutex_t write_file = PTHREAD_MUTEX_INITIALIZER;

//MUTEX E CONDITIONAL VARIABLES
pthread_cond_t cond  = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex_aterrar = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_descolar = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_pista_aterra = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_pista_descola = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_ctrl_c = PTHREAD_MUTEX_INITIALIZER;
struct timespec t;

//THREADS PARA ESCALONAR VOOS
pthread_t thr_pista_aterra[MAX], thr_pista_descola[MAX];
int id_aterra = 0;
int id_descola = 0;


//CABEÇA DAS LISTAS
aterrar head_aterrar = NULL;
descolar head_descolar = NULL;

//FUNÇÕES
Dados_config le_ficheiro();
void cria_pipe();
int cria_msq();
void cria_mem_aterra();
void cria_mem_descola();
void cria_mem_estat();
void cria_sem_pistas();
void escreve_log(char* mensagem);
void valida_comando();
void *cria_thread_aterra(void *arg);
void *cria_thread_descola(void *arg);
void cria_headers();
void inicia_estats();
void estatisticas();
aterrar adiciona_lista_aterra(aterrar head_aterrar, aterrar dados_aterrar);
descolar adiciona_lista_descola(descolar head_descolar, descolar dados_descolar);
void print_lista_aterra(aterrar lista);
void print_lista_descola(descolar lista);
aterrar remove_no_aterra(aterrar lista, aterrar no);
descolar remove_no_descola(descolar lista, descolar no);
void cleanup(int sig);
void coloca_na_pista_aterragens();
void *cria_thread_pista_descola(void *arg);
void *cria_thread_pista_aterra(void *arg);
void ignoreControlC(int sig);

//CÓDIGO DAS FUNÇÕES

Dados_config le_ficheiro(){
  //ALOCAÇÃO DINÂMICA
  dados_config = (Dados_config *) malloc(sizeof(Dados_config));

  //LEITURA DO FICHEIRO
  FILE *fp = fopen("/home/parallels/Desktop/Parallels Shared Folders/Home/Desktop/ProjetoSO2019/config.txt", "r");

  if(fp != NULL){
    //PRIMEIRA LINHA
    fscanf(fp,"%d",&dados_config->unit_temp);
    pos = ftell(fp);
    fseek(fp, pos, SEEK_SET); //muda de linha

    //SEGUNDA LINHA
    fscanf(fp,"%d, %d",&dados_config->dur_descolagem,&dados_config->int_descolagem);
    fflush(fp);
    pos = ftell(fp);
    fseek(fp, pos, SEEK_SET);

    //TERCEIRA LINHA
    fscanf(fp,"%d, %d",&dados_config->dur_aterragem,&dados_config->int_aterragem);
    fflush(fp);
    pos = ftell(fp);
    fseek(fp, pos, SEEK_SET);

    //QUARTA LINHA
    fscanf(fp,"%d, %d",&dados_config->min_holding,&dados_config->max_holding);
    fflush(fp);
    pos = ftell(fp);
    fseek(fp, pos, SEEK_SET);
    fscanf(fp,"%d",&dados_config->max_partidas);
    fflush(fp);
    pos = ftell(fp);
    fseek(fp, pos, SEEK_SET);
    fscanf(fp,"%d",&dados_config->max_chegadas);
    fflush(fp);
    pos = ftell(fp);
    fseek(fp, pos, SEEK_SET);
    fclose(fp);
  }
  else{
    printf("Erro no ficheiro de config!\n");
  }
  return (*dados_config);
}

int cria_msq(){

  //CRIA AS MESSAGE QUEUE'S, UMA PARA AS PARTIDAS E OUTRA PARA AS CHEGADAS
  if ((mqs_thread_to_ct = msgget(IPC_PRIVATE, IPC_CREAT | 0777)) < 0) {
    perror("Erro no msgget!\n");
    return -1;
  }
  if ((mqs_ct_to_thread = msgget(IPC_PRIVATE, IPC_CREAT | 0777)) < 0) {
    perror("Erro no msgget!\n");
    return -1;
  }
  return 0;

}

void cria_mem_aterra(){

  //CRIA A MEMÓRIA PARTILHADA USANDO AS FUNÇÕES SHMGET E SHMAT
  if ((shmid_aterra = shmget(IPC_PRIVATE, sizeof(Voo_aterrar), IPC_CREAT | 0777)) < 0){
    printf("Erro no shmget!\n");
  }
  else if((voo_aterra_shm = (Voo_aterrar *) shmat(shmid_aterra,NULL,0)) < 0) {
    printf("Erro no shmat!\n");
  }
}

void cria_mem_descola(){

  //CRIA A MEMÓRIA PARTILHADA USANDO AS FUNÇÕES SHMGET E SHMAT
  if ((shmid_descola = shmget(IPC_PRIVATE, sizeof(Voo_descolar), IPC_CREAT | 0777)) < 0){
    printf("Erro no shmget!\n");
  }
  else if((voo_descola_shm = (Voo_descolar *) shmat(shmid_descola,NULL,0)) < 0) {
    printf("Erro no shmat!\n");
  }
}

void cria_mem_estat(){
  if ((shmid_estat = shmget(IPC_PRIVATE, sizeof(Estatisticas), IPC_CREAT | 0777)) < 0){
    printf("Erro no shmget!\n");
  }
  else if((estat_shm = (Estatisticas *) shmat(shmid_estat,NULL,0)) < 0) {
    printf("Erro no shmat!\n");
  }
}

void cria_pipe(){

  //CRIA O NAMED PIPE COM RECURSO À FUNÇÃO MKFIFO
  if ((mkfifo(myfifo, O_CREAT | O_EXCL | 0600) < 0) && (errno != EEXIST)) {
    perror("Erro no mkfifo!\n");
  }
}

void cria_sem_pistas(){
  //CRIA OS SEMÁFOROS DAS PISTAS
  sem_init(&semid_01L, 0, 1);
  sem_init(&semid_01R, 0, 1);
  sem_init(&semid_28L, 0, 1);
  sem_init(&semid_28R, 0, 1);
}

void escreve_log(char mensagem[]){
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  FILE *f = fopen("/home/parallels/Desktop/Parallels Shared Folders/Home/Desktop/ProjetoSO2019/log.txt", "a");
  printf("%02d:%02d:%02d %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, mensagem);
  if(f != NULL){
    fseek(f,0,SEEK_END);
    fprintf(f, "%02d:%02d:%02d %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, mensagem);
    fclose(f);
  }
  mensagem[0] = '\0';
}

void valida_comando(Dados_config dados_config){

  int t_init, t_takeoff, t_eta, t_fuel, n_char;
  char init[MAX], takeoff[MAX], eta[MAX], fuel[MAX];
  char buff[MAX];
  char linha[MAX];
  char voo_id[MAX];
  char *token;
  int max_partidas = dados_config.max_partidas;
  int max_chegadas = dados_config.max_chegadas;
  int i = 0;

  while(1){
    char new[] = "NEW COMAND => ";
    char wrong[] = "WRONG COMAND => ";

    //ABRE O PIPE
    if ((fd = open(myfifo, O_RDONLY)) < 0){
      perror("Impossivel abrir o pipe!\n");
      exit(0);
    }

    //LEITURA DO PIPE
    n_char = read(fd, buff, MAX);
    buff[n_char-1] = '\0';
    fflush(stdout);
    strcpy(linha, buff);
    fflush(stdout);
    /*NA VARIÁVEL 'token' FICA ARMEAZENADA A PRIMEIRA PALAVRA DO COMANDO,
    ASSIM É POSSÍVEL IDENTIFICAR SE É UM COMANDO 'arrival', 'departure', OU
    UM ERRO DE DIGITAÇÃO*/
    token = strtok(buff, " ");
    fflush(stdout);

    if(strcmp(token, "DEPARTURE") == 0){
      //VERIFICA SE JÁ FOI EXCEDIDO O NÚMERO MÁXIMO DE DESCOLAGENS
      if(max_partidas == 0){
        printf("Excedeu o número de partidas!\n");
        exit(0);
      }

      sscanf(linha, "DEPARTURE %s %s %d %s %d", voo_id, init, &t_init, takeoff, &t_takeoff);
      if ((strcmp(init, "init:") == 0) && (strcmp(takeoff, "takeoff:") == 0)){
        strcpy(token, linha);
        token = strcat(new,token);
        pthread_mutex_unlock(&write_file);
        escreve_log(token);
        pthread_mutex_lock(&write_file);
        dados_config.max_partidas = dados_config.max_partidas - 1;

        //COLOCA OS VALORES DO COMANDO PARA A ESTRUTURA 'Voo_descola'
        dados_descolar = malloc(sizeof(Voo_descolar));
        strcpy(dados_descolar->nome, voo_id);
        dados_descolar->init = t_init;
        dados_descolar->takeoff = t_takeoff;
        dados_descolar->id = i;
        dados_descolar->next = NULL;
        pthread_create(&thr_descola[i],NULL,cria_thread_descola, dados_descolar);
        token = NULL;
        estat_shm->total_voos++;
        close(fd);
      }
      else{
        strcpy(token, linha);
        token = strcat(wrong,token);
        pthread_mutex_unlock(&write_file);
        escreve_log(token);
        pthread_mutex_lock(&write_file);
        token = NULL;
        close(fd);
      }
    }
    else if(strcmp(token, "ARRIVAL") == 0){

      //VERIFICA SE JÁ FOI EXCEDIDO O NÚMERO MÁXIMO DE CHEGADAS
      if(max_chegadas == 0){
        printf("Excedeu o número de chegadas!\n");
        exit(0);
      }
      sscanf(linha, "ARRIVAL %s %s %d %s %d %s %d", voo_id, init, &t_init, eta, &t_eta, fuel, &t_fuel);
      if ((strcmp(init, "init:") == 0) && (strcmp(eta, "eta:") == 0) && (strcmp(fuel, "fuel:") == 0)){
        strcpy(token, linha);
        token = strcat(new,token);
        pthread_mutex_unlock(&write_file);
        escreve_log(token);
        pthread_mutex_lock(&write_file);
        dados_config.max_chegadas = dados_config.max_chegadas - 1;

        //COLOCA OS VALORES DO COMANDO PARA A ESTRUTURA 'Voo_aterra'
        dados_aterrar = malloc(sizeof(Voo_aterrar));
        strcpy(dados_aterrar->nome, voo_id);
        dados_aterrar->init = t_init;
        dados_aterrar->eta = t_eta;
        dados_aterrar->fuel = t_fuel;
        dados_aterrar->id = i;
        dados_aterrar->next = NULL;

        pthread_create(&thr_aterra[i], NULL, cria_thread_aterra, dados_aterrar);
        estat_shm->total_voos++;
        token = NULL;
        close(fd);
      }
      else{
        strcpy(token, linha);
        token = strcat(wrong,token);
        pthread_mutex_unlock(&write_file);
        escreve_log(token);
        pthread_mutex_lock(&write_file);
        token = NULL;
        close(fd);
      }
    }
    else{
      strcpy(token, linha);
      token = strcat(wrong,token);
      pthread_mutex_unlock(&write_file);
      escreve_log(token);
      pthread_mutex_lock(&write_file);
      token = NULL;
      close(fd);
    }
    i++;
    max_partidas--;
    max_chegadas--;
  }
}

void cria_headers(){

  //CRIA AS CABEÇAS DAS LISTAS LIGADAS
  head_aterrar = (aterrar)malloc(sizeof(Voo_aterrar));
  head_aterrar->next = NULL;

  head_descolar = (descolar)malloc(sizeof(Voo_descolar));
  head_descolar->next = NULL;
}

aterrar adiciona_lista_aterra(aterrar head_aterrar, aterrar dados_aterrar){

  //ADICIONA UM NÓ NA LISTA DE ATERRAGENS
  aterrar act = head_aterrar, ant=NULL;
  while (act)
  {
    ant = act;
    act = act->next;
  }
  dados_aterrar->next = act;
  if(ant == NULL){
    head_aterrar = dados_aterrar;
    return head_aterrar;
  }
  else{
    ant->next = dados_aterrar;
  }
  return head_aterrar;

}

descolar adiciona_lista_descola(descolar head_descolar, descolar dados_descolar){

  //ADICIONA UM NÓ NA LISTA DE DESCOLAGENS
  descolar act = head_descolar, ant=NULL;
  while (act)
  {
    ant = act;
    act = act->next;
  }
  dados_descolar->next = act;
  if(ant == NULL){
    head_descolar = dados_descolar;
    return head_descolar;
  }
  else{
    ant->next = dados_descolar;
  }
  return head_descolar;
}

void print_lista_aterra(aterrar lista){

  /*FUNÇÃO QUE APRESENTA AS INFORMAÇÕES SOBRE CADA NÓ DA LISTA DE ATERRAGENS,
  NÃO FOI USADA NA VERSÃO FINAL DO PROGRAMA, MAS FOI UMA FUNÇÃO FULCRAL PARA A
  ANÁLISE DOS DADOS*/
  int i = 1;
  if (lista->next == NULL){
    printf("Lista vazia!\n");
    return;
  }
  lista = lista->next;
  while(lista){
    printf("%dº nó:\n", i);
    printf("%s\n", lista->nome);
    printf("%d\n", lista->init);
    printf("%d\n", lista->eta);
    printf("%d\n", lista->fuel);
    if (lista->next == NULL){
      printf("lista lida!\n");
      break;
    }
    lista = lista->next;
    i++;
  }
}

void print_lista_descola(descolar lista){

  /*FUNÇÃO QUE APRESENTA AS INFORMAÇÕES SOBRE CADA NÓ DA LISTA DE DESCOLAGENS,
  NÃO FOI USADA NA VERSÃO FINAL DO PROGRAMA, MAS FOI UMA FUNÇÃO FULCRAL PARA A
  ANÁLISE DOS DADOS*/
  int i = 1;
  if (lista->next == NULL){
    printf("Lista vazia!\n");
    return;
  }
  lista = lista->next;
  while(lista){
    printf("%dº nó:\n", i);
    printf("%s\n", lista->nome);
    printf("%d\n", lista->init);
    printf("%d\n", lista->takeoff);
    if (lista->next == NULL){
      printf("lista lida!\n");
      break;
    }
    lista = lista->next;
    i++;
  }
}

aterrar remove_no_aterra(aterrar lista, aterrar no){

  //REMOVE O NÓ CORRESPONDENTE À THREAD DE ATERRAGEM
  aterrar aux = lista , temp;
  while (aux){
    if (strcmp(aux->nome, no->nome) == 0){
      break;
    }
    if (aux->next == NULL){
      break;
    }
    temp = aux;
    aux = aux -> next;
  }
  temp -> next = aux -> next;
  free(aux);
  return lista;
}

descolar remove_no_descola(descolar lista, descolar no){

  //REMOVE O NÓ CORRESPONDENTE À THREAD DE DESCOLAGEM
  descolar aux = lista , temp;
  while (aux){
    if (strcmp(aux->nome, no->nome) == 0){
        break;
    }
    if (aux->next == NULL){
      break;
    }
    temp = aux;
    aux = aux -> next;
  }
  temp -> next = aux -> next;
  free(aux);
  return lista;
}

void *cria_thread_aterra(void *arg){
  aterrar aux;
  int tempo_espera;
  time_t T,F;
  time(&T);

  t.tv_sec = T + dados_aterrar->init;
  pthread_mutex_lock(&mutex_aterrar);
  if (pthread_cond_timedwait(&cond, &mutex_aterrar, &t) != 0){
    aux = malloc(sizeof(Voo_aterrar));
    strcpy(aux->nome, dados_aterrar->nome);
    aux->init = dados_aterrar->init;
    aux->eta = dados_aterrar->eta;
    aux->fuel = dados_aterrar->fuel;
    aux->next = NULL;
    head_aterrar = adiciona_lista_aterra(head_aterrar, arg);
  }
  pthread_mutex_unlock(&mutex_aterrar);

  //VERIFICA SE O FUEL É MAIOR QUE A (DURAÇÃO DE ATERRAGEM + ETA)
  if (aux->fuel < dados_config->dur_aterragem + aux->eta){
    pthread_mutex_lock(&mutex_aterrar);
    sprintf(mensagem, "%s LEAVING TO OTHER AIRPORT (IMPOSSIBILITY TO HOLD)", aux->nome);
    //ESCREVE NO LOG.TXT
    pthread_mutex_unlock(&write_file);
    escreve_log(mensagem);
    pthread_mutex_lock(&write_file);
    head_aterrar = remove_no_aterra(head_aterrar, aux);
    free(aux);
    estat_shm->total_rejeitados++;
    pthread_mutex_unlock(&mutex_aterrar);
    pthread_exit(NULL);
  }


  if (voo_descola_shm->state == 0 || voo_descola_shm->state == 2){
    //TENTATIVA DE ATERRAGEM
    pthread_mutex_lock(&mutex_aterrar);
    messagesnt.mytype = 1;
    messagesnt.ms_aterra.id = aux->id;
    messagesnt.voo = 0;
    strcpy(messagesnt.ms_aterra.nome, aux->nome);
    msgsnd(mqs_thread_to_ct, &messagesnt, sizeof(messagesnt), 0);
    //ESPERA DA RESPOSTA
    msgrcv(mqs_ct_to_thread, &rcmessage, sizeof(rcmessage) + 1, 2, 0);
    pthread_mutex_unlock(&mutex_aterrar);
    //SE HOUVER UMA PISTA VAZIA ELE ENTRA PARA A PISTA E TERMINA A THREAD VOO
    if (rcmessage.tipo == 1){
      pthread_mutex_lock(&mutex_aterrar);
      head_aterrar = remove_no_aterra(head_aterrar, aux);
      free(aux);
      pthread_mutex_unlock(&mutex_aterrar);
      pthread_exit(NULL);
    }
  }

  //SE AS PISTAS ESTIVEREM AMBAS OCUPADAS ENTRA EM HOLDING
  if (rcmessage.tipo == 0){
    pthread_mutex_lock(&mutex_aterrar);
    sprintf(mensagem, "%s HOLDING %d", aux->nome, (aux->eta + dados_config->dur_aterragem));
    //ESCREVE NO LOG.TXT
    pthread_mutex_unlock(&write_file);
    escreve_log(mensagem);
    pthread_mutex_lock(&write_file);
    estat_shm->total_hold++;
    pthread_mutex_unlock(&mutex_aterrar);

    //ENQUANTO O FUEL NÃO CHEGAR A ZERO, ELE VAI VERIFICAR SE HÁ ALGUMA PISTA VAZIA
    while (aux->fuel != 0){

      if (voo_descola_shm->state == 0 || voo_descola_shm->state == 2){
      //TENTATIVA DE ATERRAGEM
        messagesnt.mytype = 1;
        messagesnt.ms_aterra.id = aux->id;
        messagesnt.voo = 0;
        strcpy(messagesnt.ms_aterra.nome, aux->nome);
        msgsnd(mqs_thread_to_ct, &messagesnt, sizeof(messagesnt), 0);
        //ESPERA DA RESPOSTA
        msgrcv(mqs_ct_to_thread, &rcmessage, sizeof(rcmessage) + 1, 2, 0);
        if (rcmessage.tipo == 1){
          time(&F);
          tempo_espera = (F-T);
          estat_shm->total_temp_aterra += tempo_espera;
          head_aterrar = remove_no_aterra(head_aterrar, aux);
          free(aux);
          pthread_exit(NULL);
        }
      }
      sleep(1);
      aux->fuel--;
    }

    //CASO NÃO HAJA ENTRA EM EMERGENCY LANDING
    sprintf(mensagem, "%s EMERGENCY LANDING REQUESTED", aux->nome);
    //ESCREVE NO LOG.TXT
    pthread_mutex_unlock(&write_file);
    escreve_log(mensagem);
    pthread_mutex_lock(&write_file);

    if (voo_descola_shm->state == 0 || voo_descola_shm->state == 2){
    //ÚLTIMA TENTATIVA DE ATERRAGEM
      messagesnt.mytype = 1;
      messagesnt.ms_aterra.id = aux->id;
      messagesnt.voo = 0;
      strcpy(messagesnt.ms_aterra.nome, aux->nome);
      msgsnd(mqs_thread_to_ct, &messagesnt, sizeof(messagesnt), 0);
      //ESPERA DA RESPOSTA
      msgrcv(mqs_ct_to_thread, &rcmessage, sizeof(rcmessage) + 1, 2, 0);
      if (rcmessage.tipo == 1){
        time(&F);
        tempo_espera = (F-T);
        estat_shm->total_temp_aterra += tempo_espera;
        head_aterrar = remove_no_aterra(head_aterrar, aux);
        free(aux);
        pthread_exit(NULL);
      }
    }
    else{
        sprintf(mensagem, "%s LEAVING TO ANOTHER AIRPORT => FUEL = 0", aux->nome);
        //ESCREVE NO LOG.TXT
        pthread_mutex_unlock(&write_file);
        escreve_log(mensagem);
        pthread_mutex_lock(&write_file);
        estat_shm->total_pocrl++;
    }
    pthread_mutex_unlock(&mutex_aterrar);
  }

  //A FUNÇÃO COMENTADA TINHA COMO OBJETIVO SER USADA PARA TRATAMENTO DE DADOS
  //print_lista_aterra(head_aterrar);

  head_aterrar = remove_no_aterra(head_aterrar, aux);
  free(aux);
  pthread_mutex_unlock(&mutex_aterrar);
  pthread_exit(NULL);
  return 0;
}

void *cria_thread_descola(void *arg){
  int rc;
  descolar aux;
  int tempo_espera;
  time_t T, F;
  time(&T);
  t.tv_sec = T + dados_descolar->init;
  pthread_mutex_lock(&mutex_descolar);
  if (pthread_cond_timedwait(&cond, &mutex_descolar, &t) != 0){
    aux = malloc(sizeof(Voo_descolar));
    strcpy(aux->nome, dados_descolar->nome);
    aux->init = dados_descolar->init;
    aux->takeoff = dados_descolar->takeoff;
    aux->id = dados_descolar->id;
    aux->next = NULL;
    head_descolar = adiciona_lista_descola(head_descolar, arg);
  }
  pthread_mutex_unlock(&mutex_descolar);

  time(&T);
  t.tv_sec = T + aux->takeoff;
  pthread_mutex_lock(&mutex_descolar);
  if ((rc = pthread_cond_timedwait(&cond, &mutex_descolar, &t)) != 0){
    //DADOS PARA ENVIO DA MENSAGEM
    pthread_mutex_unlock(&mutex_descolar);

    time(&T);

    if (voo_aterra_shm->state != 0){
      while (voo_aterra_shm->state != 0){
        time(&F);
      }
      tempo_espera = F-T;
      estat_shm->total_temp_descola += tempo_espera;
    }
    else{
      tempo_espera = 0;
    }

    pthread_mutex_lock(&mutex_descolar);
    messagesnt.mytype = 1;
    messagesnt.voo = 1;
    messagesnt.ms_descola.id = aux->id;
    strcpy(messagesnt.ms_descola.nome, aux->nome);
    msgsnd(mqs_thread_to_ct, &messagesnt, sizeof(messagesnt), 0);
    //ESPERA DA RESPOSTA
    msgrcv(mqs_ct_to_thread, &rcmessage, sizeof(rcmessage) + 1, 1, 0);

    if (voo_descola_shm->muda == 1){
      while (voo_descola_shm->state != 0){
        time(&F);
      }
      tempo_espera = F-T;
      estat_shm->total_temp_descola += tempo_espera;

      messagesnt.mytype = 1;
      messagesnt.voo = 1;
      messagesnt.ms_descola.id = aux->id;
      strcpy(messagesnt.ms_descola.nome, aux->nome);
      msgsnd(mqs_thread_to_ct, &messagesnt, sizeof(messagesnt), 0);
      //ESPERA DA RESPOSTA
      msgrcv(mqs_ct_to_thread, &rcmessage, sizeof(rcmessage) + 1, 1, 0);
    }
    pthread_mutex_unlock(&mutex_descolar);
  }

  head_descolar = remove_no_descola(head_descolar, aux);
  free(aux);

  //A FUNÇÃO COMENTADA TINHA COMO OBJETIVO SER USADA PARA TRATAMENTO DE DADOS
  //print_lista_descola(head_descolar);

  pthread_exit(NULL);
  return 0;
}

void *cria_thread_pista_aterra(void *arg){
  int state;
  int estado = 1;
  int rc;
  time_t T;
  char nome_voo[MAX];

  time(&T);
  t.tv_sec = T + dados_config->dur_aterragem;
  strcpy(nome_voo, rcmessage.ms_aterra.nome);

  //VERIFICA O VALOR DOS SEMÁFOROS PARA SABER QUE PISTAS ESTÃO DISSPONÍVEIS
  sem_getvalue(&semid_28L, &value28L);
  sem_getvalue(&semid_28R, &value28R);

  pthread_mutex_lock(&mutex_aterrar);
  if (value28L == 1 && value28R == 1){
    //'messagesnt.mytype' REFERE-SE AO TIPO "ARRIVAL"
    messagesnt.mytype = 2;
    //'messagesnt.tipo' MUDA PARA 1 PARA AVISAR A THREAD VOO QUE HÁ PISTAS LIVRES
    messagesnt.tipo = 1;
    //O VALOR DO STATE INDICA A PISTA PARA ONDE ELE VAI ENTRAR
    state = 1;
    (void)strcpy(messagesnt.sms, "->    Pista 1");
    msgsnd(mqs_ct_to_thread, &messagesnt, sizeof(messagesnt), 0);
    pthread_mutex_unlock(&mutex_aterrar);
    sem_wait(&semid_28L);
    voo_aterra_shm->state = 1;
  }
  if (value28L == 1 && value28R == 0){
    //'messagesnt.mytype' REFERE-SE AO TIPO "ARRIVAL"
    messagesnt.mytype = 2;
    //'messagesnt.tipo' MUDA PARA 1 PARA AVISAR A THREAD VOO QUE HÁ PISTAS LIVRES
    messagesnt.tipo = 1;
    //O VALOR DO STATE INDICA A PISTA PARA ONDE ELE VAI ENTRAR
    state = 1;
    (void)strcpy(messagesnt.sms, "->    Pista 1");
    msgsnd(mqs_ct_to_thread, &messagesnt, sizeof(messagesnt), 0);
    pthread_mutex_unlock(&mutex_aterrar);
    sem_wait(&semid_28L);
    voo_aterra_shm->state = 1;
  }
  if (value28L == 0 && value28R == 1){
    //'messagesnt.mytype' REFERE-SE AO TIPO "ARRIVAL"
    messagesnt.mytype = 2;
    //'messagesnt.tipo' MUDA PARA 1 PARA AVISAR A THREAD VOO QUE HÁ PISTAS LIVRES
    messagesnt.tipo = 1;
    //O VALOR DO STATE INDICA A PISTA PARA ONDE ELE VAI ENTRAR
    state = 2;
    (void)strcpy(messagesnt.sms, "->    Pista 2");
    msgsnd(mqs_ct_to_thread, &messagesnt, sizeof(messagesnt), 0);
    pthread_mutex_unlock(&mutex_aterrar);
    sem_wait(&semid_28R);
    voo_aterra_shm->state = 1;
  }
  if (value28L == 0 && value28R == 0){
    //'messagesnt.mytype' REFERE-SE AO TIPO "ARRIVAL"
    messagesnt.mytype = 2;
    //'messagesnt.tipo' MUDA PARA 0 PARA AVISAR A THREAD VOO QUE NÃO HÁ PISTAS LIVRES
    messagesnt.tipo = 0;
    voo_aterra_shm->state = 1;
    //O VALOR DO ESTADO INDICA QUE ELE NÃO VAI ENTRAR NAS PISTAS
    estado = 0;
    (void)strcpy(messagesnt.sms, "->    Nenhuma pista disponível");
    msgsnd(mqs_ct_to_thread, &messagesnt, sizeof(messagesnt), 0);
    pthread_mutex_unlock(&mutex_aterrar);
  }

  //SHARED MEMORIES WAITING
  while(estado){

    //ENTRA NA PISTA 28L
    if (state == 1){
      signal(SIGINT, SIG_IGN);
      sprintf(mensagem, "%s ARRIVAL 28L started", nome_voo);
      estat_shm->total_voos_aterra++;
      //ESCREVE NO LOG.TXT
      pthread_mutex_unlock(&write_file);
      escreve_log(mensagem);
      pthread_mutex_lock(&write_file);
      pthread_mutex_lock(&mutex_aterrar);
      if ((rc = pthread_cond_timedwait(&cond, &mutex_aterrar, &t)) != 0){
        sprintf(mensagem, "%s ARRIVAL 28L conclued", nome_voo);
        //ESCREVE NO LOG.TXT
        pthread_mutex_unlock(&write_file);
        escreve_log(mensagem);
        pthread_mutex_lock(&write_file);
        estado = 0;
        voo_aterra_shm->state = 2;
        pthread_mutex_unlock(&mutex_aterrar);

        time(&T);
        t.tv_sec = (T + dados_config->int_aterragem);
        pthread_mutex_lock(&mutex_aterrar);
        if ((rc = pthread_cond_timedwait(&cond, &mutex_aterrar, &t)) != 0){
          sem_post(&semid_28L);
          voo_aterra_shm->state = 0;
        }
        pthread_mutex_unlock(&mutex_aterrar);
      }
      signal(SIGINT, cleanup);
    }

    //ENTRA NA PISTA 28R
    if (state == 2){
      signal(SIGINT, SIG_IGN);
      sprintf(mensagem, "%s ARRIVAL 28R started", nome_voo);
      estat_shm->total_voos_aterra++;
      //ESCREVE NO LOG.TXT
      pthread_mutex_unlock(&write_file);
      escreve_log(mensagem);
      pthread_mutex_lock(&write_file);
      pthread_mutex_lock(&mutex_aterrar);
      if ((rc = pthread_cond_timedwait(&cond, &mutex_aterrar, &t)) != 0){
        sprintf(mensagem, "%s ARRIVAL 28R conclued", nome_voo);
        //ESCREVE NO LOG.TXT
        pthread_mutex_unlock(&write_file);
        escreve_log(mensagem);
        pthread_mutex_lock(&write_file);
        estado = 0;
        voo_aterra_shm->state = 2;
        pthread_mutex_unlock(&mutex_aterrar);
        time(&T);
        t.tv_sec = T + dados_config->int_aterragem;
        pthread_mutex_lock(&mutex_aterrar);
        if ((rc = pthread_cond_timedwait(&cond, &mutex_aterrar, &t)) != 0){
          sem_post(&semid_28R);
          voo_aterra_shm->state = 0;
        }
        pthread_mutex_unlock(&mutex_aterrar);
      }
      signal(SIGINT, cleanup);
    }
  }
  pthread_exit(NULL);
}

void *cria_thread_pista_descola(void *arg){
  int state;
  int estado = 1;
  int rc;
  time_t T;
  char nome_voo[MAX];

  time(&T);
  t.tv_sec = T + dados_config->dur_descolagem;
  strcpy(nome_voo, rcmessage.ms_descola.nome);
  sem_getvalue(&semid_01L, &value01L);
  sem_getvalue(&semid_01R, &value01R);

  pthread_mutex_lock(&mutex_descolar);
  if (value01L == 1 && value01R == 1){
    //'messagesnt.mytype' REFERE-SE AO TIPO "DEPARTURE"
    messagesnt.mytype = 1;
    //O VALOR DO STATE INDICA A PISTA PARA ONDE ELE VAI ENTRAR
    state = 1;
    (void)strcpy(messagesnt.sms, "->    Pista 1");
    msgsnd(mqs_ct_to_thread, &messagesnt, sizeof(messagesnt), 0);
    pthread_mutex_unlock(&mutex_descolar);
    sem_wait(&semid_01L);
    voo_descola_shm->state = 1;
  }
  if (value01L == 1 && value01R == 0){
    //'messagesnt.mytype' REFERE-SE AO TIPO "DEPARTURE"
    messagesnt.mytype = 1;
    //O VALOR DO STATE INDICA A PISTA PARA ONDE ELE VAI ENTRAR
    state = 1;
    (void)strcpy(messagesnt.sms, "->    Pista 1");
    msgsnd(mqs_ct_to_thread, &messagesnt, sizeof(messagesnt), 0);
    pthread_mutex_unlock(&mutex_descolar);
    sem_wait(&semid_01L);
    voo_descola_shm->state = 1;
  }
  if (value01L == 0 && value01R == 1){
    //'messagesnt.mytype' REFERE-SE AO TIPO "DEPARTURE"
    messagesnt.mytype = 1;
    //O VALOR DO STATE INDICA A PISTA PARA ONDE ELE VAI ENTRAR
    state = 2;
    (void)strcpy(messagesnt.sms, "->    Pista 2");
    msgsnd(mqs_ct_to_thread, &messagesnt, sizeof(messagesnt), 0);
    pthread_mutex_unlock(&mutex_descolar);
    sem_wait(&semid_01R);
    voo_descola_shm->state = 1;
  }
  if (value01L == 0 && value01R == 0){
    //'messagesnt.mytype' REFERE-SE AO TIPO "DEPARTURE"
    messagesnt.mytype = 1;
    voo_descola_shm->state = 1;
    voo_descola_shm->muda = 1;
    //O VALOR DO ESTADO INDICA QUE NÃO HÁ PISTAS PARA ELE ENTRAR
    estado = 0;
    (void)strcpy(messagesnt.sms, "->    Nenhuma pista disponível");
    msgsnd(mqs_ct_to_thread, &messagesnt, sizeof(messagesnt), 0);
    pthread_mutex_unlock(&mutex_descolar);
  }

  //SHARED MEMORIES WAITING
  while(estado){

    //ENTRA NA PISTA 01L
    if (state == 1){
      signal(SIGINT, SIG_IGN);
      pthread_mutex_lock(&mutex_descolar);
      estat_shm->total_voos_descola++;
      sprintf(mensagem, "%s DEPARTURE 01L started", nome_voo);
      //ESCREVE NO LOG.TXT
      pthread_mutex_unlock(&write_file);
      escreve_log(mensagem);
      pthread_mutex_lock(&write_file);
      if ((rc = pthread_cond_timedwait(&cond, &mutex_descolar, &t)) != 0){
        sprintf(mensagem, "%s DEPARTURE 01L conclued", nome_voo);
        //ESCREVE NO LOG.TXT
        pthread_mutex_unlock(&write_file);
        escreve_log(mensagem);
        pthread_mutex_lock(&write_file);
        estado = 0;
        voo_descola_shm->state = 2;
        pthread_mutex_unlock(&mutex_descolar);
        time(&T);
        t.tv_sec = (T + dados_config->int_descolagem);
        pthread_mutex_lock(&mutex_descolar);
        if ((rc = pthread_cond_timedwait(&cond, &mutex_descolar, &t)) != 0){
          sem_post(&semid_01L);
          voo_descola_shm->state = 0;
        }
        pthread_mutex_unlock(&mutex_descolar);
      }
      signal(SIGINT, cleanup);
    }

    //ENTRA NA PISTA 01R
    if (state == 2){
      signal(SIGINT, SIG_IGN);
      pthread_mutex_lock(&mutex_descolar);
      estat_shm->total_voos_descola++;
      sprintf(mensagem, "%s DEPARTURE 01R started", nome_voo);
      //ESCREVE NO LOG.TXT
      pthread_mutex_unlock(&write_file);
      escreve_log(mensagem);
      pthread_mutex_lock(&write_file);
      if ((rc = pthread_cond_timedwait(&cond, &mutex_descolar, &t)) != 0){
        sprintf(mensagem, "%s DEPARTURE 01R conclued", nome_voo);
        //ESCREVE NO LOG.TXT
        pthread_mutex_unlock(&write_file);
        escreve_log(mensagem);
        pthread_mutex_lock(&write_file);
        estado = 0;
        voo_descola_shm->state = 2;
        pthread_mutex_unlock(&mutex_descolar);

        time(&T);
        t.tv_sec = (T + dados_config->int_descolagem);
        pthread_mutex_lock(&mutex_descolar);
        if ((rc = pthread_cond_timedwait(&cond, &mutex_descolar, &t)) != 0){
          sem_post(&semid_01R);
          voo_descola_shm->state = 0;
        }
        pthread_mutex_unlock(&mutex_descolar);
      }
      signal(SIGINT, cleanup);
    }


  }
  pthread_exit(NULL);
}

void cleanup(int sig){
    printf("\n");
    pthread_mutex_unlock(&mutex_ctrl_c);
    //ESPERA PELAS THREADS VOO ACABAREM
    for(int i = 0; i<estat_shm->total_voos;i++){
      pthread_join(thr_descola[i],NULL);
      pthread_join(thr_aterra[i],NULL);
      pthread_join(thr_pista_aterra[i], NULL);
      pthread_join(thr_pista_descola[i], NULL);
    }
    sprintf(mensagem, "END OF PROGRAM [ENDING RESOURCES...]");
    //ESCREVE NO LOG.TXT
    pthread_mutex_unlock(&write_file);
    escreve_log(mensagem);
    pthread_mutex_lock(&write_file);
    unlink(myfifo);
    //liberar memoria partilhada
    shmdt(&voo_aterra_shm);
    shmctl(shmid_aterra, IPC_RMID, NULL);
    shmdt(&voo_descola_shm);
    shmctl(shmid_descola, IPC_RMID, NULL);
    shmdt(&estat_shm);
    shmctl(shmid_estat, IPC_RMID, NULL);

    //LIBERAR OS SEMAFOROS
    sem_destroy(&semid_01L);
    sem_destroy(&semid_01R);
    sem_destroy(&semid_28L);
    sem_destroy(&semid_28R);

    // Message Queue
    msgctl(mqs_thread_to_ct, IPC_RMID, NULL);
    msgctl(mqs_ct_to_thread, IPC_RMID, NULL);
    //liberar mallocs
    free(dados_config);
    pthread_mutex_lock(&mutex_ctrl_c);
    //mutex
    pthread_mutex_destroy(&write_file);
    pthread_mutex_destroy(&mutex_ctrl_c);
    pthread_mutex_destroy(&mutex_aterrar);
    pthread_mutex_destroy(&mutex_descolar);
    //cond
    pthread_cond_destroy(&cond);
    //matar processos
    kill(Torre_Controlo, SIGKILL);
    kill(processo_manager, SIGKILL);
}

void inicia_estats(){
  estat_shm->total_voos = 0;
  estat_shm->total_voos_aterra = 0;
  estat_shm->total_voos_descola = 0;
  estat_shm->total_hold = 0;
  estat_shm->total_pocrl = 0;
  estat_shm->total_rejeitados = 0;
  estat_shm->total_temp_aterra = 0;
  estat_shm->total_temp_descola = 0;
  estat_shm->t_med_aterra = 0;
  estat_shm->t_med_descola = 0;
}

void estatisticas(){
  if (estat_shm->total_temp_aterra == 0 && estat_shm->total_voos_aterra == 0){
    estat_shm->t_med_aterra = 0;
  }
  else{
    estat_shm->t_med_aterra = (estat_shm->total_temp_aterra)/(estat_shm->total_voos_aterra);
  }

  if (estat_shm->total_temp_descola == 0 && estat_shm->total_voos_descola == 0){
    estat_shm->t_med_descola = 0;
  }
  else{
    estat_shm->t_med_descola = (estat_shm->total_temp_descola)/(estat_shm->total_voos_descola);
  }

  printf("\n\n\t\t[ ESTATÍSTICAS ]\n\n");
  printf("\tNúmero total de Voos: %d\n", estat_shm->total_voos);
  printf("\tNúmero total de Aterragens: %d\n", estat_shm->total_voos_aterra);
  printf("\tNúmero total de Descolagens: %d\n", estat_shm->total_voos_descola);
  printf("\tNúmero total de Holdings: %d\n", estat_shm->total_hold);
  printf("\tNúmero total de Voos Redirecionados: %d\n", estat_shm->total_pocrl);
  printf("\tNúmero total de Voos Rejeitados: %d\n", estat_shm->total_rejeitados);
  printf("\tTempo Médio de Aterragem: %.2lf\n", estat_shm->t_med_aterra);
  printf("\tTempo Médio de Descolagens: %.2lf\n", estat_shm->t_med_descola);
  printf("\n\n\t     [ FIM DE ESTATÍSTICAS ]\n\n\n");
}
