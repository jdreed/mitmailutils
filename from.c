/* 
 * $Id: from.c,v 1.25 1999-09-28 22:16:17 danw Exp $
 *
 * This is the main source file for a KPOP version of the from command. 
 * It was written by Theodore Y. Ts'o, MIT Project Athena.  And later 
 * modified by Jerry Larivee, MIT Project Athena,  to support both KPOP
 * and the old UCB from functionality.
 */

static const char rcsid[] = "$Id: from.c,v 1.25 1999-09-28 22:16:17 danw Exp $";

#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <pwd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#ifdef HAVE_HESIOD
#include <hesiod.h>
#endif

#include "from.h"

#define NOTOK (-1)
#define OK 0
#define DONE 1

#define MAX_HEADER_LINES 512

#ifdef OFF_SWITCH
/* 
 * if the ok file does not exist, then print the default error and do not
 * proceed with the request. If the ok file is missing and the message 
 * file exists, print the contents of the message file and do not proceed 
 * with the request. The message file will be ignored if the ok file exists.
 */

#define OK_FILE   "/afs/athena.mit.edu/system/config/from/ok"
#define MSG_FILE  "/afs/athena.mit.edu/system/config/from/message"
char    *default_err = "Cannot stat the \"ok\" file.. unable to continue with request.";
#endif /* OFF_SWITCH */

FILE 	*sfi, *sfo;
char 	Errmsg[80];
extern int	optind;
extern char     *optarg;

int	popmail_debug, verbose, unixmail, popmail, report, totals;
int	exuser, nonomail;
char	*progname, *sender, *user, *host;

char	*headers[MAX_HEADER_LINES];
int	num_headers, skip_message;

char *Short_List[] = {
	"from", NULL
	};

char *Report_List[] = {
        "from", "subject", NULL
	};

char *Verbose_List[] = {
	"to", "from", "subject", "date", NULL
	};

static int parse_args(int argc, char **argv);
static void lusage(void);
static int getmail_pop(char *user, char *host, int printhdr);
static void header_scan(char *line, int *last_header);
static int pop_scan(int msgno, void (*action)(char *, int *));
static void error(char *s1);
static int list_compare(char *s, char **list);
static void make_lower_case(char *s);
static char *parse_from_field(char *str);
static int getmail_unix(char *user);
static int match(char *line, char *str);
static void print_report(char **headers, int num_headers, int winlength);
#ifdef OFF_SWITCH
static int pop_ok(void);
#endif

int main(int argc, char **argv)
{
	int	locmail = -1, remmail = -1;

	parse_args(argc,argv);
	if (unixmail)
	  locmail = getmail_unix(user);
	if (popmail)
	  remmail = getmail_pop(user, host, locmail);
	if (!nonomail && (report || verbose || totals) &&
	    (unixmail == 0 || locmail == 0) &&
	    (popmail == 0 || remmail == 0)) {
	  if (exuser)
	    printf ("%s doesn't have any mail waiting.\n", user);
	  else
	    puts("You don't have any mail waiting.");
	}

	exit ((unixmail && locmail < 0) || (popmail && remmail < 0));
}

static int parse_args(int argc, char **argv)
{
	register struct passwd *pp;
	char *cp;
	int	c;
#ifdef HAVE_HESIOD
	struct hes_postoffice *p;
#endif

	cp = strrchr(argv[0], '/');
	if (cp)
	    progname = cp + 1;
	else
	    progname = argv[0];
	verbose = popmail_debug = 0;
	host = user = sender = NULL;
	unixmail = popmail = 1;
	
	optind = 1;
	while ((c = getopt(argc,argv,"rvdnputs:h:")) != EOF)
		switch(c) {
		case 'r':
		        /* report on no mail */
		        report = 1;
			break;
		case 'n':
			nonomail = 1;
			break;
		case 'v':
		        /* verbose mode */
			verbose++;
			report = 0;
			break;
		case 'd':
			/* debug mode */
			popmail_debug++;
			break;
		case 'p':
			/* check pop only */
			unixmail = 0;
			break;
		case 'u':
			/* check unix mail only */
			popmail = 0;
			break;
		case 's':
			/* check for mail from sender only */
			sender = optarg;
			break;
		case 'h':
			/* specify pobox host */
			host = optarg;
			break;
		case 't':
			/* print total messages only */
			totals = 1;
			report = 0;
			verbose = 0;
			break;
		case '?':
			lusage();
		}
	/* check mail for user */
	if (optind < argc) {
		exuser = 1;
		user = argv[optind];
	} else {
		user = getenv ("USER");
		if (user == NULL)
			user = strdup(getlogin());
		if (!user || !*user) {
			pp = getpwuid((int) getuid());
			if (pp == NULL) {
				fprintf (stderr, "%s: user not in password file\n", progname);
				exit(1);
			}
			user = pp->pw_name;
		}
	      }
	
	if (popmail) {
	  if (!host)
	    host = getenv("MAILHOST");
#ifdef HAVE_HESIOD
	  if (!host) {
	    p = hes_getmailhost(user);
	    if (p && !strcmp(p->po_type, "POP"))
	      host = p->po_host;
	  }
#endif
	  if (!host) {
	    if (exuser)
	      fprintf(stderr, "%s: can't find post office server for user %s.\n",
		      progname, user);
	    else
	      fprintf(stderr, "%s: can't find post office server.\n", progname);

	    if (!unixmail) {
	      return(1);
	    }
	    fprintf(stderr, "%s: Trying unix mail drop...\n", progname);
	    popmail = 0;
	  }
	}
	return 0;
}

static void lusage(void)
{
	fprintf(stderr, "Usage: %s [-v | -r | -t] [-p | -u] [-s sender] [-h host] [user]\n", progname);
	exit(1);
}


static int getmail_pop(char *user, char *host, int printhdr)
{
	int nmsgs, nbytes, linelength;
	char response[128];
	register int i, j;
	struct winsize windowsize;

#ifdef OFF_SWITCH
	if(pop_ok() == NOTOK)
	     return (1);
#endif /* OFF_SWITCH */

	if (pop_init(host) == NOTOK) {
		error(Errmsg);
		return(1);
	}

	if ((getline(response, sizeof response, sfi) != OK) ||
	    (*response != '+')){
		error(response);
		return -1;
	}

#ifdef HAVE_KRB4
	if (pop_command("USER %s", user) == NOTOK || 
	    pop_command("PASS %s", user) == NOTOK)
#else
	if (pop_command("USER %s", user) == NOTOK || 
	    pop_command("RPOP %s", user) == NOTOK)
#endif
	{
		error(Errmsg);
		(void) pop_command("QUIT");
		return -1;
	}

	if (pop_stat(&nmsgs, &nbytes) == NOTOK) {
		error(Errmsg);
		(void) pop_command("QUIT");
		return -1;
	}
	if (nmsgs == 0) {
		(void) pop_command("QUIT");
		return(0);
	}
	if (verbose || totals)
		printf("You have %d %s (%d bytes) on %s%c\n",
		       nmsgs, nmsgs > 1 ? "messages" : "message",
		       nbytes, host, verbose ? ':' : '.');
	if (totals) {
		(void) pop_command("QUIT");
		return nmsgs;
	}
	if (printhdr)
		puts("POP mail:");

	/* find out how long the line is for the stdout */
	if ((ioctl(1, TIOCGWINSZ, (void *)&windowsize) < 0) || 
	    (windowsize.ws_col == 0))
		windowsize.ws_col = 80;		/* default assume 80 */
	/* for the console window timestamp */
	linelength = windowsize.ws_col - 6;
	if (linelength < 32)
		linelength = 32;
	
	for (i = 1; i <= nmsgs; i++) {
		if (verbose && !skip_message)
			putchar('\n');
		num_headers = skip_message = 0;
		if (pop_scan(i, header_scan) == NOTOK) {
			error(Errmsg);
			(void) pop_command("QUIT");
			return -1;
		}
		if (report) {
			if (!skip_message)
				print_report(headers, num_headers, linelength);
			else
				for (j=0; j<num_headers; j++)
					free(headers[j]);
		} else {
			for (j=0; j<num_headers; j++) {
				if (!skip_message)
					puts(headers[j]);
				free(headers[j]);
			}
		}
	}
	
	(void) pop_command("QUIT");
	return nmsgs;
}

static void header_scan(char *line, int *last_header)
{
	char	*keyword, **search_list;
	register int	i;
	
	if (*last_header && isspace((unsigned char)*line)) {
		headers[num_headers++] = strdup(line);
		return;
	}

	for (i=0;line[i] && line[i]!=':';i++) ;
	keyword=malloc((unsigned) i+1);
	if (keyword == NULL)
	  {
	    fprintf (stderr, "%s: out of memory\n", progname);
	    exit (1);
	  }
	(void) strncpy(keyword,line,i);
	keyword[i]='\0';
	make_lower_case(keyword);
	if (sender && !strcmp(keyword,"from")) {
		char *mail_from = parse_from_field(line+i+1);
		if (strcmp(sender, mail_from))
			skip_message++;
		free (mail_from);
	      }
	if (verbose)
	  search_list = Verbose_List;
	else if (report)
	  search_list = Report_List;
	else
	  search_list = Short_List;
	if (list_compare(keyword, search_list)) {
		*last_header = 1;
		headers[num_headers++] = strdup(line);
	} else
		*last_header = 0;	
}

static int pop_scan(int msgno, void (*action)(char *, int *))
{	
	char buf[4096];
	int	headers = 1;
	int	scratch = 0;	/* Scratch for action() */

	(void) sprintf(buf, "RETR %d", msgno);
	if (popmail_debug) fprintf(stderr, "%s\n", buf);
	if (putline(buf, Errmsg, sfo) == NOTOK)
		return(NOTOK);
	if (getline(buf, sizeof buf, sfi) != OK) {
		(void) strcpy(Errmsg, buf);
		return(NOTOK);
	}
	while (headers) {
		switch (multiline(buf, sizeof buf, sfi)) {
		case OK:
			if (!*buf)
				headers = 0;
			(*action)(buf,&scratch);
			break;
		case DONE:
			return(OK);
		case NOTOK:
			return(NOTOK);
		}
	}
	while (1) {
		switch (multiline(buf, sizeof buf, sfi)) {
		case OK:
			break;
		case DONE:
			return(OK);
		case NOTOK:
			return(NOTOK);
		}
	}
}

/* Print error message. */

static void error(char *s)
{
  fprintf(stderr, "pop: %s\n", s);
}

static int list_compare(char *s, char **list)
{
      while (*list!=NULL) {
	      if (strcmp(*list++, s) == 0)
		      return(1);
      }
      return(0);
}

static void make_lower_case(char *s)
{
      do
              *s = tolower(*s);
      while (*s++);
}

static char *parse_from_field(char *str)
{
	register char	*cp, *scr;
	char		*stored;
	
	stored = scr = strdup(str);
	while (*scr && isspace((unsigned char)*scr))
		scr++;
	cp = strchr(scr, '<');
	if (cp)
		scr = cp + 1;
	cp = strchr(scr, '@');
	if (cp)
		*cp = '\0';
	cp = strchr(scr, '>');
	if (cp)
		*cp = '\0';
	scr = strdup(scr);
	make_lower_case(scr);
	free(stored);
	return(scr);
}

/*
 * Do the old unix mail lookup.
 */

static int getmail_unix(char *user)
{
	char lbuf[BUFSIZ];
	char lbuf2[BUFSIZ];
	int havemail = 0, stashed = 0;
	register char *name;
	char *maildrop;
	char *maildir;

	if (sender != NULL)
	  for (name = sender; *name; name++)
	    *name = tolower(*name);

	maildrop = getenv("MAILDROP");
	if (maildrop && *maildrop) {
	    if (freopen(maildrop, "r", stdin) == NULL) {
		if (!popmail)
		    fprintf(stderr, "Can't open maildrop: %s.\n", maildrop);
		unixmail = 0;
		return -1;
	    }
	} else {
	    maildir = "/var/spool/mail";
	    if (chdir(maildir) < 0) {
		maildir = "/var/mail";
		if (chdir(maildir) < 0) {
		    unixmail = 0;
		    return -1;
		}
	    }
	    if (freopen(user, "r", stdin) == NULL) {
		if (!popmail)
		    fprintf(stderr, "Can't open %s/%s.\n", maildir, user);
		unixmail = 0;
		return -1;
	    }
	}

	while (fgets(lbuf, sizeof lbuf, stdin) != NULL)
		if (lbuf[0] == '\n' && stashed) {
			stashed = 0;
			if (!havemail && !totals && popmail)
				puts("Local mail:");
			if (!totals)
				fputs(lbuf2, stdout);
			havemail++;
		} else if (strncmp(lbuf, "From ", 5) == 0 &&
		    (sender == NULL || match(&lbuf[4], sender))) {
			strcpy(lbuf2, lbuf);
			stashed = 1;
		}
	if (stashed && !totals)
		fputs(lbuf2, stdout);
	if (totals && havemail) {
		struct stat st;
		if (fstat(0, &st) != -1)
			printf("You have %d local message%s (%lu bytes).\n",
			       havemail, havemail > 1 ? "s" : "",
			       (unsigned long)st.st_size);
		else
			printf("You have %d local message%s.\n",
			       havemail, havemail > 1 ? "s" : "");
	}
	return havemail;
}

static int match(char *line, char *str)
{
	register char ch;

	while (*line == ' ' || *line == '\t')
		++line;
	if (*line == '\n')
		return (0);
	while (*str && *line != ' ' && *line != '\t' && *line != '\n') {
		ch = tolower(*line);
		if (ch != *str++)
			return (0);
		line++;
	}
	return (*str == '\0');
}

static void print_report(char **headers, int num_headers, int winlength)
{
  int j, len;
  char *p, *from_field = 0, *subject_field = 0, *buf, *buf1;
  
  for(j = 0; j < num_headers; j++) {
    p = strchr(headers[j], ':');
    if (p == NULL)
      continue;

    if (strncmp("From", headers[j], 4) == 0) {
      p++;
      while (p[0] == ' ')
	p++;
      from_field = p;
      if (subject_field)
	break;
    }
    else if (strncmp("Subject", headers[j], 7) == 0) {
      p++;
      while (p[0] == ' ') 
	p++;
      subject_field = p;
      if (from_field)
	break;
    }
  }

  buf = malloc(winlength+1);  /* add 1 for the NULL terminator */
  if (buf == NULL)
    {
      fprintf (stderr, "from: out of memory\n");
      exit (1);
    }    
  buf[0] = '\0';
  buf[winlength] = '\0';
  if (from_field)
    strncpy(buf, from_field, winlength);
  else
    strncpy(buf, "<<unknown sender>>", winlength);
  len = strlen(buf);
  if  (len < 30)
    len = 30;

  buf1 = malloc(winlength-len+1);  /* add 1 for the NULL terminator */
  if (buf1 == NULL)
    {
      fprintf (stderr, "from: out of memory\n");
      exit (1);
    }    
  buf1[0] = '\0';

  if (winlength - len - 1 < 1)
    subject_field = NULL;

  if (subject_field)
    {
      strncpy(buf1, subject_field, winlength - len - 1);
      buf1[winlength - len - 1] = 0;
    }
  
  printf("%-30s %s\n", buf, buf1);

  free(buf);
  free(buf1);
  for (j = 0; j < num_headers; j++)
    free(headers[j]);
}


#ifdef OFF_SWITCH

static int pop_ok(void)
{
  FILE *fp;
  struct stat sbuf;
  char buf[BUFSIZ];

  if(stat(OK_FILE, &sbuf) == 0)
       return(OK);

  if(!(fp = fopen(MSG_FILE, "r")))  {
       printf("%s\n", default_err);
       return(NOTOK);
  }

  memset(buf, 0, sizeof(buf));
  while(fgets(buf, sizeof(buf), fp))
      printf("%s", buf);
  fclose(fp);
  return(NOTOK);
}

#endif /* OFF_SWITCH */
  
