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
#include <regex.h>
#include "EstruturasProj.h"

int main() {

  sprintf(mensagem, "PROGRAM INITIALIZED [STARTING RESOURCES...]");
  //ESCREVE NO LOG.TXT
  pthread_mutex_unlock(&write_file);
  escreve_log(mensagem);
  pthread_mutex_lock(&write_file);
  Dados_config dados_config = le_ficheiro();
  //INICIALIZA TODOS OS MECANISMO DE SINCRONIZAÇÃO
  cria_mem_aterra();
  cria_mem_descola();
  cria_mem_estat();
  cria_msq();
  cria_pipe();
  cria_sem_pistas();
  cria_headers();
  inicia_estats();
  //CRIAÇÃO DOS PROCESSOS
  if ((processo_manager = fork()) == 0){
    signal(SIGUSR1, estatisticas);
    int i = 0;
    Torre_Controlo = getpid();
    printf("->  PID CONTROL TOWER: %d <-\n", Torre_Controlo);
    while(1){
      //A TORRE DE CONTROLO ESPERA UMA MENSAGEM COMA INFORMAÇÃO DO TIPO DE VOO
      if ((msgrcv(mqs_thread_to_ct, &rcmessage, sizeof(rcmessage) + 1, 1, 0) != -1)) {
        if(rcmessage.voo == 1){
          //SE É DEPARTURE CRIA A THREAD PISTA DESCOLA
          pthread_create(&thr_pista_descola[i],NULL,cria_thread_pista_descola, rcmessage.ms_descola.nome);
        }
        else{
          //SE É ARRIVAL CRIA A THREAD PISTA ATERRA
          pthread_create(&thr_pista_aterra[i],NULL,cria_thread_pista_aterra, rcmessage.ms_aterra.nome);
        }
      }
      i++;
    }
  }
  else if (processo_manager > 0){
    signal(SIGINT, cleanup);
    valida_comando(dados_config);
  }
  else{
    perror("Erro no fork!\n");
  }
  return 0;
}
