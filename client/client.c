/*
 *	BIRD Client
 *
 *	(c) 1999--2004 Martin Mares <mj@ucw.cz>
 *	(c) 2013 Tomas Hlavacek <tmshlvck@gmail.com>
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

/**
 * DOC: BIRD client
 *
 * There are two variants of BIRD client: regular and light. regular
 * variant depends on readline and ncurses libraries, while light
 * variant uses just libc. Most of the code and the main() is common
 * for both variants (in client.c file) and just a few functions are
 * different (in birdc.c for regular and birdcl.c for light). Two
 * binaries are generated by linking common object files like client.o
 * (which is compiled from client.c just once) with either birdc.o or
 * birdcl.o for each variant.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include "nest/bird.h"
#include "lib/resource.h"
#include "lib/string.h"
#include "client/client.h"
#include "sysdep/unix/unix.h"

#define SERVER_READ_BUF_LEN 4096

static char *opt_list = "s:vrl";
static int verbose, restricted, once;
static char *init_cmd;

static char *server_path = PATH_CONTROL_SOCKET;
static int server_fd;
static byte server_read_buf[SERVER_READ_BUF_LEN];
static byte *server_read_pos = server_read_buf;

int init = 1;		/* During intial sequence */
int busy = 1;		/* Executing BIRD command */
int interactive;	/* Whether stdin is terminal */
int last_code;		/* Last return code */

static int num_lines, skip_input;
int term_lns, term_cls;


/*** Parsing of arguments ***/

static void
usage(char *name)
{
  fprintf(stderr, "Usage: %s [-s <control-socket>] [-v] [-r] [-l]\n", name);
  exit(1);
}

static void
parse_args(int argc, char **argv)
{
  int server_changed = 0;
  int c;

  while ((c = getopt(argc, argv, opt_list)) >= 0)
    switch (c)
      {
      case 's':
	server_path = optarg;
	server_changed = 1;
	break;
      case 'v':
	verbose++;
	break;
      case 'r':
	restricted = 1;
	break;
      case 'l':
	if (!server_changed)
	  server_path = xbasename(server_path);
	break;
      default:
	usage(argv[0]);
      }

  /* If some arguments are not options, we take it as commands */
  if (optind < argc)
    {
      char *tmp;
      int i;
      int len = 0;

      for (i = optind; i < argc; i++)
	len += strlen(argv[i]) + 1;

      tmp = init_cmd = malloc(len);
      for (i = optind; i < argc; i++)
	{
	  strcpy(tmp, argv[i]);
	  tmp += strlen(tmp);
	  *tmp++ = ' ';
	}
      tmp[-1] = 0;

      once = 1;
      interactive = 0;
    }
}


/*** Input ***/

static void server_send(char *cmd);

static int
handle_internal_command(char *cmd)
{
  if (!strncmp(cmd, "exit", 4) || !strncmp(cmd, "quit", 4))
    {
      cleanup();
      exit(0);
    }
  if (!strncmp(cmd, "help", 4))
    {
      puts("Press `?' for context sensitive help.");
      return 1;
    }
  return 0;
}

static void
submit_server_command(char *cmd)
{
  busy = 1;
  num_lines = 2;
  server_send(cmd);
}

static inline void
submit_init_command(char *cmd_raw)
{
  char *cmd = cmd_expand(cmd_raw);

  if (!cmd)
  {
    cleanup();
    exit(0);
  }

  submit_server_command(cmd);
  free(cmd);
}

void
submit_command(char *cmd_raw)
{
  char *cmd = cmd_expand(cmd_raw);

  if (!cmd)
    return;

  if (!handle_internal_command(cmd))
    submit_server_command(cmd);

  free(cmd);
}

static void
init_commands(void)
{
  if (restricted)
    {
       submit_server_command("restrict");
       restricted = 0;
       return;
    }

  if (init_cmd)
    {
      /* First transition - client received hello from BIRD
	 and there is waiting initial command */
      submit_init_command(init_cmd);
      init_cmd = NULL;
      return;
    }

  if (once)
    {
      /* Initial command is finished and we want to exit */
      cleanup();
      exit((last_code < 8000) ? 0 : 1);
    }

  input_init();

  term_lns = (term_lns > 0) ? term_lns : 25;
  term_cls = (term_cls > 0) ? term_cls : 80;

  init = 0;
}


/*** Output ***/

void
more(void)
{
  more_begin();
  printf("--More--\015");
  fflush(stdout);

 redo:
  switch (getchar())
    {
    case ' ':
      num_lines = 2;
      break;
    case '\n':
    case '\r':
      num_lines--;
      break;
    case 'q':
      skip_input = 1;
      break;
    default:
      goto redo;
    }

  printf("        \015");
  fflush(stdout);
  more_end();
}


/*** Communication with server ***/

static void
server_connect(void)
{
  struct sockaddr_un sa;

  server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd < 0)
    DIE("Cannot create socket");

  if (strlen(server_path) >= sizeof(sa.sun_path))
    die("server_connect: path too long");

  bzero(&sa, sizeof(sa));
  sa.sun_family = AF_UNIX;
  strcpy(sa.sun_path, server_path);
  if (connect(server_fd, (struct sockaddr *) &sa, SUN_LEN(&sa)) < 0)
    DIE("Unable to connect to server control socket (%s)", server_path);
  if (fcntl(server_fd, F_SETFL, O_NONBLOCK) < 0)
    DIE("fcntl");
}


#define PRINTF(LEN, PARGS...) do { if (!skip_input) len = printf(PARGS); } while(0)

static void
server_got_reply(char *x)
{
  int code;
  int len = 0;

  if (*x == '+')                        /* Async reply */
    PRINTF(len, ">>> %s\n", x+1);
  else if (x[0] == ' ')                 /* Continuation */
    PRINTF(len, "%s%s\n", verbose ? "     " : "", x+1);
  else if (strlen(x) > 4 &&
           sscanf(x, "%d", &code) == 1 && code >= 0 && code < 10000 &&
           (x[4] == ' ' || x[4] == '-'))
    {
      if (code)
        PRINTF(len, "%s\n", verbose ? x : x+5);

      last_code = code;

      if (x[4] == ' ')
      {
        busy = 0;
        skip_input = 0;
        return;
      }
    }
  else
    PRINTF(len, "??? <%s>\n", x);

  if (interactive && busy && !skip_input && !init && (len > 0))
    {
      num_lines += (len + term_cls - 1) / term_cls; /* Divide and round up */
      if (num_lines >= term_lns)
        more();
    }
}

static void
server_read(void)
{
  int c;
  byte *start, *p;

 redo:
  c = read(server_fd, server_read_pos, server_read_buf + sizeof(server_read_buf) - server_read_pos);
  if (!c)
    die("Connection closed by server");
  if (c < 0)
    {
      if (errno == EINTR)
	goto redo;
      else
	DIE("Server read error");
    }

  start = server_read_buf;
  p = server_read_pos;
  server_read_pos += c;
  while (p < server_read_pos)
    if (*p++ == '\n')
      {
	p[-1] = 0;
	server_got_reply(start);
	start = p;
      }
  if (start != server_read_buf)
    {
      int l = server_read_pos - start;
      memmove(server_read_buf, start, l);
      server_read_pos = server_read_buf + l;
    }
  else if (server_read_pos == server_read_buf + sizeof(server_read_buf))
    {
      strcpy(server_read_buf, "?<too-long>");
      server_read_pos = server_read_buf + 11;
    }
}

static void
select_loop(void)
{
  int rv;
  while (1)
    {
      if (init && !busy)
	init_commands();

      if (!init)
	input_notify(!busy);

      fd_set select_fds;
      FD_ZERO(&select_fds);

      FD_SET(server_fd, &select_fds);
      if (!busy)
	FD_SET(0, &select_fds);

      rv = select(server_fd+1, &select_fds, NULL, NULL, NULL);
      if (rv < 0)
	{
	  if (errno == EINTR)
	    continue;
	  else
	    DIE("select");
	}

      if (FD_ISSET(0, &select_fds))
	{
	  input_read();
	  continue;
	}

      if (FD_ISSET(server_fd, &select_fds))
	{
	  server_read();
	  continue;
	}
    }
}

static void
wait_for_write(int fd)
{
  while (1)
    {
      int rv;
      fd_set set;
      FD_ZERO(&set);
      FD_SET(fd, &set);

      rv = select(fd+1, NULL, &set, NULL, NULL);
      if (rv < 0)
	{
	  if (errno == EINTR)
	    continue;
	  else
	    DIE("select");
	}

      if (FD_ISSET(server_fd, &set))
	return;
    }
}

static void
server_send(char *cmd)
{
  int l = strlen(cmd);
  byte *z = alloca(l + 1);

  memcpy(z, cmd, l);
  z[l++] = '\n';
  while (l)
    {
      int cnt = write(server_fd, z, l);

      if (cnt < 0)
	{
	  if (errno == EAGAIN)
	    wait_for_write(server_fd);
	  else if (errno == EINTR)
	    continue;
	  else
	    DIE("Server write error");
	}
      else
	{
	  l -= cnt;
	  z += cnt;
	}
    }
}

int
main(int argc, char **argv)
{
  interactive = isatty(0);
  parse_args(argc, argv);
  cmd_build_tree();
  server_connect();
  select_loop();
  return 0;
}
