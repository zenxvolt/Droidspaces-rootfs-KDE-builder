/*
 * hal-audio-sim: makes the same ALSA calls as Waydroid's audio HAL
 * (android_hardware_waydroid/audio/audio_hw.c, start_output_stream and
 * out_write), to reproduce on a Linux machine how sound behaves on the phone.
 *
 * It opens the "pulse" ALSA device (retrying 100 times, 20 ms apart, like the
 * HAL), asks for 4 periods and a 1024-frame buffer at 48 kHz, prepares, and
 * then writes 1024 frames per call, which is what AudioFlinger hands the HAL.
 *
 *   cc -O2 -o hal-audio-sim hal-audio-sim.c -lasound -lm
 *   PULSE_RUNTIME_PATH=<dir with "native"> ./hal-audio-sim [SECONDS]
 *
 * A healthy run writes about SECONDS of audio in SECONDS. On 20 Nov 2026 it
 * stalled both against the bridge's PulseAudio server and against a real
 * PulseAudio 16 (enable-shm = no): see waydroid/PROGRESS.md.
 */
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char**argv){
  int seconds = argc>1?atoi(argv[1]):2; unsigned rate=48000;
  snd_pcm_t*pcm; snd_pcm_hw_params_t*hw; double t0=now();
  int tries=0, r;
  while ((r=snd_pcm_open(&pcm,"pulse",SND_PCM_STREAM_PLAYBACK,0))<0) { if(++tries==100){printf("open failed after 100 tries: %s\n",snd_strerror(r));return 1;} usleep(20000);}
  printf("open ok after %d retries, %.3fs\n",tries,now()-t0);
  snd_pcm_hw_params_alloca(&hw); snd_pcm_hw_params_any(pcm,hw);
  if(snd_pcm_hw_params_set_access(pcm,hw,SND_PCM_ACCESS_RW_INTERLEAVED)<0) puts("access fail");
  if(snd_pcm_hw_params_set_format(pcm,hw,SND_PCM_FORMAT_S16_LE)<0) puts("format fail");
  if(snd_pcm_hw_params_set_rate_near(pcm,hw,&rate,0)<0) puts("rate fail");
  if(snd_pcm_hw_params_set_channels(pcm,hw,2)<0) puts("channels fail");
  if(snd_pcm_hw_params_set_periods(pcm,hw,4,0)<0) puts("periods fail");
  if(snd_pcm_hw_params_set_buffer_size(pcm,hw,1024)<0) puts("buffer fail");
  if((r=snd_pcm_hw_params(pcm,hw))<0){printf("hw_params fail %s\n",snd_strerror(r));return 1;}
  if(snd_pcm_prepare(pcm)<0){puts("prepare fail");return 1;}
  static short buf[1024*2]; long frames=0; double ph=0, start=now(), worst=0; int eps=0;
  while (now()-start < seconds) {
    for(int i=0;i<1024;i++){short v=(short)(8000*sin(ph)); ph+=2*M_PI*440/rate; buf[2*i]=buf[2*i+1]=v;}
    double a=now(); r=snd_pcm_writei(pcm,buf,1024);
    if(r==-EPIPE){eps++; snd_pcm_prepare(pcm); r=snd_pcm_writei(pcm,buf,1024);}
    double d=now()-a; if(d>worst)worst=d;
    if(r>0) frames+=r; else {printf("write error %s\n",snd_strerror(r)); break;}
  }
  double el=now()-start;
  printf("wrote %ld frames (%.2fs of audio) in %.2fs, slowest write %.3fs, xruns %d\n",frames,frames/(double)rate,el,worst,eps);
  snd_pcm_close(pcm); return 0;
}
