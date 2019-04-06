
/**************************************************************************
 * simpletun.c                                                            *
 *                                                                        *
 * A simplistic, simple-minded, naive tunnelling program using tun/tap    *
 * interfaces and TCP. DO NOT USE THIS PROGRAM FOR SERIOUS PURPOSES.      *
 *                                                                        *
 * You have been warned.                                                  *
 *                                                                        *
 * (C) 2010 Davide Brini.                                                 *
 *                                                                        *
 * DISCLAIMER AND WARNING: this is all work in progress. The code is      *
 * ugly, the algorithms are naive, error checking and input validation    *
 * are very basic, and of course there can be bugs. If that's not enough, *
 * the program has not been thoroughly tested, so it might even fail at   *
 * the few simple things it should be supposed to do right.               *
 * Needless to say, I take no responsibility whatsoever for what the      *
 * program might do. The program has been written mostly for learning     *
 * purposes, and can be used in the hope that is useful, but everything   *
 * is to be taken "as is" and without any kind of warranty, implicit or   *
 * explicit. See the file LICENSE for further details.                    *
 *************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <zlib.h>

/* buffer for reading from tun/tap interface, must be >= 1500 */
#define BUFSIZE 2000

/**************************************************************************
 * tun_alloc: allocates or reconnects to a tun/tap device. The caller     *
 *            must reserve enough space in *dev.                          *
 **************************************************************************/
int tun_alloc(char *dev, int flags) {

  struct ifreq ifr;
  int fd, err;
  char *clonedev = "/dev/net/tun";

  if( (fd = open(clonedev , O_RDWR)) < 0 ) {
    perror("Opening /dev/net/tun");
    return fd;
  }

  memset(&ifr, 0, sizeof(ifr));

  ifr.ifr_flags = flags;

  if (*dev) {
    strncpy(ifr.ifr_name, dev, IFNAMSIZ);
  }

  if( (err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0 ) {
    perror("ioctl(TUNSETIFF)");
    close(fd);
    return err;
  }

  strcpy(dev, ifr.ifr_name);

  return fd;
}

snd_pcm_t *open_playback(){
  snd_pcm_t *handle;
  snd_pcm_hw_params_t *params;
  unsigned int val;
  int dir;
  int rc = snd_pcm_open(&handle, "default",
          SND_PCM_STREAM_PLAYBACK, 0);
  if (rc < 0) {
      fprintf(stderr,
              "unable to open pcm device: %s\n",
              snd_strerror(rc));
      exit(1);
  }

  /* Allocate a hardware parameters object. */
  snd_pcm_hw_params_alloca(&params);

  /* Fill it in with default values. */
  snd_pcm_hw_params_any(handle, params);

  /* Set the desired hardware parameters. */

  /* Interleaved mode */
  snd_pcm_hw_params_set_access(handle, params,
                      SND_PCM_ACCESS_RW_INTERLEAVED);

  /* Signed 16-bit little-endian format */
  snd_pcm_hw_params_set_format(handle, params,
                              SND_PCM_FORMAT_S16_LE);

  snd_pcm_hw_params_set_channels(handle, params, 1);

  /* 44100 bits/second sampling rate (CD quality) */
  val = 44100;
  snd_pcm_hw_params_set_rate_near(handle,
                                 params, &val, &dir);

  /* Write the parameters to the driver */
  rc = snd_pcm_hw_params(handle, params);
  return handle;
}

unsigned int samp_rate = 44100;
snd_pcm_uframes_t period = 128;
void setup_playback(snd_pcm_t *handle){
  int dir = 1;
  int rc;
  snd_pcm_hw_params_t *params;  
  /* Allocate a hardware parameters object. */
  snd_pcm_hw_params_alloca(&params);

  /* Fill it in with default values. */
  snd_pcm_hw_params_any(handle, params);

  /* Set the desired hardware parameters. */

  /* Interleaved mode */
  snd_pcm_hw_params_set_access(handle, params,
                      SND_PCM_ACCESS_RW_INTERLEAVED);

  /* Signed 16-bit little-endian format */
  snd_pcm_hw_params_set_format(handle, params,
                              SND_PCM_FORMAT_S16_LE);

  snd_pcm_hw_params_set_channels(handle, params, 1);

  /* 44100 bits/second sampling rate (CD quality) */
  snd_pcm_hw_params_set_rate_near(handle,
                                 params, &samp_rate, &dir);

  snd_pcm_hw_params_set_period_size(handle, params, period, dir);

  /* Write the parameters to the driver */
  rc = snd_pcm_hw_params(handle, params);
  if(rc < 0) {
    fprintf(stderr, "audio params set failed %s", snd_strerror(rc));
    exit(1);
  }
  snd_pcm_hw_params_get_rate(params, &samp_rate, &dir);
  snd_pcm_hw_params_get_period_size(params, &period, &dir);
}

int tx_state = 0;
char tx_buf[1+2+BUFSIZE+4];
const unsigned int samp_per_bit = 20;
_Noreturn void *sound_thread(void *ptr){
   snd_pcm_t * const handle = (snd_pcm_t*) ptr;
   const double w = 2 * M_PI * 1000 / samp_rate;
   short *pcm_buf = calloc(period, sizeof(short));
   int packet_t = 0;
   int pcm_size = 0;
   int period_end = 0;
   uint16_t packet_len = 0;
   while(1) {
      switch(tx_state) {
        case 0: //ready
          memset(pcm_buf, 0, period*sizeof(short));
          break;
        case 1: //waiting to tx
          printf("packet to tx\n");
          tx_state = 2;
          packet_t = 0;
		  union{
			  uint16_t val;
			  char raw[2];
		  } lengthField;
		  memcpy(lengthField.raw, &tx_buf[1], 2);
		  packet_len = 7+(ntohs(lengthField.val));
          pcm_size = samp_per_bit * 8 * packet_len;
        case 2: //txing
          period_end = packet_t + period;
          printf("synth start=%d end=%d ...", packet_t, period_end);
          for(; packet_t<period_end && packet_t < pcm_size; packet_t++){
             int bit_ix = packet_t/samp_per_bit;
             char bit = (tx_buf[bit_ix/8]>>(bit_ix%8))&1;
             if(bit) 
                 pcm_buf[packet_t%period] = 32000 * sin(w*packet_t);
             else
                 pcm_buf[packet_t%period] = -32000 * sin(w*packet_t);
          }
          if(packet_t < period_end) {
            printf("last period ");
            memset(&pcm_buf[packet_t%period], 0, (period_end - packet_t)*sizeof(short));
            tx_state = 0;
          }
          printf("end\n");
          break;
      }
      snd_pcm_writei(handle, pcm_buf, period);
   }
}

/**************************************************************************
 * cread: read routine that checks for errors and exits if an error is    *
 *        returned.                                                       *
 **************************************************************************/
int cread(int fd, char *buf, int n){
  
  int nread;

  if((nread=read(fd, buf, n)) < 0){
    perror("Reading data");
    exit(1);
  }
  return nread;
}

/**************************************************************************
 * cwrite: write routine that checks for errors and exits if an error is  *
 *         returned.                                                      *
 **************************************************************************/
int cwrite(int fd, char *buf, int n){
  
  int nwrite;

  if((nwrite=write(fd, buf, n)) < 0){
    perror("Writing data");
    exit(1);
  }
  return nwrite;
}

_Noreturn int main(int argc, char *argv[]) {
  int tap_fd;
  int flags = IFF_TUN;
  char if_name[IFNAMSIZ] = "";
  uint16_t nread;
  char buffer[BUFSIZE];
  snd_pcm_t *pcm_handle;
  pthread_t snd_thread_id;
  
  pcm_handle = open_playback();
  setup_playback(pcm_handle);
  pthread_create(&snd_thread_id, NULL, sound_thread, (void*)pcm_handle);

  /* initialize tun/tap interface */
  if ( (tap_fd = tun_alloc(if_name, flags | IFF_NO_PI)) < 0 ) {
    fprintf(stderr, "Error connecting to tun/tap interface %s!\n", if_name);
    exit(1);
  }
  printf("listening on %s\n", if_name);
  
  while(1) {
      switch(tx_state) {
        case 0: //ready
		  tx_buf[0]=0xf5;		//Start of Frame byte, easy to demod
          nread = cread(tap_fd, &tx_buf[3], BUFSIZE);
          printf("got packet len %d\n", nread);
		  
		  union{
			  uint16_t val;
			  char raw[2];
		  } lengthField;
		  lengthField.val=htons(nread);
		  memcpy(&tx_buf[1], lengthField.raw, 2);
		  
		  union{
			  uint32_t crc;
			  char raw[4];
		  } crcField;
		  crcField.crc=htonl(crc32(0L, &tx_buf[3], nread));
		  memcpy(&tx_buf[nread+3], crcField.raw, 4);
          
          tx_state = 1;
          break;
        case 1: //waiting to tx
          usleep(1000);
          break;
        case 2: //txing
          usleep(1000);
          break;
      }
  }

  return(0);
}
