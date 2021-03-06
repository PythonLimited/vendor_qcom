/*============================================================================
O p e n M A X   w r a p p e r s
O p e n  M A X   C o r e

*//** @file omx_wma_adec.cpp
This module contains the implementation of the OpenMAX core & component.

Copyright (c) 2006-2008, 2011-2012 Qualcomm Technologies, Inc.
All Rights Reserved. Qualcomm Technologies Proprietary and Confidential.
*//*========================================================================*/

//////////////////////////////////////////////////////////////////////////////
//                             Include Files
//////////////////////////////////////////////////////////////////////////////

#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "omx_wma_adec.h"
#include <linux/msm_audio.h>
#include <linux/msm_audio_wma.h>
#include <linux/msm_audio_wmapro.h>
/*************************************************/
using namespace std;
#if DUMPS_ENABLE
int bfd=-1,pfd=-1;
#endif

// omx_cmd_queue destructor
omx_wma_adec::omx_cmd_queue::~omx_cmd_queue()
{
    // Nothing to do
}

// omx cmd queue constructor
omx_wma_adec::omx_cmd_queue::omx_cmd_queue(): m_read(0),m_write(0),m_size(0)
{
    memset(m_q,      0,sizeof(omx_event)*OMX_CORE_CONTROL_CMDQ_SIZE);
}

/*=============================================================================
FUNCTION:
insert_entry

DESCRIPTION:
Inserts the new commands into the queue

INPUT/OUTPUT PARAMETERS:
[IN] p1
[IN] p2
[IN] id

RETURN VALUE:
FALSE Failure
TRUE Success

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::omx_cmd_queue::insert_entry(unsigned p1,
                                               unsigned p2, unsigned id)
{
    bool ret = true;
    if(m_size < OMX_CORE_CONTROL_CMDQ_SIZE)
    {
        m_q[m_write].id       = id;
        m_q[m_write].param1   = p1;
        m_q[m_write].param2   = p2;
        m_write++;
        m_size ++;
        if(m_write >= OMX_CORE_CONTROL_CMDQ_SIZE)
        {
            m_write = 0;
        }
    } else
    {
        ret = false;
        DEBUG_PRINT_ERROR("ERROR!!! Command Queue Full");
    }
    return ret;
}

/*=============================================================================
FUNCTION:
pop_entry

DESCRIPTION:
reads the commands that are inserted in the queue

INPUT/OUTPUT PARAMETERS:
[OUT] p1
[OUT] p2
[OUT] id

RETURN VALUE:
FALSE Failure
TRUE Success

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::omx_cmd_queue::pop_entry(unsigned *p1,
                                            unsigned *p2, unsigned *id)
{
    bool ret = true;
    if (m_size > 0)
    {
        *id = m_q[m_read].id;
        *p1 = m_q[m_read].param1;
        *p2 = m_q[m_read].param2;
        // Move the read pointer ahead
        ++m_read;
        --m_size;
        if(m_read >= OMX_CORE_CONTROL_CMDQ_SIZE)
        {
            m_read = 0;
        }
    } else
    {
        ret = false;
        DEBUG_PRINT_ERROR("ERROR Delete!!! Command Queue Empty");
    }
    return ret;
}

// factory function executed by the core to create instances
void *get_omx_component_factory_fn(void)
{
    return (new omx_wma_adec);
}
bool omx_wma_adec::omx_cmd_queue::get_msg_id(unsigned *id)
{
   if(m_size > 0)
   {
       *id = m_q[m_read].id;
       DEBUG_PRINT("get_msg_id=%d\n",*id);
   }
   else{
       return false;
   }
   return true;
}
void omx_wma_adec::process_event_cb(void *client_data, unsigned char id)
{
    DEBUG_PRINT("PE:Waiting for event's...");
    (void)id;
    omx_wma_adec  *pThis = (omx_wma_adec *) client_data;
    pThis->process_events(pThis);
}
void omx_wma_adec::process_events(omx_wma_adec *client_data)
{
    OMX_STATETYPE          state;
    struct msm_audio_event tcxo_event;
    int rc = 0;

    (void)client_data;
    // This loop waits indefintely till an EVENT is recieved.
    while(1)
    {
        rc = ioctl(m_drv_fd,AUDIO_GET_EVENT,&tcxo_event);
        DEBUG_PRINT("PE: Event Thread %d errno=%d",rc,errno);
        if((rc == -1) )
        {
            DEBUG_PRINT("PE:Event Thread exiting %d",rc);
            return;
        }
        get_state(&m_cmp, &state);
        if((state != OMX_StatePause) ||
           (suspensionPolicy != OMX_SuspensionEnabled))
        {
            DEBUG_PRINT("PE:Ignoring Event state[%d] suspension_policy[%d]\
                            event[%d] ",state, suspensionPolicy,tcxo_event.event_type);
            // Ignore events if not Pause state;
            continue;
        }
        pthread_mutex_lock(&m_suspendresume_lock);
        if(getSuspendFlg() && getResumeFlg())
        {
            DEBUG_PRINT("PE:Ignoring Event Already in Suspended state[%d] suspension_policy[%d] \
                         event[%d] suspendflg[%d] resumeflg[%d]\n",state, suspensionPolicy,
                         tcxo_event.event_type,getSuspendFlg(),getResumeFlg());
            pthread_mutex_unlock(&m_suspendresume_lock);
            // Ignore events if not Pause state;
            continue;
        }
        pthread_mutex_unlock(&m_suspendresume_lock);
        DEBUG_PRINT("PE:state[%d] suspensionpolicy[%d] event[%d]",state,
                     suspensionPolicy, tcxo_event.event_type);
        switch(tcxo_event.event_type)
        {
            case AUDIO_EVENT_SUSPEND:
            {
                DEBUG_PRINT("PE: Recieved AUDIO_EVENT_SUSPEND");
                pthread_mutex_lock(&m_suspendresume_lock);
                if(pcm_feedback &&
                  (suspensionPolicy == OMX_SuspensionEnabled) &&
                  (state == OMX_StatePause) )
                {
                    if(getSuspendFlg())
                    {
                        DEBUG_PRINT("suspend event already in process\n");
                        pthread_mutex_unlock(&m_suspendresume_lock);
                        break;
                    }
                    pthread_mutex_unlock(&m_suspendresume_lock);
                    m_timer->stopTimer();
                    post_command(0,0,OMX_COMPONENT_SUSPEND);
                }
                else
                {
                    pthread_mutex_unlock(&m_suspendresume_lock);
                    DEBUG_PRINT("Suspend not processed Mode[%d] state[%d] Suspend[%d]",
                                pcm_feedback,state,bSuspendEventRxed);
                }
            }
            break;
            case AUDIO_EVENT_RESUME:
            {
                if ( getSuspendFlg() && !getResumeFlg() )
                {
                    DEBUG_PRINT("PE: Recieved AUDIO_EVENT_RESUME");
                    // signal the output thread that RESUME has happened
                    post_command(0,0,OMX_COMPONENT_RESUME);
                }
            }
            break;
            default:
                DEBUG_PRINT("PE: Recieved Invalid Event");
                break;
        }
    }
    return;
}

/*=============================================================================
FUNCTION:
wait_for_event

DESCRIPTION:
waits for a particular event

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::wait_for_event()
{
    pthread_mutex_lock(&m_event_lock);
    while(0 == m_is_event_done)
    {
        pthread_cond_wait(&cond, &m_event_lock);
    }
    m_is_event_done = 0;
    pthread_mutex_unlock(&m_event_lock);
}

/*=============================================================================
FUNCTION:
event_complete

DESCRIPTION:
informs about the occurance of an event

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::event_complete()
{
    pthread_mutex_lock(&m_event_lock);
    if(0 == m_is_event_done)
    {
        m_is_event_done = 1;
        pthread_cond_signal(&cond);
    }
    pthread_mutex_unlock(&m_event_lock);
}

// All this non-sense because of a single wma object
void omx_wma_adec::in_th_goto_sleep()
{
    pthread_mutex_lock(&m_in_th_lock);
    while (0 == m_is_in_th_sleep)
    {
        pthread_cond_wait(&in_cond, &m_in_th_lock);
    }
    m_is_in_th_sleep = 0;
    pthread_mutex_unlock(&m_in_th_lock);
}

void omx_wma_adec::in_th_wakeup()
{
    pthread_mutex_lock(&m_in_th_lock);
    if (0 == m_is_in_th_sleep)
    {
        m_is_in_th_sleep = 1;
        pthread_cond_signal(&in_cond);
    }
    pthread_mutex_unlock(&m_in_th_lock);
}

void omx_wma_adec::out_th_goto_sleep()
{

    pthread_mutex_lock(&m_out_th_lock);
    while (0 == m_is_out_th_sleep)
    {
        pthread_cond_wait(&out_cond, &m_out_th_lock);
    }
    m_is_out_th_sleep = 0;
    pthread_mutex_unlock(&m_out_th_lock);
}

void omx_wma_adec::out_th_wakeup()
{
    pthread_mutex_lock(&m_out_th_lock);
    if (0 == m_is_out_th_sleep)
    {
        m_is_out_th_sleep = 1;
        pthread_cond_signal(&out_cond);
    }
    pthread_mutex_unlock(&m_out_th_lock);
}
/*=============================================================================
FUNCTION:
omx_wma_adec

DESCRIPTION:
Constructor

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
omx_wma_adec::omx_wma_adec():m_timer(NULL),
m_tmp_in_meta_buf(NULL),
m_tmp_out_meta_buf(NULL),
m_trans_buffer_start(NULL),
m_flush_cnt(255),
m_eos_bm(0),
m_first(0),
bTsupdate(0),
m_bufMgr(NULL),
m_drv_fd(-1),
bFlushinprogress(0),
is_in_th_sleep(false),
is_out_th_sleep(false),
bOutputPortReEnabled(false),
bSendWMAPacketpending(false),
m_pause_to_exe(false),
waitForSuspendCmplFlg(false),
m_inp_act_buf_count (OMX_CORE_NUM_INPUT_BUFFERS),
m_out_act_buf_count (OMX_CORE_NUM_OUTPUT_BUFFERS),
m_inp_current_buf_count(0),
m_out_current_buf_count(0),
m_flags(0),
output_buffer_size(OMX_CORE_OUTPUT_BUFFER_SIZE),
input_buffer_size(OMX_CORE_INPUT_BUFFER_SIZE),
m_inp_bEnabled(OMX_TRUE),
m_out_bEnabled(OMX_TRUE),
m_inp_bPopulated(OMX_FALSE),
m_out_bPopulated(OMX_FALSE),
m_is_event_done(0),
m_app_data(NULL),
m_state(OMX_StateInvalid),
m_ipc_to_in_th(NULL),
m_ipc_to_out_th(NULL),
m_ipc_to_cmd_th(NULL),
m_ipc_to_event_th(0),
m_comp_deinit(0)
{
    int cond_ret = 0;
    memset(&m_cmp, 0, sizeof(m_cmp));
    memset(&m_cb, 0, sizeof(m_cb));

    pthread_mutexattr_init(&m_lock_attr);
    pthread_mutex_init(&m_lock, &m_lock_attr);
    pthread_mutexattr_init(&m_commandlock_attr);
    pthread_mutex_init(&m_commandlock, &m_commandlock_attr);

    pthread_mutexattr_init(&m_outputlock_attr);
    pthread_mutex_init(&m_outputlock, &m_outputlock_attr);

    pthread_mutexattr_init(&m_state_attr);
    pthread_mutex_init(&m_state_lock, &m_state_attr);

    pthread_mutexattr_init(&m_event_attr);
    pthread_mutex_init(&m_event_lock, &m_event_attr);

    pthread_mutexattr_init(&m_flush_attr);
    pthread_mutex_init(&m_flush_lock, &m_flush_attr);

    pthread_mutexattr_init(&m_in_th_attr);
    pthread_mutex_init(&m_in_th_lock, &m_in_th_attr);

    pthread_mutexattr_init(&m_out_th_attr);
    pthread_mutex_init(&m_out_th_lock, &m_out_th_attr);

    pthread_mutexattr_init(&m_in_th_attr_1);
    pthread_mutex_init(&m_in_th_lock_1, &m_in_th_attr_1);

    pthread_mutexattr_init(&m_out_th_attr_1);
    pthread_mutex_init(&m_out_th_lock_1, &m_out_th_attr_1);

    pthread_mutexattr_init(&out_buf_count_lock_attr);
    pthread_mutex_init(&out_buf_count_lock, &out_buf_count_lock_attr);

    pthread_mutexattr_init(&in_buf_count_lock_attr);
    pthread_mutex_init(&in_buf_count_lock, &in_buf_count_lock_attr);
    pthread_mutexattr_init(&m_suspendresume_lock_attr);
    pthread_mutex_init(&m_suspendresume_lock, &m_suspendresume_lock_attr);
    pthread_mutexattr_init(&m_WaitForSuspendCmpl_lock_attr);
    pthread_mutex_init(&m_WaitForSuspendCmpl_lock, &m_WaitForSuspendCmpl_lock_attr);

    if ((cond_ret = pthread_cond_init (&cond, NULL)) != 0)
    {
       DEBUG_PRINT_ERROR("pthread_cond_init returns non zero for cond\n");
       if (cond_ret == EAGAIN)
         DEBUG_PRINT_ERROR("The system lacked necessary resources(other than mem)\n");
       else if (cond_ret == ENOMEM)
          DEBUG_PRINT_ERROR("Insufficient memory to initialise condition variable\n");
    }
    if ((cond_ret = pthread_cond_init (&in_cond, NULL)) != 0)
    {
       DEBUG_PRINT_ERROR("pthread_cond_init returns non zero for in_cond\n");
       if (cond_ret == EAGAIN)
         DEBUG_PRINT_ERROR("The system lacked necessary resources(other than mem)\n");
       else if (cond_ret == ENOMEM)
          DEBUG_PRINT_ERROR("Insufficient memory to initialise condition variable\n");
    }
    if ((cond_ret = pthread_cond_init (&out_cond, NULL)) != 0)
    {
       DEBUG_PRINT_ERROR("pthread_cond_init returns non zero for out_cond\n");
       if (cond_ret == EAGAIN)
         DEBUG_PRINT_ERROR("The system lacked necessary resources(other than mem)\n");
       else if (cond_ret == ENOMEM)
          DEBUG_PRINT_ERROR("Insufficient memory to initialise condition variable\n");
    }
    sem_init(&sem_States,0, 0);
    sem_init(&sem_WaitForSuspendCmpl_states,0, 0);
    m_timer = new timer(this);
    if(m_timer == NULL)
    {
        DEBUG_PRINT_ERROR("Not able to allocate memory for timer obj\n");
    }
    m_bufMgr = new omxBufMgr;
    if(m_bufMgr == NULL)
    {
        DEBUG_PRINT_ERROR("Not able to allocate memory for Buffer Manager\n");
    }
#if DUMPS_ENABLE
    bfd = open("/data/bitstreamdump.dat", O_RDWR|O_CREAT, 0666);
    if (bfd < 0) {
        DEBUG_PRINT_ERROR("bistream file create failure\n");
    }
    pfd = open("/data/pcmdump.dat", O_RDWR|O_CREAT, 0666);
    if (pfd < 0) {
        DEBUG_PRINT_ERROR("pcm dump file create failure\n");
    }
#endif
    return;
}

/*=============================================================================
FUNCTION:
~omx_wma_adec

DESCRIPTION:
Destructor

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
omx_wma_adec::~omx_wma_adec()
{
    DEBUG_PRINT("INSIDE DESTRUCTOR comp-deinit=%d\n",m_comp_deinit);

    if ( m_comp_deinit == 0 )
        deinit_decoder();

    if (m_tmp_in_meta_buf) {
        free(m_tmp_in_meta_buf);
	m_tmp_in_meta_buf = NULL;
    }
    if (m_tmp_out_meta_buf) {
        free(m_tmp_out_meta_buf);
        m_tmp_out_meta_buf = NULL;
    }
    if(m_trans_buffer_start)
    {
       free(m_trans_buffer_start);
       m_trans_buffer_start= NULL;
    }

    pthread_mutexattr_destroy(&m_lock_attr);
    pthread_mutex_destroy(&m_lock);
    pthread_mutexattr_destroy(&m_suspendresume_lock_attr);
    pthread_mutex_destroy(&m_suspendresume_lock);

    pthread_mutexattr_destroy(&m_commandlock_attr);
    pthread_mutex_destroy(&m_commandlock);

    pthread_mutexattr_destroy(&m_outputlock_attr);
    pthread_mutex_destroy(&m_outputlock);

    pthread_mutexattr_destroy(&m_state_attr);
    pthread_mutex_destroy(&m_state_lock);
    pthread_mutexattr_destroy(&m_in_th_attr_1);
    pthread_mutexattr_destroy(&m_out_th_attr_1);
    pthread_mutexattr_destroy(&m_event_attr);
    pthread_mutex_destroy(&m_event_lock);

    pthread_mutexattr_destroy(&m_flush_attr);
    pthread_mutex_destroy(&m_flush_lock);

    pthread_mutexattr_destroy(&m_in_th_attr);
    pthread_mutex_destroy(&m_in_th_lock);

    pthread_mutexattr_destroy(&m_out_th_attr);
    pthread_mutex_destroy(&m_out_th_lock);

    pthread_mutex_destroy(&m_in_th_lock_1);
    pthread_mutex_destroy(&m_out_th_lock_1);

    pthread_mutex_destroy(&out_buf_count_lock);
    pthread_mutex_destroy(&in_buf_count_lock);
    pthread_mutex_destroy(&m_WaitForSuspendCmpl_lock);
    pthread_mutexattr_destroy(&m_WaitForSuspendCmpl_lock_attr);
    pthread_mutexattr_destroy(&out_buf_count_lock_attr);
    pthread_mutexattr_destroy(&in_buf_count_lock_attr);
    pthread_cond_destroy(&cond);
    pthread_cond_destroy(&in_cond);
    pthread_cond_destroy(&out_cond);
    sem_destroy (&sem_read_msg);
    sem_destroy (&sem_write_msg);
    sem_destroy (&sem_States);
    sem_destroy (&sem_WaitForSuspendCmpl_states);
DEBUG_PRINT_ERROR("WMA component destroyed\n");
    return;
}

/*=============================================================================
FUNCTION:
buffer_done_cb

DESCRIPTION:
function for sending EmptyBufferDone event back to IL client

INPUT/OUTPUT PARAMETERS:
[INOUT] bufHdr

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::buffer_done_cb(OMX_BUFFERHEADERTYPE *bufHdr)
{
    if(m_cb.EmptyBufferDone)
    {
        bufHdr->nFilledLen = 0;
        m_cb.EmptyBufferDone(&m_cmp, m_app_data, bufHdr);
        pthread_mutex_lock(&in_buf_count_lock);
        nNumInputBuf--;
        DEBUG_PRINT("EBD:bufHdr[0x%x] NumInBuf[%d] TS[%ld]\n",\
            (int)bufHdr,nNumInputBuf,(long int)bufHdr->nTimeStamp);
        pthread_mutex_unlock(&in_buf_count_lock);
    }
    return;
}

/*=============================================================================
FUNCTION:
flush_ack

DESCRIPTION:


INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::flush_ack()
{
    // Decrement the FLUSH ACK count and notify the waiting recepients
    pthread_mutex_lock(&m_flush_lock);
    --m_flush_cnt;
    if(0 == m_flush_cnt)
    {
        event_complete();
    }
    DEBUG_PRINT("Rxed FLUSH ACK cnt=%d\n",m_flush_cnt);
    pthread_mutex_unlock(&m_flush_lock);
}

/*=============================================================================
FUNCTION:
frame_done_cb

DESCRIPTION:


INPUT/OUTPUT PARAMETERS:
[INOUT] bufHdr

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::frame_done_cb(OMX_BUFFERHEADERTYPE *bufHdr)
{

        if(m_cb.FillBufferDone)
        {
            pthread_mutex_lock(&out_buf_count_lock);
            fbd_cnt++;
            nNumOutputBuf--;
            DEBUG_PRINT("FBD:Cnt[%d] NumOutBuf[%d]\n",fbd_cnt,nNumOutputBuf);
            pthread_mutex_unlock(&out_buf_count_lock);
            m_cb.FillBufferDone(&m_cmp, m_app_data, bufHdr);
        }
        PrintFrameHdr(bufHdr);
        return;

}

/*=============================================================================
FUNCTION:
process_out_port_msg

DESCRIPTION:
Function for handling all commands from IL client
IL client commands are processed and callbacks are generated through
this routine  Audio Command Server provides the thread context for this routine

INPUT/OUTPUT PARAMETERS:
[INOUT] client_data
[IN] id

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::process_out_port_msg(void *client_data, unsigned char id)
{
    unsigned      p1; // Parameter - 1
    unsigned      p2; // Parameter - 2
    unsigned      ident;
    unsigned      qsize     = 0; // qsize
    unsigned      tot_qsize = 0;
    omx_wma_adec  *pThis    = (omx_wma_adec *) client_data;
    OMX_STATETYPE state;

loopback_out:
    pthread_mutex_lock(&pThis->m_state_lock);
    pThis->get_state(&pThis->m_cmp, &state);
    pthread_mutex_unlock(&pThis->m_state_lock);
    if ( state == OMX_StateLoaded )
    {
        DEBUG_PRINT(" OUT: IN LOADED STATE RETURN\n");
        return;
    }
    pthread_mutex_lock(&pThis->m_outputlock);

    qsize = pThis->m_output_ctrl_cmd_q.m_size;
    tot_qsize = pThis->m_output_ctrl_cmd_q.m_size;
    tot_qsize += pThis->m_output_ctrl_fbd_q.m_size;
    tot_qsize += pThis->m_output_q.m_size;

    if ( 0 == tot_qsize )
    {
        pthread_mutex_unlock(&pThis->m_outputlock);
        DEBUG_DETAIL("OUT-->BREAK FROM LOOP...%d\n",tot_qsize);
        return;
    }
    if ( ((state != OMX_StateExecuting) || !pThis->bOutputPortReEnabled) && !qsize )
    {
        pthread_mutex_unlock(&pThis->m_outputlock);
        pthread_mutex_lock(&pThis->m_state_lock);
        pThis->get_state(&pThis->m_cmp, &state);
        pthread_mutex_unlock(&pThis->m_state_lock);
        if ( state == OMX_StateLoaded )
            return;

        DEBUG_DETAIL("OUT:1.SLEEPING OUT THREAD\n");
        pthread_mutex_lock(&pThis->m_out_th_lock_1);
        pThis->is_out_th_sleep = true;
        pthread_mutex_unlock(&pThis->m_out_th_lock_1);
        pThis->out_th_goto_sleep();

        /* Get the updated state */
        pthread_mutex_lock(&pThis->m_state_lock);
        pThis->get_state(&pThis->m_cmp, &state);
        pthread_mutex_unlock(&pThis->m_state_lock);
    }

    if ( ((!pThis->m_output_ctrl_cmd_q.m_size) && !pThis->m_out_bEnabled) )
    {
        // case where no port reconfig and nothing in the flush q
        DEBUG_DETAIL("No flush/port reconfig qsize=%d tot_qsize=%d",\
            qsize,tot_qsize);
        pthread_mutex_unlock(&pThis->m_outputlock);
        pthread_mutex_lock(&pThis->m_state_lock);
        pThis->get_state(&pThis->m_cmp, &state);
        pthread_mutex_unlock(&pThis->m_state_lock);
        if ( state == OMX_StateLoaded )
            return;

        if(pThis->m_output_ctrl_cmd_q.m_size || !(pThis->bFlushinprogress))
        {
        DEBUG_PRINT("OUT:2. SLEEPING OUT THREAD \n");
        pthread_mutex_lock(&pThis->m_out_th_lock_1);
        pThis->is_out_th_sleep = true;
        pthread_mutex_unlock(&pThis->m_out_th_lock_1);
        pThis->out_th_goto_sleep();
        }
        /* Get the updated state */
        pthread_mutex_lock(&pThis->m_state_lock);
        pThis->get_state(&pThis->m_cmp, &state);
        pthread_mutex_unlock(&pThis->m_state_lock);

        //return;
    }
    else if(state == OMX_StatePause)
    {
        DEBUG_PRINT ("\n OUT Thread in the pause state");
        if(!(pThis->m_output_ctrl_cmd_q.m_size))
        {

           // to take care of some race condition, where the output
           // thread is working on the old state;
           pThis->get_state(&pThis->m_cmp, &state);
           DEBUG_PRINT("OUT: pause state =%d m_pause_to_exe=%d\n",state,pThis->m_pause_to_exe);


            if (state == OMX_StatePause && !pThis->m_pause_to_exe )
            {

                DEBUG_DETAIL("OUT: SLEEPING OUT THREAD\n");
                pthread_mutex_lock(&pThis->m_out_th_lock_1);
                pThis->is_out_th_sleep = true;
                pthread_mutex_unlock(&pThis->m_out_th_lock_1);
                pThis->out_th_goto_sleep();
            }
            else
              DEBUG_PRINT("OUT--> In pause if() check, but now state changed\n");
        }
    }

    qsize = pThis->m_output_ctrl_cmd_q.m_size;
    tot_qsize = pThis->m_output_ctrl_cmd_q.m_size;
    tot_qsize += pThis->m_output_ctrl_fbd_q.m_size;
    tot_qsize += pThis->m_output_q.m_size;
    pthread_mutex_lock(&pThis->m_state_lock);
    pThis->get_state(&pThis->m_cmp, &state);
    pthread_mutex_unlock(&pThis->m_state_lock);
    DEBUG_DETAIL("OUT-->QSIZE-flush=%d,fbd=%d QSIZE=%d state=%d\n",\
        pThis->m_output_ctrl_cmd_q.m_size,
        pThis->m_output_ctrl_fbd_q.m_size,
        pThis->m_output_q.m_size,state);

    if ( qsize )
    {
        // process FLUSH message
        pThis->m_output_ctrl_cmd_q.pop_entry(&p1,&p2,&ident);
    } else if ( (qsize = pThis->m_output_ctrl_fbd_q.m_size) &&
        (pThis->bOutputPortReEnabled) && (pThis->m_out_bEnabled) &&
	(state == OMX_StateExecuting) )
    {
        // then process EBD's
        pThis->m_output_ctrl_fbd_q.pop_entry(&p1,&p2,&ident);
    } else if ( (qsize = pThis->m_output_q.m_size) &&
        (pThis->bOutputPortReEnabled) && (pThis->m_out_bEnabled) &&
	(state == OMX_StateExecuting) )
    {
        // if no FLUSH and FBD's then process FTB's
        pThis->m_output_q.pop_entry(&p1,&p2,&ident);
    } else if ( state == OMX_StateLoaded )
    {
        pthread_mutex_unlock(&pThis->m_outputlock);
        DEBUG_PRINT("IN: ***in OMX_StateLoaded so exiting\n");
        return ;
    } else
    {
        qsize = 0;
        DEBUG_PRINT("OUT--> Empty Queue state=%d %d %d %d\n",state,
                     pThis->m_output_ctrl_cmd_q.m_size,pThis->m_output_ctrl_fbd_q.m_size,
                     pThis->m_output_q.m_size);

        if(state == OMX_StatePause)
        {
            DEBUG_DETAIL("OUT: SLEEPING AGAIN OUT THREAD\n");
            pthread_mutex_lock(&pThis->m_out_th_lock_1);
            pThis->is_out_th_sleep = true;
            pthread_mutex_unlock(&pThis->m_out_th_lock_1);
            pthread_mutex_unlock(&pThis->m_outputlock);
            pThis->out_th_goto_sleep();
            goto loopback_out;
        }
    }
    pthread_mutex_unlock(&pThis->m_outputlock);

    if ( qsize > 0 )
    {
        id = ident;
        ident = 0;
        DEBUG_DETAIL("OUT->state[%d]ident[%d]flushq[%d]fbd[%d]dataq[%d]\n",\
            pThis->m_state,
            ident,
            pThis->m_output_ctrl_cmd_q.m_size,
            pThis->m_output_ctrl_fbd_q.m_size,
            pThis->m_output_q.m_size);

        if ( OMX_COMPONENT_GENERATE_FRAME_DONE == id )
        {
            pThis->frame_done_cb((OMX_BUFFERHEADERTYPE *)p2);
        } else if ( OMX_COMPONENT_GENERATE_FTB == id )
        {
            pThis->fill_this_buffer_proxy((OMX_HANDLETYPE)p1,
                (OMX_BUFFERHEADERTYPE *)p2);
        } else if ( OMX_COMPONENT_GENERATE_EOS == id )
        {
            pThis->m_cb.EventHandler(&pThis->m_cmp,
                pThis->m_app_data,
                OMX_EventBufferFlag,
                1, 1, NULL );
        }
        else if(id == OMX_COMPONENT_SUSPEND)
        {
            DEBUG_PRINT("OUT: Rxed SUSPEND event_eos_flag=%d\n",pThis->m_eos_bm);
            if( pThis->m_eos_bm != IP_OP_PORT_BITMASK)
            {
               pThis->append_data_to_temp_buf();
            }
        }
        else if(id == OMX_COMPONENT_RESUME)
        {
             DEBUG_PRINT("RESUMED...\n");
        }
        else if(id == OMX_COMPONENT_GENERATE_COMMAND)
        {
            // Execute FLUSH command
            if ( OMX_CommandFlush == p1 )
            {
                DEBUG_DETAIL("Executing FLUSH command on Output port\n");
                pThis->execute_output_omx_flush();
            } else
            {
                DEBUG_DETAIL("Invalid command[%d]\n",p1);
            }
        } else
        {
            DEBUG_PRINT_ERROR("ERROR:OUT-->Invalid Id[%d]\n",id);
        }
    } else
    {
        DEBUG_DETAIL("ERROR: OUT--> Empty OUTPUTQ\n");
    }

    return;
}

/*=============================================================================
FUNCTION:
process_command_msg

DESCRIPTION:


INPUT/OUTPUT PARAMETERS:
[INOUT] client_data
[IN] id

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::process_command_msg(void *client_data, unsigned char id)
{
    unsigned     p1;         // Parameter - 1
    unsigned     p2;         // Parameter - 2
    unsigned     ident;
    unsigned     qsize  = 0;
    OMX_STATETYPE state;
    omx_wma_adec *pThis = (omx_wma_adec*)client_data;
    pthread_mutex_lock(&pThis->m_commandlock);

    qsize = pThis->m_command_q.m_size;
    DEBUG_DETAIL("CMD-->QSIZE=%d state=%d\n",pThis->m_command_q.m_size,
        pThis->m_state);

    if(!qsize)
    {
        DEBUG_DETAIL("CMD-->BREAKING FROM LOOP\n");
        pthread_mutex_unlock(&pThis->m_commandlock);
        return;
    } else
    {
        pThis->m_command_q.pop_entry(&p1,&p2,&ident);
    }
    pthread_mutex_unlock(&pThis->m_commandlock);

    id = ident;
    DEBUG_DETAIL("CMD->state[%d]id[%d]cmdq[%d]n",\
        pThis->m_state,ident, \
        pThis->m_command_q.m_size);

    if(OMX_COMPONENT_GENERATE_EVENT == id)
    {
        if ( pThis->m_cb.EventHandler )
        {
            if (OMX_CommandStateSet == p1)
            {
                pthread_mutex_lock(&pThis->m_state_lock);
                pThis->m_state = (OMX_STATETYPE) p2;
                pthread_mutex_unlock(&pThis->m_state_lock);
                DEBUG_PRINT("CMD:Process->state set to %d \n", \
                    pThis->m_state);

                if ( pThis->m_state == OMX_StateExecuting ||
                    pThis->m_state == OMX_StateLoaded )
                {
                    pthread_mutex_lock(&pThis->m_in_th_lock_1);
                    if ( pThis->is_in_th_sleep )
                    {
                        pThis->is_in_th_sleep = false;
                        DEBUG_DETAIL("CMD:WAKING UP IN THREADS\n");
                        pThis->in_th_wakeup();
                    }
                    pthread_mutex_unlock(&pThis->m_in_th_lock_1);

                    pthread_mutex_lock(&pThis->m_out_th_lock_1);
                    if ( pThis->is_out_th_sleep )
                    {
                        DEBUG_DETAIL("CMD:WAKING UP OUT THREADS\n");
                        pThis->is_out_th_sleep = false;
                        pThis->out_th_wakeup();
                    }
                    pthread_mutex_unlock(&pThis->m_out_th_lock_1);
                    if((pThis->m_state == OMX_StateExecuting))
                      pThis->m_pause_to_exe=false;
                }
            }
            if ( OMX_StateInvalid == pThis->m_state )
            {
                pThis->m_cb.EventHandler(&pThis->m_cmp,
                    pThis->m_app_data,
                    OMX_EventError,
                    OMX_ErrorInvalidState,
                    0, NULL );
            }
            else if ((OMX_ERRORTYPE)p2 == OMX_ErrorPortUnpopulated)
            {
                pThis->m_cb.EventHandler(&pThis->m_cmp,
                    pThis->m_app_data,
                    OMX_EventError,
                    p2,
                    NULL,
                    NULL );
            } else
            {
                pThis->m_cb.EventHandler(&pThis->m_cmp,
                    pThis->m_app_data,
                    OMX_EventCmdComplete,
                    p1, p2, NULL );
            }
        } else
        {
            DEBUG_PRINT_ERROR("ERROR:CMD-->EventHandler NULL \n");
        }
    } else if ( OMX_COMPONENT_GENERATE_COMMAND == id )
    {
        pThis->send_command_proxy(&pThis->m_cmp,
            (OMX_COMMANDTYPE)p1,
            (OMX_U32)p2,(OMX_PTR)NULL);
    } else if ( OMX_COMPONENT_PORTSETTINGS_CHANGED == id )
    {
        DEBUG_DETAIL("CMD-->RXED PORTSETTINGS_CHANGED");
        pThis->m_cb.EventHandler(&pThis->m_cmp,
            pThis->m_app_data,
            OMX_EventPortSettingsChanged,
            1, 0, NULL );
    }
    else if(OMX_COMPONENT_SUSPEND == id)
    {
        if (!pThis->m_first)
        {
            DEBUG_PRINT("DONT PROCESS SUSPEND EVENT, PLAYBACK NOT STARTED\n");
            return;
        }

        pthread_mutex_lock(&pThis->m_state_lock);
        pThis->get_state(&pThis->m_cmp, &state);
        pthread_mutex_unlock(&pThis->m_state_lock);
        if (state != OMX_StatePause) {
             DEBUG_PRINT("DONT PROCESS SUSPEND EVENT, COMPONENT NOT IN PAUSED STATE\n");
             return;
        }

        pthread_mutex_lock(&pThis->m_suspendresume_lock);
        if (pThis->getSuspendFlg())
        {
            pthread_mutex_unlock(&pThis->m_suspendresume_lock);
            DEBUG_PRINT("DO NOT PROCESS SUSPEND EVENT, ALREADY IN SUSPEND MODE\n");
            return;
        }

        if(pThis->m_pause_to_exe)
        {
              pthread_mutex_unlock(&pThis->m_suspendresume_lock);
              DEBUG_PRINT("DONT PROCESS SUSPEND EVENT, COMPONENT MOVING FROM PAUSED TO EXE STATE\n");
              return;
        }
        pThis->setSuspendFlg();
        pthread_mutex_unlock(&pThis->m_suspendresume_lock);

        DEBUG_PRINT("CMD-->Suspend event rxed suspendflag=%d \n",\
                            pThis->getSuspendFlg());

        // signal the output thread to process suspend
        pThis->post_output(0,0,OMX_COMPONENT_SUSPEND);
        //signal the input thread to process suspend
        pThis->post_input(0,0,OMX_COMPONENT_SUSPEND);
        pthread_mutex_lock(&pThis->m_in_th_lock_1);
        if(pThis->is_in_th_sleep)
        {
                DEBUG_DETAIL("CMD:WAKING UP IN THREADS\n");
                pThis->in_th_wakeup();
                pThis->is_in_th_sleep = false;
        }
        pthread_mutex_unlock(&pThis->m_in_th_lock_1);
        pthread_mutex_lock(&pThis->m_out_th_lock_1);
        if(pThis->is_out_th_sleep)
        {
            DEBUG_DETAIL("CMD:WAKING UP OUT THREADS\n");
            pThis->out_th_wakeup();
            pThis->is_out_th_sleep = false;
        }
        pthread_mutex_unlock(&pThis->m_out_th_lock_1);
    }
    else if(id == OMX_COMPONENT_RESUME)
    {
        // signal the output thread that RESUME has happened
        pthread_mutex_lock(&pThis->m_suspendresume_lock);
        pThis->setResumeFlg();
        pthread_mutex_unlock(&pThis->m_suspendresume_lock);
        pThis->post_output(0,0,OMX_COMPONENT_RESUME);

        if (pThis->getWaitForSuspendCmplFlg())
        {
            DEBUG_PRINT_ERROR("Release P-->Executing context to IL client.\n");
            pThis->release_wait_for_suspend();
        }

        pthread_mutex_lock(&pThis->m_out_th_lock_1);
        if(pThis->is_out_th_sleep)
        {
            DEBUG_DETAIL("CMD:WAKING UP OUT THREADS\n");
            pThis->out_th_wakeup();
            pThis->is_out_th_sleep = false;
        }
        pthread_mutex_unlock(&pThis->m_out_th_lock_1);
    }
    else
    {
       DEBUG_PRINT_ERROR("CMD->state[%d]id[%d]\n",pThis->m_state,ident);
    }
    return;
}

/*=============================================================================
FUNCTION:
process_in_port_msg

DESCRIPTION:


INPUT/OUTPUT PARAMETERS:
[INOUT] client_data
[IN] id

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::process_in_port_msg(void *client_data, unsigned char id)
{
    unsigned      p1;       // Parameter - 1
    unsigned      p2;       // Parameter - 2
    unsigned      ident;
    unsigned      qsize     = 0;
    unsigned      tot_qsize = 0;
    omx_wma_adec  *pThis    = (omx_wma_adec *) client_data;
    OMX_STATETYPE state;

    if ( !pThis )
    {
        DEBUG_PRINT_ERROR("ERROR:IN--> Invalid Obj \n");
        return;
    }
loopback_in:
    pthread_mutex_lock(&pThis->m_state_lock);
    pThis->get_state(&pThis->m_cmp, &state);
    pthread_mutex_unlock(&pThis->m_state_lock);
    if ( state == OMX_StateLoaded )
    {
        DEBUG_PRINT(" IN: IN LOADED STATE RETURN\n");
        return;
    }
    pthread_mutex_lock(&pThis->m_lock);

    qsize = pThis->m_input_ctrl_cmd_q.m_size;
    tot_qsize = qsize;
    tot_qsize += pThis->m_input_ctrl_ebd_q.m_size;
    tot_qsize += pThis->m_input_q.m_size;

    if ( 0 == tot_qsize )
    {
        DEBUG_DETAIL("IN-->BREAKING FROM IN LOOP");
        pthread_mutex_unlock(&pThis->m_lock);
        return;
    }



    if ( (state != OMX_StateExecuting) && ! (pThis->m_input_ctrl_cmd_q.m_size))
    {
        pthread_mutex_unlock(&pThis->m_lock);
        DEBUG_DETAIL("SLEEPING IN THREAD\n");
        pthread_mutex_lock(&pThis->m_in_th_lock_1);
        pThis->is_in_th_sleep = true;
        pthread_mutex_unlock(&pThis->m_in_th_lock_1);
        pThis->in_th_goto_sleep();

        /* Get the updated state */
        pthread_mutex_lock(&pThis->m_state_lock);
        pThis->get_state(&pThis->m_cmp, &state);
        pthread_mutex_unlock(&pThis->m_state_lock);
    }
    else if ((state == OMX_StatePause))
    {
        if(!(pThis->m_input_ctrl_cmd_q.m_size))
        {
           pthread_mutex_unlock(&pThis->m_lock);

           DEBUG_DETAIL("IN: SLEEPING IN THREAD\n");
           pthread_mutex_lock(&pThis->m_in_th_lock_1);
           pThis->is_in_th_sleep = true;
           pthread_mutex_unlock(&pThis->m_in_th_lock_1);
           pThis->in_th_goto_sleep();

           pthread_mutex_lock(&pThis->m_state_lock);
           pThis->get_state(&pThis->m_cmp, &state);
           pthread_mutex_unlock(&pThis->m_state_lock);
        }
    }

    qsize = pThis->m_input_ctrl_cmd_q.m_size;
    tot_qsize = qsize;
    tot_qsize += pThis->m_input_ctrl_ebd_q.m_size;
    tot_qsize += pThis->m_input_q.m_size;

    DEBUG_DETAIL("Input-->QSIZE-flush=%d,ebd=%d QSIZE=%d state=%d\n",\
        pThis->m_input_ctrl_cmd_q.m_size,
        pThis->m_input_ctrl_ebd_q.m_size,
        pThis->m_input_q.m_size, state);



    if(qsize)
    {
        // process FLUSH message
        pThis->m_input_ctrl_cmd_q.pop_entry(&p1,&p2,&ident);
    } else if ( (qsize = pThis->m_input_ctrl_ebd_q.m_size) &&
        (state == OMX_StateExecuting) )
    {
        // then process EBD's
        pThis->m_input_ctrl_ebd_q.pop_entry(&p1,&p2,&ident);
    } else if ( (qsize = pThis->m_input_q.m_size) &&
        (state == OMX_StateExecuting) && !(pThis->m_bufMgr->getBufFilledSpace()))
    {
        // if no FLUSH and EBD's then process ETB's
        pThis->m_input_q.pop_entry(&p1, &p2, &ident);
    } else if ( state == OMX_StateLoaded )
    {
        pthread_mutex_unlock(&pThis->m_lock);
        DEBUG_PRINT("IN: ***in OMX_StateLoaded so exiting\n");
        return ;
    } else
    {
        qsize = 0;
        DEBUG_PRINT("IN-->state[%d]cmdq[%d]ebdq[%d]in[%d]temp_buf[%lu]\n",\
                             state,pThis->m_input_ctrl_cmd_q.m_size,
                             pThis->m_input_ctrl_ebd_q.m_size,pThis->m_input_q.m_size,
                             pThis->m_bufMgr->getBufFilledSpace());

        if(state == OMX_StatePause || pThis->m_bufMgr->getBufFilledSpace())
        {
            DEBUG_DETAIL("IN: SLEEPING AGAIN IN THREAD\n");
            pthread_mutex_lock(&pThis->m_in_th_lock_1);
            pThis->is_in_th_sleep = true;
            pthread_mutex_unlock(&pThis->m_in_th_lock_1);
            pthread_mutex_unlock(&pThis->m_lock);
            pThis->in_th_goto_sleep();
            goto loopback_in;
        }
    }
    pthread_mutex_unlock(&pThis->m_lock);

    if(qsize > 0)
    {
        id = ident;
        DEBUG_DETAIL("Input->state[%d]id[%d]flushq[%d]ebdq[%d]dataq[%d]\n",\
            pThis->m_state,
            ident,
            pThis->m_input_ctrl_cmd_q.m_size,
            pThis->m_input_ctrl_ebd_q.m_size,
            pThis->m_input_q.m_size);

        if(OMX_COMPONENT_GENERATE_BUFFER_DONE == id)
        {
            pThis->buffer_done_cb((OMX_BUFFERHEADERTYPE *)p2);
        }
        else if(id == OMX_COMPONENT_GENERATE_EOS)
        {
            pThis->m_cb.EventHandler(&pThis->m_cmp, pThis->m_app_data,
                OMX_EventBufferFlag, 0, 1, NULL );
        } else if ( OMX_COMPONENT_GENERATE_ETB == id )
        {
            pThis->empty_this_buffer_proxy((OMX_HANDLETYPE)p1,
                (OMX_BUFFERHEADERTYPE *)p2);
        } else if ( OMX_COMPONENT_GENERATE_COMMAND == id )
        {
            // Execute FLUSH command
            if(OMX_CommandFlush == p1)
            {
                DEBUG_DETAIL(" Executing FLUSH command on Input port\n");
                pThis->execute_input_omx_flush();
            } else
            {
                DEBUG_DETAIL("Invalid command[%d]\n",p1);
            }
        }
        else if(id == OMX_COMPONENT_SUSPEND)
        {
            DEBUG_PRINT("IN:FAKING EOS TO KERNEL : m_eos_bm=%d", pThis->m_eos_bm);
                // dont trigger fake EOS when actual EOS has already been sent or
                // when resume flag is set
            if(!(pThis->m_eos_bm & IP_OP_PORT_BITMASK) &&
                (pThis->getSuspendFlg() && !pThis->getResumeFlg()))
            {
                pThis->omx_wma_fake_eos();
            }
            else if((pThis->m_eos_bm & IP_OP_PORT_BITMASK) ==
                                                   IP_OP_PORT_BITMASK)
            {
                pThis->setSuspendFlg();
                pThis->setResumeFlg();
                //EOS already reached, dont trigger suspend/resume,
                // but set the flag so that one more suspend/event doesnt
                // get triggered from timeout/driver
                DEBUG_PRINT("IN--> AUDIO_STOP %d %d \n",
                              pThis->getSuspendFlg(),
                              pThis->getResumeFlg());
                ioctl(pThis->m_drv_fd, AUDIO_STOP, 0);
                if (pThis->getWaitForSuspendCmplFlg())
                {
                    DEBUG_PRINT_ERROR("Release P-->Executing context to IL client.\n");
                    pThis->release_wait_for_suspend();
                }
            }
            else
            {
                // suspend event after input eos sent
                DEBUG_PRINT("IN--> Do nothing, m_eos_bm=%d\n",pThis->m_eos_bm);
            }
        }
        else
        {
            DEBUG_PRINT_ERROR("ERROR:IN-->Invalid Id[%d]\n",id);
        }
    } else
    {
        DEBUG_DETAIL("ERROR:IN-->Empty INPUT Q\n");
    }
    return;
}

/*=============================================================================
FUNCTION:
component_init

DESCRIPTION:
Function for performing component initialization

INPUT/OUTPUT PARAMETERS:
[IN] role

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE omx_wma_adec::component_init(OMX_STRING role)
{

    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    /* Ignore role */

    m_state                   = OMX_StateLoaded;
    memset(&m_adec_param, 0, sizeof(m_adec_param));
    m_adec_param.nSize = sizeof(m_adec_param);
    m_volume = 25; /* Close to unity gain */
    m_adec_param.nChannels = 1;
    m_pause_to_exe     = false;
    m_eos_bm         =0x00;
    m_first = 0;
    bTsupdate = 0;
    m_timer->resetTimerExpiry();
    memset(&m_adec_param_wma10Pro, 0, sizeof(m_adec_param_wma10Pro));
    m_adec_param_wma10Pro.nSize = sizeof(m_adec_param_wma10Pro);
    m_pcm_param.nChannels = DEFAULT_CHANNEL_MODE;
    m_pcm_param.eNumData = OMX_NumericalDataSigned;
    m_pcm_param.bInterleaved = OMX_TRUE;
    m_pcm_param.nBitPerSample = 16;
    m_pcm_param.nSamplingRate = DEFAULT_SAMPLING_RATE;
    m_pcm_param.ePCMMode = OMX_AUDIO_PCMModeLinear;
    m_pcm_param.eChannelMapping[0] = OMX_AUDIO_ChannelLF;
    m_pcm_param.eChannelMapping[1] = OMX_AUDIO_ChannelRF;
    frameDuration = 0;
    fbd_cnt = 0;
    m_comp_deinit=0;
    nTimestamp = 0;
    wma_packet_timestamp = 0;
    DEBUG_PRINT(" Enabling Non-Tunnel mode \n");
    pcm_feedback = 1;    /* by default enable non-tunnel mode */
    ntotal_playtime = 0xffffffff;
    nNumInputBuf = 0;
    nNumOutputBuf = 0;
    bEOSSent = 0;
    set_pcm_config = 0;
    m_trans_buf_cnt = 0; // making number of transcoding buffer count to 0
    bBufferAfterFlush = FALSE;
    m_ipc_to_in_th = NULL;  // Command server instance
    m_ipc_to_out_th = NULL;  // Client server instance
    m_ipc_to_cmd_th = NULL;  // command instance

    m_is_out_th_sleep = 0;
    m_is_in_th_sleep = 0;
    is_out_th_sleep= false;
    suspensionPolicy= OMX_SuspensionDisabled;
    is_in_th_sleep=false;
    bOutputPortReEnabled = false;
    bSendWMAPacketpending = false;


    resetSuspendFlg();
    resetResumeFlg();;
    fake_eos_recieved = false;

    m_trans_buffer_length = 0;
    m_dwSpillOverBitsLength = 0;
    m_notfirstblock = FALSE;
    m_adec_param.nBlockAlign = 8000;/*Default value to set to pass khronos test*/
    m_adec_param_wma10Pro.nBlockAlign = 8000;
    memset(&m_priority_mgm, 0, sizeof(m_priority_mgm));
    m_priority_mgm.nGroupID =0;
    m_priority_mgm.nGroupPriority=0;
    packet_num = 0;
    memset(&m_buffer_supplier, 0, sizeof(m_buffer_supplier));
    m_buffer_supplier.nPortIndex=OMX_BufferSupplyUnspecified;


    DEBUG_PRINT_ERROR(" component init: role = %s\n",role);
    if(!strcmp(role,"OMX.qcom.audio.decoder.wma"))
    {
        pcm_feedback = 1;
        wma10Pro = 0;
        DEBUG_PRINT("\ncomponent_init: Component %s LOADED \n", role);
    }
    else if(!strcmp(role,"OMX.qcom.audio.decoder.tunneled.wma"))
    {
        pcm_feedback = 0;
        wma10Pro = 0;
        DEBUG_PRINT("\ncomponent_init: Component %s LOADED \n", role);
    }
    else if(!strcmp(role,"OMX.qcom.audio.decoder.wma10Pro"))
    {
        pcm_feedback = 1;
        wma10Pro = 1;
        DEBUG_PRINT("\ncomponent_init: Component %s LOADED \n", role);
    }
    else if(!strcmp(role,"OMX.qcom.audio.decoder.tunneled.wma10Pro"))
    {
        pcm_feedback = 0;
        wma10Pro = 1;
        DEBUG_PRINT("\ncomponent_init: Component %s LOADED \n", role);
    }
    else
    {
        DEBUG_PRINT("\ncomponent_init: Component %s LOADED is invalid\n", role);
    }
    if(wma10Pro)
    {
        if(0 == pcm_feedback)
        {
            m_drv_fd = open("/dev/msm_wmapro",O_WRONLY);
        }
        else
        {
            m_drv_fd = open("/dev/msm_wmapro",O_RDWR);
        }
    }
    else
    {
        if(0 == pcm_feedback)
        {
            m_drv_fd = open("/dev/msm_wma",O_WRONLY);
        }
        else
        {
            m_drv_fd = open("/dev/msm_wma",O_RDWR);
        }
    }
    if (m_drv_fd < 0)
    {
        DEBUG_PRINT_ERROR("Component_init-->Dev Open Failed[%d]",m_drv_fd);
        return OMX_ErrorInsufficientResources;
    }
    if(ioctl(m_drv_fd, AUDIO_GET_SESSION_ID,&m_session_id) == -1)
    {
       DEBUG_PRINT("AUDIO_GET_SESSION_ID FAILED\n");
    }
    if(!m_ipc_to_in_th)
    {
        m_ipc_to_in_th = omx_wma_thread_create(process_in_port_msg, this,
            (char *)"INPUT_THREAD");
        if(!m_ipc_to_in_th)
        {
            DEBUG_PRINT_ERROR("ERROR!!! Failed to start Input port thread\n");
            return OMX_ErrorInsufficientResources;
        }
    }

    if(!m_ipc_to_cmd_th)
    {
        m_ipc_to_cmd_th = omx_wma_thread_create(process_command_msg, this,
            (char *)"CMD_THREAD");
        if(!m_ipc_to_cmd_th)
        {
            DEBUG_PRINT_ERROR("ERROR!!!Failed to start command message thread\n");
            return OMX_ErrorInsufficientResources;
        }
    }

    if(pcm_feedback)
    {
        if(!m_ipc_to_out_th)
        {
            m_ipc_to_out_th = omx_wma_thread_create(process_out_port_msg, this,
                (char *)"OUTPUT_THREAD");
            if(!m_ipc_to_out_th)
            {
                DEBUG_PRINT_ERROR("ERROR!!! Failed to start output port thread\n");
                return OMX_ErrorInsufficientResources;
            }
        }
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
get_component_version

DESCRIPTION:
Function to retrieve version of component

INPUT/OUTPUT PARAMETERS:
[IN] hComp
[OUT] componentName
[OUT] componentVersion
[OUT] specVersion
[OUT] componentUUID

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::get_component_version(
    OMX_IN OMX_HANDLETYPE               hComp,
    OMX_OUT OMX_STRING          componentName,
    OMX_OUT OMX_VERSIONTYPE* componentVersion,
    OMX_OUT OMX_VERSIONTYPE*      specVersion,
    OMX_OUT OMX_UUIDTYPE*       componentUUID)
{
    if((hComp == NULL) || (componentName == NULL) ||
        (componentVersion == NULL) || (specVersion == NULL) ||
        (componentUUID == NULL))
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Get Comp Version in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    /* TBD -- Return the proper version */
    return OMX_ErrorNone;
}

/*=============================================================================
FUNCTION:
send_command

DESCRIPTION:
Function that handles command from IL client
This function simply queue up commands from IL client.
Commands will be processed in command server thread context later

INPUT/OUTPUT PARAMETERS:
[IN] hComp
[IN] cmd
[IN] param1
[IN] cmdData

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::send_command(OMX_IN OMX_HANDLETYPE hComp,
                                          OMX_IN OMX_COMMANDTYPE  cmd,
                                          OMX_IN OMX_U32       param1,
                                          OMX_IN OMX_PTR      cmdData)
{

    int portIndex = (int)param1;

    if(hComp == NULL)
    {
        cmdData = NULL;
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }

    if(OMX_StateInvalid == m_state)
    {
        return OMX_ErrorInvalidState;
    }

    if ( (cmd == OMX_CommandFlush) && (portIndex > 1) )
    {
        return OMX_ErrorBadPortIndex;
    }
    if((m_state == OMX_StatePause) )
    {
        DEBUG_PRINT("Send Command-->State=%d cmd=%d param1=%lu sus=%d res=%d\n",
                             m_state,cmd,param1,getSuspendFlg(),getResumeFlg());
        if((cmd == OMX_CommandStateSet) )
        {
            pthread_mutex_lock(&m_suspendresume_lock);
            if((OMX_STATETYPE)param1 == OMX_StateExecuting)
            {
                DEBUG_PRINT("SCP: changing flag state m_pause_to_exe=%d\n",m_pause_to_exe);
                m_pause_to_exe = true;
            }

            if(getSuspendFlg() && !getResumeFlg())
            {
                DEBUG_PRINT_ERROR("Send Command, waiting for suspend/resume procedure to complete\n");
                pthread_mutex_unlock(&m_suspendresume_lock);
                wait_for_suspend_cmpl();
            }
            else
                pthread_mutex_unlock(&m_suspendresume_lock);
        }
    }
    post_command((unsigned)cmd,(unsigned)param1,OMX_COMPONENT_GENERATE_COMMAND);

    DEBUG_PRINT_ERROR("Send Command : returns with OMX_ErrorNone \n");
    DEBUG_PRINT("send_command : recieved state before semwait= %d\n", (int)param1);
    sem_wait (&sem_States);
    DEBUG_PRINT("send_command : recieved state after semwait\n");
    return OMX_ErrorNone;
}

/*=============================================================================
FUNCTION:
send_command_proxy

DESCRIPTION:
Function that performs actual processing of commands excluding
empty buffer call

INPUT/OUTPUT PARAMETERS:
[IN] hComp
[IN] cmd
[IN] param1
[IN] cmdData

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::send_command_proxy(OMX_IN OMX_HANDLETYPE hComp,
                                                OMX_IN OMX_COMMANDTYPE  cmd,
                                                OMX_IN OMX_U32       param1,
                                                OMX_IN OMX_PTR      cmdData)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    //   Handle only IDLE and executing
    OMX_STATETYPE eState = (OMX_STATETYPE) param1;
    int rc = 0;
    int bFlag = 1;
    nState = eState;

    DEBUG_PRINT("Inside send_command_proxy: The current state is %d\n", m_state);
    DEBUG_PRINT("Inside send_command_proxy: The eState is %d\n", eState);
    if(hComp == NULL)
    {
        cmdData = NULL;
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(OMX_CommandStateSet == cmd)
    {
        /***************************/
        /* Current State is Loaded */
        /***************************/
        if(OMX_StateLoaded == m_state)
        {
            if(OMX_StateIdle == eState)
            {

                if (allocate_done() ||(m_inp_bEnabled
                    == OMX_FALSE && m_out_bEnabled == OMX_FALSE))
                {
                    DEBUG_PRINT("SCP-->Loaded->Idle\n");
                }
                else
                {
                    DEBUG_PRINT("SCP-->Loaded to Idle-Pending\n");
                    BITMASK_SET(&m_flags, OMX_COMPONENT_IDLE_PENDING);
                    bFlag = 0;
                }
            }
            else if(eState == OMX_StateLoaded)
            {
                DEBUG_PRINT("OMXCORE-SM: Loaded-->Loaded\n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorSameState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorSameState;
            }

            else if(eState == OMX_StateWaitForResources)
            {
                DEBUG_PRINT("OMXCORE-SM: Loaded-->WaitForResources\n");
                eRet = OMX_ErrorNone;
            }

            else if(eState == OMX_StateExecuting)
            {
                DEBUG_PRINT("OMXCORE-SM: Loaded-->Executing\n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorIncorrectStateTransition,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorIncorrectStateTransition;
            }

            else if(eState == OMX_StatePause)
            {
                DEBUG_PRINT("OMXCORE-SM: Loaded-->Pause\n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorIncorrectStateTransition,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorIncorrectStateTransition;
            }

            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT("OMXCORE-SM: Loaded-->Invalid\n");
                m_state = OMX_StateInvalid;
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorInvalidState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorInvalidState;
            }
            else
            {
                DEBUG_PRINT_ERROR("SCP-->Loaded to Invalid(%d))\n",eState);
                eRet = OMX_ErrorBadParameter;
            }
        }

        /***************************/
        /* Current State is IDLE */
        /***************************/
        else if(OMX_StateIdle == m_state)
        {
            if(OMX_StateLoaded == eState)
            {
                if ( release_done(-1) )
                {
                  if(suspensionPolicy == OMX_SuspensionEnabled)
                  {
                      ioctl(m_drv_fd,AUDIO_ABORT_GET_EVENT,NULL);
                  }
                    if ( ioctl(m_drv_fd, AUDIO_STOP, 0) == -1 )
                    {
                        DEBUG_PRINT("SCP:Idle->Loaded,ioctl stop failed \n");
                    }
		    if(m_trans_buffer_start)
		    {
                        free(m_trans_buffer_start);
                        m_trans_buffer_start= NULL;
		    }

                    DEBUG_PRINT("SCP-->Idle to Loaded\n");
                }
                else
                {
                    DEBUG_PRINT("SCP--> Idle to Loaded-Pending\n");
                    BITMASK_SET(&m_flags, OMX_COMPONENT_LOADING_PENDING);
                    // Skip the event notification
                    bFlag = 0;
                }
            }
            else if(OMX_StateExecuting == eState)
            {
                DEBUG_PRINT("CREATING EVENT THREAD -->GNG TO EXE STATE suspensionPolicy=%d \
                             m_ipc_to_event_th=%p",suspensionPolicy ,m_ipc_to_event_th);
                if((suspensionPolicy == OMX_SuspensionEnabled) && !m_ipc_to_event_th)
                {
                  m_ipc_to_event_th = omx_wma_event_thread_create(
                                            process_event_cb, this,
                                            (char *)"EVENT_THREAD");
                  if(!m_ipc_to_event_th)
                  {
                      DEBUG_PRINT_ERROR("ERROR!!! EVENT THREAD failed to get created\n");
                      sem_post (&sem_States);
                      return OMX_ErrorHardware;
                  }
              }

                struct msm_audio_pcm_config  pcm_config;
                struct msm_audio_config      config;
                OMX_S16 data_thresh;
                ioctl(m_drv_fd, AUDIO_GET_CONFIG, &config);
                if(pcm_feedback)
                {
                    config.meta_field = 1;
                }
                else
                {
                    config.meta_field = 0;
                }
            if(!wma10Pro)
            {
                config.sample_rate = (OMX_U16)m_adec_param.nSamplingRate;
                config.channel_count = (OMX_U16)m_adec_param.nChannels;
		DEBUG_PRINT("PCM configuration: sample_rate = %d channel_count = %d\n",
                           config.sample_rate, config.channel_count);
                struct msm_audio_wma_config drv_config;
                ioctl(m_drv_fd, AUDIO_SET_CONFIG, &config);
                ioctl(m_drv_fd, AUDIO_GET_WMA_CONFIG, &drv_config);
                drv_config.wmasamplingfreq =  (OMX_U16)m_adec_param.nSamplingRate;
                drv_config.channelsdecoded =(OMX_U16)m_adec_param.nChannels;
                drv_config.wmabytespersec =(OMX_U16)(m_adec_param.nBitRate/8);
                data_thresh =
                        WMA_DATA_THR_MAX - (OMX_U16)( m_adec_param.nBlockAlign/ 2) - WMA_DATA_THR_OFFSET;
                if (data_thresh < WMA_DATA_REQUEST_THRESHOLD)
                {
                    data_thresh = WMA_DATA_REQUEST_THRESHOLD;
                }
                drv_config.armdatareqthr  = data_thresh;
                drv_config.wmaencoderopts = m_adec_param.nEncodeOptions;
                ioctl(m_drv_fd, AUDIO_SET_WMA_CONFIG, &drv_config);
            }
            else
            {
                config.sample_rate = (OMX_U16)m_adec_param_wma10Pro.nSamplingRate;
                config.channel_count = (OMX_U16)m_adec_param_wma10Pro.nChannels;
		DEBUG_PRINT("PCM configuration: sample_rate = %d channel_count = %d\n",
                           config.sample_rate, config.channel_count);
                struct msm_audio_wmapro_config drv_config;
                ioctl(m_drv_fd, AUDIO_SET_CONFIG, &config);
                ioctl(m_drv_fd, AUDIO_GET_WMAPRO_CONFIG, &drv_config);
                drv_config.samplingrate =  (OMX_U16)m_adec_param_wma10Pro.nSamplingRate;
                drv_config.numchannels =(OMX_U16)m_adec_param_wma10Pro.nChannels;
                drv_config.avgbytespersecond =(OMX_U16)(m_adec_param_wma10Pro.nBitRate/8);
                data_thresh =
                    WMA_DATA_THR_MAX - (OMX_U16)( m_adec_param_wma10Pro.nBlockAlign/ 2) - WMA_DATA_THR_OFFSET;
                if (data_thresh < WMA_DATA_REQUEST_THRESHOLD)
                {
                    data_thresh = WMA_DATA_REQUEST_THRESHOLD;
                }
                drv_config.armdatareqthr  = data_thresh;
                drv_config.encodeopt = m_adec_param_wma10Pro.nEncodeOptions;
                drv_config.validbitspersample = m_adec_param_wma10Pro.validBitsPerSample;
                drv_config.formattag = m_adec_param_wma10Pro.formatTag;
                if(drv_config.numchannels == STEREO)
                {
                    drv_config.channelmask = 0x03;
                }
                else if(drv_config.numchannels == MONO)
                {
                    drv_config.channelmask = 0x04;
                }
                drv_config.asfpacketlength =  m_adec_param_wma10Pro.nVirtualPktSize;
                drv_config.advancedencodeopt = m_adec_param_wma10Pro.advancedEncodeOpt;
                drv_config.advancedencodeopt2 = m_adec_param_wma10Pro.advancedEncodeOpt2;
                ioctl(m_drv_fd, AUDIO_SET_WMAPRO_CONFIG, &drv_config);
            }
                DEBUG_PRINT(" configure driver mode as %d \n",pcm_feedback);
                ioctl(m_drv_fd, AUDIO_GET_PCM_CONFIG, &pcm_config);
                pcm_config.pcm_feedback = pcm_feedback;
                pcm_config.buffer_size  = output_buffer_size + sizeof(META_OUT);
                ioctl(m_drv_fd, AUDIO_SET_PCM_CONFIG, &pcm_config);

                pthread_mutex_lock(&m_suspendresume_lock);
                resetSuspendFlg();
                resetResumeFlg();
                pthread_mutex_unlock(&m_suspendresume_lock);
                DEBUG_PRINT("SCP-->Idle to Executing\n");
                nState = eState;
            }
            else if(eState == OMX_StateIdle)
            {
                DEBUG_PRINT("OMXCORE-SM: Idle-->Idle\n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorSameState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorSameState;
            }

            else if(eState == OMX_StateWaitForResources)
            {
                DEBUG_PRINT("OMXCORE-SM: Idle-->WaitForResources\n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorIncorrectStateTransition,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorIncorrectStateTransition;
            }

            else if(eState == OMX_StatePause)
            {
                DEBUG_PRINT("OMXCORE-SM: Idle-->Pause\n");
            }

            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT("OMXCORE-SM: Idle-->Invalid\n");
                m_state = OMX_StateInvalid;
                this->m_cb.EventHandler(&this->m_cmp,
                    this->m_app_data, OMX_EventError, OMX_ErrorInvalidState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorInvalidState;
            }
            else
            {
                DEBUG_PRINT_ERROR("SCP--> Idle to %d Not Handled\n",eState);
                eRet = OMX_ErrorBadParameter;
            }
        }

        /******************************/
        /* Current State is Executing */
        /******************************/
        else if(OMX_StateExecuting == m_state)
        {
            if(OMX_StateIdle == eState)
            {
                DEBUG_PRINT("SCP-->Executing to Idle \n");
                if ( 0 == pcm_feedback )
                {
                    execute_omx_flush(0,false);
                } else
                {
                    execute_omx_flush(-1,false); // Flush all ports
                }
            } else if ( OMX_StatePause == eState )
            {
                DEBUG_DETAIL("*************************\n");
                DEBUG_PRINT("SCP-->RXED PAUSE STATE\n");
                DEBUG_DETAIL("*************************\n");
                //ioctl(m_drv_fd, AUDIO_PAUSE, 0);
                DEBUG_PRINT("SCP:E-->P, start timer\n");
                getTimerInst()->startTimer();
            } else if ( eState == OMX_StateLoaded )
            {
                DEBUG_PRINT("\n OMXCORE-SM: Executing --> Loaded \n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorIncorrectStateTransition,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            else if(eState == OMX_StateWaitForResources)
            {
                DEBUG_PRINT("\n OMXCORE-SM: Executing --> WaitForResources \n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorIncorrectStateTransition,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            else if(eState == OMX_StateExecuting)
            {
                DEBUG_PRINT("\n OMXCORE-SM: Executing --> Executing \n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorSameState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorSameState;
            }
            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT("\n OMXCORE-SM: Executing --> Invalid \n");
                m_state = OMX_StateInvalid;
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorInvalidState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorInvalidState;
            }
            else
            {
                DEBUG_PRINT_ERROR("SCP--> Executing to %d Not Handled\n",eState);
                eRet = OMX_ErrorBadParameter;
            }
        }
        /***************************/
        /* Current State is Pause  */
        /***************************/
        else if(OMX_StatePause == m_state)
        {
            DEBUG_PRINT("SCP: Paused --> Executing %d %d in=%d out=%d\n",\
                             getSuspendFlg(),getResumeFlg(),
                             is_in_th_sleep,is_out_th_sleep);
            if(!getTimerInst()->getTimerExpiry())
            {
                getTimerInst()->stopTimer();
            }
            getTimerInst()->resetTimerExpiry();

            if( (eState == OMX_StateExecuting || eState == OMX_StateIdle) )
            {
                pthread_mutex_lock(&m_out_th_lock_1);
                if(is_out_th_sleep)
                {
                    DEBUG_DETAIL("PE: WAKING UP OUT THREAD\n");
                    out_th_wakeup();
                    is_out_th_sleep = false;
                }
                pthread_mutex_unlock(&m_out_th_lock_1);
            }
            if(OMX_StateExecuting == eState)
            {
                nState = eState;
                pthread_mutex_lock(&m_suspendresume_lock);
                if(getSuspendFlg())
                    setResumeFlg();
                DEBUG_PRINT("SCP: Paused --> Executing %d %d\n",\
                            getSuspendFlg(),getResumeFlg());
                if(getSuspendFlg() && getResumeFlg())
                {
                    post_output(0,0,OMX_COMPONENT_RESUME);
                    rc = ioctl(m_drv_fd, AUDIO_START, 0);
                    if(rc <0)
                    {
                      DEBUG_PRINT_ERROR("AUDIO_START FAILED\n");
                      post_command((unsigned)OMX_CommandStateSet,
                             (unsigned)OMX_StateInvalid,
                              OMX_COMPONENT_GENERATE_COMMAND);
                      pthread_mutex_unlock(&m_suspendresume_lock);
                      execute_omx_flush(-1,false);
                      return OMX_ErrorInvalidState;

                    }
                    resetSuspendFlg();
                    resetResumeFlg();
                }
                pthread_mutex_unlock(&m_suspendresume_lock);
            } else if ( OMX_StateIdle == eState )
            {
                DEBUG_PRINT("SCP-->Paused to Idle \n");
                pthread_mutex_lock(&m_flush_lock);
                m_flush_cnt = 2;
                pthread_mutex_unlock(&m_flush_lock);
                if ( 0 == pcm_feedback )
                {
                    execute_omx_flush(0,false);
                } else
                {
                    execute_omx_flush(-1,false); // Flush all ports
                }
            } else if ( eState == OMX_StateLoaded )
            {
                DEBUG_PRINT("\n Pause --> loaded \n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorIncorrectStateTransition,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            else if(eState == OMX_StateWaitForResources)
            {
                DEBUG_PRINT("\n Pause --> WaitForResources \n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorIncorrectStateTransition,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            else if(eState == OMX_StatePause)
            {
                DEBUG_PRINT("\n Pause --> Pause \n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorSameState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorSameState;
            }
            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT("\n Pause --> Invalid \n");
                m_state = OMX_StateInvalid;
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorInvalidState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorInvalidState;
            }
            else
            {
                DEBUG_PRINT("SCP-->Paused to %d Not Handled\n",eState);
                eRet = OMX_ErrorBadParameter;
            }
        }
        /**************************************/
        /* Current State is WaitForResources  */
        /**************************************/
        else if(m_state == OMX_StateWaitForResources)
        {
            if(eState == OMX_StateLoaded)
            {
                DEBUG_PRINT("OMXCORE-SM: WaitForResources-->Loaded\n");
            }
            else if(eState == OMX_StateWaitForResources)
            {
                DEBUG_PRINT("OMXCORE-SM: WaitForResources-->WaitForResources\n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorSameState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorSameState;
            }
            else if(eState == OMX_StateExecuting)
            {
                DEBUG_PRINT("OMXCORE-SM: WaitForResources-->Executing\n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorIncorrectStateTransition,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            else if(eState == OMX_StatePause)
            {
                DEBUG_PRINT("OMXCORE-SM: WaitForResources-->Pause\n");
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorIncorrectStateTransition,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorIncorrectStateTransition;
            }
            else if(eState == OMX_StateInvalid)
            {
                DEBUG_PRINT("OMXCORE-SM: WaitForResources-->Invalid\n");
                m_state = OMX_StateInvalid;
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorInvalidState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorInvalidState;
            }
            else
            {
                DEBUG_PRINT_ERROR("SCP--> %d to %d(Not Handled)\n",m_state,eState);
                eRet = OMX_ErrorBadParameter;
            }
        }
        /****************************/
        /* Current State is Invalid */
        /****************************/
        else if(m_state == OMX_StateInvalid)
        {
            if(OMX_StateLoaded == eState || OMX_StateWaitForResources == eState
                || OMX_StateIdle == eState || OMX_StateExecuting == eState
                || OMX_StatePause == eState || OMX_StateInvalid == eState)
            {
                DEBUG_PRINT("OMXCORE-SM: Invalid-->Loaded/Idle/Executing/Pause/\
                                                Invalid/WaitForResources\n");
                m_state = OMX_StateInvalid;
                this->m_cb.EventHandler(&this->m_cmp, this->m_app_data,
                    OMX_EventError, OMX_ErrorInvalidState,
                    OMX_COMPONENT_GENERATE_EVENT, NULL );
                eRet = OMX_ErrorInvalidState;
            }
        }
        else
        {
            DEBUG_PRINT_ERROR("OMXCORE-SM: %d --> %d(Not Handled)\n",m_state,eState);
            eRet = OMX_ErrorBadParameter;
        }

    }
    else if (OMX_CommandFlush == cmd)
    {
        DEBUG_DETAIL("*************************\n");
        DEBUG_PRINT("SCP-->RXED FLUSH COMMAND port=%d\n", (int)param1);
        DEBUG_DETAIL("*************************\n");
        bFlag = 0;
        if ( param1 == OMX_CORE_INPUT_PORT_INDEX ||
            param1 == OMX_CORE_OUTPUT_PORT_INDEX ||
            (int)param1 == -1 )
        {
            execute_omx_flush(param1);
        } else
        {
            eRet = OMX_ErrorBadPortIndex;
            m_cb.EventHandler(&m_cmp, m_app_data, OMX_EventError,
                OMX_CommandFlush, OMX_ErrorBadPortIndex, NULL );
        }
    } else if ( cmd == OMX_CommandPortDisable )
    {
	// Skip the event notification
        bFlag = 0;
        DEBUG_PRINT("Disable - param1 - portIndex= %d\n", (int)param1);

        if(param1 == OMX_CORE_INPUT_PORT_INDEX || param1 == OMX_ALL)
        {
            DEBUG_PRINT("SCP: Disabling Input port Indx\n");
            m_inp_bEnabled = OMX_FALSE;
            if ( (m_state == OMX_StateLoaded || m_state == OMX_StateIdle)
                && release_done(0) )
            {
                DEBUG_PRINT("send_command_proxy:OMX_CommandPortDisable:\
                            OMX_CORE_INPUT_PORT_INDEX:release_done \n");
                DEBUG_PRINT("************* OMX_CommandPortDisable:\
                            m_inp_bEnabled - %d ********\n",m_inp_bEnabled);

                post_command(OMX_CommandPortDisable,
                    OMX_CORE_INPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
            }

            else
            {
                if(m_state == OMX_StatePause ||m_state == OMX_StateExecuting)
                {
                    DEBUG_PRINT("SCP: execute_omx_flush in Disable in  param1=%d \
                                 m_state=%d \n", (int)param1, m_state);
                    execute_omx_flush(param1, false);
                }
                DEBUG_PRINT("send_command_proxy:OMX_CommandPortDisable:\
                            OMX_CORE_INPUT_PORT_INDEX \n");
                BITMASK_SET(&m_flags, OMX_COMPONENT_INPUT_DISABLE_PENDING);
            }

        }
        if (param1 == OMX_CORE_OUTPUT_PORT_INDEX || param1 == OMX_ALL)
        {
            DEBUG_PRINT("SCP: Disabling Output port Indx\n");
            m_out_bEnabled = OMX_FALSE;
            bOutputPortReEnabled = false;
            if ( (m_state == OMX_StateLoaded || m_state == OMX_StateIdle)
                && release_done(1) )
            {
                DEBUG_PRINT("send_command_proxy:OMX_CommandPortDisable:\
                            OMX_CORE_OUTPUT_PORT_INDEX:release_done \n");
                DEBUG_PRINT("************* OMX_CommandPortDisable:\
                            m_out_bEnabled - %d********\n", m_inp_bEnabled);

                post_command(OMX_CommandPortDisable,
                    OMX_CORE_OUTPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT
                    );
            }

            else
            {
                if(m_state == OMX_StatePause ||m_state == OMX_StateExecuting)
                {
                    DEBUG_PRINT("SCP: execute_omx_flush in Disable out param1=%d \
                               m_state=%d \n", (int)param1, m_state);
                    execute_omx_flush(param1, false);
                }
                BITMASK_SET(&m_flags, OMX_COMPONENT_OUTPUT_DISABLE_PENDING);
            }
        }
        else
        {
            DEBUG_PRINT_ERROR("OMX_CommandPortDisable: disable wrong port ID");
            eRet = OMX_ErrorBadPortIndex;
        }
    }
    else if (OMX_CommandPortEnable == cmd)
    {
	// Skip the event notification
        bFlag = 0;
        DEBUG_PRINT("Enable - param1 - portIndex= %d\n", (int)param1);
        if (param1 == OMX_CORE_INPUT_PORT_INDEX  || param1 == OMX_ALL)
        {
            m_inp_bEnabled = OMX_TRUE;
            DEBUG_PRINT("SCP: Enabling Input port Indx\n");
            if((m_state == OMX_StateLoaded
                && !BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING))
                || (m_state == OMX_StateWaitForResources)
                || (m_inp_bPopulated == OMX_TRUE))
            {
                post_command(OMX_CommandPortEnable,
                    OMX_CORE_INPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
            } else
            {
                BITMASK_SET(&m_flags, OMX_COMPONENT_INPUT_ENABLE_PENDING);
            }
        }
        if (param1 == OMX_CORE_OUTPUT_PORT_INDEX || param1 == OMX_ALL)
        {
            DEBUG_PRINT("SCP: Enabling Output port Indx\n");
            m_out_bEnabled = OMX_TRUE;
            if((m_state == OMX_StateLoaded
                && !BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING))
                || (m_state == OMX_StateWaitForResources)
                || (m_out_bPopulated == OMX_TRUE))
            {
                bOutputPortReEnabled = 1;
                post_command(OMX_CommandPortEnable,
                    OMX_CORE_OUTPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT );

            } else
            {
                DEBUG_PRINT("send_command_proxy:OMX_CommandPortEnable:\
                            OMX_CORE_OUTPUT_PORT_INDEX:release_done \n");
                BITMASK_SET(&m_flags, OMX_COMPONENT_OUTPUT_ENABLE_PENDING);
            }
            pthread_mutex_lock(&m_in_th_lock_1);
            if(is_in_th_sleep)
            {
                    is_in_th_sleep = false;
                    DEBUG_DETAIL("SCP:WAKING UP IN THREADS\n");
                    in_th_wakeup();
            }
            pthread_mutex_unlock(&m_in_th_lock_1);
            pthread_mutex_lock(&m_out_th_lock_1);
            if ( is_out_th_sleep )
            {
                is_out_th_sleep = false;
                DEBUG_PRINT("SCP:WAKING OUT THR, OMX_CommandPortEnable\n");
                out_th_wakeup();
            }
            pthread_mutex_unlock(&m_out_th_lock_1);
        } else
        {
            DEBUG_PRINT_ERROR("OMX_CommandPortEnable: disable wrong port ID");
        }

    }
    else
    {
        DEBUG_PRINT_ERROR("SCP-->ERROR: Invali Command [%d]\n",cmd);
        eRet = OMX_ErrorNotImplemented;
    }
    DEBUG_PRINT("posting sem_States\n");
    sem_post (&sem_States);
    if ( eRet == OMX_ErrorNone && bFlag )
    {
        post_command(cmd,eState,OMX_COMPONENT_GENERATE_EVENT);
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
execute_omx_flush

DESCRIPTION:
Function that flushes buffers that are pending to be written to driver

INPUT/OUTPUT PARAMETERS:
[IN] param1
[IN] cmd_cmpl

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::execute_omx_flush(OMX_IN OMX_U32 param1, bool cmd_cmpl)
{
    bool bRet = true;

    DEBUG_PRINT("Execute_omx_flush Port[%d]", (int)param1);
    struct timespec abs_timeout;
    abs_timeout.tv_sec = 1;
    abs_timeout.tv_nsec = 0; //333333;
    if (-1 == (signed)param1)
    {
        bFlushinprogress = true;
        DEBUG_PRINT("Execute flush for both I/p O/p port\n");
        pthread_mutex_lock(&m_flush_lock);
        m_flush_cnt = 2;
        pthread_mutex_unlock(&m_flush_lock);

        // Send Flush commands to input and output threads
        post_input(OMX_CommandFlush,
            OMX_CORE_INPUT_PORT_INDEX,OMX_COMPONENT_GENERATE_COMMAND);
        post_output(OMX_CommandFlush,
            OMX_CORE_OUTPUT_PORT_INDEX,OMX_COMPONENT_GENERATE_COMMAND);
        // Send Flush to the kernel so that the in and out buffers are released
        if ( ioctl( m_drv_fd, AUDIO_FLUSH, 0) == -1 )
            DEBUG_PRINT("FLush:ioctl flush failed \n");
        DEBUG_DETAIL("****************************************");
        DEBUG_DETAIL("is_in_th_sleep=%d is_out_th_sleep=%d\n",\
            is_in_th_sleep,is_out_th_sleep);
        DEBUG_DETAIL("****************************************");
        pthread_mutex_lock(&m_in_th_lock_1);
        if ( is_in_th_sleep )
        {
            is_in_th_sleep = false;
            DEBUG_DETAIL("For FLUSH-->WAKING UP IN THREADS\n");
            in_th_wakeup();
        }
        pthread_mutex_unlock(&m_in_th_lock_1);

        pthread_mutex_lock(&m_out_th_lock_1);
        if(is_out_th_sleep)
        {
            is_out_th_sleep = false;
            DEBUG_DETAIL("For FLUSH-->WAKING UP OUT THREADS\n");
            out_th_wakeup();
        }
        pthread_mutex_unlock(&m_out_th_lock_1);

        // sleep till the FLUSH ACK are done by both the input and output threads
        DEBUG_DETAIL("WAITING FOR FLUSH ACK's param1 = %d", (int)param1);

        while ( 1 )
        {
            pthread_mutex_lock(&out_buf_count_lock);
            pthread_mutex_lock(&in_buf_count_lock);
            DEBUG_PRINT("Flush:nNumOutputBuf = %d nNumInputBuf=%d\n",\
                nNumOutputBuf,nNumInputBuf);
            if ( nNumOutputBuf > 0 || nNumInputBuf > 0 )
            {
                pthread_mutex_unlock(&in_buf_count_lock);
                pthread_mutex_unlock(&out_buf_count_lock);
                pthread_mutex_lock(&m_in_th_lock_1);
                if(is_in_th_sleep)
                {
                    is_in_th_sleep = false;
                    DEBUG_DETAIL("FLUSH-->WAKING UP IN THREAD\n");
                    in_th_wakeup();
                }
                pthread_mutex_unlock(&m_in_th_lock_1);

                pthread_mutex_lock(&m_out_th_lock_1);
                if ( is_out_th_sleep )
                {
                    is_out_th_sleep = false;
                    DEBUG_DETAIL("FLUSH-->WAKING UP OUT THREAD\n");
                    out_th_wakeup();
                }
                pthread_mutex_unlock(&m_out_th_lock_1);
                DEBUG_PRINT(" READ FLUSH PENDING HENCE WAIT\n");
                DEBUG_PRINT("BEFORE READ ioctl_flush\n");
                usleep (10000);
                if ( ioctl( m_drv_fd, AUDIO_FLUSH, 0) == -1 )
                    DEBUG_PRINT("Flush: ioctl flush failed \n");
                DEBUG_PRINT("AFTER READ ioctl_flush\n");
                sem_timedwait(&sem_read_msg,&abs_timeout);
                DEBUG_PRINT("AFTER READ SEM_TIMEWAIT\n");
            } else
            {
                pthread_mutex_unlock(&in_buf_count_lock);
                pthread_mutex_unlock(&out_buf_count_lock);
                break;
            }
        }
        wait_for_event();
        DEBUG_PRINT("RECIEVED BOTH FLUSH ACK's param1=%d cmd_cmpl=%d",\
            (int)param1,cmd_cmpl);
        memset(m_trans_buffer, 0x0, m_trans_buffer_length);
        m_trans_buffer = m_trans_buffer_start;
        m_trans_buffer_length = 0;
        packet_num = 0;
        bTsupdate = 0;
        bBufferAfterFlush = TRUE;

        // If not going to idle state, Send FLUSH complete message to the Client,
        // now that FLUSH ACK's have been recieved.
        if(cmd_cmpl)
        {
            m_cb.EventHandler(&m_cmp, m_app_data, OMX_EventCmdComplete,
                OMX_CommandFlush, OMX_CORE_INPUT_PORT_INDEX, NULL );
            m_cb.EventHandler(&m_cmp, m_app_data, OMX_EventCmdComplete,
                OMX_CommandFlush, OMX_CORE_OUTPUT_PORT_INDEX, NULL );
            DEBUG_PRINT("Inside FLUSH.. sending FLUSH CMPL\n");
        }
        bFlushinprogress = false;
        bEOSSent = 0; //Set EOS sent flag back to zero to indicate new session
    }
    else if (OMX_CORE_INPUT_PORT_INDEX == param1)
    {
        DEBUG_PRINT("Execute FLUSH for I/p port\n");
        pthread_mutex_lock(&m_flush_lock);
        m_flush_cnt = 1;
        pthread_mutex_unlock(&m_flush_lock);
        post_input(OMX_CommandFlush,
            OMX_CORE_INPUT_PORT_INDEX,OMX_COMPONENT_GENERATE_COMMAND);
        if ( ioctl( m_drv_fd, AUDIO_FLUSH, 0) == -1 )
            DEBUG_PRINT("Flush:Input port, ioctl flush failed \n");
        DEBUG_DETAIL("****************************************");
        DEBUG_DETAIL("is_in_th_sleep=%d is_out_th_sleep=%d\n",\
                     is_in_th_sleep,is_out_th_sleep);
        DEBUG_DETAIL("****************************************");

        if (is_in_th_sleep)
        {
            pthread_mutex_lock(&m_in_th_lock_1);
            is_in_th_sleep = false;
            pthread_mutex_unlock(&m_in_th_lock_1);
            DEBUG_DETAIL("For FLUSH-->WAKING UP IN THREADS\n");
            in_th_wakeup();
        }

        if (is_out_th_sleep)
        {
            pthread_mutex_lock(&m_out_th_lock_1);
            is_out_th_sleep = false;
            pthread_mutex_unlock(&m_out_th_lock_1);
            DEBUG_DETAIL("For FLUSH-->WAKING UP OUT THREADS\n");
            out_th_wakeup();
        }
        //sleep till the FLUSH ACK are done by both the input and output threads
        DEBUG_DETAIL("Executing FLUSH for I/p port\n");
        DEBUG_DETAIL("WAITING FOR FLUSH ACK's param1=%lu",param1);
        wait_for_event();
        DEBUG_DETAIL(" RECIEVED FLUSH ACK FOR I/P PORT param1=%lu",param1);
        memset(m_trans_buffer, 0x0, m_trans_buffer_length);
        m_trans_buffer = m_trans_buffer_start;
        m_trans_buffer_length = 0;
        packet_num = 0;
        bTsupdate = 0;
        bBufferAfterFlush = TRUE;
        // Send FLUSH complete message to the Client,
        // now that FLUSH ACK's have been recieved.
        if (cmd_cmpl)
        {
            m_cb.EventHandler(&m_cmp, m_app_data, OMX_EventCmdComplete,
                              OMX_CommandFlush, OMX_CORE_INPUT_PORT_INDEX, NULL );
        }
    } else if (OMX_CORE_OUTPUT_PORT_INDEX == param1)
    {
        DEBUG_PRINT("Executing FLUSH for O/p port\n");
        pthread_mutex_lock(&m_flush_lock);
        m_flush_cnt = 1;
        pthread_mutex_unlock(&m_flush_lock);
        DEBUG_DETAIL("Executing FLUSH for O/p port\n");
        DEBUG_DETAIL("WAITING FOR FLUSH ACK's param1=%lu",param1);
        post_output(OMX_CommandFlush,
                    OMX_CORE_OUTPUT_PORT_INDEX,OMX_COMPONENT_GENERATE_COMMAND);
        DEBUG_DETAIL("****************************************");
        DEBUG_DETAIL("is_in_th_sleep=%d is_out_th_sleep=%d\n",\
            is_in_th_sleep,is_out_th_sleep);
        DEBUG_DETAIL("****************************************");
        if ( is_in_th_sleep )
        {
            pthread_mutex_lock(&m_in_th_lock_1);
            is_in_th_sleep = false;
            pthread_mutex_unlock(&m_in_th_lock_1);
            DEBUG_DETAIL("For FLUSH-->WAKING UP IN THREADS\n");
            in_th_wakeup();
        }

        if ( is_out_th_sleep )
        {
            pthread_mutex_lock(&m_out_th_lock_1);
            is_out_th_sleep = false;
            pthread_mutex_unlock(&m_out_th_lock_1);
            DEBUG_DETAIL("For FLUSH-->WAKING UP OUT THREADS\n");
            out_th_wakeup();
        }
        // Send Flush to the kernel so that the in and out buffers are released
        // sleep till the FLUSH ACK are done by both the input and output threads
        DEBUG_DETAIL("Executing FLUSH for I/p port\n");
        DEBUG_DETAIL("WAITING FOR FLUSH ACK's param1 = %d", (int)param1);
        wait_for_event();
        DEBUG_DETAIL(" RECIEVED FLUSH ACK FOR I/P PORT param1 = %d", (int)param1);

        // Send FLUSH complete message to the Client,
        // now that FLUSH ACK's have been recieved.
        if(cmd_cmpl)//if not going to idle state, send FLUSH complete message to client
        {
            m_cb.EventHandler(&m_cmp, m_app_data, OMX_EventCmdComplete,
                OMX_CommandFlush, OMX_CORE_OUTPUT_PORT_INDEX, NULL );
        }
        bEOSSent = 0; //Set EOS sent flag back to zero to indicate new session
        DEBUG_DETAIL("RECIEVED FLUSH ACK FOR O/P PORT param1 = %d", (int)param1);
    }
    else
    {
        DEBUG_PRINT("Invalid Port ID[%d]", (int)param1);
    }
    return bRet;
}

/*=============================================================================
FUNCTION:
execute_input_omx_flush

DESCRIPTION:
Function that flushes buffers that are pending to be written to driver

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::execute_input_omx_flush()
{
    OMX_BUFFERHEADERTYPE *omx_buf;
    unsigned      p1; // Parameter - 1
    unsigned      p2; // Parameter - 2
    unsigned      ident;
    unsigned       qsize=0; // qsize
    unsigned       tot_qsize=0; // qsize

    DEBUG_PRINT("Execute_omx_flush on input port");

    pthread_mutex_lock(&m_lock);
    do
    {
        qsize = m_input_q.m_size;
        tot_qsize = qsize;
        tot_qsize += m_input_ctrl_ebd_q.m_size;

        DEBUG_DETAIL("Input FLUSH-->flushq[%d] ebd[%d]dataq[%d]"
            ,m_input_ctrl_cmd_q.m_size,m_input_ctrl_ebd_q.m_size,qsize);
        if(!tot_qsize)
        {
            DEBUG_DETAIL("Input-->BREAKING FROM execute_input_flush LOOP");
            break;
        }
        if (qsize)
        {
            m_input_q.pop_entry(&p1, &p2, &ident);
            if ((ident == OMX_COMPONENT_GENERATE_ETB) ||
                (ident == OMX_COMPONENT_GENERATE_BUFFER_DONE))
            {
                omx_buf = (OMX_BUFFERHEADERTYPE *) p2;
                DEBUG_DETAIL("Input Buf_Addr=%x \n", (int)omx_buf);
                omx_buf->nFilledLen = 0;
                buffer_done_cb((OMX_BUFFERHEADERTYPE *)omx_buf);
            }
        } else if ( (m_input_ctrl_ebd_q.m_size) )
        {
            m_input_ctrl_ebd_q.pop_entry(&p1, &p2, &ident);
            if(ident == OMX_COMPONENT_GENERATE_BUFFER_DONE)
            {
                omx_buf = (OMX_BUFFERHEADERTYPE *) p2;
                omx_buf->nFilledLen = 0;
                buffer_done_cb((OMX_BUFFERHEADERTYPE *)omx_buf);
            }
        } else
        {
        }
    }while ( tot_qsize>0 );

    if(!m_bufMgr->isEmpty())
    {
       DEBUG_DETAIL("OUT-->Flush TCXO Buffer\n");
       m_bufMgr->reset();
    }

    DEBUG_DETAIL("*************************\n");
    DEBUG_DETAIL("IN-->FLUSHING DONE\n");
    DEBUG_DETAIL("*************************\n");
    flush_ack();
    pthread_mutex_unlock(&m_lock);
    return true;
}

/*=============================================================================
FUNCTION:
execute_output_omx_flush

DESCRIPTION:
Function that flushes buffers that are pending to be written to driver

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::execute_output_omx_flush()
{
    OMX_BUFFERHEADERTYPE *omx_buf;
    unsigned      p1; // Parameter - 1
    unsigned      p2; // Parameter - 2
    unsigned      ident;
    unsigned       qsize=0; // qsize
    unsigned       tot_qsize=0; // qsize

    DEBUG_PRINT("Execute_omx_flush on output port");

    pthread_mutex_lock(&m_outputlock);
    do
    {
        qsize = m_output_q.m_size;
        DEBUG_DETAIL("OUT FLUSH-->flushq[%d] fbd[%d]dataq[%d]",\
            m_output_ctrl_cmd_q.m_size,
            m_output_ctrl_fbd_q.m_size,qsize);
        tot_qsize = qsize;
        tot_qsize += m_output_ctrl_fbd_q.m_size;
        if(!tot_qsize)
        {
            DEBUG_DETAIL("OUT-->BREAKING FROM execute_input_flush LOOP");
            break;
        }
        if (qsize)
        {
            m_output_q.pop_entry(&p1,&p2,&ident);
            if ( (OMX_COMPONENT_GENERATE_FTB == ident) ||
                (OMX_COMPONENT_GENERATE_FRAME_DONE == ident))
            {
                omx_buf = (OMX_BUFFERHEADERTYPE *) p2;
                DEBUG_DETAIL("Ouput Buf_Addr=%x \n", (int)omx_buf);
                omx_buf->nFilledLen = 0;
                frame_done_cb((OMX_BUFFERHEADERTYPE *)omx_buf);
                DEBUG_DETAIL("CALLING FBD FROM FLUSH");
            }
        }
        else if((qsize = m_output_ctrl_fbd_q.m_size))
        {
            m_output_ctrl_fbd_q.pop_entry(&p1, &p2, &ident);
            if(OMX_COMPONENT_GENERATE_FRAME_DONE == ident)
            {
                omx_buf = (OMX_BUFFERHEADERTYPE *) p2;
                omx_buf->nFilledLen = 0;
                frame_done_cb((OMX_BUFFERHEADERTYPE *)omx_buf);
                DEBUG_DETAIL("CALLING FROM CTRL-FBDQ FROM FLUSH");
            }
        }
    }while(qsize>0);
    DEBUG_DETAIL("*************************\n");
    DEBUG_DETAIL("OUT-->BEFORE CALLING FLUSH ACK\n");
    DEBUG_DETAIL("*************************\n");
    flush_ack();
    pthread_mutex_unlock(&m_outputlock);
    return true;
}

/*=============================================================================
FUNCTION:
post_input

DESCRIPTION:
Function that posts command in the command queue

INPUT/OUTPUT PARAMETERS:
[IN] p1
[IN] p2
[IN] id - command ID
[IN] lock - self-locking mode

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::post_input(unsigned int p1,
                              unsigned int p2,
                              unsigned int id
                              )
{
    bool bRet = false;
    pthread_mutex_lock(&m_lock);

    if((OMX_COMPONENT_GENERATE_COMMAND == id) || (id == OMX_COMPONENT_SUSPEND))
    {
        // insert flush message and ebd
        m_input_ctrl_cmd_q.insert_entry(p1,p2,id);
    } else if ( (OMX_COMPONENT_GENERATE_BUFFER_DONE == id) )
    {
        // insert ebd
        m_input_ctrl_ebd_q.insert_entry(p1,p2,id);
    } else
    {
        // ETBS in this queue
        m_input_q.insert_entry(p1,p2,id);
    }
    if ( m_ipc_to_in_th )
    {
        bRet = true;
        omx_wma_post_msg(m_ipc_to_in_th, id);
    }

    DEBUG_DETAIL("PostInput-->state[%d]id[%d]flushq[%d]ebdq[%d]dataq[%d] \n",\
        m_state,
        id,
        m_input_ctrl_cmd_q.m_size,
        m_input_ctrl_ebd_q.m_size,
        m_input_q.m_size);

    pthread_mutex_unlock(&m_lock);


    return bRet;
}

/*=============================================================================
FUNCTION:
post_command

DESCRIPTION:
Function that posts command in the command queue

INPUT/OUTPUT PARAMETERS:
[IN] p1
[IN] p2
[IN] id - command ID
[IN] lock - self-locking mode

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::post_command(unsigned int p1,
                                unsigned int p2,
                                unsigned int id
                                )
{
    bool bRet  = false;

    pthread_mutex_lock(&m_commandlock);

    m_command_q.insert_entry(p1,p2,id);

    if(m_ipc_to_cmd_th)
    {
        bRet = true;
        omx_wma_post_msg(m_ipc_to_cmd_th, id);
    }

    DEBUG_DETAIL("PostCmd-->state[%d]id[%d]cmdq[%d]flags[%x]\n",\
        m_state,
        id,
        m_command_q.m_size,
        m_flags >> 3);

    pthread_mutex_unlock(&m_commandlock);


    return bRet;
}

/*=============================================================================
FUNCTION:
post_output

DESCRIPTION:
Function that posts command in the command queue

INPUT/OUTPUT PARAMETERS:
[IN] p1
[IN] p2
[IN] id - command ID
[IN] lock - self-locking mode

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::post_output(unsigned int p1,
                               unsigned int p2,
                               unsigned int id
                               )
{
    bool bRet = false;

    pthread_mutex_lock(&m_outputlock);
    if((OMX_COMPONENT_GENERATE_COMMAND == id) || (id == OMX_COMPONENT_SUSPEND)
        || (id == OMX_COMPONENT_RESUME))
    {
        // insert flush message and fbd
        m_output_ctrl_cmd_q.insert_entry(p1,p2,id);
    } else if ( (OMX_COMPONENT_GENERATE_FRAME_DONE == id) )
    {
        // insert flush message and fbd
        m_output_ctrl_fbd_q.insert_entry(p1,p2,id);
    } else
    {
        m_output_q.insert_entry(p1,p2,id);
    }
    if ( m_ipc_to_out_th )
    {
        bRet = true;
        omx_wma_post_msg(m_ipc_to_out_th, id);
    }
    DEBUG_DETAIL("PostOutput-->state[%d]id[%d]flushq[%d]ebdq[%d]dataq[%d]\n",\
        m_state,
        id,
        m_output_ctrl_cmd_q.m_size,
        m_output_ctrl_fbd_q.m_size,
        m_output_q.m_size);

    pthread_mutex_unlock(&m_outputlock);


    return bRet;
}

/*=============================================================================
FUNCTION:
get_parameter

DESCRIPTION:
Function that return parameters to IL client

INPUT/OUTPUT PARAMETERS:
[INOUT] paramData
[IN] hComp
[IN] paramIndex

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::get_parameter(OMX_IN OMX_HANDLETYPE     hComp,
                                           OMX_IN OMX_INDEXTYPE paramIndex,
                                           OMX_INOUT OMX_PTR     paramData)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    if(hComp == NULL)
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Get Param in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    if(paramData == NULL)
    {
        DEBUG_PRINT("get_parameter: paramData is NULL\n");
        return OMX_ErrorBadParameter;
    }

    switch(paramIndex)
    {
    case OMX_IndexParamPortDefinition:
        {
            OMX_PARAM_PORTDEFINITIONTYPE *portDefn;
            portDefn = (OMX_PARAM_PORTDEFINITIONTYPE *) paramData;

            DEBUG_PRINT("OMX_IndexParamPortDefinition PortIndex = %d\n",\
                (int)portDefn->nPortIndex);

            portDefn->nVersion.nVersion = OMX_SPEC_VERSION;
            portDefn->nSize = sizeof(portDefn);
            portDefn->eDomain    = OMX_PortDomainAudio;

            if (0 == portDefn->nPortIndex)
            {
                portDefn->eDir       = OMX_DirInput;
                portDefn->bEnabled   = m_inp_bEnabled;
                portDefn->bPopulated = m_inp_bPopulated;
                portDefn->nBufferCountActual = m_inp_act_buf_count;
                portDefn->nBufferCountMin    = OMX_CORE_NUM_INPUT_BUFFERS;
                portDefn->nBufferSize        = input_buffer_size;
                portDefn->format.audio.bFlagErrorConcealment = OMX_TRUE;
                portDefn->format.audio.eEncoding = OMX_AUDIO_CodingWMA;
                portDefn->format.audio.pNativeRender = 0;
            }
            else if (1 == portDefn->nPortIndex)
            {
                portDefn->eDir =  OMX_DirOutput;
                portDefn->bEnabled   = m_out_bEnabled;
                portDefn->bPopulated = m_out_bPopulated;
                portDefn->nBufferCountActual = m_out_act_buf_count;
                portDefn->nBufferCountMin    = OMX_CORE_NUM_OUTPUT_BUFFERS;
                portDefn->nBufferSize        = output_buffer_size;
                portDefn->format.audio.bFlagErrorConcealment = OMX_TRUE;
                portDefn->format.audio.eEncoding = OMX_AUDIO_CodingPCM;
                portDefn->format.audio.pNativeRender = 0;
            }
            else
            {
                portDefn->eDir =  OMX_DirMax;
                DEBUG_PRINT_ERROR("Bad Port idx %d\n", (int)portDefn->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }

    case OMX_IndexParamAudioInit:
        {
            OMX_PORT_PARAM_TYPE *portParamType =
                (OMX_PORT_PARAM_TYPE *) paramData;
            DEBUG_PRINT("OMX_IndexParamAudioInit\n");

            portParamType->nVersion.nVersion = OMX_SPEC_VERSION;
            portParamType->nSize = sizeof(portParamType);
            portParamType->nPorts           = 2;
            portParamType->nStartPortNumber = 0;
            break;
        }

    case OMX_IndexParamAudioPortFormat:
        {
            OMX_AUDIO_PARAM_PORTFORMATTYPE *portFormatType =
                (OMX_AUDIO_PARAM_PORTFORMATTYPE *) paramData;
            DEBUG_PRINT("OMX_IndexParamAudioPortFormat\n");
            portFormatType->nVersion.nVersion = OMX_SPEC_VERSION;
            portFormatType->nSize = sizeof(portFormatType);

            if (OMX_CORE_INPUT_PORT_INDEX == portFormatType->nPortIndex)
            {
                portFormatType->eEncoding = OMX_AUDIO_CodingWMA;
            }
            else if(OMX_CORE_OUTPUT_PORT_INDEX== portFormatType->nPortIndex)
            {
                DEBUG_PRINT("get_parameter: OMX_IndexParamAudioFormat: %d\n",
                    (int)portFormatType->nIndex);
                portFormatType->eEncoding = OMX_AUDIO_CodingPCM;
            }
            else
            {
                DEBUG_PRINT_ERROR("get_parameter: Bad port index %d\n",
                    (int)portFormatType->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }

    case OMX_IndexParamAudioWma:
        {
            OMX_AUDIO_PARAM_WMATYPE *wmaParam =
                (OMX_AUDIO_PARAM_WMATYPE *) paramData;
            DEBUG_PRINT("OMX_IndexParamAudiowma\n");

            if (OMX_CORE_INPUT_PORT_INDEX== wmaParam->nPortIndex)
            {
                *wmaParam = m_adec_param;
            }
            else
            {
                DEBUG_PRINT_ERROR("get_parameter:OMX_IndexParamAudioWma \
                                  OMX_ErrorBadPortIndex %d\n", (int)wmaParam->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }
   case QOMX_IndexParamAudioWma10Pro:
        {
            QOMX_AUDIO_PARAM_WMA10PROTYPE *wmaParam10Pro =
                (QOMX_AUDIO_PARAM_WMA10PROTYPE *) paramData;
            DEBUG_PRINT("OMX_IndexParamAudiowma10Pro\n");

            if (OMX_CORE_INPUT_PORT_INDEX== wmaParam10Pro->nPortIndex)
            {
                *wmaParam10Pro = m_adec_param_wma10Pro;
            }
            else
            {
                DEBUG_PRINT_ERROR("get_parameter:OMX_IndexParamAudioWma10Pro \
                                  OMX_ErrorBadPortIndex %d\n", (int)wmaParam10Pro->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }

    case OMX_IndexParamAudioPcm:
        {
            OMX_AUDIO_PARAM_PCMMODETYPE *pcmparam =
                (OMX_AUDIO_PARAM_PCMMODETYPE *) paramData;

            if (OMX_CORE_OUTPUT_PORT_INDEX== pcmparam->nPortIndex)
            {
            if(!wma10Pro) {
               pcmparam->nSamplingRate = m_adec_param.nSamplingRate;
               pcmparam->nChannels = m_adec_param.nChannels;
            } else {
               pcmparam->nSamplingRate = m_adec_param_wma10Pro.nSamplingRate;
               pcmparam->nChannels = m_adec_param_wma10Pro.nChannels;
            }
                pcmparam->eNumData  =    m_pcm_param.eNumData;
                pcmparam->bInterleaved  = m_pcm_param.bInterleaved;
                pcmparam->nBitPerSample = m_pcm_param.nBitPerSample;
                pcmparam->ePCMMode = m_pcm_param.ePCMMode;
                pcmparam->eChannelMapping[0] = m_pcm_param.eChannelMapping[0];
                pcmparam->eChannelMapping[1] = m_pcm_param.eChannelMapping[1] ;
                DEBUG_PRINT("get_parameter: Sampling rate %d", (int)pcmparam->nSamplingRate);
                DEBUG_PRINT("get_parameter: Number of channels %d", (int)pcmparam->nChannels);
            }
            else
            {
                DEBUG_PRINT_ERROR("get_parameter:OMX_IndexParamAudioPcm \
                                  OMX_ErrorBadPortIndex %d\n", (int)pcmparam->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }
         case OMX_IndexParamComponentSuspended:
        {
            OMX_PARAM_SUSPENSIONTYPE *suspend= (OMX_PARAM_SUSPENSIONTYPE *) paramData;
            if(bSuspendEventRxed)
            {
              suspend->eType = OMX_Suspended;
            }
            else
            {
              suspend->eType = OMX_NotSuspended;
            }
            DEBUG_PRINT("get_parameter: suspend type %d", suspend->eType);

           break;
        }
    case OMX_IndexParamVideoInit:
        {
            OMX_PORT_PARAM_TYPE *portParamType = (OMX_PORT_PARAM_TYPE *) paramData;
            DEBUG_PRINT("get_parameter: OMX_IndexParamVideoInit\n");
            portParamType->nVersion.nVersion = OMX_SPEC_VERSION;
            portParamType->nSize = sizeof(portParamType);
            portParamType->nPorts           = 0;
            portParamType->nStartPortNumber = 0;
            break;
        }
    case OMX_IndexParamPriorityMgmt:
        {
            OMX_PRIORITYMGMTTYPE *priorityMgmtType =
                (OMX_PRIORITYMGMTTYPE*)paramData;
            DEBUG_PRINT("get_parameter: OMX_IndexParamPriorityMgmt\n");
            priorityMgmtType->nSize = sizeof(priorityMgmtType);
            priorityMgmtType->nVersion.nVersion = OMX_SPEC_VERSION;
            priorityMgmtType->nGroupID = m_priority_mgm.nGroupID;
            priorityMgmtType->nGroupPriority = m_priority_mgm.nGroupPriority;
            break;
        }
    case OMX_IndexParamImageInit:
        {
            OMX_PORT_PARAM_TYPE *portParamType =
                (OMX_PORT_PARAM_TYPE *) paramData;
            DEBUG_PRINT("get_parameter: OMX_IndexParamImageInit\n");
            portParamType->nVersion.nVersion = OMX_SPEC_VERSION;
            portParamType->nSize = sizeof(portParamType);
            portParamType->nPorts           = 0;
            portParamType->nStartPortNumber = 0;
            break;
        }

    case OMX_IndexParamCompBufferSupplier:
        {
            DEBUG_PRINT("get_parameter: OMX_IndexParamCompBufferSupplier\n");
            OMX_PARAM_BUFFERSUPPLIERTYPE *bufferSupplierType
                = (OMX_PARAM_BUFFERSUPPLIERTYPE*) paramData;
            DEBUG_PRINT("get_parameter: OMX_IndexParamCompBufferSupplier\n");

            bufferSupplierType->nSize = sizeof(bufferSupplierType);
            bufferSupplierType->nVersion.nVersion = OMX_SPEC_VERSION;
            if(OMX_CORE_INPUT_PORT_INDEX   == bufferSupplierType->nPortIndex)
            {
                bufferSupplierType->nPortIndex = OMX_BufferSupplyUnspecified;
            }
            else if (OMX_CORE_OUTPUT_PORT_INDEX == bufferSupplierType->nPortIndex)
            {
                bufferSupplierType->nPortIndex = OMX_BufferSupplyUnspecified;
            }
            else
            {
                eRet = OMX_ErrorBadPortIndex;
            }
            DEBUG_PRINT_ERROR("get_parameter:OMX_IndexParamCompBufferSupplier \
                              eRet %08x\n", eRet);
            break;
        }

        /*Component should support this port definition*/
    case OMX_IndexParamOtherInit:
        {
            OMX_PORT_PARAM_TYPE *portParamType = (OMX_PORT_PARAM_TYPE *) paramData;
            DEBUG_PRINT("get_parameter: OMX_IndexParamOtherInit\n");
            portParamType->nVersion.nVersion = OMX_SPEC_VERSION;
            portParamType->nSize = sizeof(portParamType);
            portParamType->nPorts           = 0;
            portParamType->nStartPortNumber = 0;
            break;
        }
        case QOMX_IndexParamAudioSessionId:
    {
           QOMX_AUDIO_STREAM_INFO_DATA *streaminfoparam =
                   (QOMX_AUDIO_STREAM_INFO_DATA *) paramData;
           streaminfoparam->sessionId = m_session_id;
           break;
         }
    default:
        {
            DEBUG_PRINT_ERROR("unknown param %08x\n", paramIndex);
            eRet = OMX_ErrorUnsupportedIndex;
        }
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
set_parameter

DESCRIPTION:
Function that set paramter from IL client

INPUT/OUTPUT PARAMETERS:
[IN] paramData - pointer to memory space which holds the paramter
[IN] hComp - handle to component instance
[IN] paramIndex - parameter type

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::set_parameter(OMX_IN OMX_HANDLETYPE     hComp,
                                           OMX_IN OMX_INDEXTYPE paramIndex,
                                           OMX_IN OMX_PTR        paramData)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    if(hComp == NULL)
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Set Param in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    if(paramData == NULL)
    {
        DEBUG_PRINT("param data is NULL");
        return OMX_ErrorBadParameter;
    }

    DEBUG_PRINT("Inside set_parameter\n");

    switch(paramIndex)
    {
    case OMX_IndexParamAudioWma:
        {
            DEBUG_PRINT("OMX_IndexParamAudiowma");
            m_adec_param = *((OMX_AUDIO_PARAM_WMATYPE *) paramData);
            if((m_adec_param.nChannels > WMA_DSP_MAX_CHANNELS) || \
               ( m_adec_param.nChannels < WMA_DSP_MIN_CHANNELS ))
            {
               DEBUG_PRINT_ERROR("set_parameter:ERROR!! number of channels %d unsupported\n",\
                               m_adec_param.nChannels);
               post_command((unsigned)OMX_CommandStateSet,
                               (unsigned)OMX_StateInvalid,
                               OMX_COMPONENT_GENERATE_COMMAND);
               execute_omx_flush(-1,false);
               return OMX_ErrorInvalidState;
            }
            break;
        }
    case QOMX_IndexParamAudioWma10Pro:
        {
            DEBUG_PRINT("OMX_IndexParamAudiowma10Pro");
            m_adec_param_wma10Pro = *((QOMX_AUDIO_PARAM_WMA10PROTYPE *) paramData);
            if((m_adec_param_wma10Pro.nChannels > WMA_DSP_MAX_CHANNELS) || \
               ( m_adec_param_wma10Pro.nChannels < WMA_DSP_MIN_CHANNELS ))
            {
               DEBUG_PRINT_ERROR("set_parameter:ERROR!! number of channels %d unsupported\n",\
                               m_adec_param_wma10Pro.nChannels);
               post_command((unsigned)OMX_CommandStateSet,
                               (unsigned)OMX_StateInvalid,
                               OMX_COMPONENT_GENERATE_COMMAND);
               execute_omx_flush(-1,false);
               return OMX_ErrorInvalidState;
            }
            break;
        }
    case OMX_IndexParamPortDefinition:
        {
            OMX_PARAM_PORTDEFINITIONTYPE *portDefn;
            portDefn = (OMX_PARAM_PORTDEFINITIONTYPE *) paramData;

            if ( ((m_state == OMX_StateLoaded)&&
                !BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING))
                || (m_state == OMX_StateWaitForResources &&
                ((OMX_DirInput == portDefn->eDir &&
                m_inp_bEnabled == true)||
                (OMX_DirInput == portDefn->eDir &&
                m_out_bEnabled == true)))
                ||(((OMX_DirInput == portDefn->eDir &&
                m_inp_bEnabled == false)||
                (OMX_DirInput == portDefn->eDir &&
                m_out_bEnabled == false)) &&
                (m_state != OMX_StateWaitForResources)) )
            {
                DEBUG_PRINT("Set Parameter called in valid state\n");
            } else
            {
                DEBUG_PRINT_ERROR("Set Parameter called in Invalid State\n");
                return OMX_ErrorIncorrectStateOperation;
            }
            DEBUG_PRINT("OMX_IndexParamPortDefinition nPortIndex = %d\n",\
                (int)portDefn->nPortIndex);
            if ( OMX_CORE_INPUT_PORT_INDEX == portDefn->nPortIndex )
            {

                if ( portDefn->nBufferCountActual >
                    OMX_CORE_NUM_INPUT_BUFFERS )
                {
                    m_inp_act_buf_count = portDefn->nBufferCountActual;
                } else
                {
                    m_inp_act_buf_count =OMX_CORE_NUM_INPUT_BUFFERS;
                }
                input_buffer_size = portDefn->nBufferSize;

            } else if ( OMX_CORE_OUTPUT_PORT_INDEX == portDefn->nPortIndex )
            {

                if ( portDefn->nBufferCountActual >
                    OMX_CORE_NUM_OUTPUT_BUFFERS )
                {
                    m_out_act_buf_count = portDefn->nBufferCountActual;
                } else
                {
                    m_out_act_buf_count =OMX_CORE_NUM_OUTPUT_BUFFERS;
                }
                if(portDefn->nBufferSize < OMX_CORE_OUTPUT_BUFFER_SIZE)
                {
                    output_buffer_size = OMX_CORE_OUTPUT_BUFFER_SIZE;
                }
                else
                {
                    output_buffer_size = portDefn->nBufferSize;
                }
            } else
            {
                DEBUG_PRINT(" set_parameter: Bad Port idx %d",\
                    (int)portDefn->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }
    case OMX_IndexParamPriorityMgmt:
        {
            DEBUG_PRINT("set_parameter: OMX_IndexParamPriorityMgmt\n");

            if(m_state != OMX_StateLoaded)
            {
                DEBUG_PRINT_ERROR("Set Parameter called in Invalid State\n");
                return OMX_ErrorIncorrectStateOperation;
            }
            OMX_PRIORITYMGMTTYPE *priorityMgmtype
                = (OMX_PRIORITYMGMTTYPE*) paramData;
            DEBUG_PRINT("set_parameter: OMX_IndexParamPriorityMgmt %d\n",
                (int)priorityMgmtype->nGroupID);

            DEBUG_PRINT("set_parameter: priorityMgmtype %d\n",
                (int)priorityMgmtype->nGroupPriority);

            m_priority_mgm.nGroupID = priorityMgmtype->nGroupID;
            m_priority_mgm.nGroupPriority = priorityMgmtype->nGroupPriority;

            break;
        }
    case  OMX_IndexParamAudioPortFormat:
        {

            OMX_AUDIO_PARAM_PORTFORMATTYPE *portFormatType =
                (OMX_AUDIO_PARAM_PORTFORMATTYPE *) paramData;
            DEBUG_PRINT("set_parameter: OMX_IndexParamAudioPortFormat\n");

            if (OMX_CORE_INPUT_PORT_INDEX== portFormatType->nPortIndex)
            {
                portFormatType->eEncoding = OMX_AUDIO_CodingWMA;
            }
            else if(OMX_CORE_OUTPUT_PORT_INDEX == portFormatType->nPortIndex)
            {
                DEBUG_PRINT("set_parameter: OMX_IndexParamAudioFormat: %d\n",
                    (int)portFormatType->nIndex);
                portFormatType->eEncoding = OMX_AUDIO_CodingPCM;
            }
            else
            {
                DEBUG_PRINT_ERROR("set_parameter: Bad port index %d\n",
                    (int)portFormatType->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }


    case OMX_IndexParamCompBufferSupplier:
        {
            DEBUG_PRINT("set_parameter: OMX_IndexParamCompBufferSupplier\n");
            OMX_PARAM_BUFFERSUPPLIERTYPE *bufferSupplierType
                = (OMX_PARAM_BUFFERSUPPLIERTYPE*) paramData;
            DEBUG_PRINT("set_parameter: OMX_IndexParamCompBufferSupplier %d\n",
                bufferSupplierType->eBufferSupplier);

            if(bufferSupplierType->nPortIndex== OMX_CORE_INPUT_PORT_INDEX
                || bufferSupplierType->nPortIndex == OMX_CORE_OUTPUT_PORT_INDEX)
            {
                DEBUG_PRINT("set_parameter: OMX_IndexParamCompBufferSupplier \
                            In/Out put \n");
                m_buffer_supplier.eBufferSupplier = bufferSupplierType->eBufferSupplier;
            }
            else
            {
                eRet = OMX_ErrorBadPortIndex;
            }

            DEBUG_PRINT_ERROR("set_parameter:OMX_IndexParamCompBufferSupplier: \
                              eRet  %08x\n", eRet);
            break;
        }

    case OMX_IndexParamAudioPcm:
        {
            DEBUG_PRINT("set_parameter: OMX_IndexParamAudioPcm\n");
            OMX_AUDIO_PARAM_PCMMODETYPE *pcmparam
                = (OMX_AUDIO_PARAM_PCMMODETYPE *) paramData;

            if (OMX_CORE_OUTPUT_PORT_INDEX== pcmparam->nPortIndex)
            {
                m_adec_param.nChannels=pcmparam->nChannels;
                m_pcm_param.nChannels =  pcmparam->nChannels;
                m_pcm_param.eNumData = pcmparam->eNumData;
                m_pcm_param.bInterleaved = pcmparam->bInterleaved;
                m_pcm_param.nBitPerSample =   pcmparam->nBitPerSample;
                m_pcm_param.nSamplingRate =   pcmparam->nSamplingRate;
                m_pcm_param.ePCMMode =  pcmparam->ePCMMode;
                m_pcm_param.eChannelMapping[0] =  pcmparam->eChannelMapping[0];
                m_pcm_param.eChannelMapping[1] =  pcmparam->eChannelMapping[1];

                DEBUG_PRINT("set_parameter: Number of channels %d",
                    (int)pcmparam->nChannels);
            }
            else
            {
                DEBUG_PRINT_ERROR("get_parameter:OMX_IndexParamAudioPcm \
                                  OMX_ErrorBadPortIndex %d\n", (int)pcmparam->nPortIndex);
                eRet = OMX_ErrorBadPortIndex;
            }
            break;
        }
        case OMX_IndexParamSuspensionPolicy:
            {
                OMX_PARAM_SUSPENSIONPOLICYTYPE *suspend_policy;
                suspend_policy = (OMX_PARAM_SUSPENSIONPOLICYTYPE*)paramData;
                suspensionPolicy= suspend_policy->ePolicy;
                DEBUG_PRINT("SET_PARAMETER: Set SUSPENSION POLICY %d  m_ipc_to_event_th=%p\n",
                                    suspensionPolicy,m_ipc_to_event_th);
                break;
            }
    case OMX_IndexParamStandardComponentRole:
        {
            OMX_PARAM_COMPONENTROLETYPE *componentRole;
            componentRole = (OMX_PARAM_COMPONENTROLETYPE*)paramData;
            component_Role.nSize = componentRole->nSize;
            component_Role.nVersion = componentRole->nVersion;
            strcpy((char *)component_Role.cRole,
                (const char*)componentRole->cRole);
            break;
        }

    default:
        {
            DEBUG_PRINT_ERROR("unknown param %d\n", paramIndex);
            eRet = OMX_ErrorUnsupportedIndex;
        }
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
get_config

DESCRIPTION:
OMX Get Config Method implementation.

INPUT/OUTPUT PARAMETERS:
[INOUT] configData
[IN] hComp
[IN] configIndex

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::get_config(OMX_IN OMX_HANDLETYPE      hComp,
                                        OMX_IN OMX_INDEXTYPE configIndex,
                                        OMX_INOUT OMX_PTR     configData)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    if(hComp == NULL)
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Get Config in Invalid State\n");
        return OMX_ErrorInvalidState;
    }

    switch(configIndex)
    {
    case OMX_IndexConfigAudioVolume:
        {
            OMX_AUDIO_CONFIG_VOLUMETYPE *volume =
                (OMX_AUDIO_CONFIG_VOLUMETYPE*) configData;

            if (OMX_CORE_INPUT_PORT_INDEX == volume->nPortIndex)
            {
                volume->nSize = sizeof(volume);
                volume->nVersion.nVersion = OMX_SPEC_VERSION;
                volume->bLinear = OMX_TRUE;
                volume->sVolume.nValue = m_volume;
                volume->sVolume.nMax   = OMX_ADEC_MAX;
                volume->sVolume.nMin   = OMX_ADEC_MIN;
            } else {
                eRet = OMX_ErrorBadPortIndex;
            }
        }
        break;

    case OMX_IndexConfigAudioMute:
        {
            OMX_AUDIO_CONFIG_MUTETYPE *mute =
                (OMX_AUDIO_CONFIG_MUTETYPE*) configData;

            if (OMX_CORE_INPUT_PORT_INDEX == mute->nPortIndex)
            {
                mute->nSize = sizeof(mute);
                mute->nVersion.nVersion = OMX_SPEC_VERSION;
                mute->bMute = (BITMASK_PRESENT(&m_flags,
                    OMX_COMPONENT_MUTED)?OMX_TRUE:OMX_FALSE);
            }
            else
            {
                eRet = OMX_ErrorBadPortIndex;
            }
        }
        break;

    default:
        eRet = OMX_ErrorUnsupportedIndex;
        break;
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
set_config

DESCRIPTION:
OMX Set Config method implementation

INPUT/OUTPUT PARAMETERS:
[IN] configData
[IN] hComp
[IN] configIndex

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::set_config(OMX_IN OMX_HANDLETYPE      hComp,
                                        OMX_IN OMX_INDEXTYPE configIndex,
                                        OMX_IN OMX_PTR        configData)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    if(hComp == NULL)
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Set Config in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    if( m_state == OMX_StateExecuting)
    {
        DEBUG_PRINT_ERROR("set_config:Ignore in Exe state\n");
        return OMX_ErrorInvalidState;
    }

    switch(configIndex)
    {
    case OMX_IndexConfigAudioVolume:
        {
            OMX_AUDIO_CONFIG_VOLUMETYPE *vol = (OMX_AUDIO_CONFIG_VOLUMETYPE*)
                configData;
            if (OMX_CORE_INPUT_PORT_INDEX == vol->nPortIndex)
            {
                if ((vol->sVolume.nValue <= OMX_ADEC_MAX) &&
                    (vol->sVolume.nValue >= OMX_ADEC_MIN))
                {
                    m_volume = vol->sVolume.nValue;
                    if (BITMASK_ABSENT(&m_flags, OMX_COMPONENT_MUTED))
                    {
                        /* ioctl(m_drv_fd, AUDIO_VOLUME, m_volume * OMX_ADEC_VOLUME_STEP); */
                    }
                }
                else
                {
                    eRet = OMX_ErrorBadParameter;
                }
            }
            else
            {
                eRet = OMX_ErrorBadPortIndex;
            }
        }
        break;

    case OMX_IndexConfigAudioMute:
        {
            OMX_AUDIO_CONFIG_MUTETYPE *mute = (OMX_AUDIO_CONFIG_MUTETYPE*)
                configData;
            if (OMX_CORE_INPUT_PORT_INDEX == mute->nPortIndex)
            {
                if (OMX_TRUE == mute->bMute)
                {
                    BITMASK_SET(&m_flags, OMX_COMPONENT_MUTED);
                    /* ioctl(m_drv_fd, AUDIO_VOLUME, 0); */
                }
                else
                {
                    BITMASK_CLEAR(&m_flags, OMX_COMPONENT_MUTED);
                    /* ioctl(m_drv_fd, AUDIO_VOLUME, m_volume * OMX_ADEC_VOLUME_STEP); */
                }
            }
            else
            {
                eRet = OMX_ErrorBadPortIndex;
            }
        }
        break;

    default:
        eRet = OMX_ErrorUnsupportedIndex;
        break;
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
get_extension_index

DESCRIPTION:
OMX GetExtensionIndex method implementaion.

INPUT/OUTPUT PARAMETERS:
[OUT] indexType
[IN] hComp
[IN] paramName

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::get_extension_index(
    OMX_IN OMX_HANDLETYPE      hComp,
    OMX_IN OMX_STRING      paramName,
    OMX_OUT OMX_INDEXTYPE* indexType)
{
    if((hComp == NULL) || (paramName == NULL) || (indexType == NULL))
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Get Extension Index in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    if(strncmp(paramName,"OMX.Qualcomm.index.audio.sessionId",strlen("OMX.Qualcomm.index.audio.session Id")) == 0){
          *indexType =(OMX_INDEXTYPE)QOMX_IndexParamAudioSessionId;
          DEBUG_PRINT("Extension index type - %d\n", *indexType);

    }
    else if(strncmp(paramName,"OMX.Qualcomm.index.audio.wma10Pro",strlen("OMX.Qualcomm.index.audio.wma10Pro")) == 0)
    {
        *indexType =(OMX_INDEXTYPE)QOMX_IndexParamAudioWma10Pro;
        DEBUG_PRINT("Extension index type - %d\n", *indexType);

    }
    else
    {
          return OMX_ErrorBadParameter;

    }
  return OMX_ErrorNone;
}

/*=============================================================================
FUNCTION:
get_state

DESCRIPTION:
Returns the state information back to the caller.<TBD>

INPUT/OUTPUT PARAMETERS:
[OUT] state
[IN] hComp

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::get_state(OMX_IN OMX_HANDLETYPE  hComp,
                                       OMX_OUT OMX_STATETYPE* state)
{
    *state = m_state;
    DEBUG_PRINT("Returning the state %d\n",*state);
    if(hComp == NULL)
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    return OMX_ErrorNone;
}

/*=============================================================================
FUNCTION:
component_tunnel_request

DESCRIPTION:
OMX Component Tunnel Request method implementation. <TBD>

INPUT/OUTPUT PARAMETERS:
[INOUT] tunnelSetup
[IN] hComp
[IN] port
[IN] peerComponent
[IN] peerPort

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::component_tunnel_request(
    OMX_IN OMX_HANDLETYPE                hComp,
    OMX_IN OMX_U32                        port,
    OMX_IN OMX_HANDLETYPE        peerComponent,
    OMX_IN OMX_U32                    peerPort,
    OMX_INOUT OMX_TUNNELSETUPTYPE* tunnelSetup)
{
    DEBUG_PRINT_ERROR("Error: component_tunnel_request Not Implemented\n");
    if((hComp == NULL) || (peerComponent == NULL) || (tunnelSetup == NULL))
    {
        port = 0;
        peerPort = 0;
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    return OMX_ErrorNotImplemented;
}

/*=============================================================================
FUNCTION:
allocate_input_buffer

DESCRIPTION:
Helper function for allocate buffer in the input pin

INPUT/OUTPUT PARAMETERS:
[INOUT] bufferHdr
[IN] hComp
[IN] port
[IN] appData
[IN] bytes

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::allocate_input_buffer(
    OMX_IN OMX_HANDLETYPE                hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
    OMX_IN OMX_U32                        port,
    OMX_IN OMX_PTR                     appData,
    OMX_IN OMX_U32                       bytes)
{
    OMX_ERRORTYPE         eRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE  *bufHdr;
    unsigned              nBufSize = MAX(bytes, input_buffer_size);
    char                  *buf_ptr;
    OMX_U16               blocksize;
    if(!wma10Pro)
    {
       blocksize = m_adec_param.nBlockAlign;
    }
    else
    {
       blocksize = m_adec_param_wma10Pro.nBlockAlign;
    }
    unsigned              transcode_buffer_size = \
    PACKET_NUM *(nBufSize +((nBufSize/blocksize) *( DRIVER_PACKET_SIZE + PADDING_SIZE)));
    if (transcode_buffer_size <= ((WMA_DSP_MIN_BUF_SIZE) + (2 * blocksize) + sizeof(META_IN))) {
	    transcode_buffer_size = ((WMA_DSP_MIN_BUF_SIZE) + (2 * blocksize) + sizeof(META_IN));
    }
    DEBUG_PRINT("m_inp_current_buf_count - %d\n", m_inp_current_buf_count);
    DEBUG_PRINT("m_inp_current_buf_count - %d\n", m_inp_current_buf_count);
    DEBUG_PRINT("transcode_buffer_size = %d",transcode_buffer_size);
    if(hComp == NULL)
    {
        port = 0;
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(m_trans_buf_cnt == 0)
    {
         m_trans_buffer_start = (OMX_U8 *)malloc(sizeof(OMX_U8) * transcode_buffer_size) ;
         if(m_trans_buffer_start == NULL)
         {
            DEBUG_PRINT("FAILURE IN ALLOCATING TRAANSCODED BUFFER\n");
            return OMX_ErrorInsufficientResources;
         }
         memset(m_trans_buffer_start, 0, sizeof(OMX_U8) * transcode_buffer_size) ;

         if(pcm_feedback)
         {
            m_tmp_in_meta_buf = (OMX_U8*) malloc((sizeof(OMX_U8) * transcode_buffer_size) + sizeof(META_IN));

            if (NULL == m_tmp_in_meta_buf)
            {
               DEBUG_PRINT("UseBuf: Mem alloc failed for meta buf\n");
               return OMX_ErrorInsufficientResources;
            }
            else
               DEBUG_PRINT("UseBuf: Mem alloc success for meta buf\n");
         }
         m_trans_buffer = m_trans_buffer_start;
         m_trans_buf_cnt++;

    }
    if(m_inp_current_buf_count < m_inp_act_buf_count)
    {

        buf_ptr = (char *) calloc( (nBufSize + sizeof(OMX_BUFFERHEADERTYPE) ) , 1);

        if (buf_ptr != NULL)
        {
            bufHdr = (OMX_BUFFERHEADERTYPE *) buf_ptr;
            *bufferHdr = bufHdr;
            memset(bufHdr,0,sizeof(OMX_BUFFERHEADERTYPE));


            bufHdr->pBuffer           = (OMX_U8 *)((buf_ptr) +
                sizeof(OMX_BUFFERHEADERTYPE));
            DEBUG_PRINT("bufHdr %x bufHdr->pBuffer %x", (int)bufHdr, (int)bufHdr->pBuffer);
            bufHdr->nSize             = sizeof(OMX_BUFFERHEADERTYPE);
            bufHdr->nVersion.nVersion = OMX_SPEC_VERSION;
            bufHdr->nAllocLen         = nBufSize;
            bufHdr->pAppPrivate       = appData;
            bufHdr->nInputPortIndex   = OMX_CORE_INPUT_PORT_INDEX;
            m_input_buf_hdrs.insert(bufHdr, NULL);
            m_inp_current_buf_count++;
        }
        else
        {
            DEBUG_PRINT("Input buffer memory allocation failed 1 \n");
            eRet =  OMX_ErrorInsufficientResources;
        }
    }
    else
    {
        DEBUG_PRINT("Input buffer memory allocation failed 2\n");
        eRet =  OMX_ErrorInsufficientResources;
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
allocate_output_buffer

DESCRIPTION:
Helper function for allocate buffer in the output pin

INPUT/OUTPUT PARAMETERS:
[INOUT] bufferHdr
[IN] hComp
[IN] port
[IN] appData
[IN] bytes

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::allocate_output_buffer(
    OMX_IN OMX_HANDLETYPE                hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
    OMX_IN OMX_U32                        port,
    OMX_IN OMX_PTR                     appData,
    OMX_IN OMX_U32                       bytes)
{
    OMX_ERRORTYPE         eRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE  *bufHdr;
    unsigned               nBufSize = MAX(bytes,output_buffer_size);
    char                  *buf_ptr;

    if(hComp == NULL)
    {
        port = 0;
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }

    if(NULL == m_tmp_out_meta_buf)
    {
        m_tmp_out_meta_buf = (OMX_U8*)malloc((sizeof(OMX_U8)*output_buffer_size) + sizeof(META_OUT));
        if (NULL == m_tmp_out_meta_buf)
        {
            DEBUG_PRINT("Mem alloc failed for out meta buf\n");
            return OMX_ErrorInsufficientResources;
        }
        else
            DEBUG_PRINT("Mem alloc success for out meta buf\n");
    }

    if(m_out_current_buf_count < m_out_act_buf_count)
    {
        buf_ptr = (char *) calloc( (nBufSize + sizeof(OMX_BUFFERHEADERTYPE) ) , 1);

        if (buf_ptr != NULL)
        {
            bufHdr = (OMX_BUFFERHEADERTYPE *) buf_ptr;
            *bufferHdr = bufHdr;
            memset(bufHdr,0,sizeof(OMX_BUFFERHEADERTYPE));

            bufHdr->pBuffer           = (OMX_U8 *)((buf_ptr) +
                sizeof(OMX_BUFFERHEADERTYPE));
            DEBUG_PRINT("AOB::bufHdr %x bufHdr->pBuffer %x", (int)bufHdr, (int)bufHdr->pBuffer);
            bufHdr->nSize             = sizeof(OMX_BUFFERHEADERTYPE);
            bufHdr->nVersion.nVersion = OMX_SPEC_VERSION;
            bufHdr->nAllocLen         = nBufSize;
            bufHdr->pAppPrivate       = appData;
            bufHdr->nOutputPortIndex   = OMX_CORE_OUTPUT_PORT_INDEX;
            m_output_buf_hdrs.insert(bufHdr, NULL);
            m_out_current_buf_count++;
        }
        else
        {
            DEBUG_PRINT("Output buffer memory allocation failed 1 \n");
            eRet =  OMX_ErrorInsufficientResources;
        }
    }
    else
    {
        DEBUG_PRINT("Output buffer memory allocation failed 2 \n");
        eRet =  OMX_ErrorInsufficientResources;
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
allocate_buffer

DESCRIPTION:
Returns zero if all the buffers released..

INPUT/OUTPUT PARAMETERS:
[INOUT] bufferHdr
[IN] hComp
[IN] port
[IN] appData
[IN] bytes

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::allocate_buffer(
    OMX_IN OMX_HANDLETYPE                hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE**     bufferHdr,
    OMX_IN OMX_U32                       port,
    OMX_IN OMX_PTR                       appData,
    OMX_IN OMX_U32                       bytes)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone; // OMX return type

    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT_ERROR("Allocate Buf in Invalid State\n");
        return OMX_ErrorInvalidState;
    }

    // What if the client calls again.
    if(OMX_CORE_INPUT_PORT_INDEX == port)
    {
        eRet = allocate_input_buffer(hComp,bufferHdr,port,appData,bytes);
    } else if ( OMX_CORE_OUTPUT_PORT_INDEX == port )
    {
        eRet = allocate_output_buffer(hComp,bufferHdr,port,appData,bytes);
    } else
    {
        DEBUG_PRINT_ERROR("Error: Invalid Port Index received %d\n",
            (int)port);
        eRet = OMX_ErrorBadPortIndex;
    }

    if((eRet == OMX_ErrorNone))
    {
        if(allocate_done())
        {
            if ( BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING) )
            {
                BITMASK_CLEAR(&m_flags, OMX_COMPONENT_IDLE_PENDING);
                post_command(OMX_CommandStateSet,OMX_StateIdle,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
        }

        if(port == OMX_CORE_INPUT_PORT_INDEX && m_inp_bPopulated)
        {
            if(BITMASK_PRESENT(&m_flags,OMX_COMPONENT_INPUT_ENABLE_PENDING))
            {
                BITMASK_CLEAR((&m_flags),OMX_COMPONENT_INPUT_ENABLE_PENDING);
                post_command(OMX_CommandPortEnable, OMX_CORE_INPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
        }
        if(port == OMX_CORE_OUTPUT_PORT_INDEX && m_out_bPopulated)
        {
            if(BITMASK_PRESENT(&m_flags,OMX_COMPONENT_OUTPUT_ENABLE_PENDING))
            {
                BITMASK_CLEAR((&m_flags),OMX_COMPONENT_OUTPUT_ENABLE_PENDING);
                m_out_bEnabled = OMX_TRUE;
                DEBUG_PRINT("AllocBuf-->is_out_th_sleep=%d\n",is_out_th_sleep);
                bOutputPortReEnabled = 1;
                pthread_mutex_lock(&m_out_th_lock_1);
                if ( is_out_th_sleep )
                {
                    is_out_th_sleep = false;
                    DEBUG_DETAIL("AllocBuf:WAKING UP OUT THREADS\n");
                    out_th_wakeup();
                }
                pthread_mutex_unlock(&m_out_th_lock_1);
                pthread_mutex_lock(&m_in_th_lock_1);
                if(is_in_th_sleep)
                {
                   is_in_th_sleep = false;
                   DEBUG_DETAIL("AB:WAKING UP IN THREADS\n");
                   in_th_wakeup();
                }
                pthread_mutex_unlock(&m_in_th_lock_1);
                post_command(OMX_CommandPortEnable, OMX_CORE_OUTPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
        }
    }
    DEBUG_PRINT("Allocate Buffer exit with ret Code %d\n", eRet);
    return eRet;
}

/*=============================================================================
FUNCTION:
use_buffer

DESCRIPTION:
OMX Use Buffer method implementation.

INPUT/OUTPUT PARAMETERS:
[INOUT] bufferHdr
[IN] hComp
[IN] port
[IN] appData
[IN] bytes
[IN] buffer

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::use_buffer(
    OMX_IN OMX_HANDLETYPE            hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
    OMX_IN OMX_U32                   port,
    OMX_IN OMX_PTR                   appData,
    OMX_IN OMX_U32                   bytes,
    OMX_IN OMX_U8*                   buffer)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    if(OMX_CORE_INPUT_PORT_INDEX == port)
    {
        eRet = use_input_buffer(hComp,bufferHdr,port,appData,bytes,buffer);
    }
    else if(OMX_CORE_OUTPUT_PORT_INDEX == port)
    {
        eRet = use_output_buffer(hComp,bufferHdr,port,appData,bytes,buffer);
    }
    else
    {
        DEBUG_PRINT_ERROR("Error: Invalid Port Index received %d\n",(int)port);
        eRet = OMX_ErrorBadPortIndex;
    }

    if ( eRet == OMX_ErrorNone )
    {
        DEBUG_PRINT("Checking for Output Allocate buffer Done");
        if ( allocate_done() )
        {
            if (BITMASK_PRESENT(&m_flags,OMX_COMPONENT_IDLE_PENDING))
            {
                BITMASK_CLEAR(&m_flags, OMX_COMPONENT_IDLE_PENDING);
                post_command(OMX_CommandStateSet,OMX_StateIdle,
                    OMX_COMPONENT_GENERATE_EVENT);
            }
        }

        if(port == OMX_CORE_INPUT_PORT_INDEX && m_inp_bPopulated)
        {
            if(BITMASK_PRESENT(&m_flags,OMX_COMPONENT_INPUT_ENABLE_PENDING))
            {
                BITMASK_CLEAR((&m_flags),OMX_COMPONENT_INPUT_ENABLE_PENDING);
                post_command(OMX_CommandPortEnable, OMX_CORE_INPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);

            }
        }
        if(port == OMX_CORE_OUTPUT_PORT_INDEX && m_out_bPopulated)
        {
            if(BITMASK_PRESENT(&m_flags,OMX_COMPONENT_OUTPUT_ENABLE_PENDING))
            {
                BITMASK_CLEAR((&m_flags),OMX_COMPONENT_OUTPUT_ENABLE_PENDING);
                post_command(OMX_CommandPortEnable, OMX_CORE_OUTPUT_PORT_INDEX,
                    OMX_COMPONENT_GENERATE_EVENT);
                bOutputPortReEnabled = 1;
                pthread_mutex_lock(&m_out_th_lock_1);
                if ( is_out_th_sleep )
                {
                    is_out_th_sleep = false;
                    DEBUG_DETAIL("UseBuf:WAKING UP OUT THREADS\n");
                    out_th_wakeup();
                }
                pthread_mutex_unlock(&m_out_th_lock_1);
                pthread_mutex_lock(&m_in_th_lock_1);
                if(is_in_th_sleep)
                {
                   is_in_th_sleep = false;
                   DEBUG_DETAIL("UB:WAKING UP IN THREADS\n");
                   in_th_wakeup();
                }
                pthread_mutex_unlock(&m_in_th_lock_1);
            }
        }

    }
    DEBUG_PRINT("Use Buffer for port%d\n", (int)port);


    return eRet;
}
/*=============================================================================
FUNCTION:
use_input_buffer

DESCRIPTION:
Helper function for Use buffer in the input pin

INPUT/OUTPUT PARAMETERS:
[INOUT] bufferHdr
[IN] hComp
[IN] port
[IN] appData
[IN] bytes
[IN] buffer

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::use_input_buffer(
    OMX_IN OMX_HANDLETYPE            hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
    OMX_IN OMX_U32                   port,
    OMX_IN OMX_PTR                   appData,
    OMX_IN OMX_U32                   bytes,
    OMX_IN OMX_U8*                   buffer)
{
    OMX_ERRORTYPE         eRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE  *bufHdr;
    unsigned              nBufSize = MAX(bytes, input_buffer_size);
    char                  *buf_ptr;
    OMX_U16               blocksize;
    if(!wma10Pro)
    {
       blocksize = m_adec_param.nBlockAlign;
    }
    else
    {
       blocksize = m_adec_param_wma10Pro.nBlockAlign;
    }

    unsigned              transcode_buffer_size = \
    PACKET_NUM *(nBufSize +((nBufSize/ blocksize) *( DRIVER_PACKET_SIZE + PADDING_SIZE)));
    if (transcode_buffer_size <= ((WMA_DSP_MIN_BUF_SIZE) + (2 * blocksize) + sizeof(META_IN))) {
	    transcode_buffer_size = ((WMA_DSP_MIN_BUF_SIZE) + (2 * blocksize) + sizeof(META_IN));
    }
    DEBUG_PRINT("m_inp_current_buf_count - %d\n", m_inp_current_buf_count);
    DEBUG_PRINT("m_inp_current_buf_count - %d\n", m_inp_current_buf_count);
    DEBUG_PRINT("transcode_buffer_size = %d",transcode_buffer_size);
    DEBUG_PRINT("Inside omx_wma_adec::use_input_buffer\n");

    if(hComp == NULL)
    {
        port = 0;
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(m_trans_buf_cnt == 0)
    {
        m_trans_buffer_start = (OMX_U8 *)malloc(sizeof(OMX_U8) * transcode_buffer_size) ;
        if(m_trans_buffer_start == NULL)
        {
            DEBUG_PRINT("FAILURE IN ALLOCATING TRAANSCODED BUFFER\n");
            return OMX_ErrorInsufficientResources;
        }
        memset(m_trans_buffer_start, 0, sizeof(OMX_U8) * transcode_buffer_size) ;

        if(pcm_feedback)
        {
            m_tmp_in_meta_buf = (OMX_U8*) malloc((sizeof(OMX_U8) * transcode_buffer_size) + sizeof(META_IN));

            if (NULL == m_tmp_in_meta_buf)
            {
               DEBUG_PRINT("UseBuf: Mem alloc failed for meta buf\n");
               return OMX_ErrorInsufficientResources;
            }
            else
               DEBUG_PRINT("UseBuf: Mem alloc success for meta buf\n");
        }

        m_trans_buffer = m_trans_buffer_start;
        m_trans_buf_cnt++;

    }

    if ( m_inp_current_buf_count < m_inp_act_buf_count )
    {

        buf_ptr = (char *) calloc(sizeof(OMX_BUFFERHEADERTYPE), 1);

        if (buf_ptr != NULL)
        {
            bufHdr = (OMX_BUFFERHEADERTYPE *) buf_ptr;
            *bufferHdr = bufHdr;
            memset(bufHdr,0,sizeof(OMX_BUFFERHEADERTYPE));

            bufHdr->pBuffer           = (OMX_U8 *)(buffer);
            DEBUG_PRINT("bufHdr %x bufHdr->pBuffer %x", (int)bufHdr, (int)bufHdr->pBuffer);
            bufHdr->nSize             = sizeof(OMX_BUFFERHEADERTYPE);
            bufHdr->nVersion.nVersion = OMX_SPEC_VERSION;
            bufHdr->nAllocLen         = nBufSize;
            input_buffer_size         = nBufSize;
            bufHdr->pAppPrivate       = appData;
            bufHdr->nInputPortIndex   = OMX_CORE_INPUT_PORT_INDEX;
            m_input_buf_hdrs.insert(bufHdr, NULL);
            m_inp_current_buf_count++;
        }
        else
        {
            DEBUG_PRINT("Input buffer memory allocation failed 1 \n");
            eRet =  OMX_ErrorInsufficientResources;
        }
    }
    else
    {
        DEBUG_PRINT("Input buffer memory allocation failed 2\n");
        eRet =  OMX_ErrorInsufficientResources;
    }
    return eRet;

}

/*=============================================================================
FUNCTION:
use_output_buffer

DESCRIPTION:
Helper function for Use buffer in the output pin

INPUT/OUTPUT PARAMETERS:
[INOUT] bufferHdr
[IN] hComp
[IN] port
[IN] appData
[IN] bytes
[IN] buffer

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::use_output_buffer(
    OMX_IN OMX_HANDLETYPE            hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE** bufferHdr,
    OMX_IN OMX_U32                   port,
    OMX_IN OMX_PTR                   appData,
    OMX_IN OMX_U32                   bytes,
    OMX_IN OMX_U8*                   buffer)
{
    OMX_ERRORTYPE         eRet = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE  *bufHdr;
    unsigned              nBufSize = MAX(bytes,output_buffer_size);
    char                  *buf_ptr;

    if(hComp == NULL)
    {
        port = 0;
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }

    if ( bytes < output_buffer_size )
    {
        /* return if o\p buffer size provided by client
        is less than min o\p buffer size supported by omx component*/
        return OMX_ErrorInsufficientResources;
    }
    DEBUG_PRINT("Inside omx_wma_adec::use_output_buffer");

    if(NULL == m_tmp_out_meta_buf)
    {
        m_tmp_out_meta_buf = (OMX_U8*)malloc((sizeof(OMX_U8)*output_buffer_size) + sizeof(META_OUT));
        if (NULL == m_tmp_out_meta_buf)
        {
            DEBUG_PRINT("Mem alloc failed for out meta buf\n");
            return OMX_ErrorInsufficientResources;
        }
        else
            DEBUG_PRINT("Mem alloc success for out meta buf\n");
    }

    if ( m_out_current_buf_count < m_out_act_buf_count )
    {

        buf_ptr = (char *) calloc(sizeof(OMX_BUFFERHEADERTYPE), 1);

        if (buf_ptr != NULL)
        {
            bufHdr = (OMX_BUFFERHEADERTYPE *) buf_ptr;
            DEBUG_PRINT("BufHdr=%p buffer=%p\n",bufHdr,buffer);
            *bufferHdr = bufHdr;
            memset(bufHdr,0,sizeof(OMX_BUFFERHEADERTYPE));

            bufHdr->pBuffer           = (OMX_U8 *)(buffer);
            DEBUG_PRINT("bufHdr %x bufHdr->pBuffer %x", (int)bufHdr, (int)bufHdr->pBuffer);
            bufHdr->nSize             = sizeof(OMX_BUFFERHEADERTYPE);
            bufHdr->nVersion.nVersion = OMX_SPEC_VERSION;
            bufHdr->nAllocLen         = nBufSize;
            output_buffer_size        = nBufSize;
            bufHdr->pAppPrivate       = appData;
            bufHdr->nOutputPortIndex   = OMX_CORE_OUTPUT_PORT_INDEX;
            m_output_buf_hdrs.insert(bufHdr, NULL);
            m_out_current_buf_count++;
        }
        else
        {
            DEBUG_PRINT("Output buffer memory allocation failed\n");
            eRet =  OMX_ErrorInsufficientResources;

        }
    }
    else
    {
        DEBUG_PRINT("Output buffer memory allocation failed 2\n");
        eRet =  OMX_ErrorInsufficientResources;
    }

    return eRet;
}

/*=============================================================================
FUNCTION:
search_input_bufhdr

DESCRIPTION:
Function that searches for caller buffer

INPUT/OUTPUT PARAMETERS:
[INOUT] buffer - pointer to buffer header

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::search_input_bufhdr(OMX_BUFFERHEADERTYPE *buffer)
{

    bool eRet = false;
    OMX_BUFFERHEADERTYPE *temp = NULL;

    //access only in IL client context
    temp = m_input_buf_hdrs.find_ele(buffer);
    if(buffer && temp)
    {
        DEBUG_DETAIL("search_input_bufhdr %x \n", (int)buffer);
        eRet = true;
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
search_output_bufhdr

DESCRIPTION:
Function that searches for caller buffer

INPUT/OUTPUT PARAMETERS:
[INOUT] buffer - pointer to buffer header

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::search_output_bufhdr(OMX_BUFFERHEADERTYPE *buffer)
{

    bool eRet = false;
    OMX_BUFFERHEADERTYPE *temp = NULL;

    //access only in IL client context
    temp = m_output_buf_hdrs.find_ele(buffer);
    if(buffer && temp)
    {
        DEBUG_DETAIL("search_output_bufhdr %x \n", (int)buffer);
        eRet = true;
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
free_buffer

DESCRIPTION:
Function that handles free buffer command from IL client
This function is a block-call function that handles IL client request to
freeing the buffer

INPUT/OUTPUT PARAMETERS:
[INOUT] buffer - pointer to buffer header
[IN] hComp - handle to component instance
[IN] port - id of port which holds the buffer

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::free_buffer(OMX_IN OMX_HANDLETYPE         hComp,
                                         OMX_IN OMX_U32                 port,
                                         OMX_IN OMX_BUFFERHEADERTYPE* buffer)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    DEBUG_PRINT("Free_Buffer buf %x\n", (int)buffer);

    if(hComp == NULL)
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(m_state == OMX_StateIdle &&
        (BITMASK_PRESENT(&m_flags ,OMX_COMPONENT_LOADING_PENDING)))
    {
        DEBUG_PRINT(" free buffer while Component in Loading pending\n");
    } else if ( (m_inp_bEnabled == OMX_FALSE &&
        port == OMX_CORE_INPUT_PORT_INDEX)||
        (m_out_bEnabled == OMX_FALSE &&
        port == OMX_CORE_OUTPUT_PORT_INDEX) )
    {
        DEBUG_PRINT("Free Buffer while port %d disabled\n", (int)port);
    } else if ( m_state == OMX_StateExecuting || m_state == OMX_StatePause )
    {
        DEBUG_PRINT("Invalid state to free buffer,ports need to be disabled:\
                    OMX_ErrorPortUnpopulated\n");
        post_command(OMX_EventError,
            OMX_ErrorPortUnpopulated,
            OMX_COMPONENT_GENERATE_EVENT);

        return eRet;
    } else
    {
        DEBUG_PRINT("free_buffer: Invalid state to free buffer,ports need to be\
                    disabled:OMX_ErrorPortUnpopulated\n");
        post_command(OMX_EventError,
            OMX_ErrorPortUnpopulated,
            OMX_COMPONENT_GENERATE_EVENT);
    }

    if(OMX_CORE_INPUT_PORT_INDEX == port)
    {
        DEBUG_PRINT("Before calling search_input_bufhdr 1\n");
        if(m_inp_current_buf_count != 0)
        {
            DEBUG_PRINT("Before calling search_input_bufhdr 2\n");
            m_inp_bPopulated = OMX_FALSE;
            if(true == search_input_bufhdr(buffer))
            {
                /* Buffer exist */
                //access only in IL client context
                m_input_buf_hdrs.erase(buffer);
		if (buffer) {
			free(buffer);
			buffer = NULL;
		}
                m_inp_current_buf_count--;
                DEBUG_PRINT("After erasing: m_inp_current_buf_count - %d\n",
                    m_inp_current_buf_count);
            } else {
                DEBUG_PRINT_ERROR("Free_Buf:Error-->free_buffer, \
                                  Invalid Input buffer header\n");
                eRet = OMX_ErrorBadParameter;
            }
        }
        else
        {
            DEBUG_PRINT_ERROR("Error: free_buffer , invalid Input buffer header\n");
            eRet = OMX_ErrorBadPortIndex;
        }
        if ( BITMASK_PRESENT((&m_flags),OMX_COMPONENT_INPUT_DISABLE_PENDING)
            && release_done(0) )
        {
            DEBUG_PRINT("INPUT PORT MOVING TO DISABLED STATE \n");
            BITMASK_CLEAR((&m_flags),OMX_COMPONENT_INPUT_DISABLE_PENDING);
            post_command(OMX_CommandPortDisable,
                OMX_CORE_INPUT_PORT_INDEX,
                OMX_COMPONENT_GENERATE_EVENT);

	    if(m_trans_buffer_start!= NULL)
	    {
		    free(m_trans_buffer_start);
		    m_trans_buffer_start= NULL;
		    if(m_trans_buf_cnt)
			    m_trans_buf_cnt--;
		    DEBUG_PRINT("FREEING m_trans_buf_cnt = %d\n",m_trans_buf_cnt);
	    }
        }

    }
    else if(OMX_CORE_OUTPUT_PORT_INDEX == port)
    {
        if(m_out_current_buf_count != 0)
        {
            m_out_bPopulated = OMX_FALSE;
            if(true == search_output_bufhdr(buffer))
            {
                /* Buffer exist */
                //access only in IL client context
                DEBUG_PRINT_ERROR(": free_buffer , Output buffer headerbuffer=%p\n",
                    buffer);
                m_output_buf_hdrs.erase(buffer);
		if (buffer) {
			free(buffer);
			buffer = NULL;
		}
                m_out_current_buf_count--;
            }
            else
            {
                DEBUG_PRINT_ERROR("Error: free_buffer , invalid Output buffer \
                                  header\n");
                eRet = OMX_ErrorBadParameter;
            }
        }
        else
        {
            eRet = OMX_ErrorBadPortIndex;
        }


        if(BITMASK_PRESENT((&m_flags),OMX_COMPONENT_OUTPUT_DISABLE_PENDING)
            && release_done(1) )
        {
            DEBUG_PRINT("OUTPUT PORT MOVING TO DISABLED STATE \n");
            BITMASK_CLEAR((&m_flags),OMX_COMPONENT_OUTPUT_DISABLE_PENDING);
            post_command(OMX_CommandPortDisable,
                OMX_CORE_OUTPUT_PORT_INDEX,
                OMX_COMPONENT_GENERATE_EVENT);

        }
    }
    else
    {
        eRet = OMX_ErrorBadPortIndex;
    }


    if((OMX_ErrorNone == eRet) &&
        (BITMASK_PRESENT(&m_flags ,OMX_COMPONENT_LOADING_PENDING)))
    {
        if ( release_done(-1) )
        {
            if(suspensionPolicy == OMX_SuspensionEnabled)
            {
               ioctl(m_drv_fd,AUDIO_ABORT_GET_EVENT,NULL);
            }
            if(ioctl(m_drv_fd, AUDIO_STOP, 0) < 0)
               DEBUG_PRINT("AUDIO STOP in free buffer failed\n");
            else
               DEBUG_PRINT("AUDIO STOP in free buffer passed\n");

            if(m_trans_buffer_start)
	    {
               free(m_trans_buffer_start);
	       m_trans_buffer_start= NULL;
	    }
            // Send the callback now
            BITMASK_CLEAR((&m_flags),OMX_COMPONENT_LOADING_PENDING);
            post_command(OMX_CommandStateSet,
                OMX_StateLoaded,OMX_COMPONENT_GENERATE_EVENT);
        }
    }

    return eRet;
}

/*=============================================================================
FUNCTION:
empty_this_buffer

DESCRIPTION:
Function that handles empty this buffer command
This function meremly queue up the command and data would be consumed
in command server thread context

INPUT/OUTPUT PARAMETERS:
[INOUT] buffer - pointer to buffer header
[IN] hComp - handle to component instance

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::empty_this_buffer(
    OMX_IN OMX_HANDLETYPE         hComp,
    OMX_IN OMX_BUFFERHEADERTYPE* buffer)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;

    DEBUG_PRINT("ETB:Buf:%x Len %d TS %d numInBuf=%d\n", (int)buffer,
        (int)buffer->nFilledLen, (int)buffer->nTimeStamp, (nNumInputBuf+1));
    if(m_state == OMX_StateInvalid)
    {
        DEBUG_PRINT("Empty this buffer in Invalid State\n");
        return OMX_ErrorInvalidState;
    }
    if(!m_inp_bEnabled)
    {
        DEBUG_PRINT("empty_this_buffer OMX_ErrorIncorrectStateOperation Port \
                    Status %d \n", m_inp_bEnabled);
        return OMX_ErrorIncorrectStateOperation;
    }
    if(buffer->nSize != sizeof(OMX_BUFFERHEADERTYPE))
    {
        DEBUG_PRINT("omx_wma_adec::etb--> Buffer Size Invalid\n");
        return OMX_ErrorBadParameter;
    }
    if(buffer->nVersion.nVersion != OMX_SPEC_VERSION)
    {
        DEBUG_PRINT("omx_wma_adec::etb--> OMX Version Invalid\n");
        return OMX_ErrorVersionMismatch;
    }

    if (buffer->nInputPortIndex != OMX_CORE_INPUT_PORT_INDEX)
    {
        return OMX_ErrorBadPortIndex;
    }
    if ((m_state != OMX_StateExecuting) &&
        (m_state != OMX_StatePause))
    {
        DEBUG_PRINT_ERROR("Invalid state\n");
        eRet = OMX_ErrorInvalidState;
    }
    if (OMX_ErrorNone == eRet)
    {
        if (search_input_bufhdr(buffer) == true)
        {
            pthread_mutex_lock(&in_buf_count_lock);
            nNumInputBuf++;
            pthread_mutex_unlock(&in_buf_count_lock);
            post_input((unsigned)hComp,
                (unsigned) buffer,OMX_COMPONENT_GENERATE_ETB);
        }
        else
        {
            DEBUG_PRINT_ERROR("Bad header %x \n", (int)buffer);
            eRet = OMX_ErrorBadParameter;
        }
    }

    return eRet;
}

/*=============================================================================
FUNCTION:
empty_this_buffer_proxy

DESCRIPTION:
Function that writes encoded data to kernel driver

INPUT/OUTPUT PARAMETERS:
[INOUT] buffer - pointer to buffer header
[IN] hComp - handle to component instance

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::empty_this_buffer_proxy(OMX_IN OMX_HANDLETYPE  hComp,
                                                     OMX_BUFFERHEADERTYPE* buffer)
{
    OMX_STATETYPE state;
    if(m_first == 0)
    {
        m_first++;
        if(ioctl(m_drv_fd, AUDIO_START, 0) < 0)
        {
            DEBUG_PRINT_ERROR("AUDIO_START FAILED\n");
            post_command((unsigned)OMX_CommandStateSet,
                             (unsigned)OMX_StateInvalid,
                              OMX_COMPONENT_GENERATE_COMMAND);
            post_command(OMX_CommandFlush,-1,
                                        OMX_COMPONENT_GENERATE_COMMAND);
            buffer_done_cb(buffer);
            //execute_omx_flush(-1,false);
            return OMX_ErrorInvalidState;
        }
	if(wma10Pro) {
		int iPlusVersion = (m_adec_param_wma10Pro.advancedEncodeOpt &
				ENCOPT4_PLUSVER);
		if (((iPlusVersion == ENCOPT4_PLUSV2) ||
				(iPlusVersion == ENCOPT4_PLUSV3)) &&
		((m_adec_param_wma10Pro.advancedEncodeOpt & ENCOPT4_PLUSV2_CHEX)
			== ENCOPT4_PLUSV2_CHEX)) {
		    if (((m_adec_param_wma10Pro.advancedEncodeOpt2 &
			ENCOPT4_PLUSV2_CXLCCHMASK) >>
			ENCOPT4_PLUSV2_CXLCCHMASK_BITSOFFSET) == CxSpeaker2) {
			    m_adec_param_wma10Pro.nChannels = 2;
		    }
		}
	}
	struct msm_audio_wmapro_config drv_config;
	ioctl(m_drv_fd, AUDIO_GET_WMAPRO_CONFIG, &drv_config);
	if (drv_config.numchannels != (OMX_U16)m_adec_param_wma10Pro.nChannels)
	{
		DEBUG_PRINT("Trigger OMX_COMPONENT_PORTSETTINGS_CHANGED");
		bOutputPortReEnabled=0;
		DEBUG_PRINT("Detected ch=%u cfg ch=%u\n",
			m_adec_param_wma10Pro.nChannels,
			drv_config.numchannels);
		DEBUG_PRINT("ETBP:: SLEEPING IN THREAD\n");
		pthread_mutex_lock(&m_in_th_lock_1);
		is_in_th_sleep = true;
		pthread_mutex_unlock(&m_in_th_lock_1);
		post_command((unsigned)&hComp, (unsigned)buffer,
				OMX_COMPONENT_PORTSETTINGS_CHANGED);
		in_th_goto_sleep();
		DEBUG_PRINT("ETBP: -->IN thread woken up by somebody\n");
	} else {
		m_out_bEnabled = OMX_TRUE;
		bOutputPortReEnabled = 1;
		DEBUG_PRINT("No OMX_COMPONENT_PORTSETTINGS_CHANGED");
		if ( is_out_th_sleep )
		{
			is_out_th_sleep = false;
			DEBUG_DETAIL("SCP:WAKING UP OUT THREADS\n");
			out_th_wakeup();
		}
	}
    }

    if(m_eos_bm)
    {
        m_eos_bm=0x00;
    }

    if(buffer->nFlags & OMX_BUFFERFLAG_EOS)
    {
       m_eos_bm |= IP_PORT_BITMASK;
       DEBUG_PRINT("ETBP:EOSFLAG=%d\n",m_eos_bm);
    }

    DEBUG_PRINT("Inside empty_this_buffer_proxy \n");
    DEBUG_PRINT("buffer->nFilledLen - %d buffer->nTimeStamp - %lld\n", (int)buffer->nFilledLen, buffer->nTimeStamp);
    if(!wma10Pro)
    {
        transcode_wma(hComp,buffer->pBuffer,buffer);
    }
    else
    {
        transcode_wma10Pro(hComp,buffer->pBuffer,buffer);
    }
    pthread_mutex_lock(&m_state_lock);
    get_state(&m_cmp, &state);
    pthread_mutex_unlock(&m_state_lock);
    nTimestamp = buffer->nTimeStamp;
    if (state == OMX_StateExecuting)
    {
        DEBUG_DETAIL("In Exe state, EBD CB");
        buffer_done_cb((OMX_BUFFERHEADERTYPE *)buffer);
    }
    else
    {
        post_input((unsigned) & hComp,(unsigned) buffer,
            OMX_COMPONENT_GENERATE_BUFFER_DONE);
    }
    return OMX_ErrorNone;
}

/*=============================================================================
FUNCTION:
fill_this_buffer_proxy

DESCRIPTION:
Function that reads decoded data from kernel driver

INPUT/OUTPUT PARAMETERS:
[INOUT] buffer - pointer to buffer header
[IN] hComp - handle to component instance

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::fill_this_buffer_proxy(
    OMX_IN OMX_HANDLETYPE         hComp,
    OMX_BUFFERHEADERTYPE* buffer)
{
    int byte_count = 0;
    int meta_count = 0;
    int nDatalen = 0;
    OMX_STATETYPE state;
    META_OUT      meta_out;
    META_OUT_SUS      meta_out_sus;
    int nReadbytes = 0;
    int ret = 0;
    unsigned int id = 0;
    get_state(&m_cmp, &state);
    if(fake_eos_recieved)
    {
        DEBUG_PRINT("Residual still left[%lu]state[%d]\n",
                                               m_bufMgr->getBufFilledSpace(),state);
        if(state == OMX_StateExecuting)
        {
            meta_count = m_bufMgr->emptyToBuf((OMX_U8 *)&meta_out_sus,sizeof(META_OUT_SUS));
            DEBUG_PRINT("Num of bytes meta copied[%d]Residual[%lu]\n",meta_out_sus.offsetVal,
                                                         m_bufMgr->getBufFilledSpace());
            if(meta_count > 0)
            {
                byte_count = m_bufMgr->emptyToBuf(buffer->pBuffer,meta_out_sus.offsetVal);
                DEBUG_PRINT("Num of bytes copied[%d]Residual[%lu]\n",byte_count,
                                                          m_bufMgr->getBufFilledSpace());
            }

            if(byte_count == 0)
            {
                byte_count = 0;
                fake_eos_recieved = false;

                // input EOS sent to DSP
                DEBUG_PRINT("FTBP: EOSFLAG=%d\n",m_eos_bm);
                if((m_eos_bm & IP_OP_PORT_BITMASK) == IP_OP_PORT_BITMASK)
                {
                   DEBUG_PRINT("FTBP:ENABLING SUSPEND FLAG\n");
                   buffer->nFlags= 0x01;
                   m_eos_bm = 0;
                }
                pthread_mutex_lock(&m_in_th_lock_1);
                if(is_in_th_sleep)
                {
                    DEBUG_DETAIL("FTBP::WAKING UP IN THREADS\n");
                    in_th_wakeup();
                    is_in_th_sleep = false;
                }
                pthread_mutex_unlock(&m_in_th_lock_1);

            }

            buffer->nTimeStamp = meta_out_sus.nTimeStamp;
            buffer->nFilledLen = byte_count;
            frame_done_cb((OMX_BUFFERHEADERTYPE *)buffer);
            return OMX_ErrorNone;
        }
    }
    /* Assume fill this buffer function has already checked
    validity of buffer */
    if(NULL == m_tmp_out_meta_buf)
    {
        DEBUG_PRINT_ERROR("Invalid Meta Buffer");
        return OMX_ErrorUndefined;
    }

    if(true == search_output_bufhdr(buffer))
    {
        if (0 == bEOSSent)
        {
            int ncount = buffer->nAllocLen/output_buffer_size;
            int nRead = 0;
            OMX_IN OMX_U8  *pBuf = NULL;
            pBuf = buffer->pBuffer;
            for (nRead = 0; nRead < ncount; nRead++)
            {
                if (m_output_ctrl_cmd_q.m_size)
                {
                     ret = m_output_ctrl_cmd_q.get_msg_id(&id);
                     if(id != OMX_COMPONENT_SUSPEND)
                     {
                        DEBUG_DETAIL("FTBP-->: FLUSH CMD IN Q, Exit from Loop");
                        break;
                      }
                }

                DEBUG_PRINT("\nBefore Read..m_drv_fd = %d, pBuf = %u\n",m_drv_fd, (unsigned int)pBuf);
                nReadbytes = read(m_drv_fd,m_tmp_out_meta_buf,output_buffer_size + sizeof(meta_out));
                DEBUG_DETAIL("FTBP->Al_len[%d]buf[%p]Loop[%d]size[%d]numOutBuf[%d]\n",\
                             (int)buffer->nAllocLen, buffer->pBuffer,
                             nRead, nReadbytes,nNumOutputBuf);

                if ((0 == nReadbytes) || (-1 == nReadbytes))
                {
                    DEBUG_PRINT("FTBP: breaking read since nReadbytes = 0 or -1");
                    buffer->nFilledLen = 0;
                    buffer->nTimeStamp= nTimestamp;
                    post_output((unsigned) & hComp,(unsigned) buffer,
                            OMX_COMPONENT_GENERATE_FRAME_DONE);
                    return OMX_ErrorNone;
                }

                // extract the metadata contents
                memcpy(&meta_out, m_tmp_out_meta_buf,sizeof(META_OUT));

                DEBUG_PRINT("FTBP->Meta:loop[%d]bytesread[%d]meta-len[%d]TS[0x%lld]"\
                            "nFlags[0x%lu]\n",\
                            nRead,nReadbytes, meta_out.offsetVal,
                            meta_out.nTimeStamp, meta_out.nFlags);

                if (nRead == 0)
                {
                    buffer->nTimeStamp = (meta_out.nTimeStamp);
                    DEBUG_PRINT("FTBP:Meta nRead=%d TS[%llu] TS[%lld]\n",\
                                nRead,buffer->nTimeStamp,meta_out.nTimeStamp);
                    nTimestamp = buffer->nTimeStamp;
                }

                // copy the pcm frame to the client buffer
                buffer->nFlags |= meta_out.nFlags;
                memcpy(pBuf, &m_tmp_out_meta_buf[sizeof(META_OUT)],
                       (nReadbytes - sizeof(META_OUT)));
#if DUMPS_ENABLE
		DEBUG_PRINT_ERROR("PCM dump to local buff");
		write(pfd, pBuf, (nReadbytes - sizeof(META_OUT)));
#endif

                // just reading msize, should it be guarded by mutex ?
                if(m_output_ctrl_cmd_q.m_size)
                {
                    DEBUG_DETAIL("FTBP-->: FLUSH CMD IN Q, STOP READING");
                    nDatalen = nDatalen + (nReadbytes - sizeof(META_OUT));
                    break;
                }
                pBuf += (nReadbytes - sizeof(META_OUT));
                nDatalen = nDatalen + (nReadbytes - sizeof(META_OUT));

                if (((buffer->nFlags) & 0x0001) == OMX_BUFFERFLAG_EOS)
                {
                    DEBUG_PRINT("FTBP: EOS reached Output port");
                    if(m_eos_bm){
                      DEBUG_PRINT("FTBP: END OF STREAM \n");
                      m_eos_bm |= OP_PORT_BITMASK; //output EOS recieved;
                    }
                    bEOSSent = 1;
                    DEBUG_PRINT("FTBP: END OF STREAM m_eos_bm=%d\n",m_eos_bm);
                    break;
                }
            }
            buffer->nFilledLen = nDatalen;
        }
        else
        {
            buffer->nFilledLen = 0;
            buffer->nFlags |= OMX_BUFFERFLAG_EOS;
            frame_done_cb((OMX_BUFFERHEADERTYPE *)buffer);
            return OMX_ErrorNone;
        }

        if(((nDatalen < 0) || ((unsigned)nDatalen > output_buffer_size)) && !(getSuspendFlg()))
        {
            buffer->nFilledLen = 0;
            frame_done_cb((OMX_BUFFERHEADERTYPE *)buffer);
            DEBUG_PRINT("FTBP-->: Invalid data length read \n");
            if ((buffer->nFlags & OMX_BUFFERFLAG_EOS) == OMX_BUFFERFLAG_EOS )
            {
                DEBUG_PRINT("FTBP: Now, Send EOS flag to Client \n");
                post_output((unsigned) & hComp,
                        (unsigned) buffer,OMX_COMPONENT_GENERATE_EOS);
                bEOSSent = 1;
                DEBUG_PRINT("FTBP: END OF STREAM m_eos_bm=%d\n",m_eos_bm);
            }
        }
        else
        {
           if ((buffer->nFlags & OMX_BUFFERFLAG_EOS) == OMX_BUFFERFLAG_EOS )
           {
              if(getSuspendFlg())
              {
                unsigned      p1; // Parameter - 1
                unsigned      p2; // Parameter - 2
                unsigned      id;
                bool          ret = false;
                fake_eos_recieved = true;

                // Faked EOS recieved, no processing of this mesg is reqd.
                ret = m_output_ctrl_cmd_q.get_msg_id(&id);
                if(ret && (id == OMX_COMPONENT_SUSPEND))
                {
                    m_output_ctrl_cmd_q.pop_entry(&p1,&p2,&id);
                }
                if(!m_eos_bm)
                {
                    // reset the flag as input eos was not set when this scenario happened
                    // o/p thread blocked on read() before faked_eos sent to dsp,
                    // now i/p thread fakes eos, o/p thread recieves the faked eos,
                    // since suspend flag is true, reset the nflags
                    buffer->nFlags = 0;
                }

                ioctl(m_drv_fd, AUDIO_STOP, 0);
                // now inform the client that the comp is SUSPENDED
                DEBUG_PRINT("\nFTBP: EOS reached Sending EH ctrlq=%d fake_eos=%d",\
                                               m_output_ctrl_cmd_q.m_size,fake_eos_recieved);
            }
            else
            {
                DEBUG_PRINT("FTBP: Now, Send EOS flag to Client \n");
                post_output((unsigned) & hComp,(unsigned) buffer,
                            OMX_COMPONENT_GENERATE_EOS);
                post_output((unsigned) & hComp,(unsigned) buffer,
                            OMX_COMPONENT_GENERATE_FRAME_DONE);
                return OMX_ErrorNone;
            }
           }
         DEBUG_PRINT("nState %d \n",nState );
            pthread_mutex_lock(&m_state_lock);
            get_state(&m_cmp, &state);
            pthread_mutex_unlock(&m_state_lock);

            if ( state == OMX_StatePause )
            {
                DEBUG_PRINT("FTBP:Post the FBD to event thread currstate=%d\n",\
                            state);
                post_output((unsigned) & hComp,(unsigned) buffer,
                            OMX_COMPONENT_GENERATE_FRAME_DONE);
            }
            else
            {
                frame_done_cb((OMX_BUFFERHEADERTYPE *)buffer);
            }
        }

    }
    else
    {
        DEBUG_PRINT("\n FTBP-->Invalid buffer in FTB \n");
    }

    return OMX_ErrorNone;
}

/*=============================================================================
FUNCTION:
fill_this_buffer

DESCRIPTION:
Function that handles fill this buffer command
This function meremly queue up the command and data would be consumed
in command server thread context

INPUT/OUTPUT PARAMETERS:
[INOUT] buffer - pointer to buffer header
[IN] hComp - handle to component instance

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::fill_this_buffer(
    OMX_IN OMX_HANDLETYPE         hComp,
    OMX_IN OMX_BUFFERHEADERTYPE* buffer)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    if(buffer->nSize != sizeof(OMX_BUFFERHEADERTYPE))
    {
        DEBUG_PRINT("omx_wma_adec::etb--> Buffer Size Invalid\n");
        return OMX_ErrorBadParameter;
    }
    if (m_out_bEnabled == OMX_FALSE)
    {
        return OMX_ErrorIncorrectStateOperation;
    }

    if(buffer->nVersion.nVersion != OMX_SPEC_VERSION)
    {
        DEBUG_PRINT("omx_wma_adec::etb--> OMX Version Invalid\n");
        return OMX_ErrorVersionMismatch;
    }
    if (buffer->nOutputPortIndex != OMX_CORE_OUTPUT_PORT_INDEX)
    {
        return OMX_ErrorBadPortIndex;
    }
    pthread_mutex_lock(&out_buf_count_lock);
    nNumOutputBuf++;
    DEBUG_DETAIL("FTB:nNumOutputBuf is %d", nNumOutputBuf);
    pthread_mutex_unlock(&out_buf_count_lock);
    post_output((unsigned)hComp,
        (unsigned) buffer,OMX_COMPONENT_GENERATE_FTB);
    return eRet;
}

/*=============================================================================
FUNCTION:
set_callbacks

DESCRIPTION:
Set the callbacks.

INPUT/OUTPUT PARAMETERS:
[IN] hComp - handle to component instance
[IN] callbacks
[IN] appData

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::set_callbacks(
    OMX_IN OMX_HANDLETYPE        hComp,
    OMX_IN OMX_CALLBACKTYPE* callbacks,
    OMX_IN OMX_PTR             appData)
{
    m_cb       = *callbacks;
    m_app_data =    appData;

    if(hComp == NULL)
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    return OMX_ErrorNone;
}

/*=============================================================================
FUNCTION:
component_deinit

DESCRIPTION:
Destroys the component and release memory allocated to the heap.

INPUT/OUTPUT PARAMETERS:
[IN] hComp - handle to component instance

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::component_deinit(OMX_IN OMX_HANDLETYPE hComp)
{
    if(hComp == NULL)
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }

    if (OMX_StateLoaded != m_state && OMX_StateInvalid != m_state)
    {
        DEBUG_PRINT_ERROR("Warning: Received DeInit when not in LOADED state, \
                          cur_state %d\n",m_state);
}
#if DUMPS_ENABLE
	if (bfd != -1)
		close(bfd);
	if (pfd != -1)
		close(pfd);
#endif
  deinit_decoder();
DEBUG_PRINT_ERROR("WMA component deinited\n");
  return OMX_ErrorNone;
}

/* ======================================================================
FUNCTION
  omx_wma_adec::deinit_decoder

DESCRIPTION
  Closes all the threads and release memory allocated to the heap.

PARAMETERS
  None.

RETURN VALUE
  None.

========================================================================== */
void  omx_wma_adec::deinit_decoder()
{
    DEBUG_PRINT("Component-deinit being processed\n");
    DEBUG_PRINT("********************************\n");



    m_out_act_buf_count = 0;
    m_inp_act_buf_count = 0;

    if(OMX_StateLoaded != m_state)
    {
        DEBUG_PRINT_ERROR("%s,Deinit called in state[%d]\n",__FUNCTION__,\
                                                                 m_state);
        // Get back any buffers from driver
        execute_omx_flush(-1,false);
        // force state change to loaded so that all threads can be exited
        pthread_mutex_lock(&m_state_lock);
        m_state = OMX_StateLoaded;
        pthread_mutex_unlock(&m_state_lock);
        DEBUG_PRINT_ERROR("Freeing Buf:inp_current_buf_count[%d][%d]\n",\
        m_inp_current_buf_count,
        m_input_buf_hdrs.size());
        m_input_buf_hdrs.eraseall();
        DEBUG_PRINT_ERROR("Freeing Buf:out_current_buf_count[%d][%d]\n",\
        m_out_current_buf_count,
        m_output_buf_hdrs.size());
        m_output_buf_hdrs.eraseall();
       if(suspensionPolicy == OMX_SuspensionEnabled)
       {
           ioctl(m_drv_fd,AUDIO_ABORT_GET_EVENT,NULL);
       }

    }

    pthread_mutex_lock(&m_in_th_lock_1);
    if ( is_in_th_sleep )
    {
        is_in_th_sleep = false;
        DEBUG_DETAIL("Deinit:WAKING UP IN THREADS\n");
        in_th_wakeup();
    }
    pthread_mutex_unlock(&m_in_th_lock_1);

    pthread_mutex_lock(&m_out_th_lock_1);
    if ( is_out_th_sleep )
    {
        is_out_th_sleep = false;
        DEBUG_DETAIL("SCP:WAKING UP OUT THREADS\n");
        out_th_wakeup();

    }
    pthread_mutex_unlock(&m_out_th_lock_1);
    if ( m_ipc_to_in_th != NULL )
    {
        omx_wma_thread_stop(m_ipc_to_in_th);
        m_ipc_to_in_th = NULL;
    }

    if (m_ipc_to_cmd_th != NULL)
    {
        omx_wma_thread_stop(m_ipc_to_cmd_th);
        m_ipc_to_cmd_th = NULL;
    }

    if(pcm_feedback ==1)
    {
        if (m_ipc_to_out_th != NULL)
        {
            omx_wma_thread_stop(m_ipc_to_out_th);
            m_ipc_to_out_th = NULL;
        }
    }
    bEOSSent = 0;
    if(suspensionPolicy == OMX_SuspensionEnabled)
    {
          ioctl(m_drv_fd,AUDIO_ABORT_GET_EVENT,NULL);
    }
    if(m_ipc_to_event_th != NULL)
    {

        omx_wma_thread_stop(m_ipc_to_event_th);
          m_ipc_to_event_th = NULL;
    }
    m_inp_current_buf_count=0;
    m_out_current_buf_count=0;
    nNumInputBuf = 0;
    nNumOutputBuf = 0;
    m_first = 0;
    bTsupdate = 0;
    wma_packet_timestamp = 0;

    resetSuspendFlg();
    resetResumeFlg();
    fake_eos_recieved = false;
    m_eos_bm=false;
    m_pause_to_exe=false;
    bFlushinprogress = 0;
    m_inp_bEnabled = OMX_TRUE;
    m_out_bEnabled = OMX_TRUE;
    m_inp_bPopulated = OMX_FALSE;
    m_out_bPopulated = OMX_FALSE;
    packet_num =0;
    bOutputPortReEnabled = false;
    bSendWMAPacketpending = false;

    if (m_drv_fd >= 0)
    {
        close(m_drv_fd);
    }
    else
    {
        DEBUG_PRINT_ERROR(" wma device already closed\n");
    }
    m_comp_deinit=1;
    m_is_out_th_sleep = 1;
    m_is_in_th_sleep = 1;
    if(m_timer)
        delete m_timer;
    if(m_bufMgr)
        delete m_bufMgr;
    DEBUG_PRINT("************************************\n");
    DEBUG_PRINT(" DEINIT COMPLETED");
    DEBUG_PRINT("************************************\n");
}

/*=============================================================================
FUNCTION:
use_EGL_image

DESCRIPTION:
OMX Use EGL Image method implementation <TBD>.

INPUT/OUTPUT PARAMETERS:
[INOUT] bufferHdr - pointer to buffer header
[IN] hComp - handle to component instance
[IN] port
[IN] appData
[IN] eglImage

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::use_EGL_image(
    OMX_IN OMX_HANDLETYPE                hComp,
    OMX_INOUT OMX_BUFFERHEADERTYPE**     bufferHdr,
    OMX_IN OMX_U32                       port,
    OMX_IN OMX_PTR                       appData,
    OMX_IN void*                         eglImage)
{
    DEBUG_PRINT_ERROR("Error : use_EGL_image:  Not Implemented \n");
    if((hComp == NULL) || (bufferHdr == NULL) || (appData == NULL) ||
        (eglImage == NULL))
    {
        port = 0;
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    return OMX_ErrorNotImplemented;
}

/*=============================================================================
FUNCTION:
component_role_enum

DESCRIPTION:
OMX Component Role Enum method implementation.

INPUT/OUTPUT PARAMETERS:
[OUT] role
[IN] hComp - handle to component instance
[IN] index

RETURN VALUE:
OMX_ERRORTYPE

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE  omx_wma_adec::component_role_enum(
    OMX_IN OMX_HANDLETYPE  hComp,
    OMX_OUT OMX_U8*        role,
    OMX_IN OMX_U32         index)
{
    OMX_ERRORTYPE eRet = OMX_ErrorNone;
    const char *cmp_role = "audio_decoder.wma";

    if(hComp == NULL)
    {
        DEBUG_PRINT_ERROR("Returning OMX_ErrorBadParameter\n");
        return OMX_ErrorBadParameter;
    }
    if(index == 0 && role)
    {
        memcpy(role, cmp_role, sizeof(cmp_role));
        *(((char *) role) + sizeof(cmp_role)) = '\0';
    }
    else
    {
        eRet = OMX_ErrorNoMore;
    }
    return eRet;
}

/*=============================================================================
FUNCTION:
allocate_done

DESCRIPTION:
Checks if entire buffer pool is allocated by IL Client or not.
Need this to move to IDLE state.

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::allocate_done(void)
{
    OMX_BOOL bRet = OMX_FALSE;
    if(pcm_feedback==1)
    {
        if ( (m_inp_act_buf_count == m_inp_current_buf_count)
            &&(m_out_act_buf_count == m_out_current_buf_count) )
        {
            bRet=OMX_TRUE;

        }
        if ( (m_inp_act_buf_count == m_inp_current_buf_count) && m_inp_bEnabled )
        {
            m_inp_bPopulated = OMX_TRUE;
        }

        if ( (m_out_act_buf_count == m_out_current_buf_count) && m_out_bEnabled )
        {
            m_out_bPopulated = OMX_TRUE;
        }
    } else if ( pcm_feedback==0 )
    {
        if ( m_inp_act_buf_count == m_inp_current_buf_count )
        {
            bRet=OMX_TRUE;

        }
        if ( (m_inp_act_buf_count == m_inp_current_buf_count) && m_inp_bEnabled )
        {
            m_inp_bPopulated = OMX_TRUE;
        }

    }

    return bRet;
}


/*=============================================================================
FUNCTION:
release_done

DESCRIPTION:
Checks if IL client has released all the buffers.

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
true
false

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
bool omx_wma_adec::release_done(OMX_U32 param1)
{
    DEBUG_PRINT("Inside omx_wma_adec::release_done");

    OMX_BOOL bRet = OMX_FALSE;
    if ( param1 == OMX_ALL )
    {
        if ( (0 == m_inp_current_buf_count)&&(0 == m_out_current_buf_count) )
        {
            bRet=OMX_TRUE;
        }
    } else if ( param1 == OMX_CORE_INPUT_PORT_INDEX )
    {
        if ( (0 == m_inp_current_buf_count) )
        {
            bRet=OMX_TRUE;
        }
    } else if ( param1 == OMX_CORE_OUTPUT_PORT_INDEX )
    {
        if ( (0 == m_out_current_buf_count) )
        {
            bRet=OMX_TRUE;
        }
    }
    return bRet;
}

/*=============================================================================
FUNCTION:
transcode_wma

DESCRIPTION:
Does the transcoding of WMA Payload to Qcom format

INPUT/OUTPUT PARAMETERS:
Source pointer

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE omx_wma_adec::transcode_wma(OMX_HANDLETYPE hComp,
                                          OMX_U8 *srcStart ,
                                          OMX_BUFFERHEADERTYPE* buffer)
{
    OMX_U16 u16Temp = 0;
    int nSize = 0;
    bool spillover_payload = (m_adec_param.nEncodeOptions & 0x02)?true:false;
    OMX_U8  *src;


    if((srcStart == NULL) || (buffer->nFilledLen == 0))
    {
        DEBUG_PRINT(" no source on input stream\n");
        if(m_trans_buffer_length != 0)
        {
            if (m_trans_buffer_length%2 != 0)
            {
                memset(m_trans_buffer + m_trans_buffer_length,0x0,1);
                m_trans_buffer_length++;
            }

            if(buffer->nFlags & OMX_BUFFERFLAG_EOS)
            {
                nSize = WMA_DSP_MIN_BUF_SIZE - m_trans_buffer_length;
                if(nSize > 0)
                {
                    m_trans_buffer[m_trans_buffer_length++] = 0xff;
                    m_trans_buffer[m_trans_buffer_length++] = 0xf0;
                    m_trans_buffer[m_trans_buffer_length++] = 0;
                    m_trans_buffer[m_trans_buffer_length++] = 0;
                    m_trans_buffer[m_trans_buffer_length++] = 0;
                    m_trans_buffer[m_trans_buffer_length++] = 0;

                   memset(m_trans_buffer+m_trans_buffer_length, WMA_DUMMY_BYTE, nSize);
                   m_trans_buffer_length += nSize;
                }
            }

            SendWMAPacket(buffer);
            bTsupdate = 0;
        }
        /*EOS set with no payload*/
        if(buffer->nFlags & OMX_BUFFERFLAG_EOS)
        {
            SendDummyPacket(buffer);
            ntotal_playtime = buffer->nTimeStamp;
            if ( pcm_feedback == 0 )
            {
                fsync(m_drv_fd);
                post_input((unsigned) & hComp,
                    (unsigned) buffer,
                    OMX_COMPONENT_GENERATE_EOS);
            }
        }
        return OMX_ErrorNone;
    }


    int cNumFrm;
    int cPacketNum;
    int nSpillOverBits;
    int nSrcBitOffset = 0;
    int nPrevBytes = 0;
    int nPrevExtraBits = 0;
    int srcoffset =0;
    if(spillover_payload)
    {

    float bits_Per_Sample = ((float)m_adec_param.nBitRate)
                    /(m_adec_param.nSamplingRate * m_adec_param.nChannels);
        m_dwSpillOverBitsLength = logarithm(ceil(bits_Per_Sample
            * WmAudioGetSamplesPerFrame(m_adec_param.nSamplingRate, 2)/8.0f))+2+3;
        DEBUG_PRINT("m_dwSpillOverBitsLength =%d \n", (int)m_dwSpillOverBitsLength);

        for(OMX_U32 itr = 0;itr < (buffer->nFilledLen/m_adec_param.nBlockAlign); itr++)
        {

            int nCurrLoc = 0;
            src=srcStart+srcoffset;
            /* first 4 bits are number of frames in packet */
            cNumFrm = (src[nCurrLoc] & 0x0F);

            /* next 4 bits are packet number */
            cPacketNum = (src[nCurrLoc] & 0xF0) >> 4;

            DEBUG_PRINT("Current Packet Id %d, number of Frames %d m_notfirstblock %d\n",\
                cPacketNum, cNumFrm, m_notfirstblock);

            nCurrLoc++;

            /* next m_dwSpillOverBitsLength bits have number of bits
            of previous frame in packet */
            nSpillOverBits = 0;

            for(int i=0; i<(int)m_dwSpillOverBitsLength/8; i++)
            {
                nSpillOverBits = nSpillOverBits << 8;
                nSpillOverBits = nSpillOverBits | src[nCurrLoc + i];
            }

            nCurrLoc += (m_dwSpillOverBitsLength/8);

            if(m_dwSpillOverBitsLength%8)
            {
                OMX_U8 cTemp = src[nCurrLoc] >> (8 - (m_dwSpillOverBitsLength%8));
                nSpillOverBits = nSpillOverBits << (m_dwSpillOverBitsLength%8);
                nSpillOverBits = nSpillOverBits | cTemp;
            }

            nSrcBitOffset = m_dwSpillOverBitsLength + 8 + nSrcBitOffset;

            /* first copy previous frame bits */
            nPrevBytes = nSpillOverBits/8;
            nPrevExtraBits = nSpillOverBits%8;

            DEBUG_PRINT("Number of spill over bits %d Previous Bytes %d & Bits %d \n",\
                nSpillOverBits, nPrevBytes, nPrevExtraBits);
            DEBUG_PRINT(" Bits to be skiped(nSrcBitOffset) %d \n",\
                nSrcBitOffset);

            if(m_notfirstblock == TRUE)
            {
                /*Copy the previous bits to buffer*/
                for (int i = 0; i < nPrevBytes; i++)
                {
                    GetByteFromBitStream(m_trans_buffer + m_trans_buffer_length,
                        src, nSrcBitOffset + (i*8), 8);
                    m_trans_buffer_length++;
                }

                if (nPrevExtraBits)
                {
                    GetByteFromBitStream(m_trans_buffer + m_trans_buffer_length, src,
                        nSrcBitOffset + (nPrevBytes*8), 8);
                    m_trans_buffer_length++;
                }
                /*Transcoded packet is ready, send data to Audio Driver*/

                if (m_trans_buffer_length%2 != 0)
                {
                    memset(m_trans_buffer + m_trans_buffer_length,0x0,1);
                    m_trans_buffer_length++;
                }

               if (m_trans_buffer_length < WMA_DSP_MIN_BUF_SIZE) {
		     memset(m_trans_buffer+ m_trans_buffer_length, 0,
		      (WMA_DSP_MIN_BUF_SIZE - m_trans_buffer_length));
		     m_trans_buffer_length = WMA_DSP_MIN_BUF_SIZE;
	       }

               SendWMAPacket(buffer);
               bTsupdate = 0;
            }

           int nSkipBytes = (nSrcBitOffset + nSpillOverBits)/8;
           int nSkipBits  = (nSrcBitOffset + nSpillOverBits);

           if (!bTsupdate) {
                     wma_packet_timestamp = buffer->nTimeStamp;
                     DEBUG_PRINT("buffer->nTimeStamp - %lld, wma_packet_timestamp - %lld\n",
                            buffer->nTimeStamp, wma_packet_timestamp);
                     bTsupdate = 1;
            }

            /*Prepare the Header*/
            /* sync + lost bits */
            m_trans_buffer[m_trans_buffer_length++] = 0xff;
            m_trans_buffer[m_trans_buffer_length++] = 0xf0;

            /* size of frame in words with extra byte to make it word aligned */
            u16Temp = (m_adec_param.nBlockAlign)/2;  /* size of packet in words */

            m_trans_buffer[m_trans_buffer_length++] = *(((OMX_U8*)&u16Temp)+1);
            m_trans_buffer[m_trans_buffer_length++] = *((OMX_U8*)&u16Temp);

            /* number of frames in this packet */
            m_trans_buffer[m_trans_buffer_length++] = cNumFrm;
            /* start bit */
            m_trans_buffer[m_trans_buffer_length++] = (nSkipBits%8)<<4;

            memcpy (m_trans_buffer+ m_trans_buffer_length, (srcStart + nSkipBytes)
                , (m_adec_param.nBlockAlign - nSkipBytes));
            m_trans_buffer_length += (m_adec_param.nBlockAlign - nSkipBytes);
            m_notfirstblock = TRUE;
        srcoffset+=m_adec_param.nBlockAlign;

        }
    packet_num++;

    }
    else
    {
        for(OMX_U32 itr = 0;itr < (buffer->nFilledLen/m_adec_param.nBlockAlign); itr++)
        {

           src =srcStart+srcoffset;
               if (m_trans_buffer_length < WMA_DSP_MIN_BUF_SIZE) {
		     memset(m_trans_buffer+ m_trans_buffer_length, 0,
		      (WMA_DSP_MIN_BUF_SIZE - m_trans_buffer_length));
		     m_trans_buffer_length = WMA_DSP_MIN_BUF_SIZE;
	       }

           SendWMAPacket(buffer);
           bSendWMAPacketpending = false;
           bTsupdate = 0;

           if (!bTsupdate) {
               wma_packet_timestamp = buffer->nTimeStamp;
               DEBUG_PRINT("buffer->nTimeStamp - %lld, wma_packet_timestamp - %lld\n",
                                buffer->nTimeStamp, wma_packet_timestamp);
               bTsupdate = 1;
           }

            /*Prepare the Header*/
            /* sync + lost bits */
            m_trans_buffer[m_trans_buffer_length++] = 0xff;
            m_trans_buffer[m_trans_buffer_length++] = 0xf0;

            /* size of frame in words with extra byte to make it word aligned */
            u16Temp = (m_adec_param.nBlockAlign)/2;  /* size of packet in words */

            m_trans_buffer[m_trans_buffer_length++] = *(((OMX_U8*)&u16Temp)+1);
            m_trans_buffer[m_trans_buffer_length++] = *((OMX_U8*)&u16Temp);

            /* number of frames in this packet */
            m_trans_buffer[m_trans_buffer_length++] = 1;
            /* start bit */
            m_trans_buffer[m_trans_buffer_length++] = 0;

            memcpy (m_trans_buffer+ m_trans_buffer_length, src
                , m_adec_param.nBlockAlign);
            m_trans_buffer_length += m_adec_param.nBlockAlign;
            srcoffset+=m_adec_param.nBlockAlign;

           if (m_trans_buffer_length%2 != 0)
           {
               memset(m_trans_buffer + m_trans_buffer_length,0x0,1);
               m_trans_buffer_length++;
           }

        }
        /*Transcoded packet is ready, send data to Audio Driver*/
    packet_num++;
    }
    /* Handling case with Valid payload and EOS */
    if(buffer->nFlags == OMX_BUFFERFLAG_EOS) /*last packet*/
    {
        /* send the last WMA packet and also dummy data to Audio Driver*/
        if( m_trans_buffer_length != 0)
        {
            nSize = WMA_DSP_MIN_BUF_SIZE - m_trans_buffer_length;
            if(nSize > 0)
            {
                 m_trans_buffer[m_trans_buffer_length++] = 0xff;
                 m_trans_buffer[m_trans_buffer_length++] = 0xf0;
                 m_trans_buffer[m_trans_buffer_length++] = 0;
                 m_trans_buffer[m_trans_buffer_length++] = 0;
                 m_trans_buffer[m_trans_buffer_length++] = 0;
                 m_trans_buffer[m_trans_buffer_length++] = 0;

                 memset(m_trans_buffer+m_trans_buffer_length, WMA_DUMMY_BYTE, nSize);

                 m_trans_buffer_length += nSize;
            }

            SendWMAPacket(buffer);
            bTsupdate = 0;
        }

        SendDummyPacket(buffer);
        ntotal_playtime = buffer->nTimeStamp;
        if ( pcm_feedback == 0 )
        {
            fsync(m_drv_fd);
            post_input((unsigned) & hComp,(unsigned) buffer,OMX_COMPONENT_GENERATE_EOS);
        }
    }
    return OMX_ErrorNone;
}

/*=============================================================================
FUNCTION:
transcode_wma10Pro

DESCRIPTION:
Does the transcoding of WMA Payload to Qcom format

INPUT/OUTPUT PARAMETERS:
Source pointer

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
OMX_ERRORTYPE omx_wma_adec::transcode_wma10Pro(OMX_HANDLETYPE hComp,
                                          OMX_U8 *srcStart ,
                                          OMX_BUFFERHEADERTYPE* buffer)
{
    OMX_U16 u16Temp = 0;
    OMX_U8  *src;
    if((srcStart == NULL) || (buffer->nFilledLen == 0))
    {
        DEBUG_PRINT(" no source on input stream\n");
        if(m_trans_buffer_length != 0)
        {
            if (m_trans_buffer_length%2 != 0)
            {
                memset(m_trans_buffer + m_trans_buffer_length,0x0,1);
                m_trans_buffer_length++;
            }
                SendWMAPacket(buffer);
                bTsupdate = 0;

        }
        /*EOS set with no payload*/
        if(buffer->nFlags & OMX_BUFFERFLAG_EOS)
        {
            SendDummyPacket(buffer);
            ntotal_playtime = buffer->nTimeStamp;
            if ( pcm_feedback == 0 )
            {
                fsync(m_drv_fd);
                post_input((unsigned) & hComp,
                    (unsigned) buffer,
                    OMX_COMPONENT_GENERATE_EOS);
            }
        }
        return OMX_ErrorNone;
    }


    int cNumFrm;
    int cPacketNum;
    int nSpillOverBits;
    int nSrcBitOffset = 0;
    int nPrevBytes = 0;
    int nPrevExtraBits = 0;
    int srcoffset =0;
    int bits_left = 0;
    int seekable_frame_in_packet= 0;
    int spliced_packet = 0;
    int two_bits_in_header = 0;
    int nSpillOverBits_m =0;

        m_dwSpillOverBitsLength=logarithm(buffer->nFilledLen * 8 ) + 1;
        DEBUG_PRINT("m_dwSpillOverBitsLength =%d \n", (int)m_dwSpillOverBitsLength);

        for(OMX_U32 itr = 0;itr < (buffer->nFilledLen/m_adec_param_wma10Pro.nBlockAlign); itr++)
        {

            int nCurrLoc = 0;
            src=srcStart+srcoffset;
            /* first 4 bits are number of packet number */

            cNumFrm = 1;
            /* packet_number - 4 bits */
            /* seekable_frame_in_packet - 1 bit */
            /* spliced_packet - 1 bit */
            /* seek_offset_in_bits  - */


            cPacketNum = (src[nCurrLoc] & 0xF0) >> 4;
              seekable_frame_in_packet = (src[nCurrLoc] & 0x08) >> 3;
            spliced_packet           = (src[nCurrLoc] & 0x04) >> 2;
            two_bits_in_header       = (src[nCurrLoc] & 0x03);
            nSpillOverBits_m           = two_bits_in_header;


            DEBUG_PRINT("Current Packet Id %d, number of Frames %d m_notfirstblock %d\n",\
                cPacketNum, cNumFrm, m_notfirstblock);

            nCurrLoc++;

            /* next m_dwSpillOverBitsLength bits have number of bits
            of previous frame in packet */
            nSpillOverBits = 0;

            for(int i=0; i<(int)m_dwSpillOverBitsLength/8; i++)
            {
                nSpillOverBits = nSpillOverBits << 8;
                nSpillOverBits = nSpillOverBits | src[nCurrLoc + i];
            }

            nCurrLoc += (m_dwSpillOverBitsLength/8);
            bits_left = (m_dwSpillOverBitsLength%8);
            if(m_dwSpillOverBitsLength%8)
            {
                OMX_U8 cTemp = src[nCurrLoc] >> (8 - (bits_left));
                nSpillOverBits = nSpillOverBits << (bits_left);
                nSpillOverBits = nSpillOverBits | cTemp;
            }
            nSpillOverBits_m <<= (m_dwSpillOverBitsLength - 2);
            nSpillOverBits >>= 2;
            nSpillOverBits |=nSpillOverBits_m;

            nSrcBitOffset = m_dwSpillOverBitsLength + 6 + nSrcBitOffset;

            /* first copy previous frame bits */
            nPrevBytes = nSpillOverBits/8;
            nPrevExtraBits = nSpillOverBits%8;

	    if (nSpillOverBits > (signed)((buffer->nFilledLen * 8)-nSrcBitOffset))
	    {
		    DEBUG_PRINT("Spill over size greater than packet size\n");
		    nPrevBytes = ((buffer->nFilledLen * 8)-nSrcBitOffset)/8;
		    nPrevExtraBits = ((buffer->nFilledLen * 8)-nSrcBitOffset)%8;
	    }

	    DEBUG_PRINT("Number of spill over bits %d Previous Bytes %d & Bits %d \n",\
                nSpillOverBits, nPrevBytes, nPrevExtraBits);
            DEBUG_PRINT(" Bits to be skiped(nSrcBitOffset) %d \n",\
                nSrcBitOffset);

            if(m_notfirstblock == TRUE)
            {
                /*Copy the previous bits to buffer*/
                for (int i = 0; i < nPrevBytes; i++)
                {
                    GetByteFromBitStream(m_trans_buffer + m_trans_buffer_length,
                        src, nSrcBitOffset + (i*8), 8);
                    m_trans_buffer_length++;
                }

                if (nPrevExtraBits)
                {
                    GetByteFromBitStream(m_trans_buffer + m_trans_buffer_length, src,
                        nSrcBitOffset + (nPrevBytes*8), 8);
                    m_trans_buffer_length++;
                }
                /*Transcoded packet is ready, send data to Audio Driver*/



               if (m_trans_buffer_length%2 != 0)
               {
                   memset(m_trans_buffer + m_trans_buffer_length,0x0,1);
                   m_trans_buffer_length++;
               }

               if(bBufferAfterFlush) {
                     m_trans_buffer_length = 0;
                     bBufferAfterFlush = FALSE;
               }

               if (m_trans_buffer_length < WMA_DSP_MIN_BUF_SIZE) {
                      memset(m_trans_buffer+ m_trans_buffer_length, 0,
                          (WMA_DSP_MIN_BUF_SIZE - m_trans_buffer_length));
                      m_trans_buffer_length = WMA_DSP_MIN_BUF_SIZE;
               }
               SendWMAPacket(buffer);
               bTsupdate = 0;
            }
            if (!bTsupdate) {
                     wma_packet_timestamp = buffer->nTimeStamp;
                     DEBUG_PRINT("buffer->nTimeStamp - %lld, wma_packet_timestamp - %lld\n",
                               buffer->nTimeStamp, wma_packet_timestamp);
                     bTsupdate = 1;
            }

            if (nSpillOverBits < (signed)((buffer->nFilledLen * 8)-nSrcBitOffset))
            {
            int nSkipBytes = (nSrcBitOffset + nSpillOverBits)/8;
            int nSkipBits  = (nSrcBitOffset + nSpillOverBits);


            /*Prepare the Header*/
            /* sync + lost bits */
            m_trans_buffer[m_trans_buffer_length++] = 0xff;
            m_trans_buffer[m_trans_buffer_length++] = 0xf0;

            /* size of frame in words with extra byte to make it word aligned */
            u16Temp = (m_adec_param_wma10Pro.nBlockAlign)/2;  /* size of packet in words */

            m_trans_buffer[m_trans_buffer_length++] = *(((OMX_U8*)&u16Temp)+1);
            m_trans_buffer[m_trans_buffer_length++] = *((OMX_U8*)&u16Temp);

            /* number of frames in this packet */
            m_trans_buffer[m_trans_buffer_length++] = 1;
            /* start bit */
            m_trans_buffer[m_trans_buffer_length] = (nSkipBits%8)<<4;

            if(seekable_frame_in_packet)
            m_trans_buffer[m_trans_buffer_length]|= SEEKABLE_BIT;

            if(spliced_packet)
            m_trans_buffer[m_trans_buffer_length]|= SPLICE_BIT;
            m_trans_buffer_length++;
            memcpy (m_trans_buffer+ m_trans_buffer_length, (srcStart + nSkipBytes)
                , (m_adec_param_wma10Pro.nBlockAlign - nSkipBytes));

            m_trans_buffer_length += (m_adec_param_wma10Pro.nBlockAlign - nSkipBytes);
	    }

            m_notfirstblock = TRUE;

            srcoffset+=m_adec_param_wma10Pro.nBlockAlign;

        }
    packet_num++;


    /* Handling case with Valid payload and EOS */
    if(buffer->nFlags == OMX_BUFFERFLAG_EOS) /*last packet*/
    {
        /* send the last WMA packet and also dummy data to Audio Driver*/
    if( m_trans_buffer_length != 0)
    {
            SendWMAPacket(buffer);
            bTsupdate = 0;
    }

        SendDummyPacket(buffer);
        ntotal_playtime = buffer->nTimeStamp;
        if ( pcm_feedback == 0 )
        {
            fsync(m_drv_fd);
            post_input((unsigned) & hComp,(unsigned) buffer,OMX_COMPONENT_GENERATE_EOS);
        }
    }
    return OMX_ErrorNone;
}
/*=============================================================================
FUNCTION:
GetByteFromBitStream

DESCRIPTION:
Extracts byte from bitstream

INPUT/OUTPUT PARAMETERS:
Number of bits

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::GetByteFromBitStream(OMX_U8 *pByte,
                                        OMX_U8 *pSrc,
                                        int nFromBit,
                                        int nBits)
{
    int a, b, i;
    OMX_U8 temp;


    if(nBits > 8)
        return;

    *pByte = 0;       /* reset all the bits */

    for( i=0; i<nBits; i++)
    {
        a = nFromBit/8;
        b = nFromBit%8;
        *pByte = *pByte << 1;     /* make space for next bit */
        temp = pSrc[a] << b;
        *pByte |= temp >> 7;      /* OR after masking all other bits */
        nFromBit++;
    }
} /* GetByteFromBitStream */

/*=============================================================================
FUNCTION:
WmAudioGetSamplesPerFrame

DESCRIPTION:
Gets SamplesPerSec depending on version

INPUT/OUTPUT PARAMETERS:
Version/Samplesperframe

RETURN VALUE:
Samplesperframe

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
unsigned long omx_wma_adec::WmAudioGetSamplesPerFrame(unsigned long SamplesPerSec,
                                                      unsigned int nVersion)
{
    unsigned long dwSamplesPerFrame = 0;

    if(!SamplesPerSec)
    {

        return dwSamplesPerFrame;
    }

    if(nVersion == 1)
    {
        if( SamplesPerSec<=16000 )
        {
            dwSamplesPerFrame = 512;
        }
        else if(SamplesPerSec <= 32000 )
        {
            dwSamplesPerFrame = 1024;
        }
        else if(SamplesPerSec > 32000 )
        {
            dwSamplesPerFrame = 2048;
        }
    }
    else if (nVersion == 2)
    {
        if( SamplesPerSec<=16000 )
        {
            dwSamplesPerFrame = 512;
        }
        else if(SamplesPerSec <= 22050 )
        {
            dwSamplesPerFrame = 1024;
        }
        else if(SamplesPerSec > 22050 )
        {
            dwSamplesPerFrame = 2048;
        }
    }

    if(!dwSamplesPerFrame)
    {
        DEBUG_PRINT("Unsupported format\n");
    }
    return dwSamplesPerFrame;
} /* WmAudioGetSamplesPerFrame */

/*=============================================================================
FUNCTION:
logarithm

DESCRIPTION:
Gets SamplesPerSec depending on version

INPUT/OUTPUT PARAMETERS:
Input argument can be any valid numeric expression greater than 0

RETURN VALUE:
logarithm

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
int  omx_wma_adec::logarithm(int n )
{
    int iLog = -1;
    while(TRUE)
    {
        if (n == 0)
            break;
        n = n >> 1;
        iLog++;
    }
    return iLog;
} /*logarithm*/

/*=============================================================================
FUNCTION:
SendWMAPacket

DESCRIPTION:
Send the transcoded WMA packet to driver

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::SendWMAPacket(OMX_BUFFERHEADERTYPE* buffer)
{
    OMX_U8 *data=NULL;
    META_IN meta_in;

    if (pcm_feedback && m_tmp_in_meta_buf)//Non-Tunnelled mode
    {
        data = m_tmp_in_meta_buf;
        // copy the metadata info from the BufHdr and insert to payload
        meta_in.offsetVal  = sizeof(META_IN);
        meta_in.nTimeStamp = wma_packet_timestamp;
        meta_in.nFlags     = 0;
        memcpy(data,&meta_in, meta_in.offsetVal);
        memcpy(&data[sizeof(META_IN)], m_trans_buffer_start,m_trans_buffer_length);
        DEBUG_PRINT("ETBP->Meta:meta-len[%d]TS[0x%llu]"\
                    "nFlags[0x%lu]\n",\
                     meta_in.offsetVal,
                     meta_in.nTimeStamp, meta_in.nFlags);
    }

    if(pcm_feedback)
    {
        if(data == NULL)
        {
            DEBUG_PRINT_ERROR("ETBP: unable to write: failed to allocate m_tmp_meta_buf\n");
        }
        else
        {  
            write(m_drv_fd, data , m_trans_buffer_length + sizeof(META_IN));
#if DUMPS_ENABLE
		DEBUG_PRINT_ERROR("bitstream dump to local buff");
		write(bfd, (data+sizeof(META_IN)), m_trans_buffer_length);
#endif
        }
    }
    else
    {
        write(m_drv_fd, m_trans_buffer_start, m_trans_buffer_length);
    }
    DEBUG_PRINT("Empty buffer to kernel driver\n");
    DEBUG_PRINT("m_trans_buffer_length length = %d\n", (int)m_trans_buffer_length);
    m_trans_buffer = m_trans_buffer_start;
    m_trans_buffer_length = 0;
    packet_num = 0;

}
/*=============================================================================
FUNCTION:
SendDummypacket

DESCRIPTION:
Send the dummy bytes to driver to notify end of playback

INPUT/OUTPUT PARAMETERS:
None

RETURN VALUE:
None

Dependency:
None

SIDE EFFECTS:
None
=============================================================================*/
void omx_wma_adec::SendDummyPacket(OMX_BUFFERHEADERTYPE* buffer)

{
    OMX_U32 DummyBytesSent = 0;
    META_IN meta_in;
    OMX_U8 *data=NULL;
    /* End of file is reached ,send dummy bytes to driver to notify end of playback*/
    m_trans_buffer[m_trans_buffer_length++] = 0xff;
    m_trans_buffer[m_trans_buffer_length++] = 0xf0;
    m_trans_buffer[m_trans_buffer_length++] = 0;
    m_trans_buffer[m_trans_buffer_length++] = 0;
    m_trans_buffer[m_trans_buffer_length++] = 0;
    m_trans_buffer[m_trans_buffer_length++] = 0;

    do{
        int nSize = 0;
        nSize = WMA_DSP_MIN_BUF_SIZE;
        memset(m_trans_buffer+m_trans_buffer_length, WMA_DUMMY_BYTE, nSize);
        m_trans_buffer_length += nSize;
        if(pcm_feedback)
        {
            data = m_tmp_in_meta_buf;
            memset(&meta_in,0,sizeof(META_IN));
            meta_in.offsetVal  = sizeof(META_IN);
            meta_in.nTimeStamp = (((OMX_BUFFERHEADERTYPE*)buffer)->nTimeStamp);
            meta_in.nFlags     &= ~OMX_BUFFERFLAG_EOS;
            memcpy(data,&meta_in, meta_in.offsetVal);
            memcpy(&data[sizeof(META_IN)], m_trans_buffer_start,m_trans_buffer_length);
            write(m_drv_fd, data, m_trans_buffer_length + sizeof(META_IN));
        }
        else
        {
            write(m_drv_fd, m_trans_buffer_start, m_trans_buffer_length);
        }
        DEBUG_PRINT("m_trans_buffer_length length = %d\n", (int)m_trans_buffer_length);
        DEBUG_PRINT("Writing the Dummy Bytes to kernel\n");
        DummyBytesSent += nSize;
        m_trans_buffer_length= 0;
    }while (DummyBytesSent < WMA_TOTAL_DUMMY_BYTES_TO_SEND);

    m_trans_buffer = m_trans_buffer_start;
    m_trans_buffer_length = 0;
    m_notfirstblock = FALSE;
    DEBUG_PRINT("EOS OCCURED \n");
    DEBUG_PRINT("Writting the Final EOS data to Driver ");

    if(pcm_feedback && m_tmp_in_meta_buf)
    {
        data = m_tmp_in_meta_buf;
        meta_in.offsetVal  = sizeof(META_IN);
        meta_in.nTimeStamp = (((OMX_BUFFERHEADERTYPE*)buffer)->nTimeStamp);
        meta_in.nFlags     = buffer->nFlags;
        memcpy(data,&meta_in, meta_in.offsetVal);
        write(m_drv_fd,data,sizeof(META_IN));
    }
    else
    {
       write(m_drv_fd, m_trans_buffer_start,0);
    }

} /*SendDummypacket*/

bool omx_wma_adec::omx_wma_fake_eos()
{
    int ret;
    META_IN meta_in;

    // FAKE an EOS
    // copy the metadata info from the BufHdr and insert to payload
    meta_in.offsetVal  = sizeof(META_IN);
    meta_in.nTimeStamp = nTimestamp;
    meta_in.nFlags     = OMX_BUFFERFLAG_EOS;
    ret = write(m_drv_fd, &meta_in, sizeof(META_IN));
    DEBUG_PRINT("omx_wma_fake_eos :ret=%d", ret);
    return true;
}
void omx_wma_adec::append_data_to_temp_buf()
{
    int bytes_read = 0;
    unsigned int tot_bytes = 0;
    unsigned int cur_bytes = 0;
    META_OUT meta_out;
    META_OUT_SUS meta_out_sus;

    DEBUG_PRINT("APPEND:SUSPEND: output_buffer_size = %d\n", output_buffer_size);

    while( 1)
    {
        bytes_read = read(m_drv_fd, m_tmp_out_meta_buf,
                                      (OMX_CORE_OUTPUT_BUFFER_SIZE + sizeof(META_OUT)));
        DEBUG_PRINT("Append-->current bytes_read=%d total=%d\n",bytes_read,tot_bytes);
        if(bytes_read > 0)
        {
            memcpy(&meta_out, m_tmp_out_meta_buf, sizeof(META_OUT));

            /* user meta out offset to store the pcm frame length*/
            meta_out_sus.offsetVal =(OMX_U32)bytes_read - sizeof(META_OUT);
            meta_out_sus.nTimeStamp = meta_out.nTimeStamp;
            meta_out_sus.nFlags = meta_out.nFlags;
            if((meta_out.nFlags & 0x001) == OMX_BUFFERFLAG_EOS)
            {
                DEBUG_PRINT("**********READ=0**********************");
                DEBUG_PRINT("Append:EOS found Residual pcm[%d][%lu]",tot_bytes,
                                               m_bufMgr->getBufFilledSpace());
                DEBUG_PRINT("**********READ=0**********************");
                break;
            }
            else
            {
                if(m_bufMgr->getBufFreeSpace() >= (bytes_read + sizeof(META_OUT_SUS)))
                {
                       cur_bytes = m_bufMgr->appendToBuf(
                                         (OMX_U8 *)&meta_out_sus,
                                         sizeof(META_OUT_SUS));
                       if(cur_bytes == 0)
                       {
                            DEBUG_PRINT_ERROR("Append: Reject residual pcm,"
                                     " no more space \n");
                            break;
                       }
                       else
                       {
                           tot_bytes += cur_bytes;
                           cur_bytes = m_bufMgr->appendToBuf(
                                      &m_tmp_out_meta_buf[sizeof(META_OUT)],
                                      (bytes_read - sizeof(META_OUT)));
                           if(cur_bytes == 0)
                           {
                                DEBUG_PRINT_ERROR("Append: Reject meta data,"
                                      " no more space \n");
                                break;
                           }
                           tot_bytes += cur_bytes;
                        }
                }
                else
                {
                       DEBUG_PRINT_ERROR("Append: Reject meta data,"
                                     " no more space \n");
                       break;
                 }
            }
        }
        else /*if(bytes_read ==-1)*/
        {
            DEBUG_PRINT("**********READ=-1**********************");
            DEBUG_PRINT("Append:Read=-1 Residual pcm[%d] [%lu]",tot_bytes,
                                      m_bufMgr->getBufFilledSpace());
            DEBUG_PRINT("**********READ=-1**********************");
            break;

        }
    }
    // EOS recieved.
    pthread_mutex_lock(&m_suspendresume_lock);
    setResumeFlg();
    pthread_mutex_unlock(&m_suspendresume_lock);
    fake_eos_recieved = true;
     if(m_eos_bm & IP_PORT_BITMASK)
     {
         m_eos_bm |= OP_PORT_BITMASK;
     }
     /*m_cb.EventHandler(&m_cmp, m_app_data,
               OMX_EventError,OMX_ErrorComponentSuspended,
                                   0, NULL );*/


     DEBUG_PRINT_ERROR("Rel DSP res, eos_bm[%d]sus[%d]\n",m_eos_bm,
                                     getWaitForSuspendCmplFlg());

    ioctl(m_drv_fd, AUDIO_STOP, 0);

    if (getWaitForSuspendCmplFlg())
    {
        DEBUG_PRINT_ERROR("Release P-->Executing context to IL client.\n");
        release_wait_for_suspend();
    }

    // now inform the client that the comp is SUSPENDED
    //m_cb.EventHandler(&m_cmp, m_app_data,
    //                   OMX_EventError,OMX_ErrorComponentSuspended,
    //                   0, NULL );
    DEBUG_PRINT("**********OMX_IndexParamComponentSUSPENDED****");
    if(m_timer->getTimerExpiry())
    {
        post_command(0,0,OMX_COMPONENT_RESUME);
    }
DEBUG_PRINT_ERROR("Enter into TCXO shutdown mode, residualdata[%lu]\n",m_bufMgr->getBufFilledSpace());
    return ;
}

timer::timer(omx_wma_adec* base):
    m_timerExpiryFlg(false),
    m_timeout(30),
    m_deleteTimer(OMX_FALSE),
    m_timer_cnt(0),
    m_base(base),
    m_timerinfo(NULL)
{
    sem_init(&m_sem_state,0, 0);

    pthread_cond_init (&m_timer_cond, NULL);
    pthread_mutexattr_init(&m_timer_mutex_attr);
    pthread_mutex_init(&m_timer_mutex, &m_timer_mutex_attr);

    pthread_cond_init (&m_tcond, NULL);
    pthread_mutexattr_init(&m_tmutex_attr);
    pthread_mutex_init(&m_tmutex, &m_tmutex_attr);

    m_timerinfo = (TIMERINFO*)malloc(sizeof(TIMERINFO));
    m_timerinfo->pTimer = this;
    m_timerinfo->base  = m_base;
    int rc = pthread_create(&m_timerinfo->thr ,0, omx_wma_comp_timer_handler,
                             (void*)m_timerinfo );
    if(rc < 0)
{
        DEBUG_PRINT_ERROR("Fail to create timer thread rc=%d errno=%d\n",rc,
                                                                     errno);
        free(m_timerinfo);
        m_timerinfo = NULL;
        }
        else
        DEBUG_PRINT("Created thread for timer object...\n");
}

timer::~timer()
{

    releaseTimer();
    stopTimer();
    if(m_timerinfo)
    {
        int rc = pthread_join(m_timerinfo->thr,NULL);
        DEBUG_PRINT("******************************\n");
        DEBUG_PRINT("CLOSING TIMER THREAD...%d\n",rc);
        DEBUG_PRINT("******************************\n");
        m_timerinfo->pTimer = NULL;
        m_timerinfo->base = NULL;
        free(m_timerinfo);
        m_timerinfo = NULL;
        }

    sem_destroy(&m_sem_state);
    pthread_mutexattr_destroy(&m_timer_mutex_attr);
    pthread_mutex_destroy(&m_timer_mutex);
    pthread_cond_destroy(&m_timer_cond);

    pthread_mutexattr_destroy(&m_tmutex_attr);
    pthread_mutex_destroy(&m_tmutex);
    pthread_cond_destroy(&m_tcond);

    m_timerExpiryFlg=false;
    m_deleteTimer = OMX_FALSE;
    m_timer_cnt = 1;
    m_base = NULL;
    }

void timer::wait_for_timer_event()
{
    sem_wait(&m_sem_state);
}

void timer::startTimer()
{
    sem_post(&m_sem_state);
}

int timer::timer_run()
{
    int rc =0;

    struct timespec   ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    DEBUG_PRINT("%s: Starting timer at %ld %ld %d\n",
                            __FUNCTION__,
                            ts.tv_sec,ts.tv_nsec,m_timer_cnt);
    clock_gettime(CLOCK_REALTIME, &ts);
            /* Convert from timeval to timespec */
    ts.tv_sec += m_timeout;
    pthread_mutex_lock(&m_timer_mutex);
    while (m_timer_cnt == 0)
    {
        if(getReleaseTimerStat()== OMX_TRUE)
        {
            DEBUG_PRINT_ERROR("Killing timer thread...\n");
            pthread_mutex_unlock(&m_timer_mutex);
            return 0;
        }
        rc = pthread_cond_timedwait(&m_timer_cond,
                                    &m_timer_mutex,
                                    &ts);
        DEBUG_PRINT("Timed wait rc=%d\n",rc);
        break;
    }
    m_timer_cnt = 0;
    pthread_mutex_unlock(&m_timer_mutex);
    clock_gettime(CLOCK_REALTIME, &ts);
    DEBUG_PRINT("%s: Elapsed Timer: %ld %ld\n",
                            __FUNCTION__,
                            ts.tv_sec,ts.tv_nsec);
    return rc;
    }

void timer::stopTimer()
{
    pthread_mutex_lock(&m_timer_mutex);
    if(m_timer_cnt == 0)
    {
        m_timer_cnt = 1;
        pthread_cond_signal(&m_timer_cond);
    }
    m_timer_cnt=0;
    pthread_mutex_unlock(&m_timer_mutex);
    DEBUG_PRINT("STOP TIMER...\n");
    return;
}

void* omx_wma_comp_timer_handler(void *pT)
{
    TIMERINFO *pTime = (TIMERINFO*)pT;
    timer *pt = pTime->pTimer;
    omx_wma_adec *pb = pTime->base;
    int               rc = 0;

    while(1)
    {
        pt->wait_for_timer_event();
        if(pt->getReleaseTimerStat()== OMX_TRUE)
        {
            DEBUG_PRINT_ERROR("Killing timer thread...\n");
            goto exit_th;
        }
        rc = pt->timer_run();
        if(rc != ETIMEDOUT)
        {
            if(pt->getReleaseTimerStat()== OMX_TRUE)
            {
                DEBUG_PRINT_ERROR("Now, Kill timer thread...\n");
                goto exit_th;
            }
            else
            {
                DEBUG_PRINT("Timer, go and wait again...\n");
                continue;
            }
}
        // now post event to command thread;
        OMX_STATETYPE state;
        DEBUG_DETAIL("SH:state=%d suspendstat=%d\n",pb->get_state((OMX_HANDLETYPE)pb,&state),\
                                                    pb->getSuspendFlg());
        pb->get_state((OMX_HANDLETYPE)pb,&state);
        if( ((state== OMX_StatePause)) && !(pb->getSuspendFlg()))
        {
            // post suspend message to command thread;
            pb->post_command(0,0,omx_wma_adec::OMX_COMPONENT_SUSPEND);
            pt->setTimerExpiry();
        }
        else
        {
            DEBUG_PRINT("SH: Ignore Timer expiry state=%d",state);
        }
    }

exit_th:
    DEBUG_PRINT_ERROR("Timer thread exited\n");
    return NULL;
}

OMX_U32 omxBufMgr::appendToBuf(OMX_U8 *srcbuf, OMX_U32 len)
{
    OMX_U32 tempLen=len;
    DEBUG_DETAIL("Remaining space in buffer=%lu\n",m_remaining_space);
    DEBUG_DETAIL("Append: write=%p read=%p\n",m_write,m_read);

    if(!m_remaining_space || (m_remaining_space < len))
    {
        m_rejected_bytes += len;
        m_tot_rejected_bytes += len;
        DEBUG_DETAIL("Reject pcm data %lu\n",m_rejected_bytes);
        return NULL;
    }

    if(m_write > m_read)
    {
        if((m_write + len) >= m_end )
        {
           OMX_U32 temp=0;
           memcpy(m_write,srcbuf,(m_end - m_write));
           temp = (m_end - m_write);
           m_write=m_start;
           memcpy(m_write,&srcbuf[temp],(len - temp));
           m_write += (len - temp);
        }
        else
        {
            memcpy(m_write,srcbuf,len);
            m_write += len;
        }
    }
    else if(m_write < m_read)
    {
        if((m_write + len) >= m_read )
        {
           OMX_U32 temp=0;
           memcpy(m_write,srcbuf,(m_read - m_write));
           temp = (m_read - m_write);
           m_write += temp;
           tempLen = temp;
        }
        else
        {
            memcpy(m_write,srcbuf,len);
            m_write += len;
        }
    }
    else
    {
        if((m_write + len) >= m_end )
        {
           OMX_U32 temp=0;
           memcpy(m_write,srcbuf,(m_end - m_write));
           temp = (m_end - m_write);
           m_write=m_start;
           memcpy(m_write,&srcbuf[temp],(len - temp));
           m_write += (len - temp);

        }
        else
        {
            memcpy(m_write,srcbuf,len);
            m_write += len;
        }

    }
    m_remaining_space -= tempLen;

    if(m_write >= m_end)
        m_write = m_start;

    DEBUG_DETAIL("==>Remaining space in buffer=%lu\n",m_remaining_space);
    return tempLen;
}

OMX_U32 omxBufMgr::emptyToBuf(OMX_U8 *destBuf, OMX_U32 destLen)
{
    if(destBuf == NULL)
    {
        DEBUG_DETAIL("emptyToBuf: NULL destBuf...\n");
        return 0;
    }
    OMX_U32 filledSpace = (OMX_TCXO_BUFFER -1 - m_remaining_space);
    OMX_U32 len=0;

    DEBUG_DETAIL("***********************\n");
    DEBUG_DETAIL("FilledSpace=%lu destLen=%lu\n",filledSpace,destLen);
    DEBUG_DETAIL("***********************\n");
    if(!filledSpace)
    {
        DEBUG_DETAIL("Buf empty ...\n");
        return 0;
    }
    if(filledSpace > destLen)
    {
        len = destLen;
    }
    else
    {
        len = filledSpace;
    }
    if(m_read > m_write)
    {
        if((m_read + len) >= m_end)
        {
            memcpy(destBuf,m_read,(m_end - m_read));
            memset(m_read,0,(m_end - m_read));

            OMX_U32 temp = (m_end - m_read);
            m_read=m_start;
            memcpy(&destBuf[temp],m_read,(len- temp));
            memset(m_read,0,(len - temp));
            m_read += (len - temp);
        }
        else
        {
            memcpy(destBuf,m_read,len);
            memset(m_read,0,len);
            m_read += len;
        }
    }
    else
    {
        memcpy(destBuf,m_read,len);
        memset(m_read,0,len);
        m_read += len;
    }
    m_remaining_space += len;

    if(m_read >= m_end)
        m_read = m_start;
DEBUG_PRINT("--> Copied %lu bytes\n",len);
    return len;
}
