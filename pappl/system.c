//
// System object for the Printer Application Framework
//
// Copyright © 2019-2020 by Michael R Sweet.
// Copyright © 2010-2019 by Apple Inc.
//
// Licensed under Apache License v2.0.  See the file "LICENSE" for more
// information.
//

//
// Include necessary headers...
//

#include "pappl-private.h"
#include "resource-private.h"


//
// Local globals...
//

static bool		shutdown_system = false;
					// Set to true on signal


//
// Local functions...
//

static void		sigterm_handler(int sig);


//
// 'papplSystemCreate()' - Create a system object.
//

pappl_system_t *			// O - System object
papplSystemCreate(
    pappl_soptions_t options,		// I - Server options
    const char       *uuid,		// I - UUID or `NULL` for auto
    const char       *name,		// I - System name
    const char       *hostname,		// I - Hostname or `NULL` for auto
    int              port,		// I - Port number or `0` for auto
    const char       *subtypes,		// I - DNS-SD sub-types or `NULL` for none
    const char       *spooldir,		// I - Spool directory or `NULL` for default
    const char       *logfile,		// I - Log file or `NULL` for default
    pappl_loglevel_t loglevel,		// I - Log level
    const char       *auth_service,	// I - PAM authentication service or `NULL` for none
    bool             tls_only)		// I - Only support TLS connections?
{
  pappl_system_t	*system;	// System object
  const char		*tmpdir;	// Temporary directory


  if (!name)
    return (NULL);

  // Allocate memory...
  if ((system = (pappl_system_t *)calloc(1, sizeof(pappl_system_t))) == NULL)
    return (NULL);

  // Initialize values...
  pthread_rwlock_init(&system->rwlock, NULL);

  system->options         = options;
  system->start_time      = time(NULL);
  system->uuid            = uuid ? strdup(uuid) : NULL;
  system->name            = strdup(name);
  system->hostname        = hostname ? strdup(hostname) : NULL;
  system->port            = port ? port : 8000 + (getuid() % 1000);
  system->directory       = spooldir ? strdup(spooldir) : NULL;
  system->logfd           = 2;
  system->logfile         = logfile ? strdup(logfile) : NULL;
  system->loglevel        = loglevel;
  system->next_client     = 1;
  system->next_printer_id = 1;
  system->tls_only        = tls_only;
  system->admin_gid       = (gid_t)-1;

  if (subtypes)
    system->subtypes = strdup(subtypes);
  if (auth_service)
    system->auth_service = strdup(auth_service);

  // Initialize DNS-SD as needed...
  _papplSystemInitDNSSD(system);

  // Make sure the system name is initialized...
  if (!system->hostname)
  {
    char	temp[1024];		// Temporary hostname string

#ifdef HAVE_AVAHI
    const char *avahi_name = avahi_client_get_host_name_fqdn(system->dns_sd_client);
					// mDNS hostname

    if (avahi_name)
      system->hostname = strdup(avahi_name);
    else
#endif /* HAVE_AVAHI */

    system->hostname = strdup(httpGetHostname(NULL, temp, sizeof(temp)));
  }

  // Set the system TLS credentials...
  cupsSetServerCredentials(NULL, system->hostname, 1);

  // Make sure the system UUID is set...
  if (!system->uuid)
  {
    char	newuuid[64];		// UUID string

    _papplSystemMakeUUID(system, NULL, 0, newuuid, sizeof(newuuid));
    system->uuid      = strdup(newuuid);
    system->save_time = time(NULL);
  }

  // See if the spool directory can be created...
  if ((tmpdir = getenv("TMPDIR")) == NULL)
#ifdef __APPLE__
    tmpdir = "/private/tmp";
#else
    tmpdir = "/tmp";
#endif // __APPLE__

  if (!system->directory)
  {
    char	newspooldir[256];	// Spool directory

    // TODO: May need a different default spool directory...
    snprintf(newspooldir, sizeof(newspooldir), "%s/pappl%d.d", tmpdir, (int)getuid());
    system->directory = strdup(newspooldir);
  }

  if (mkdir(system->directory, 0700) && errno != EEXIST)
  {
    perror(system->directory);
    goto fatal;
  }

  // Initialize logging...
  if (system->loglevel == PAPPL_LOGLEVEL_UNSPEC)
    system->loglevel = PAPPL_LOGLEVEL_ERROR;

  if (!system->logfile)
  {
    // Default log file is $TMPDIR/papplUID.log...
    char newlogfile[256];		// Log filename

    snprintf(newlogfile, sizeof(newlogfile), "%s/pappl%d.log", tmpdir, (int)getuid());

    system->logfile = strdup(newlogfile);
  }

  if (!strcmp(system->logfile, "syslog"))
  {
    // Log to syslog...
    system->logfd = -1;
  }
  else if (!strcmp(system->logfile, "-"))
  {
    // Log to stderr...
    system->logfd = 2;
  }
  else if ((system->logfd = open(system->logfile, O_CREAT | O_WRONLY | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600)) < 0)
  {
    // Fallback to stderr if we can't open the log file...
    perror(system->logfile);

    system->logfd = 2;
  }

  // Initialize authentication...
  if (system->auth_service && !strcmp(system->auth_service, "none"))
  {
    free(system->auth_service);
    system->auth_service = NULL;
  }

  return (system);

  // If we get here, something went wrong...
  fatal:

  papplSystemDelete(system);

  return (NULL);
}


//
// 'papplSystemDelete()' - Delete a system object.
//

void
papplSystemDelete(
    pappl_system_t *system)		// I - System object
{
  int	i;				// Looping var


  if (!system)
    return;

  _papplSystemUnregisterDNSSDNoLock(system);

  free(system->uuid);
  free(system->name);
  free(system->dns_sd_name);
  free(system->hostname);
  free(system->firmware_name);
  free(system->server_header);
  free(system->directory);
  free(system->logfile);
  free(system->subtypes);
  free(system->auth_service);
  free(system->admin_group);
  free(system->default_print_group);
  free(system->session_key);

  if (system->logfd >= 0 && system->logfd != 2)
    close(system->logfd);

  for (i = 0; i < system->num_listeners; i ++)
    close(system->listeners[i].fd);

  cupsArrayDelete(system->printers);

  pthread_rwlock_destroy(&system->rwlock);

  free(system);
}


//
// 'papplSystemRun()' - Run the printer service.
//

void
papplSystemRun(pappl_system_t *system)// I - System
{
  int			i,		// Looping var
			count;		// Number of listeners that fired
  pappl_client_t	*client;	// New client
  char			header[HTTP_MAX_VALUE];
					// Server: header value


  // Range check...
  if (!system)
    return;

  if (!system->is_running)
  {
    papplLog(system, PAPPL_LOGLEVEL_FATAL, "Tried to run main loop when already running.");
    return;
  }

  if (system->num_listeners == 0)
  {
    papplLog(system, PAPPL_LOGLEVEL_FATAL, "Tried to run main loop without listeners.");
    return;
  }

  system->is_running = true;

  // Add fallback resources...
  papplSystemAddResourceData(system, "/apple-touch-icon.png", "image/png", apple_touch_icon_png, sizeof(apple_touch_icon_png));
  papplSystemAddResourceData(system, "/nav-icon.png", "image/png", icon_sm_png, sizeof(icon_sm_png));
  papplSystemAddResourceData(system, "/icon-lg.png", "image/png", icon_lg_png, sizeof(icon_lg_png));
  papplSystemAddResourceData(system, "/icon-md.png", "image/png", icon_md_png, sizeof(icon_md_png));
  papplSystemAddResourceData(system, "/icon-sm.png", "image/png", icon_sm_png, sizeof(icon_sm_png));
  papplSystemAddResourceString(system, "/style.css", "text/css", style_css);

  // Catch important signals...
  papplLog(system, PAPPL_LOGLEVEL_INFO, "Starting main loop.");

  signal(SIGTERM, sigterm_handler);
  signal(SIGINT, sigterm_handler);

  // Set the server header...
  free(system->server_header);
  if (system->firmware_name)
    snprintf(header, sizeof(header), "%s/%s PAPPL/" PAPPL_VERSION " CUPS IPP/2.0", system->firmware_name, system->firmware_sversion);
  else
    strlcpy(header, "Unknown PAPPL/" PAPPL_VERSION " CUPS IPP/2.0", sizeof(header));
  system->server_header = strdup(header);

  // Loop until we are shutdown or have a hard error...
  while (!shutdown_system)
  {
    if ((count = poll(system->listeners, (nfds_t)system->num_listeners, 1000)) < 0 && errno != EINTR && errno != EAGAIN)
    {
      papplLog(system, PAPPL_LOGLEVEL_ERROR, "Unable to accept new connections: %s", strerror(errno));
      break;
    }

    if (count > 0)
    {
      // Accept client connections as needed...
      for (i = 0; i < system->num_listeners; i ++)
      {
	if (system->listeners[i].revents & POLLIN)
	{
	  if ((client = papplClientCreate(system, system->listeners[i].fd)) != NULL)
	  {
	    if (pthread_create(&client->thread_id, NULL, (void *(*)(void *))_papplClientRun, client))
	    {
	      // Unable to create client thread...
	      papplLog(system, PAPPL_LOGLEVEL_ERROR, "Unable to create client thread: %s", strerror(errno));
	      papplClientDelete(client);
	    }
	    else
	    {
	      // Detach the main thread from the client thread to prevent hangs...
	      pthread_detach(client->thread_id);
	    }
	  }
	}
      }
    }

    if (system->dns_sd_any_collision)
    {
      // Handle name collisions...
      pappl_printer_t	*printer;	// Current printer

      pthread_rwlock_rdlock(&system->rwlock);

      if (system->dns_sd_collision)
        _papplSystemRegisterDNSSDNoLock(system);

      for (printer = (pappl_printer_t *)cupsArrayFirst(system->printers); printer; printer = (pappl_printer_t *)cupsArrayNext(system->printers))
      {
        if (printer->dns_sd_collision)
          _papplPrinterRegisterDNSSDNoLock(printer);
      }

      system->dns_sd_any_collision = false;
      pthread_rwlock_unlock(&system->rwlock);
    }

    if (system->save_time)
    {
      if (system->save_cb)
      {
        // Save the configuration...
	(system->save_cb)(system, system->save_cbdata);
      }

      system->save_time = 0;
    }

    if (system->shutdown_time)
    {
      // Shutdown requested, see if we can do so safely...
      int		count = 0;	// Number of active jobs
      pappl_printer_t	*printer;	// Current printer

      // Force shutdown after 60 seconds
      if ((time(NULL) - system->shutdown_time) > 60)
        break;

      // Otherwise shutdown immediately if there are no more active jobs...
      pthread_rwlock_rdlock(&system->rwlock);
      for (printer = (pappl_printer_t *)cupsArrayFirst(system->printers); printer; printer = (pappl_printer_t *)cupsArrayNext(system->printers))
      {
        pthread_rwlock_rdlock(&printer->rwlock);
        count += cupsArrayCount(printer->active_jobs);
        pthread_rwlock_unlock(&printer->rwlock);
      }
      pthread_rwlock_unlock(&system->rwlock);

      if (count == 0)
        break;
    }

    // Clean out old jobs...
    if (system->clean_time && time(NULL) >= system->clean_time)
      papplSystemCleanJobs(system);
  }

  papplLog(system, PAPPL_LOGLEVEL_INFO, "Shutting down main loop.");

  if (system->save_time && system->save_cb)
  {
    // Save the configuration...
    (system->save_cb)(system, system->save_cbdata);
  }

  system->is_running = false;
}


//
// '_papplSystemMakeUUID()' - Make a UUID for a system, printer, or job.
//
// Unlike httpAssembleUUID, this function does not introduce random data for
// printers so the UUIDs are stable.
//

char *					// I - UUID string
_papplSystemMakeUUID(
    pappl_system_t *system,		// I - System
    const char     *printer_name,	// I - Printer name or `NULL` for none
    int            job_id,		// I - Job ID or `0` for none
    char           *buffer,		// I - String buffer
    size_t         bufsize)		// I - Size of buffer
{
  char			data[1024];	// Source string for MD5
  unsigned char		sha256[32];	// SHA-256 digest/sum


  // Build a version 3 UUID conforming to RFC 4122.
  //
  // Start with the SHA-256 sum of the hostname, port, object name and
  // number, and some random data on the end for jobs (to avoid duplicates).
  if (printer_name && job_id)
    snprintf(data, sizeof(data), "_PAPPL_JOB_:%s:%d:%s:%d:%08x", system->uuid, system->port, printer_name, job_id, _papplGetRand());
  else if (printer_name)
    snprintf(data, sizeof(data), "_PAPPL_PRINTER_:%s:%d:%s", system->uuid, system->port, printer_name);
  else
    snprintf(data, sizeof(data), "_PAPPL_SYSTEM_:%08x:%08x:%08x:%08x", _papplGetRand(), _papplGetRand(), _papplGetRand(), _papplGetRand());

  cupsHashData("sha-256", (unsigned char *)data, strlen(data), sha256, sizeof(sha256));

  // Generate the UUID from the SHA-256...
  snprintf(buffer, bufsize, "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", sha256[0], sha256[1], sha256[3], sha256[4], sha256[5], sha256[6], (sha256[10] & 15) | 0x30, sha256[11], (sha256[15] & 0x3f) | 0x40, sha256[16], sha256[20], sha256[21], sha256[25], sha256[26], sha256[30], sha256[31]);

  return (buffer);
}


//
// 'sigterm_handler()' - SIGTERM handler.
//

static void
sigterm_handler(int sig)		// I - Signal (ignored)
{
  shutdown_system = true;
}
