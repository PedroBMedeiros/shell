#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include "./mush.h"
#include <stdbool.h>

#define PROMPT "8-P "

void SIGINThandler(int signum);
 
int main(int argc, char *argv[]) {
  
  char *longline; /* holds current read line */
  FILE *filein; /* file in */
  pipeline pline; /* line of pipes*/
  int i, j, fdin, fdout;
  int (* pipefds)[2]; /* fds for pipes */
  pid_t * child_pids; /* pid of children */
  int status; /* status of children processes */
  char* homePath; /* holds path to home directory */
  bool childrenInterrupted; /* check if children have been interrupted*/
  
  /* signal handling */
  struct sigaction sa;
  sa.sa_handler = SIGINThandler;
  sigset_t previousMask;
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGINT);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  /* -------------------- */


  /* command line parsing*/
  if (argc == 1) {
    /* 0 arguments =>  prompt and use stdin*/
    filein = stdin;
  } else if (argc == 2) {
    /* 1 argument => open file with commands to process*/
    if( (filein = fopen(argv[1], "r")) ==  NULL) {
      perror("error opening input file");
      exit(1);
    }
  } else {
    /* 2+ arguments => kick out*/
    fprintf(stderr,"usage: mush <input file>\n");
    exit(1);
  }
  /* PRINT PROMPT - "8-P " */
  if (isatty(fileno(filein)) && isatty(fileno(stdout))) {
    printf(PROMPT);
    fflush(stdout);
  }

  /* parse and process line by line of received commands*/
  while (!feof(filein)) {
    /* read one line at a time*/
    if ((longline = readLongString(filein)) == NULL) {
      if (feof(filein)) {
        continue; /* to take care of ^D; gracefully continue  */
      } else if (errno == EINTR) { /* to handle interrupted system call*/
        continue;
      } 
      else {
        perror("problem with readLongString"); /* exitable problem */
        exit(1);
      }
    }
    
    /* parse line into pipeline of commands */
    if ((pline = crack_pipeline(longline)) == NULL) {
      /* error message comes from library */
      if (isatty(fileno(filein)) && isatty(fileno(stdout))) {
        printf(PROMPT);
        fflush(stdout);
      }
      continue;
    }
    //print_pipeline(fileout, pline); /* for visualization only */
    /* after parsing use yylex_destroy to help wtih Valgrind tests*/
    yylex_destroy();
    
    /* before doing anything, test if pipeline starts with cd as it needs to 
                                                be executed inside parent */
    if (strcmp(pline->stage[0].argv[0], "cd") == 0) {
      switch (pline->stage[0].argc)
      {
      case 1: /* no directory path provided */
        if ((homePath = getenv("HOME")) == NULL) {
          if ((homePath = getpwuid(getuid())->pw_dir) == NULL) {
            fprintf(stderr, "unable to determine home directory");
            exit(1);
          }
        }
        if (chdir(homePath) == -1) {
          perror("change dir to home failure");
          exit(1);
        }
        break;

      case 2: /* directory path provided */
        if (chdir(pline->stage[0].argv[1]) == -1) {
          perror(pline->stage[0].argv[1]);
        }
        break;
      
      default:
        fprintf(stderr, "usage: cd <directory>\n");
        break;
      }
      /* print PROMPT only when in tty mode */
      if (isatty(fileno(filein)) && isatty(fileno(stdout))) {
        printf(PROMPT);
        fflush(stdout);
      }
      continue;
    }

    /* create pipes */
    if ((pipefds = calloc(pline->length - 1, 2*sizeof(int))) == NULL) {
      perror("calloc pipe fd error");
      exit(1);
    }
    for (i = 0; i < (pline->length - 1); i++) {
      if (pipe(pipefds[i]) == -1) {
        perror("problem creating pipes");
        exit(1);
      }
    }

    /* child1 | pipe1 | child2 | pipe2 | child3 |... pipeN-1 | childN */
    /* child count = length of pipeline */
    /* pipe count = length of pipeline -1 */
    
    /* but block SIGINT before creating children */
    if (sigprocmask(SIG_BLOCK, &sa.sa_mask, &previousMask) == -1) {
      perror("sigprocmask error");
      exit(1);
    }

    /* create children processes*/
    if ((child_pids = calloc(pline->length, sizeof(pid_t))) == NULL) {
      perror("calloc children pid error");
      exit(1);
    }
    for (i = 0; i < pline->length; i++) {
      if ((child_pids[i] = fork()) == -1) { /* fork each child */
        perror("fork error");
        exit(1);
      } 
      if (child_pids[i] == 0) { /* are we children? */
        /* unblock the SIGINT signal. */
        if (sigprocmask(SIG_UNBLOCK, &sa.sa_mask, &previousMask) == -1) {
          perror("sigprocmask error");
          exit(1);
        }
        /* connect child to pipes using dup2*/
        /* test for special cases begin and end of pipeline*/
        if (i == 0) { /* first child */
          if (pline->length > 1) {
            if (dup2(pipefds[0][1], STDOUT_FILENO) == -1) {
              perror("dupping error");
              exit(1);
            }
          }
          if (pline->stage[0].inname != NULL) {
            /* only in case of input redirection */
            if ((fdin = open(pline->stage[0].inname, O_RDONLY, 0644)) == -1) {
              perror(pline->stage[0].inname);
              exit(errno);
            }
            if (dup2(fdin, STDIN_FILENO) == -1) {
              perror("dupping error");
              exit(1);
            }
          }
        }
        if (i == (pline->length - 1)) { /* last child */
          if (pline->length > 1) {
            if (dup2(pipefds[i-1][0], STDIN_FILENO) == -1) {
              perror("dupping error");
              exit(1);
            }
          }
          if (pline->stage[i].outname != NULL) { 
            /* only in case of output redirection */
            if ((fdout = open(pline->stage[i].outname, O_WRONLY | O_CREAT | 
                                                      O_TRUNC, 0644)) == -1) {
              perror(pline->stage[i].outname);
              exit(errno);
            }
            if (dup2(fdout, STDOUT_FILENO) == -1) {
              perror("dupping error");
              exit(1);
            }
          }
        }
        if (i > 0 && i < (pline->length -1)) { /* middle child */
          if (dup2(pipefds[i][1], STDOUT_FILENO) == -1) {
            perror("dupping error");
            exit(1);
          }
          if (dup2(pipefds[i-1][0], STDIN_FILENO) == -1) {
            perror("dupping error");
            exit(1);
          }
        }

        /* close all UNUSED pipes of each i child*/
        for (j = 0; j < (pline->length - 1); j++) {
          if (j == (i-1)) {
            if (close(pipefds[j][1]) == -1) {
              perror("fail to close");
              exit(1);
            }
          } else if (j == i) {
            if (close(pipefds[j][0]) == -1) {
              perror("fail to close");
              exit(1);
            }
          } else {
            if (close(pipefds[j][1]) == -1) {
              perror("fail to close");
              exit(1);
            }
            if (close(pipefds[j][0]) == -1) {
              perror("fail to close");
              exit(1);
            }
          }
        }
        /* run command at last using execvp*/
        execvp(pline->stage[i].argv[0], pline->stage[i].argv);
        /* we should never get here unless there was a problem with execvp*/
        perror(pline->stage[i].argv[0]);
        exit(errno);
      }  
    }
       
    /* if we got here still parent*/
    /* unblock SIGINT */ 
    if (sigprocmask(SIG_UNBLOCK, &sa.sa_mask, NULL) == -1) {
      perror("sigprocmask error");
      exit(1);
    }

    /* close all pipes inside parent after children are spawn */
    for (i = 0; i < (pline->length -1); i++) {
      if (close(pipefds[i][1]) == -1) {
        perror("fail to close");
        exit(1);
      }
      if (close(pipefds[i][0]) == -1) {
        perror("fail to close");
        exit(1);
      }
    }
    /* wait for children processes to end*/
    errno = 0;
    childrenInterrupted = false;
    for (i = 0; i < (pline->length); i++) {
      waitpid(child_pids[i], &status, 0);
      if (errno == EINTR) {
        //printf("children interrupted"); /* visualization only */
        childrenInterrupted = true; 
      }
      // if(status != 0) {
      //   printf("\n");
      // }
    }
    /* double check we are in tty mode before printing prompt again*/
    if (isatty(fileno(filein)) && isatty(fileno(stdout)) && 
                                                  !childrenInterrupted) {
      printf(PROMPT);
      fflush(stdout);
    }

    /* clean up memory*/
    free(child_pids);
    free(pipefds);
    
  }
  /* only print extra newline at the end if in tty mode*/
  if (isatty(fileno(filein)) && isatty(fileno(stdout))) {
    printf("\n"); 
  }
  return 0;
}

/* handler for SIGINT signal (^C)*/
void SIGINThandler(int signum) {
  //fprintf(stderr, "\ncaught sigint signal\n");
  if (isatty(fileno(stdin)) && isatty(fileno(stdout))) {
    printf("\n");
    printf(PROMPT);
    fflush(stdout);
  }
  return;
}
