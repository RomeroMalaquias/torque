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
 * Synopsis:
 * 	int diswl_(int stream, dis_long_double_t value, unsigned int ndigs)
 *
 *	Converts <value> into a Data-is-Strings floating point number and sends
 *	it to <stream>.  The converted number consists of two consecutive signed
 *	integers.  The first is the coefficient, at most <ndigs> long, with its
 *	implied decimal point at the low-order end.  The second is the exponent
 *	as a power of 10.
 *
 *	This function is only invoked through the macros, diswf, diswd, and
 *	diswl, which are defined in the header file, dis.h.
 *
 *	Returns DIS_SUCCESS if everything works well.  Returns an error code
 *	otherwise.  In case of an error, no characters are sent to <stream>.
 */

#include <pbs_config.h>   /* the master config generated by configure */

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include "dis.h"
#include "dis_.h"

/* to work around a problem in a compiler */
#if SIZEOF_LONG_DOUBLE == SIZEOF_DOUBLE
#define LDBL_MAX DBL_MAX
#endif 

int diswl_(stream, value, ndigs)
    int			stream;
    dis_long_double_t		value;
    unsigned		ndigs;
    {
	int		c;
	int		expon;
	int		negate;
	int		retval;
	unsigned	pow2;
	char		*cp;
	char		*ocp;
	dis_long_double_t	ldval;

	assert(ndigs > 0 && ndigs <= LDBL_DIG);
	assert(stream >= 0);
	assert(dis_puts != NULL);
	assert(disw_commit != NULL);

/* Make zero a special case.  If we don't it will blow exponent		*/
/* calculation.								*/
	if (value == 0.0L) {
		retval = (*dis_puts)(stream, "+0+0", 4) < 0 ?
		    DIS_PROTO : DIS_SUCCESS;
		return (((*disw_commit)(stream, retval == DIS_SUCCESS) < 0) ?
		       DIS_NOCOMMIT : retval);
	}
/* Extract the sign from the coefficient.				*/
        ldval = (negate = value < 0.0L) ? -value : value;
/* Detect and complain about the infinite form.				*/
	if (ldval > LDBL_MAX)
	        return (DIS_HUGEVAL);
/* Compute the integer part of the log to the base 10 of ldval.  As a	*/
/* byproduct, reduce the range of ldval to the half-open interval,      */
/* [1, 10).								*/
	if (dis_lmx10 == 0)
	        disi10l_();
	expon = 0;
	pow2 = dis_lmx10 + 1;
	if (ldval < 1.0L) {
		do {
			if (ldval < dis_ln10[--pow2]) {
				ldval *= dis_lp10[pow2];
				expon += 1 << pow2;
			}
		} while (pow2);
		ldval *= 10.0;
		expon = -expon - 1;
	} else {
		do {
			if (ldval >= dis_lp10[--pow2]) {
				ldval *= dis_ln10[pow2];
				expon += 1 << pow2;
			}
		} while (pow2);
	}
/* Round the value to the last digit					*/
	ldval += 5.0L * disp10l_(-ndigs);
	if (ldval >= 10.0L) {
		expon++;
		ldval *= 0.1L;
	}
/* Starting in the middle of the buffer, convert coefficient digits,	*/
/* most significant first.						*/
	ocp = cp = &dis_buffer[DIS_BUFSIZ - ndigs];
	do {
		c = ldval;
		ldval = (ldval - c) * 10.0L;
		*ocp++ = c + '0';
	} while (--ndigs);
/* Eliminate trailing zeros.						*/
	while (*--ocp == '0');
/* The decimal point is at the low order end of the coefficient		*/
/* integer, so adjust the exponent for the number of digits in the	*/
/* coefficient.								*/
	ndigs = ++ocp - cp;
	expon -= ndigs - 1;
/* Put the coefficient sign into the buffer, left of the coefficient.	*/
	*--cp = negate ? '-' : '+';
/* Insert the necessary number of counts on the left.			*/
	while (ndigs > 1)
	        cp = discui_(cp, ndigs, &ndigs);
/* The complete coefficient integer is done.  Put it out.		*/
	retval = (*dis_puts)(stream, cp, (size_t)(ocp - cp)) < 0 ?
	    DIS_PROTO : DIS_SUCCESS;
/* If that worked, follow with the exponent, commit, and return.	*/
	if (retval == DIS_SUCCESS)
		return (diswsi(stream, expon));
/* If coefficient didn't work, negative commit and return the error.	*/
	return (((*disw_commit)(stream, FALSE) < 0)  ? DIS_NOCOMMIT : retval);
}
