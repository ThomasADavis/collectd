/**
 * collectd - src/memcached.c, based on src/hddtemp.c
 * Copyright (C) 2007       Antony Dovgal
 * Copyright (C) 2007-2010  Florian Forster
 * Copyright (C) 2009       Doug MacEachern
 * Copyright (C) 2009       Franck Lombardi
 * Copyright (C) 2012       Nicolas Szalay
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Antony Dovgal <tony at daylessday dot org>
 *   Florian octo Forster <octo at collectd.org>
 *   Doug MacEachern <dougm at hyperic.com>
 *   Franck Lombardi
 *   Nicolas Szalay
 **/

#include "collectd.h"
#include "common.h"
#include "plugin.h"
#include "configfile.h"

# include <poll.h>
# include <netdb.h>
# include <sys/socket.h>
# include <sys/un.h>
# include <netinet/in.h>
# include <netinet/tcp.h>

/* Hack to work around the missing define in AIX */
#ifndef MSG_DONTWAIT
# define MSG_DONTWAIT MSG_NONBLOCK
#endif

#define MEMCACHED_DEF_HOST "127.0.0.1"
#define MEMCACHED_DEF_PORT "11211"

#define MEMCACHED_RETRY_COUNT 100

struct memcached_s
{
  char *name;
  char *socket;
  char *host;
  char *port;
};

typedef struct memcached_s memcached_t;

static int memcached_read (user_data_t *user_data);

static void memcached_free (memcached_t *st)
{
  if (st == NULL)
    return;

  sfree (st->name);
  sfree (st->socket);
  sfree (st->host);
  sfree (st->port);
}

static int memcached_query_daemon (char *buffer, int buffer_size, user_data_t *user_data)
{
  int fd=-1;
  ssize_t status;
  int buffer_fill;
  int i = 0;

  memcached_t *st;
  st = user_data->data;
  if (st->socket != NULL) {
    struct sockaddr_un serv_addr;

     memset (&serv_addr, 0, sizeof (serv_addr));
     serv_addr.sun_family = AF_UNIX;
     sstrncpy (serv_addr.sun_path, st->socket,
     sizeof (serv_addr.sun_path));

     /* create our socket descriptor */
     fd = socket (AF_UNIX, SOCK_STREAM, 0);
     if (fd < 0) {
       char errbuf[1024];
       ERROR ("memcached: unix socket: %s", sstrerror (errno, errbuf,
       sizeof (errbuf)));
       return -1;
     }
  }
  else {
    if (st->port != NULL) {
      const char *host;
      const char *port;

      struct addrinfo  ai_hints;
      struct addrinfo *ai_list, *ai_ptr;
      int              ai_return = 0;

      memset (&ai_hints, '\0', sizeof (ai_hints));
      ai_hints.ai_flags    = 0;
#ifdef AI_ADDRCONFIG
    /*  ai_hints.ai_flags   |= AI_ADDRCONFIG; */
#endif
      ai_hints.ai_family   = AF_INET;
      ai_hints.ai_socktype = SOCK_STREAM;
      ai_hints.ai_protocol = 0;

      host = st->host;
      if (host == NULL) {
        host = MEMCACHED_DEF_HOST;
      }

      port = st->port;
      if (strlen (port) == 0) {
        port = MEMCACHED_DEF_PORT;
      }

      if ((ai_return = getaddrinfo (host, port, &ai_hints, &ai_list)) != 0) {
        char errbuf[1024];
        ERROR ("memcached: getaddrinfo (%s, %s): %s",
          host, port,
          (ai_return == EAI_SYSTEM)
          ? sstrerror (errno, errbuf, sizeof (errbuf))
          : gai_strerror (ai_return));
        return -1;
      }

      fd = -1;
      for (ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next) {
        /* create our socket descriptor */
        fd = socket (ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol);
        if (fd < 0) {
          char errbuf[1024];
          ERROR ("memcached: socket: %s", sstrerror (errno, errbuf, sizeof (errbuf)));
          continue;
        }

        /* connect to the memcached daemon */
        status = (ssize_t) connect (fd, (struct sockaddr *) ai_ptr->ai_addr, ai_ptr->ai_addrlen);
        if (status != 0) {
          shutdown (fd, SHUT_RDWR);
          close (fd);
          fd = -1;
          continue;
        }

        /* A socket could be opened and connecting succeeded. We're
         * done. */
        break;
      }

      freeaddrinfo (ai_list);
    }
  }

  if (fd < 0) {
    ERROR ("memcached: Could not connect to daemon.");
    return -1;
  }

  if (send(fd, "stats\r\n", sizeof("stats\r\n") - 1, MSG_DONTWAIT) != (sizeof("stats\r\n") - 1)) {
    ERROR ("memcached: Could not send command to the memcached daemon.");
    return -1;
  }

  {
    struct pollfd p;
    int status;

    memset (&p, 0, sizeof (p));
    p.fd = fd;
    p.events = POLLIN | POLLERR | POLLHUP;
    p.revents = 0;

    status = poll (&p, /* nfds = */ 1,
        /* timeout = */ CDTIME_T_TO_MS (interval_g));
    if (status <= 0)
    {
      if (status == 0)
      {
        ERROR ("memcached: poll(2) timed out after %.3f seconds.",
            CDTIME_T_TO_DOUBLE (interval_g));
      }
      else
      {
        char errbuf[1024];
        ERROR ("memcached: poll(2) failed: %s",
            sstrerror (errno, errbuf, sizeof (errbuf)));
      }
      shutdown (fd, SHUT_RDWR);
      close (fd);
      return (-1);
    }
  }

  /* receive data from the memcached daemon */
  memset (buffer, '\0', buffer_size);

  buffer_fill = 0;
  while ((status = recv (fd, buffer + buffer_fill, buffer_size - buffer_fill, MSG_DONTWAIT)) != 0) {
    if (i > MEMCACHED_RETRY_COUNT) {
      ERROR("recv() timed out");
      break;
    }
    i++;

    if (status == -1) {
      char errbuf[1024];

      if (errno == EAGAIN) {
        continue;
      }

      ERROR ("memcached: Error reading from socket: %s",
          sstrerror (errno, errbuf, sizeof (errbuf)));
      shutdown(fd, SHUT_RDWR);
      close (fd);
      return -1;
    }
    buffer_fill += status;

    if (buffer_fill > 3 && buffer[buffer_fill-5] == 'E' && buffer[buffer_fill-4] == 'N' && buffer[buffer_fill-3] == 'D') {
      /* we got all the data */
      break;
    }
  }

  if (buffer_fill >= buffer_size) {
    buffer[buffer_size - 1] = '\0';
    WARNING ("memcached: Message from memcached has been truncated.");
  } else if (buffer_fill == 0) {
    WARNING ("memcached: Peer has unexpectedly shut down the socket. "
        "Buffer: `%s'", buffer);
    shutdown(fd, SHUT_RDWR);
    close(fd);
    return -1;
  }

  shutdown(fd, SHUT_RDWR);
  close(fd);
  return 0;
}

/* Configuration handling functiions
 * <Plugin memcached>
 *   <Instance "instance_name">
 *     Host foo.zomg.com
 *     Port "1234"
 *   </Instance>
 * </Plugin>
 */
static int config_set_string (char **ret_string, oconfig_item_t *ci)
{
  char *string;

  if ((ci->values_num != 1)
      || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("memcached plugin: The `%s' config option "
        "needs exactly one string argument.", ci->key);
    return (-1);
  }

  string = strdup (ci->values[0].value.string);
  if (string == NULL)
  {
    ERROR ("memcached plugin: strdup failed.");
    return (-1);
  }

  if (*ret_string != NULL)
    free (*ret_string);
  *ret_string = string;

  return (0);
}

static int config_add_instance(oconfig_item_t *ci)
{
  memcached_t *st;
  int i;
  int status;

  if ((ci->values_num != 1)
    || (ci->values[0].type != OCONFIG_TYPE_STRING))
  {
    WARNING ("memcached plugin: The `%s' config option "
      "needs exactly one string argument.", ci->key);
    return (-1);
  }

  st = (memcached_t *) malloc (sizeof (*st));
  if (st == NULL)
  {
    ERROR ("memcached plugin: malloc failed.");
    return (-1);
  }

  st->name = NULL;
  st->socket = NULL;
  st->host = NULL;
  st->port = NULL;
  memset (st, 0, sizeof (*st));

  status = config_set_string (&st->name, ci);
  if (status != 0)
  {
    sfree (st);
    return (status);
  }
  assert (st->name != NULL);

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp ("Socket", child->key) == 0)
      status = config_set_string (&st->socket, child);
    else if (strcasecmp ("Host", child->key) == 0)
      status = config_set_string (&st->host, child);
    else if (strcasecmp ("Port", child->key) == 0)
      status = config_set_string (&st->port, child);
    else
    {
      WARNING ("memcached plugin: Option `%s' not allowed here.",
          child->key);
      status = -1;
    }

    if (status != 0)
      break;
  }

  if (status == 0)
  {
    user_data_t ud;
    char callback_name[3*DATA_MAX_NAME_LEN];

    memset (&ud, 0, sizeof (ud));
    ud.data = st;
    ud.free_func = (void *) memcached_free;

    memset (callback_name, 0, sizeof (callback_name));
    ssnprintf (callback_name, sizeof (callback_name),
        "memcached/%s/%s",
        (st->host != NULL) ? st->host : hostname_g,
        (st->port != NULL) ? st->port : "default"),

    status = plugin_register_complex_read (/* group = */ NULL,
        /* name      = */ callback_name,
        /* callback  = */ memcached_read,
        /* interval  = */ NULL,
        /* user_data = */ &ud);
  }

  if (status != 0)
  {
    memcached_free(st);
    return (-1);
  }

  return (0);
}

static int config (oconfig_item_t *ci)
{
  int status = 0;
  int i;

  for (i = 0; i < ci->children_num; i++)
  {
    oconfig_item_t *child = ci->children + i;

    if (strcasecmp ("Instance", child->key) == 0)
      config_add_instance (child);
    else
      WARNING ("memcached plugin: The configuration option "
          "\"%s\" is not allowed here. Did you "
          "forget to add an <Instance /> block "
          "around the configuration?",
          child->key);
  } /* for (ci->children) */

  return (status);
}

static void submit_derive (const char *type, const char *type_inst,
    derive_t value, memcached_t *st)
{
  value_t values[1];
  value_list_t vl = VALUE_LIST_INIT;

  values[0].derive = value;

  vl.values = values;
  vl.values_len = 1;
  sstrncpy (vl.host, hostname_g, sizeof (vl.host));
  sstrncpy (vl.plugin, "memcached", sizeof (vl.plugin));
  if (st->name != NULL)
    sstrncpy (vl.plugin_instance, st->name,  sizeof (vl.plugin_instance));
  sstrncpy (vl.type, type, sizeof (vl.type));
  if (type_inst != NULL)
    sstrncpy (vl.type_instance, type_inst, sizeof (vl.type_instance));

  plugin_dispatch_values (&vl);
}

static void submit_derive2 (const char *type, const char *type_inst,
    derive_t value0, derive_t value1, memcached_t *st)
{
  value_t values[2];
  value_list_t vl = VALUE_LIST_INIT;

  values[0].derive = value0;
  values[1].derive = value1;

  vl.values = values;
  vl.values_len = 2;
  sstrncpy (vl.host, hostname_g, sizeof (vl.host));
  sstrncpy (vl.plugin, "memcached", sizeof (vl.plugin));
  if (st->name != NULL)
    sstrncpy (vl.plugin_instance, st->name,  sizeof (vl.plugin_instance));
  sstrncpy (vl.type, type, sizeof (vl.type));
  if (type_inst != NULL)
    sstrncpy (vl.type_instance, type_inst, sizeof (vl.type_instance));

  plugin_dispatch_values (&vl);
}

static void submit_gauge (const char *type, const char *type_inst,
    gauge_t value, memcached_t *st)
{
  value_t values[1];
  value_list_t vl = VALUE_LIST_INIT;

  values[0].gauge = value;

  vl.values = values;
  vl.values_len = 1;
  sstrncpy (vl.host, hostname_g, sizeof (vl.host));
  sstrncpy (vl.plugin, "memcached", sizeof (vl.plugin));
  if (st->name != NULL)
    sstrncpy (vl.plugin_instance, st->name,  sizeof (vl.plugin_instance));
  sstrncpy (vl.type, type, sizeof (vl.type));
  if (type_inst != NULL)
    sstrncpy (vl.type_instance, type_inst, sizeof (vl.type_instance));

  plugin_dispatch_values (&vl);
}

static void submit_gauge2 (const char *type, const char *type_inst,
    gauge_t value0, gauge_t value1, memcached_t *st)
{
  value_t values[2];
  value_list_t vl = VALUE_LIST_INIT;

  values[0].gauge = value0;
  values[1].gauge = value1;

  vl.values = values;
  vl.values_len = 2;
  sstrncpy (vl.host, hostname_g, sizeof (vl.host));
  sstrncpy (vl.plugin, "memcached", sizeof (vl.plugin));
  if (st->name != NULL)
    sstrncpy (vl.plugin_instance, st->name,  sizeof (vl.plugin_instance));
  sstrncpy (vl.type, type, sizeof (vl.type));
  if (type_inst != NULL)
    sstrncpy (vl.type_instance, type_inst, sizeof (vl.type_instance));

  plugin_dispatch_values (&vl);
}

static int memcached_read (user_data_t *user_data)
{
  char buf[4096];
  char *fields[3];
  char *ptr;
  char *line;
  char *saveptr;
  int fields_num;

  gauge_t bytes_used = NAN;
  gauge_t bytes_total = NAN;
  gauge_t hits = NAN;
  gauge_t gets = NAN;
  derive_t rusage_user = 0;
  derive_t rusage_syst = 0;
  derive_t octets_rx = 0;
  derive_t octets_tx = 0;

  memcached_t *st;
  st = user_data->data;

  /* get data from daemon */
  if (memcached_query_daemon (buf, sizeof (buf), user_data) < 0) {
    return -1;
  }

#define FIELD_IS(cnst) \
  (((sizeof(cnst) - 1) == name_len) && (strcmp (cnst, fields[1]) == 0))

  ptr = buf;
  saveptr = NULL;
  while ((line = strtok_r (ptr, "\n\r", &saveptr)) != NULL)
  {
    int name_len;

    ptr = NULL;

    fields_num = strsplit(line, fields, 3);
    if (fields_num != 3)
      continue;

    name_len = strlen(fields[1]);
    if (name_len == 0)
      continue;

    /*
     * For an explanation on these fields please refer to
     * <http://code.sixapart.com/svn/memcached/trunk/server/doc/protocol.txt>
     */

    /*
     * CPU time consumed by the memcached process
     */
    if (FIELD_IS ("rusage_user"))
    {
      rusage_user = atoll (fields[2]);
    }
    else if (FIELD_IS ("rusage_system"))
    {
      rusage_syst = atoll(fields[2]);
    }

    /*
     * Number of threads of this instance
     */
    else if (FIELD_IS ("threads"))
    {
      submit_gauge2 ("ps_count", NULL, NAN, atof (fields[2]), st);
    }

    /*
     * Number of items stored
     */
    else if (FIELD_IS ("curr_items"))
    {
      submit_gauge ("memcached_items", "current", atof (fields[2]), st);
    }

    /*
     * Number of bytes used and available (total - used)
     */
    else if (FIELD_IS ("bytes"))
    {
      bytes_used = atof (fields[2]);
    }
    else if (FIELD_IS ("limit_maxbytes"))
    {
      bytes_total = atof(fields[2]);
    }

    /*
     * Connections
     */
    else if (FIELD_IS ("curr_connections"))
    {
      submit_gauge ("memcached_connections", "current", atof (fields[2]), st);
    }

    /*
     * Commands
     */
    else if ((name_len > 4) && (strncmp (fields[1], "cmd_", 4) == 0))
    {
      const char *name = fields[1] + 4;
      submit_derive ("memcached_command", name, atoll (fields[2]), st);
      if (strcmp (name, "get") == 0)
        gets = atof (fields[2]);
    }

    /*
     * Operations on the cache, i. e. cache hits, cache misses and evictions of items
     */
    else if (FIELD_IS ("get_hits"))
    {
      submit_derive ("memcached_ops", "hits", atoll (fields[2]), st);
      hits = atof (fields[2]);
    }
    else if (FIELD_IS ("get_misses"))
    {
      submit_derive ("memcached_ops", "misses", atoll (fields[2]), st);
    }
    else if (FIELD_IS ("evictions"))
    {
      submit_derive ("memcached_ops", "evictions", atoll (fields[2]), st);
    }

    /*
     * Network traffic
     */
    else if (FIELD_IS ("bytes_read"))
    {
      octets_rx = atoll (fields[2]);
    }
    else if (FIELD_IS ("bytes_written"))
    {
      octets_tx = atoll (fields[2]);
    }
  } /* while ((line = strtok_r (ptr, "\n\r", &saveptr)) != NULL) */

  if (!isnan (bytes_used) && !isnan (bytes_total) && (bytes_used <= bytes_total))
    submit_gauge2 ("df", "cache", bytes_used, bytes_total - bytes_used, st);

  if ((rusage_user != 0) || (rusage_syst != 0))
    submit_derive2 ("ps_cputime", NULL, rusage_user, rusage_syst, st);

  if ((octets_rx != 0) || (octets_tx != 0))
    submit_derive2 ("memcached_octets", NULL, octets_rx, octets_tx, st);

  if (!isnan (gets) && !isnan (hits))
  {
    gauge_t rate = NAN;

    if (gets != 0.0)
      rate = 100.0 * hits / gets;

    submit_gauge ("percent", "hitratio", rate, st);
  }

  return 0;
}

void module_register (void)
{
  plugin_register_complex_config ("memcached", config);
}
