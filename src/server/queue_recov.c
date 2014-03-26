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
 * queue_recov.c - This file contains the functions to record a queue
 * data struture to disk and to recover it from disk.
 *
 * The data is recorded in a file whose name is the queue name
 *
 * The following public functions are provided:
 *  que_save()  - save the disk image
 *  que_recov()  - recover (read) queue from disk
 */

#include <pbs_config.h>   /* the master config generated by configure */
#include "queue_recov.h"

#include <sys/types.h>
#include <sys/param.h>
#include "pbs_ifl.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "server_limits.h"
#include "list_link.h"
#include "attribute.h"
#include "queue.h"
#include "svrfunc.h"
#include "log.h"
#include "../lib/Liblog/pbs_log.h"
#include "../lib/Liblog/log_event.h"
#include "../lib/Libifl/lib_ifl.h"
#include "utils.h"
#include <pthread.h>
#include "queue_func.h" /* que_alloc, que_free */

/* data global to this file */

extern char   *path_queues;



/*
 * que_save() - Saves a queue structure image on disk
 *
 *
 * For a save, to ensure no data is ever lost due to system crash:
 * 1. write new image to a new file using a temp name
 * 2. rename the new file over the old one
 *
 * Then, if the queue has any access control lists, they are saved
 * to their own files.
 */

int que_save(

  pbs_queue *pque)

  {
  int fds;
  int rc;
  char namebuf1[MAXPATHLEN];
  char namebuf2[MAXPATHLEN];
  char buf[MAXLINE<<8];

  pque->qu_attr[QA_ATR_MTime].at_val.at_long = time(NULL);
  pque->qu_attr[QA_ATR_MTime].at_flags = ATR_VFLAG_SET;

  snprintf(namebuf1,sizeof(namebuf1),
    "%s%s",
    path_queues,
    pque->qu_qs.qu_name);
  snprintf(namebuf2,sizeof(namebuf2),"%s.new",namebuf1);

  fds = open(namebuf2, O_CREAT | O_WRONLY | O_Sync, 0600);

  if (fds < 0)
    {
    log_err(errno, __func__, (char *)"open error");

    return(-1);
    }

  /* save basic queue structure (fixed length stuff) */
  snprintf(buf,sizeof(buf),
    "<queue>\n<modified>%d</modified>\n<type>%d</type>\n<create_time>%llu</create_time>\n<modify_time>%llu</modify_time>\n<name>%s</name>\n",
    pque->qu_qs.qu_modified,
    pque->qu_qs.qu_type,
    (long long)pque->qu_qs.qu_ctime,
    (long long)pque->qu_qs.qu_mtime,
    pque->qu_qs.qu_name);

  if ((rc = write_buffer(buf,strlen(buf),fds)))
    {
    log_err(rc, __func__, (char *)"unable to write to the file");
    close(fds);
    return(-1);
    }

  /* save queue attributes  */

  if (save_attr_xml(que_attr_def, pque->qu_attr, QA_ATR_LAST,fds) != 0)
    {
    log_err(-1, __func__, (char *)"save_attr failed");
    close(fds);
    return(-1);
    }

  /* close the queue tag */
  snprintf(buf,sizeof(buf),"</queue>");
  if ((rc = write_buffer(buf,strlen(buf),fds)))
    {
    log_err(rc, __func__, (char *)"unable to write to the queue's file");
    close(fds);
    return(-1);
    }

  /* Close the file descriptor and check for errors */
  if (close(fds) < 0)
    {
    log_err(errno, __func__, (char *)"unable to close the queue's file");
    return(-1);
    }

  if (rename(namebuf2, namebuf1) < 0)
    {
    log_err(errno, __func__, (char *)"unable to replace queue file");
    return(-1);
    }

  return(0);
  } /* END que_save_xml() */





pbs_queue *que_recov_xml(

  char *filename)

  {
  int          fds;
  int          rc;
  pbs_queue   *pq;
  char         namebuf[MAXPATHLEN];
  char         buf[MAXLINE<<10];
  char        *parent;
  char        *child;

  char        *current;
  char        *begin;
  char        *end;
  char         log_buf[LOCAL_LOG_BUF_SIZE];
  time_t       time_now = time(NULL);

  pq = que_alloc(filename, TRUE);  /* allocate & init queue structure space */

  if (pq == NULL)
    {
    log_err(-1, __func__, "que_alloc failed");

    return(NULL);
    }

  snprintf(namebuf, sizeof(namebuf), "%s%s", path_queues, filename);

  fds = open(namebuf, O_RDONLY, 0);

  if (fds < 0)
    {
    log_err(errno, __func__, "open error");

    que_free(pq, TRUE);

    return(NULL);
    }

  /* read in queue save sub-structure */
  if (read_ac_socket(fds,buf,sizeof(buf)) < 0)
    {
    snprintf(log_buf,sizeof(log_buf),
      "Unable to read from queue file %s",
      filename);
    log_err(errno, __func__, log_buf);
    
    close(fds);
    
    que_free(pq, TRUE);

    return(NULL);
    }

  current = begin = buf;

  /* advance past the queue tag */
  current = strstr(current,"<queue>");
  if (current == NULL)
    {
    log_event(PBSEVENT_SYSTEM,
      PBS_EVENTCLASS_SERVER,
      __func__,
      "Cannot find a queue tag, attempting to load legacy format");
    que_free(pq, TRUE);
    
    close(fds);

    return(que_recov(filename));
    }

  end = strstr(current,"</queue>");

  if (end == NULL)
    {
    log_err(-1, __func__, "No queue tag found in the queue file???");
    que_free(pq, TRUE);
    close(fds);
    return(NULL);
    }

  /* move past the queue tag */
  current += strlen("<queue>");
  /* adjust the end for the newline preceeding the close queue tag */
  end--;

  while (current < end)
    {
    if (get_parent_and_child(current,&parent,&child,&current))
      {
      /* ERROR */
      snprintf(log_buf,sizeof(log_buf),
        "Bad XML in the queue file at: %s",
        current);
      log_err(-1, __func__, log_buf);

      que_free(pq, TRUE);
      close(fds);
      return(NULL);
      }

    if (!strcmp(parent,"modified"))
      pq->qu_qs.qu_modified = atoi(child);
    else if (!strcmp(parent,"type"))
      pq->qu_qs.qu_type = atoi(child);
    else if (!strcmp(parent,"create_time"))
      pq->qu_qs.qu_ctime = atoi(child);
    else if (!strcmp(parent,"modify_time"))
      pq->qu_qs.qu_mtime = atoi(child);
    else if (!strcmp(parent,"name"))
      snprintf(pq->qu_qs.qu_name,sizeof(pq->qu_qs.qu_name),"%s",child);
    else if (!strcmp(parent,"attributes"))
      {
      char *attr_ptr = child;
      char *child_parent;
      char *child_attr;

      while (*attr_ptr != '\0')
        {
        if (get_parent_and_child(attr_ptr,&child_parent,&child_attr,&attr_ptr))
          {
          /* ERROR */
          snprintf(log_buf,sizeof(log_buf),
            "Bad XML in the queue file at: %s",
            current);
          log_err(-1, __func__, log_buf);
          
          que_free(pq, TRUE);
          close(fds);
          return(NULL);
          }

        if ((rc = str_to_attr(child_parent,child_attr,pq->qu_attr,que_attr_def,QA_ATR_LAST)))
          {
          /* ERROR */
          snprintf(log_buf,sizeof(log_buf),
            "Error creating attribute %s",
            child_parent);
          log_err(rc, __func__, log_buf);

          que_free(pq, TRUE);
          close(fds);
          return(NULL);
          }
        }
      }
    } 

  /* all done recovering the queue */

  close(fds);

  if ((pq->qu_attr[QA_ATR_MTime].at_flags & ATR_VFLAG_SET) == 0)
    {
    /* if we are recovering a pre-2.1.2 queue, save a new mtime */

    pq->qu_attr[QA_ATR_MTime].at_val.at_long = time_now;
    pq->qu_attr[QA_ATR_MTime].at_flags = ATR_VFLAG_SET;

    que_save(pq);
    }

  return(pq);
  } /* END que_recov_xml() */





/*
 * que_recov() - load (recover) a queue from its save file
 *
 * This function is only needed upon server start up.
 *
 * The queue structure is recovered from the disk.
 * Space to hold the above is calloc-ed as needed.
 *
 * Returns: pointer to new queue structure if successful
 *   null if error
 */

pbs_queue *que_recov(

  char *filename) /* pathname to queue save file */

  {
  int        fds;
  int        i;
  pbs_queue *pq;
  char       namebuf[MAXPATHLEN];
  time_t     time_now = time(NULL);

  pq = que_alloc(filename, TRUE);  /* allocate & init queue structure space */

  if (pq == NULL)
    {
    log_err(-1, __func__, "que_alloc failed");

    return(NULL);
    }

  snprintf(namebuf, sizeof(namebuf), "%s%s", path_queues, filename);

  fds = open(namebuf, O_RDONLY, 0);

  if (fds < 0)
    {
    log_err(errno, __func__, "open error");

    que_free(pq, TRUE);

    return(NULL);
    }

  /* read in queue save sub-structure */

  if (read_ac_socket(fds, (char *)&pq->qu_qs, sizeof(queuefix)) !=
      sizeof(queuefix))
    {
    log_err(errno, __func__, "read error");
    que_free(pq, TRUE);
    close(fds);
    return ((pbs_queue *)0);
    }

  /* read in queue attributes */

  if (recov_attr(fds, pq, que_attr_def, pq->qu_attr,
	               QA_ATR_LAST, 0, TRUE) != 0)
    {
    log_err(-1, __func__, "recov_attr[common] failed");
    que_free(pq, TRUE);
    close(fds);
    return ((pbs_queue *)0);
    }

  /*
   * now reload the access control lists, these attributes were
   * saved separately
   */

  for (i = 0;i < QA_ATR_LAST;i++)
    {
    if (pq->qu_attr[i].at_type == ATR_TYPE_ACL)
      {
      recov_acl(
        &pq->qu_attr[i],
        &que_attr_def[i],
        que_attr_def[i].at_name,
        pq->qu_qs.qu_name);
      }
    }

  /* all done recovering the queue */

  close(fds);

  if ((pq->qu_attr[QA_ATR_MTime].at_flags & ATR_VFLAG_SET) == 0)
    {
    /* if we are recovering a pre-2.1.2 queue, save a new mtime */

    pq->qu_attr[QA_ATR_MTime].at_val.at_long = time_now;
    pq->qu_attr[QA_ATR_MTime].at_flags = ATR_VFLAG_SET;

    que_save(pq);
    }

  return(pq);
  }
