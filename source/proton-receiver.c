/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>

#include "proton/reactor.h"
#include "proton/message.h"
#include "proton/connection.h"
#include "proton/session.h"
#include "proton/link.h"
#include "proton/delivery.h"
#include "proton/event.h"
#include "proton/handlers.h"
#include "proton/transport.h"
#include "proton/url.h"
#include <time.h>

#define ROW_START()        do {} while (0)

#define COL_HDR(NAME)      printf("| %20.20s", (NAME))

#define COL_INT(NAME,VAL)  printf("| %20d", (VAL))
#define COL_TIME(NAME,VAL) printf("| %20ld ", (VAL))
#define COL_CLOCK(NAME,VALS,VALD) printf("| %20s.%ld",(VALS),(VALD))
#define COL_STR(NAME,VAL)  printf("| %20s ", (VAL))
#define ROW_END()     do {                 \
                                printf("\n");   \
                                rows_written++; \
                        } while (0)


static void fatal (const char *str)
{
  if (errno)
    perror(str);
  else
    fprintf(stderr, "Error: %s\n", str);

  fflush(stderr);
  exit(1);
}

static int done = 0;
static void
stop (int sig)
{
  done = 1;
}

// return wall clock time not in msec but in usec
static time_t now ()
{
  
    struct timeval tv;
    int rc = gettimeofday(&tv, NULL);
    if (rc) fatal("gettimeofday() failed");
    return (tv.tv_sec * 1000) + (time_t)(tv.tv_usec / 1000);
}


// Example application data.  This data will be instantiated in the event
// handler, and is available during event processing.  In this example it
// holds configuration and state information.
//
typedef struct
{
  int debug;
  const char *host_address;
  int message_count;
  int pre_fetch;
  const char *target;
  uint32_t display_interval_sec;
  int latency;
  long int last_then; 
  int dump_csv;
  pn_link_t *receiver;
  char *decode_buffer;
  size_t buffer_len;
  pn_message_t *message;
  uint64_t expected_sequence;
  unsigned dropped_msgs;      // based on sequence
  unsigned duplicate_msgs;    // based on sequence
  time_t start;
  unsigned long received_count;
  // these are all in msec
  float max_latency;
  float min_latency;
  float total_latency;
  /* distribution in msec over 0..9, 10-99, 100-999, ... 99999 */
#define MAX_ORDER 4
  unsigned distribution[MAX_ORDER][100];
  unsigned overflow;
} app_data_t;

// helper to pull pointer to app_data_t instance out of the pn_handler_t
//
#define GET_APP_DATA(handler) ((app_data_t *)pn_handler_mem(handler))

// Called when reactor exits to clean up app_data
//
static void delete_handler (pn_handler_t * handler)
{
  app_data_t *app_data = GET_APP_DATA (handler);
  if (app_data->receiver)
    {
      pn_decref (app_data->receiver);
      app_data->receiver = NULL;
    }

  if (app_data->message)
    {
      pn_decref (app_data->message);
      app_data->message = NULL;
    }

  free (app_data->decode_buffer);
}

static void formatLocaltime (unsigned long long  _time)
{
   time_t l_time;
   l_time=_time/1000;
   char *timestamp = malloc (sizeof (char) * 64);
   strftime(timestamp , 64, "%c", localtime(&l_time));
  //strftime(timestamp , 64, "%Y:%m:%d %H:%M:%S", localtime(&l_time));
   printf("| %20s.%llu",timestamp,_time%1000);

  
}

static void print_latency (app_data_t * data, time_t msecs, time_t then, time_t now)
{


  static int rows_written = 0;
  long int pause_time=0;
  if(data->last_then)
     pause_time=then-data->last_then;
  data->last_then=then;


  if (!rows_written)
    {
      ROW_START ();
      COL_HDR ("THEN DATE");
      COL_HDR ("NOW DATA");
      COL_HDR ("COUNT");
      COL_HDR ("THEN");
      COL_HDR ("NOW");
      COL_HDR("PAUSE_TIME");
      COL_HDR ("LATENCY");
      ROW_END ();

    }

  char buffer[1024];
  size_t buffsize = sizeof (buffer);
  pn_data_t *body = pn_message_body (data->message);
  pn_data_format (body, buffer, &buffsize);
  ROW_START ();
  formatLocaltime(then);
  formatLocaltime(now);
  COL_INT ("count", rows_written);
  COL_TIME ("then_msecs", then);
  COL_TIME ("now_msecs", now);
  COL_TIME ("pause_time",pause_time);
  COL_TIME ("latency", msecs);
  ROW_END ();

}

static void update_latency (app_data_t * data, time_t msecs)
{
  if (data->debug)
    fprintf (stdout, "latency %ld\n", msecs);

  if (msecs > data->max_latency)
    data->max_latency = msecs;
  if (data->min_latency == 0 || msecs < data->min_latency)
    data->min_latency = msecs;
  data->total_latency += msecs;

  if (msecs < 100)
    {
      data->distribution[0][msecs]++;
    }
  else if (msecs < 1000)
    {
      data->distribution[1][msecs / 10]++;
    }
  else if (msecs < 10000)
    {
      data->distribution[2][msecs / 100]++;
    }
  else if (msecs < 10000)
    {
      data->distribution[3][msecs / 1000]++;
    }
  else
    {
      data->overflow++;
    }
}


static void display_latency (app_data_t * data)
{
  static unsigned long last_count = 0;

  if (data->received_count == 0)
    return;

  if (data->dump_csv)
    {
      printf ("Messages, Latency (msec)\n");
    }
  else
    {
      printf ("\n\nLatency:   (%lu msgs received", data->received_count);
      if (data->display_interval_sec && data->received_count > last_count)
	printf (", %lu msgs/sec)\n",
		(data->received_count -
		 last_count) / data->display_interval_sec);
      else
	printf (")\n");

      printf ("  Average: %f msec\n"
	      "  Minimum: %f msec\n"
	      "  Maximum: %f msec\n",
	      (data->received_count)
	      ? (data->total_latency / data->received_count)
	      : 0, data->min_latency, data->max_latency);
      printf ("  Distribution:\n");
      if (data->dropped_msgs)
          printf("  Dropped: %u\n", data->dropped_msgs);
      if (data->duplicate_msgs)
          printf("  Duplicate: %u\n", data->duplicate_msgs);
    }

  unsigned power = 1;
  int order;
  int i;
  for (order = 0; order < MAX_ORDER; ++order)
    {
      for (i = 0; i < 100; ++i)
	{
	  if (data->distribution[order][i] > 0)
	    {
	      if (data->dump_csv)
		{
		  printf ("%u, %u\n",
			  data->distribution[order][i], power * i);
		}
	      else
		{
		  printf ("    msecs: %d  messages: %u\n",
			  power * i, data->distribution[order][i]);
		}
	    }
	}
      power *= 10;
    }

  if (!data->dump_csv && data->overflow > 0)
    printf ("> 100 sec: %u\n", data->overflow);

  last_count = data->received_count;
}


/* Process interesting events posted by the reactor.
 * This is called from pn_reactor_process()
 */
static void event_handler (pn_handler_t * handler,
	       pn_event_t * event, pn_event_type_t type)
{
  app_data_t *data = GET_APP_DATA (handler);

  switch (type)
    {

    case PN_CONNECTION_INIT:
      {
	// reactor is ready, create a link to the broker
	pn_connection_t *conn;
	pn_session_t *ssn;

	conn = pn_event_connection (event);
	pn_connection_open (conn);
	ssn = pn_session (conn);
	pn_session_open (ssn);
	data->receiver = pn_receiver (ssn, "MyReceiver");
	pn_terminus_set_address (pn_link_source (data->receiver),
				 data->target);
	pn_link_open (data->receiver);
	// cannot receive without granting credit:
	pn_link_flow (data->receiver, data->pre_fetch);
      }
      break;

    case PN_LINK_REMOTE_OPEN:
      {
        // discard any messages generated before the link becomes active
        data->start = now();
      }
      break;

    case PN_LINK_REMOTE_CLOSE:
      {
	// shutdown - clean up connection and session
	// This will cause the main loop to eventually exit
	pn_session_close (pn_event_session (event));
	pn_connection_close (pn_event_connection (event));
      }
      break;

    case PN_DELIVERY:
      {
	// A message has been received
	pn_delivery_t *dlv = pn_event_delivery (event);
	if (pn_delivery_readable (dlv) && !pn_delivery_partial (dlv))
	  {
	    // A full message has arrived
	    if (data->latency)
	      {
		ssize_t len;
		time_t _now = now ();
		// try to decode the message body
		if (pn_delivery_pending (dlv) > data->buffer_len)
		  {
		    data->buffer_len = pn_delivery_pending (dlv);
		    free (data->decode_buffer);
		    data->decode_buffer = (char *) malloc (data->buffer_len);
		    if (!data->decode_buffer)
		      fatal ("cannot allocate buffer");
		  }

		// read in the raw data
		len =
		  pn_link_recv (data->receiver, data->decode_buffer,
				data->buffer_len);
		if (len > 0)
		  {
		    // decode it into a proton message
		    pn_message_clear (data->message);
		    if (PN_OK ==
			pn_message_decode (data->message, data->decode_buffer,
					   len))
		      {
                        pn_atom_t id = pn_message_get_id(data->message);
			time_t _then =
			  pn_message_get_creation_time (data->message);
			if (_then && _then >= data->start &&_now >= _then)
			  {
			    print_latency (data, _now - _then, _then, _now);
			    update_latency (data, _now - _then);
			  }

                        if (id.type != PN_ULONG) fatal("Bad sequence type: expected ulong");
                        if (id.u.as_long == data->expected_sequence)
                          {
                            ++data->expected_sequence;
                          }
                        else
                          {
                            if (data->debug)
                                fprintf(stdout,
                                        "Sequence mismatch! Expected %lu, got %lu\n",
                                        data->expected_sequence, id.u.as_long);
                            if (id.u.as_long > data->expected_sequence)
                              {
                                data->dropped_msgs += id.u.as_long - data->expected_sequence;
                                data->expected_sequence = id.u.as_long + 1;
                              }
                            else
                              {
                                // older sequence #, likely re-transmit
                                ++data->duplicate_msgs;
                                // leave expected_sequence alone - should
                                // 'catch up'
                              }
                          }
		      }
		  }
	      }

	    if (data->debug)
	      fprintf (stdout, "Message received!\n");
	    ++data->received_count;

	    if (!pn_delivery_settled (dlv))
	      {
		// remote has not settled, so it is tracking the delivery.  Ack
		// it.
		pn_delivery_update (dlv, PN_ACCEPTED);
	      }

	    // done with the delivery, move to the next and free it
	    pn_link_advance (data->receiver);
	    pn_delivery_settle (dlv);	// dlv is now freed

	    // replenish credit if it drops below 1/2 prefetch level
	    int credit = pn_link_credit (data->receiver);
	    if (credit < data->pre_fetch / 2)
	      pn_link_flow (data->receiver, data->pre_fetch - credit);

	    if (data->message_count > 0 && --data->message_count == 0)
	      {
		// done receiving, close the endpoints
		pn_link_close (data->receiver);
	      }
	  }
      }
      break;

    case PN_TRANSPORT_ERROR:
      {
	// The connection to the peer failed.
	pn_transport_t *tport = pn_event_transport (event);
	pn_condition_t *cond = pn_transport_condition (tport);
	fprintf (stderr, "Network transport failed!\n");
	if (pn_condition_is_set (cond))
	  {
	    const char *name = pn_condition_get_name (cond);
	    const char *desc = pn_condition_get_description (cond);
	    fprintf (stderr, "    Error: %s  Description: %s\n",
		     (name) ? name : "<error name not provided>",
		     (desc) ? desc : "<no description provided>");
	  }
	// pn_reactor_process() will exit with a false return value, stopping
	// the main loop.
      } break;

    default:
      // ignore the rest
      break;
    }
}

static void usage (const char *name)
{
  printf ("Usage: %s <options>\n", name);
  printf ("-a \tThe host address [localhost:5672]\n");
  printf ("-c \t# of messages to receive (-1==forever) [1]\n");
  printf ("-t \tTopic address [topic]\n");
  printf ("-i \tDisplay interval [0]\n");
  printf ("-v \tIncrease debug verbosity\n");
  printf ("-l \tEnable latency measurement\n");
  printf ("-u \tOutput in CSV format\n");
  printf ("-p \tpre-fetch window size [100]\n");
  printf("-S \tExpected first sequence # [0]\n");
}


/* parse command line options */
static int parse_args (int argc, char *argv[], app_data_t * app)
{
  int c;

  // set defaults:
  app->debug = 0;
  app->host_address = "localhost:5672";
  app->message_count = 1;
  app->pre_fetch = 100;
  app->target = "topic";
  app->display_interval_sec = 0;
  app->latency = 0;
  app->expected_sequence = 0;

  opterr = 0;
  while ((c = getopt (argc, argv, "a:c:t:i:p:S:luv")) != -1)
    {
      switch (c)
	{
	case 'h':
	  usage (argv[0]);
	  return -1;
	case 'a':
	  app->host_address = optarg;
	  break;
	case 'c':
	  app->message_count = atoi (optarg);
	  break;
	case 't':
	  app->target = optarg;
	  break;
	case 'i':
	  app->display_interval_sec = atoi (optarg);
	  break;
	case 'v':
	  app->debug++;
	  break;
	case 'l':
	  app->latency = 1;
	  break;
	case 'p':
	  app->pre_fetch = atoi (optarg);
	  break;
	case 'u':
	  app->dump_csv = 1;
	  break;
        case 'S':
          app->expected_sequence = atol(optarg);
          break;
	default:
	  fprintf (stderr, "Unknown option: %c\n", c);
	  usage (argv[0]);
	  return -1;
	}
    }

  if (app->pre_fetch <= 0)
    fatal ("pre-fetch must be >= zero");

  if (app->display_interval_sec && app->latency == 0)
    fatal ("must enable latency if display enabled");

  if (app->debug)
    {
      fprintf (stdout, "Configuration:\n"
	       " Bus: %s\n"
	       " Count: %d\n"
	       " Topic: %s\n"
	       " Display Intrv: %d\n"
	       " Latency: %s\n"
	       " Pre-fetch: %d\n",
	       app->host_address, app->message_count, app->target,
	       app->display_interval_sec,
	       (app->latency) ? "enabled" : "disabled", app->pre_fetch);
    }

  return 0;
}


int main (int argc, char *argv[])
{
  pn_reactor_t *reactor = NULL;
  pn_url_t *url = NULL;
  pn_connection_t *conn = NULL;

  errno = 0;
  signal (SIGINT, stop);

  /* Create a handler for the connection's events.  event_handler() will be
   * called for each event and delete_handler will be called when the
   * connection is released.  The handler will allocate an app_data_t
   * instance which can be accessed when the event_handler is called.
   */
  pn_handler_t *handler = pn_handler_new (event_handler,
					  sizeof (app_data_t),
					  delete_handler);

  /* set up the application data with defaults */
  app_data_t *app_data = GET_APP_DATA (handler);
  memset (app_data, 0, sizeof (app_data_t));
  app_data->buffer_len = 64;
  app_data->decode_buffer = malloc (app_data->buffer_len);
  if (!app_data->decode_buffer)
    fatal ("Cannot allocate encode buffer");
  app_data->message = pn_message ();
  if (!app_data->message)
    fatal ("Message allocation failed");
  if (parse_args (argc, argv, app_data) != 0)
    exit (-1);

  /* Attach the pn_handshaker() handler.  This handler deals with endpoint
   * events from the peer so we don't have to.
   */
  {
    pn_handler_t *handshaker = pn_handshaker ();
    pn_handler_add (handler, handshaker);
    pn_decref (handshaker);
  }

  reactor = pn_reactor ();

  url = pn_url_parse (app_data->host_address);
  if (url == NULL)
    {
      fprintf (stderr, "Invalid host address %s\n", app_data->host_address);
      exit (1);
    }
  conn = pn_reactor_connection_to_host (reactor,
					pn_url_get_host (url),
					pn_url_get_port (url), handler);
  if (!conn)
    fatal ("cannot create connection");

  pn_decref (url);
  pn_decref (handler);

  // the container name should be unique for each client
  // attached to the broker
  {
    char hname[HOST_NAME_MAX + 1];
    char cname[256];

    gethostname (hname, sizeof (hname));
    snprintf (cname, sizeof (cname), "receiver-container-%s-%d-%d",
	      hname, getpid (), rand ());

    pn_connection_set_container (conn, cname);
  }

  // make pn_reactor_process() wake up every second
  pn_reactor_set_timeout (reactor, 1000);
  pn_reactor_start (reactor);

  time_t last_display = now ();
  time_t display_interval = app_data->display_interval_sec * 1000;

  // pn_reactor_process() returns 'true' until the connection is shut down.
  while (!done && pn_reactor_process (reactor))
    {
      if (display_interval)
	{
	  time_t _now = now ();
	  if (_now >= last_display + display_interval)
	    {
	      last_display = _now;
	      display_latency (app_data);
	    }
	}
    }

  if (app_data->latency)
    {
      display_latency (app_data);
    }

  pn_decref (reactor);

  return 0;
}
