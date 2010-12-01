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
 * svr_movejob.c - functions to move a job to another queue
 *
 * Included functions are:
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/wait.h>

#include <pbs_config.h>   /* the master config generated by configure */

#include "libpbs.h"
#include "pbs_error.h"
#include "list_link.h"
#include "attribute.h"
#include "server_limits.h"
#include "work_task.h"
#include "log.h"
#include "queue.h"
#include "pbs_job.h"
#include "pbs_nodes.h"
#include "credential.h"
#include "batch_request.h"
#include "net_connect.h"
#include "svrfunc.h"
#include "rpp.h"
#include "mcom.h"
#include "array.h"

#if __STDC__ != 1
#include <memory.h>
#endif


/* reduced retry from 3 to 2 (CRI - Mar 23, 2004) */

#define RETRY 2 /* number of times to retry network move */

/* External functions called */

extern void stat_mom_job(job *);
extern void remove_stagein(job *);
extern void remove_checkpoint(job *);
extern int  job_route(job *);

extern struct pbsnode *PGetNodeFromAddr(pbs_net_t);



/* Private Functions local to this file */

static int  local_move(job *, struct batch_request *);
static void net_move_die(int sig);
static void post_movejob(struct work_task *);
static void post_routejob(struct work_task *);
static int should_retry_route(int err);
static int move_job_file(int con, job *pjob, enum job_file which);

/* Global Data */

extern char *path_jobs;
extern char *path_spool;
extern attribute_def job_attr_def[];
extern int  queue_rank;
extern char  server_host[];
extern char  server_name[];
extern char *msg_badexit;
extern char *msg_routexceed;
extern char *msg_manager;
extern char *msg_movejob;
extern char     *msg_err_malloc;
extern int comp_resc_gt, comp_resc_eq, comp_resc_lt;
extern int pbs_errno;
extern char    *pbs_o_host;
extern pbs_net_t pbs_server_addr;
extern unsigned int pbs_server_port_dis;
extern time_t pbs_tcp_timeout;
extern int resc_access_perm;
extern int      LOGLEVEL;

int net_move(job *, struct batch_request *);

/*
 * svr_movejob
 *
 * Test if the destination is local or not and call a routine to
 * do the appropriate move.
 *
 * Returns:
 *  0 success
 *        -1 permenent failure or rejection,
 *  1 failed but try again
 *  2 deferred (ie move in progress), check later
 */

int svr_movejob(

  job                 *jobp,
  char                 *destination,
  struct batch_request *req)

  {
  pbs_net_t destaddr;
  int local;
  unsigned int port;
  char *toserver;

  if (strlen(destination) >= (size_t)PBS_MAXROUTEDEST)
    {
    sprintf(log_buffer, "name %s over maximum length of %d\n",
            destination,
            PBS_MAXROUTEDEST);

    log_err(-1, "svr_movejob", log_buffer);

    pbs_errno = PBSE_QUENBIG;

    return(-1);
    }

  strncpy(jobp->ji_qs.ji_destin, destination, PBS_MAXROUTEDEST);

  jobp->ji_qs.ji_un_type = JOB_UNION_TYPE_ROUTE;

  local = 1;

  if ((toserver = strchr(destination, '@')) != NULL)
    {
    /* check to see if the part after '@' is this server */

    destaddr = get_hostaddr(parse_servername(++toserver, &port));

    if (destaddr != pbs_server_addr)
      {
      local = 0;
      }
    }

  if (local != 0)
    {
    return(local_move(jobp, req));
    }

  return(net_move(jobp, req));
  }  /* svr_movejob() */





/*
 * local_move - internally move a job to another queue
 *
 * Check the destination to see if it can accept the job.
 *
 * Returns:
 *  0 success
 *        -1 permanent failure or rejection, see pbs_errno
 *  1 failed but try again
 */

static int local_move(

  job                  *jobp,
  struct batch_request *req)

  {
  char   *id = "local_move";
  pbs_queue *qp;
  char   *destination = jobp->ji_qs.ji_destin;
  int    mtype;

  /* search for destination queue */

  if ((qp = find_queuebyname(destination)) == NULL)
    {
    sprintf(log_buffer, "queue %s does not exist\n",
            destination);

    log_err(-1, id, log_buffer);

    pbs_errno = PBSE_UNKQUE;

    return(-1);
    }

  /*
   * if being moved at specific request of administrator, then
   * checks on queue availability, etc. are skipped;
   * otherwise all checks are enforced.
   */

  if (req == 0)
    {
    mtype = MOVE_TYPE_Route; /* route */
    }
  else if (req->rq_perm & (ATR_DFLAG_MGRD | ATR_DFLAG_MGWR))
    {
    mtype = MOVE_TYPE_MgrMv; /* privileged move */
    }
  else
    {
    mtype = MOVE_TYPE_Move; /* non-privileged move */
    }

  if ((pbs_errno = svr_chkque(
                     jobp,
                     qp,
                     get_variable(jobp, pbs_o_host), mtype, NULL)))
    {
    /* should this queue be retried? */

    return(should_retry_route(pbs_errno));
    }

  /* dequeue job from present queue, update destination and */
  /* queue_rank for new queue and enqueue into destination  */

  svr_dequejob(jobp);

  strcpy(jobp->ji_qs.ji_queue, destination);

  jobp->ji_wattr[JOB_ATR_qrank].at_val.at_long = ++queue_rank;

  pbs_errno = svr_enquejob(jobp);

  if (pbs_errno != 0)
    {
    return(-1); /* should never ever get here */
    }

  jobp->ji_lastdest = 0; /* reset in case of another route */

  job_save(jobp, SAVEJOB_FULL, 0);

  return(0);
  }  /* END local_move() */




/*
 * post_routejob - clean up action for child started in net_move/send_job
 *     to "route" a job to another server
 *
 * If route was successfull, delete job.
 *
 * If route didn't work, mark destination not to be tried again for this
 * job and call route again.
 *
 * Returns: none.
 */

static void post_routejob(

  struct work_task *pwt)

  {
  int  newstate;
  int  newsub;
  int  r;
  int  stat = pwt->wt_aux;
  char *id = "post_routejob";
  job *jobp = (job *)pwt->wt_parm1;

  if (WIFEXITED(stat))
    {
    r = WEXITSTATUS(stat);
    }
  else
    {
    r = 2;

    sprintf(log_buffer, msg_badexit,
            stat);

    strcat(log_buffer, id);

    log_event(
      PBSEVENT_SYSTEM,
      PBS_EVENTCLASS_JOB,
      jobp->ji_qs.ji_jobid,
      log_buffer);
    }

  switch (r)
    {
    case 0:  /* normal return, job was routed */

      if (jobp->ji_qs.ji_svrflags & JOB_SVFLG_StagedIn)
        remove_stagein(jobp);

      if (jobp->ji_qs.ji_svrflags & JOB_SVFLG_CHECKPOINT_COPIED)
        remove_checkpoint(jobp);

      job_purge(jobp); /* need to remove server job struct */

      return;

      /*NOTREACHED*/

      break;

    case 1:  /* permanent rejection (or signal) */

      if (jobp->ji_qs.ji_substate == JOB_SUBSTATE_ABORT)
        {
        /* job delete in progress, just set to queued status */

        svr_setjobstate(jobp, JOB_STATE_QUEUED, JOB_SUBSTATE_ABORT);

        return;
        }

      add_dest(jobp);  /* else mark destination as bad */

      /* fall through */

    default : /* try routing again */

      /* force re-eval of job state out of Transit */

      svr_evaljobstate(jobp, &newstate, &newsub, 1);
      svr_setjobstate(jobp, newstate, newsub);

      if ((r = job_route(jobp)) == PBSE_ROUTEREJ)
        job_abt(&jobp, pbse_to_txt(PBSE_ROUTEREJ));
      else if (r != 0)
        job_abt(&jobp, msg_routexceed);

      break;
    }  /* END switch (r) */

  return;
  }  /* END post_routejob() */





/*
 * post_movejob - clean up action for child started in net_move/send_job
 *     to "move" a job to another server
 *
 * If move was successfull, delete server's copy of thejob structure,
 * and reply to request.
 *
 * If route didn't work, reject the request.
 *
 * Returns: none.
 */

static void post_movejob(

  struct work_task *pwt)

  {
  char *id = "post_movejob";

  struct batch_request *req;
  int newstate;
  int newsub;
  int stat;
  int r;
  job *jobp;

  req  = (struct batch_request *)pwt->wt_parm2;

  stat = pwt->wt_aux;

  pbs_errno = PBSE_NONE;

  if (req->rq_type != PBS_BATCH_MoveJob)
    {
    sprintf(log_buffer, "bad request type %d\n",
            req->rq_type);

    log_err(-1, id, log_buffer);

    return;
    }

  jobp = find_job(req->rq_ind.rq_move.rq_jid);

  if ((jobp == NULL) || (jobp != (job *)pwt->wt_parm1))
    {
    sprintf(log_buffer, "job %s not found\n",
            req->rq_ind.rq_move.rq_jid);

    log_err(-1, id, log_buffer);
    }

  if (WIFEXITED(stat))
    {
    r = WEXITSTATUS(stat);

    if (r == 0)
      {
      /* purge server's job structure */

      if (jobp->ji_qs.ji_svrflags & JOB_SVFLG_StagedIn)
        remove_stagein(jobp);

      if (jobp->ji_qs.ji_svrflags & JOB_SVFLG_CHECKPOINT_COPIED)
        remove_checkpoint(jobp);

      strcpy(log_buffer, msg_movejob);

      sprintf(log_buffer + strlen(log_buffer), msg_manager,
              req->rq_ind.rq_move.rq_destin,
              req->rq_user,
              req->rq_host);

      job_purge(jobp);
      }
    else
      {
      r = PBSE_ROUTEREJ;
      }
    }
  else
    {
    r = PBSE_SYSTEM;

    sprintf(log_buffer, msg_badexit, stat);

    strcat(log_buffer, id);

    log_event(
      PBSEVENT_SYSTEM,
      PBS_EVENTCLASS_JOB,
      jobp->ji_qs.ji_jobid,
      log_buffer);
    }

  if (r)
    {
    if (jobp != NULL)
      {
      /* force re-eval of job state out of Transit */

      svr_evaljobstate(jobp, &newstate, &newsub, 1);
      svr_setjobstate(jobp, newstate, newsub);
      }

    req_reject(r, 0, req, NULL, NULL);
    }
  else
    {
    reply_ack(req);
    }

  return;
  }  /* END post_movejob() */





/*
 * send_job - send a job over the network to some other server or MOM
 *
 * Start a child to do the work.  Connect to the destination host and port,
 * and go through the protocol to transfer the job.
 *
 * @see svr_strtjob2() - parent
 *
 * Returns (parent): 2 on success (child forked),
 *    -1 on failure (pbs_errno set to error number)
 *
 * Child exit status:
 *  0 success, job sent
 *  1 permanent failure or rejection
 *  2 failed but try again
 */

int send_job(

  job       *jobp,
  pbs_net_t  hostaddr, /* host address, host byte order */
  int        port, /* service port, host byte order */
  int        move_type, /* move, route, or execute */
  void (*post_func)(struct work_task *),     /* after move */
  void      *data)  /* ptr to optional batch_request to be put */
                    /* in the work task structure */

  {
  tlist_head  attrl;
  enum conn_type cntype = ToServerDIS;
  int    con;
  char  *destin = jobp->ji_qs.ji_destin;
  int    encode_type;
  int    i;
  int    NumRetries;

  char  *id = "send_job";

  attribute *pattr;

  pid_t  pid;

  struct attropl *pqjatr;      /* list (single) of attropl for quejob */
  char  *safail = "sigaction failed\n";
  char  *spfail = "sigprocmask failed\n";
  char   script_name[MAXPATHLEN + 1];
  sigset_t  child_set, all_set;

  struct  sigaction child_action;

  struct work_task *ptask;

  mbool_t        Timeout = FALSE;

  char          *pc;

  sigemptyset(&child_set);
  sigaddset(&child_set, SIGCHLD);
  sigfillset(&all_set);

  /* block SIGCHLD until work task is established */

  if (sigprocmask(SIG_BLOCK, &child_set, NULL) == -1)
    {
    log_err(errno,id,spfail);

    pbs_errno = PBSE_SYSTEM;

    log_event(
      PBSEVENT_JOB,
      PBS_EVENTCLASS_JOB,
      jobp->ji_qs.ji_jobid,
      "cannot set signal mask");

    return(-1);
    }

  if (LOGLEVEL >= 6)
    {
    sprintf(log_buffer,"about to send job - type=%d",
      move_type);
 
    log_event(
      PBSEVENT_JOB,
      PBS_EVENTCLASS_JOB,
      jobp->ji_qs.ji_jobid,
      "forking in send_job");
    }

  pid = fork();

  if (pid == -1)
    {
    /* error on fork */

    log_err(errno, id, "fork failed\n");

    if (sigprocmask(SIG_UNBLOCK, &child_set, NULL) == -1)
      log_err(errno, id, spfail);

    pbs_errno = PBSE_SYSTEM;

    return(-1);
    }

  if (pid != 0)
    {
    /* The parent (main server) */

    /* create task to monitor job startup */

    /* CRI:   need way to report to scheduler job is starting, not started */

    ptask = set_task(WORK_Deferred_Child, pid, post_func, jobp);

    if (ptask == NULL)
      {
      log_err(errno, id, msg_err_malloc);

      return(-1);
      }

    ptask->wt_parm2 = data;

    append_link(
      &((job *)jobp)->ji_svrtask,
      &ptask->wt_linkobj,
      ptask);

    /* now can unblock SIGCHLD */

    if (sigprocmask(SIG_UNBLOCK, &child_set, NULL) == -1)
      log_err(errno, id, spfail);

    if (LOGLEVEL >= 1)
      {
      extern long   DispatchTime[];
      extern job   *DispatchJob[];
      extern char  *DispatchNode[];

      extern time_t time_now;

      struct pbsnode *NP;

      /* record job dispatch time */

      int jindex;

      for (jindex = 0;jindex < 20;jindex++)
        {
        if (DispatchJob[jindex] == NULL)
          {
          DispatchTime[jindex] = time_now;

          DispatchJob[jindex] = jobp;

          if ((NP = PGetNodeFromAddr(hostaddr)) != NULL)
            DispatchNode[jindex] = NP->nd_name;
          else
            DispatchNode[jindex] = NULL;

          break;
          }
        }
      }

    /* SUCCESS */

    return(2);
    }  /* END if (pid != 0) */

  /*
   * the child process
   *
   * set up signal catcher for error return
   */

  rpp_terminate();

  child_action.sa_handler = net_move_die;

  sigfillset(&child_action.sa_mask);

  child_action.sa_flags = 0;

  if (sigaction(SIGHUP, &child_action, NULL))
    log_err(errno, id, safail);

  if (sigaction(SIGINT, &child_action, NULL))
    log_err(errno, id, safail);

  if (sigaction(SIGQUIT, &child_action, NULL))
    log_err(errno, id, safail);

  /* signal handling is set, now unblock */

  if (sigprocmask(SIG_UNBLOCK, &child_set, NULL) == -1)
    log_err(errno, id, spfail);

  /* encode job attributes to be moved */

  CLEAR_HEAD(attrl);

  /* select attributes/resources to send based on move type */

  if (move_type == MOVE_TYPE_Exec)
    {
    /* moving job to MOM - ie job start */

    resc_access_perm = ATR_DFLAG_MOM;
    encode_type = ATR_ENCODE_MOM;
    cntype = ToServerDIS;
    }
  else
    {
    /* moving job to alternate server? */

    resc_access_perm =
      ATR_DFLAG_USWR |
      ATR_DFLAG_OPWR |
      ATR_DFLAG_MGWR |
      ATR_DFLAG_SvRD;

    encode_type = ATR_ENCODE_SVR;

    /* clear default resource settings */

    svr_dequejob(jobp);
    }

  pattr = jobp->ji_wattr;

  for (i = 0;i < JOB_ATR_LAST;i++)
    {
    if (((job_attr_def + i)->at_flags & resc_access_perm) ||
      ((strncmp((job_attr_def + i)->at_name,"session_id",10) == 0) &&
      (jobp->ji_wattr[JOB_ATR_checkpoint_name].at_flags & ATR_VFLAG_SET)))
      {
      (job_attr_def + i)->at_encode(
        pattr + i,
        &attrl,
        (job_attr_def + i)->at_name,
        NULL,
        encode_type);
      }
    }    /* END for (i) */

  attrl_fixlink(&attrl);

  /* put together the job script file name */

  strcpy(script_name, path_jobs);

  if (jobp->ji_wattr[JOB_ATR_job_array_request].at_flags & ATR_VFLAG_SET)
    {
    strcat(script_name, jobp->ji_arraystruct->ai_qs.fileprefix);
    }
  else
    {
    strcat(script_name, jobp->ji_qs.ji_fileprefix);
    }

  strcat(script_name, JOB_SCRIPT_SUFFIX);


  pbs_errno = 0;
  con = -1;

  for (NumRetries = 0;NumRetries < RETRY;NumRetries++)
    {
    int rc;

    /* connect to receiving server with retries */

    if (NumRetries > 0)
      {
      /* recycle after an error */

      if (con >= 0)
        svr_disconnect(con);

      /* check pbs_errno from previous attempt */

      if (should_retry_route(pbs_errno) == -1)
        {
        sprintf(log_buffer, "child failed in previous commit request for job %s",
                jobp->ji_qs.ji_jobid);

        log_err(pbs_errno, id, log_buffer);

        exit(1); /* fatal error, don't retry */
        }

      sleep(1 << NumRetries);
      }

    /* NOTE:  on node hangs, svr_connect is successful */

    if ((con = svr_connect(hostaddr, port, 0, cntype)) == PBS_NET_RC_FATAL)
      {
      sprintf(log_buffer, "send_job failed to %lx port %d",
        hostaddr,
        port);

      log_err(pbs_errno, id, log_buffer);

      exit(1);
      }

    if (con == PBS_NET_RC_RETRY)
      {
      pbs_errno = 0; /* should retry */

      continue;
      }

    /*
     * if the job is substate JOB_SUBSTATE_TRNOUTCM which means
     * we are recovering after being down or a late failure, we
     * just want to send the "ready-to-commit/commit"
     */

    if (jobp->ji_qs.ji_substate != JOB_SUBSTATE_TRNOUTCM)
      {
      if (jobp->ji_qs.ji_substate != JOB_SUBSTATE_TRNOUT)
        {
        jobp->ji_qs.ji_substate = JOB_SUBSTATE_TRNOUT;

        job_save(jobp, SAVEJOB_QUICK, 0);
        }

      pqjatr = &((svrattrl *)GET_NEXT(attrl))->al_atopl;

      if ((pc = PBSD_queuejob(
                  con,
                  jobp->ji_qs.ji_jobid,
                  destin,
                  pqjatr,
                  NULL)) == NULL)
        {
        if (pbs_errno == PBSE_EXPIRED)
          {
          /* queue job timeout based on pbs_tcp_timeout */

          Timeout = TRUE;
          }

        if ((pbs_errno == PBSE_JOBEXIST) && (move_type == MOVE_TYPE_Exec))
          {
          /* already running, mark it so */

          log_event(
            PBSEVENT_ERROR,
            PBS_EVENTCLASS_JOB,
            jobp->ji_qs.ji_jobid,
            "MOM reports job already running");

          exit(0);
          }

        sprintf(log_buffer, "send of job to %s failed error = %d",
          destin,
          pbs_errno);

        log_event(
          PBSEVENT_JOB,
          PBS_EVENTCLASS_JOB,
          jobp->ji_qs.ji_jobid,
          log_buffer);

        continue;
        }  /* END if ((pc = PBSD_queuejob() == NULL) */

      free(pc);

      if (jobp->ji_qs.ji_svrflags & JOB_SVFLG_SCRIPT)
        {
        if (PBSD_jscript(con, script_name, jobp->ji_qs.ji_jobid) != 0)
          continue;
        }

      /* XXX may need to change the logic below, if we are sending the job to
         a mom on the same host and the mom and server are not sharing the same
         spool directory, then we still need to move the file */

      if ((move_type == MOVE_TYPE_Exec) &&
          (jobp->ji_qs.ji_svrflags & JOB_SVFLG_HASRUN) &&
          (hostaddr != pbs_server_addr))
        {
        /* send files created on prior run */

        if ((move_job_file(con,jobp,StdOut) != 0) ||
            (move_job_file(con,jobp,StdErr) != 0) ||
            (move_job_file(con,jobp,Checkpoint) != 0))
          {
          continue;
          }
        }

      /* ignore signals */

      if (sigprocmask(SIG_BLOCK, &all_set, NULL) == -1)
        log_err(errno, id, "sigprocmask\n");

      jobp->ji_qs.ji_substate = JOB_SUBSTATE_TRNOUTCM;

      job_save(jobp, SAVEJOB_QUICK, 0);
      }
    else
      {
      /* ignore signals */

      if (sigprocmask(SIG_BLOCK, &all_set, NULL) == -1)
        log_err(errno, id, "sigprocmask\n");
      }

    if (PBSD_rdytocmt(con, jobp->ji_qs.ji_jobid) != 0)
      {
      if (sigprocmask(SIG_UNBLOCK, &all_set, NULL) == -1)
        log_err(errno, id, "sigprocmask\n");

      continue;
      }


    if ((rc = PBSD_commit(con, jobp->ji_qs.ji_jobid)) != 0)
      {
      int errno2;

      /* NOTE:  errno is modified by log_err */

      errno2 = errno;

      sprintf(log_buffer, "send_job commit failed, rc=%d (%s)",
              rc,
              (connection[con].ch_errtxt != NULL) ? connection[con].ch_errtxt : "N/A");

      log_ext(errno2, id, log_buffer, LOG_WARNING);

      /* if failure occurs, pbs_mom should purge job and pbs_server should set *
         job state to idle w/error msg */

      if (errno2 == EINPROGRESS)
        {
        /* request is still being processed */

        /* increase tcp_timeout in qmgr? */

        Timeout = TRUE;

        /* do we need a continue here? */

        sprintf(log_buffer, "child commit request timed-out for job %s, increase tcp_timeout?",
                jobp->ji_qs.ji_jobid);

        log_ext(errno2, id, log_buffer, LOG_WARNING);

        /* don't retry on timeout--break out and report error! */

        break;
        }
      else
        {
        sprintf(log_buffer, "child failed in commit request for job %s",
                jobp->ji_qs.ji_jobid);

        log_ext(errno2, id, log_buffer, LOG_CRIT);

        /* FAILURE */

        exit(1);
        }
      }    /* END if ((rc = PBSD_commit(con,jobp->ji_qs.ji_jobid)) != 0) */

    svr_disconnect(con);

    /* child process is done */

    /* SUCCESS */

    exit(0);
    }  /* END for (NumRetries) */

  if (con >= 0)
    svr_disconnect(con);

  if (Timeout == TRUE)
    {
    /* 10 indicates that job migrate timed out, server will mark node down *
          and abort the job - see post_sendmom() */

    sprintf(log_buffer, "child timed-out attempting to start job %s",
            jobp->ji_qs.ji_jobid);

    log_ext(pbs_errno, id, log_buffer, LOG_WARNING);

    exit(10);
    }

  if (should_retry_route(pbs_errno) == -1)
    {
    sprintf(log_buffer, "child failed and will not retry job %s",
      jobp->ji_qs.ji_jobid);

    log_err(pbs_errno, id, log_buffer);

    exit(1);
    }

  exit(2);

  /*NOTREACHED*/

  return(0);
  }  /* END send_job() */





/*
 * net_move_die - clean up child and report bad status to parent.
 *
 * Returns: exit with status of 1.
 */

static void net_move_die(

  int sig)

  {
  sprintf(log_buffer, "Routing child got signal %d\n",
          sig);

  log_event(
    PBSEVENT_SYSTEM,
    PBS_EVENTCLASS_SERVER,
    "net_move_die",
    log_buffer);

  exit(1);
  }




/*
 * net_move - move a job over the network to another queue.
 *
 * Get the address of the destination server and call send_job()
 *
 * Returns: 2 on success (child started, see send_job()), -1 on error
 */

int net_move(

  job                  *jobp,
  struct batch_request *req)

  {
  void  *data;
  char  *destination = jobp->ji_qs.ji_destin;
  pbs_net_t  hostaddr;
  char  *hostname;
  int   move_type;
  unsigned int  port = pbs_server_port_dis;
  void (*post_func)(struct work_task *);
  char  *toserver;
  char  *id = "net_move";

  /* Determine to whom are we sending the job */

  if ((toserver = strchr(destination, '@')) == NULL)
    {
    sprintf(log_buffer, "no server specified in %s\n",
            destination);

    log_err(-1, id, log_buffer);

    return(-1);
    }

  toserver++;  /* point to server name */

  hostname = parse_servername(toserver, &port);
  hostaddr = get_hostaddr(hostname);

  if (req)
    {
    /* note, in this case, req is the orginal Move Request */

    move_type = MOVE_TYPE_Move;
    post_func = post_movejob;

    data      = req;
    }
  else
    {
    /* note, in this case req is NULL */

    move_type = MOVE_TYPE_Route;
    post_func = post_routejob;

    data      = 0;
    }

  svr_setjobstate(jobp, JOB_STATE_TRANSIT, JOB_SUBSTATE_TRNOUT);

  return(send_job(
           jobp,
           hostaddr,
           port,
           move_type,
           post_func,
           data));
  }  /* END net_move() */





/*
 * should_retry_route - should the route be retried based on the error return
 *
 * Certain error are temporary, and that destination should not be
 * considered bad.
 *
 * Return:  1 if should retry this destination
 *  -1 if destination should not be retried
 */

static int should_retry_route(

  int err)

  {
  switch (err)
    {
    case 0:

    case EADDRINUSE:

    case EADDRNOTAVAIL:

    case PBSE_SYSTEM:

    case PBSE_INTERNAL:

    case PBSE_EXPIRED:

    case PBSE_MAXQUED:

    case PBSE_MAXUSERQUED:

    case PBSE_QUNOENB:

    case PBSE_NOCONNECTS:

      /* retry destination */

      return(1);

      /*NOTREACHED*/

      break;

    default:

      /* NO-OP */

      break;
    }

  return(-1);
  }  /* END should_retry_route() */





static int
move_job_file(int conn, job *pjob, enum job_file which)
  {
  char path[MAXPATHLEN+1];

  (void)strcpy(path, path_spool);
  (void)strcat(path, pjob->ji_qs.ji_fileprefix);

  if (which == StdOut)
    (void)strcat(path, JOB_STDOUT_SUFFIX);
  else if (which == StdErr)
    (void)strcat(path, JOB_STDERR_SUFFIX);
  else if (which == Checkpoint)
    (void)strcat(path, JOB_CHECKPOINT_SUFFIX);

  if (access(path, F_OK) < 0)
    {
    if (errno == ENOENT)
      return (0);
    else
      return (errno);
    }

  return PBSD_jobfile(conn, PBS_BATCH_MvJobFile, path, pjob->ji_qs.ji_jobid, which);
  }
