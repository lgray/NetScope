/*
 * Author: Lindsey Gray (FNAL)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * convert_ots2hdf5 file nEventsPerChunk...
 */

/* waitpid on linux */
#include <sys/types.h>
#include <sys/wait.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>

#ifdef __linux /* on linux */
#include <pty.h>
#include <utmp.h>
#elif defined(__FreeBSD__)
#include <libutil.h>
#else /* defined(__APPLE__) && defined(__MACH__) */
#include <util.h>
#endif

#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>

#include "common.h"
#include "hdf5io.h"
#include "fifo.h"

#ifdef DEBUG
  #define debug_printf(fmt, ...) do { fprintf(stderr, fmt, ##__VA_ARGS__); fflush(stderr); \
                                    } while (0)
#else
  #define debug_printf(...) ((void)0)
#endif
#define error_printf(fmt, ...) do { fprintf(stderr, fmt, ##__VA_ARGS__); fflush(stderr); \
                                  } while(0)

#ifndef strlcpy
#define strlcpy(a, b, c) do { \
    strncpy(a, b, (c)-1); \
    (a)[(c)-1] = '\0'; \
} while (0)
#endif

static unsigned int chMask;
static size_t nCh;
static size_t nEvents;
static unsigned int doneReading;

static struct hdf5io_waveform_file *waveformFile;
static struct hdf5io_waveform_event waveformEvent;
static struct waveform_attribute waveformAttr;

#define FIFO_SIZE (512*1024*1024)
static struct fifo_t *fifo;

#define MAXSLEEP 2

static void extract_waveform_attribute(char buf[BUFSIZ], struct waveform_attribute *wavAttr)
/* fills wavAttr as well */
{    
  sscanf(buf, 
	 "waveform_attribute:"
	 "     chMask  = 0x%02x"
	 "     nPt     = %zd"
	 "     nFrames = %zd"
	 "     dt      = %g"
	 "     t0      = %g"
	 "     ymult   = %g %g %g %g"
	 "     yoff    = %g %g %g %g"
	 "     yzero   = %g %g %g %g",
	 &wavAttr->chMask, &wavAttr->nPt, &wavAttr->nFrames, &wavAttr->dt, &wavAttr->t0,
	 &wavAttr->ymult[0], &wavAttr->ymult[1], &wavAttr->ymult[2], &wavAttr->ymult[3],
	 &wavAttr->yoff[0], &wavAttr->yoff[1], &wavAttr->yoff[2], &wavAttr->yoff[3],
	 &wavAttr->yzero[0], &wavAttr->yzero[1], &wavAttr->yzero[2], &wavAttr->yzero[3]);
  
  printf("waveform_attribute:\n"
	 "     chMask  = 0x%02x\n"
	 "     nPt     = %zd\n"
	 "     nFrames = %zd\n"
	 "     dt      = %g\n"
	 "     t0      = %g\n"
	 "     ymult   = %g %g %g %g\n"
	 "     yoff    = %g %g %g %g\n"
	 "     yzero   = %g %g %g %g\n",
	 wavAttr->chMask, wavAttr->nPt, wavAttr->nFrames, wavAttr->dt, wavAttr->t0,
	 wavAttr->ymult[0], wavAttr->ymult[1], wavAttr->ymult[2], wavAttr->ymult[3],
	 wavAttr->yoff[0], wavAttr->yoff[1], wavAttr->yoff[2], wavAttr->yoff[3],
	 wavAttr->yzero[0], wavAttr->yzero[1], wavAttr->yzero[2], wavAttr->yzero[3]);    
}

static void atexit_flush_files(void)
{
    hdf5io_flush_file(waveformFile);
    hdf5io_close_file(waveformFile);
}

static void signal_kill_handler(int sig)
{
    printf("\nstop time  = %zd\n", time(NULL));
    fflush(stdout);
    
    error_printf("Killed, cleaning up...\n");
    atexit(atexit_flush_files);
    exit(EXIT_SUCCESS);
}

static size_t raw_event_size(struct hdf5io_waveform_file *wavFile)
{
    char buf[BUFSIZ];
    size_t chHeaderSize;
    
    chHeaderSize = snprintf(buf, sizeof(buf), "#X%zd", wavFile->nPt);
    return (chHeaderSize + wavFile->nPt) * wavFile->nCh + 1;
}

// this just reads the file as fast as it can and sticks it into the fifo for decoding
static void *receive_and_push(FILE *infile)
{        
  
  doneReading = 0;

  char ibuf[16*BUFSIZ];
  ssize_t nr;
  
  while(!feof(infile)) {    
    nr = fread(ibuf, sizeof(char), BUFSIZ, infile);
    fifo_push(fifo, ibuf, nr);    
  }
  
  doneReading = 1;
  
  return (void*)NULL;
}

static void *pop_and_save(void *arg)
{

    int fStartEvent, fEndEvent, fStartCh, fGetNDig, fGetRetChLen; /* flags of states */
    size_t nDig=0, retChLen, iCh, iRetChLen=0, i, j, wavBufN;
    char ibuf[BUFSIZ], retChLenBuf[BUFSIZ];
    size_t nr;
    size_t iEvent = 0;
    char *wavBuf;
/*
    FILE *fp;
    if((fp=fopen("log1.txt", "w"))==NULL) {
        perror("log1.txt");
        return (void*)NULL;
    }
*/
    wavBufN = waveformAttr.nPt * nCh;
    wavBuf = (char*)malloc(wavBufN * sizeof(char));

    iCh = 0; j = 0;
    fStartEvent = 1; fEndEvent = 0; fStartCh = 0; fGetNDig = 0; fGetRetChLen = 0;
    unsigned int fifo_empty = 0;
    for(;;) {
        nr = fifo_pop(fifo, ibuf, sizeof(ibuf));
        //printf("%zd popped.\n", nr);
        //fflush(stdout);
//        write(fileno(fp), ibuf, nr);

        if(nr == 0) {
	  break; /* there will be nothing from the fifo any more */
	  fifo_empty = 1;
	}
        for(i=0; i<nr; i++) {	  
            if(fStartEvent) {
                printf("iEvent = %zd, ", iEvent);
                iCh = 0;
                j = 0;
                fStartEvent = 0;
                fStartCh = 1;
                i--; continue;
            } else if(fEndEvent) {
                if(ibuf[i] == ';') { /* ';' only appears in curvestream? mode */
                    continue;
                } else if(ibuf[i] == '\n') {
		  //printf("ENDEVENT iEvent = %zd\n", iEvent);
                    printf("\n");
                    fflush(stdout);
                    fEndEvent = 0;
                    fStartEvent = 1;

                    /* Will trigger next event query.  Requesting next
                     * event before writing the current event to file
                     * may boost data rate a bit */
                    // iEventIncLocked();
                    iEvent++;

                    waveformEvent.wavBuf = wavBuf;
                    waveformEvent.eventId = iEvent-1;
                    hdf5io_write_event(waveformFile, &waveformEvent);

		    //printf("%u %u", doneReading, fifo_empty);
		    
                    if(doneReading == 1 && nr < BUFSIZ) {
                        printf("\n");
                        goto end;
                    }
                }
            } else {
                if(fStartCh) {
                    if(ibuf[i] == '#') {
                        fGetNDig = 1;
                        continue;
                    } else if(fGetNDig) {
                        retChLenBuf[0] = ibuf[i];
                        retChLenBuf[1] = '\0';
                        nDig = atol(retChLenBuf);
                        printf("nDig = %zd, ", nDig);
                        
                        iRetChLen = 0;
                        fGetNDig = 0;
                        continue;
                    } else {
                        retChLenBuf[iRetChLen] = ibuf[i];
                        iRetChLen++;
                        if(iRetChLen >= nDig) {
                            retChLenBuf[iRetChLen] = '\0';
                            retChLen = atol(retChLenBuf);
                            printf("iRetChLen = %zd, retChLen = %zd, ",
                                   iRetChLen, retChLen);
                            fStartCh = 0;
                            continue;
                        }
                    }
                } else {
                    wavBuf[j] = ibuf[i];
                    j++;
                    if((j % waveformAttr.nPt) == 0 && (j!=0)) {
                        printf("iCh = %zd, ", iCh);
                        iCh++;
                        fStartCh = 1;
                        if(iCh >= nCh) {
                            fEndEvent = 1;
                        }
                    }
                }
            }
        }
    }
end:
    fflush(stdout);
    

    free(wavBuf);
//    fclose(fp);
    return (void*)NULL;
}

int main(int argc, char **argv)
{
  char *inFileName, *outFileName;    
  time_t startTime, stopTime;
  FILE* infile;
  pthread_t wTid;
  size_t nWfmPerChunk = 100;
  
  if(argc<3) {
    error_printf("%s inFile outFile nWfmPerChunk\n",
		 argv[0]);
    return EXIT_FAILURE;
  }
  inFileName = argv[1];
  outFileName = argv[2];
  
  if(argc>=4)
    nWfmPerChunk = atol(argv[3]);
  
  infile = fopen(inFileName,"r");
  if(infile == NULL) {
    error_printf("Failed to open %s.\n",inFileName);
    return EXIT_FAILURE;
  }
  
  
  
  // get the file header and setup the waveform attribute
  char headerbuf[BUFSIZ];
  fgets(headerbuf, BUFSIZ, infile);
  
  //printf("%s",headerbuf);
  
  extract_waveform_attribute(headerbuf,&waveformAttr);

  nEvents = 1000000;
  nCh = 0;
  chMask = waveformAttr.chMask;
  unsigned temp = 0;
  for( temp = 0; temp < 4; ++temp ) {
    nCh += (chMask >> temp)&0x1;
  }

  debug_printf("outFileName: %s, chMask: 0x%02x, nCh: %zd, nEvents: %zd, nWfmPerChunk: %zd\n",
	       outFileName, chMask, nCh, nEvents, nWfmPerChunk);
  
  fifo = fifo_init(FIFO_SIZE);
  waveformFile = hdf5io_open_file(outFileName, nWfmPerChunk, nCh);
  
  hdf5io_write_waveform_attribute_in_file_header(waveformFile, &waveformAttr);
  
  signal(SIGKILL, signal_kill_handler);
  signal(SIGINT, signal_kill_handler);
  
  pthread_create(&wTid, NULL, pop_and_save, &infile);
  
  printf("start time = %zd\n", startTime = time(NULL));
  
  receive_and_push(infile);
  
  stopTime = time(NULL);
  pthread_join(wTid, NULL);
  
  printf("\nstart time = %zd\n", startTime);
  printf("stop time  = %zd\n", stopTime);
  
  fifo_close(fifo);
  fclose(infile);
  atexit_flush_files();
  return EXIT_SUCCESS;
}
