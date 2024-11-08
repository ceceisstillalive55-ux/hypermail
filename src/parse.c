/*
** Copyright (C) 1994, 1995 Enterprise Integration Technologies Corp.
**         VeriFone Inc./Hewlett-Packard. All Rights Reserved.
** Kevin Hughes, kev@kevcom.com 3/11/94
** Kent Landfield, kent@landfield.com 4/6/97
** Hypermail Project 1998-2023
**
** This program and library is free software; you can redistribute it and/or
** modify it under the terms of the GNU (Library) General Public License
** as published by the Free Software Foundation; either version 3
** of the License, or any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU (Library) General Public License for more details.
**
** You should have received a copy of the GNU (Library) General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA
*/

#include <fcntl.h>

#include "hypermail.h"
#include "setup.h"
#include "struct.h"
#include "uudecode.h"
#include "base64.h"
#include "search.h"
#include "getname.h"
#include "parse.h"
#include "print.h"

#ifdef GDBM
#include "gdbm.h"
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifdef HAVE_DIRENT_H
#ifdef __LCC__
#include "../lcc/dirent.h"
#include <direct.h>
#else
#include <dirent.h>
#endif
#else
#include <sys/dir.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

/*
** LCC doesn't have lstat() defined in sys/stat.h.  We'll define it
** in lcc_extras.h, but really it just calls stat().
*/
#ifdef __LCC__
#include <sys/stat.h>
#include "../lcc/lcc_extras.h"
#endif

#define NEW_PARSER 1

typedef enum {
    ENCODE_NORMAL,
    ENCODE_QP,			/* quoted printable */

    ENCODE_MULTILINED,		/* this is not a real type, but just a separator showing
				   that the types below are encoded in a way that makes
				   one line in the indata may become one or more lines
				   in the outdata */

    ENCODE_BASE64,		/* base64 */
    ENCODE_UUENCODE,		/* well, it seems there exist some kind of semi-standard
				   for uu-encoded attachments. */

    ENCODE_UNKNOWN		/* must be the last one */
} EncodeType;

typedef enum {
    CONTENT_TEXT,		/* normal mails are text based default */
    CONTENT_BINARY,		/* this kind we store separately and href to */
    CONTENT_HTML,		/* this is html formated text */
    CONTENT_IGNORE,		/* don't care about this content */

    CONTENT_UNKNOWN		/* must be the last one */
} ContentType;

typedef enum {
    NO_FILE,
    MAKE_FILE,
    MADE_FILE
} FileStatus;		        /* for attachments */

int ignorecontent(char *type)
{
    if (inlist(set_ignore_types, "$NONPLAIN") && !textcontent(type)
	&& strncasecmp(type, "multipart/", 10))
      return 1;
    if (inlist(set_ignore_types, "$BINARY") && !textcontent(type)
	&& strcasecmp(type, "text/html") && strncasecmp(type, "multipart/", 10))
      return 1;
    return (inlist(set_ignore_types, type));
}

int inlinecontent(char *type)
{
    return (inlist(set_inline_types, type));
}

int preferedcontent(int *current_weight, char *type, int decode)
{
    int weight = -1;
    int status;

    status = 0;

    if (set_save_alts == 1)
	return 1;

    /* We let plain text remain PREFERED at all times */
    if (!strcasecmp("text/plain", type)) {
	if (*current_weight != 0) {
	    /* to avoid having two text/plain alternatives */
	    *current_weight = 0;
	    status = 1;
	}
    }
    /* find the weight of the type arg. If the weight is
       inferior to the current_weight, we make it the
       prefered content */
    else {
	if (set_prefered_types)
	    weight = inlist_pos(set_prefered_types, type);
	if (weight == -1) {	/* not known to be good, use weaker evidence */
	    if (!strncasecmp("text/", type, 5))
	        weight = 1000;
	    else weight = 2000 + decode;
	}
	if (weight != -1) {
	    /* +1 so that weight 0 is reserved for text/plain */
	    weight++;
	    if (*current_weight == -1) {
		*current_weight = weight;
		status = 1;
	    }
	    else if (*current_weight > weight) {
		*current_weight = weight;
		status = 1;
	    }
 	}
    }

    return status;
}

int textcontent(char *type)
{
    /* We let text/plain remain text at all times.  Appearantly, older mailers
     * can still use just "text" as content-type, and we better treat that as
     * text/plain to make all those users happy.  */
    if (!strcasecmp("text/plain", type) || !strcasecmp("text", type))
	return 1;

    if (set_text_types) {
	return (inlist(set_text_types, type));
    }

    return 0;
}

static int is_applemail_ua(char *ua_string)
{
    /* returns TRUE if the ua_string is one of the declared applemail
     * clients */

    int res = FALSE;

    if (ua_string && *ua_string != '\0') {
        char *buff;
        char *ptr;

        buff = strsav(ua_string);
        ptr = strcasestr(buff, " Mail (");
        if (ptr) {
            *ptr = '\0';
            res = inlist(set_applemail_ua_value, buff);
        }
        free(buff);
    }

    return res;
}

/*
 * Should return TRUE if the input is a Re: start. The end pointer should
 * then point on the first character after the Re:
 *
 * Identifies "Re:", "Fw:" as well as "Re[<number>]:" strings.
 */

int isre(char *re, char **end)
{
    char *endp = NULL;
    if (!strncasecmp("Re:", re, 3)) {
	endp = re + 3;
    }
    else if (!strncasecmp("Fw:", re, 3)) {
	endp = re + 3;
    }
    else if (!strncasecmp("Re[", re, 3)) {
	re += 3;
	strtol(re, &re, 10);	/* eat the number */
	if (!strncmp("]:", re, 2)) {
	    /* we have an end "]:" and therefore it qualifies as a Re */
	    endp = re + 2;
	}
    }
    if (endp) {
	if (end)
	    *end = endp;
	return TRUE;
    }
    return FALSE;
}

/*
 * Find the first re-substring in the input and return the position
 * where it is. The 'end' parameter will be filled in the first position
 * *after* the re.
 */

char *findre(char *in, char **end)
{
    while (*in) {
	if (isre(in, end))
	    return in;
	if (isspace(*in)) {
	  in++;
	} else {
	  break;
	}
    }
    return NULL;
}


void print_progress(int num, char *msg, char *filename)
{
    char bufstr[256];
    register int i;
    static int lastlen = 0;
    static int longest = 0;
    int len = 0;
    int newline = 0;

    newline = 0;

    if (msg != NULL) {
	if (filename != NULL) {
	    trio_snprintf(bufstr, sizeof(bufstr), "%4d %-s %-s", num, msg, filename);
	    if (set_showprogress > 1)
		newline = 1;
	}
	else {
	    trio_snprintf(bufstr, sizeof(bufstr), "%4d %-s.", num, msg);
	    newline = 1;
	}
    }
    else
	sprintf(bufstr, "%4d", num);

    for (i = 0; i < lastlen; i++)	/* Back up to the beginning of line */
	fputc('\b', stdout);

    fputs(bufstr, stdout);	/* put out the string */
    len = strlen(bufstr);	/* get length of new string */

    /*
     * If there is a new message then erase
     * the trailing info from the enw string
     */

    if (msg != NULL) {
	for (i = len; i <= longest; i++)
	    fputc(' ', stdout);
	for (i = len; i <= longest; i++)
	    fputc('\b', stdout);
    }

    lastlen = len;
    if (lastlen > longest)
	longest = lastlen;

    if (newline)
	fputc('\n', stdout);
    fflush(stdout);
}

char *safe_filename(char *name)
{
    register char *sp;
    register char *np;

    np = name;

    if (!np || *np == '\0') {
        return NULL;
    }

    /* skip leading spaces in the filename */
    while (*np && (*np == ' ' || *np == '\t'))
	np++;

    if (!*np || !(*np == '\n') || *np == '\r') {
        /* filename is made of only spaces; replace them with
           REPLACEMENT_CHAR */
        np = name;
    }

    for (sp = name, np = name; *np && *np != '\n' && *np != '\r';) {
	/* if valid character then store it */
	if (((*np >= 'a' && *np <= 'z') || (*np >= '0' && *np <= '9') ||
	     (*np >= 'A' && *np <= 'Z') || (*np == '-') || (*np == '.') ||
	     (*np == ':') || (*np == '_'))
	    && !(set_unsafe_chars && strchr(set_unsafe_chars, *np))) {
	    *sp = *np;
	}
	else	/* Need to replace the character with a safe one */
	    *sp = REPLACEMENT_CHAR;
	sp++;
	np++;
    }
    *sp = '\0';
    if (sp >= name + 6 && !strcmp(sp - 6, ".shtml"))
	strcpy(sp - 6, ".html");

    return name;
}

static void
create_attachname(char *attachname, int max_len)
{
    int i, max_i;
    char suffix[8];
    max_i = strlen(attachname);
    if(max_i >= max_len)
	max_i = max_len - 1;
    i = max_i;
    while( i >= 0 && i > (max_i - (int) sizeof(suffix)) && attachname[i] != '.' )
	--i;
    if(i >= 0 && attachname[i] == '.')
	strncpy(suffix, attachname + i, sizeof(suffix) - 1);
    else
	suffix[0] = 0;
    strncpy(attachname, set_filename_base, max_len - 1);
    /* make sure it is a NULL terminated string */
    attachname[max_len - 1] = '\0';
    strncat(attachname, suffix, max_len - strlen(attachname) - 1);
    safe_filename(attachname);
}

/*
** Cross-indexes - adds to a list of replies. If a message is a reply to
** another, the number of the message it's replying to is added to the list.
** This list is searched upon printing.
*/

void crossindex(void)
{
    int num, status, maybereply;
    struct emailinfo *email;

    num = 0;
    if(!set_linkquotes)
        replylist = NULL;

    while (num <= max_msgnum) {
	if (!hashnumlookup(num, &email)) {
	    ++num;
	    continue;
	}
	status = hashreplynumlookup(email->msgnum,
				    email->inreplyto, email->subject,
				    &maybereply);
	if (status != -1) {
	    struct emailinfo *email2;

	    if (!hashnumlookup(status, &email2)) {
		++num;
		continue;
	    }
	    /*  make sure there is no recursion between the message
                and reply lookup if a message and its reply-to were
                archived in reverse, both messages share the same
                subject (regardless of Re), and the message itself was
                a reply to a non-archived message. */
	    if (maybereply && !strcmp (email2->inreplyto, email->msgid)) {
                ++num;
                continue;
            }

	    if (set_linkquotes) {
	        struct reply *rp;
		int found_num = 0;
		for (rp = replylist; rp != NULL; rp = rp->next)
		    if(rp->msgnum == status && rp->frommsgnum == num) {
		        found_num = 1;
			break;
		    }
		if (!found_num && !(maybereply || num <= status))
#ifdef FASTREPLYCODE
		    replylist = addreply2(replylist, email2, email, maybereply,
					 &replylist_end);
#else
		    replylist = addreply(replylist, status, email, maybereply,
					 &replylist_end);
#endif
	    }
	    else {
#ifdef FASTREPLYCODE
		replylist = addreply2(replylist, email2, email, maybereply,
				      &replylist_end);
#else
		replylist = addreply(replylist, status, email, maybereply,
				     &replylist_end);
#endif
	    }
	}
	num++;
    }
#if DEBUG_THREAD
    {
	struct reply *r;
	r = replylist;
	fprintf(stderr, "START of replylist after crossindex\n");
	fprintf(stderr, "- msgnum frommsgnum maybereply msgid\n");
	while (r != NULL) {
	    fprintf(stderr, "- %d %d %d '%s'\n",
		    r->data->msgnum,
		    r->frommsgnum, r->maybereply, r->data->msgid);
	    r = r->next;
	}
	fprintf(stderr, "END of replylist after crossindex\n");
    }
#endif
}

/*
** Recursively checks for replies to replies to a message, etc.
** Replies are added to the thread list.
*/

#ifdef FASTREPLYCODE
void crossindexthread2(int num)
{
    struct reply *rp;
    struct emailinfo *ep;
    if(!hashnumlookup(num, &ep)) {
        trio_snprintf(errmsg, sizeof(errmsg),
                 "internal error crossindexthread2 %d", num);
	progerr(errmsg);
    }

    for (rp = ep->replylist; rp != NULL; rp = rp->next) {
	if (!(rp->data->flags & USED_THREAD)) {
	    rp->data->flags |= USED_THREAD;
	    if (0) fprintf(stderr, "add thread.b %d %d %d\n", num, rp->data->msgnum, rp->msgnum);
	    threadlist = addreply(threadlist, num, rp->data, 0,
				  &threadlist_end);
#ifdef FIX_OR_DELETE_ME
            /* JK: 2023-05-17: this seems to have been a longtime typo, 
               it produces memory leaks and didn't have any use in the
               thread code. Tentatively correcting it to printthreadlist 
               and checking for side effects */
            printedlist = markasprinted(printedthreadlist, rp->msgnum);
#else
            printedthreadlist = markasprinted(printedthreadlist, rp->msgnum);
#endif
	    crossindexthread2(rp->msgnum);
	}
    }
}
#else
void crossindexthread2(int num)
{
    struct reply *rp;

    for (rp = replylist; rp != NULL; rp = rp->next) {
	if (!(rp->data->flags & USED_THREAD) && (rp->frommsgnum == num)) {
	    rp->data->flags |= USED_THREAD;
	    threadlist = addreply(threadlist, num, rp->data, 0,
				  &threadlist_end);
#ifdef FIX_OR_DELETE_ME
            /* JK: 2023-05-17: this seems to have been a longtime typo, 
               it produces memory leaks and didn't have any use in the
               thread code. Tentatively correcting it to printthreadlist 
               and checking for side effects */            
	    printedlist = markasprinted(printedthreadlist, rp->msgnum);
#endif
	    printedthreadlist = markasprinted(printedthreadlist, rp->msgnum);            
	    crossindexthread2(rp->msgnum);
	}
    }
}
#endif


/*
** First, print out the threads in order by date...
** Each message number is appended to a thread list. Threads and individual
** messages are separated by a -1.
*/

void crossindexthread1(struct header *hp)
{
    int isreply;

#ifndef FASTREPLYCODE
    struct reply *rp;
#endif

    if (hp) {
	crossindexthread1(hp->left);

#ifdef FASTREPLYCODE
	isreply = hp->data->isreply;
#else
	for (isreply = 0, rp = replylist; rp != NULL; rp = rp->next) {
	    if (rp->msgnum == hp->data->msgnum) {
		isreply = 1;
		break;
	    }
	}
#endif

	/* If this message is not a reply to any other messages then it
	 * is the first message in a thread.  If it hasn't already
	 * been dealt with, then add it to the thread list, followed by
	 * any descendants and then the end of thread marker.
	 */
	if (!isreply && !wasprinted(printedthreadlist, hp->data->msgnum) &&
	    !(hp->data->flags & USED_THREAD)) {
	    hp->data->flags |= USED_THREAD;
	    threadlist = addreply(threadlist, hp->data->msgnum, hp->data,
				  0, &threadlist_end);
	    crossindexthread2(hp->data->msgnum);
	    threadlist = addreply(threadlist, -1, NULL, 0, &threadlist_end);
	}

	crossindexthread1(hp->right);
    }
}

/*
** Grabs the date string from a Date: header. (Y2K OK)
*/

char *getmaildate(char *line)
{
    int i;
    int len;
    char *c;
    struct Push buff;

    INIT_PUSH(buff);

    c = strchr(line, ':');
    if (!*(c + 1) 
        || ((*(c + 1) == '\n')
            || (*(c + 1) == '\r'))) {
	PushString(&buff, NODATE);
	RETURN_PUSH(buff);
    }
    c += 2;
    while (*c == ' ' || *c == '\t')
	c++;
    for (i = 0, len = DATESTRLEN - 1; *c && *c != '\n' && *c != '\r' && i < len; c++)
	PushByte(&buff, *c);

    RETURN_PUSH(buff);
}

/*
** Grabs the date string from a From article separator. (Y2K OK)
*/

char *getfromdate(char *line)
{
    static char tmpdate[DATESTRLEN];
    int i;
    int len;
    char *c = NULL;

    for (i = 0; days[i] != NULL &&
	 ((c = strstr(line, days[i])) == NULL); i++);
    if (days[i] == NULL)
	tmpdate[0] = '\0';
    else {
	for (i = 0, len = DATESTRLEN - 1; *c && *c != '\n' && *c != '\r' && i < len; c++)
	    tmpdate[i++] = *c;

	tmpdate[i] = '\0';
    }
    return tmpdate;
}


/*
** Grabs the message ID, like <...> from the Message-ID: header.
*/

char *getid(char *line)
{
    int i;
    char *c;

    struct Push buff;

    INIT_PUSH(buff);

    if (strrchr(line, '<') == NULL) {
	/*
         * bozo alert!
	 *   msg-id = "<" addr-spec ">"
	 * try to recover as best we can
	 */
	c = strchr(line, ':') + 1;	/* we know this exists! */

	/* skip spaces before message ID */
	while (*c && (*c == ' ' || *c == '\t'))
	    c++;
    }
    else
	c = strrchr(line, '<') + 1;

    for (i = 0; *c && *c != '>' && *c != '\n' && *c != '\r'; c++) {
	if (*c == '\\')
	    continue;
	PushByte(&buff, *c);
	i++;
    }

    if (i == 0)
	PushString(&buff, "BOZO");

    RETURN_PUSH(buff);
}


/*
** Grabs the subject from the Subject: header.
**
** Need to add a table of Re: equivalents (different languages, MUA, etc...)
**
** Returns ALLOCATED string.
*/

char *getsubject(char *line)
{
    int i;
    int len;
    char *c;
    char *startp;
    char *strip_subject = NULL;
    char *postre = NULL;

    struct Push buff;

    INIT_PUSH(buff);

    c = strchr(line, ':');
    if (!c)
	return NULL;

    c += 2;

    if (set_stripsubject) {
	/* compute a new subject */
	strip_subject = replace(c, set_stripsubject, "");
	/* point to it */
	c = strip_subject;
    }

    while (isspace(*c))
	c++;

    startp = c;

    for (i = len = 0; c && *c && (*c != '\n') && (*c != '\r'); c++) {
	i++;
	/* keep track of the max length without trailing white spaces: */
	if (!isspace(*c))
	    len = i;
    }

    if (isre(startp, &postre)) {
	if (!*postre || (*postre == '\n') || (*postre == '\r'))
	    len = 0;
    }

    if (!len)
	PushString(&buff, NOSUBJECT);
    else
	PushNString(&buff, startp, len);

    if (set_stripsubject && (strip_subject != NULL))
	free(strip_subject);

    RETURN_PUSH(buff);
}

/*
** Grabs the annotation values given in the annotation user-defined header
**
** annotation_content is set to the value of the content annotation
** annotation_robot is set to the values of the robot annotations
** Returns TRUE if an annotation was found, FALSE otherwise.
*/

static bool
getannotation(char *line, annotation_content_t *annotation_content,
	       annotation_robot_t *annotation_robot)
{
  char *c;

  *annotation_content = ANNOTATION_CONTENT_NONE;;
  *annotation_robot = ANNOTATION_ROBOT_NONE;

  c = strchr(line, ':');
  if (!c)
    return FALSE;
  c++;

  while (*c != '\n') {
    int len;
    char *startp;

    while (isspace(*c))
      c++;

    startp = c;
    while (!isspace (*c) && *c != '\n' && *c != '\r' && *c != ',') {
      c++;
    }

    len = (int) (c-startp);
    if (len > 0) {
      if (!strncasecmp (startp, "deleted", len)) {
	*annotation_content = ANNOTATION_CONTENT_DELETED_OTHER;
	break;
      }
      else if (!strncasecmp (startp, "spam", len)) {
	*annotation_content = ANNOTATION_CONTENT_DELETED_SPAM;
	break;
      }
      else if (!strncasecmp (startp, "edited", len))
	*annotation_content = ANNOTATION_CONTENT_EDITED;
      else if (!strncasecmp (startp, "noindex", len))
	*annotation_robot |= ANNOTATION_ROBOT_NO_INDEX;
      else if (!strncasecmp (startp, "nofollow", len))
	*annotation_robot |= ANNOTATION_ROBOT_NO_FOLLOW;
    }
    if (*c == ',')
      c++;
  }

  /* only return true if at least a valid annotation was found */
  return (*annotation_content != ANNOTATION_CONTENT_NONE
	  || *annotation_robot != ANNOTATION_ROBOT_NONE);
}

/*
** Grabs the message ID, or date, from the In-reply-to: header.
**
** Maybe I'm confused but....
**     What either ? Should it not be consistent and choose to return
**     one (the msgid) as the default and fall back to date when a
**     msgid cannot be found ?
**
** Who knows what other formats are out there...
**
** In-Reply-To: <1DD9B854E27@everett.pitt.cc.nc.us>
** In-Reply-To: <199709181645.MAA02097@mail.clark.net> from "Marcus J. Ranum" at Sep 18, 97 12:41:40 pm
** In-Reply-To: <199709181645.MAA02097@mail.clark.net> from
** In-Reply-To: "L. Detweiler"'s message of Fri, 04 Feb 94 22:51:22 -0700 <199402050551.WAA16189@longs.lance.colostate.edu>
**
** The message id should always be returned for threading purposes. Mixing
** message-ids and dates just does not allow for proper threading lookups.
**
** Returns ALLOCATED string.  */

char *getreply(char *line)
{
    char *c;
    char *m;

    struct Push buff;

    INIT_PUSH(buff);

    /* Check for blank line */

    /*
     * Check for line with " from " and " at ".  Format of the line is
     *     <msgid> from "quoted user name" at date-string
     */

    if (strstr(line, " from ") != NULL) {
	if ((strstr(line, " at ")) != NULL) {
	    if ((m = strchr(line, '<')) != NULL) {
		for (m++; *m && *m != '>' && *m != '\n' && *m != '\r'; m++) {
		    PushByte(&buff, *m);
		}
		RETURN_PUSH(buff);
	    }
	}

	/*
	 * If no 'at' the line may be a continued line or a truncated line.
	 * Both will be picked up later.
	 */
    }

    /*
     * Check for line with " message of ".  Format of the line is
     *     "quoted user name"'s message of date-string <msgid>
     */

    if ((c = strstr(line, "message of ")) != NULL) {
	/*
	 * Check to see if there is a message ID on the line.
	 * If not this is a continued line and when you add a readline()
	 * function that concatenates continuation lines collapsing
	 * white space, you might want to revisit this...
	 */

	if ((m = strchr(line, '<')) != NULL) {
	    for (m++; *m && *m != '>' && *m != '\n' && *m != '\r'; m++) {
		PushByte(&buff, *m);
	    }
	    RETURN_PUSH(buff);
	}

	/* Nope... Go for the Date info... Bug... */
	c += 11;
	while (isspace(*c))
	    c++;
	if (*c == '"')
	    c++;

	for (; *c && *c != '.' && *c != '\n' && *c != '\r'; c++) {
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);
    }

    if ((c = strstr(line, "dated: ")) != NULL) {
	c += 7;
	for (; *c && *c != '.' && *c != '\n' && *c != '\r'; c++) {
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);
    }

    if ((c = strstr(line, "dated ")) != NULL) {
	c += 6;
	for (; *c && *c != '.' && *c != '\n'  && *c != '\r'; c++) {
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);

    }

    if ((c = strchr(line, '<')) != NULL) {
	c++;
	for (; *c && *c != '>' && *c != '\n' && *c != '\r'; c++) {
	    if (*c == '\\')
		continue;
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);
    }

    if ((c = strstr(line, "sage of ")) != NULL) {
	c += 8;
	if (*c == '\"')
	    c++;

	for (; *c && *c != '.' && *c != '\n'  && *c != '\r' && *c != 'f'; c++) {
	    PushByte(&buff, *c);
	}
	RETURN_PUSH(buff);
    }

    PushByte(&buff, '\0');
    RETURN_PUSH(buff);
}

/* Converts an RFC-822 header line to UTF-8. If there's no declared
** charset, it will try to use the Content-Type charset. If it fails,
** it will call the chardet library.
** If the function fails to detect the charset, it will return an
** "(invalid string)" string.
** Input:
** string: RFC-822 header line to convert
** ct_charset: charset as declared in the Content-Type line
** charsetsave: the previous detected charset for this message
** Output
** converted line (must be freed by caller)

** charsetsave: If libchardet detects a chardet that is different from
** the current charsetsave for this message, charsetsave will be
** update to the newly detected charset.
*/   
static char *
header_detect_charset_and_convert_to_utf8 (char *string,  char *ct_charset, char *charsetsave)
{
    if ( i18n_is_valid_us_ascii(string) ) {
        /* nothing to do, passing thru */
    }
        
    /* RFC6532 allows for using UTF-8 as a header value; we make
       sure that it is valid UTF-8 */
    else if ( i18n_is_valid_utf8(string) ) {
        /* "default" UTF-8 charset */
        strcpy(charsetsave, "UTF-8");
        
    } else {
        char header_name[129];
        char *header_value;
        struct Push pbuf;

        /*
        ** save the header_name:\s
        */
        INIT_PUSH(pbuf);
        
        sscanf(string, "%127[^:]", header_name);
        PushString(&pbuf, header_name);
        
        header_value = string + strlen(header_name);
        PushByte(&pbuf, *header_value);
        header_value++;
        PushByte(&pbuf, *header_value);
        header_value++;

#if defined HAVE_CHARDET && HAVE_ICONV
        {
            /*
             * try to detect the charset of the string and convert it to UTF-8;
             * in case of failure, replace the header value with "(invalid string)"
             */
            bool did_anything = FALSE;
        

            char *detected_charset;
            char *conv_string;
 
            size_t conv_string_sz;
        
            /*
            **consider the header_value everything after header_name:\s
            */
            
            /* let's try the charset if present in Content-Type */
            if (ct_charset && *ct_charset) {
                conv_string = i18n_convstring(header_value, ct_charset, "UTF-8", &conv_string_sz);
                if (conv_string) {
                    if ( i18n_is_valid_utf8(conv_string) ) {
                        PushString(&pbuf, conv_string);
                        did_anything = TRUE;
                    }
                    free(conv_string);
                }
            }
        
            /* nope, let's try the previous saved_charset */
            if ( !did_anything && charsetsave && *charsetsave) {
                conv_string = i18n_convstring(header_value, ct_charset, "UTF-8", &conv_string_sz);
                if (conv_string) {
                    if ( i18n_is_valid_utf8(conv_string) ) {
                        PushString(&pbuf, conv_string);
                        did_anything = TRUE;
                    }
                    free(conv_string);
                }        
            }
             
            /* nope, let's try to libchardet */
            if ( !did_anything ) {
                detected_charset = i18n_charset_detect(header_value);
                
                if ( detected_charset ) {
                    /* we detected a charset */
                    if ( detected_charset[0] != '\0' ) {
                        conv_string = i18n_convstring(header_value,
                                                      detected_charset, "UTF-8", &conv_string_sz);
                        if ( conv_string) {
                            if ( i18n_is_valid_utf8(conv_string) ) {
                                int detected_charsetlen =
                                    strlen(detected_charset) < 255 ? strlen(detected_charset) : 255;
                                memcpy(charsetsave, detected_charset, detected_charsetlen);
                                charsetsave[detected_charsetlen] = '\0';
                                PushString(&pbuf, conv_string);
                                did_anything = TRUE;
                            }
                            free(conv_string);
                        }
                    }
                    free(detected_charset);
                }
            }
        
            if (!did_anything) {
                PushString (&pbuf, "(invalid string)");            
            }
        }        
#else /* ! CHARDET &&  ICONV */
        PushString (&pbuf, "(invalid string)");
#endif        
        free(string);
        string = PUSH_STRING(pbuf);
    }
    
    return string;
}

static char *
extract_rfc2047_content(char *iptr)
{
    char *end_ptr, *ptr;
    struct Push buff;

    INIT_PUSH(buff);

    /* skip the charset, find the encoding */
    ptr = strchr (iptr + 2, '?');
    ptr++;
    if ((*ptr == 'Q' || *ptr == 'q' || *ptr == 'B' || *ptr == 'b')
	&& *(ptr + 1) == '?') {
      /* it's a valid encoding */
      ptr = ptr + 2;
      end_ptr = strstr(ptr, "?=");
      if (end_ptr && ptr > iptr) {
	PushNString(&buff, ptr, end_ptr - ptr);
	RETURN_PUSH(buff);
      }
    }
    return NULL;
}

/*
** RFC 2047 defines MIME extensions for mail headers.
**
** This function decodes that into binary/8bit data.
**
** Example:
**   =?iso-8859-1?q?I'm_called_?= =?iso-8859-1?q?Daniel?=
**
** Should result in "I'm called Daniel", but:
**
**   =?iso-8859-1?q?I'm_called?= Daniel
**
** Should result in "I'm called Daniel" too.
**
** Returns the newly allcated string, or the previous if nothing changed
*/

static char *mdecodeRFC2047(char *string, int length, char *charsetsave)
{
    char *iptr = string;
    char *oldptr;
    char *storage = (char *)emalloc(length*4 + 1);

    char *output = storage;

    char charset[129];
    char encoding[33];
    char dummy[129];
    char *endptr;

#ifdef NOTUSED
    char equal;
#endif
    unsigned int value;

    char didanything = FALSE;

    while (*iptr) {
	if (!strncmp(iptr, "=?", 2) &&
	    (3 == sscanf(iptr + 2, "%128[^?]?%32[^?]?%128[^ ]",
			 charset, encoding, dummy))) {
	    /* This is a full, valid 'encoded-word'. Decode! */
	    char *ptr;
	    char *blurb = extract_rfc2047_content(iptr);
	    if (!blurb) {
		*output++ = *iptr++;
		/* it wasn't a real encoded-word */
		continue;
	    }
	    ptr = blurb;

	    didanything = TRUE;	/* yes, we decode something */

	    /* we could've done this with a %n in the sscanf, but we know all
	       sscanfs don't grok that */

	    iptr +=
		2 + strlen(charset) + 1 + strlen(encoding) + 1 +
		strlen(blurb) + 2;

	    if (!strcasecmp("q", encoding)) {
		/* quoted printable decoding */
#ifdef HAVE_ICONV
                char *orig2,*output2,*output3;
                size_t len, charsetlen;
#endif
                endptr = ptr + strlen(ptr);

#ifdef HAVE_ICONV
                orig2=output2=malloc(strlen(string)+1);
                memset(output2,0,strlen(string)+1);

		for (; ptr < endptr; ptr++) {
		    switch (*ptr) {
		    case '=':
			sscanf(ptr + 1, "%02X", &value);
			*output2++ = value;
			ptr += 2;
			break;
		    case '_':
			*output2++ = ' ';
			break;
		    default:
			*output2++ = *ptr;
			break;
		    }
		}
		output3=i18n_convstring(orig2,charset,"UTF-8",&len);
		free(orig2);
		memcpy(output,output3,len);
		output += len;
		free(output3);
		charsetlen = strlen(charset) < 255 ? strlen(charset) : 255;
		memcpy(charsetsave,charset,charsetlen);
		charsetsave[charsetlen] = '\0';
#else
		for (; ptr < endptr; ptr++) {
		    switch (*ptr) {
		    case '=':
			sscanf(ptr + 1, "%02X", &value);
			*output++ = value;
			ptr += 2;
			break;
		    case '_':
			*output++ = ' ';
			break;
		    default:
			*output++ = *ptr;
			break;
		    }
		}
#endif
	    }
	    else if (!strcasecmp("b", encoding)) {
		/* base64 decoding */
#ifdef HAVE_ICONV
	        size_t charsetlen;
                size_t tmplen;
		char *output2;
                
		base64_decode_string(ptr, output);
		output2=i18n_convstring(output,charset,"UTF-8",&tmplen);
		memcpy(output,output2,tmplen);
		output += tmplen;
		free(output2);
		charsetlen = strlen(charset) < 255 ? strlen(charset) : 255;
		memcpy(charsetsave,charset,charsetlen);
		charsetsave[charsetlen] = '\0';
#else
                int len;
                
		len = base64_decode_string(ptr, output);
		output += len;
#endif
	    }
	    else {
		/* unsupported encoding type */
		strcpy(output, "<unknown>");
		output += 9;
	    }

	    free(blurb);
	    blurb = ptr = NULL;

	    oldptr = iptr;	/* save start position */

	    while (*iptr && isspace(*iptr))
		iptr++;		/* pass all whitespaces */

	    /* if this is an encoded word here, we should skip the passed
	       whitespaces. If it isn't an encoded-word, we should include the
	       whitespaces in the output. */

	    if (!strncmp(iptr, "=?", 2) &&
		(3 == sscanf(iptr + 2, "%128[^?]?%32[^?]?%128[^?]?",
			     charset, encoding, dummy)) &&
		(blurb = extract_rfc2047_content(iptr))) {
		free(blurb);
		continue;	/* this IS an encoded-word, continue from here */
	    }
	    else
		/* this IS NOT an encoded-word, move back to the first whitespace */
		iptr = oldptr;
	}
	else
	    *output++ = *iptr++;
    }
    *output = 0;

    if (didanything) {
	/* this check prevents unneccessary strsav() calls if not needed */
	free(string);		/* free old memory */

#if DEBUG_PARSE
	/* debug display */
	printf("NEW: %s\n", storage);

	{
	    char *f;
	    puts("NEW:");
	    for (f = storage; f < output; f++) {
		if (isgraph(*f))
		    printf("%c", *f);
		else
		    printf("%02X", (unsigned char)*f);
	    }
	    puts("");
	}
#endif
        /* here we should add calls to validate the utf8 string,
           to avoid security issues */
	return storage;		/* return new */
    }
    else {
	free(storage);
        return string;
    }
}

/*
** RFC 3676 format=flowed parsing routines
*/

/* 
** returns true if a string line is s signature start 
** rfc3676 gives "-- \n" and "-- \r\n" as signatures. 
** We also add "--\n" to this list, as mutt allows it
*/
static int is_sig_separator (const char *line)
{
    bool rv;
    
    if (!strcmp (line, "-- \n")
        || !strcmp (line, "-- \r\n")
        || !strcmp (line, "--\n")) {
        rv = TRUE;
    } else {
        rv = FALSE;
    }

    return rv;
}

/* get_quote_level returns the number of quotes in a line,
   following the RFC 3676 section 4.5 criteria.
*/
static int get_quotelevel (const char *line)
{
  int quoted = 0;
  const char *p = line;

  while (p && *p == '>')
  {
    quoted++;
    p++;
  }

  return quoted;
}

/*
** rfc3676_handler parses lines according to RFC 3676.  Its inputs are
** the current line to parse, the delsp value (from the message
** headers), the previous line quotelevel, and a flag saying if the
** previous line was marked as a continuing one.
**
** The function updates the quotelevel to that of the current parsed
** line. The function will update the continue_prev_flow_flag to say
** if the current line should be joined to the previous one, and, if
** positive, the padding offset that should be applied to the current
** line when merging it (for skipping quotes or space-stuffing).
**
** If delsp is true, the function will remove the space in the soft
** line break if the line is flowed.
**
** The function returns true if the current line is flowed.
**
*/
static bool rfc3676_handler (char *line, bool delsp_flag, int *quotelevel,
			     bool *continue_prev_flow_flag)
{
  int new_quotelevel = 0;
  int tmp_padding = 0;
  bool sig_sep = FALSE;
  bool flowed = FALSE;

  /* rules for evaluation if the flow should stop:
     1. new quote level is different from previous one
     2. The line is a signature "[(quotes)][(ss)]-- \n"
     3. The line is a hard break "\n"
     4. The message body has ended

     rules for removing space-stuffing:
     1. if f=f, then remove the first space of any line beginning with a space,
        before processing for f=f.
     2. space char may depend on charset.

     rules for quotes:
     1. quoted lines always begin with a '>' char. This symbol may depend on the
        msg charset.
     2. They are not ss before the quote symbol but may be after it
        appears.

     rules for seeing if a line should be flowed with the next one:
     1. line ends with a soft line break sp\n
     2. remove the sp if delsp=true; keep it otherwise

     special case, space-stuffed or f=f? A line that has only this content:
     " \n": this is a space-stuffed newline.
     @@ test this special case with mutt
  */


#if DEBUG_PARSE
  printf("RFC3676: Previous quote level: %d\n", *quotelevel);
  printf("RFC3676: Previous line flow flag: %d\n", *continue_prev_flow_flag);
#endif

  /*
  ** hard crlf detection.
  */
  if (rfc3676_ishardlb(line)) {
      /* Hard crlf, reset flags */
      *quotelevel = 0;
      *continue_prev_flow_flag = FALSE;
#if DEBUG_PARSE
      printf("RFC3676: hard CRLF detected. Stopping ff\n");
#endif
      return FALSE;
  }

  /*
  ** quote level detection
  */
  new_quotelevel = get_quotelevel (line);
#if DEBUG_PARSE
  printf("RFC3676: New quote level: %d\n", new_quotelevel);
#endif

  /* change of quote level, stop ff */
  if (new_quotelevel != *quotelevel
      || (new_quotelevel > 0 && set_format_flowed_disable_quoted)) {
      *continue_prev_flow_flag = FALSE;

#if DEBUG_PARSE
      printf("RFC3676: different quote levels detected. Stopping ff\n");
#endif
  }
  tmp_padding = new_quotelevel;

  /*
  ** skip space stuffing if any
  */
  if (line[tmp_padding] == ' ') {
      tmp_padding++;
#if DEBUG_PARSE
      printf("RFC3676: space-stuffing detected; skipping space\n");
#endif
  }

  /*
  ** hard crlf detection after quotes
  */
  if (rfc3676_ishardlb(line+tmp_padding)) {
      /* Hard crlf, reset flags */
      /* *continue_prev_flow_flag = FALSE; */
      *quotelevel = new_quotelevel;
#if DEBUG_PARSE
      printf("RFC3676: hard CRLF detected after quote. Stopping ff\n");
#endif
      return FALSE;
  }

  /*
  ** signature detection
  */

  /* Is it an RFC3676  signature separator? */
  if (is_sig_separator (line + tmp_padding)) {
      /* yes, stop f=f */
      *continue_prev_flow_flag = FALSE;
      sig_sep = TRUE;
#if DEBUG_PARSE
      printf ("RFC3676: -- signature detected. Stopping ff\n", sig_sep);
#endif
      if (delsp_flag) {
          rfc3676_trim_softlb (line);
      }
  }

  /*
  ** is this line f=f?
  */
  if (!sig_sep) {
      char *eold;
      eold = strrchr (line, '\n');
      if (line != eold) {
          if (*(eold - 1) == '\r')
              eold--;
      }
      if (line != eold && (line + tmp_padding) != eold) {
          if (*(eold - 1) == ' ') {
              if (!sig_sep) {
                  flowed = TRUE;
#if DEBUG_PARSE
                  printf("RFC3676: f=f line detected\n");
#endif
              }
              if (delsp_flag) {
                  /* remove the space stuffing and copy the end of line */
                  rfc3676_trim_softlb(line);
              }
          }
      }
  }

  /*
  ** update flags
  */
  *quotelevel = new_quotelevel;

#if DEBUG_PARSE
  if (*continue_prev_flow_flag)
      printf("RFC3676: Continuing previous flow\n");
  if (flowed) {
      printf("RFC3676: Current line is flowed\n");
  }
#endif

  return flowed;
}

/*
** Decode this [virtual] Quoted-Printable line as defined by RFC2045.
** Written by Daniel.Stenberg@haxx.nu
*/

static char * mdecodeQP(FILE *file, char *input, char **result, int *length,
			FILE *fpo)
{
    int outcount = 0;
    char i_buffer[MAXLINE];
    char *buffer;
    unsigned char inchar;
    char *output;
    struct Push pbuf;

    int len = strlen(input);
    output = strsav(input);

    INIT_PUSH(pbuf);

    while ((inchar = *input) != '\0') {

	if (outcount >= len - 1) {
	    /* we need to enlarge the destination area! */
	    /* double the size each time enlargement is needed */
	    char *newp = (char *)realloc(output, len * 2);
	    if (newp) {
		output = newp;
		len *= 2;
	    }
	    else
		break;
	}

	input++;
	if ('=' == inchar) {
	    unsigned int value;
	    if ('\n' == *input) {
		if (!fgets(i_buffer, MAXLINE, file))
		    break;
		buffer = i_buffer + set_ietf_mbox;
		if (set_append) {
		  if(fputs(buffer, fpo) < 0) {
		    progerr("Can't write to \"mbox\""); /* revisit me */
		  }
		}
		input = buffer;
		PushString(&pbuf, buffer);
		continue;
	    }
	    else if ('=' == *input) {
		inchar = '=';
		input++;	/* pass this */
	    }
	    else if (isxdigit(*input)) {
		sscanf(input, "%02X", &value);
		inchar = (unsigned char)value;
		input += 2;	/* pass the two letters */
	    }
	    else
		inchar = '=';
	}
	output[outcount++] = inchar;
    }
    output[outcount] = 0;	/* zero terminate */

    *result = output;
    *length = outcount;
    RETURN_PUSH(pbuf);
}

char *createlink(char *format, char *dir, char *file, int num, char *type)
{
    struct Push buff;
    char buffer[16];

    INIT_PUSH(buff);

    if (!format || !*format)
	/* nothing set, use internal default: */
	format = "%p";

    while (*format) {
	if ('%' == *format) {
	    format++;
	    switch (*format) {
	    default:
		PushByte(&buff, '%');
		PushByte(&buff, *format);
		break;
	    case '%':
		PushByte(&buff, '%');
		break;
	    case 'p':		/* the full path+file */
		PushString(&buff, dir);
		PushByte(&buff, '/');	/* this is for a HTML link and always uses
					   this path separator */
		if (file)
		    PushString(&buff, file);
		else
		    PushString(&buff, "<void>");
		break;
	    case 'f':		/* file name */
		PushString(&buff, file);
		break;
	    case 'd':		/* dir name */
		PushString(&buff, dir);
		break;
	    case 'n':		/* message number */
		sprintf(buffer, "%04d", num);
		PushString(&buff, buffer);
		break;
	    case 'c':		/* content-type (TODO: URL-encode this) */
		PushString(&buff, type);
		break;
	    }
	}
	else {
	    PushByte(&buff, *format);
	}
	format++;
    }

    RETURN_PUSH(buff);
}


void emptydir(char *directory)
{
    struct stat fileinfo;

    char *realdir = directory;

    if (!lstat(realdir, &fileinfo)) {
	if (S_ISDIR(fileinfo.st_mode)) {
	    /* It exists AND it is a dir */
	    DIR *dir = opendir(realdir);
	    char *filename;
	    if (dir) {
#ifdef HAVE_DIRENT_H
		struct dirent *entry;
#else
		struct direct *entry;
#endif
		while ((entry = readdir(dir))) {
		    if (!strcmp(".", entry->d_name) ||
			!strcmp("..", entry->d_name)) continue;
		    trio_asprintf(&filename, "%s%c%s", realdir,
				  PATH_SEPARATOR, entry->d_name);
		    if (set_showprogress)
		        fprintf(stderr, "\nWe delete %s\n", filename);
		    unlink(filename);
		    free(filename);
		}
		closedir(dir);
	    }
	}
    }
}

static int do_uudecode(FILE *fp, char *line, char *line_buf,
		       struct Push *raw_text_buf, FILE *fpo)
{
    struct Push pbuf;
    char *p2;
    INIT_PUSH(pbuf);

    if (uudecode(fp, line, line, NULL, &pbuf))
      /*
       * oh gee, we failed this is chaos
       */
        return 0;
    p2 = PUSH_STRING(pbuf);
    if (p2) {
        if (set_append) {
	    if(fputs(p2, fpo) < 0) {
	        progerr("Can't write to \"mbox\"");
	    }
	}
	if (set_txtsuffix) {
	    PushString(raw_text_buf, line_buf);
	    line_buf[0] = 0; /*avoid dup at next for iter*/
	    PushString(raw_text_buf, p2);
	}
	free(p2);
    }
    return 1;
}

static void write_txt_file(struct emailinfo *emp, struct Push *raw_text_buf)
{
    char *txt_filename;
    char *p = PUSH_STRING(*raw_text_buf);
    char tmp_buf[32];
    sprintf(tmp_buf, "%.4d", emp->msgnum);
    txt_filename = htmlfilename(tmp_buf, emp, set_txtsuffix);
    if ((!emp->is_deleted
	 || ((emp->is_deleted & (FILTERED_DELETE | FILTERED_OLD | FILTERED_NEW
				 | FILTERED_DELETE_OTHER))
	     && set_delete_level > 2)
	 || (emp->is_deleted == FILTERED_EXPIRE && set_delete_level == 2))
	&& (set_overwrite || !isfile(txt_filename))) {
        FILE *fp = fopen(txt_filename, "w");
	if (fp) {
	    fwrite(p, strlen(p), 1, fp);
	    fclose(fp);
	}
    }
    free(p);
    INIT_PUSH(*raw_text_buf);
}

/*
** returns the value for a message_node skip value field
** following some heuristics
*/
static message_node_skip_t message_node_skip_status(FileStatus file_created,
                                                    ContentType content,
                                                    char *content_type)
{
    message_node_skip_t rv;

    if (content == CONTENT_IGNORE) {
        rv = MN_SKIP_ALL;
        /* we want to skip adding a section when root is multipart/foo
           but we'll handle that elsewhere */
        
    }

    else if (!strncasecmp(content_type, "multipart/", 10)
               && content == CONTENT_BINARY && file_created == NO_FILE) {
                rv = MN_SKIP_BUT_KEEP_CHILDREN;
    }
    
    else if (content == CONTENT_BINARY || content == CONTENT_UNKNOWN) {
        rv = MN_SKIP_STORED_ATTACHMENT;
    }
    
    else {
        rv = MN_KEEP;
    }

    return rv;
}

/* 
** singlecontent_get_charset
**
** for single (not multipart/) messages, returns
** the best charset; if none available returns
** set_default_charset
**
** caller must free the returned string
*/
static char *_single_content_get_charset(char *charset, char *charsetsave)
{
    char *rv;
    char *s;
    
    s = choose_charset(charset, charsetsave);
    if (!s || *s == '\0') {
        rv = set_default_charset;
    } else {
        rv = s;
    }

    return strsav(rv);
}

/*
** returns TRUE if line is just a stand-alone
** "--" or "-- "
*/
static bool _is_signature_separator(const char *line)
{
    bool rv;
    int l = strlen(line);
    
    if (!strncmp(line, "--", 2)
        && ((l == 2 &&  line[2] =='\0')
            || (l > 2
                && (line[2] == ' ' || line[2] == '\r' || line[2] == '\n')))) {
        rv = TRUE;
    } else {
        rv= FALSE;
    }

    return rv;
}

/*
** Some old versions of thunderbird, pine, and other UA
** URL-escaped the <> in the In-Reply-To and first
** Reference header values.
** This functions normalizes them by unescaping those
** characters.
**
** If any unescaping takes place, returns a new string
** that the caller must free.
**
** If none unescaping happened, returns NULL.
**
*/
static char * _unescape_reply_and_reference_values(char *line)
{
    char *ptr_lower_than;
    char *ptr_greater_than;
    char *c;
    struct Push buff;
    
    if (!line || !*line || *line == '\n' || *line == '\r') {
        return NULL;
    }

    ptr_lower_than = strstr(line, " %3C");
    ptr_greater_than = strstr(line, "%3E");

    /* we only do the replacement if we found both <> */
    if (!ptr_lower_than || !ptr_greater_than) {
        return NULL;
    }

    /* verify that we have a contiguous string between both
     * characters */
    for (c = ptr_lower_than + sizeof(char) * 1; c < ptr_greater_than; c++) {
        if (isspace(*c) || *c == '\r' || *c == '\n')
            return NULL;
    }

    /* verify what the char immediately after the ptr_greater_than to make
       sure it's a separator or EOL */
    c = ptr_greater_than + sizeof(char) * 3;
    if (!isspace(*c) && *c != '\n' && *c != '\r') {
        return NULL;
    }

    INIT_PUSH(buff);

    PushNString(&buff, line, ptr_lower_than - line + 1);
    PushByte(&buff, '<');
    PushNString(&buff, ptr_lower_than + sizeof(char) * 4, ptr_greater_than - ptr_lower_than - sizeof(char) *4); 
    PushString(&buff, ">");
    PushString(&buff, ptr_greater_than + sizeof(char) * 3);

    RETURN_PUSH(buff);
}

/*
**  parses a filename in either a Content-Disposition or Content-Description
**  line.
**
**  np must be pointing at the first character after the attribute and equal
**  sign, i.e., filename=  or name=, respectively.
**  attachname is a preallocated string of size attachname_size
**  the function copies the filename, if found, to attachname and calls
**  safe_filename to make sure it's a valid O.S. name.
*/
static void _extract_attachname(char *np, char *attachname, size_t attachname_size)
{
    char *jp;

    /* some UA may have done line folding between filename= and the "foo" attribute value;
       if this is the case, we skip all spaces until we find the first non-space char */
    jp = np;
    while (*jp && isspace(*jp)) {
        jp++;
    }

    /* if we find a non space character, update np to the new position;
       otherwise we ignore jp and just use np as it was as
       we'll handle the only spaces case further down */
    if (*jp && *jp != '\n' && *jp != '\r' && *jp != ';') {
        np = jp;
    }

    /* skip the first quote */
    if (*np == '"')
        np++;
                                         
    for (jp = attachname; np && *np != '\n' && *np != '\r'
             && *np != '"' && *np != ';'
             && jp < attachname + attachname_size - 1;) {
        *jp++ = *np++;
    }
    *jp = '\0';
    safe_filename(attachname);
}

/*
** if the attachname that is given is empty, searchs the Content-Type:
** header value for a name attribute and, if found, copies it to
** attachname; If in this case, the Content-Type: header value doesn't
** have a name attribute, it clears the attachname.
*/
static void _control_attachname(char *content_type, char *attachname, size_t attachname_size)
{
    /* only use the Content-Type name attribute to get 
       the filename if Content-Disposition didn't 
       provide a filename */
    char *fname;
    
    if (*attachname == '\0') {
        fname = strcasestr(content_type, "name=");
        if (fname) {
            fname += 5;
            _extract_attachname(fname, attachname, attachname_size);
#ifdef FACTORIZE_ATTACHNAME                                
            if ('\"' == *fname)
                fname++;
            sscanf(fname, "%128[^\"]", attachname);
            safe_filename(attachname);
#endif /* FACTORIZE_ATTACHNAME */            
        }
        else {
            attachname[0] = '\0';	/* just clear it */
        }
    }
}

/* validates that a header name is RFC282 compliant
   returns TRUE if valid, FALSE otherwise
*/
static bool _validate_header(const char *header_line)
{
    char header_name[129];
    const char *ptr;
    
    /* control that we have a header_name: header_value */
    if (!header_line
        || *header_line=='\0'
        || !(ptr = strstr(header_line, ":"))
        || ptr == header_line
        || *(ptr + 1) == '\0'
        || (*(ptr + 1) != ' ' && *(ptr + 1) != '\t')) {

        return FALSE;
    }
    
    /* control length of header-name and its requirement
       to be only valid printable US-ASCII */
    
    if (!sscanf(header_line, "%127[^:]", header_name)
        /* line doesn't start with : */
        || header_line[strlen(header_name)] != ':'
        /* header name is us_ascii */
        || !i18n_is_valid_us_ascii(header_name)) {
        
        return FALSE;
    }

    /* control that we have a value that is not spaces */
    ptr = header_line + strlen(header_name) + 1;
    while (*ptr) {
        if (*ptr != ' ' && *ptr != '\t' && *ptr != '\r' && *ptr != '\n') {
            return TRUE;
        }
        ptr++;
    }
        
    return FALSE;
}
    
/*
** Parsing...the heart of Hypermail!
** This loads in the articles from stdin or a mailbox, adding the right
** field variables to the right structures. If readone is set, it will
** think anything it reads in is one article only. Increment should be set
** if this updates an archive.
*/

int parsemail(char *mbox,	/* file name */
	      int use_stdin,	/* read from stdin */
	      int readone,	/* only one mail */
	      int increment,	/* update an existing archive */
	      char *dir, int inlinehtml,	/* if HTML should be inlined */
	      int startnum)
{
    FILE *fp;
    struct Push raw_text_buf;
    FILE *fpo = NULL;
    char *date = NULL;
    char *subject = NULL;
    char *msgid = NULL;
    char *inreply = NULL;
    char *namep = NULL;
    char *emailp = NULL;
    char  message_headers_parsed = FALSE; /* we use this flag to avoid
                                             having message/rfc822
                                             headers clobber the
                                             encapsulating message
                                             headers */
    char *line = NULL;
    char line_buf[MAXLINE], fromdate[DATESTRLEN] = "";
    char *cp;
    char *dp = NULL;
    int num, isinheader, hassubject, hasdate;
    int num_added = 0;
    long exp_time = -1;
    time_t delete_older_than = (set_delete_older ? convtoyearsecs(set_delete_older) : 0);
    time_t delete_newer_than = (set_delete_newer ? convtoyearsecs(set_delete_newer) : 0);
    annotation_robot_t annotation_robot = ANNOTATION_ROBOT_NONE;
    annotation_content_t annotation_content = ANNOTATION_CONTENT_NONE;
    int is_deleted = 0;
    int pos;
    bool *require_filter, *require_filter_full;
    int require_filter_len, require_filter_full_len;
    struct hmlist *tlist;
    char filename[MAXFILELEN];
    char directory[MAXFILELEN];
    char pathname[MAXFILELEN];
    struct emailinfo *emp;
    char *att_dir = NULL;	/* directory name to store attachments in */
    char *meta_dir = NULL;	/* directory name where we're storing the meta data
				   that describes the attachments */
    /* -- variables for the multipart/alternative parser -- */
    struct body *origbp = NULL;	/* store the original bp */
    struct body *origlp = NULL;	/* ... and the original lp */
    char alternativeparser = FALSE;	/* set when inside alternative parser mode */
    int alternative_weight = -1;	/* the current weight of the prefered alternative content */
    char *prefered_charset = NULL;  /* the charset for a message as chosen by heuristics */
    struct body *alternative_lp = NULL;	/* the previous alternative lp */
    struct body *alternative_bp = NULL;	/* the previous alternative bp */
    struct body *append_bp = NULL; /* text to append to body after parse done*/
    struct body *append_lp = NULL;

    FileStatus alternative_lastfile_created = NO_FILE;	/* previous alternative attachments, for non-inline MIME types */
    char alternative_file[131];	/* file name where we store the non-inline alternatives */
    char alternative_lastfile[131];	/* last file name where we store the non-inline alternatives */
    char last_alternative_type[131];      /* the alternative Content-Type value */
    int att_counter = 0;	/* used to generate a unique name for attachments */

    int parse_multipart_alternative_force_save_alts = 0; /* used to control if we are parsing alternative as multipart */
    
    /* used to store the set_save_alts when overriding it for apple mail */
    int applemail_old_set_save_alts = -1;
    /* code optimization to avoid computing it each time */
    int applemail_ua_header_len = (set_applemail_mimehack) ? strlen (set_applemail_ua_header) : 0;
    /* we make a local copy of this config variable because the apple mail
       hack will alter it and we may need to fall back to the original value
       while processing a complex multipart/ message/rfc822 message */
    int local_set_save_alts = set_save_alts;
    
    /*
    ** keeps track of attachment file name used so far for this message
    */
    struct hmlist *att_name_list = NULL;
    struct hmlist *att_name_last = NULL;

    /* -- end of alternative parser variables -- */

    struct body *bp;
    struct body *lp = NULL;	/* the last pointer, points to the last node in the
				   body list. Initially set to NULL since we have
				   none at the moment. */

    struct body *headp = NULL;	/* stored pointer to the point where we last
				   scanned the headers of this mail. */

    char Mime_B = FALSE;
    char boundbuffer[256] = "";

    /* This variable is used to store a stack of boundary separators
       when having multipart body parts embeeded inside other
       multipart body parts */
    struct boundary_stack *boundp = NULL; 

    /* This variable is used to store a stack of mime types when
       dealing with multipart mails */
    struct hm_stack *multipartp = NULL; 

    struct message_node *root_message_node = NULL;  /* points to the first node of a message */
    struct message_node *current_message_node = NULL;
    struct message_node *root_alt_message_node = NULL; /* for temporarily storing alternatives */
    struct message_node *current_alt_message_node = NULL;
    char alternative_message_node_created = FALSE; /* true if we have created a node used to
                                                      store multipart/alternative while selecting
                                                      the prefered one */

    bool skip_mime_epilogue = FALSE;  /* This variable is used to help skip multipart/foo
                                           epilogues */

    char multilinenoend = FALSE;	/* This variable is set TRUE if we have read
					   a partial line off a multiline-encoded line,
					   and the next line we read is supposed to get
					   appended to the previous one */

    int bodyflags = 0;		/* This variable is set to extra flags that the
				   addbody() calls should OR in the flag parameter */

    /* RFC 3676 related variables, set while parsing the headers and body content */
    textplain_format_t textplain_format = FORMAT_FIXED;
    bool flowed_line = FALSE;
    int quotelevel = 0;
    bool continue_previous_flow_flag = FALSE;
    bool delsp_flag = FALSE;

    int binfile = -1;

    char *charset = NULL;   /* this is the charset declared in the Content-Type header */
    char *charsetsave;      /* charset in MIME encoded text */

    char *boundary_id = NULL;
    char type[129];		/* for Content-Type type */
    char *content_type_ptr;     /* pointing to the Content-Type parsed line */
    bool attachment_rfc822; /* set to TRUE if the current attachment type is
                               message/rfc822 */

    char charbuffer[129];	/* for Content-Type charset */
    FileStatus file_created = NO_FILE;	/* for attachments */

    char attachname[129];	/* for attachment file names */
    char *att_binname = NULL;   /* full path + filename pointing to a stored attachment */
    char *meta_filename = NULL; /* full path + filename to metadata associated with
                                   a stored attachment */
    char *att_link = NULL;      /* for a stored attachment HTML link */
    char *att_comment_filename = NULL; /* for the HTML comment that is inserted after att_link */
    char inline_force = FALSE;	/* show a attachment in-line, regardles of
				   the content_disposition */
    char *description = NULL;	/* user-supplied description for an attachment */
    char attach_force;
    struct base64_decoder_state *b64_decoder_state = NULL; /* multi-line base64 decoding */
    
    EncodeType decode = ENCODE_NORMAL;
    ContentType content = CONTENT_TEXT;

    charsetsave=malloc(256);
    memset(charsetsave,0,255);
    *directory = 0;
    *filename = 0;
    *pathname = 0;
    *attachname = '\0';
    
    if (use_stdin || !mbox || !strcasecmp(mbox, "NONE"))
	fp = stdin;
    else if ((fp = fopen(mbox, "rb")) == NULL) {
        trio_snprintf(errmsg, sizeof(errmsg), "%s \"%s\".",
                 lang[MSG_CANNOT_OPEN_MAIL_ARCHIVE], mbox);
	progerr(errmsg);
    }
    if(set_append) {

	/* add to an mbox as we read */
	if (set_append_filename) {
            time_t curtime;
            const struct tm *local_curtime;

	    time(&curtime);
            local_curtime = localtime(&curtime);

	    if(strncmp(set_append_filename, "$DIR/", 5) == 0) {
	        strncpy(directory, dir, MAXFILELEN - 1);
                strftime(filename, MAXFILELEN - 1, set_append_filename+5,
                         local_curtime);
            } else {
                strftime(filename, MAXFILELEN - 1, set_append_filename,
                         local_curtime);
	    }
	} else {
	    strncpy(directory, dir, MAXFILELEN - 1);
	    strncpy(filename, "mbox", MAXFILELEN - 1);
	}

	if(trio_snprintf(pathname, sizeof(pathname), "%s%s", directory,
			filename) == sizeof(pathname)) {
	    progerr("Can't build mbox filename");
	}
	if(!(fpo = fopen(pathname, "a"))) {
	    trio_snprintf(errmsg, sizeof(errmsg), "%s \"%s\".",
			  lang[MSG_CANNOT_OPEN_MAIL_ARCHIVE], pathname);
	    progerr(errmsg);
	}
        *directory = 0;
	*filename = 0;
	*pathname = 0;
    }

    num = startnum;

    INIT_PUSH(raw_text_buf);

    hassubject = 0;
    hasdate = 0;
    isinheader = 1;
    inreply = NULL;
    msgid = NULL;
    bp = NULL;
    subject = NOSUBJECT;
    message_headers_parsed = FALSE;

    parse_multipart_alternative_force_save_alts = 0;
    attachment_rfc822 = FALSE;
    applemail_old_set_save_alts = -1;
    local_set_save_alts = set_save_alts;
    
    require_filter_len = require_filter_full_len = 0;
    for (tlist = set_filter_require; tlist != NULL; require_filter_len++, tlist = tlist->next)
	;
    for (tlist = set_filter_require_full_body; tlist != NULL;
	 require_filter_full_len++, tlist = tlist->next)
	;
    pos = require_filter_len + require_filter_full_len;
    require_filter = (pos ? (bool *)emalloc(pos * sizeof(*require_filter)) : NULL);
    require_filter_full = require_filter + require_filter_len;
    for (pos = 0; pos < require_filter_len; ++pos)
	require_filter[pos] = FALSE;
    for (pos = 0; pos < require_filter_full_len; ++pos)
	require_filter_full[pos] = FALSE;

    if (!increment) {
	replylist = NULL;
	subjectlist = NULL;
	authorlist = NULL;
	datelist = NULL;
    }

    /* now what has this to do if readone is set or not? (Daniel) */
    if (set_showprogress) {
	if (readone)
	    printf("%s\n", lang[MSG_READING_NEW_HEADER]);
	else {
	    if ((mbox && !strcasecmp(mbox, "NONE")) || use_stdin)
		printf("%s...\n", lang[MSG_LOADING_MAILBOX]);
	    else
		printf("%s \"%s\"...\n", lang[MSG_LOADING_MAILBOX], mbox);
	}
    }

    for ( ; fgets(line_buf, MAXLINE, fp) != NULL;
	  set_txtsuffix ? PushString(&raw_text_buf, line_buf) : 0) {
#if DEBUG_PARSE
        fprintf(stderr,"\n^IN: %s", line_buf);
        fprintf(stderr, "^  BP %.0s: %.40s|\n^  LP %.0s: %.40s|\n^ ABP %.0s: %.40s|\n^ ALP %.0s: %.40s|\n^ OBP %.0s: %.40s|\n^ "
                "OLP %.0s: %.40s|\n^HEAD %.0s: %.40s|\n",
                "bp", (bp) ? bp->line : "",
                "lp", (lp) ? lp->line : "",
                "alternative_bp", (alternative_bp) ? alternative_bp->line : "",
                "alternative_lp", (alternative_lp) ? alternative_lp->line : "",
                "origbp", (origbp) ? origbp->line : "",
                "origlp", (origlp) ? origlp->line : "",
                "headp", (headp) ? headp->line : "");
#endif
	if(set_append) {
	    if(fputs(line_buf, fpo) < 0) {
	        progerr("Can't write to \"mbox\""); /* revisit me */
	    }
	}
	line = line_buf + set_ietf_mbox;

        /* skip the mime epilogue until we find a known boundary or
           a new message */
        if (skip_mime_epilogue) {
            if ((strncmp(line, "--", 2)
                 || _is_signature_separator(line)
                 || !boundary_stack_has_id(boundp, line))
                && strncasecmp(line_buf, "From ", 5)) {
                continue;
            } else {
                skip_mime_epilogue = FALSE;
            }
        }

	if (!is_deleted &&
	    inlist_regex_pos(set_filter_out_full_body, line) != -1) {
	    is_deleted = FILTERED_OUT;
	}
	pos = inlist_regex_pos(set_filter_require_full_body, line);
	if (pos != -1 && pos < require_filter_full_len) {
	    require_filter_full[pos] = TRUE;
	}
	if (isinheader) {
	    if (!strncasecmp(line_buf, "From ", 5))
		strcpymax(fromdate, dp = getfromdate(line), DATESTRLEN);
	    /* check for MIME */
	    else if (!strncasecmp(line, "MIME-Version:", 13))
		Mime_B = TRUE;
            else if (!strncasecmp(line, "Content-Type:", 13)) {
                /* we don't do anything here except switch off anti-spam
                   to avoid having boundaries with @ chars being changed 
                   by the antispam functions */
                bp = addbody(bp, &lp, line, BODY_HEADER | BODY_NO_ANTISPAM | bodyflags);
            }
	    else if (isspace(line[0]) && ('\n' != line[0]) \
		     && !('\r' == line[0] && '\n' == line[1])) {
		/*
		 * since this begins with a whitespace, it means the
		 * previous line is continued on this line, leave only
		 * one space character and go!
		 */
		char *ptr = line;
		while (isspace(*ptr))
		    ptr++;
		ptr--;		/* leave one space */
		*ptr = ' ';	/* make it a true space, no tabs here! */
		bp =
		    addbody(bp, &lp, ptr,
			    BODY_CONTINUE | BODY_HEADER | bodyflags);
	    }

	    else if (line[0] == '\n' || (line[0] == '\r' && line[1] == '\n')) {
		struct body *head;

		char savealternative;

		/*
		 * we mark this as a header-line, and we use it to
		 * track end-of-header displays
		 */

		/* skip the alternate "\n", otherwise, we'll have
		   an extra "\n" in the HTMLized message */
		if (!alternativeparser)
		    bp = addbody(bp, &lp, line, BODY_HEADER | bodyflags);
		isinheader--;

		/*
		 * This signals us that we are no longer in the header,
		 * let's fill in all those fields we are interested in.
		 * Parse the headers up to now and copy to the target
		 * variables
		 */

                /* the first header we'll extract is Content-Type
                   so that we can get the charset and use it if we
                   get messages that are not using UTF-8 but encoding
                   their header values with 8-bit */
                
                /* testing separating parsing from post-processing */
                /* extract content-type and other values from the headers */
                content_type_ptr = NULL;
                
                /* @@ we were using headp and here it is bp... test with attachments */
                   
                for (head = bp; head; head = head->next) {
                    if (head->parsedheader || !head->header || head->invalid_header)
                        continue;
                    
                    if (!strncasecmp(head->line, "Content-Type:", 13)) {
			char *ptr = head->line + 13;
#define DISP_HREF 1
#define DISP_IMG  2
#define DISP_IGNORE 3
			/* we must make sure this is not parsed more times
			   than this */
			head->parsedheader = TRUE;

			while (isspace(*ptr))
			    ptr++;

                        /* @@ if ptr is bogus, initialize it to default text/plain? */
                        /* some bogus mail messages have empty Content-Type headers */
                        if (*ptr) {
                            content_type_ptr = ptr;
                            sscanf(ptr, "%128[^;]", type);
                            filter_content_type_values(type);
                        }
                          
			/* now, check if there's a charset indicator here too! */
			cp = strcasestr(ptr, "charset=");
			if (cp) {
			    cp += 8;	/* pass charset= */
			    if ('\"' == *cp)
				cp++;	/* pass a quote too if one is there */
                            
			    sscanf(cp, "%128[^;\"\n\r]", charbuffer);
                            /* @@ we need a better filter here, to remove all non US-ASCII */
                            filter_content_type_values(charbuffer);
                            /* some old messages use DEFAULT_CHARSET or foo_CHARSET,
                               we strip it out */
                            filter_charset_value(charbuffer);
			    /* save the charset info */
                            if (charbuffer[0] != '\0') {
                                charset = strsav(charbuffer);
                            }
                        }

			/* now check if there's a format indicator */
			if (set_format_flowed) {
                            cp = strcasestr(ptr, "format=");
                            if (cp) {
                                cp += 7;	/* pass charset= */
                                if ('\"' == *cp)
                                    cp++;	/* pass a quote too if one is there */
                                
                                sscanf(cp, "%128[^;\"\n\r]", charbuffer);
                                /* save the format info */
                                if (!strcasecmp (charbuffer, "flowed"))
                                    textplain_format = FORMAT_FLOWED;
                            }

                            /* now check if there's a delsp indicator */
                            cp = strcasestr(ptr, "delsp=");
                            if (cp) {
                                cp += 6;	/* pass charset= */
                                if ('\"' == *cp)
                                    cp++;	/* pass a quote too if one is there */
                                
                                sscanf(cp, "%128[^;\"\n\r]", charbuffer);
                                /* save the delsp info */
                                if (!strcasecmp (charbuffer, "yes"))
                                    delsp_flag = TRUE;
                            }
			}
                        break;
                    }
                    
                } /* for content-type */

                /* post-processing Content-Type:
                   check if we have the a Content=Type, a boundary parameter,
                   and a corresponding start bondary
                   revert to a default type otherwise.
                */
                if (content_type_ptr == NULL) {
                    /* missing Content-Type header, use default text/plain unless
                       immediate parent is multipart/digest; in that case, use 
                       message/rfc822 (RFC 2046) */
                    if (multipart_stack_top_has_type(multipartp, "multipart/digest")
                        && !attachment_rfc822) {
                        strcpy(type, "message/rfc822");
                    } else {
                        strcpy(type, "text/plain");
                    }
                    content_type_ptr = type;
#if DEBUG_PARSE
                    printf("Missing Content-Type header, defaulting to %s\n", type);
#endif
                } else if (!strncasecmp(type, "multipart/", 10)) {
                    boundary_id = strcasestr(content_type_ptr, "boundary=");
#if DEBUG_PARSE
                    printf("boundary found in %s\n", content_type_ptr);
#endif
                    if (boundary_id) {
                        boundary_id = strchr(boundary_id, '=');
                        if (boundary_id) {
                            boundary_id++;
                            while (isspace(*boundary_id))
                                boundary_id++;
                            *boundbuffer ='\0';
                            if ('\"' == *boundary_id) {
                                sscanf(++boundary_id, "%255[^\"]",
                                       boundbuffer);
                            }
                            else
                                sscanf(boundary_id, "%255[^;\n]",
                                       boundbuffer);
                            boundary_id = (*boundbuffer) ? boundbuffer : NULL;
                        }
                    }

                    /* if we have multipart/ but there's no missing
                       boundary attribute, downgrade the content type to
                       text/plain */
                    if (!boundary_id) {
                        strcpy(type, "text/plain");
                        content_type_ptr = type;
#if DEBUG_PARSE
                        printf("Missing boundary attribute in multipart/*, downgrading to text/plain\n");
#endif                        
                    }
                }

                /* have we reached the too many attachments per message limit?
                   this protects against the message_node tree growing
                   uncontrollably */
                if ((set_max_attach_per_msg != 0)
                    && (att_counter > set_max_attach_per_msg)) {
                    content = CONTENT_IGNORE;
#if DEBUG_PARSE
                    printf("Hit max_attach_per_msg limit; ignoring further attachments for msgid %s\n", msgid);
#endif                        
                }
                
                if (content == CONTENT_IGNORE) {
                    continue;
                } else if (ignorecontent(type)) {
                    /* don't save this */
                    content = CONTENT_IGNORE;
                    continue;
                }
#if 0
                /* not sure if we should add charset save here or wait until later */
                if (charset[0] == NULL) {
                    strcpy(charset, set_default_charset);
                }
#endif
                
                /* parsing of all headers except for Content-* related ones */
		for (head = bp; head; head = head->next) {
		    char head_name[129];

                    /* if we have a single \n, we just mark it as head->demimed
                       and skip the rest of the checks, which would give the
                       same result */
                    if (head->line && rfc3676_ishardlb(head->line)) {
                        head->demimed = TRUE;
                        continue;
                    }
                    
		    if (head->header && !head->demimed) {
                        
                        /* control that we have a valid header line */
                        if ( !_validate_header(head->line) ) {
                            /* not a valid header line, we mark it as so to ignore it
                               later on */
                            head->invalid_header = TRUE;
                            head->parsedheader = TRUE;
                            /* the following line is probably overkill and can be skipped */
                            head->demimed = TRUE;
                            continue;
                        }
                        
                        head->line =
                            mdecodeRFC2047(head->line, strlen(head->line), charsetsave);
                        head->demimed = TRUE;
		    }

		    if (head->parsedheader
#ifdef DELETE_ME
                        || head->attached
#endif
			|| !head->header) {
			continue;
		    }

                    /* we probably would be ok just with the sscanf as we
                       validated the header line some lines above */
                    if (!sscanf(head->line, "%127[^:]", head_name)) {
                        head->invalid_header = TRUE;
                        head->parsedheader = TRUE;
                        continue;
                    }
                    
		    if (inlist(set_deleted, head_name)) {
                        if (!message_headers_parsed) {
                            char *val = getsubject(head->line); /* revisit me */
                            if (!strcasecmp(val, "yes"))
                                is_deleted = FILTERED_DELETE;
                            free(val);
                        }
                        head->parsedheader = TRUE;
		    }

		    if (inlist(set_expires, head_name)) {
                        if (!message_headers_parsed) {
                            char *val = getmaildate(head->line);
                            exp_time = convtoyearsecs(val);
                            if (exp_time != -1 && exp_time < time(NULL))
                                is_deleted = FILTERED_EXPIRE;
                            free(val);
                        }
                        head->parsedheader = TRUE;
		    }

		    if (inlist(set_annotated, head_name)) {
                        if (!message_headers_parsed) {                        
                            getannotation(head->line, &annotation_content,
                                          &annotation_robot);
                            if (annotation_content == ANNOTATION_CONTENT_DELETED_OTHER)
                                is_deleted = FILTERED_DELETE_OTHER;
                            else if (annotation_content == ANNOTATION_CONTENT_DELETED_SPAM)
                                is_deleted = FILTERED_DELETE;
                        }
		      head->parsedheader = TRUE;
		    }

                    
		    if (!message_headers_parsed) {
                        if (!is_deleted
                            && inlist_regex_pos(set_filter_out, head->line) != -1) {
                            is_deleted = FILTERED_OUT;
                        }

                        pos = inlist_regex_pos(set_filter_require, head->line);
                        if (pos != -1 && pos < require_filter_len) {
                            require_filter[pos] = TRUE;
                        }
                    }
                    
                    if (!strncasecmp(head->line, "Received:", 8)) {
                        /* we are not doing anything with these
                           headers and there can be many of them, let's
                           mark them as parsed to speed up the processing
                           further below */
                        head->parsedheader = TRUE;
                        continue;
                    }              
		    else if (!strncasecmp(head->line, "Date:", 5)) {
                        strlftonl(head->line);
                        head->parsedheader = TRUE;
                        if (!message_headers_parsed) {
                            if (hasdate) {
                                /* msg has two or more of this header,
                                   ignore them */
                                continue;
                            }
                            date = getmaildate(head->line);
                            hasdate = 1;
                        }
		    }
		    else if (!strncasecmp(head->line, "From:", 5)) {
                        head->parsedheader = TRUE;
                        strlftonl(head->line);                        
                        if (!message_headers_parsed) {
                            if (namep || emailp) {
                                /* msg has two or more of this header,
                                   ignore them */
                                continue;
                            }
                            head->line = header_detect_charset_and_convert_to_utf8 (head->line,
                                                                                    charset,
                                                                                    charsetsave);
                            
                            getname(head->line, &namep, &emailp);
                            if (set_spamprotect) {
                                char *tmp;
                                tmp = emailp;
                                emailp = spamify(tmp);
                                free(tmp);
                                /* we need to "fix" the name as well, as sometimes
                                   the email ends up in the name part */
                                tmp = namep;
                                namep = spamify(tmp);
                                free(tmp);
                            }
                        }
		    }
                    else if (!strncasecmp(head->line, "To:", 3)) {
                        /* we don't do anything specific with this header,
                           we just want to mark it as parsed to avoid
                           processing it over and over here below
                        */
                        head->parsedheader = TRUE;
                        head->line = header_detect_charset_and_convert_to_utf8 (head->line,
                                                                                charset,
                                                                                charsetsave);
                        strlftonl(head->line);
                    }
		    else if (!strncasecmp(head->line, "Message-Id:", 11)) {
                        head->parsedheader = TRUE;
                        strlftonl(head->line);
                        if (!message_headers_parsed) {
                            if (msgid) {
                                /* msg has two or more of this header,
                                   ignore them */
                                continue;
                            }
                            msgid = getid(head->line);
                        }
		    }
		    else if (!strncasecmp(head->line, "Subject:", 8)) {
                        head->parsedheader = TRUE;
                        head->line = header_detect_charset_and_convert_to_utf8 (head->line,
                                                                                charset,
                                                                                charsetsave);
                        strlftonl(head->line);
                        if (!message_headers_parsed) {
                            if (hassubject) {
                                /* msg has two or more of this header,
                                   ignore them */
                                continue;
                            }
                            subject = getsubject(head->line);
                            hassubject = 1;
                        }
		    }
		    else if (!strncasecmp(head->line, "In-Reply-To:", 12)) {
                        char *unescaped_reply_to;
                        head->parsedheader = TRUE;
                        strlftonl(head->line);
                        unescaped_reply_to = 
                            _unescape_reply_and_reference_values(head->line);
                        if (unescaped_reply_to) {
                            free(head->line);
                            head->line = unescaped_reply_to;
                        }
                        if (!message_headers_parsed) {
                            if (inreply) {
                                /* we already parsed a References: header before, but
                                   we're going to give priority to In-Reply-To */
                                free(inreply);
                            }
                            inreply = getreply(head->line);
                        }
		    }
		    else if (!strncasecmp(head->line, "References:", 11)) {
                        head->parsedheader = TRUE;
                        if (!message_headers_parsed) {
                            char *unescaped_references;
                            
                            unescaped_references =
                                _unescape_reply_and_reference_values(head->line);
                            if (unescaped_references) {
                                free(head->line);
                                head->line = unescaped_references;
                            }
                            
                            /*
                             * Adding threading capability for the "References"
                             * header, ala RFC 822, used only for messages that
                             * have "References" but do not have an "In-reply-to"
                             * field. This is partically a concession for Netscape's
                             * email composer, which erroneously uses "References"
                             * when it should use "In-reply-to".
                             */
                            if (!inreply) {
                                inreply = getid(head->line);
                            }
                            if (set_linkquotes) {
                                bp = addbody(bp, &lp, line, 0);
                            }
                        }
		    }
		    else if (applemail_ua_header_len > 0
                             && !strncasecmp(head_name, set_applemail_ua_header,
                                             applemail_ua_header_len)) {
                        /* we only need to set this one up once per message*/
                        head->parsedheader = TRUE;
                        if (alternativeparser
                            || !Mime_B
                            || local_set_save_alts
                            || !set_applemail_mimehack) {
                            continue;
                        }

                        /* If the UA is an apple mail client and we're configured to do the
                         * applemail hack and we're not already configured to
                         * save the alternatives, memorize the old setting and force
                         * the alternatives save
                         */
                        if (!parse_multipart_alternative_force_save_alts
                            && is_applemail_ua(head->line + applemail_ua_header_len + 2)) {

                            parse_multipart_alternative_force_save_alts = 1;

			    /* to avoid confusion and quoting out of
                            ** context, we won't show the alternatives
                            ** in-line.
                            */

                            applemail_old_set_save_alts = local_set_save_alts;
			    local_set_save_alts = 2;

#if DEBUG_PARSE
                            printf("Applemail_hack force save_alts: yes\n");
			    printf("Applemail_hack set_save_alts changed from %d to %d\n",
                                   applemail_old_set_save_alts, local_set_save_alts);
#endif
                        }
                    }
                }

                /* avoid overwriting the message headers by those coming from
                   message/rfc attachments */
                if (!message_headers_parsed) {
                    message_headers_parsed = TRUE;
                }

		if (!is_deleted && set_delete_older && (date || *fromdate)) {
		    time_t email_time = convtoyearsecs(date);
		    if (email_time == -1)
		        email_time = convtoyearsecs(fromdate);
		    if (email_time != -1 && email_time < delete_older_than)
		        is_deleted = FILTERED_OLD;
		}
		if (!is_deleted && set_delete_newer && (date || *fromdate)) {
		    time_t email_time = convtoyearsecs(date);
		    if (email_time == -1)
		        email_time = convtoyearsecs(fromdate);
		    if (email_time != -1 && email_time > delete_newer_than)
		        is_deleted = FILTERED_NEW;
		}

		if (!headp)
		    headp = bp;

		savealternative = FALSE;
		attach_force = FALSE;

#if NEW_PARSER
#if 0
                /* testing separating parsing from post-processing */
                /* extract content-type and other values from the headers */
                content_type_ptr = NULL;
                for (head = headp; head; head = head->next) {
                    if (head->parsedheader || !head->header || head->invalid_header)
                        continue;
                    
                    if (!strncasecmp(head->line, "Content-Type:", 13)) {
			char *ptr = head->line + 13;
#define DISP_HREF 1
#define DISP_IMG  2
#define DISP_IGNORE 3
			/* we must make sure this is not parsed more times
			   than this */
			head->parsedheader = TRUE;

			while (isspace(*ptr))
			    ptr++;

                        content_type_ptr = ptr;
			sscanf(ptr, "%128[^;]", type);

                        filter_content_type_values(type);
                          
			/* now, check if there's a charset indicator here too! */
			cp = strcasestr(ptr, "charset=");
			if (cp) {
			    cp += 8;	/* pass charset= */
			    if ('\"' == *cp)
				cp++;	/* pass a quote too if one is there */
                            
			    sscanf(cp, "%128[^;\"\n\r]", charbuffer);
                            /* @@ we need a better filter here, to remove all non US-ASCII */
                            filter_content_type_values(charbuffer);
                            /* some old messages use DEFAULT_CHARSET or foo_CHARSET,
                               we strip it out */
                            filter_charset_value(charbuffer);
			    /* save the charset info */
                            if (charbuffer[0] != '\0') {
                                charset = strsav(charbuffer);
                            }
                        }

			/* now check if there's a format indicator */
			if (set_format_flowed) {
                            cp = strcasestr(ptr, "format=");
                            if (cp) {
                                cp += 7;	/* pass charset= */
                                if ('\"' == *cp)
                                    cp++;	/* pass a quote too if one is there */
                                
                                sscanf(cp, "%128[^;\"\n\r]", charbuffer);
                                /* save the format info */
                                if (!strcasecmp (charbuffer, "flowed"))
                                    textplain_format = FORMAT_FLOWED;
                            }

                            /* now check if there's a delsp indicator */
                            cp = strcasestr(ptr, "delsp=");
                            if (cp) {
                                cp += 6;	/* pass charset= */
                                if ('\"' == *cp)
                                    cp++;	/* pass a quote too if one is there */
                                
                                sscanf(cp, "%128[^;\"\n\r]", charbuffer);
                                /* save the delsp info */
                                if (!strcasecmp (charbuffer, "yes"))
                                    delsp_flag = TRUE;
                            }
			}
                        break;
                    }
                    
                } /* for content-type */

                /* post-processing Content-Type:
                   check if we have the a Content=Type, a boundary parameter,
                   and a corresponding start bondary
                   revert to a default type otherwise.
                */
                if (content_type_ptr == NULL) {
                    /* missing Content-Type header, use default text/plain unless
                       immediate parent is multipart/digest; in that case, use 
                       message/rfc822 (RFC 2046) */
                    if (multipart_stack_top_has_type(multipartp, "multipart/digest")
                        && !attachment_rfc822) {
                        strcpy(type, "message/rfc822");
                    } else {
                        strcpy(type, "text/plain");
                    }
                    content_type_ptr = type;
#if DEBUG_PARSE
                    printf("Missing Content-Type header, defaulting to %s\n", type);
#endif
                } else if (!strncasecmp(type, "multipart/", 10)) {
                    boundary_id = strcasestr(content_type_ptr, "boundary=");
#if DEBUG_PARSE
                    printf("boundary found in %s\n", content_type_ptr);
#endif
                    if (boundary_id) {
                        boundary_id = strchr(boundary_id, '=');
                        if (boundary_id) {
                            boundary_id++;
                            while (isspace(*boundary_id))
                                boundary_id++;
                            *boundbuffer ='\0';
                            if ('\"' == *boundary_id) {
                                sscanf(++boundary_id, "%255[^\"]",
                                       boundbuffer);
                            }
                            else
                                sscanf(boundary_id, "%255[^;\n]",
                                       boundbuffer);
                            boundary_id = (*boundbuffer) ? boundbuffer : NULL;
                        }
                    }

                    /* if we have multipart/ but there's no missing
                       boundary attribute, downgrade the content type to
                       text/plain */
                    if (!boundary_id) {
                        strcpy(type, "text/plain");
                        content_type_ptr = type;
#if DEBUG_PARSE
                        printf("Missing boundary attribute in multipart/*, downgrading to text/plain\n");
#endif                        
                    }
                }
#endif /* 0 */
                /* a limit to avoid having the message_node tree growing
                   uncontrollably */
                if ((set_max_attach_per_msg != 0)
                    && (att_counter > set_max_attach_per_msg)) {
                    content = CONTENT_IGNORE;
#if DEBUG_PARSE
                    printf("Hit max_attach_per_msg limit; ignoring further attachments for msgid %s\n", msgid);
#endif                        
                }
                
                if (content == CONTENT_IGNORE) {
                    continue;
                } else if (ignorecontent(type)) {
                    /* don't save this */
                    content = CONTENT_IGNORE;
                    continue;
                }
#if 0
                /* not sure if we should add charset save here or wait until later */
                if (charset[0] == NULL) {
                    strcpy(charset, set_default_charset);
                }
#endif

                /* parsing of all Content-* related headers except for Content-Type */
		description = NULL;
		for (head = headp; head; head = head->next) {
		    if (head->parsedheader || !head->header || head->invalid_header)
			continue;
                    
		    /* Content-Description is defined ... where?? */
		    if (!strncasecmp(head->line, "Content-Description:", 20)) {
			char *ptr = head->line;
			description = ptr + 21;
			head->parsedheader = TRUE;
		    }
		    /* Content-Disposition is defined in RFC 2183 */
		    else if (!strncasecmp (head->line, "Content-Disposition:", 20)) {
			char *ptr = head->line + 20;
			char *fname;
			char *np;

                        head->parsedheader = TRUE;

                        if (inlist(set_ignore_content_disposition, type)) {
                            continue;
                        }
                        
			while (*ptr && isspace(*ptr))
			    ptr++;
			if (!strncasecmp(ptr, "attachment", 10)
			    && (content != CONTENT_IGNORE)) {
			    /* signal we want to attach, rather than embeed this MIME
			       attachment */
			    if (inlist(set_ignore_types, "$NONPLAIN")
				|| inlist(set_ignore_types, "$BINARY"))
                                content = CONTENT_IGNORE;
			    else {
				attach_force = TRUE;

				/* make sure it is binary */
				content = CONTENT_BINARY;

				/* see if there's a file name to use: */
				fname = strcasestr(ptr, "filename=");
				if (fname) {
                                    np = fname+9;
                                    _extract_attachname(np, attachname, sizeof(attachname));
				}
				else {
				    attachname[0] = '\0';  /* just clear it */
				}
				file_created = MAKE_FILE; /* please make one */
			    }
			}

			else if (!strncasecmp(ptr, "inline", 6)
				 && (content != CONTENT_IGNORE)
				 && inlinecontent(type)) {
			    inline_force = TRUE;
			    /* make sure it is binary */
			    content = CONTENT_BINARY;
			    /* see if there's a file name to use: */
			    fname = strcasestr(ptr, "filename=");
			    if (fname) {
                                np = fname+9;
                                _extract_attachname(np, attachname, sizeof(attachname));
			    }
			    else {
				attachname[0] = '\0';	/* just clear it */
			    }
			    file_created = MAKE_FILE;	/* please make one */
			} /* inline */
                        
                    } /* Content-Disposition: */
		    else if (!strncasecmp(head->line, "Content-Base:", 13)) {
#ifdef NOTUSED
			char *ptr = head->line + 13;
                        /* we just ignore this header. Why were we ignoring the whole
                           attachment? */
                        content=CONTENT_IGNORE;
#endif
			/* we must make sure this is not parsed more times
			   than this */
			head->parsedheader = TRUE;

                    } else if (!strncasecmp
                               (head->line, "Content-Transfer-Encoding:", 26)) {
			char *ptr = head->line + 26;

			head->parsedheader = TRUE;

			while (isspace(*ptr))
			    ptr++;
			if (!strncasecmp(ptr, "QUOTED-PRINTABLE", 16)) {
			    decode = ENCODE_QP;
			}
			else if (!strncasecmp(ptr, "BASE64", 6)) {
			    decode = ENCODE_BASE64;
                            b64_decoder_state = base64_decoder_state_new();
			}
			else if (!strncasecmp(ptr, "8BIT", 4)) {
			    decode = ENCODE_NORMAL;
			}
			else if (!strncasecmp(ptr, "7BIT", 4)) {
			    decode = ENCODE_NORMAL;
			}
			else if (!strncasecmp(ptr, "x-uue", 5)) {
			    decode = ENCODE_UUENCODE;
                            /* JK 20230504: what does this do?
                               break; do we need to abort content-type too?  */
			    if (!do_uudecode(fp, line, line_buf,
					     &raw_text_buf, fpo))
			        break;
			}
			else {
			    /* Unknown format, we use default decoding */
			    char code[64];

			    /* is there any value for content-encoding or is it missing? */
			    if (sscanf(ptr, "%63s", code) != EOF) {

			      trio_snprintf(line, sizeof(line_buf) - set_ietf_mbox,
					    " ('%s' %s)\n", code,
					    lang[MSG_ENCODING_IS_NOT_SUPPORTED]);

			      bp = addbody(bp, &lp, line,
					   BODY_HTMLIZED | bodyflags);

#if DEBUG_PARSE
			      printf("Ignoring unknown Content-Transfer-Encoding: %s\n", code);
#endif
			    } else {
#if DEBUG_PARSE
			      printf("Missing Content-Transfer-Encoding value\n");
#endif
			    }
			}
#if DEBUG_PARSE
			printf("DECODE set to %d\n", decode);
#endif
		    } /* Content-Transfer-Encoding */
                } /* for Content-* except Content-Type */

                /* process specific Content-Type values */
                do {
                    if (alternativeparser) {
                        struct body *temp_bp = NULL;
                        
                        /* We are parsing alternatives... */
                        
                        if (parse_multipart_alternative_force_save_alts
                            && multipart_stack_top_has_type(multipartp, "multipart/alternative")
                            && *last_alternative_type
                            && !strcasecmp(last_alternative_type, "text/plain")) {
                            
                            /* if the UA is Apple mail and if the only
                            ** alternatives are text/plain and
                            ** text/html and if the preference is
                            ** text/plain, skip the text/html version
                            ** if the applemail_hack is enabled
                            */
                            if (!strcasecmp(type, "text/html")) {
#if DEBUG_PARSE
                                fprintf(stderr, "Discarding apparently equivalent text/html alternative\n");
#endif
                                content = CONTENT_IGNORE;
                                break;
                            }
                        }
                        
                        if (preferedcontent(&alternative_weight, type, decode)) {
                            /* ... this is a prefered type, we want to store
                               this [instead of the earlier one]. */
                            /* erase the previous alternative info */
                            if (current_message_node->alternative) {
                                current_message_node->skip = MN_SKIP_ALL;
                            }

                            strncpy(last_alternative_type, type,
                                    sizeof(last_alternative_type) - 2);
                            /* make sure it's a NULL ending string if ever type > 128 */
                            last_alternative_type[sizeof(last_alternative_type) - 1] = '\0';
#ifdef DEBUG_PARSE
                            fprintf(stderr, "setting new prefered alternative charset to %s\n", charset);
#endif

                            alternative_lastfile_created = NO_FILE;
                            content = CONTENT_UNKNOWN;
                            /* @@ JK: add here a delete for mmixed, for all children,
                               composite or not under this node */
                            if (root_message_node != current_message_node
                                && current_alt_message_node == current_message_node) {
                                message_node_delete_attachments(current_message_node);
                            }
                            if (alternative_lastfile[0] != '\0') {
                                /* remove the previous attachment */
                                /* unlink(alternative_lastfile); */
                                alternative_lastfile[0] = '\0';
                            }
                        }
                        else if (local_set_save_alts == 2) {
                            content = CONTENT_BINARY;
                        } else {
                            /* ...and this type is not a prefered one. Thus, we
                             * shall ignore it completely! */
                            content = CONTENT_IGNORE;
                            /* erase the current alternative info */
                            temp_bp = bp;	/* remember the value of bp for GC */
                            /*
                              lp = alternative_lp;
                              bp = alternative_bp;
                            */
                            lp = bp = headp = NULL;
                            strcpy(alternative_file,
                                   alternative_lastfile);
                            file_created =
                                alternative_lastfile_created;
                            alternative_bp = alternative_lp = NULL;
                            alternative_lastfile_created = NO_FILE;
                            alternative_lastfile[0] = '\0';
                            /* we haven't yet created any attachment file, so there's no need
                               to erase it yet */
                        }
                        
                        /* free any previous alternative */
                        free_body (temp_bp);
                        
                        /* @@ not sure if I should add a diff flag to do this break */
                        if (content == CONTENT_IGNORE)
                            /* end the header parsing... we already know what we want */
                            break;
                        
                    } /* alternativeparser */
                    
                    if (content == CONTENT_IGNORE)
                        break;
                    else if (ignorecontent(type)) {
                        /* don't save this */
                        content = CONTENT_IGNORE;
                        break;
                    } else if (textcontent(type)
                             || (inlinehtml &&
                                 !strcasecmp(type, "text/html"))) {
                        /* text content or text/html follows.
                         */
                        
                        if (local_set_save_alts && alternativeparser
                            && content == CONTENT_BINARY) {
                            file_created = MAKE_FILE; /* please make one */
                            description = set_alts_text ? set_alts_text
                                : "alternate version of message";
                            /* JK 2023/04: why is description tied to
                               the length of attachname and why it was
                               using it to make a filename?  code
                               commented out while investigating. We
                               get the filename from the filename
                               found in Content-Disposition or
                               Content-Type, and if none is found, we
                               generate one.
                            */
#ifdef FIX_OR_DELETE_ME
                            strncpy(attachname, description, sizeof(attachname) - 1);
                            /* make sure it's a NULL terminated string */
                            attachname[sizeof(attachname) - 1] = '\0';
                            safe_filename(attachname);
#endif
                        }

                        /* if it's not a stored attachment,
                        ** try to define content more precisely
                        ** The condition to detect if it's a 
                        ** is to see if file_created == MAKE_FILE
                        ** or content = CONTENT_BINARY */
                        else if (file_created != MAKE_FILE) {
                            if (!strcasecmp(type, "text/html"))
                                content = CONTENT_HTML;
                            else
                                content = CONTENT_TEXT;
                        } else {
                            /* we should refactor and simplify the cases when
                               we call the following function. 
                               It's needed here when a text/plain part has
                               Content-Disposition: attachment and a filename
                               given only in the Content-Type name attribute */
                            _control_attachname(content_type_ptr, attachname, sizeof(attachname));
                        }
                        break;

                    } /* textcontent(type) || inlinehtml && type == text/html */

                    
#if 1 || TESTING_IF_THIS_IS_AN_ERROR
                    else if (attach_force) {
                        /* maybe copy description and desc default values here?
                           other things here? 
                           what to do with content == CONTENT_BINARY?
                        */

                        {
                            /* don't like calling this function in two parts,
                               but we need to fix a bug. Will have to refactorize how
                               to handle attach_force when we revisit the code */
                            
                            /* if attachname is empty, copy the value of the name attribute,
                               if given in the Content-Type header */
                            _control_attachname(content_type_ptr, attachname, sizeof(attachname));
                        }
                        break;
                    }
#endif
                    else if (!strncasecmp(type, "message/rfc822", 14)) {
                        /*
                         * Here comes an attached mail! This can be ugly,
                         * since the attached mail may very well itself
                         * contain attached binaries, or why not another
                         * attached mail? :-)
                         *
                         * We need to store the current boundary separator
                         * in order to get it back when we're done parsing
                         * this particular mail, since each attached mail
                         * will have its own boundary separator that *might*
                         * be used.
                         */
                        
                        /* need to take into account alternates with rfc822? */
                        if (boundp == NULL && multipartp == NULL) {
                            /* we have a non multipart message with a message/rfc822
                               content-type body */
                            bp = addbody(bp, &lp,
                                         NULL,
                                         BODY_ATTACHMENT | BODY_ATTACHMENT_RFC822);
                            
                        } else {
                            free_body(bp);
                            description = NULL;
                            bp = lp = headp = NULL;
                            attachment_rfc822 = TRUE;
                        }
                        isinheader = 1;

                        /* RFC2046 states that message/rfc822 can only
                           have Content-Transfer-Encoding values of 7bit, 
                           8bit, and binary. Some broken mail clients 
                           may have used something else */
                        if (decode != ENCODE_NORMAL) {
#if DEBUG_PARSE
                            printf("Error: msgid %s : message/rfc822 Content-Type associated with a\n"
                                   "Content-Transfer-Encoding that is not\n7bit, 8bit, or binary.\n"
                                   "Forcing ENCODE_NORMAL\n", msgid);
#endif                            
                            if (decode == ENCODE_BASE64) {
                                base64_decoder_state_free(b64_decoder_state);
                                b64_decoder_state = NULL;
                            }
                            decode = ENCODE_NORMAL;
                        }
                        
                        /* reset the apple mail hack and the
                           local_set_save_alts as we don't know if the
                           forwarded message was originally sent from
                           an apple mal client */
                        parse_multipart_alternative_force_save_alts = 0;
                        applemail_old_set_save_alts = -1;
                        local_set_save_alts = set_save_alts;
                        break;
                        
                    } /* message/rfc822 */

                    else if (strncasecmp(type, "multipart/", 10)) {
                        /*
                         * This is not a multipart and not text
                         */
                        
                        /*
                         * only do anything here if we're not
                         * ignoring this content
                         */
                        if (CONTENT_IGNORE != content) {
                            /* only use the Content-Type name attribute to get 
                               the filename if Content-Disposition didn't 
                               provide a filename */
                            _control_attachname(content_type_ptr, attachname, sizeof(attachname));
                            file_created = MAKE_FILE;	/* please make one */
                            content = CONTENT_BINARY;	/* uknown turns into binary */
                        }
                        break;
                        
                    } /* !multipart/ */

                    else {
                        /*
                         * Find the first boundary separator
                         */
                        
                        struct body *tmpbp;
                        struct body *tmplp;
                        bool found_start_boundary;
                        

#if DELETE_ME_CODE_MOVED_UP
                        boundary_id = strcasestr(content_type_ptr, "boundary=");
#if DEBUG_PARSE
                        printf("boundary found in %s\n", ptr);
#endif
#endif
                        if (boundary_id) {
#if DELETE_ME_CODE_MOVED_UP
                            boundary_id = strchr(boundary_id, '=');
                            if (boundary_id) {
                                boundary_id++;
                                while (isspace(*boundary_id))
                                    boundary_id++;
                                *boundbuffer = '\0';
                                if ('\"' == *boundary_id) {
                                    sscanf(++boundary_id, "%255[^\"]",
                                           boundbuffer);
                                }
                                else
                                    sscanf(boundary_id, "%255[^;\n]",
                                           boundbuffer);
                                boundary_id = boundbuffer;
                            }
#endif
                            
                            /* restart on a new list: */
                            tmpbp = tmplp = NULL;
                            found_start_boundary = FALSE;
                            
                            while (fgets(line_buf, MAXLINE, fp)) {
                                char *tmpline;
                                
                                if(set_append) {
                                    if(fputs(line_buf, fpo) < 0) {
                                        progerr("Can't write to \"mbox\""); /* revisit me */
                                    }
                                }

                                tmpline = line_buf + set_ietf_mbox;

                                /* 
                                ** detect different cases where we may have broken, missing,
                                ** or unexpected start and end boundaries. 
                                ** Using mutt as a reference on how to process each case
                                **/

                                /* start boundary? */
                                if (is_start_boundary(boundary_id, tmpline)) {
                                    found_start_boundary = TRUE;
                                    break;
                                }
                                /* new message found */
                                if (!strncasecmp(line_buf, "From ", 5)) {
#if DEBUG_PARSE
                                    printf("Error, new message found instead of expected start_boundary: %s\n", boundbuffer);                     
#endif
                                    break;

                                }
                                /* a preceding non-closed boundary?  */
                                else if (!strncmp(tmpline, "--", 2)
                                         && ! _is_signature_separator(line)) {
                                    char *tmp_boundary = boundary_stack_has_id(boundp, tmpline);

                                    boundary_id = tmp_boundary;
#if DEBUG_PARSE
                                    printf("Error, an existing boundary found instead of expected start_boundary: %s\n", boundbuffer);                     
#endif
                                    break;
                                }
                                /* save lines in case no boundary found */
                                tmpbp = addbody(tmpbp, &tmplp, tmpline, bodyflags);
                            }
                            
                            /* control we found the start boundary we were expecting */
                            if (!found_start_boundary) {
#if DEBUG_PARSE
                                printf("Error: didn't find start boundary\n");
                                printf("last line read:\n%s", line_buf);
#endif
                                isinheader = 0;
                                boundary_id = NULL;
                                
                                if (tmpbp) {
                                    bp = append_body(bp, &lp, tmpbp, TRUE); 
                                }

                                /* downgrading to text/plain */
                                strcpy(type, "text/plain");
                                content_type_ptr = type;
#if DEBUG_PARSE
                                printf("Downgrading to text/plain\n");
#endif         
                                goto leave_header;
                            }
                            free_body(tmpbp);
                            
                            /*
                            **  we got a new part coming
                            */
                            current_message_node =
                                message_node_mimetest(current_message_node,
                                                      bp, lp, charset, charsetsave,
                                                      type,
                                                      (boundp) ? boundp->boundary_id : NULL,
                                                      boundary_id,
                                                      att_binname,
                                                      meta_filename,
                                                      att_link,
                                                      att_comment_filename,
                                                      attachment_rfc822,
                                                      message_node_skip_status(file_created,
                                                                               content,
                                                                               type));
#if DEBUG_PARSE_MSGID_TRACE
                            current_message_node->msgid = strsav(msgid);
#endif
                            if (alternativeparser) {
                                current_alt_message_node = current_message_node;
                            }
                            if (att_binname) {
                                free(att_binname);
                                att_binname = NULL;
                            }
                            if (meta_filename) {
                                free(meta_filename);
                                meta_filename = NULL;
                            }
                            if (att_link) {
                                free(att_link);
                                att_link = NULL;
                            }
                            if (att_comment_filename) {
                                free(att_comment_filename);
                                att_comment_filename = NULL;
                            }
                            
                            if (alternativeparser) {
                                current_message_node->alternative = TRUE;
                            }

                            /*
                            if (!strncasecmp(type, "multipart/related", 17)) {
                                current_message_node->skip = MN_SKIP_BUT_KEEP_CHILDREN;
                            }
                            */
                            
                            if (!root_message_node) {
                                root_message_node = current_message_node;
                            }
                            
                            /*
                             * This stores the boundary string in a stack
                             * of strings:
                             */
                            if (boundp && alternativeparser) {
                                /* if we were dealing with multipart/alternative or
                                   message/rfc822, store the current content */
                                boundp->alternativeparser = alternativeparser;
                                boundp->alternative_weight = alternative_weight;
                                boundp->alternative_message_node_created =
                                    alternative_message_node_created;
                                strcpy(boundp->alternative_file, alternative_file);
                                strcpy(boundp->alternative_lastfile, alternative_lastfile);
                                strcpy(boundp->last_alternative_type, last_alternative_type);
                                boundp->alternative_lp = alternative_lp;
                                boundp->alternative_bp = alternative_bp;
                                boundp->current_alt_message_node = current_alt_message_node;
                                boundp->root_alt_message_node = root_alt_message_node;
                                current_alt_message_node = root_alt_message_node = NULL;
                                alternative_file[0] = alternative_lastfile[0] = last_alternative_type[0] = '\0';
                                alternative_message_node_created = FALSE;
                                alternativeparser = FALSE;
                            }

                            boundp = boundary_stack_push(boundp, boundbuffer);
                            boundp->parse_multipart_alternative_force_save_alts = parse_multipart_alternative_force_save_alts;
                            boundp->applemail_old_set_save_alts = applemail_old_set_save_alts;
                            boundp->set_save_alts = local_set_save_alts;
                            multipartp = multipart_stack_push(multipartp, type);
                            skip_mime_epilogue = FALSE;

                            attachment_rfc822 = FALSE;
                            
                            description = NULL;
                            *filename = '\0';
                            bp = lp = headp = NULL;
                            /* printf("set new boundary: %s\n", boundp->boundary_id); */

                            if (charset) {
                                free(charset);
                                charset = NULL;
                            }
                            charsetsave[0] = '\0';
                            
#ifdef DEBUG_PARSE
                            fprintf(stderr, "restoring parents charset %s and charsetsave %s\n", charset, charsetsave);
#endif
                            
                            /*
                             * We set ourselves, "back in header" since there is
                             * gonna come MIME headers now after the separator
                             */
                            isinheader = 1;
                            
                            /* Daniel Stenberg started adding the
                             * "multipart/alternative" parser 13th of July
                             * 1998!  We check if this is a 'multipart/
                             * alternative' header, in which case we need to
                             * treat it very special.
                             */
                            
                            if (!strncasecmp
                                (&content_type_ptr[10], "alternative", 11)) {
                                /* It *is* an alternative session!  Alternative
                                 * means there will be X parts with the same text
                                 * using different content-types. We are supposed
                                 * to take the most prefered format of the ones
                                 * used and only output that one. MIME defines
                                 * the order of the texts to start with pure text
                                 * and then continue with more and more obscure
                                 * formats. (well, it doesn't use those terms but
                                 * that's what it means! ;-))
                                 */
                                
                                /* How "we" are gonna deal with them:
                                 *
                                 * We create a "spare" linked list body for the
                                 * very first part. Since the first part is
                                 * defined to be the most readable, we save that
                                 * in case no content-type present is prefered!
                                 *
                                 * We skip all parts that are not prefered. All
                                 * prefered parts found will replace the first
                                 * one that is saved. When we reach the end of
                                 * the alternatives, we will use the last saved
                                 * one as prefered.
                                 */
                                
                                savealternative = TRUE;
#if DEBUG_PARSE
                                printf("SAVEALTERNATIVE: yes\n");
#endif
                            }

                        }
                        else
                            boundary_id = NULL;
                    }
                    break;
                } while (0); /* do .. while (0) */
                
#endif /* NEW_PARSER */

		/* @@@ here we try to do a post parsing cleanup */
		/* have to find out all the conditions to turn it off */
		if (attach_force) {
		    savealternative = FALSE;
		    isinheader = 0;
                    /* a kludge while I wait to see how to better integrate this
                       case */
                    content = CONTENT_BINARY;
		}

		if (savealternative) {
		    alternativeparser = TRUE;
		    /* restart on a new list: */
		    lp = bp = headp = NULL;
		    /* clean the alternative status variables */
		    alternative_weight = -1;
		    alternative_lp = alternative_bp = NULL;
		    alternative_lastfile_created = NO_FILE;
		    alternative_file[0] = alternative_lastfile[0] = '\0';
                    last_alternative_type[0] = '\0';
		}
		headp = lp;	/* start at this point next time */
	    }
	    else {
		bp = addbody(bp, &lp, line, BODY_HEADER | bodyflags);
	    }
	}
	else {

	    /* not in header */
	leave_header:
	    /* If this isn't a single mail: see if the line is a message
	     * separator. If there is a "^From " found, check to see if there
	     * is a valid date field in the line. If not then consider it a
	     * part of the body of the message and skip it.
	     * Daniel: I don't like this. I don't think there is something like
	     * "a valid date field" in that line 100%.
	     */
	    if (!readone &&
		!strncmp(line_buf, "From ", 5) &&
		(*(dp = getfromdate(line)) != '\0')) {
		if (-1 != binfile) {
		    close(binfile);
		    binfile = -1;
		}

                if (bp || lp) {
                    /* if we reach this condition, it means the message is missing one or
                       more mime boundary ends. Closing the current active node should fix
                       this */
                    if (current_message_node) {
                        current_message_node =
                            message_node_mimetest(current_message_node,
                                                  bp, lp, charset, charsetsave,
                                                  type,
                                                  (boundp) ? boundp->boundary_id : NULL,
                                                  boundary_id,
                                                  att_binname,
                                                  meta_filename,
                                                  att_link,
                                                  att_comment_filename,
                                                  attachment_rfc822,
                                                  message_node_skip_status(file_created,
                                                                           content,
                                                                           type));
#if DEBUG_PARSE_MSGID_TRACE
                        current_message_node->msgid = strsav(msgid);
#endif
                    }
                }

                /* THE PREFERED CHARSET ALGORITHM */

                /* as long as we don't handle UTF-8 throughout), use the prefered
                   content charset if we got one  */

                /* see struct.c:choose_charset() for the algo heuristics 1 */
                if (root_message_node) {
                    prefered_charset = message_node_get_charset(root_message_node);
                } else {
                    prefered_charset = _single_content_get_charset(charset, charsetsave);
                }

                if (prefered_charset && set_replace_us_ascii_with_utf8
                    && !strncasecmp(prefered_charset, "us-ascii", 8)) {
                    if (set_debug_level) {
                        fprintf(stderr, "Replacing content charset %s with UTF-8\n",
                                prefered_charset);
                    }                                    
                    free(prefered_charset);
                    prefered_charset = strsav("UTF-8");
                }

                if (set_debug_level) {
                    fprintf(stderr, "Message will be stored using charset %s\n", prefered_charset);
                }

#ifdef CHARSETSP
                if (prefered_content_charset) {
                    if (charset) {
                        free(charset);
                    }
                    charset = prefered_content_charset;
                    prefered_content_charset = NULL;
                }

#ifdef HAVE_ICONV
		if (!charset) {
                    if (*charsetsave!=0){
#ifdef DEBUG_PARSE
                        printf("put charset from subject header..\n");
#endif
                        charset=strsav(charsetsave);
                    } else{
                        /* default charset for plain/text is US-ASCII */
                        /* UTF-8 is modern, however (DM) */
                        charset=strsav(set_default_charset);
#ifdef DEBUG_PARSE
                        fprintf(stderr, "found no charset for body, using default_charset %s.\n", set_default_charset);
#endif
                    }
		} else {
                    /* if body is us-ascii but subject is not,
                       try to use subject's charset. */
                    if (strncasecmp(charset,"us-ascii",8)==0){
                        if (*charsetsave!=0 && strcasecmp(charsetsave,"us-ascii")!=0){
                            free(charset);
                            charset=strsav(charsetsave);
                        }
                    }
		}
#endif /* ICONV */
#endif /* CHARSETSP */
                
		isinheader = 1;
		if (!hassubject)
		    subject = NOSUBJECT;
		else
		    hassubject = 1;

		if (!hasdate)
		    date = NODATE;
		else
		    hasdate = 1;

		if (!inreply)
		    inreply = oneunre(subject);

		/* control the use of format and delsp according to RFC 3676 */
		if (textplain_format == FORMAT_FLOWED
		    && (content != CONTENT_TEXT
                        || (content == CONTENT_TEXT
                            && strcasecmp (type, "text/plain")))) {
                    /* format flowed only allowed on text/plain */
                    textplain_format = FORMAT_FIXED;
		}

		if (textplain_format == FORMAT_FIXED && delsp_flag) {
                    /* delsp only accepted for format=flowed */
                    delsp_flag = FALSE;
		}

                if (root_message_node) {
                    /* multipart message */
                    
                    if (set_debug_level == DEBUG_DUMP_ATT
                        || set_debug_level == DEBUG_DUMP_ATT_VERBOSE) {
                        message_node_dump (root_message_node);
                        progerr("exiting");
                    }
                
                    bp = message_node_flatten (&lp, root_message_node);
                    /* free memory allocated to message nodes */
                    message_node_free(root_message_node);
                    root_message_node = current_message_node = NULL;
                    root_alt_message_node = current_alt_message_node = NULL;
                } else {
                    /* it was not a multipart message, remove all empty lines
                       at the end of the message */
                    while (rmlastlines(bp));
                }
                
		if (append_bp && append_bp != bp) {
                    /* if we had attachments, close the structure */
                    append_bp = addbody(append_bp, &append_lp,
                                        NULL,
                                        BODY_ATTACHMENT_LINKS | BODY_ATTACHMENT_LINKS_END);
                    lp = quick_append_body(lp, append_bp);
		    append_bp = append_lp = NULL;
		}
		else if(!bp) {	/* probably never used */
		    bp = addbody(bp, &lp, "Hypermail was not able "
				 "to parse this message correctly.\n",
				 bodyflags);
                }
                
		if (set_mbox_shortened && !increment && num == startnum
		    && max_msgnum >= set_startmsgnum) {
		    emp = hashlookupbymsgid(msgid);
		    if (!emp) {
                        trio_snprintf(errmsg, sizeof(errmsg),
                                      "Message with msgid '%s' not found in .hm2index",
                                      msgid);
                        progerr(errmsg);
		    }
		    num = emp->msgnum;
		    num_added = insert_older_msgs(num);
		}
		emp = NULL;
		if (set_mbox_shortened) {
		    if (hashnumlookup(num, &emp)) {
			if(strcmp(msgid, emp->msgid)
			   && !strstr(emp->msgid, "hypermail.dummy")) {
			    trio_snprintf(errmsg, sizeof(errmsg),
                                          "msgid mismatch %s %s", msgid, emp->msgid);
			    progerr(errmsg);
			}
		    }
		}
		if (!emp) {
		  emp =
		    addhash(num, date, namep, emailp, msgid, subject,
			    inreply, fromdate, prefered_charset, NULL, NULL, bp);
                }
                /*
                 * dp, if it has a value, has a date from the "From " line of
                 * the message after the one we are just finishing.
                 * SMR 19 Oct 99: moved this *after* the addhash() call so it
                 * isn't erroneously associate with the previous message
                 */

                strcpymax(fromdate, dp ? dp : "", DATESTRLEN);

		if (emp) {
		    emp->exp_time = exp_time;
		    emp->is_deleted = is_deleted;
		    emp->annotation_robot = annotation_robot;
		    emp->annotation_content = annotation_content;

		    if (insert_in_lists(emp, require_filter,
					require_filter_len + require_filter_full_len))
		        ++num_added;
		    num++;
                    
		} else {
                    /* addhash refused to add this message, maybe it's a duplicate id
                       or it failed one of its tests. 
                       We delete the body to avoid and associated attachments to 
                       avoid memory leaks */
                    free_body(bp);
                    
                    if (att_dir != NULL) {
                        emptydir(att_dir);
                        rmdir(att_dir);
                    }
                }
		for (pos = 0; pos < require_filter_len; ++pos)
		    require_filter[pos] = FALSE;
		for (pos = 0; pos < require_filter_full_len; ++pos)
		    require_filter_full[pos] = FALSE;
		if (set_txtsuffix && emp && set_increment != -1)
		    write_txt_file(emp, &raw_text_buf);

		if (hasdate) {
		    free(date);
                    date = NULL;
                }
		if (hassubject) {
		    free(subject);
                    subject = NULL;
                }
		if (charset) {
		    free(charset);
		    charset = NULL;
		}
		if (charsetsave){
		  *charsetsave = 0;
		}
                if (prefered_charset) {
                    free(prefered_charset);
                    prefered_charset = NULL;
                }                
		if (msgid) {
		    free(msgid);
		    msgid = NULL;
		}
		if (inreply) {
		    free(inreply);
		    inreply = NULL;
		}
		if (namep) {
		    free(namep);
		    namep = NULL;
		}
		if (emailp) {
		    free(emailp);
		    emailp = NULL;
		}

		bp = lp = headp = NULL;
		bodyflags = 0;	/* reset state flags */

		/* reset related RFC 3676 state flags */
		textplain_format = FORMAT_FIXED;
		delsp_flag = FALSE;
		flowed_line = FALSE;
		quotelevel = 0;
		continue_previous_flow_flag = FALSE;

		/* go back to default mode: */
                file_created = alternative_lastfile_created = NO_FILE;
		content = CONTENT_TEXT;
                if (decode == ENCODE_BASE64) {
                    base64_decoder_state_free(b64_decoder_state);
                    b64_decoder_state = NULL;
                }                                
		decode = ENCODE_NORMAL;
		Mime_B = FALSE;
                skip_mime_epilogue = FALSE;
		headp = NULL;
                attachment_rfc822 = FALSE;
		multilinenoend = FALSE;
		if (att_dir) {
		    free(att_dir);
		    att_dir = NULL;
		}
		if (set_usemeta && meta_dir) {
		    free(meta_dir);
		    meta_dir = NULL;
		}
		att_counter = 0;
                if (att_name_list) {
                    hmlist_free (att_name_list);
                    att_name_list = NULL;
                }                
		inline_force = FALSE;
                attach_force = FALSE;
		*attachname = '\0';

                if (att_binname) {
                    free(att_binname);
                    att_binname = NULL;
                }
                if (meta_filename) {
                    free(meta_filename);
                    meta_filename = NULL;
                }
                if (att_link) {
                    free(att_link);
                    att_link = NULL;
                }
                if (att_comment_filename) {
                    free(att_comment_filename);
                    att_comment_filename = NULL;
                }
                
		/* by default we have none! */
		hassubject = 0;
		hasdate = 0;
                message_headers_parsed = FALSE;

		annotation_robot = ANNOTATION_ROBOT_NONE;
		annotation_content = ANNOTATION_CONTENT_NONE;

		is_deleted = 0;
		exp_time = -1;

		boundary_stack_free(boundp);
		boundp = NULL;
                boundary_id = NULL;

                multipart_stack_free(multipartp);
		multipartp = NULL;

                alternativeparser = FALSE; /* there is none anymore */

		if (parse_multipart_alternative_force_save_alts) {
                    parse_multipart_alternative_force_save_alts = 0;

#if DEBUG_PARSE
                    printf("Applemail_hack resetting parse_multipart_alternative_force_save_alts\n");
#endif
                    if (applemail_old_set_save_alts != -1) {
                        local_set_save_alts = applemail_old_set_save_alts;
                        applemail_old_set_save_alts = -1;
#if DEBUG_PARSE
                        printf("Applemail_hack resetting save_alts to %d\n", local_set_save_alts);
#endif
                    }
		}

		if (!(num % 10) && set_showprogress && !readone) {
		    print_progress(num - startnum, NULL, NULL);
		}
#if DEBUG_PARSE
		printf("LAST: %s", line);
#endif
	    }
	    else {		/* decode MIME complient gibberish */
		char newbuffer[MAXLINE];
		char *data;
		int datalen = -1;	/* -1 means use strlen to get length */

		if (set_linkquotes && !inreply) { /* why only if set_linkquotes? pcm */
		    char *new_inreply = getreply(line);
		    if (new_inreply && !*new_inreply) {
                        free(new_inreply);
                    } else {
                        inreply = new_inreply;
                    }
		}

		if (Mime_B) {
		    if (boundp &&
			!strncmp(line, "--", 2)
                        && ! _is_signature_separator(line)
                        && boundary_stack_has_id(boundp, line)) {
			/* right at this point, we have another part coming up */
#if DEBUG_PARSE
			printf("hit %s\n", line);
#endif

                        if (bp) {
                            /* store the current attachment and prepare for
                               the new one */
                            current_message_node =
                                message_node_mimetest(current_message_node,
                                                      bp, lp, charset, charsetsave,
                                                      type,
                                                      (boundp) ? boundp->boundary_id : NULL,
                                                      boundary_id,
                                                      att_binname,
                                                      meta_filename,
                                                      att_link,
                                                      att_comment_filename,
                                                      attachment_rfc822,
                                                      message_node_skip_status(file_created,
                                                                               content,
                                                                               type));
#if DEBUG_PARSE_MSGID_TRACE
                            current_message_node->msgid = strsav(msgid);
#endif
                            if (alternativeparser) {
                                current_alt_message_node = current_message_node;
                            }
                            if (att_binname) {
                                free(att_binname);
                                att_binname = NULL;
                            }
                            if (meta_filename) {
                                free(meta_filename);
                                meta_filename = NULL;
                            }
                            if (att_link) {
                                free(att_link);
                                att_link = NULL;
                            }
                            if (att_comment_filename) {
                                free(att_comment_filename);
                                att_comment_filename = NULL;
                            }
                            if (alternativeparser) {
                                current_message_node->alternative = TRUE;
                            }

                            attachment_rfc822 = FALSE;
                            
                            description = NULL;
                            *filename = '\0';
                            bp = lp = headp = NULL;
                        }

                        /* make sure the boundaryp stack's top corresponds
                           to the boundary we're processing. This is to take
                           into account missing end boundaries */
                        if ( ! boundary_stack_top_has_id(boundp, line) ) {
                            boundary_stack_pop_to_id(&boundp, line);
                            /* move the current_message_node pointer */
                            current_message_node = message_node_get_parent_with_boundid(current_message_node, boundp);
                            /* restore context for this boundp here (hate that this
                               restore context code is duplicated) */
                            if (boundp) {
                                parse_multipart_alternative_force_save_alts = boundp->parse_multipart_alternative_force_save_alts;
                                applemail_old_set_save_alts = boundp->applemail_old_set_save_alts;
                                local_set_save_alts = boundp->set_save_alts;
                                
                                if (boundp->alternativeparser) {
                                    alternativeparser = boundp->alternativeparser;
                                    alternative_weight = boundp->alternative_weight;
                                    alternative_message_node_created =
                                        boundp->alternative_message_node_created;
                                    strcpy(alternative_file, boundp->alternative_file);
                                    strcpy(alternative_lastfile, boundp->alternative_lastfile);
                                    strcpy(last_alternative_type, boundp->last_alternative_type);
                                    alternative_lp = boundp->alternative_lp;
                                    alternative_bp = boundp->alternative_bp;
                                    current_alt_message_node = boundp->current_alt_message_node;
                                    root_alt_message_node = boundp->root_alt_message_node;
                                    boundp->alternative_file[0] = '\0';
                                    boundp->alternative_lastfile[0] = '\0';
                                    boundp->last_alternative_type[0] = '\0';
                                    boundp->current_alt_message_node = NULL;
                                    boundp->root_alt_message_node = NULL;
                                    boundp->alternativeparser = FALSE;
                                    boundp->alternative_message_node_created = FALSE;
                                }
                            }                            
                        }
                        
                        if (is_end_boundary(boundp->boundary_id, line)) {
                            isinheader = 0;	/* no header, the ending boundary
                                                   can't have any describing
                                                   headers */

#if DEBUG_PARSE
			    printf("End boundary %s\n", line);
                            printf("alternativeparser %d\n", alternativeparser);
                            printf("has_more_alternatives %d\n", multipart_stack_has_type(multipartp, "multipart/alternative"));
#endif

                            /* this multipart/ part ends, move the message_node cursor to
                               its parent unless we are at root  */
                            if (current_message_node->parent) {
                                current_message_node = message_node_get_parent(current_message_node);
                            }
			    boundp = boundary_stack_pop(boundp);
                            /* restore the context associated with the active boundary */
                            if (boundp) {
                                parse_multipart_alternative_force_save_alts = boundp->parse_multipart_alternative_force_save_alts;
                                applemail_old_set_save_alts = boundp->applemail_old_set_save_alts;
                                local_set_save_alts = boundp->set_save_alts;
                                
                                if (boundp->alternativeparser) {
                                    alternativeparser = boundp->alternativeparser;
                                    alternative_weight = boundp->alternative_weight;
                                    alternative_message_node_created =
                                        boundp->alternative_message_node_created;
                                    strcpy(alternative_file, boundp->alternative_file);
                                    strcpy(alternative_lastfile, boundp->alternative_lastfile);
                                    strcpy(last_alternative_type, boundp->last_alternative_type);
                                    alternative_lp = boundp->alternative_lp;
                                    alternative_bp = boundp->alternative_bp;
                                    current_alt_message_node = boundp->current_alt_message_node;
                                    root_alt_message_node = boundp->root_alt_message_node;
                                    boundp->alternative_file[0] = '\0';
                                    boundp->alternative_lastfile[0] = '\0';
                                    boundp->last_alternative_type[0] = '\0';
                                    boundp->current_alt_message_node = NULL;
                                    boundp->root_alt_message_node = NULL;
                                    boundp->alternativeparser = FALSE;
                                    boundp->alternative_message_node_created = FALSE;
                                }
                            }
#if DELETE_ME
                            if (!boundp) {
				bodyflags &= ~BODY_ATTACHED;
                            }
#endif                       
                            /* skip the MIME epilogue until the next section (or next message!) */
                            skip_mime_epilogue = TRUE;
			    multipartp = multipart_stack_pop(multipartp);
                            
                            *charsetsave='\0';
                            if (charset) {
                                free(charset);
                                charset = NULL;
                            }

			    if (alternativeparser
				&& !multipart_stack_has_type(multipartp, "multipart/alternative")) {
#ifdef NOTUSED
				struct body *next;
#endif

#if DEBUG_PARSE
				printf("We no longer have alternatives\n");
#endif

				/* we no longer have alternatives */
				alternativeparser = FALSE;
				/* reset the alternative variables (I think we can skip
				   this step without problems */
				alternative_weight = -1;
				alternative_bp = alternative_lp = NULL;
				alternative_lastfile_created = NO_FILE;
				alternative_file[0] =
				    alternative_lastfile[0] = '\0';
                                last_alternative_type[0] = '\0';
                                type[0] = '\0';
                                root_alt_message_node = current_alt_message_node = NULL;
#if DEBUG_PARSE
				printf("We DUMP the chosen alternative\n");
#endif

                                bp = lp = NULL;
                                /*
				if (bp != origbp)
				    origbp = append_body(origbp, &origlp, bp, TRUE);
                                */
				bp = origbp;
				lp = origlp;
				origbp = origlp = NULL;

				headp = NULL;
			    }
#if DEBUG_PARSE
			    if (boundp)
				printf("back %s\n", boundp->boundary_id);
			    else
				printf("back to NONE\n");

                            if (multipartp)
                                printf("current multipart: %s\n", multipart_stack_top_type(multipartp));
			    else
				printf("current multipart: NONE\n");
#endif
			}
			else {
			    /* we found the beginning of a new section */
			    skip_mime_epilogue = FALSE;

			    if (alternativeparser && !local_set_save_alts) {
				/*
				 * parsing another alternative, so we save the
				 * precedent values
				 */

                                /* JK: can we delete this? */
                                /*
				alternative_bp = bp;
				alternative_lp = lp;
                                */
				alternative_lastfile_created =
				    file_created;
				strcpy(alternative_lastfile,
				       alternative_file);
                                strncpy(last_alternative_type, type,
                                        sizeof(last_alternative_type) - 2);
                                /* make sure it's a NULL ending string if ever type > 128 */
                                last_alternative_type[sizeof(last_alternative_type) - 1] = '\0';
                                
				/* and now reset them */
				headp = bp = lp = NULL;
				alternative_file[0] = '\0';
                                type[0] = '\0';
			    }
			    else {
				att_counter++;
				if (alternativeparser && local_set_save_alts == 1) {
                                    /* JK: @@@ REVIEW THIS FOR WAI CONTENT. WE DON'T WANT
                                       TO USE <hr /> ANYMORE .. 
                                       set_save_alts NEEDS REVIEW AFTER OUR RECENT CHANGES 202305*/
				    bp = addbody(bp, &lp,
						 set_alts_text ? set_alts_text
						 : "<hr />",
						 BODY_HTMLIZED | bodyflags);
				}
			    }
#if DEBUG_PARSE
			    printf("mime parsing isinheader set to 1\n");
#endif
			    isinheader = 1;	/* back on a kind-of-header */
			    /* @@@ why are we changing the status of this variable? */
			    file_created = NO_FILE;	/* not created any file yet */
			}
			/* go back to the MIME attachment default mode */
			content = CONTENT_TEXT;
                        if (decode == ENCODE_BASE64) {
                            base64_decoder_state_free(b64_decoder_state);
                            b64_decoder_state = NULL;
                        }                                              
			decode = ENCODE_NORMAL;
			multilinenoend = FALSE;
                        *attachname = '\0';
                        
			/* reset related RFC 3676 state flags */
			textplain_format = FORMAT_FIXED;
			delsp_flag = FALSE;
			flowed_line = FALSE;
			quotelevel = 0;
			continue_previous_flow_flag = FALSE;

                        *charsetsave = '\0';
                        if(charset) {
                            free(charset);
                            charset = NULL;
                        }
                        
			if (-1 != binfile) {
			    close(binfile);
			    binfile = -1;
			}

			continue;
		    }
                }

		switch (decode) {
		case ENCODE_QP:
		    {
			char *p2 = mdecodeQP(fp, line, &data, &datalen, fpo);
			if (p2) {
			    if (set_txtsuffix) {
			        PushString(&raw_text_buf, line);
				line_buf[0] = 0;
				PushString(&raw_text_buf, p2);
			    }
			    free(p2);
			}
		    }
		    break;
		case ENCODE_BASE64:
                    datalen = base64_decode_stream(b64_decoder_state, line, newbuffer);
		    data = newbuffer;
		    break;
		case ENCODE_UUENCODE:
		    uudecode(NULL, line, newbuffer, &datalen, NULL);
		    data = newbuffer;
		    break;
		case ENCODE_NORMAL:
		    data = line;
		    break;
		default:
		    /* we have no clue! */
		    data = NULL;
		    break;
		}
#if DEBUG_PARSE
		printf("LINE %s\n", (content != CONTENT_BINARY) ? data : "<binary>");
#endif
		if (data) {
                    if (content == CONTENT_TEXT &&
                        charset && !strncasecmp (charset, "UTF-8", 5)) {
                        /* replace all unicode spaces with  ascii spaces,
                        ** as hypermail is using C-lib functions that don't
                        ** understand them (like isspace() and sscanf() ) */
                        i18n_replace_unicode_spaces(data, strlen(data));
#if DEBUG_PARSE
                        printf("LINE with ascii spaces: %s\n", data);
#endif
                    }

		    if ((content == CONTENT_TEXT) ||
			(content == CONTENT_HTML)) {
			if (decode > ENCODE_MULTILINED) {
			    /*
			     * This can be more than one resulting line,
			     * as the decoded the string may look like:
			     * "#!/bin/sh\r\n\r\nhelp() {\r\n echo 'Usage: difftree"
			     */
			    char *p = data;
			    char *n;
			    char store;

#if DEBUG_PARSE
			    printf("decode type %d\n", decode);
#endif

			    while ((n = strchr(p, '\n'))) {
				store = n[1];
				n[1] = 0;
#if DEBUG_PARSE
				printf("UNFOLDED %s", p);
#endif
				bp = addbody(bp, &lp, p,
					     (content == CONTENT_HTML ?
					      BODY_HTMLIZED : 0) |
					     (multilinenoend ?
					      BODY_CONTINUE : 0) |
					     bodyflags);
				multilinenoend = FALSE;	/* full line pushed */
				n[1] = store;
				p = n + 1;
			    }
			    if (strlen(p)) {
				/*
				 * This line doesn't really end here,
				 * we will get another line soon that
				 * should get appended!
				 */
#if DEBUG_PARSE
				printf("CONTINUE %s\n", p);
#endif
				bp = addbody(bp, &lp, p,
					     (content == CONTENT_HTML ?
					      BODY_HTMLIZED : 0) |
					     (multilinenoend ?
					      BODY_CONTINUE : 0) |
					     bodyflags);

				/*
				 * We want the next line to get appended to this!
				 */
				multilinenoend = TRUE;
			    }
			}
			else {
			  if (!isinheader && (textplain_format == FORMAT_FLOWED)) {
                              /* remove both space stuffing and quotes
                               * where applicable for f=f */
                              bodyflags |= BODY_DEL_SSQ;
                              flowed_line = rfc3676_handler (data, delsp_flag, &quotelevel,
                                                             &continue_previous_flow_flag);
                              if (continue_previous_flow_flag) {
                                  bodyflags |= BODY_CONTINUE;
                              } else  {
                                  bodyflags &= ~BODY_CONTINUE;
                                  if (flowed_line) {
                                      bodyflags |= BODY_FORMAT_FLOWED;
                                  } else {
                                      bodyflags &= ~BODY_FORMAT_FLOWED;
				  }
                              }
                              continue_previous_flow_flag = flowed_line;
			  } else {
                              bodyflags &= ~BODY_DEL_SSQ;
			  }
			  bp = addbody(bp, &lp, data,
				       (content == CONTENT_HTML ?
					BODY_HTMLIZED : 0) | bodyflags);
			}
#if DEBUG_PARSE
			printf("ALIVE?\n");
#endif
		    }
		    else if (content == CONTENT_BINARY) {

		        /* don't create the attachments of deleted files */
		        /* (JK: this seems like a good place to call emptydir() to remove
			   existing attachments) from deleted messages */
		        if (is_deleted && file_created == MAKE_FILE) {
			  file_created = MADE_FILE;
			}

			/* If there is no file created, we create and init one */
			if (file_created == MAKE_FILE) {
			    char *fname;
			    char *file = NULL;
			    char buffer[1024];

			    file_created = MADE_FILE;	/* we have, or at least we tried */

			    /* create the attachment directory if it doesn't exist */
			    if (att_dir == NULL) {
				/* first check the DIR_PREFIXER */
				trio_asprintf(&att_dir,"%s%c" DIR_PREFIXER "%04d",
					      dir, PATH_SEPARATOR, num);

				if (set_increment != -1)
				    check1dir(att_dir);
				/* If this is a repeated run on the same archive we already
				 * have HTML'ized, we risk extracting the same attachments
				 * several times and therefore we need to remove all the
				 * attachments currently present before we go ahead!
				 *(Daniel -- August 6, 1999) */
				/* jk: disabled it as it's not so necessary
				   as we have collision detection for attachment names
                                   and a safer mechanism when rebuilding archives to guarantee
                                   that the same attachment files and names are recreated
                                   after each rebuild run */
#if DEBUG_PARSE
				emptydir(att_dir);
#endif
				if (set_usemeta && set_increment != -1) {
				    /* make the meta dir where we'll store the meta info,
				       such as content-type */
				    trio_asprintf(&meta_dir, "%s%c" META_DIR,
						  att_dir, PATH_SEPARATOR);
				    check1dir(meta_dir);
				}
			    }

			    /* If the attachment has a name, we keep it and add the
			       current value of the counter, to guarantee that we
			       have a unique name. Otherwise, we use a fixed name +
			       the counter. We go thru all this trouble so that we
			       can easily regenerate the same archive, without breaking
			       any links */

			    if (att_counter > 99)
				att_binname = NULL;
			    else {
				if (set_filename_base)
				    create_attachname(attachname, sizeof(attachname));
				if (attachname[0])
				    fname = attachname;
				else
				    fname = FILE_SUFFIXER;
				if (!attachname[0] || inlist(att_name_list, fname))
                                    trio_asprintf(&att_binname, "%s%c%02d-%s",
                                                  att_dir, PATH_SEPARATOR,
                                                  att_counter, fname);
				else
                                    trio_asprintf(&att_binname, "%s%c%s",
                                                  att_dir, PATH_SEPARATOR,
                                                  fname);

				if (att_name_list == NULL)
				    att_name_list = att_name_last = (struct hmlist *)emalloc(sizeof(struct hmlist));
				else {
				  att_name_last->next = (struct hmlist *)emalloc(sizeof(struct hmlist));
				  att_name_last = att_name_last->next;
				}
				att_name_last->next = NULL;
				att_name_last->val = strsav(fname);

				/* JK: moved this one up */
				/* att_counter++; */
			    }

			    /*
                             * Saving of the attachments is being done
                             * inline as they are encountered. The
                             * directories must exist first...
                             */

#ifdef O_BINARY
#define OPENBITMASK O_WRONLY | O_CREAT | O_TRUNC | O_BINARY
#else
#define OPENBITMASK O_WRONLY | O_CREAT | O_TRUNC
#endif
			    if (att_binname) {
				binfile = open(att_binname, OPENBITMASK,
					       set_filemode);

#if DEBUG_PARSE
				printf("%4d open attachment %s\n", num, att_binname);
#endif
				if (-1 != binfile) {
				    chmod(att_binname, set_filemode);
				    if (set_showprogress)
					print_progress(num, lang
					       [MSG_CREATED_ATTACHMENT_FILE],
					       att_binname);
				    if (set_usemeta) {
					/* write the mime meta info */
					FILE *file_ptr;
					char *ptr;

					ptr = strrchr(att_binname, PATH_SEPARATOR);
					*ptr = '\0';
					trio_asprintf(&meta_filename, "%s%c%s"
						      META_EXTENSION,
						      meta_dir,
						      PATH_SEPARATOR,
						      ptr + 1);
					*ptr = PATH_SEPARATOR;
					file_ptr = fopen(meta_filename, "w");
					if (file_ptr) {
					    if (*type) {
						if (charset)
						    fprintf(file_ptr,
							    "Content-Type: %s; charset=\"%s\"\n",
							    type, charset);
						else
						    fprintf(file_ptr,
							    "Content-Type: %s\n",
							    type);
					    }
					    if (annotation_robot != ANNOTATION_ROBOT_NONE
                                                && set_userobotmeta) {
					      /* annotate the attachments using the experimental
						 google X-Robots-Tag HTTP header.
						 See https://developers.google.com/webmasters/control-crawl-index/docs/robots_meta_tag */
                                                char *value = NULL;
                                                
                                                if (annotation_robot == ANNOTATION_ROBOT_NO_FOLLOW)
                                                    value = "nofollow";
                                                else if (annotation_robot == ANNOTATION_ROBOT_NO_INDEX)
                                                    value = "noindex";
                                                else if (annotation_robot == (ANNOTATION_ROBOT_NO_FOLLOW | ANNOTATION_ROBOT_NO_INDEX))
                                                    value = "nofollow, noindex";
                                                fprintf(file_ptr,"X-Robots-Tag: %s\n", value);
					    }
					    fclose(file_ptr);
					    chmod(meta_filename, set_filemode);
					}
				    }
				    if (alternativeparser) {
					/* save the last name, in case we need to supress it */
					strncpy(alternative_file, att_binname,
						sizeof(alternative_file) -
						1);
                                        /* make sure it's a NULL ending string if ever type > 128 */
                                        alternative_file[sizeof(alternative_file) - 1] = '\0';
                                        /* save the last mime type to help deal with the
                                         * apple mail hack */
					strncpy(last_alternative_type, type,
						sizeof(last_alternative_type) - 2);
                                        /* make sure it's a NULL ending string if ever type > 128 */
                                        last_alternative_type[sizeof(last_alternative_type) - 1] = '\0';
                                    }

				}
				else {
				    if (alternativeparser) {
					/* save the last name, in case we need to supress it */
					alternative_file[0] = '\0';
                                        /* save the last mime type to  help deal with the apple
                                         * hack */
                                        last_alternative_type[0] = '\0';
                                    }
				}

				/* point to the filename and skip the separator */
				file = &att_binname[strlen(att_dir) + 1];

				/* protection against having a filename bigger than buffer */
				if (strlen(file) <= 500) {
				    char *desc;
                                    bool free_desc=FALSE;
				    char *sp;
				    struct emailsubdir *subdir;

				    if (description && description[0] != '\0'
                                        && !strisspace(description)) {
                                            desc = convchars(description, charset);
                                            free_desc = TRUE;
                                    }
				    else if (inline_force ||
					     inlinecontent(type))
				        desc =
					    attachname[0] ? attachname
					    : "picture";
				    else
					desc =
					    attachname[0] ? attachname
					    : "stored";

				    subdir = NULL;
				    if (set_msgsperfolder || set_folder_by_date) {
					struct emailinfo e;
					fill_email_dates(&e, date, fromdate,
							 NULL, NULL);
					subdir = msg_subdir(num,
							    set_use_sender_date
							    ? e.date
							    : e.fromdate);
				    }

				    if (inline_force ||
					inlinecontent(type)) {
					/* if we know our browsers can show this type of context
					   as-is, we make a <img> tag instead of <a href>! */
 				      if(set_inline_addlink){
 					char *created_link =
					  createlink(set_attachmentlink,
						     &att_dir[strlen(dir)
							      + 1],
						     file, num, type);
 					trio_snprintf(buffer, sizeof(buffer),
						      "<li>%s %s: <a href=\"%s%s\">%s</a><br />\n"
						      "<img src=\"%s%s%c%s\" alt=\"%s\" />\n"
						      "</li>\n",
						      type,
						      lang[MSG_ATTACHMENT],
						      subdir ? subdir->rel_path_to_top : "",
						      created_link, file,
						      subdir ? subdir->rel_path_to_top : "",
						      &att_dir[strlen(dir) + 1],
						      PATH_SEPARATOR, file,
						      desc);
 					free(created_link);
 				      } else {
					trio_snprintf(buffer, sizeof(buffer),
						      "<li>%s %s:<br />\n"
						      "<img src=\"%s%s%c%s\" alt=\"%s\" />\n"
						      "</li>\n",
						      type,
						      lang[MSG_ATTACHMENT],
						      subdir ? subdir->rel_path_to_top : "",
						      &att_dir[strlen(dir) + 1],
						      PATH_SEPARATOR, file,
						      desc);
				      }
				    } else {
					char *created_link =
					    createlink(set_attachmentlink,
						       &att_dir[strlen(dir)
								+ 1],
						       file, num, type);

					if ((sp = strchr(desc, '\n')) !=
					    NULL) *sp = '\0';

					trio_snprintf(buffer, sizeof(buffer),
						 "<li>%s %s: <a href=\"%s%s\">%s</a></li>\n",
						 type,
						 lang[MSG_ATTACHMENT],
						 subdir ? subdir->rel_path_to_top : "",
						 created_link, desc);
					free(created_link);
				    }
                                    att_link = strsav(buffer);
                                    att_comment_filename = strsav(file);

                                    /* use the correct condition to know we're not in
                                       a multipart/ message, just in a single message 
                                       that has non-inline content */
                                    if (!root_message_node && !boundary_id && !boundp) {
                                        /* Print attachment comment before attachment */
                                        /* add a SECTION to store all this info first */
                                        if (!append_bp)
                                            append_bp =
                                                addbody(append_bp, &append_lp,
                                                        NULL,
                                                        BODY_ATTACHMENT_LINKS | BODY_ATTACHMENT_LINKS_START | bodyflags);
                                        append_bp =
                                            addbody(append_bp, &append_lp, buffer,
                                                    BODY_HTMLIZED | BODY_ATTACHMENT_LINKS | bodyflags);
                                        trio_snprintf(buffer, sizeof(buffer),
                                                      "<!-- attachment=\"%.80s\" -->\n",
                                                      file);
                                        append_bp =
                                            addbody(append_bp, &append_lp, buffer,
                                                    BODY_HTMLIZED | BODY_ATTACHMENT_LINKS | bodyflags);
                                    }

                                    if (free_desc) {
                                        free(desc);
                                    }
				}
			    }

			    inline_force = FALSE;
			    attachname[0] = '\0';

			    if (att_binname && (binfile != -1))
				content = CONTENT_BINARY;
			    else
				content = CONTENT_UNKNOWN;
			}
		    }
                    
		    if (-1 != binfile) {
			if (datalen < 0)
			    datalen = strlen(data);

			write(binfile, data, datalen);
		    }
		}

		if (ENCODE_QP == decode) {
		    free(data);	/* this was allocatd by mdecodeQP() */
                }
	    }
	}
    }
    if(set_append && fclose(fpo)) {
	progerr("Can't close \"mbox\"");
    }

    if (!isinheader || readone) {

#ifdef CHARSETSP

#ifdef HAVE_ICONV        
        /* THE PREFERED CHARSET ALGORITHM ... AGAIN */
        if (root_message_node) {
            /* multipart message */
            if (charset) {
                free(charset);
            }
            prefered_charset = strsav(message_node_get_charset(root_message_node));
            
        } else {
            if (!charset){
                if (*charsetsave!=0){
#ifdef DEBUG_PARSE
                    printf("put charset from subject header..\n");
#endif       
                    charset=strsav(charsetsave);
                } else {
                    /* default charset is US-ASCII */
                    charset=strsav(set_default_charset);
#ifdef DEBUG_PARSE
                    fprintf(stderr, "found no charset for body, using default_charset %s.\n", set_default_charset);
#endif                   
                }
            } else {
                /* if body is us-ascii but subject is not,
                   try to use subject's charset. */
                if (strncasecmp(charset,"us-ascii",8)==0){
                    if (*charsetsave!=0 && strcasecmp(charsetsave,"us-ascii")!=0){
                        free(charset);
                        charset=strsav(charsetsave);
                    }
                }
            }
        }
#endif /* HAVE_ICONV */
#endif /* CHARSETSP */
        
	if (!hassubject)
	    subject = NOSUBJECT;

	if (!hasdate)
	    date = NODATE;

	if (!inreply)
	    inreply = oneunre(subject);

	/* control the use of format and delsp according to RFC2646 */
	if ((textplain_format == FORMAT_FLOWED)
	    && (content != CONTENT_TEXT
                || (content == CONTENT_TEXT && strcasecmp (type, "text/plain")))) {
	  /* format flowed only allowed on text/plain */
	  textplain_format = FORMAT_FIXED;
	}

	if (textplain_format == FORMAT_FIXED && delsp_flag) {
	  /* delsp only accepted for format=flowed */
	  delsp_flag = FALSE;
	}

        if (bp || lp) {
            /* if we reach this condition, it means the message is missing one or
               more mime boundary ends. Closing the current active node should fix
               this */
            if (current_message_node) {
                current_message_node =
                    message_node_mimetest(current_message_node,
                                          bp, lp, charset, charsetsave,
                                          type,
                                          (boundp) ? boundp->boundary_id : NULL,
                                          boundary_id,
                                          att_binname,
                                          meta_filename,
                                          att_link,
                                          att_comment_filename,
                                          attachment_rfc822,
                                          message_node_skip_status(file_created,
                                                                   content,
                                                                   type));
#if DEBUG_PARSE_MSGID_TRACE
                current_message_node->msgid = strsav(msgid);
#endif
                
            }
        }

        /* use heuristics to choose the charset for the whole parsed
         * message 2 */
        if (root_message_node) {
            prefered_charset = message_node_get_charset(root_message_node);
        } else {
            prefered_charset = _single_content_get_charset(charset, charsetsave);
        }

        if (prefered_charset && set_replace_us_ascii_with_utf8
            && !strncasecmp(prefered_charset, "us-ascii", 8)) {
            if (set_debug_level) {
                fprintf(stderr, "Replacing content charset %s with UTF-8\n",
                        prefered_charset);
            }                                    
            free(prefered_charset);
            prefered_charset = strsav("UTF-8");
        }
        
        if (set_debug_level) {
            fprintf(stderr, "Message will be stored using charset %s\n", prefered_charset);
        }
        
	if (root_message_node) {
            /* multipart message */
            
            if (set_debug_level == DEBUG_DUMP_ATT
                || set_debug_level == DEBUG_DUMP_ATT_VERBOSE) {
                message_node_dump (root_message_node);
                progerr("exiting");
            }
            
            bp = message_node_flatten (&lp, root_message_node);
            /* free memory allocated to message nodes */
            message_node_free(root_message_node);
            root_message_node = current_message_node = NULL;
            root_alt_message_node = current_alt_message_node = NULL;
	} else {
            /* it was not a multipart message, remove all empty lines
               at the end of the message */
            while (rmlastlines(bp));
        }


	if (append_bp && append_bp != bp) {
            append_bp = addbody(append_bp, &append_lp,
                                NULL,
                                BODY_ATTACHMENT_LINKS | BODY_ATTACHMENT_LINKS_END);

            /*
              bp = append_body(bp, &lp, append_bp, TRUE);
            */
            lp = quick_append_body(lp, append_bp);
	    append_bp = append_lp = NULL;
	}

	strcpymax(fromdate, dp ? dp : "", DATESTRLEN);

	emp = addhash(num, date, namep, emailp, msgid, subject, inreply,
		      fromdate, prefered_charset, NULL, NULL, bp);
	if (emp) {
	    emp->exp_time = exp_time;
	    emp->is_deleted = is_deleted;
	    emp->annotation_robot = annotation_robot;
	    emp->annotation_content = annotation_content;
	    if (insert_in_lists(emp, require_filter,
				require_filter_len + require_filter_full_len))
	        ++num_added;
	    if (set_txtsuffix && set_increment != -1)
	        write_txt_file(emp, &raw_text_buf);
	    num++;
	} else {
            /* addhash refused to add this message, maybe it's a duplicate id
               or it failed one of its tests.
               We delete the body to avoid and associated attachments to 
               avoid memory leaks */
            free_body(bp);
            bp = NULL;
            
            if (att_dir != NULL) {
                emptydir(att_dir);
                rmdir(att_dir);
            }
        }
        
	/* @@@ if we didn't add the message, we should consider erasing the attdir
	   if it's there */
        if (att_binname) {
            free(att_binname);
            att_binname = NULL;
        }
        if (meta_filename) {
            free(meta_filename);
            meta_filename = NULL;
        }
        if (att_link) {
            free(att_link);
            att_link = NULL;
        }
        if (att_comment_filename) {
            free(att_comment_filename);
            att_comment_filename = NULL;
        }
	if (hasdate) {
	    free(date);
            date = NULL;
        }
	if (hassubject) {
	    free(subject);
            subject = NULL;
        }
	if (charset) {
	    free(charset);
	    charset = NULL;
	}
	if (charsetsave){
            *charsetsave = 0;
	}
        if (prefered_charset) {
            free(prefered_charset);
            prefered_charset = NULL;
        }
	if (msgid) {
	    free(msgid);
	    msgid = NULL;
	}
	if (inreply) {
	    free(inreply);
	    inreply = NULL;
	}
	if (namep) {
	    free(namep);
	    namep = NULL;
	}
	if (emailp) {
	    free(emailp);
	    emailp = NULL;
	}

	/* reset the status counters */
	/* @@ verify we're doing it everywhere */
	bodyflags = 0;		/* reset state flags */

	/* reset related RFC 3676 state flags */
	textplain_format = FORMAT_FIXED;
	delsp_flag = FALSE;
	flowed_line = FALSE;
	quotelevel = 0;
	continue_previous_flow_flag = FALSE;

	/* go back to default mode: */
	content = CONTENT_TEXT;
        if (ENCODE_BASE64 == decode) {
            base64_decoder_state_free(b64_decoder_state);
            b64_decoder_state = NULL;
        }                              
	decode = ENCODE_NORMAL;
	Mime_B = FALSE;
        skip_mime_epilogue = FALSE;
	headp = NULL;
	multilinenoend = FALSE;
	if (att_dir) {
	    free(att_dir);
	    att_dir = NULL;
	}
	if (set_usemeta && meta_dir) {
	    free(meta_dir);
	    meta_dir = NULL;
	}
	att_counter = 0;
        if (att_name_list) {
            hmlist_free (att_name_list);
            att_name_list = NULL;
        }
	description = NULL;
        *attachname = '\0';
        
	if (parse_multipart_alternative_force_save_alts) {
            parse_multipart_alternative_force_save_alts = 0;

#if DEBUG_PARSE
            printf("Applemail_hack resetting parse_multipart_alternative_force_save_alts\n");
#endif
            if (applemail_old_set_save_alts != -1) {
                local_set_save_alts = applemail_old_set_save_alts;
                applemail_old_set_save_alts = -1;
#if DEBUG_PARSE
                printf("Applemail_hack resetting save_alts to %d\n", local_set_save_alts);
#endif
            }
        }

	/* by default we have none! */
	hassubject = 0;
	hasdate = 0;
        message_headers_parsed = FALSE;

	annotation_robot = ANNOTATION_ROBOT_NONE;
	annotation_content = ANNOTATION_CONTENT_NONE;
    }
    if (require_filter) free(require_filter);

    if (set_showprogress && !readone)
	print_progress(num, lang[MSG_ARTICLES], NULL);
#if DEBUG_PARSE
    printf("\b\b\b\b%4d %s.\n", num, lang[MSG_ARTICLES]);
#endif

    /* kpm - this is to prevent the closing of std and hypermail crashing
     * if the input is from stdin
     */
    if (fp != stdin)
	fclose(fp);

#ifdef FASTREPLYCODE
    threadlist_by_msgnum = (struct reply **)emalloc((num + 1)*sizeof(struct reply *));
    {
	int i;
	for (i = 0; i <= num; ++i)
	    threadlist_by_msgnum[i] = NULL;
    }
#endif
    if (num > max_msgnum)
	max_msgnum = num - 1;
    crossindex();
    threadlist = NULL;
    printedthreadlist = NULL;
    crossindexthread1(datelist);

#if DEBUG_THREAD
    {
	struct reply *r;
	r = threadlist;
	fprintf(stderr, "START of threadlist after crossindexthread1\n");
	fprintf(stderr, "- msgnum frommsgnum maybereply msgid\n");
	while (r != NULL) {
	    if (r->data == NULL) {
		fprintf(stderr, "- XX %d %d XX\n",
			r->frommsgnum, r->maybereply);
	    }
	    else {
		fprintf(stderr, "- %d %d %d '%s'\n",
			r->data->msgnum,
			r->frommsgnum, r->maybereply, r->data->msgid);
	    }
	    r = r->next;
	}
	fprintf(stderr, "END of threadlist after crossindexthread1\n");
    }
#endif

    /* can we clean up a bit please... */
    
    if (printedthreadlist) {
        printed_free(printedthreadlist);
        printedthreadlist = NULL;
    }
    
    boundary_stack_free(boundp);
    multipart_stack_free(multipartp);

    if(charsetsave){
      free(charsetsave);
    }

    if (set_debug_level == DEBUG_DUMP_BODY) {
        dump_mail(0, num_added);
    }

    return num_added;			/* amount of mails read */
}

static void check_expiry(struct emailinfo *emp)
{
    time_t email_time;
    const char *option = "expires";
    if (!emp->is_deleted) {
        if (emp->exp_time != -1 && emp->exp_time < time(NULL))
	    emp->is_deleted = FILTERED_EXPIRE;
	email_time = emp->fromdate;
	if (email_time == -1)
	    email_time = emp->date;
	if (email_time != -1 && set_delete_older
	    && email_time < convtoyearsecs(set_delete_older)) {
	    emp->is_deleted = FILTERED_OLD;
	    option = "delete_older";
	}
	if (email_time != -1 && set_delete_newer
	    && email_time < convtoyearsecs(set_delete_newer)) {
	    emp->is_deleted = FILTERED_NEW;
	    option = "delete_newer";
	}
	if (emp->is_deleted)
	    printf("message %d deleted under option %s. msgid: %s\n",
		   emp->msgnum+1, option, emp->msgid);
    }
}

int parse_old_html(int num, struct emailinfo *ep, int parse_body,
		   int do_insert, struct reply **replylist_tmp, int cmp_msgid)
{
    char line[MAXLINE];
    char *name = NULL;
    char *email = NULL;
    char *date = NULL;
    char *msgid = NULL;
    char *subject = NULL;
    char *inreply = NULL;
    char *fromdate = NULL;
    char *charset = NULL;
    char *isodate = NULL;
    char *isofromdate = NULL;
    char command[100];
    char *valp;
    char legal = FALSE;
    int reply_msgnum = -1;
    long exp_time = -1;
    int is_deleted = 0;
    int num_added = 0;
    struct body *bp = NULL;
    struct body *lp = NULL;
    int msgids_are_same = 0;

    struct emailsubdir *subdir = ep ? ep->subdir : msg_subdir(num, 0);
    char *filename;

    FILE *fp;

    char inreply_start[256];
    static char *inreply_start_old = "<li><span class=\"heading\">In reply to</span>: <a href=\"";

    if (set_nonsequential && !msgnum_id_table[num])
      return 0;

    if (set_linkquotes) {
        trio_snprintf(inreply_start, sizeof(inreply_start),
                      "<span class=\"heading\">%s</span>: <a href=\"", lang[MSG_IN_REPLY_TO]);
    }

    /* prepare the name of the file that stores the message */
    if (set_nonsequential)
        trio_asprintf(&filename, "%s%s%s.%s", set_dir,
                      subdir ? subdir->subdir : "",
                      msgnum_id_table[num],
                      set_htmlsuffix);
    else
        trio_asprintf(&filename, "%s%s%.4d.%s", set_dir,
                      subdir ? subdir->subdir : "", num, set_htmlsuffix);

    /*
     * fromdate == <!-- received="Wed Jun  3 10:12:00 1998 CDT" -->
     * date     == <!-- sent="Wed, 3 Jun 1998 10:12:07 -0500 (CDT)" -->
     * name     == <!-- name="Kent Landfield" -->
     * email    == <!-- email="kent@landfield.com" -->
     * subject  == <!-- subject="Test of the testmail mail address." -->
     * msgid    == <!-- id="199806031512.KAA22323@landfield.com" -->
     * inreply  == <!-- inreplyto="" -->
     *
     * New for 2b10:
     * charset  == <!-- charset="iso-8859-2" -->
     *
     * New for 2b18:
     * isofromdate == <!-- isoreceived="19980603101200" -->
     * isodate     == <!-- isosent="19980603101207" -->
     */

    if ((fp = fopen(filename, "r")) != NULL) {
	while (fgets(line, sizeof(line), fp)) {

	    if (1 == sscanf(line, "<!-- %99[^=]=", command)) {
		if (!strcasecmp(command, "received"))
		    fromdate = getvalue(line);
		else if (!strcasecmp(command, "sent"))
		    date = getvalue(line);
		else if (!strcasecmp(command, "name")) {
                    valp = getvalue(line);
                    if (valp) {
                        name = unconvchars(valp);
                        free(valp);
                    }
                }
		else if (!strcasecmp(command, "email")) {
                    char *tmp = getvalue(line);
                    if (tmp) {
                        valp = unconvchars(line);
                        free (tmp);
                        if (valp) {
                            email = unobfuscate_email_address(valp);
                            free(valp);
                        }
                    }
                }
		else if (!strcasecmp(command, "subject")) {
		    valp = getvalue(line);
		    if (valp) {
			subject = unconvchars(valp);
			free(valp);
		    }
		}
		else if (!strcasecmp(command, "id")) {
                    valp = getvalue(line);
                    if (valp) {
                        char *raw_msgid = unconvchars(valp);
                        free(valp);
                        msgid = unspamify(raw_msgid);
                        if (raw_msgid) {
                            free(raw_msgid);
                        }
                    }
		    if (msgid && !strstr(line,"-->") && set_linkquotes)
		        msgid = NULL;/* old version of Hypermail wrote junk? */
		}
		else if (!strcasecmp(command, "charset"))
		    charset = getvalue(line);
		else if (!strcasecmp(command, "isosent"))
		    isodate = getvalue(line);
		else if (!strcasecmp(command, "isoreceived"))
		    isofromdate = getvalue(line);
		else if (!strcasecmp(command, "expires")) {
		    valp = getvalue(line);
		    if (valp) {
			exp_time = strcmp(valp, "-1") ? iso_to_secs(valp) : -1;
			free(valp);
		    }
		}
		else if (!strcasecmp(command, "isdeleted")) {
		    valp = getvalue(line);
		    if (valp) {
			is_deleted = atoi(valp);
			free(valp);
		    }
		}
		else if (!strcasecmp(command, "inreplyto")) {
		    char *raw_msgid = getvalue(line);
		    if (raw_msgid) {
                        valp = unspamify(raw_msgid);
                        free(raw_msgid);
                    } else {
                        valp = NULL;
                    }
		    if (valp) {
			inreply = unconvchars(valp);
			free(valp);
		    }
		}
		else if (!strcasecmp(command, "body")) {
		    /*
		     * When we reach the mail body, we know we've got all the
		     * headers there were!
		     */
		    if (parse_body) {
			while (fgets(line, MAXLINE, fp)) {
			    char *ptr;
			    char *line2;
			    if (!strcmp(line,"<!-- body=\"end\" -->\n"))
			        break;
#if 0
			    if (!strcmp(line,"<p><!-- body=\"end\" -->\n"))
			        break;
#endif
			    line2 = remove_hypermail_tags(line);
			    if (line2) {
			        if (!bp && *line2 != '\n') {
				    bp = addbody(bp, &lp, "\n", 0);
				    if (ep != NULL)
				        ep->bodylist = bp;
				}
				ptr = unconvchars(line2);
				bp = addbody(bp, &lp, ptr ? ptr : "", 0);
				if (ep != NULL && !ep->bodylist->line[0])
				    ep->bodylist = bp;
				if(0) fprintf(stderr,"addbody %p %d from %s",
					      bp, !ep->bodylist->line[0], ptr);
				free(ptr);
				if (set_linkquotes && !inreply) {
				    char *new_inreply = getreply(line2);
				    if (!*new_inreply) free(new_inreply);
				    else inreply = new_inreply;
				}
				free(line2);
			    }
			}
		    }
		    if (!bp)
			bp = addbody(bp, &lp, "\0", 0);
		    fclose(fp);
		    legal = TRUE;	/* with a body tag we consider this a valid syntax */
		    break;
		}
	    }
	    else if (set_linkquotes) {
		char *ptr;
		if ((ptr = strcasestr(line, inreply_start)) != NULL)
		    reply_msgnum = atoi(ptr + strlen(inreply_start));
		else if ((ptr = strstr(line, inreply_start_old)) != NULL)
		    reply_msgnum = atoi(ptr + strlen(inreply_start_old));
	    }
	}
    }
    else if (cmp_msgid) {
        free(filename);
	return -1;
    }
    
    if (legal) {	    /* only do this if the input was reliable */
	struct emailinfo *emp;

#if HAVE_ICONV
	if (charset){
	  char *tmpptr;
	  size_t tmplen=0;
	  tmpptr=subject;
	  subject=i18n_convstring(tmpptr,charset,"UTF-8",&tmplen);
	  if(tmpptr)
	    free(tmpptr);
	  tmpptr=name;
	  name=i18n_convstring(tmpptr,charset,"UTF-8",&tmplen);
	  if(tmpptr)
	    free(tmpptr);
	}
#endif
	if (replylist_tmp == NULL || !do_insert)
	    emp = ep;
	else
	    emp = addhash(num, date ? date : NODATE,
			  name, email, msgid, subject, inreply,
			  fromdate, charset, isodate, isofromdate, bp);
	if (cmp_msgid) {
            /* at this point, special xml chars have been escaped in msgid,
               but not in ep->msgid. We temporarily unconvert them so that we
               can do the comparition */
            char *tmpmsgid = unconvchars(msgid);
            
	    msgids_are_same = !strcmp(ep->msgid, tmpmsgid);
            free(tmpmsgid);
        }
	if (emp != NULL && replylist_tmp != NULL) {
	    if (do_insert) {
	        emp->exp_time = exp_time;
		emp->is_deleted = is_deleted;
		check_expiry(emp);
		if (insert_in_lists(emp, NULL, 0))
		    ++num_added;
	    }

	    if(set_linkquotes && reply_msgnum != -1) {
#ifdef FASTREPLYCODE
		struct emailinfo *email2;
		if (hashnumlookup(reply_msgnum, &email2))
		    *replylist_tmp = addreply2(*replylist_tmp, email2, emp,
					       0, NULL);
#else
		*replylist_tmp = addreply(*replylist_tmp, reply_msgnum,
					  emp, 0, NULL);
#endif
	    }
	}
    }
    if (charset) {
	free(charset);
    }
    if (name) {
	free(name);
    }
    if (subject) {
	free(subject);
    }
    if (msgid) {
	free(msgid);
    }
    if (inreply) {
	free(inreply);
    }
    if (fromdate) {
	free(fromdate);
    }
    if (date) {
	free(date);
    }
    if (email) {
	free(email);
    }
    if (isodate) {
	free(isodate);
    }
    if (isofromdate) {
	free(isofromdate);
    }
    free(filename);
    
    free_body(bp);
#if 0
    if (bp != NULL) {		/* revisit me */
	if (bp->line)
	    free(bp->line);
	free(bp);
    }
#endif
    return (cmp_msgid ? msgids_are_same : num_added);
}

/*
** All this does is get all the relevant header information from the
** comment fields in existing archive files. Everything is loaded into
** structures in the exact same way as if articles were being read from
** stdin or a mailbox.
**
** Return the number of mails read.
*/

static int loadoldheadersfrommessages(char *dir, int num_from_gdbm)
{
    int num = 0;
    int num_added = 0;
    int max_num;
    struct emailinfo *e0 = NULL;

    struct reply *replylist_tmp = NULL;
    int first_read_body = set_startmsgnum;

    if (num_from_gdbm != -1)
      max_num = num_from_gdbm - 1;
    else if (set_nonsequential)
      max_num = find_max_msgnum_id();
    else
      max_num = find_max_msgnum();

    if (max_num > max_msgnum)
	max_msgnum = max_num;
    if (set_searchbackmsgnum) {
	first_read_body = max_num - set_searchbackmsgnum;
	if (first_read_body < set_startmsgnum)
	    first_read_body = set_startmsgnum;
	if (num_from_gdbm != -1)
	    num = first_read_body;
    }
#if 0
    else if (set_searchbackmsgnum && set_increment) {
	int jump = 1000;	 /* search for biggest message number */
	while (jump && first_read_body >= 0) {
	    subdir = msg_subdir(first_read_body, 0);
	    trio_asprintf(&filename,"%s%s%.4d.%s", set_dir,
			  subdir ? subdir->subdir : "",
			  first_read_body, set_htmlsuffix);
	    if ((fp = fopen(filename, "r")) != NULL) {
	        fclose(fp);
		if (jump < 0) jump = -jump/2;
		first_read_body += jump;
	    }
	    else {
	        if (jump > 0) jump = -jump/2;
		first_read_body += jump;
	    }
	    free(filename);
	    free(subdir);
	}
	first_read_body -= set_searchbackmsgnum + 1;
    }
#endif
    if (set_folder_by_date) {
	if (!num_from_gdbm)
	    return 0;
#ifdef GDBM
	if (set_usegdbm && !hashnumlookup(first_read_body, &e0)
	    && set_startmsgnum == 0 && first_read_body == 0
	    && num_from_gdbm != -1 && hashnumlookup(1, &e0)) {
	    /* kludge to handle old archives that mistakenly started with 0001 */
	    first_read_body = 1;
	}
#endif
	if (!hashnumlookup(first_read_body, &e0)) {
#ifdef GDBM
	    if (set_usegdbm) {
	        if (num_from_gdbm == -1) {
		    if (is_empty_archive())
		        return 0;
                    trio_snprintf(errmsg, sizeof(errmsg),
                                  "Error: This archive does not appear to be empty, "
                                  "and it has no gdbm file\n(%s). If you want to "
                                  "use incremental updates with the folder_by_date\n"
                                  "option, you must start with an empty archive or "
                                  "with an archive\nthat was generated using the "
                                  "usegdbm option.", GDBM_INDEX_NAME);
		}
		else
                    trio_snprintf(errmsg, sizeof(errmsg),
                                  "Error set_folder_by_date msg %d num_from_gdbm %d",
                                  first_read_body, num_from_gdbm);
	    }
	    else
                trio_snprintf(errmsg, sizeof(errmsg), "folder_by_date with incremental update requires usegdbm option");
#else
                trio_snprintf(errmsg, sizeof(errmsg),
                              "folder_by_date requires usegdbm option"
                              ". gdbm support has not been compiled into this"
                              " copy of hypermail. You probably need to install"
                              "gdbm and rerun configure.");
#endif
                progerr(errmsg);
	}
    }

    if (num_from_gdbm == -1)
        authorlist = subjectlist = datelist = NULL;

#ifdef WANTDUPMESSAGES
    if (set_showprogress)
	printf("%s...\n", lang[MSG_READING_OLD_HEADERS]);
#endif


    /* Strategy: loop on files, opening each and copying the header comments
     * into dynamically-allocated memory, then saving if it's not corrupt. */

    if (set_nonsequential)
      /* read the msgid to msgnum table */
      msgnum_id_table = read_msgnum_id_table (max_num);

    while (num <= max_num) {
	struct emailinfo *ep0 = NULL;
	int parse_body = (set_linkquotes && num >= first_read_body);
	if (num_from_gdbm != -1 || set_folder_by_date) {
	    if (!hashnumlookup(num, &ep0)) {
	        if (++num > max_num)
		    break;
	        continue;
	    }
	}
	num_added += parse_old_html(num, ep0, parse_body, num_from_gdbm == -1,
				    &replylist_tmp, 0);

	num++;

	if (!(num % 10) && set_showprogress) {
	    printf("\r%4d", num);
	    fflush(stdout);
	}
    }

    if (set_nonsequential)
      {
	/* free the msgnum_id_table */
	free_msgnum_id_table (msgnum_id_table, max_num);
	msgnum_id_table = NULL;
      }

#ifdef WANTDUPMESSAGES
    if (set_showprogress)
	printf("\b\b\b\b%4d %s.\n", num, lang[MSG_ARTICLES]);
#endif

    if (set_linkquotes)
	set_alt_replylist(replylist_tmp);

    return num_added;
} /* end loadoldheadersfrommessages() */

/*
** Load message summary information from a GDBM index.
*/
#ifdef GDBM

int loadoldheadersfromGDBMindex(char *dir, int get_count_only)
{
      char *indexname;
      GDBM_FILE gp;
      int num;
      int num_added = 0;
      int old_delete_level = -1;

      if (!get_count_only)
	authorlist = subjectlist = datelist = NULL;

      /* Use gdbm performance hack: instead of opening each and
       * every .html file to get the comment information, get it
       * from a gdbm index, where the key is the message number and
       * the content is a string containing the values separated by
       * nullchars, in this order:
       *   fromdate
       *   date
       *   name
       *   email
       *   subject
       *   inreply
       *   charset      v2.0
       *   isofromdate  v2.0
       *   isodate      v2.0
       */

      trio_asprintf(&indexname, (dir[strlen(dir)-1] == '/') ? "%s%s" : "%s/%s",
		    dir, GDBM_INDEX_NAME);

      if ((gp = gdbm_open(indexname, 0, GDBM_READER, 0, 0))) {

	/* we _can_ read the index */

	datum content;
	datum key;
	int max_num;

	key.dptr = "delete_level";
	key.dsize = strlen(key.dptr);
	content = gdbm_fetch(gp, key);
	if (content.dptr)
	    old_delete_level = atoi(content.dptr);

	key.dptr = (char *) &num;
	key.dsize = sizeof(num);

	num = -1;
	content = gdbm_fetch(gp, key);
	if (!content.dptr)
	    max_num = -1;
	else
	    max_num = atoi(content.dptr);
	if (get_count_only) {
	    gdbm_close(gp);
	    return max_num;
	}

	for(num = 0; max_num == -1 || num <= max_num; num++) {
	  char *dp, *dp_end;
	  char *name=NULL;
	  char *email=NULL;
	  char *date=NULL;
	  char *msgid=NULL;
	  char *subject=NULL;
	  char *inreply=NULL;
	  char *fromdate=NULL;
	  char *charset=NULL;
	  char *isodate=NULL;
	  char *isofromdate=NULL;
	  long exp_time = -1;
	  int is_deleted = 0;
	  struct emailinfo *emp;
	  struct body *bp = NULL;
	  struct body *lp = NULL;
	  bp = addbody(bp, &lp, "\0", 0);

	  content = gdbm_fetch(gp, key);
	  if(!(dp = content.dptr)) {
	      if (max_num == -1) /* old file where gaps in nums not legal */
		  break;	 /* must be at end */
	      continue;
	  }
	  dp_end = dp + content.dsize;
	  fromdate = dp;
	  dp += strlen(dp) + 1;
	  date = dp;
	  dp += strlen(dp) + 1;
	  name = dp;
	  dp += strlen(dp) + 1;
	  email = dp;
	  dp += strlen(dp) + 1;
	  subject = unconvchars(dp);
	  dp += strlen(dp) + 1;
	  msgid = dp;
	  dp += strlen(dp) + 1;
	  inreply = unconvchars(dp);
	  dp += strlen(dp) + 1;
	  charset = dp;
	  dp += strlen(dp) + 1;
	  isofromdate = dp;
	  dp += strlen(dp) + 1;
	  isodate = dp;
	  dp += strlen(dp) + 1;
	  if (dp < dp_end) {
	      exp_time = iso_to_secs(dp);
	      if (!*dp) exp_time = -1;
	      dp += strlen(dp) + 1;
	  }
	  if (dp < dp_end) {
	      is_deleted = atoi(dp);
	      dp += strlen(dp) + 1;
	  }

	  if ((emp = addhash(num, date, name, email, msgid, subject, inreply,
			   fromdate, charset, isodate, isofromdate, bp))) {
	      emp->exp_time = exp_time;
	      emp->is_deleted = is_deleted;
	      emp->deletion_completed = old_delete_level;
	      check_expiry(emp);
	      if (insert_in_lists(emp, NULL, 0))
		  ++num_added;
	      if (num == max_num) {
		  char *filename = articlehtmlfilename(emp);
		  if (!isfile(filename) && !is_deleted) {
		      trio_snprintf(errmsg, sizeof(errmsg),
			       "%s \"%s\". If you deleted files,"
			       " you need to delete the gdbm file %s as well.",
			       lang[MSG_CANNOT_OPEN_MAIL_ARCHIVE],
			       filename, indexname);
		      progerr(errmsg);
		  }
		  free(filename);
	      }
	  }
	  free(subject);
	  free(inreply);
#if 0
	  if(bp) {
	      if (bp->line)
		  free(bp->line);
	      free(bp);
	  }
#endif

	  if (!(num % 10) && set_showprogress) {
	    printf("\r%4d", num);
	    fflush(stdout);
	  }

	} /* end loop on messages */

	gdbm_close(gp);
	if (set_linkquotes)
	    loadoldheadersfrommessages(dir, num);
      } /* end case of able to read gdbm index */

      else {
	struct emailinfo *emp;

	if (get_count_only)
	    return 0;
	/* can't read?  create. */

	if (set_showprogress)
	  printf(lang[MSG_CREATING_GDBM_INDEX]);
	num = loadoldheadersfrommessages(dir, -1);

	if(!(gp = gdbm_open(indexname, 0, GDBM_NEWDB, 0600, 0))){

	  /* Serious problem here: can't create! So, just muddle on. */

	  if (set_showprogress)
	    printf(lang[MSG_CANT_CREATE_GDBM_INDEX]);
	  return num;
	}

	/* Can create new; now, populate it */

	for (num = 0; hashnumlookup(num, &emp); num++) {
	    togdbm((void *) gp, emp);
	}
	gdbm_close(gp);

      } /* end case of could not read gdbm index */

      free(indexname);

      return num_added;

} /* end loadoldheadersfromGDBMindex() */
#endif

/* All this does is get all the relevant header information.
** Everything is loaded into structures in the exact same way as if
** articles were being read from stdin or a mailbox.
**
** Return the number of mails read.  */

int loadoldheaders(char *dir)
{
  int num;

  if (set_showprogress)
    printf("%s...\n", lang[MSG_READING_OLD_HEADERS]);
#ifdef GDBM
  if(set_usegdbm)
    num = loadoldheadersfromGDBMindex(dir, 0);
  else
#endif
    num = loadoldheadersfrommessages(dir, -1);

  if (set_showprogress)
    printf("\b\b\b\b%4d %s.\n", num, lang[MSG_ARTICLES]);

  return num;

} /* end loadoldheaders() */


/*
** Adds a "Next:" link in the proper article, after the archive has been
** incrementally updated.
*/

void fixnextheader(char *dir, int num, int direction)
{
    char *filename;
    char line[MAXLINE];
    struct emailinfo *email;

    struct body *bp, *cp, *dp = NULL, *lp = NULL;
    int ul;
    FILE *fp;
    char *ptr;
    struct emailinfo *e3 = NULL;

    dp = NULL;
    ul = 0;

    if ((e3 = neighborlookup(num, direction)) != NULL
	&& (email = neighborlookup(num-1, 1)) != NULL)
	filename = articlehtmlfilename(e3);
    else
	return;
    bp = NULL;
    fp = fopen(filename, "r");
    if (fp) {
	while ((fgets(line, MAXLINE, fp)) != NULL)
	    bp = addbody(bp, &lp, line, 0);
    }
    else
	return;
    fclose(fp);

    cp = bp;			/* save start of list to free later */


    fp = fopen(filename, "w+");
    if (fp) {
#ifdef HAVE_ICONV
        char *numsubject,*numname;
        numsubject=i18n_utf2numref(email->subject,1);
        numname=i18n_utf2numref(email->name,1);
#endif        
	while (bp) {
	    if (!strncmp(bp->line, "<!-- emptylink=", 15)) {
	      /* JK: just skip this line and the following which is just our
	       empty marker. */
	      bp = bp->next;
	      bp = bp->next;
	      continue;
	    }
	    fprintf(fp, "%s", bp->line);

	    if (!strncmp(bp->line, "<!-- unext=", 11)) {
                if (email) {
                    fprintf(fp, "<li><a href=\"%s\">%s</a></li>\n",
                            msg_href (email, e3, FALSE),
                            lang[MSG_NEXT_MESSAGE]);
                }
	    }
	    else if (!strncmp(bp->line, "<!-- lnext=", 11)) {
#ifdef HAVE_ICONV
                ptr = strsav(numsubject);
#else
                ptr = convchars(email->subject, email->charset);
#endif
                fprintf(fp, "<li><span class=\"heading\">%s</span>: ", lang[MSG_NEXT_MESSAGE]);
                fprintf(fp, "<a href=\"%s\">%s: \"%s\"</a></li>\n",
                        msg_href(email, e3, FALSE),
#ifdef HAVE_ICONV
                        numname, ptr ? ptr : "");
#else
                        email->name, ptr ? ptr : "");
#endif
                if (ptr)
                    free(ptr);
            }
            /* 2021/10/04: this one seems to be here for retro-compatiblity with
            ** pre 2.4. (as old as 2.1). Probably good to deprecate / delete
            ** as we're not generating this comment anymore since some time */
            else if (!strncmp(bp->line, "<!-- next=", 10)) {
                dp = bp->next;
                if (!strncmp(dp->line, "<ul", 3)) {
                    fprintf(fp, "%s", dp->line);
                    ul = 1;
                }
                fprintf(fp, "<li><strong>%s:</strong> ",
                        lang[MSG_NEXT_MESSAGE]);
                fprintf(fp, "%s%s: \"%s\"</a></li>\n", msg_href(email, e3, TRUE),
#ifdef HAVE_ICONV
                        numname, numsubject);
#else
                        email->name, ptr = convchars(email->subject, email->charset));
                free(ptr);
#endif
                if (ul) {
                    bp = dp;
                    ul = 0;
                }

            }
            bp = bp->next;
       }
#ifdef HAVE_ICONV
       free(numsubject);
       free(numname);
#endif
    }
    fclose(fp);

    /* can we clean up a bit please... */
    free_body(cp);
    free(filename);
}

/*
** Adds a "Reply:" link in the proper article, after the archive has been
** incrementally updated.
*/

void fixreplyheader(char *dir, int num, int remove_maybes, int max_update)
{
    char *filename;
    char line[MAXLINE];

    int subjmatch = 0;
    int replynum = -1;

    struct body *bp, *cp, *status;
    struct body *lp = NULL;
    FILE *fp;

    struct emailinfo *email;
    struct emailinfo *email2 = NULL;

    const char *last_reply = "";
    int next_in_thread = -1;

    const char *old_maybe_pattern = "<li> <b>Maybe reply:</b> <a href=";
    const char *old_reply_pattern = "<b>Reply:</b> ";
    const char *old_nextinthread_pattern = "<b>Next in thread:</b> <a href=\"";
    const char *old_next_pattern = "<li> <b>Next message:</b>:";

    /* pre-WAI patterns */
    char old2_maybe_pattern[MAXLINE];
    char old2_link_maybe_pattern[MAXLINE];
    char old2_reply_pattern[MAXLINE];
    char old2_link_reply_pattern[MAXLINE];
    char old2_nextinthread_pattern[MAXLINE];
    char old2_next_pattern[MAXLINE];

    char current_maybe_pattern[MAXLINE];
    char current_link_maybe_pattern[MAXLINE];
    char current_reply_pattern[MAXLINE];
    char current_link_reply_pattern[MAXLINE];
    char current_nextinthread_pattern[MAXLINE];
    char current_next_pattern[MAXLINE];

    status = hashnumlookup(num, &email);

    if (status == NULL || email->is_deleted)
	return;

    if (remove_maybes || set_linkquotes) {
        /* these are the patterns that may appear in lreply, with and without
           the replies anchor */
        trio_snprintf(current_maybe_pattern, sizeof(current_maybe_pattern),
                      "<li><span class=\"heading\">%s</span>: <a href=", lang[MSG_MAYBE_REPLY]);
        trio_snprintf(current_link_maybe_pattern, sizeof(current_link_maybe_pattern),
                      "<li id=\"replies\"><span class=\"heading\">%s</span>: <a href=", lang[MSG_MAYBE_REPLY]);
        trio_snprintf(current_reply_pattern, sizeof(current_reply_pattern),
                      "<li><span class=\"heading\">%s</span>: <a href=", lang[MSG_REPLY]);
        trio_snprintf(current_link_reply_pattern, sizeof(current_reply_pattern),
                      "<li id=\"replies\"><span class=\"heading\">%s</span>: <a href=",
                      lang[MSG_REPLY]);
        trio_snprintf(current_nextinthread_pattern,
                      sizeof(current_nextinthread_pattern),
                      "<li><span class=\"heading\">%s</span>: <a href=", lang[MSG_NEXT_IN_THREAD]);
        trio_snprintf(current_next_pattern, sizeof(current_next_pattern),
                      "<li><class span=\"heading\">%s</span>: <a href=", lang[MSG_NEXT_MESSAGE]);

	/* backwards compatiblity */
	trio_snprintf(old2_maybe_pattern, sizeof(old2_maybe_pattern),
                      "<li><strong>%s:</strong> <a href=", lang[MSG_MAYBE_REPLY]);
        trio_snprintf(old2_link_maybe_pattern, sizeof(old2_link_maybe_pattern),
                      "<li><strong>%s</strong>: <a href=", lang[MSG_MAYBE_REPLY]);        
        trio_snprintf(old2_reply_pattern, sizeof(old2_reply_pattern),
                      "<li><strong>%s:</strong> <a href=", lang[MSG_REPLY]);
        trio_snprintf(old2_nextinthread_pattern,
                      sizeof(old2_nextinthread_pattern),
                      "<li><strong>%s:</strong> <a href=", lang[MSG_NEXT_IN_THREAD]);
        trio_snprintf(old2_next_pattern, sizeof(old2_next_pattern),
                      "<li><strong>%s:</strong> <a href=", lang[MSG_NEXT_MESSAGE]);
    }

    if (set_linkquotes) {
      struct reply *rp;
      for (rp = replylist; rp != NULL; rp = rp->next) {
	if (rp->msgnum == num && !rp->maybereply) {
	  replynum = rp->frommsgnum;
	  break;
	}
      }
      if (!set_showreplies && replynum != num - 1)
	return;
      if (replynum == -1 && email->inreplyto && email->inreplyto[0]) {
	email2 = hashreplylookup(email->msgnum, email->inreplyto,
				 email->subject, &subjmatch);
	if (!email2)
	  return;
	replynum = email2->msgnum;
	if (subjmatch && remove_maybes)
	  return;
      }
      if (replynum == -1)
	  return;
    }
    else {
	if (!email->inreplyto || !email->inreplyto[0])
	    return;
	email2 = hashreplylookup(email->msgnum, email->inreplyto,
				 email->subject, &subjmatch);
	if (!email2)
	    return;
	replynum = email2->msgnum;
    }
    if (replynum >= max_update)	/* was created this session, must be current */
	return;

    if (email2 == NULL)
	hashnumlookup(replynum, &email2);
    filename = articlehtmlfilename(email2);

    bp = NULL;
    fp = fopen(filename, "r");
    if (fp) {
	while ((fgets(line, MAXLINE, fp)) != NULL) {
	    if (set_linkquotes) {
	        const char *ptr = strstr(line, old_nextinthread_pattern);
		if (ptr)
		    next_in_thread = atoi(ptr+strlen(old_nextinthread_pattern));
		else {
		    ptr = strstr(line, current_nextinthread_pattern);
		    if (ptr)
		        next_in_thread = atoi(ptr+strlen(current_nextinthread_pattern));
		    else {
		      ptr = strstr(line, old2_nextinthread_pattern);
		      if (ptr) {
		        next_in_thread = atoi(ptr+strlen(old2_nextinthread_pattern));
		      }
		    }
		}
	    }
	    bp = addbody(bp, &lp, line, 0);
	}
    }
    else {
	free(filename);
	return;
    }
    fclose(fp);

    cp = bp;			/* save start of list to free later */

    fp = fopen(filename, "w+");
    if (fp) {
        bool list_started = FALSE; /* tells when we're starting a reply list for the
				      first time */
#ifdef HAVE_ICONV
        char *numsubject,*numname;
        
        numsubject=i18n_utf2numref(email->subject,1);
        numname=i18n_utf2numref(email->name,1);
#endif        
	while (bp) {
	    if (!strncmp(bp->line, "<!-- emptylink=", 15)) {
	      /* JK: just skip this line and the following which is just our
	       empty marker. */
	      bp = bp->next;
	      bp = bp->next;
	      continue;
	    }
            /* this is the top anchor that points to the lower #replies */
            if (!strncmp(bp->line, "<li><a href=\"#replies\">", 23)) {
	      list_started = TRUE;
	      fprintf (fp, "%s", bp->line);
	      bp = bp->next;
	      continue;
	    }
	    if (!strncmp(bp->line, "<!-- ureply", 11)) {
                /* we reached the end of ureply, if we don't see the link, we add it */
                if (list_started == FALSE)
                    fprintf (fp, "<li><a href=\"#replies\">%s</a></li>\n",
                             lang[MSG_REPLIES]);
                fprintf (fp, "%s", bp->line);
                bp = bp->next;
                continue;
	    }
	    if (!strncmp(bp->line, "<!-- lreply", 11)) {
	        char *del_msg = (email2->is_deleted ? lang[MSG_DEL_SHORT] : "");
                char *ptr, *ptr1;
#ifdef HAVE_ICONV
		ptr=strsav(numsubject);
#else
		ptr = convchars(email->subject, email->charset);
#endif
		if (list_started == FALSE) {
                    list_started = TRUE;
                    fprintf (fp, "<li id=\"replies\">");
                } else {
                    fprintf (fp, "<li>");
                }

                trio_asprintf(&ptr1,
				"<span class=\"heading\">%s</span>: %s <a href=\"%s\">"
				"%s: \"%s\"</a></li>\n",
				lang[subjmatch ? MSG_MAYBE_REPLY : MSG_REPLY],
				del_msg, msg_href(email, email2, FALSE),
#ifdef HAVE_ICONV
				numname, ptr);
#else
				email->name, ptr);
#endif
		free(ptr);

		if (!last_reply || strcmp(ptr1, last_reply))
		    fputs(ptr1, fp);
		free(ptr1);
	    }
	    else if (!strncmp(bp->line, "<!-- reply", 10)) {
                /* backwards compatiblity with the pre-WAI code */
	        char *del_msg = (email2->is_deleted ? lang[MSG_DEL_SHORT] : "");
                char *ptr, *ptr1;
#ifdef HAVE_ICONV
		ptr=strsav(email->subject);
#else
		ptr = convchars(email->subject, email->charset);
#endif
		trio_asprintf(&ptr1,
			      "<li><strong>%s:</strong>%s %s%s: \"%s\"</a></li>\n",
			      lang[subjmatch ? MSG_MAYBE_REPLY : MSG_REPLY],
			      del_msg, msg_href(email, email2, TRUE),
#ifdef HAVE_ICONV
			      numname, ptr);
#else
			      email->name, ptr);
#endif
		free(ptr);

		if (!last_reply || strcmp(ptr1, last_reply))
		    fputs(ptr1, fp);
		free(ptr1);
	    }
	    if (next_in_thread - 1 == replynum
		&& (strcasestr(bp->line, current_next_pattern)
		    || strcasestr(bp->line, old2_next_pattern)
		    || strstr(bp->line, old_next_pattern))) {
	        bp = bp->next;
		continue; /* line duplicates next in thread; suppress */
	    }

	    if (!remove_maybes
		|| strncasecmp(bp->line, current_maybe_pattern, strlen(current_maybe_pattern))
		|| strncasecmp(bp->line, current_link_maybe_pattern,
			       strlen(current_link_maybe_pattern))
		|| strncasecmp(bp->line, old2_link_maybe_pattern,
			       strlen(old2_link_maybe_pattern))
		|| strncasecmp(bp->line, old_maybe_pattern, strlen(old_maybe_pattern)))
	        fprintf(fp, "%s", bp->line); /* not redundant or disproven */
	    if (set_linkquotes && (strcasestr(bp->line, current_reply_pattern)
				   || strcasestr(bp->line, current_link_reply_pattern)
				   || strcasestr(bp->line, old2_reply_pattern)
				   || strcasestr(bp->line, old2_link_reply_pattern)
				   || strstr(bp->line, old_reply_pattern)))
	        last_reply = bp->line;
	    bp = bp->next;
	}
#ifdef HAVE_ICONV
        free(numsubject);
        free(numname);
#endif
    }
    fclose(fp);

    /* can we clean up a bit please... */
    free_body(cp);
    free(filename);
}

/*
** Adds a "Next in thread:" link in the proper article, after the archive
** has been incrementally updated.
*/

void fixthreadheader(char *dir, int num, int max_update)
{
    char *filename;
    char line[MAXLINE];
    char *name = NULL;
    char *subject = NULL;
    FILE *fp;
    struct reply *rp;
    struct body *bp, *cp;
    struct body *lp = NULL;
    int threadnum = 0;
    char *ptr;
    
    for (rp = threadlist; rp != NULL; rp = rp->next) {
	if (rp->next != NULL &&
	    (rp->next->data && rp->next->data->msgnum == num) &&
	    (rp->data && rp->msgnum != -1)
	    ) {

	    threadnum = rp->msgnum;
	    name = rp->next->data->name;
	    subject = rp->next->data->subject;
	    break;
	}
    }

    if (rp == NULL || threadnum >= max_update)
	return;

    filename = articlehtmlfilename(rp->data);

    bp = NULL;
    if ((fp = fopen(filename, "r")) != NULL) {
	while ((fgets(line, MAXLINE, fp)) != NULL)
	    bp = addbody(bp, &lp, line, 0);
    }
    else {
	free(filename);
	return;
    }

    fclose(fp);

    cp = bp;			/* save start of list to free later */

    if ((fp = fopen(filename, "w+")) != NULL) {
#ifdef HAVE_ICONV
        char *numsubject,*numname;
        ptr=NULL;
        numsubject=i18n_utf2numref(subject,1);
        numname=i18n_utf2numref(name,1);
#endif
	while (bp != NULL) {
	   if (!strncmp(bp->line, "<!-- emptylink=", 15)) {
	      /* JK: just skip this line and the following which is just our
	       empty marker. */
	      bp = bp->next;
	      bp = bp->next;
	      continue;
	    }
	    fprintf(fp, "%s", bp->line);
	    if (!strncmp(bp->line, "<!-- unextthr", 13)) {
                struct emailinfo *e3;
                if (hashnumlookup(num, &e3)) {
                    fprintf (fp, "<li><a href=\"%s\">%s</a></li>\n",
                             msg_href (e3, rp->data, FALSE),
                             lang[MSG_NEXT_IN_THREAD]);
                    if (bp->next && strstr(bp->next->line, lang[MSG_NEXT_IN_THREAD]))
                        bp = bp->next; /* skip old copy of this line */
                }
	    }
	    else if (!strncmp(bp->line, "<!-- lnextthr", 13)) {
	      struct emailinfo *e3;
	      if (hashnumlookup(num, &e3)) {
		fprintf(fp, "<li><span class=\"heading\">%s</span>: ",
			lang[MSG_NEXT_IN_THREAD]);
                fprintf(fp, "<a href=\"%s\">%s: \"%s\"</a></li>\n",
                        msg_href(e3, rp->data, FALSE),
#ifdef HAVE_ICONV
                        numname, numsubject);
                ptr=NULL;
#else
			name, ptr = convchars(subject, NULL));
#endif
                if (ptr)
                    free(ptr);
		if (bp->next && strstr(bp->next->line, lang[MSG_NEXT_IN_THREAD]))
                    bp = bp->next; /* skip old copy of this line */
	      }
	    }
            /* this seems like old pre-WAI code we could remove */
	    else if (!strncmp(bp->line, "<!-- nextthr", 12)) {
		struct emailinfo *e3;
		if(hashnumlookup(num, &e3)) {
		    fprintf(fp, "<li><strong>%s:</strong> ",
			    lang[MSG_NEXT_IN_THREAD]);
		    fprintf(fp, "%s", msg_href(e3, rp->data, TRUE));
		    fprintf(fp, "%s: \"%s\"</a></li>\n",
#ifdef HAVE_ICONV
			    numname, numsubject);
                    ptr=NULL;
#else
			    name, ptr = convchars(subject, NULL));
#endif
		    free(ptr);
		    if (bp->next && strstr(bp->next->line, lang[MSG_NEXT_IN_THREAD]))
		        bp = bp->next; /* skip old copy of this line */
		}
	    }
	    bp = bp->next;
	}
#ifdef HAVE_ICONV
        free(numsubject);
        free(numname);
#endif
    }
    fclose(fp);

    /* can we clean up a bit please... */
    free_body(cp);
    free(filename);
}

int count_deleted(int limit)
{
    struct hashemail *hp;
    struct emailsubdir *sd;
    int total = 0;
    for (hp = deletedlist; hp != NULL; hp = hp->next) {
	if (hp->data->msgnum < limit) {
	    ++total;
	    if ((sd = hp->data->subdir) != NULL)
	        --sd->count;
	}
    }
    return total;
}
