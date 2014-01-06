/*
*         OpenPBS (Portable Batch System) v2.3 Software License
*
* Copyright (c) 1999-2000 Veridian Information Solutions, Inc.
* All rights reserved.
*
* ---------------------------------------------------------------------------
* For a license to use or redistribute the OpenPBS software under conditions
* other than those described below, or to purchase support for this software,
* please contact Veridian Systems, PBS Products Department ("Licensor") at:
*
*    www.OpenPBS.org  +1 650 967-4675                  sales@OpenPBS.org
*                        877 902-4PBS (US toll-free)
* ---------------------------------------------------------------------------
*
* This license covers use of the OpenPBS v2.3 software (the "Software") at
* your site or location, and, for certain users, redistribution of the
* Software to other sites and locations.  Use and redistribution of
* OpenPBS v2.3 in source and binary forms, with or without modification,
* are permitted provided that all of the following conditions are met.
* After December 31, 2001, only conditions 3-6 must be met:
*
* 1. Commercial and/or non-commercial use of the Software is permitted
*    provided a current software registration is on file at www.OpenPBS.org.
*    If use of this software contributes to a publication, product, or
*    service, proper attribution must be given; see www.OpenPBS.org/credit.html
*
* 2. Redistribution in any form is only permitted for non-commercial,
*    non-profit purposes.  There can be no charge for the Software or any
*    software incorporating the Software.  Further, there can be no
*    expectation of revenue generated as a consequence of redistributing
*    the Software.
*
* 3. Any Redistribution of source code must retain the above copyright notice
*    and the acknowledgment contained in paragraph 6, this list of conditions
*    and the disclaimer contained in paragraph 7.
*
* 4. Any Redistribution in binary form must reproduce the above copyright
*    notice and the acknowledgment contained in paragraph 6, this list of
*    conditions and the disclaimer contained in paragraph 7 in the
*    documentation and/or other materials provided with the distribution.
*
* 5. Redistributions in any form must be accompanied by information on how to
*    obtain complete source code for the OpenPBS software and any
*    modifications and/or additions to the OpenPBS software.  The source code
*    must either be included in the distribution or be available for no more
*    than the cost of distribution plus a nominal fee, and all modifications
*    and additions to the Software must be freely redistributable by any party
*    (including Licensor) without restriction.
*
* 6. All advertising materials mentioning features or use of the Software must
*    display the following acknowledgment:
*
*     "This product includes software developed by NASA Ames Research Center,
*     Lawrence Livermore National Laboratory, and Veridian Information
*     Solutions, Inc.
*     Visit www.OpenPBS.org for OpenPBS software support,
*     products, and information."
*
* 7. DISCLAIMER OF WARRANTY
*
* THIS SOFTWARE IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND. ANY EXPRESS
* OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT
* ARE EXPRESSLY DISCLAIMED.
*
* IN NO EVENT SHALL VERIDIAN CORPORATION, ITS AFFILIATED COMPANIES, OR THE
* U.S. GOVERNMENT OR ANY OF ITS AGENCIES BE LIABLE FOR ANY DIRECT OR INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
* This license will be governed by the laws of the Commonwealth of Virginia,
* without reference to its choice of law rules.
*/
/*
 * svr_modify.c
 *
 * Functions relating to the Modify Job Batch Requests.
 *
 * Included funtions are:
 *
 *
 */
#include <pbs_config.h>   /* the master config generated by configure */

#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>
#include "libpbs.h"
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "resource.h"
#include "server.h"
#include "queue.h"
#include "credential.h"
#include "batch_request.h"
#include "pbs_job.h"
#include "work_task.h"
#include "pbs_error.h"
#include "log.h"
#include "../lib/Liblog/pbs_log.h"
#include "../lib/Liblog/log_event.h"
#include "svrfunc.h"
#include "array.h"
#include "svr_func.h" /* get_svr_attr_* */
#include "ji_mutex.h"
#include "mutex_mgr.hpp"
#include "threadpool.h"
#include "mutex_mgr.hpp"
#include <string>

#define CHK_HOLD 1
#define CHK_CONT 2


/* Global Data Items: */

extern attribute_def     job_attr_def[];
extern char *msg_jobmod;
extern char *msg_manager;
extern char *msg_mombadmodify;
extern int   comp_resc_lt;
extern int   LOGLEVEL;
extern char *path_checkpoint;
extern char server_name[];

extern const char *PJobSubState[];
extern char *PJobState[];

/* External Functions called */

extern void cleanup_restart_file(job *);
extern void rel_resc(job *);

extern job  *chk_job_request(char *, struct batch_request *);
extern struct batch_request *cpy_checkpoint(struct batch_request *, job *, enum job_atr, int);

/* prototypes */
void post_modify_arrayreq(batch_request *preq);
void post_modify_req(batch_request *preq);


/*
 * post_modify_req - clean up after sending modify request to MOM
 */

void post_modify_req(

  batch_request *preq)

  {
  job  *pjob;
  char  log_buf[LOCAL_LOG_BUF_SIZE];

  if (preq == NULL)
    return;

  preq->rq_conn = preq->rq_orgconn;  /* restore socket to client */

  if ((preq->rq_reply.brp_code) && (preq->rq_reply.brp_code != PBSE_UNKJOBID))
    {
    sprintf(log_buf, msg_mombadmodify, preq->rq_reply.brp_code);

    log_event(
      PBSEVENT_JOB,
      PBS_EVENTCLASS_JOB,
      preq->rq_ind.rq_modify.rq_objname,
      log_buf);

    req_reject(preq->rq_reply.brp_code, 0, preq, NULL, NULL);
    }
  else
    {
    if (preq->rq_reply.brp_code == PBSE_UNKJOBID)
      {
      if ((pjob = svr_find_job(preq->rq_ind.rq_modify.rq_objname, FALSE)) == NULL)
        {
        req_reject(preq->rq_reply.brp_code, 0, preq, NULL, NULL);
        return;
        }
      else
        {
        mutex_mgr job_mutex(pjob->ji_mutex, true);

        if (LOGLEVEL >= 0)
          {
          sprintf(log_buf, "post_modify_req: PBSE_UNKJOBID for job %s in state %s-%s, dest = %s",
            pjob->ji_qs.ji_jobid,
            PJobState[pjob->ji_qs.ji_state],
            PJobSubState[pjob->ji_qs.ji_substate],
            pjob->ji_qs.ji_destin);

          log_event(PBSEVENT_JOB,PBS_EVENTCLASS_JOB,pjob->ji_qs.ji_jobid,log_buf);
          }
        }
      }

    reply_ack(preq);
    }

  return;
  }  /* END post_modify_req() */



/*
 * mom_cleanup_checkpoint_hold - Handle the clean up of mom after checkpoint and
 * hold.  This gets messy because there is a race condition between getting the
 * job obit and having the copy checkpoint complete.  After both have occured
 * we can request the mom to cleanup the job
 */

void mom_cleanup_checkpoint_hold(

  struct work_task *ptask)

  {
  int            rc = 0;
  job           *pjob;
  char          *jobid;

  batch_request *preq;
  char           log_buf[LOCAL_LOG_BUF_SIZE];
  time_t         time_now = time(NULL);

  jobid = (char *)ptask->wt_parm1;
  free(ptask->wt_mutex);
  free(ptask);

  if (jobid == NULL)
    {
    log_err(ENOMEM, __func__, "Cannot allocate memory");
    return;
    }

  pjob = svr_find_job(jobid, FALSE);
  if (pjob == NULL)
    {
    if (LOGLEVEL >= 3)
      {
      sprintf(log_buf,
        "%s:failed to find job\n",
        __func__);

      log_event(PBSEVENT_JOB,PBS_EVENTCLASS_JOB,jobid,log_buf);
      }
    free(jobid);
    return;
    }
  free(jobid);

  mutex_mgr job_mutex(pjob->ji_mutex, true);

  if (LOGLEVEL >= 7)
    {
    sprintf(log_buf,
      "checking mom cleanup job state is %s-%s\n",
      PJobState[pjob->ji_qs.ji_state],
      PJobSubState[pjob->ji_qs.ji_substate]);

    log_event(PBSEVENT_JOB,PBS_EVENTCLASS_JOB,pjob->ji_qs.ji_jobid,log_buf);
    }

  /* 
   * if the job is no longer running then we have recieved the job obit
   * and need to request the mom to clean up after the job
   */

  if (pjob->ji_qs.ji_state != JOB_STATE_RUNNING)
    {
    if ((preq = alloc_br(PBS_BATCH_DeleteJob)) == NULL)
      {
      log_err(-1, __func__, "unable to allocate DeleteJob request - big trouble!");
      }
    else
      {
      strcpy(preq->rq_ind.rq_delete.rq_objname, pjob->ji_qs.ji_jobid);
      /* The preq is freed in relay_to_mom (failure)
       * or in issue_Drequest (success) */
      if ((rc = relay_to_mom(&pjob, preq, NULL)) != PBSE_NONE)
        {
        if (pjob != NULL)
          {
          snprintf(log_buf,sizeof(log_buf),
            "Unable to relay information to mom for job '%s'\n",
            pjob->ji_qs.ji_jobid);
          
          log_err(rc, __func__, log_buf);
          }
        else
          job_mutex.set_lock_on_exit(false);

        free_br(preq);

        return;
        }
      else
        free_br(preq);

      if ((LOGLEVEL >= 7) &&
          (pjob != NULL))
        {
        log_event(
          PBSEVENT_JOB,
          PBS_EVENTCLASS_JOB,
          pjob->ji_qs.ji_jobid,
          "requested mom cleanup");
        }
      }
    }
  else
    {
    set_task(WORK_Timed, time_now + 1, mom_cleanup_checkpoint_hold, strdup(pjob->ji_qs.ji_jobid), FALSE);
    }

  if (pjob == NULL)
    job_mutex.set_lock_on_exit(false);
  } /* END mom_cleanup_checkpoint_hold() */




/*
 * chkpt_xfr_hold - Handle the clean up of the transfer of the checkpoint files.
 */

void chkpt_xfr_hold(

  batch_request *preq,
  job           *pjob)

  {
  char   log_buf[LOCAL_LOG_BUF_SIZE];

  if ((preq == NULL) ||
      (preq->rq_extra == NULL) ||
      (pjob == NULL))
    return;

  if (LOGLEVEL >= 7)
    {
    sprintf(log_buf,
      "BLCR copy completed (state is %s-%s)",
      PJobState[pjob->ji_qs.ji_state],
      PJobSubState[pjob->ji_qs.ji_substate]);

    log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, pjob->ji_qs.ji_jobid, log_buf);
    }
  
  free_br(preq);

  set_task(WORK_Immed, 0, mom_cleanup_checkpoint_hold, strdup(pjob->ji_qs.ji_jobid), FALSE);

  return;
  }  /* END chkpt_xfr_hold() */





/*
 * chkpt_xfr_done - Handle the clean up of the transfer of the checkpoint files.
 */

void chkpt_xfr_done(

  batch_request *preq)

  {
  free_br(preq);
  }  /* END chkpt_xfr_done() */





/*
 * modify_job()
 * modifies a job according to the newattr
 *
 * @param preq - the copy of preq for this array subjob. Must be freed here 
 * @param j - the job being altered
 * @param newattr - the new attributes
 * @return SUCCESS if set, FAILURE if problems
 */

int modify_job(

  void          **j,               /* O */
  svrattrl       *plist,           /* I */
  batch_request  *preq,            /* I */
  int             checkpoint_req,  /* I */
  int             flag)            /* I */

  {
  int                    bad = 0;
  int                    i;
  int                    newstate;
  int                    newsubstate;
  resource_def          *prsd;
  int                    rc;
  int                    sendmom = 0;
  int                    copy_checkpoint_files = FALSE;
  char                   jobid[PBS_MAXSVRJOBID + 1];
  char                   log_buf[LOCAL_LOG_BUF_SIZE];
  svrattrl              *plist_hold;

  job                  **pjob_ptr = (job **)j;
  job                   *pjob = *pjob_ptr;
  
  if (pjob == NULL)
    {
    sprintf(log_buf, "job structure is NULL");
    log_err(PBSE_IVALREQ, __func__, log_buf);

    req_reject(PBSE_IVALREQ, 0, preq, NULL, NULL);

    return(PBSE_IVALREQ);
    }
  
  plist_hold = plist;

  /* cannot be in exiting or transit, exiting has already been checked */
  if (pjob->ji_qs.ji_state == JOB_STATE_TRANSIT)
    {
    /* FAILURE */
    snprintf(log_buf,sizeof(log_buf),
      "Cannot modify job '%s' in transit\n",
      pjob->ji_qs.ji_jobid);

    log_err(PBSE_BADSTATE, __func__, log_buf);
    
    req_reject(PBSE_BADSTATE, 0, preq, NULL, NULL);

    return(PBSE_BADSTATE);
    }

  if (((checkpoint_req == CHK_HOLD) || (checkpoint_req == CHK_CONT)) &&
      (pjob->ji_qs.ji_substate == JOB_SUBSTATE_RUNNING))
    {
    /* May need to request copy of the checkpoint file from mom */

    copy_checkpoint_files = TRUE;

    if (checkpoint_req == CHK_HOLD)
      {
      sprintf(log_buf,"setting jobsubstate for %s to RERUN\n", pjob->ji_qs.ji_jobid);

      pjob->ji_qs.ji_substate = JOB_SUBSTATE_RERUN;

      job_save(pjob, SAVEJOB_QUICK, 0);

      log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, pjob->ji_qs.ji_jobid, log_buf);

      /* remove checkpoint restart file if there is one */
      
      if (pjob->ji_wattr[JOB_ATR_restart_name].at_flags & ATR_VFLAG_SET)
        {
        cleanup_restart_file(pjob);
        }

      }
    }

  /* if job is running, special checks must be made */

  /* NOTE:  must determine if job exists down at MOM - this will occur if
            job is running, job is held, or job was held and just barely
            released (ie qhold/qrls) */
  if (pjob->ji_qs.ji_state == JOB_STATE_RUNNING)
    {
    while (plist != NULL)
      {
      /* is the pbs_attribute modifiable in RUN state ? */

      i = find_attr(job_attr_def, plist->al_name, JOB_ATR_LAST);

      if ((i < 0) ||
          ((job_attr_def[i].at_flags & ATR_DFLAG_ALTRUN) == 0))
        {
        /* FAILURE */
        snprintf(log_buf,sizeof(log_buf),
          "Cannot modify attribute '%s' while running\n",
          plist->al_name);
        log_err(PBSE_MODATRRUN, __func__, log_buf);
    
        reply_badattr(PBSE_MODATRRUN, 1, plist, preq);

        return(PBSE_MODATRRUN);
        }

      /* NOTE:  only explicitly specified job attributes are routed down to MOM */

      if (i == JOB_ATR_resource)
        {
        /* is the specified resource modifiable while */
        /* the job is running                         */

        prsd = find_resc_def(svr_resc_def, plist->al_resc, svr_resc_size);

        if (prsd == NULL)
          {
          /* FAILURE */
          snprintf(log_buf,sizeof(log_buf),
            "Unknown attribute '%s'\n",
            plist->al_name);

          log_err(PBSE_UNKRESC, __func__, log_buf);
    
          reply_badattr(PBSE_UNKRESC, 1, plist, preq);

          return(PBSE_UNKRESC);
          }

        if ((prsd->rs_flags & ATR_DFLAG_ALTRUN) == 0)
          {
          /* FAILURE */
          snprintf(log_buf,sizeof(log_buf),
            "Cannot modify attribute '%s' while running\n",
            plist->al_name);
          log_err(PBSE_MODATRRUN, __func__, log_buf);
    
          reply_badattr(PBSE_MODATRRUN, 1, plist, preq);

          return(PBSE_MODATRRUN);
          }

        sendmom = 1;
        }

      plist = (svrattrl *)GET_NEXT(plist->al_link);
      }
    } /* END if (pjob->ji_qs.ji_state == JOB_STATE_RUNNING) */

  /* modify the job's attributes */
  bad = 0;

  /* if the job was running we need to reset plist */
  plist = plist_hold;
  while (plist != NULL)
    {
    rc = modify_job_attr(pjob, plist, preq->rq_perm, &bad);

    if (rc)
      {
      /* FAILURE */
      snprintf(log_buf,sizeof(log_buf),
        "Cannot set attributes for job '%s'\n",
        pjob->ji_qs.ji_jobid);
      log_err(rc, __func__, log_buf);

      if (rc == PBSE_JOBNOTFOUND)
        *j = NULL;
    
      req_reject(rc, 0, preq, NULL, NULL);

      return(rc);
      }

    plist = (svrattrl *)GET_NEXT(plist->al_link);
    }
  /* Reset any defaults resource limit which might have been unset */
  set_resc_deflt(pjob, NULL, FALSE);

  /* if job is not running, may need to change its state */
  if (pjob->ji_qs.ji_state != JOB_STATE_RUNNING)
    {
    svr_evaljobstate(pjob, &newstate, &newsubstate, 0);
    svr_setjobstate(pjob, newstate, newsubstate, FALSE);
    }
  else
    {
    job_save(pjob, SAVEJOB_FULL, 0);
    }

  sprintf(log_buf, msg_manager, msg_jobmod, preq->rq_user, preq->rq_host);
  log_event(PBSEVENT_JOB,PBS_EVENTCLASS_JOB,pjob->ji_qs.ji_jobid,log_buf);

  /* if a resource limit changed for a running job, send to MOM */
  if (sendmom)
    {
    /* if the NO_MOM_RELAY flag is set the calling function will call
       relay_to_mom so we do not need to do it here */
    if (flag != NO_MOM_RELAY)
      {
      /* The last number is unused unless this is an array */
      if ((rc = relay_to_mom(&pjob, preq, NULL)))
        {
        req_reject(rc, 0, preq, NULL, NULL);

        if (pjob != NULL)
          {
          snprintf(log_buf,sizeof(log_buf),
            "Unable to relay information to mom for job '%s'\n",
            pjob->ji_qs.ji_jobid);
          
          log_err(rc, __func__, log_buf);
          }

        return(rc); /* unable to get to MOM */
        }
      else
        {
        jobid[0] = '\0';

        if (pjob != NULL)
          {
          strcpy(jobid, pjob->ji_qs.ji_jobid);
          unlock_ji_mutex(pjob, __func__, "2", LOGLEVEL);
          }

        post_modify_req(preq);

        if (jobid[0] != '\0')
          pjob = svr_find_job(jobid, TRUE);

        if (pjob == NULL)
          *pjob_ptr = NULL;
        }
      }
    else
      reply_ack(preq);

    return(PBSE_RELAYED_TO_MOM);
    }
  else
    {
    reply_ack(preq);
    preq = NULL;
    }

  if (copy_checkpoint_files)
    {
    struct batch_request *momreq = 0;
    momreq = cpy_checkpoint(momreq, pjob, JOB_ATR_checkpoint_name, CKPT_DIR_OUT);

    if (momreq != NULL)
      {
      /* have files to copy */
      momreq->rq_extra = strdup(pjob->ji_qs.ji_jobid);

      /* The momreq is freed in relay_to_mom (failure)
       * or in issue_Drequest (success) */
      rc = relay_to_mom(&pjob, momreq, NULL);

      if (rc != PBSE_NONE)
        {
        free_br(momreq);
   
        if (preq != NULL)
          req_reject(rc, 0, preq, NULL, NULL);

        if (pjob != NULL)
          {
          snprintf(log_buf,sizeof(log_buf),
            "Unable to relay information to mom for job '%s'\n",
            pjob->ji_qs.ji_jobid);
          
          log_err(rc, __func__, log_buf);
          }

        return(PBSE_NONE);  /* come back when mom replies */
        }
      else if (checkpoint_req == CHK_HOLD)
        chkpt_xfr_hold(momreq, pjob);
      else
        chkpt_xfr_done(momreq);
      }
    else
      {
      log_err(-1, __func__, "Failed to get batch request");
      }
    }

  return(PBSE_NONE);
  } /* END modify_job() */




/*
 * modify_whole_array()
 * modifies the entire job array 
 * @SEE req_modify_array PARENT
 */

int modify_whole_array(

  job_array            *pa,             /* I/O */
  svrattrl             *plist,          /* I */
  struct batch_request *preq,           /* I */
  int                   checkpoint_req) /* I */

  {
  int   i;
  int   rc = PBSE_NONE;
  int   modify_job_rc = PBSE_NONE;
  job  *pjob;

  for (i = 0; i < pa->ai_qs.array_size; i++)
    {
    if (pa->job_ids[i] == NULL)
      continue;

    if ((pjob = svr_find_job(pa->job_ids[i], FALSE)) == NULL)
      {
      free(pa->job_ids[i]);
      pa->job_ids[i] = NULL;
      }
    else
      {
      /* NO_MOM_RELAY will prevent modify_job from calling relay_to_mom */
      batch_request *array_req = duplicate_request(preq, i);
      mutex_mgr job_mutex(pjob->ji_mutex, true);
      pthread_mutex_unlock(pa->ai_mutex);
      array_req->rq_noreply = TRUE;
      rc = modify_job((void **)&pjob, plist, array_req, checkpoint_req, NO_MOM_RELAY);
      if (rc != PBSE_NONE)
        {
        modify_job_rc = rc;
        }
      pa = get_jobs_array(&pjob);
      
      if (pa == NULL)
        {
        if (pjob == NULL)
          job_mutex.set_lock_on_exit(false);

        return(PBSE_JOB_RECYCLED);
        }

      if (pjob == NULL)
        {
        pa->job_ids[i] = NULL;
        job_mutex.set_lock_on_exit(false);
        continue;
        }
      }
    } /* END foreach job in array */

  return(modify_job_rc);
  } /* END modify_whole_array() */



void *modify_array_work(

  void *vp)

  {
  batch_request *preq = (batch_request *)vp;
  svrattrl      *plist;
  int            rc = 0;
  char          *pcnt = NULL;
  char          *array_spec = NULL;
  int            checkpoint_req = FALSE;
  job           *pjob = NULL;
  job_array     *pa;

  pa = get_array(preq->rq_ind.rq_modify.rq_objname);

  if (pa == NULL)
    {
    req_reject(PBSE_UNKARRAYID, 0, preq, NULL, "unable to find array");
    return(NULL);
    }

  mutex_mgr array_mutex(pa->ai_mutex, true);

  /* pbs_mom sets the extend string to trigger copying of checkpoint files */
  if (preq->rq_extend != NULL)
    {
    if (strcmp(preq->rq_extend,CHECKPOINTHOLD) == 0)
      {
      checkpoint_req = CHK_HOLD;
      }
    else if (strcmp(preq->rq_extend,CHECKPOINTCONT) == 0)
      {
      checkpoint_req = CHK_CONT;
      }
    }

  /* find if an array range was specified */
  if ((preq->rq_extend != NULL) && 
      ((array_spec = strstr(preq->rq_extend,ARRAY_RANGE)) != NULL))
    {
    /* move array spec past ARRAY_RANGE= */
    char *equals = strchr(array_spec,'=');
    if (equals != NULL)
      {
      array_spec = equals + 1;
      }

    if ((pcnt = strchr(array_spec,'%')) != NULL)
      {
      int slot_limit = atoi(pcnt+1);
      pa->ai_qs.slot_limit = slot_limit;
      }
    }
  
  plist = (svrattrl *)GET_NEXT(preq->rq_ind.rq_modify.rq_attr);

  if ((array_spec != NULL) &&
      (pcnt != array_spec))
    {
    if (pcnt != NULL)
      *pcnt = '\0';

    /* there is more than just a slot given, modify that range */
    rc = modify_array_range(pa,array_spec,plist,preq,checkpoint_req);

    if (pcnt != NULL)
      *pcnt = '%';

    if ((rc != 0) && 
        (rc != PBSE_RELAYED_TO_MOM))
      {
      req_reject(PBSE_IVALREQ,0,preq,NULL,"Error reading array range");
      return(NULL);
      }
    else
      reply_ack(preq);

    return(NULL);
    }
  else 
    {
    rc = modify_whole_array(pa,plist,preq,checkpoint_req);

    if ((rc != 0) && 
        (rc != PBSE_RELAYED_TO_MOM))
      {
      req_reject(PBSE_IVALREQ, 0, preq, NULL, "At least one array element did not modify successfully. Use qstat -f to verify changes");
      return(NULL);
      }

    /* we modified the job array. We now need to update the job */
    if ((pjob = chk_job_request(preq->rq_ind.rq_modify.rq_objname, preq)) == NULL)
      return(NULL);

    mutex_mgr job_mutex = mutex_mgr(pjob->ji_mutex, true);

    /* modify_job will reply to preq and free it */
    modify_job((void **)&pjob, plist, preq, checkpoint_req, NO_MOM_RELAY);
    }

  return(NULL);
  } /* END modify_array_work() */




/*
 * req_modifyarray()
 * modifies a job array
 * additionally, can change the slot limit of the array
 */

void *req_modifyarray(

  batch_request *vp) /* I */

  {
  job_array            *pa;
  struct batch_request *preq = (struct batch_request *)vp;

  pa = get_array(preq->rq_ind.rq_modify.rq_objname);

  if (pa == NULL)
    {
    req_reject(PBSE_UNKARRAYID, 0, preq, NULL, "unable to find array");
    return(NULL);
    }

  unlock_ai_mutex(pa, __func__, "4", LOGLEVEL);

  /* If async modify, reply now; otherwise reply is handled later */
  if (preq->rq_type == PBS_BATCH_AsyModifyJob)
    {
    preq->rq_noreply = TRUE; /* set for no more replies */
    reply_ack(preq);

    enqueue_threadpool_request(modify_array_work, preq);
    }
  else
    modify_array_work(preq);

  return(NULL);
  } /* END req_modifyarray() */



void *modify_job_work(

  batch_request *vp) /* I */

  {
  job           *pjob;
  svrattrl      *plist;
  int            checkpoint_req = FALSE;
  batch_request *preq = (struct batch_request *)vp;
  
  pjob = svr_find_job(preq->rq_ind.rq_modify.rq_objname, FALSE);

  if (pjob == NULL)
    {
    req_reject(PBSE_JOBNOTFOUND, 0, preq, NULL, "Job unexpectedly deleted");
    return(NULL);
    }

  mutex_mgr job_mutex(pjob->ji_mutex, true);
  
  /* pbs_mom sets the extend string to trigger copying of checkpoint files */
  if (preq->rq_extend != NULL)
    {
    if (strcmp(preq->rq_extend,CHECKPOINTHOLD) == 0)
      {
      checkpoint_req = CHK_HOLD;
      }
    else if (strcmp(preq->rq_extend,CHECKPOINTCONT) == 0)
      {
      checkpoint_req = CHK_CONT;
      }
    }

  plist = (svrattrl *)GET_NEXT(preq->rq_ind.rq_modify.rq_attr);

  /* modify_job will free preq and respond to it */
  modify_job((void **)&pjob, plist, preq, checkpoint_req, 0);

  return(NULL);
  } /* END modify_job_work() */




/*
 * req_modifyjob - service the Modify Job Request
 *
 * This request modifes a job's attributes.
 *
 * @see relay_to_mom() - child - routes change down to pbs_mom
 */

void *req_modifyjob(

  batch_request *preq) /* I */

  {
  job       *pjob;
  svrattrl  *plist;
  char       log_buf[LOCAL_LOG_BUF_SIZE];

  pjob = chk_job_request(preq->rq_ind.rq_modify.rq_objname, preq);

  if (pjob == NULL)
    {
    return(NULL);
    }

  mutex_mgr job_mutex(pjob->ji_mutex, true);

  plist = (svrattrl *)GET_NEXT(preq->rq_ind.rq_modify.rq_attr);

  if (plist == NULL)
    {
    /* nothing to do */
    reply_ack(preq);

    /* SUCCESS */
    return(NULL);
    }

  job_mutex.unlock();


  /* If async modify, reply now; otherwise reply is handled later */
  if (preq->rq_type == PBS_BATCH_AsyModifyJob)
    {
    /* reply_ack will free preq. We need to copy it before we call reply_ack */
    batch_request *new_preq;

    new_preq = duplicate_request(preq, -1);
    if (new_preq == NULL)
      {
      sprintf(log_buf, "failed to duplicate batch request");
      log_event(PBSEVENT_JOB, PBS_EVENTCLASS_JOB, __func__, log_buf);
      return(NULL);
      }

    get_batch_request_id(new_preq);
    reply_ack(preq);

    new_preq->rq_noreply = TRUE; /* set for no more replies */

    enqueue_threadpool_request((void *(*)(void *))modify_job_work, new_preq);
  }
  else
    modify_job_work(preq);
  
  return(NULL);
  }  /* END req_modifyjob() */





/*
 * modify_job_attr - modify the attributes of a job atomically
 * Used by req_modifyjob() to alter the job attributes and by
 * stat_update() [see req_stat.c] to update with latest from MOM
 */

int modify_job_attr(

  job      *pjob,  /* I (modified) */
  svrattrl *plist, /* I */
  int       perm,
  int      *bad)   /* O */

  {
  int            allow_unkn = -1;
  long           i;
  pbs_attribute  newattr[JOB_ATR_LAST];
  pbs_attribute *pattr;
  int            rc;
  char           log_buf[LOCAL_LOG_BUF_SIZE];
  pbs_queue     *pque;

  if ((pque = get_jobs_queue(&pjob)) != NULL)
    {
    mutex_mgr pque_mutex = mutex_mgr(pque->qu_mutex, true);
    if (pque->qu_qs.qu_type != QTYPE_Execution)
      allow_unkn = JOB_ATR_UNKN;

    }
  else if (pjob->ji_parent_job != NULL)
    {
    allow_unkn = JOB_ATR_UNKN;
    }
  else
    {
    log_err(PBSE_JOBNOTFOUND, __func__, "Job lost while acquiring queue 5");
    return(PBSE_JOBNOTFOUND);
    }

  pattr = pjob->ji_wattr;

  /* call attr_atomic_set to decode and set a copy of the attributes */

  rc = attr_atomic_set(
         plist,        /* I */
         pattr,        /* I */
         newattr,      /* O */
         job_attr_def, /* I */
         JOB_ATR_LAST,
         allow_unkn,   /* I */
         perm,         /* I */
         bad);         /* O */

  /* if resource limits are being changed ... */

  if ((rc == 0) &&
      (newattr[JOB_ATR_resource].at_flags & ATR_VFLAG_SET))
    {
    if ((perm & (ATR_DFLAG_MGWR | ATR_DFLAG_OPWR)) == 0)
      {
      /* If job is running, only manager/operator can raise limits */

      if (pjob->ji_qs.ji_state == JOB_STATE_RUNNING)
        {
        long lim = TRUE;
        int comp_resc_lt;
       
        get_svr_attr_l(SRV_ATR_QCQLimits, &lim);
        comp_resc_lt = comp_resc2(&pjob->ji_wattr[JOB_ATR_resource],
                                      &newattr[JOB_ATR_resource],
                                      lim,
                                      NULL,
                                      LESS);

        if (comp_resc_lt != 0)
          {
          rc = PBSE_PERM;
          }
        }

      /* Also check against queue and system limits */

      if (rc == 0)
        {
        if ((pque = get_jobs_queue(&pjob)) != NULL)
          {
          mutex_mgr pque_mutex = mutex_mgr(pque->qu_mutex, true);
          rc = chk_resc_limits( &newattr[JOB_ATR_resource], pque, NULL);
          }
        else if (pjob == NULL)
          {
          log_err(PBSE_JOBNOTFOUND, __func__, "Job lost while acquiring queue 6");
          return(PBSE_JOBNOTFOUND);
          }
        else
          rc = PBSE_QUENOTAVAILABLE;
        }
      }
    }    /* END if ((rc == 0) && ...) */

  /* special check on permissions for hold */

  if ((rc == 0) &&
      (newattr[JOB_ATR_hold].at_flags & ATR_VFLAG_MODIFY))
    {
    i = newattr[JOB_ATR_hold].at_val.at_long ^
        (pattr + JOB_ATR_hold)->at_val.at_long;

    rc = chk_hold_priv(i, perm);
    }

  if (rc == 0)
    {
    for (i = 0;i < JOB_ATR_LAST;i++)
      {
      if (newattr[i].at_flags & ATR_VFLAG_MODIFY)
        {
        if (job_attr_def[i].at_action)
          {
          rc = job_attr_def[i].at_action(
                 &newattr[i],
                 pjob,
                 ATR_ACTION_ALTER);

          if (rc)
            break;
          }
        }
      }    /* END for (i) */

    if ((rc == 0) &&
        ((newattr[JOB_ATR_userlst].at_flags & ATR_VFLAG_MODIFY) ||
         (newattr[JOB_ATR_grouplst].at_flags & ATR_VFLAG_MODIFY)))
      {
      /* need to reset execution uid and gid */

      rc = set_jobexid(pjob, newattr, NULL);
      }

    if ((rc == 0) &&
        (newattr[JOB_ATR_outpath].at_flags & ATR_VFLAG_MODIFY))
      {
      /* need to recheck if JOB_ATR_outpath is a special case of host only */

      if (newattr[JOB_ATR_outpath].at_val.at_str[strlen(newattr[JOB_ATR_outpath].at_val.at_str) - 1] == ':')
        {
        std::string ds = "";
        newattr[JOB_ATR_outpath].at_val.at_str = strdup(prefix_std_file(pjob, ds, (int)'o'));
        }
      /*
       * if the output path was specified and ends with a '/'
       * then append the standard file name
       */
      else if (newattr[JOB_ATR_outpath].at_val.at_str[strlen(newattr[JOB_ATR_outpath].at_val.at_str) - 1] == '/')
        {
        std::string ds = "";
        newattr[JOB_ATR_outpath].at_val.at_str[strlen(newattr[JOB_ATR_outpath].at_val.at_str) - 1] = '\0';
        
        replace_attr_string(&newattr[JOB_ATR_outpath],
          (strdup(add_std_filename(pjob, newattr[JOB_ATR_outpath].at_val.at_str, (int)'o', ds))));
        }
      }

    if ((rc == 0) &&
        (newattr[JOB_ATR_errpath].at_flags & ATR_VFLAG_MODIFY))
      {
      /* need to recheck if JOB_ATR_errpath is a special case of host only */

      if (newattr[JOB_ATR_errpath].at_val.at_str[strlen(newattr[JOB_ATR_errpath].at_val.at_str) - 1] == ':')
        {
        std::string ds = "";
        newattr[JOB_ATR_errpath].at_val.at_str = strdup(prefix_std_file(pjob, ds, (int)'e'));
        }
      /*
       * if the error path was specified and ends with a '/'
       * then append the standard file name
       */
      else if (newattr[JOB_ATR_errpath].at_val.at_str[strlen(newattr[JOB_ATR_errpath].at_val.at_str) - 1] == '/')
        {
        std::string ds = "";
        newattr[JOB_ATR_errpath].at_val.at_str[strlen(newattr[JOB_ATR_errpath].at_val.at_str) - 1] = '\0';
        
        replace_attr_string(&newattr[JOB_ATR_errpath],
          (strdup(add_std_filename(pjob, newattr[JOB_ATR_errpath].at_val.at_str,(int)'e', ds))));
        }
      }

    }  /* END if (rc == 0) */

  if (rc != 0)
    {
    for (i = 0;i < JOB_ATR_LAST;i++)
      job_attr_def[i].at_free(newattr + i);

    /* FAILURE */

    return(rc);
    }  /* END if (rc != 0) */

  /* OK, now copy the new values into the job attribute array */

  for (i = 0;i < JOB_ATR_LAST;i++)
    {
    if (newattr[i].at_flags & ATR_VFLAG_MODIFY)
      {
      if (LOGLEVEL >= 7)
        {
        sprintf(log_buf, "attr %s modified", job_attr_def[i].at_name);

        log_event(PBSEVENT_JOB,PBS_EVENTCLASS_JOB,pjob->ji_qs.ji_jobid,log_buf);
        }

      job_attr_def[i].at_free(pattr + i);

      if ((newattr[i].at_type == ATR_TYPE_LIST) ||
          (newattr[i].at_type == ATR_TYPE_RESC))
        {
        list_move(
          &newattr[i].at_val.at_list,
          &(pattr + i)->at_val.at_list);
        }
      else
        {
        *(pattr + i) = newattr[i];
        }

      (pattr + i)->at_flags = newattr[i].at_flags;
      }
    }    /* END for (i) */

  /* note, the newattr[] attributes are on the stack, they go away automatically */

  pjob->ji_modified = 1;

  return(PBSE_NONE);
  }  /* END modify_job_attr() */




/*
 * post_modify_arrayreq - clean up after sending modify request to MOM
 */

void post_modify_arrayreq(

  batch_request *preq)

  {
  job           *pjob;
  char           log_buf[LOCAL_LOG_BUF_SIZE];

  if (preq == NULL)
    return;

  preq->rq_conn = preq->rq_orgconn;  /* restore socket to client */

  if ((preq->rq_reply.brp_code) && (preq->rq_reply.brp_code != PBSE_UNKJOBID))
    {
    sprintf(log_buf, msg_mombadmodify, preq->rq_reply.brp_code);

    log_event(PBSEVENT_JOB,PBS_EVENTCLASS_JOB,preq->rq_ind.rq_modify.rq_objname,log_buf);

    free_br(preq);
    }
  else
    {
    if (preq->rq_reply.brp_code == PBSE_UNKJOBID)
      {
      if ((pjob = svr_find_job(preq->rq_ind.rq_modify.rq_objname, FALSE)) == NULL)
        {
        free_br(preq);
        return;
        }
      else
        {
        mutex_mgr job_mutex = mutex_mgr(pjob->ji_mutex, true);

        if (LOGLEVEL >= 0)
          {
          sprintf(log_buf, "post_modify_req: PBSE_UNKJOBID for job %s in state %s-%s, dest = %s",
            pjob->ji_qs.ji_jobid,
            PJobState[pjob->ji_qs.ji_state],
            PJobSubState[pjob->ji_qs.ji_substate],
            pjob->ji_qs.ji_destin);

          log_event(PBSEVENT_JOB,PBS_EVENTCLASS_JOB,pjob->ji_qs.ji_jobid,log_buf);
          }
        }
      }

    free_br(preq);
    }

  return;
  }  /* END post_modify_arrayreq() */

