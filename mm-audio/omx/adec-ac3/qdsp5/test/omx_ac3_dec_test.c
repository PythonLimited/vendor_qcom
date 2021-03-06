/*
 * Copyright (c) 2012 Qualcomm Technologies, Inc.  All Rights Reserved.
 * Qualcomm Technologies Proprietary and Confidential.
 */


/*
    An Open max test application ....
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/ioctl.h>
#include "OMX_Core.h"
#include "OMX_Component.h"
#include "QOMX_AudioExtensions.h"
#include "QOMX_AudioIndexExtensions.h"
#include "pthread.h"
#include <signal.h>

#include <stdint.h>
#include <linux/ioctl.h>
#include <linux/msm_audio.h>
#define SAMPLE_RATE 48000
#define STEREO      2
#define OMX_ADEC_DEFAULT_PCM_SCALE_FACTOR 100
#define OMX_ADEC_DEFAULT_DYNAMIC_SCALE_BOOST 100
#define OMX_ADEC_DEFAULT_DYNAMIC_SCALE_CUT 100

uint32_t samplerate = 48000;
uint32_t channels = 2;
uint32_t pcmplayback = 0;
uint32_t tunnel      = 0;
uint32_t filewrite   = 0;
uint32_t dual_mono   = 0;
uint32_t stereo_mode = 0;
uint32_t karaoke_mode = 0;
#define DEBUG_PRINT printf
uint32_t flushinprogress = 0;
int start_done = 0;

#define PCM_PLAYBACK /* To write the pcm decoded data to the msm_pcm device for playback*/

  int                          m_pcmdrv_fd;

/************************************************************************/
/*                #DEFINES                            */
/************************************************************************/
#define false 0
#define true 1

#define CONFIG_VERSION_SIZE(param) \
    param.nVersion.nVersion = CURRENT_OMX_SPEC_VERSION;\
    param.nSize = sizeof(param);

#define FAILED(result) (result != OMX_ErrorNone)

#define SUCCEEDED(result) (result == OMX_ErrorNone)

/************************************************************************/
/*                GLOBAL DECLARATIONS                     */
/************************************************************************/

pthread_mutex_t lock;
pthread_cond_t cond;
pthread_mutex_t elock;
pthread_cond_t econd;
pthread_mutex_t lock1;
pthread_mutexattr_t lock1_attr;
pthread_cond_t fcond;
pthread_mutex_t etb_lock;
pthread_mutex_t etb_lock1;
pthread_cond_t etb_cond;
pthread_mutexattr_t etb_lock_attr;
FILE * inputBufferFile;
FILE * outputBufferFile;
OMX_PARAM_PORTDEFINITIONTYPE inputportFmt;
OMX_PARAM_PORTDEFINITIONTYPE outputportFmt;
QOMX_AUDIO_PARAM_AC3TYPE ac3param;
QOMX_AUDIO_PARAM_AC3PP ac3PP;
OMX_PORT_PARAM_TYPE portParam;
OMX_ERRORTYPE error;
int bReconfigureOutputPort = 0;


#define ID_RIFF 0x46464952
#define ID_WAVE 0x45564157
#define ID_FMT  0x20746d66
#define ID_DATA 0x61746164

#define FORMAT_PCM 1

static int bFileclose = 0;
//typedef unsigned int uint32_t;
//typedef unsigned short int uint16_t;

struct wav_header {
  uint32_t riff_id;
  uint32_t riff_sz;
  uint32_t riff_fmt;
  uint32_t fmt_id;
  uint32_t fmt_sz;
  uint16_t audio_format;
  uint16_t num_channels;
  uint32_t sample_rate;
  uint32_t byte_rate;       /* sample_rate * num_channels * bps / 8 */
  uint16_t block_align;     /* num_channels * bps / 8 */
  uint16_t bits_per_sample;
  uint32_t data_id;
  uint32_t data_sz;
};

static unsigned totaldatalen = 0;
/************************************************************************/
/*                GLOBAL INIT                    */
/************************************************************************/

int input_buf_cnt = 0;
int output_buf_cnt = 0;
int used_ip_buf_cnt = 0;
volatile int event_is_done = 0;
volatile int ebd_event_is_done = 0;
volatile int fbd_event_is_done = 0;
volatile int etb_event_is_done = 0;
int ebd_cnt;
int bOutputEosReached = 0;
int bInputEosReached = 0;
int bEosOnInputBuf = 0;
int bEosOnOutputBuf = 0;
static int etb_done = 0;
int bFlushing = false;
int bPause    = false;
const char *in_filename;
const char out_filename[512];
OMX_U8* pBuffer_tmp = NULL;

//* OMX Spec Version supported by the wrappers. Version = 1.1 */
const OMX_U32 CURRENT_OMX_SPEC_VERSION = 0x00000101;
OMX_COMPONENTTYPE* ac3_dec_handle = 0;

OMX_BUFFERHEADERTYPE  **pInputBufHdrs = NULL;
OMX_BUFFERHEADERTYPE  **pOutputBufHdrs = NULL;

/************************************************************************/
/*                GLOBAL FUNC DECL                        */
/************************************************************************/
int Init_Decoder(char*);
int Play_Decoder();
OMX_STRING aud_comp;

/**************************************************************************/
/*                STATIC DECLARATIONS                       */
/**************************************************************************/

static int open_audio_file ();
static int Read_Buffer(OMX_BUFFERHEADERTYPE  *pBufHdr );
static OMX_ERRORTYPE Allocate_Buffer ( OMX_COMPONENTTYPE *ac3_dec_handle,
                                       OMX_BUFFERHEADERTYPE  ***pBufHdrs,
                                       OMX_U32 nPortIndex,
                                       long bufCntMin, long bufSize);


static OMX_ERRORTYPE EventHandler(OMX_IN OMX_HANDLETYPE hComponent,
                                  OMX_IN OMX_PTR pAppData,
                                  OMX_IN OMX_EVENTTYPE eEvent,
                                  OMX_IN OMX_U32 nData1, OMX_IN OMX_U32 nData2,
                                  OMX_IN OMX_PTR pEventData);
static OMX_ERRORTYPE EmptyBufferDone(OMX_IN OMX_HANDLETYPE hComponent,
                                     OMX_IN OMX_PTR pAppData,
                                     OMX_IN OMX_BUFFERHEADERTYPE* pBuffer);

static OMX_ERRORTYPE FillBufferDone(OMX_IN OMX_HANDLETYPE hComponent,
                                     OMX_IN OMX_PTR pAppData,
                                     OMX_IN OMX_BUFFERHEADERTYPE* pBuffer);
void wait_for_event(void)
{
    pthread_mutex_lock(&lock);
    DEBUG_PRINT("%s: event_is_done=%d", __FUNCTION__, event_is_done);
    while (event_is_done == 0) {
        pthread_cond_wait(&cond, &lock);
    }
    event_is_done = 0;
    pthread_mutex_unlock(&lock);
}

void event_complete(void )
{
    pthread_mutex_lock(&lock);
    if (event_is_done == 0) {
        event_is_done = 1;
        pthread_cond_broadcast(&cond);
    }
    pthread_mutex_unlock(&lock);
}

void etb_wait_for_event(void)
{
    pthread_mutex_lock(&etb_lock1);
    DEBUG_PRINT("%s: etb_event_is_done=%d", __FUNCTION__, etb_event_is_done);
    while (etb_event_is_done == 0) {
        pthread_cond_wait(&etb_cond, &etb_lock1);
    }
    etb_event_is_done = 0;
    pthread_mutex_unlock(&etb_lock1);
}

void etb_event_complete(void )
{
    pthread_mutex_lock(&etb_lock1);
    if (etb_event_is_done == 0) {
        etb_event_is_done = 1;
        pthread_cond_broadcast(&etb_cond);
    }
    pthread_mutex_unlock(&etb_lock1);
}



OMX_ERRORTYPE EventHandler(OMX_IN OMX_HANDLETYPE hComponent,
                           OMX_IN OMX_PTR pAppData,
                           OMX_IN OMX_EVENTTYPE eEvent,
                           OMX_IN OMX_U32 nData1, OMX_IN OMX_U32 nData2,
                           OMX_IN OMX_PTR pEventData)
{
    DEBUG_PRINT("Function %s \n", __FUNCTION__);

   int bufCnt=0;
   (void)hComponent;
   (void)pAppData;
   (void)pEventData;
    switch(eEvent) {
        case OMX_EventCmdComplete:
            DEBUG_PRINT("*********************************************\n");
        DEBUG_PRINT("\n OMX_EventCmdComplete \n");
            DEBUG_PRINT("*********************************************\n");
            if(OMX_CommandPortDisable == (OMX_COMMANDTYPE)nData1)
            {
                DEBUG_PRINT("******************************************\n");
                DEBUG_PRINT("Recieved DISABLE Event Command Complete[%lu]\n",nData2);
                DEBUG_PRINT("******************************************\n");
            }
            else if(OMX_CommandPortEnable == (OMX_COMMANDTYPE)nData1)
            {
                DEBUG_PRINT("*********************************************\n");
                DEBUG_PRINT("Recieved ENABLE Event Command Complete[%lu]\n",nData2);
                DEBUG_PRINT("*********************************************\n");
            }
            else if(OMX_CommandFlush== (OMX_COMMANDTYPE)nData1)
            {
                DEBUG_PRINT("*********************************************\n");
                DEBUG_PRINT("Recieved FLUSH Event Command Complete[%lu]\n",nData2);
                DEBUG_PRINT("*********************************************\n");
            }
        event_complete();
        break;
        case OMX_EventError:
            DEBUG_PRINT("*********************************************\n");
            DEBUG_PRINT("\n OMX_EventError \n");
            DEBUG_PRINT("*********************************************\n");
            if(OMX_ErrorInvalidState == (OMX_ERRORTYPE)nData1)
            {
               DEBUG_PRINT("\n OMX_ErrorInvalidState \n");
               for(bufCnt=0; bufCnt < input_buf_cnt; ++bufCnt)
               {
                  OMX_FreeBuffer(ac3_dec_handle, 0, pInputBufHdrs[bufCnt]);
               }
               if(tunnel == 0)
               {
                   for(bufCnt=0; bufCnt < output_buf_cnt; ++bufCnt)
                   {
                     OMX_FreeBuffer(ac3_dec_handle, 1, pOutputBufHdrs[bufCnt]);
                   }
               }

               DEBUG_PRINT("*********************************************\n");
               DEBUG_PRINT("\n Component Deinitialized \n");
               DEBUG_PRINT("*********************************************\n");
               exit(0);
            }
            else if(OMX_ErrorComponentSuspended == (OMX_ERRORTYPE)nData1)
            {
               DEBUG_PRINT("*********************************************\n");
               DEBUG_PRINT("\n Component Received Suspend Event \n");
               DEBUG_PRINT("*********************************************\n");
            }
            break;

         case OMX_EventBufferFlag:
             DEBUG_PRINT("\n *********************************************\n");
             DEBUG_PRINT("\n OMX_EventBufferFlag \n");
             DEBUG_PRINT("\n *********************************************\n");
             if(tunnel)
             {
                 bInputEosReached = true;
             }
             else
             {
                 bOutputEosReached = true;
             }
             event_complete();
             break;

       case OMX_EventComponentResumed:
           DEBUG_PRINT("*********************************************\n");
           DEBUG_PRINT("\n Component Received Suspend Event \n");
           DEBUG_PRINT("*********************************************\n");
           break;

       default:
            DEBUG_PRINT("\n Unknown Event \n");
            break;
    }
    return OMX_ErrorNone;
}

OMX_ERRORTYPE FillBufferDone(OMX_IN OMX_HANDLETYPE hComponent,
                              OMX_IN OMX_PTR pAppData,
                              OMX_IN OMX_BUFFERHEADERTYPE* pBuffer)
{
   OMX_U32 i=0;
   int bytes_writen = 0;
   static int count = 0;
   static int copy_done = 0;
   static int length_filled = 0;
   static int spill_length = 0;
   static int  pcm_buf_size = 4800;
   static int unsigned pcm_buf_count = 2;
   struct msm_audio_config drv_pcm_config;
   (void)pAppData;

   if(flushinprogress == 1)
   {
       DEBUG_PRINT(" FillBufferDone: flush is in progress so hold the buffers\n");
       return OMX_ErrorNone;
   }
   if(count == 0 && pcmplayback)
   {
       DEBUG_PRINT(" open pcm device \n");
       m_pcmdrv_fd = open("/dev/msm_pcm_out", O_RDWR);
       if (m_pcmdrv_fd < 0)
       {
          DEBUG_PRINT("Cannot open audio device\n");
          return -1;
       }
       else
       {
          DEBUG_PRINT("Open pcm device successfull\n");
          DEBUG_PRINT("Configure Driver for PCM playback \n");
          ioctl(m_pcmdrv_fd, AUDIO_GET_CONFIG, &drv_pcm_config);
          DEBUG_PRINT("drv_pcm_config.buffer_count %d \n", drv_pcm_config.buffer_count);
          DEBUG_PRINT("drv_pcm_config.buffer_size %d \n", drv_pcm_config.buffer_size);
          drv_pcm_config.sample_rate = samplerate; //SAMPLE_RATE; //m_adec_param.nSampleRate;
          drv_pcm_config.channel_count = channels;  /* 1-> mono 2-> stereo*/
          ioctl(m_pcmdrv_fd, AUDIO_SET_CONFIG, &drv_pcm_config);
          DEBUG_PRINT("Configure Driver for PCM playback \n");
          ioctl(m_pcmdrv_fd, AUDIO_GET_CONFIG, &drv_pcm_config);
          DEBUG_PRINT("drv_pcm_config.buffer_count %d \n", drv_pcm_config.buffer_count);
          DEBUG_PRINT("drv_pcm_config.buffer_size %d \n", drv_pcm_config.buffer_size);
          pcm_buf_size = drv_pcm_config.buffer_size;
          pcm_buf_count = drv_pcm_config.buffer_count;
       }
       pBuffer_tmp= (OMX_U8*)malloc(pcm_buf_count*sizeof(OMX_U8)*pcm_buf_size);
       if (pBuffer_tmp == NULL)
       {
           return -1;
       }
       else
       {
           memset(pBuffer_tmp, 0, pcm_buf_count*pcm_buf_size);
       }
   }
   DEBUG_PRINT(" FillBufferDone #%d size %lu\n", count++,pBuffer->nFilledLen);
    if(bEosOnOutputBuf)
       return OMX_ErrorNone;
   if((tunnel == 0) && (filewrite == 1))
   {
       bytes_writen =
       fwrite(pBuffer->pBuffer,1,pBuffer->nFilledLen,outputBufferFile);
       DEBUG_PRINT(" FillBufferDone size writen to file  %d\n",bytes_writen);
       totaldatalen += bytes_writen ;
    }

#ifdef PCM_PLAYBACK
    if(pcmplayback && pBuffer->nFilledLen)
    {
        if(start_done == 0)
        {
            if((length_filled+pBuffer->nFilledLen)>=(pcm_buf_count*pcm_buf_size))
            {
                spill_length = (pBuffer->nFilledLen-(pcm_buf_count*pcm_buf_size)+length_filled);
                memcpy (pBuffer_tmp+length_filled, pBuffer->pBuffer, ((pcm_buf_count*pcm_buf_size)-length_filled));

                length_filled = (pcm_buf_count*pcm_buf_size);
                copy_done = 1;
            }
            else
            {
                memcpy (pBuffer_tmp+length_filled, pBuffer->pBuffer, pBuffer->nFilledLen);
                length_filled +=pBuffer->nFilledLen;
            }
            if (copy_done == 1)
            {
           for (i=0; i<pcm_buf_count; i++)
           {
                    if (write(m_pcmdrv_fd, pBuffer_tmp+i*pcm_buf_size, pcm_buf_size ) != pcm_buf_size)
                    {
                         DEBUG_PRINT("FillBufferDone: Write data to PCM failed\n");
                         return -1;
                    }

           }
               DEBUG_PRINT("AUDIO_START called for PCM \n");
               ioctl(m_pcmdrv_fd, AUDIO_START, 0);
           if (spill_length != 0)
           {
                   if (write(m_pcmdrv_fd, pBuffer->pBuffer+((pBuffer->nFilledLen)-spill_length), spill_length) != spill_length)
                   {
                       DEBUG_PRINT("FillBufferDone: Write data to PCM failed\n");
                    return -1;
                   }
               }

               copy_done = 0;
               start_done = 1;

            }
        }
        else
        {
            if (write(m_pcmdrv_fd, pBuffer->pBuffer, pBuffer->nFilledLen ) !=(ssize_t)
                pBuffer->nFilledLen)
            {
                DEBUG_PRINT("FillBufferDone: Write data to PCM failed\n");
                return OMX_ErrorNone;
            }
        }
        DEBUG_PRINT(" FillBufferDone: writing data to pcm device for play succesfull \n");
    }
#endif   // PCM_PLAYBACK


    if(pBuffer->nFlags != OMX_BUFFERFLAG_EOS)
    {
        DEBUG_PRINT(" FBD calling FTB");
        OMX_FillThisBuffer(hComponent,pBuffer);
    }
    else
    {
       DEBUG_PRINT(" FBD EOS REACHED...........\n");
       bEosOnOutputBuf = true;

    }
    return OMX_ErrorNone;

}


OMX_ERRORTYPE EmptyBufferDone(OMX_IN OMX_HANDLETYPE hComponent,
                              OMX_IN OMX_PTR pAppData,
                              OMX_IN OMX_BUFFERHEADERTYPE* pBuffer)
{
    int readBytes =0;
    (void)pAppData;

    DEBUG_PRINT("\nFunction %s cnt[%d], used_ip_buf_cnt[%d]\n", __FUNCTION__, ebd_cnt,used_ip_buf_cnt);
    DEBUG_PRINT("\nFunction %s %p %lu\n", __FUNCTION__, pBuffer,pBuffer->nFilledLen);
    ebd_cnt++;
    used_ip_buf_cnt--;
    pthread_mutex_lock(&etb_lock);
    if(!etb_done)
    {
        DEBUG_PRINT("\n*********************************************\n");
        DEBUG_PRINT("Wait till first set of buffers are given to component\n");
        DEBUG_PRINT("\n*********************************************\n");
        etb_done++;
        pthread_mutex_unlock(&etb_lock);
        etb_wait_for_event();
    }
    else
    {
        pthread_mutex_unlock(&etb_lock);
    }


    if(bEosOnInputBuf)
    {
        DEBUG_PRINT("\n*********************************************\n");
        DEBUG_PRINT("   EBD::EOS on input port\n ");
        DEBUG_PRINT("*********************************************\n");
        return OMX_ErrorNone;
    }else if (bFlushing == true) {
      DEBUG_PRINT("omx_ac3_adec_test: bFlushing is set to TRUE used_ip_buf_cnt=%d\n",used_ip_buf_cnt);
      if (used_ip_buf_cnt == 0) {
        //fseek(inputBufferFile, 0, 0);
        bFlushing = false;
      } else {
        DEBUG_PRINT("omx_ac3_adec_test: more buffer to come back used_ip_buf_cnt=%d\n",used_ip_buf_cnt);
        return OMX_ErrorNone;
      }
    }

    if((readBytes = Read_Buffer(pBuffer)) > 0) {
        pBuffer->nFilledLen = readBytes;
        used_ip_buf_cnt++;
        OMX_EmptyThisBuffer(hComponent,pBuffer);
    }
    else{
        pBuffer->nFlags |= OMX_BUFFERFLAG_EOS;
            used_ip_buf_cnt++;
            //bInputEosReached = true;
            bEosOnInputBuf = true;
        pBuffer->nFilledLen = 0;
        OMX_EmptyThisBuffer(hComponent,pBuffer);
        DEBUG_PRINT("EBD..Either EOS or Some Error while reading file\n");
    }
    return OMX_ErrorNone;
}

void signal_handler(int sig_id) {

  /* Flush */


   if (sig_id == SIGUSR1) {
    DEBUG_PRINT("%s Initiate flushing\n", __FUNCTION__);
    bFlushing = true;
    OMX_SendCommand(ac3_dec_handle, OMX_CommandFlush, OMX_ALL, NULL);
  } else if (sig_id == SIGUSR2) {
    if (bPause == true) {
      DEBUG_PRINT("%s resume playback\n", __FUNCTION__);
      bPause = false;
      OMX_SendCommand(ac3_dec_handle, OMX_CommandStateSet, OMX_StateExecuting, NULL);
    } else {
      DEBUG_PRINT("%s pause playback\n", __FUNCTION__);
      bPause = true;
      OMX_SendCommand(ac3_dec_handle, OMX_CommandStateSet, OMX_StatePause, NULL);
    }
  }
}

int main(int argc, char **argv)
{
    int bufCnt=0;
    OMX_ERRORTYPE result;
    struct sigaction sa;


    struct wav_header hdr;
    int bytes_writen = 0;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &signal_handler;
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);

    pthread_cond_init(&cond, 0);
    pthread_mutex_init(&lock, 0);

    pthread_cond_init(&etb_cond, 0);
    pthread_mutex_init(&etb_lock, 0);
    pthread_mutex_init(&etb_lock1, 0);
    pthread_mutexattr_init(&lock1_attr);
    pthread_mutex_init(&lock1, &lock1_attr);

//add args for ac3 here
    if (argc >= 10) {
      in_filename = argv[1];
      samplerate = atoi(argv[2]);
      channels = atoi(argv[3]);
      pcmplayback = atoi(argv[4]);
      tunnel  = atoi(argv[5]);
      filewrite = atoi(argv[6]);
      dual_mono = atoi(argv[7]);
      stereo_mode = atoi(argv[8]);
      karaoke_mode = atoi(argv[9]);
      strncpy((char *)out_filename,argv[1],strlen(argv[1]));

      strcat((char *)out_filename,".wav");

      if (tunnel == 1) {
           pcmplayback = 0; /* This feature holds good only for non tunnel mode*/
            filewrite = 0;  /* File write not supported in tunnel mode */
      }
    } else {

        DEBUG_PRINT(" invalid format: \n");
        DEBUG_PRINT("ex: ./mm-adec-omxac3 AC3INPUTFILE SAMPFREQ CHANNEL PCMPLAYBACK TUNNEL FILEWRITE"
                    "DUAL_MONO_MODE STEREO_MODE KARAOKE_MODE\n");
        DEBUG_PRINT( "PCMPLAYBACK = 1 (ENABLES PCM PLAYBACK IN NON TUNNEL MODE) \n");
        DEBUG_PRINT( "PCMPLAYBACK = 0 (DISABLES PCM PLAYBACK IN NON TUNNEL MODE) \n");
        DEBUG_PRINT( "TUNNEL = 1 (DECODED AC3 SAMPLES IS PLAYED BACK)\n");
        DEBUG_PRINT( "TUNNEL = 0 (DECODED AC3 SAMPLES IS LOOPED BACK TO THE USER APP)\n");
        DEBUG_PRINT( "FILEWRITE = 1 (ENABLES PCM FILEWRITE IN NON TUNNEL MODE) \n");
        DEBUG_PRINT( "FILEWRITE = 0 (DISABLES PCM FILEWRITE IN NON TUNNEL MODE) \n");
        DEBUG_PRINT( "DUAL_MONO = 0 (STEREO) (default)\n");
        DEBUG_PRINT( "DUAL_MONO = 1 (LEFT MONO) \n");
        DEBUG_PRINT( "DUAL_MONO = 2 (RIGHT MONO) \n");
        DEBUG_PRINT( "DUAL_MONO = 2 (MIXED MONO) \n");
        DEBUG_PRINT( "STEREO_MODE = 0 (AUTO DETECT) (default)\n");
        DEBUG_PRINT( "STEREO_MODE = 1 (DOLBY SURROUND COMPATIBLE Lt/Rt) \n");
        DEBUG_PRINT( "STEREO_MODE = 2 (STEREO Lo/Ro) \n");
        DEBUG_PRINT( "KARAOKE_MODE = 0 (NO VOCAL) \n");
        DEBUG_PRINT( "KARAOKE_MODE = 1 (LEFT VOCAL) \n");
        DEBUG_PRINT( "KARAOKE_MODE = 2 (RIGHT VOCAL) \n");
        DEBUG_PRINT( "KARAOKE_MODE = 3 (BOTH VOCALS) (default) \n");
        return 0;
    }

    if(tunnel == 0)
        aud_comp = "OMX.qcom.audio.decoder.ac3";
    else
        aud_comp = "OMX.qcom.audio.decoder.tunneled.ac3"; //add entry in registry table

    if(Init_Decoder(aud_comp)!= 0x00)
    {
        DEBUG_PRINT("Decoder Init failed\n");
        return -1;
    }

    if(Play_Decoder() != 0x00)
    {
        DEBUG_PRINT("Play_Decoder failed\n");
        return -1;
    }

    // Wait till EOS is reached...

   printf("before wait_for_event\n");
    wait_for_event();
   if(bOutputEosReached || (tunnel && bInputEosReached)) {

        /******************************************************************/
        #ifdef PCM_PLAYBACK
        if(pcmplayback == 1)
        {
            sleep(1);
            ioctl(m_pcmdrv_fd, AUDIO_STOP, 0);

            if(m_pcmdrv_fd >= 0) {
                close(m_pcmdrv_fd);
                m_pcmdrv_fd = -1;
                DEBUG_PRINT(" PCM device closed succesfully \n");
            }
            else
            {
                DEBUG_PRINT(" PCM device close failure \n");
            }
        }
        #endif // PCM_PLAYBACK

        if((tunnel == 0)&& (filewrite == 1))
        {
            hdr.riff_id = ID_RIFF;
            hdr.riff_sz = 0;
            hdr.riff_fmt = ID_WAVE;
            hdr.fmt_id = ID_FMT;
            hdr.fmt_sz = 16;
            hdr.audio_format = FORMAT_PCM;
            hdr.num_channels = channels;//2;
            hdr.sample_rate = samplerate; //SAMPLE_RATE;  //44100;
            hdr.byte_rate = hdr.sample_rate * hdr.num_channels * 2;
            hdr.block_align = hdr.num_channels * 2;
            hdr.bits_per_sample = 16;
            hdr.data_id = ID_DATA;
            hdr.data_sz = 0;

            DEBUG_PRINT("output file closed and EOS reached total decoded data length %d\n",totaldatalen);
            hdr.data_sz = totaldatalen;
            hdr.riff_sz = totaldatalen + 8 + 16 + 8;
            fseek(outputBufferFile, 0L , SEEK_SET);
            bytes_writen = fwrite(&hdr,1,sizeof(hdr),outputBufferFile);
            if (bytes_writen <= 0) {
                DEBUG_PRINT("Invalid Wav header write failed\n");
            }
            bFileclose = 1;
            fclose(outputBufferFile);
        }
        /************************************************************************************/
        DEBUG_PRINT("\nMoving the decoder to idle state \n");
        OMX_SendCommand(ac3_dec_handle, OMX_CommandStateSet, OMX_StateIdle,0);
        wait_for_event();
        DEBUG_PRINT("\nMoving the decoder to loaded state \n");
        OMX_SendCommand(ac3_dec_handle, OMX_CommandStateSet, OMX_StateLoaded,0);

        DEBUG_PRINT("\nFillBufferDone: Deallocating i/p buffers \n");
        for(bufCnt=0; bufCnt < input_buf_cnt; ++bufCnt) {
            OMX_FreeBuffer(ac3_dec_handle, 0, pInputBufHdrs[bufCnt]);
        }

        if(tunnel == 0)
        {
            DEBUG_PRINT("\nFillBufferDone: Deallocating o/p buffers \n");
            for(bufCnt=0; bufCnt < output_buf_cnt; ++bufCnt) {
            OMX_FreeBuffer(ac3_dec_handle, 1, pOutputBufHdrs[bufCnt]);
            }
        }


        ebd_cnt=0;
            wait_for_event();
            ebd_cnt=0;
        bOutputEosReached = false;
        bInputEosReached = false;
            bEosOnInputBuf = 0;
            bEosOnOutputBuf = 0;
        result = OMX_FreeHandle(ac3_dec_handle);
        if (result != OMX_ErrorNone) {
            DEBUG_PRINT ("\nOMX_FreeHandle error. Error code: %d\n", result);
        }
           ac3_dec_handle = NULL;
        /* Deinit OpenMAX */

        OMX_Deinit();

        pthread_cond_destroy(&cond);
        pthread_mutex_destroy(&lock);
        etb_done = 0;
        bReconfigureOutputPort = 0;
        if (pBuffer_tmp)
        {
            free(pBuffer_tmp);
            pBuffer_tmp =NULL;
        }

        DEBUG_PRINT("*****************************************\n");
        DEBUG_PRINT("******...TEST COMPLETED...***************\n");
        DEBUG_PRINT("*****************************************\n");
    }
    return 0;
}

int Init_Decoder(OMX_STRING audio_component)
{
    DEBUG_PRINT("Inside %s \n", __FUNCTION__);
    OMX_ERRORTYPE omxresult;
    OMX_U32 total = 0;
    typedef OMX_U8* OMX_U8_PTR;
    OMX_STRING role ="audio_decoder.ac3";

    static OMX_CALLBACKTYPE call_back = {
        &EventHandler,&EmptyBufferDone,&FillBufferDone
    };

    /* Init. the OpenMAX Core */
    DEBUG_PRINT("\nInitializing OpenMAX Core....\n");
    omxresult = OMX_Init();

    if(OMX_ErrorNone != omxresult) {
        DEBUG_PRINT("\n Failed to Init OpenMAX core");
          return -1;
    }
    else {
        DEBUG_PRINT("\nOpenMAX Core Init Done\n");
    }

   /* Query for audio decoders*/
    DEBUG_PRINT("Ac3_test: Before entering OMX_GetComponentOfRole");
//    OMX_GetComponentsOfRole(role, &total, 0);
    DEBUG_PRINT ("\nTotal components of role=%s :%lu", role, total);



    omxresult = OMX_GetHandle((OMX_HANDLETYPE*)(&ac3_dec_handle),
                        (OMX_STRING)audio_component, NULL, &call_back);
    if (FAILED(omxresult)) {
        DEBUG_PRINT("\nFailed to Load the component:%s\n", audio_component);
    return -1;
    }
    else
    {
        DEBUG_PRINT("\nComponent %s is in LOADED state\n", audio_component);
    }

    /* Get the port information */
    CONFIG_VERSION_SIZE(portParam);
    omxresult = OMX_GetParameter(ac3_dec_handle, OMX_IndexParamAudioInit,
                                (OMX_PTR)&portParam);

    if(FAILED(omxresult)) {
        DEBUG_PRINT("\nFailed to get Port Param\n");
    return -1;
    }
    else
    {
        DEBUG_PRINT("\nportParam.nPorts:%lu\n", portParam.nPorts);
    DEBUG_PRINT("\nportParam.nStartPortNumber:%lu\n",
                                             portParam.nStartPortNumber);
    }
    return 0;
}

int Play_Decoder()
{
    int i;
    int Size=0;
    DEBUG_PRINT("Inside %s \n", __FUNCTION__);
    OMX_ERRORTYPE ret;
    OMX_INDEXTYPE index;
    DEBUG_PRINT("sizeof[%d]\n", sizeof(OMX_BUFFERHEADERTYPE));
    /* open the i/p and o/p files based on the video file format passed */
    if(open_audio_file()) {
        DEBUG_PRINT("\n Returning -1");
    return -1;
    }

    /*  Configuration of Input Port definition */

    /* Query the decoder input min buf requirements */
    CONFIG_VERSION_SIZE(inputportFmt);

    /* Port for which the Client needs to obtain info */
    inputportFmt.nPortIndex = portParam.nStartPortNumber;

    OMX_GetParameter(ac3_dec_handle,OMX_IndexParamPortDefinition,&inputportFmt);
    DEBUG_PRINT ("\nDec: Input Buffer Count %lu\n", inputportFmt.nBufferCountMin);
    DEBUG_PRINT ("\nDec: Input Buffer Size %lu\n", inputportFmt.nBufferSize);

    if(OMX_DirInput != inputportFmt.eDir) {
        DEBUG_PRINT ("\nDec: Expect Input Port\n");
    return -1;
    }

    inputportFmt.nBufferCountActual = inputportFmt.nBufferCountMin + 5;
    OMX_SetParameter(ac3_dec_handle,OMX_IndexParamPortDefinition,&inputportFmt);
if(tunnel == 0)
{
      /*  Configuration of Ouput Port definition */

    /* Query the decoder outport's min buf requirements */
    CONFIG_VERSION_SIZE(outputportFmt);
    /* Port for which the Client needs to obtain info */
    outputportFmt.nPortIndex = portParam.nStartPortNumber + 1;

    OMX_GetParameter(ac3_dec_handle,OMX_IndexParamPortDefinition,&outputportFmt);
    DEBUG_PRINT ("\nDec: Output Buffer Count %lu\n", outputportFmt.nBufferCountMin);
    DEBUG_PRINT ("\nDec: Output Buffer Size %lu\n", outputportFmt.nBufferSize);

    if(OMX_DirOutput != outputportFmt.eDir) {
        DEBUG_PRINT ("\nDec: Expect Output Port\n");
    return -1;
    }

    outputportFmt.nBufferCountActual = outputportFmt.nBufferCountMin;
    OMX_SetParameter(ac3_dec_handle,OMX_IndexParamPortDefinition,&outputportFmt);
}


//add ac3param and ac3pp here
    memset(&ac3param, 0, sizeof(ac3param));
    CONFIG_VERSION_SIZE(ac3param);
    OMX_GetParameter(ac3_dec_handle,QOMX_IndexParamAudioAc3,&ac3param);
    ac3param.nPortIndex   =  0;
    ac3param.nSamplingRate = samplerate;
    ac3param.nChannels = channels;
/*
    ac3param.eFormat = omx_audio_ac3; //dsp parses bit stream to know ac3 type, so set to default
    ac3param.eChannelConfig = OMX_AUDIO_AC3_CHANNEL_CONFIG_2_0;
    ac3param.bCompressionOn = OMX_TRUE;
    ac3param.bLfeOn = OMX_TRUE;
    ac3param.bDelaySurroundChannels = OMX_TRUE;
*/
    OMX_SetParameter(ac3_dec_handle,QOMX_IndexParamAudioAc3,&ac3param);

    memset(&ac3PP, 0, sizeof(ac3PP));
    CONFIG_VERSION_SIZE(ac3PP);
    OMX_GetParameter(ac3_dec_handle,QOMX_IndexParamAudioAc3PostProc,&ac3PP);
/*
    ac3PP.eChannelRouting[0] =  OMX_AUDIO_AC3_CHANNEL_LEFT;
    ac3PP.eChannelRouting[1] =  OMX_AUDIO_AC3_CHANNEL_RIGHT;
    ac3PP.eChannelRouting[2] =  OMX_AUDIO_AC3_CHANNEL_CENTER;
    ac3PP.eChannelRouting[3] =  OMX_AUDIO_AC3_CHANNEL_LEFT_SURROUND;
    ac3PP.eChannelRouting[4] =  OMX_AUDIO_AC3_CHANNEL_RIGHT_SURROUND;
    ac3PP.eChannelRouting[5] =  OMX_AUDIO_AC3_CHANNEL_SURROUND;
    ac3PP.eCompressionMode = OMX_AUDIO_AC3_COMPRESSION_MODE_LINE_OUT;
    ac3PP.usPcmScale = OMX_ADEC_DEFAULT_PCM_SCALE_FACTOR;
    ac3PP.usDynamicScaleBoost = OMX_ADEC_DEFAULT_DYNAMIC_SCALE_BOOST;
    ac3PP.usDynamicScaleCut = OMX_ADEC_DEFAULT_DYNAMIC_SCALE_CUT;
*/
    ac3PP.eStereoMode = dual_mono;
    ac3PP.eDualMonoMode = stereo_mode;
    ac3PP.eKaraokeMode = karaoke_mode;

    OMX_SetParameter(ac3_dec_handle,QOMX_IndexParamAudioAc3PostProc,&ac3PP);


    DEBUG_PRINT ("\nOMX_SendCommand Decoder -> IDLE\n");
    OMX_SendCommand(ac3_dec_handle, OMX_CommandStateSet, OMX_StateIdle,0);
    /* wait_for_event(); should not wait here event complete status will
       not come until enough buffer are allocated */

    input_buf_cnt = inputportFmt.nBufferCountActual; //  inputportFmt.nBufferCountMin + 5;
    DEBUG_PRINT("Transition to Idle State succesful...\n");
    /* Allocate buffer on decoder's i/p port */
    error = Allocate_Buffer(ac3_dec_handle, &pInputBufHdrs, inputportFmt.nPortIndex,
                            input_buf_cnt, inputportFmt.nBufferSize);
    if (error != OMX_ErrorNone) {
        DEBUG_PRINT ("\nOMX_AllocateBuffer Input buffer error\n");
    return -1;
    }
    else {
        DEBUG_PRINT ("\nOMX_AllocateBuffer Input buffer success\n");
    }

if(tunnel == 0)
{
    output_buf_cnt = outputportFmt.nBufferCountActual; // outputportFmt.nBufferCountMin ;

    /* Allocate buffer on decoder's O/Pp port */
    error = Allocate_Buffer(ac3_dec_handle, &pOutputBufHdrs, outputportFmt.nPortIndex,
                            output_buf_cnt, outputportFmt.nBufferSize);
    if (error != OMX_ErrorNone) {
        DEBUG_PRINT ("\nOMX_AllocateBuffer Output buffer error\n");
    return -1;
    }
    else {
        DEBUG_PRINT ("\nOMX_AllocateBuffer Output buffer success\n");
    }
}

    wait_for_event();


if (tunnel == 1)
{
    DEBUG_PRINT ("\nOMX_SendCommand to enable TUNNEL MODE during IDLE\n");
    OMX_SendCommand(ac3_dec_handle, OMX_CommandPortDisable,1,0);
    wait_for_event();
}

    DEBUG_PRINT ("\nOMX_SendCommand Decoder -> Executing\n");
    OMX_SendCommand(ac3_dec_handle, OMX_CommandStateSet, OMX_StateExecuting,0);
    wait_for_event();

if(tunnel == 0)
{
    DEBUG_PRINT(" Start sending OMX_FILLthisbuffer\n");
    for(i=0; i < output_buf_cnt; i++) {
        DEBUG_PRINT ("\nOMX_FillThisBuffer on output buf no.%d\n",i);
        pOutputBufHdrs[i]->nOutputPortIndex = 1;
        pOutputBufHdrs[i]->nFlags = 0;
        ret = OMX_FillThisBuffer(ac3_dec_handle, pOutputBufHdrs[i]);
        if (OMX_ErrorNone != ret) {
            DEBUG_PRINT("OMX_FillThisBuffer failed with result %d\n", ret);
    }
        else {
            DEBUG_PRINT("OMX_FillThisBuffer success!\n");
    }
    }
}


    DEBUG_PRINT(" Start sending OMX_emptythisbuffer\n");
    for (i = 0;i < input_buf_cnt;i++) {
        DEBUG_PRINT ("\nOMX_EmptyThisBuffer on Input buf no.%d\n",i);
        pInputBufHdrs[i]->nInputPortIndex = 0;
        Size = Read_Buffer(pInputBufHdrs[i]);
        if(Size <=0 ){
          DEBUG_PRINT("NO DATA READ\n");
          //bInputEosReached = true;
          bEosOnInputBuf = true;
          pInputBufHdrs[i]->nFlags= OMX_BUFFERFLAG_EOS;
        }
        pInputBufHdrs[i]->nFilledLen = Size;
        pInputBufHdrs[i]->nInputPortIndex = 0;
        used_ip_buf_cnt++;
        ret = OMX_EmptyThisBuffer(ac3_dec_handle, pInputBufHdrs[i]);
        if (OMX_ErrorNone != ret) {
            DEBUG_PRINT("OMX_EmptyThisBuffer failed with result %d\n", ret);
        }
        else {
            DEBUG_PRINT("OMX_EmptyThisBuffer success!\n");
        }
        if(Size <=0 ){
            break;//eos reached
        }
    }
    pthread_mutex_lock(&etb_lock);
    if(etb_done)
{
        DEBUG_PRINT("Component is waiting for EBD to be released.\n");
        etb_event_complete();
    }
    else
    {
        DEBUG_PRINT("\n****************************\n");
        DEBUG_PRINT("EBD not yet happened ...\n");
        DEBUG_PRINT("\n****************************\n");
        etb_done++;
    }
    pthread_mutex_unlock(&etb_lock);
    while(1)
    {
    wait_for_event();
        if(bOutputEosReached || (tunnel && bInputEosReached))
        {
            bReconfigureOutputPort = 0;
            printf("bOutputEosReached || (tunnel && bInputEosReached breaking\n");
            break;
    }
    }
    return 0;
}

static OMX_ERRORTYPE Allocate_Buffer ( OMX_COMPONENTTYPE *avc_dec_handle,
                                       OMX_BUFFERHEADERTYPE  ***pBufHdrs,
                                       OMX_U32 nPortIndex,
                                       long bufCntMin, long bufSize)
{
    DEBUG_PRINT("Inside %s \n", __FUNCTION__);
    OMX_ERRORTYPE error=OMX_ErrorNone;
    long bufCnt=0;
    (void)avc_dec_handle;
    *pBufHdrs= (OMX_BUFFERHEADERTYPE **)
                   malloc(sizeof(OMX_BUFFERHEADERTYPE*)*bufCntMin);

    for(bufCnt=0; bufCnt < bufCntMin; ++bufCnt) {
        DEBUG_PRINT("\n OMX_AllocateBuffer No %lu \n", bufCnt);
        error = OMX_AllocateBuffer(ac3_dec_handle, &((*pBufHdrs)[bufCnt]),
                                   nPortIndex, NULL, bufSize);
    }

    return error;
}




static int Read_Buffer (OMX_BUFFERHEADERTYPE  *pBufHdr )
{
    int bytes_read=0;
    pBufHdr->nFilledLen = 0;
    pBufHdr->nFlags |= OMX_BUFFERFLAG_EOS;
    DEBUG_PRINT("\n Length : %lu, buffer address : %p\n", pBufHdr->nAllocLen, pBufHdr->pBuffer);
        bytes_read = fread(pBufHdr->pBuffer, 1, pBufHdr->nAllocLen , inputBufferFile);
    pBufHdr->nFilledLen = bytes_read;
    if(bytes_read == 0)
    {
       pBufHdr->nFlags |= OMX_BUFFERFLAG_EOS;
       DEBUG_PRINT ("\nBytes read zero\n");
    }
    else
    {
       pBufHdr->nFlags &= ~OMX_BUFFERFLAG_EOS;
       DEBUG_PRINT ("\nBytes read is Non zero=%d\n",bytes_read);
    }
    return bytes_read;;
}


static int open_audio_file ()
{
    int error_code = 0;
    struct wav_header hdr;
    int header_len = 0;
    memset(&hdr,0,sizeof(hdr));

    hdr.riff_id = ID_RIFF;
    hdr.riff_sz = 0;
    hdr.riff_fmt = ID_WAVE;
    hdr.fmt_id = ID_FMT;
    hdr.fmt_sz = 16;
    hdr.audio_format = FORMAT_PCM;
    hdr.num_channels = channels;//2;
    hdr.sample_rate = samplerate; //SAMPLE_RATE;  //44100;
    hdr.byte_rate = hdr.sample_rate * hdr.num_channels * 2;
    hdr.block_align = hdr.num_channels * 2;
    hdr.bits_per_sample = 16;
    hdr.data_id = ID_DATA;
    hdr.data_sz = 0;

    DEBUG_PRINT("Inside %s filename=%s -->%s\n", __FUNCTION__, in_filename,out_filename);
    inputBufferFile = fopen (in_filename, "rb");
    DEBUG_PRINT("\n FILE DESCRIPTOR : %p\n", inputBufferFile );
    if (inputBufferFile == NULL) {
        DEBUG_PRINT("\ni/p file %s could NOT be opened\n",
                                         in_filename);
    error_code = -1;
    }

if((tunnel == 0)&& (filewrite == 1))
{
    DEBUG_PRINT("output file is opened\n");
      outputBufferFile = fopen(out_filename,"wb");
    if (outputBufferFile == NULL) {
        DEBUG_PRINT("\no/p file %s could NOT be opened\n",
                                         out_filename);
        error_code = -1;
    }

    header_len = fwrite(&hdr,1,sizeof(hdr),outputBufferFile);


    if (header_len <= 0) {
        DEBUG_PRINT("Invalid Wav header \n");
    }
    DEBUG_PRINT(" Length og wav header is %d \n",header_len );
}
     return error_code;
}





