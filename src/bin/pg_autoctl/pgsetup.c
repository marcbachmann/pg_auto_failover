/*
 * src/bin/pg_autoctl/pgsetup.c
 *   Discovers a PostgreSQL setup by calling pg_controldata and reading
 *   postmaster.pid file, getting clues from the process environment and from
 *   user given hints (options).
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 */

#include <inttypes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "parson.h"

#include "postgres_fe.h"
#include "pqexpbuffer.h"

#include "defaults.h"
#include "env_utils.h"
#include "pgctl.h"
#include "log.h"
#include "signals.h"
#include "string_utils.h"


static bool get_pgpid(PostgresSetup *pgSetup, bool pg_is_not_running_is_ok);
static PostmasterStatus pmStatusFromString(const char *postmasterStatus);
static char *pmStatusToString(PostmasterStatus pm_status);


/*
 * Discover PostgreSQL environment from given clues, or a partial setup.
 *
 * This routines check the PATH for pg_ctl, and is ok when there's a single
 * entry in found. It then uses either given PGDATA or the environment value
 * and runs a pg_controldata to get system identifier and PostgreSQL version
 * numbers. Then it reads PGDATA/postmaster.pid to get the pid and the port of
 * the running PostgreSQL server. Then it can connects to it and see if it's in
 * recovery.
 */
bool
pg_setup_init(PostgresSetup *pgSetup,
			  PostgresSetup *options,
			  bool missing_pgdata_is_ok,
			  bool pg_is_not_running_is_ok)
{
	bool pgIsReady = false;
	int errors = 0;

	/*
	 * Make sure that we keep the options->nodeKind in the pgSetup.
	 */
	pgSetup->pgKind = options->pgKind;

	/*
	 * Make sure that we keep the SSL options too.
	 */
	pgSetup->ssl.active = options->ssl.active;
	pgSetup->ssl.createSelfSignedCert = options->ssl.createSelfSignedCert;
	pgSetup->ssl.sslMode = options->ssl.sslMode;
	strlcpy(pgSetup->ssl.sslModeStr, options->ssl.sslModeStr, SSL_MODE_STRLEN);
	strlcpy(pgSetup->ssl.caFile, options->ssl.caFile, MAXPGPATH);
	strlcpy(pgSetup->ssl.crlFile, options->ssl.crlFile, MAXPGPATH);
	strlcpy(pgSetup->ssl.serverCert, options->ssl.serverCert, MAXPGPATH);
	strlcpy(pgSetup->ssl.serverKey, options->ssl.serverKey, MAXPGPATH);

	/* check or find pg_ctl */
	if (options->pg_ctl[0] != '\0')
	{
		char *version = pg_ctl_version(options->pg_ctl);

		if (version == NULL)
		{
			/* we already logged about it */
			return false;
		}

		strlcpy(pgSetup->pg_ctl, options->pg_ctl, MAXPGPATH);
		strlcpy(pgSetup->pg_version, version, PG_VERSION_STRING_MAX);
		free(version);

		log_debug("pg_setup_init: %s version %s",
				  pgSetup->pg_ctl, pgSetup->pg_version);
	}
	else
	{
		int count_of_pg_ctl = config_find_pg_ctl(pgSetup);

		if (count_of_pg_ctl != 1)
		{
			/* config_find_pg_ctl already logged errors */
			errors++;
		}

		if (count_of_pg_ctl > 1)
		{
			log_error("Found several pg_ctl in PATH, please provide --pgctl");
		}
	}

	/* check or find PGDATA */
	if (options->pgdata[0] != '\0')
	{
		strlcpy(pgSetup->pgdata, options->pgdata, MAXPGPATH);
	}
	else
	{
		if (!get_env_pgdata(pgSetup->pgdata))
		{
			log_error("Failed to set PGDATA either from the environment "
					  "or from --pgdata");
			errors++;
		}
	}

	/*
	 * We want to know if PostgreSQL is running, and if that's the case, we
	 * want to discover all we can about its properties: port, pid, socket
	 * directory, is_in_recovery, etc.
	 */
	if (errors == 0)
	{
		if (!missing_pgdata_is_ok && !directory_exists(pgSetup->pgdata))
		{
			log_fatal("Database directory \"%s\" not found", pgSetup->pgdata);
			return false;
		}

		pg_controldata(pgSetup, missing_pgdata_is_ok);

		if (pgSetup->control.pg_control_version == 0)
		{
			/* we already logged about it */
			if (!missing_pgdata_is_ok)
			{
				errors++;
			}
		}
		else
		{
			log_debug("Found PostgreSQL system %" PRIu64 " at \"%s\", "
					  "version %u, catalog version %u",
					  pgSetup->control.system_identifier,
					  pgSetup->pgdata,
					  pgSetup->control.pg_control_version,
					  pgSetup->control.catalog_version_no);
		}
	}

	/* check of find username */
	if (options->username[0] != '\0')
	{
		strlcpy(pgSetup->username, options->username, NAMEDATALEN);
	}
	else
	{
		/*
		 * If a PGUSER environment variable is defined, take the value from
		 * there. Otherwise we attempt to connect without username. In that
		 * case the username will be determined based on the current user.
		 */
		if (!get_env_copy_with_fallback("PGUSER", pgSetup->username, NAMEDATALEN, ""))
		{
			/* errors have already been logged */
			return false;
		}
	}

	/* check or find dbname */
	if (options->dbname[0] != '\0')
	{
		strlcpy(pgSetup->dbname, options->dbname, NAMEDATALEN);
	}
	else
	{
		/*
		 * If a PGDATABASE environment variable is defined, take the value from
		 * there. Otherwise we attempt to connect without a database name, and
		 * the default will use the username here instead.
		 */
		if (!get_env_copy_with_fallback("PGDATABASE", pgSetup->dbname, NAMEDATALEN,
										DEFAULT_DATABASE_NAME))
		{
			/* errors have already been logged */
			return false;
		}
	}

	/*
	 * Read the postmaster.pid file to find out pid, port and unix socket
	 * directory of a running PostgreSQL instance.
	 */
	pgIsReady = pg_setup_is_ready(pgSetup, pg_is_not_running_is_ok);

	if (!pgIsReady && !pg_is_not_running_is_ok)
	{
		/* errors have already been logged */
		errors++;
	}

	/*
	 * check or find PGHOST
	 *
	 * By order of preference, we use:
	 *  --pghost     command line option
	 *  PGDATA/postmaster.pid
	 *  PGHOST       from the environment
	 */
	if (options->pghost[0] != '\0')
	{
		strlcpy(pgSetup->pghost, options->pghost, _POSIX_HOST_NAME_MAX);
	}
	else
	{
		/* read_pg_pidfile might already have set pghost for us */
		if (pgSetup->pghost[0] == '\0')
		{
			/*
			 * We can (at least try to) connect without host= in the connection
			 * string, so missing PGHOST and --pghost isn't an error.
			 */
			if (!get_env_copy_with_fallback("PGHOST", pgSetup->pghost,
											_POSIX_HOST_NAME_MAX, ""))
			{
				/* errors have already been logged */
				return false;
			}

		}
	}

	/*
	 * In test environment we might disable unix socket directories. In that
	 * case, we need to have an host to connect to, accepting to connect
	 * without host= in the connection string is not going to cut it.
	 */
	if (IS_EMPTY_STRING_BUFFER(pgSetup->pghost))
	{
		if (env_found_empty("PG_REGRESS_SOCK_DIR"))
		{
			log_error("PG_REGRESS_SOCK_DIR is set to \"\" to disable unix "
					  "socket directories, now --pghost is mandatory, "
					  "but unset.");
			errors++;
		}
	}

	/* check or find PGPORT
	 *
	 * By order or preference, we use:
	 *   --pgport       command line option
	 *   PGDATA/postmaster.pid
	 *   PGPORT         from the environment
	 *   POSTGRES_PORT  from our hard coded defaults (5432, see defaults.h)
	 */
	if (options->pgport > 0)
	{
		pgSetup->pgport = options->pgport;
	}
	else
	{
		/* if we have a running cluster, just use its port */
		if (pgSetup->pidFile.pid > 0 && pgSetup->pidFile.port > 0)
		{
			pgSetup->pgport = pgSetup->pidFile.port;
		}
		else
		{
			/*
			 * no running cluster, what about using PGPORT then?
			 */
			pgSetup->pgport = pgsetup_get_pgport();
		}
	}

	/* Set proxy port */
	if (options->proxyport > 0)
	{
		pgSetup->proxyport = options->proxyport;
	}


	/*
	 * If --listen is given, then set our listen_addresses to this value
	 */
	if (!IS_EMPTY_STRING_BUFFER(options->listen_addresses))
	{
		strlcpy(pgSetup->listen_addresses,
				options->listen_addresses, MAXPGPATH);
	}
	else
	{
		/*
		 * The default listen_addresses is '*', because we are dealing with a
		 * cluster setup and 'localhost' isn't going to cut it: the monitor and
		 * the coordinator nodes need to be able to connect to our local node
		 * using a connection string with nodename:port.
		 */
		strlcpy(pgSetup->listen_addresses,
				POSTGRES_DEFAULT_LISTEN_ADDRESSES, MAXPGPATH);
	}


	/*
	 * If --auth is given, then set our authMethod to this value
	 * otherwise it remains empty
	 */
	if (!IS_EMPTY_STRING_BUFFER(options->authMethod))
	{
		strlcpy(pgSetup->authMethod,
				options->authMethod, NAMEDATALEN);
	}

	pgSetup->settings = options->settings;

	/*
	 * And we always double-check with PGDATA/postmaster.pid if we have it, and
	 * we should have it in the normal/expected case.
	 */
	if (pgIsReady
		&& pgSetup->pidFile.pid > 0
		&& pgSetup->pgport != pgSetup->pidFile.port)
	{
		log_error("Given --pgport %d doesn't match PostgreSQL "
				  "port %d from \"%s/postmaster.pid\"",
				  pgSetup->pgport, pgSetup->pidFile.port, pgSetup->pgdata);
		errors++;
	}

	/*
	 * If PostgreSQL is running, register if it's in recovery or not.
	 */
	if (pgSetup->control.pg_control_version > 0
		&& pgSetup->pidFile.port > 0
		&& pgSetup->pgport == pgSetup->pidFile.port)
	{
		PGSQL pgsql = { 0 };
		char connInfo[MAXCONNINFO];
		char dbname[NAMEDATALEN];

		/*
		 * Sometimes `pg_ctl start` returns with success and Postgres is still
		 * in crash recovery replaying WAL files, in the "starting" state
		 * rather than the "ready" state.
		 *
		 * In that case, we wait until Postgres is ready for connections. The
		 * whole pg_autoctl code is expecting to be able to connect to
		 * Postgres, so there's no point in returning now and having the next
		 * connection attempt fail with something like the following:
		 *
		 * ERROR Connection to database failed: FATAL: the database system is
		 * starting up
		 */
		if (!pgIsReady)
		{
			log_error("Failed to read Postgres pidfile, see above for details");
			return false;
		}

		/*
		 * Postgres is running, is it in recovery?
		 *
		 * We're going to connect to "template1", because our target database
		 * might not have been created yet at this point: for instance, in case
		 * of problems during the `pg_autoctl create` command.
		 */
		strlcpy(dbname, pgSetup->dbname, NAMEDATALEN);
		strlcpy(pgSetup->dbname, "template1", NAMEDATALEN);

		/* initialise a SQL connection to the local postgres server */
		pg_setup_get_local_connection_string(pgSetup, connInfo);
		pgsql_init(&pgsql, connInfo, PGSQL_CONN_LOCAL);

		if (!pgsql_is_in_recovery(&pgsql, &pgSetup->is_in_recovery))
		{
			/* we logged about it already */
			errors++;
		}

		pgsql_finish(&pgsql);

		strlcpy(pgSetup->dbname, dbname, NAMEDATALEN);
	}

	if (errors > 0)
	{
		log_fatal("Failed to discover PostgreSQL setup, "
				  "please fix previous errors.");
		return false;
	}

	return true;
}


/*
 * Read the first line of the PGDATA/postmaster.pid file to get Postgres PID.
 */
static bool
get_pgpid(PostgresSetup *pgSetup, bool pg_is_not_running_is_ok)
{
	char *contents = NULL;
	long fileSize = 0;
	char pidfile[MAXPGPATH];
	char *lines[1];
	int pid = -1;

	join_path_components(pidfile, pgSetup->pgdata, "postmaster.pid");

	if (!read_file(pidfile, &contents, &fileSize))
	{
		if (!pg_is_not_running_is_ok)
		{
			log_error("Failed to open file \"%s\": %m", pidfile);
			log_info("Is PostgreSQL at \"%s\" up and running?", pgSetup->pgdata);
		}
		return false;
	}

	if (fileSize == 0)
	{
		log_warn("The PID file \"%s\" is empty\n", pidfile);
	}
	else if (splitLines(contents, lines, 1) != 1 ||
			 !stringToInt(lines[0], &pid))
	{
		log_warn("Invalid data in PID file \"%s\"\n", pidfile);
	}

	if (pid > 0 && pid <= INT_MAX)
	{
		if (kill(pid, 0) == 0)
		{
			pgSetup->pidFile.pid = pid;

			log_trace("get_pgpid: %d", pid);
			return true;
		}
		else
		{
			if (!pg_is_not_running_is_ok)
			{
				log_warn("Read a stale pid in \"postmaster.pid\": %d", pid);
			}
			else
			{
				log_debug("Read a stale pid in \"postmaster.pid\": %d", pid);
			}

			return false;
		}
	}
	else
	{
		/* that's more like a bug, really */
		log_error("Invalid PID \"%d\" read in \"postmaster.pid\"", pid);
		return false;
	}
}


/*
 * Read the PGDATA/postmaster.pid file to get the port number of the running
 * server we're asked to keep highly available.
 */
bool
read_pg_pidfile(PostgresSetup *pgSetup, bool pg_is_not_running_is_ok)
{
	FILE *fp;
	int lineno;
	char line[BUFSIZE];
	char pidfile[MAXPGPATH];

	join_path_components(pidfile, pgSetup->pgdata, "postmaster.pid");

	if ((fp = fopen_read_only(pidfile)) == NULL)
	{
		if (!pg_is_not_running_is_ok)
		{
			log_error("Failed to open file \"%s\": %m", pidfile);
			log_info("Is PostgreSQL at \"%s\" up and running?", pgSetup->pgdata);
		}
		return false;
	}

	for (lineno = 1; lineno <= LOCK_FILE_LINE_PM_STATUS; lineno++)
	{
		int lineLength = -1;

		if (fgets(line, sizeof(line), fp) == NULL)
		{
			/* don't use %m to print errno here, errno is not set by fgets */
			log_error("Failed to read line %d from file \"%s\"",
					  lineno, pidfile);
			fclose(fp);
			return false;
		}

		lineLength = strlen(line);

		/* chomp the ending Newline (\n) */
		if (lineLength > 0)
		{
			line[lineLength - 1] = '\0';
			lineLength = strlen(line);
		}

		if (lineno == LOCK_FILE_LINE_PID)
		{
			int pid = 0;
			if (!stringToInt(line, &pid))
			{
				log_error("Postgres pidfile does not contain a valid pid %s", line);

				return false;
			}

			pgSetup->pidFile.pid = pid;

			if (kill(pgSetup->pidFile.pid, 0) != 0)
			{
				log_error("Postgres pidfile contains pid %ld, "
						  "which is not running", pgSetup->pidFile.pid);

				/* well then reset the PID to our unknown value */
				pgSetup->pidFile.pid = 0;

				return false;
			}
		}

		if (lineno == LOCK_FILE_LINE_PORT)
		{
			if (!stringToUShort(line, &pgSetup->pidFile.port))
			{
				log_error("Postgres pidfile does not contain a valid port %s", line);

				return false;
			}
		}

		if (lineno == LOCK_FILE_LINE_SOCKET_DIR)
		{
			if (lineLength > 0)
			{
				int n = strlcpy(pgSetup->pghost, line, _POSIX_HOST_NAME_MAX);

				if (n >= _POSIX_HOST_NAME_MAX)
				{
					log_error("Failed to read unix socket directory \"%s\" "
							  "from file \"%s\": the directory name is %d "
							  "characters long, "
							  "and pg_autoctl only accepts up to %d characters",
							  line, pidfile, n, _POSIX_HOST_NAME_MAX - 1);
					return false;
				}
			}
		}

		if (lineno == LOCK_FILE_LINE_PM_STATUS)
		{
			if (lineLength > 0)
			{
				pgSetup->pm_status = pmStatusFromString(line);
			}
		}
	}
 	fclose(fp);

	log_trace("read_pg_pidfile: pid %ld, port %d, host %s, status \"%s\"",
			  pgSetup->pidFile.pid,
			  pgSetup->pidFile.port,
			  pgSetup->pghost,
			  pmStatusToString(pgSetup->pm_status));

	return true;
}


/*
 * fprintf_pg_setup prints to given STREAM the current setting found in
 * pgSetup.
 */
void
fprintf_pg_setup(FILE *stream, PostgresSetup *pgSetup)
{
	fformat(stream, "pgdata:             %s\n", pgSetup->pgdata);
	fformat(stream, "pg_ctl:             %s\n", pgSetup->pg_ctl);
	fformat(stream, "pg_version:         %s\n", pgSetup->pg_version);
	fformat(stream, "pghost:             %s\n", pgSetup->pghost);
	fformat(stream, "pgport:             %d\n", pgSetup->pgport);
	fformat(stream, "proxyport:          %d\n", pgSetup->proxyport);
	fformat(stream, "pid:                %ld\n", pgSetup->pidFile.pid);
	fformat(stream, "is in recovery:     %s\n",
			pgSetup->is_in_recovery ? "yes" : "no");
	fformat(stream, "Control Version:    %u\n",
			pgSetup->control.pg_control_version);
	fformat(stream, "Catalog Version:    %u\n",
				 pgSetup->control.catalog_version_no);
	fformat(stream, "System Identifier:  %" PRIu64 "\n",
				 pgSetup->control.system_identifier);
	fflush(stream);
}


/*
 * pg_setup_as_json copies in the given pre-allocated string the json
 * representation of the pgSetup.
 */
bool
pg_setup_as_json(PostgresSetup *pgSetup, JSON_Value *js)
{
	JSON_Object *jsobj = json_value_get_object(js);
	char system_identifier[BUFSIZE];

	json_object_set_string(jsobj, "pgdata", pgSetup->pgdata);
	json_object_set_string(jsobj, "pg_ctl", pgSetup->pg_ctl);
	json_object_set_string(jsobj, "version", pgSetup->pg_version);
	json_object_set_string(jsobj, "host", pgSetup->pghost);
	json_object_set_number(jsobj, "port", (double) pgSetup->pgport);
	json_object_set_number(jsobj, "proxyport", (double) pgSetup->proxyport);
	json_object_set_number(jsobj, "pid", (double) pgSetup->pidFile.pid);
	json_object_set_boolean(jsobj, "in_recovery", pgSetup->is_in_recovery);

	json_object_dotset_number(jsobj,
							  "control.version",
							  (double) pgSetup->control.pg_control_version);

	json_object_dotset_number(jsobj,
							  "control.catalog_version",
							  (double) pgSetup->control.catalog_version_no);

	sformat(system_identifier, BUFSIZE, "%" PRIu64,
			pgSetup->control.system_identifier);
	json_object_dotset_string(jsobj,
							  "control.system_identifier",
							  system_identifier);

	return true;
}

/*
 * pg_setup_get_local_connection_string build a connecting string to connect
 * to the local postgres server and writes it to connectionString, which should
 * be at least MAXCONNINFO in size.
 */
bool
pg_setup_get_local_connection_string(PostgresSetup *pgSetup,
									 char *connectionString)
{
	char pg_regress_sock_dir[MAXPGPATH];
	bool pg_regress_sock_dir_exists = env_exists("PG_REGRESS_SOCK_DIR");
	PQExpBuffer connStringBuffer = createPQExpBuffer();

	if (connStringBuffer == NULL)
	{
		log_error("Failed to allocate memory");
		return false;
	}

	appendPQExpBuffer(connStringBuffer, "port=%d dbname=%s",
					  pgSetup->pgport, pgSetup->dbname);

	if (pg_regress_sock_dir_exists &&
			!get_env_copy("PG_REGRESS_SOCK_DIR", pg_regress_sock_dir, MAXPGPATH))
	{
		/* errors have already been logged */
		return false;
	}
	/*
	 * When PG_REGRESS_SOCK_DIR is set and empty, we force the connection
	 * string to use "localhost" (TCP/IP hostname for IP 127.0.0.1 or ::1,
	 * usually), even when the configuration setup is using a unix directory
	 * setting.
	 */
	if (env_found_empty("PG_REGRESS_SOCK_DIR")
		&& (IS_EMPTY_STRING_BUFFER(pgSetup->pghost)
			|| pgSetup->pghost[0] == '/'))
	{
		appendPQExpBufferStr(connStringBuffer, " host=localhost");
	}
	else if (!IS_EMPTY_STRING_BUFFER(pgSetup->pghost))
	{
		if (pg_regress_sock_dir_exists && strlen(pg_regress_sock_dir) > 0
			&& strcmp(pgSetup->pghost, pg_regress_sock_dir) != 0)
		{
			/*
			 * It might turn out ok (stray environment), but in case of
			 * connection error, this warning should be useful to debug the
			 * situation.
			 */
			log_warn("PG_REGRESS_SOCK_DIR is set to \"%s\", "
					 "and our setup is using \"%s\"",
					 pg_regress_sock_dir,
					 pgSetup->pghost);
		}
		appendPQExpBuffer(connStringBuffer, " host=%s", pgSetup->pghost);
	}

	if (!IS_EMPTY_STRING_BUFFER(pgSetup->username))
	{
		appendPQExpBuffer(connStringBuffer, " user=%s", pgSetup->username);
	}

	if (PQExpBufferBroken(connStringBuffer))
	{
		log_error("Failed to allocate memory");
		destroyPQExpBuffer(connStringBuffer);
		return false;
	}

	if (strlcpy(connectionString,
				connStringBuffer->data, MAXCONNINFO) >= MAXCONNINFO)
	{
		log_error("Failed to copy connection string \"%s\" which is %lu bytes "
				  "long, pg_autoctl only supports connection strings up to "
				  " %d bytes",
				  connStringBuffer->data, connStringBuffer->len, MAXCONNINFO);
	}

	return true;
}


/*
 * pg_setup_pgdata_exists returns true when the pg_controldata probe was
 * susccessful.
 */
bool
pg_setup_pgdata_exists(PostgresSetup *pgSetup)
{
	return pgSetup->control.system_identifier != 0;
}


/*
 * pg_setup_pgdata_exists returns true when the pg_controldata probe was
 * susccessful.
 */
bool
pg_setup_is_running(PostgresSetup *pgSetup)
{
	bool pg_is_not_running_is_ok = true;

	return pgSetup->pidFile.pid != 0
		/* if we don't have the PID yet, try reading it now */
		|| (get_pgpid(pgSetup, pg_is_not_running_is_ok)
			&& pgSetup->pidFile.pid != 0);
}


/*
 * pg_setup_is_ready returns true when the postmaster.pid file has a "ready"
 * status in it, which we parse in pgSetup->pm_status.
 */
bool
pg_setup_is_ready(PostgresSetup *pgSetup, bool pg_is_not_running_is_ok)
{
	log_trace("pg_setup_is_ready");

	if (pgSetup->control.pg_control_version > 0)
	{
		bool firstTime = true;
		int warnings = 0;

		/*
		 * Invalidate in-memory Postmaster status cache.
		 *
		 * This makes sure we enter the main loop and attempt to read the
		 * postmaster.pid file at least once: if Postgres was stopped, then the
		 * file that we've read previously might not exists any-more.
		 */
		pgSetup->pm_status = POSTMASTER_STATUS_UNKNOWN;

		/*
		 * Sometimes `pg_ctl start` returns with success and Postgres is still
		 * in crash recovery replaying WAL files, in the "starting" state
		 * rather than the "ready" state.
		 *
		 * In that case, we wait until Postgres is ready for connections. The
		 * whole pg_autoctl code is expecting to be able to connect to
		 * Postgres, so there's no point in returning now and having the next
		 * connection attempt fail with something like the following:
		 *
		 * ERROR Connection to database failed: FATAL: the database system is
		 * starting up
		 */
		while (pgSetup->pm_status != POSTMASTER_STATUS_READY)
		{
			log_trace("pg_setup_is_ready: %s",
					  pmStatusToString(pgSetup->pm_status));

 			if (!get_pgpid(pgSetup, pg_is_not_running_is_ok))
			{
				/*
				 * We failed to read the Postgres pid file, and infinite
				 * looping might not help here anymore. Better give control
				 * back to the launching process (might be init scripts,
				 * systemd or the like) so that they may log a transient
				 * failure and try again.
				 */
				if (!pg_is_not_running_is_ok)
				{
					log_error("Failed to get Postgres pid, "
							  "see above for details");
				}

				/*
				 * we failed to get Postgres pid from the first line of its pid
				 * file, so we consider that Postgres is not running, thus not
				 * ready.
				 */
				return false;
			}

			/*
			 * Here, we know that Postgres is running, and we even have its
			 * PID. Time to try and read the rest of the PID file. This might
			 * fail when the file isn't complete yet, in which case we're going
			 * to retry.
			 */
			if (!read_pg_pidfile(pgSetup, pg_is_not_running_is_ok))
			{
				log_warn("Failed to read Postgres \"postmaster.pid\" file");
				return false;
			}

			/* avoid an extra wait if that's possible */
			if (pgSetup->pm_status == POSTMASTER_STATUS_READY)
			{
				break;
			}

			if (firstTime)
			{
				firstTime = false;
			}
			else
			{
				warnings++;
				log_warn("Postgres is not ready for connections: "
						 "postmaster status is \"%s\", retrying in %ds.",
						 pmStatusToString(pgSetup->pm_status),
						 PG_AUTOCTL_KEEPER_SLEEP_TIME);

				sleep(PG_AUTOCTL_KEEPER_SLEEP_TIME);
			}

			if (asked_to_stop == 1 || asked_to_stop_fast == 1)
			{
				log_info("pg_autoctl service stopping");
				exit(EXIT_CODE_QUIT);
			}
		}

		/*
		 * If we did warn the user, let them know that we're back to a normal
		 * situation (when that's the case).
		 */
		if (warnings > 0 && pgSetup->pm_status == POSTMASTER_STATUS_READY)
		{
			log_info("Postgres is ready");
		}
	}

	return pgSetup->pm_status == POSTMASTER_STATUS_READY;
}


/*
 * pg_setup_is_primary returns true when the local PostgreSQL instance is known
 * to not be recovery.
 */
bool
pg_setup_is_primary(PostgresSetup *pgSetup)
{
	if (pg_setup_is_running(pgSetup))
	{
		return !pgSetup->is_in_recovery;
	}
	else
	{
		/*
		 * PostgreSQL is not running, we don't know... assume we're not in
		 * recovery, otherwise `pg_autoctl create` bails out without even
		 * trying.
		 */
		return true;
	}
}


/*
 * pg_setup_get_username returns pgSetup->username when it exists, otherwise it
 * looksup the username in passwd. Lastly it fallsback to the USER environment
 * variable. When nothing works it returns DEFAULT_USERNAME PGUSER is only used
 * when creating our configuration for the first time.
 */
char *
pg_setup_get_username(PostgresSetup *pgSetup)
{
	uid_t uid;
	struct passwd *pw;
	char userEnv[NAMEDATALEN];

	/* use a configured username if provided */
	if (!IS_EMPTY_STRING_BUFFER(pgSetup->username))
	{
		return pgSetup->username;
	}

	log_trace("username not configured");

	/* use the passwd file to find the username, same as whoami */
	uid = geteuid();
	pw = getpwuid(uid);
	if (pw)
	{
		log_trace("username found in passwd: %s", pw->pw_name);
		/* struct passwd is in thread shared space, return a copy */
		return strdup(pw->pw_name);
	}


	/* fallback on USER from env if the user cannot be found in passwd */
	if (get_env_copy("USER", userEnv, NAMEDATALEN))
	{
		log_trace("username found in USER environment variable: %s", userEnv);
		return strdup(userEnv);
	}

	log_trace("username fallback to default: %s", DEFAULT_USERNAME);

	return DEFAULT_USERNAME;
}


/*
 * pg_setup_get_auth_method returns pgSetup->authMethod when it exists,
 * otherwise it returns DEFAULT_AUTH_METHOD
 */
char *
pg_setup_get_auth_method(PostgresSetup *pgSetup)
{
	if (!IS_EMPTY_STRING_BUFFER(pgSetup->authMethod))
	{
		return pgSetup->authMethod;
	}

	log_trace("auth method not configured, falling back to default value : %s",
			  DEFAULT_AUTH_METHOD);

	return DEFAULT_AUTH_METHOD;
}


/*
 * pg_setup_skip_hba_edits returns true when the user had setup pg_autoctl to
 * skip editing HBA entries.
 */
bool
pg_setup_skip_hba_edits(PostgresSetup *pgSetup)
{
	return !IS_EMPTY_STRING_BUFFER(pgSetup->authMethod)
		&& strcmp(pgSetup->authMethod, SKIP_HBA_AUTH_METHOD) == 0;
}


/*
 * pg_setup_set_absolute_pgdata uses realpath(3) to make sure that we re using
 * the absolute real pathname for PGDATA in our setup, so that services will
 * work correctly after keeper/monitor init, even when initializing in a
 * relative path and starting the service from elsewhere. This function returns
 * true if the pgdata path has been updated in the setup.
 */
bool
pg_setup_set_absolute_pgdata(PostgresSetup *pgSetup)
{
	char absolutePgdata[PATH_MAX];

	/*
	 * We managed to initdb, refresh our configuration file location with
	 * the realpath(3): we might have been given a relative pathname.
	 */
	if (!realpath(pgSetup->pgdata, absolutePgdata))
	{
		/* unexpected error, but not fatal, just don't overwrite the config. */
		log_warn("Failed to get the realpath of given pgdata \"%s\": %m",
				 pgSetup->pgdata);
		return false;
	}

	if (strcmp(pgSetup->pgdata, absolutePgdata) != 0)
	{
		/* use the absolute path now that it exists. */
		strlcpy(pgSetup->pgdata, absolutePgdata, MAXPGPATH);

		log_info("Now using absolute pgdata value \"%s\" in the configuration",
				 pgSetup->pgdata);

		return true;
	}

	return false;
}


/*
 * nodeKindFromString returns a PgInstanceKind from a string.
 */
PgInstanceKind
nodeKindFromString(const char *nodeKind)
{
	PgInstanceKind kindArray[] = { NODE_KIND_UNKNOWN,
								   NODE_KIND_UNKNOWN,
								   NODE_KIND_STANDALONE,
								   NODE_KIND_CITUS_COORDINATOR,
								   NODE_KIND_CITUS_WORKER };
	char *kindList[] = {
		"", "unknown", "standalone", "coordinator", "worker", NULL
	};

	for(int listIndex = 0; kindList[listIndex] != NULL; listIndex++)
	{
		char *candidate = kindList[listIndex];

		if (strcmp(nodeKind, candidate) == 0)
		{
			PgInstanceKind pgKind = kindArray[listIndex];
			log_trace("nodeKindFromString: \"%s\" ➜ %d", nodeKind, pgKind);
			return pgKind;
		}
	}

	log_fatal("Failed to parse nodeKind \"%s\"", nodeKind);

	/* never happens, make compiler happy */
	return NODE_KIND_UNKNOWN;
}


/*
 * nodeKindToString returns a textual representatin of given PgInstanceKind.
 * This must be kept in sync with src/monitor/formation_metadata.c function
 * FormationKindFromNodeKindString.
 */
char *
nodeKindToString(PgInstanceKind kind)
{
	switch (kind)
	{
		case NODE_KIND_STANDALONE:
			return "standalone";

		case NODE_KIND_CITUS_COORDINATOR:
			return "coordinator";

		case NODE_KIND_CITUS_WORKER:
			return "worker";

		default:
			log_fatal("nodeKindToString: unknown node kind %d", kind);
			return NULL;
	}

	/* can't happen, keep compiler happy */
	return NULL;
}


/*
 * pmStatusFromString parses the Postgres postmaster.pid PM_STATUS line into
 * our own enum to represent the value.
 */
static PostmasterStatus
pmStatusFromString(const char *postmasterStatus)
{
	log_trace("pmStatusFromString: postmaster status is \"%s\"",
			  postmasterStatus);

	if (strcmp(postmasterStatus, PM_STATUS_STARTING) == 0)
	{
		return POSTMASTER_STATUS_STARTING;
	}
	else if (strcmp(postmasterStatus, PM_STATUS_STOPPING) == 0)
	{
		return POSTMASTER_STATUS_STOPPING;
	}
	else if (strcmp(postmasterStatus, PM_STATUS_READY) == 0)
	{
		return POSTMASTER_STATUS_READY;
	}
	else if (strcmp(postmasterStatus, PM_STATUS_STANDBY) == 0)
	{
		return POSTMASTER_STATUS_STANDBY;
	}

	log_warn("Failed to read Postmaster status: \"%s\"", postmasterStatus);
	return POSTMASTER_STATUS_UNKNOWN;
}


/*
 * pmStatusToString returns a textual representation of given Postmaster status
 * given as a PmStatus enum.
 *
 * We're not using the PM_STATUS_READY etc constants here because those are
 * blank-padded to always be the same length, and then the warning messages
 * including "ready " look buggy in a way.
 */
static char *
pmStatusToString(PostmasterStatus pm_status)
{
	switch (pm_status)
	{
		case POSTMASTER_STATUS_UNKNOWN:
			return "unknown";

		case POSTMASTER_STATUS_STARTING:
			return "starting";

		case POSTMASTER_STATUS_STOPPING:
			return "stopping";

		case POSTMASTER_STATUS_READY:
			return "ready";

		case POSTMASTER_STATUS_STANDBY:
			return "standby";
	};

	/* keep compiler happy */
	return "unknown";
}


/*
 * pgsetup_get_pgport returns the port to use either from the PGPORT
 * environment variable, or from our default hard-coded value of 5432.
 */
int
pgsetup_get_pgport()
{
	char pgport_env[NAMEDATALEN];
	int pgport = 0;

	if (env_exists("PGPORT") && get_env_copy("PGPORT", pgport_env, NAMEDATALEN))
	{
		if (stringToInt(pgport_env, &pgport) && pgport > 0)
		{
			return pgport;
		}
		else
		{
			log_warn("Failed to parse PGPORT value \"%s\", using %d",
					 pgport_env, POSTGRES_PORT);
			return POSTGRES_PORT;
		}
	}
	else
	{
		/* no PGPORT */
		return POSTGRES_PORT;
	}
}


/*
 * pgsetup_validate_ssl_settings returns true if our SSL settings are following
 * one of the three following cases:
 *
 *  - --no-ssl:          ssl is not activated and no file has been provided
 *  - --ssl-self-signed: ssl is activated and no file has been provided
 *  - --ssl-*-files:     ssl is activated and all the files have been provided
 *
 * Otherwise it logs an error message and return false.
 */
bool
pgsetup_validate_ssl_settings(PostgresSetup *pgSetup)
{
	SSLOptions *ssl = &(pgSetup->ssl);

	log_trace("pgsetup_validate_ssl_settings");

	/*
	 * When using the full SSL options, we validate that the files exists where
	 * given and set the default sslmode to verify-full.
	 *
	 *  --ssl-ca-file
	 *  --ssl-crl-file
	 *  --server-crt
	 *  --server-key
	 */
	if (ssl->active && !ssl->createSelfSignedCert)
	{
		/* we say allFilesGiven but we can do without the CRL file */
		bool allFilesGiven = !IS_EMPTY_STRING_BUFFER(ssl->caFile)
			&& !IS_EMPTY_STRING_BUFFER(ssl->serverCert)
			&& !IS_EMPTY_STRING_BUFFER(ssl->serverKey);

		if (!allFilesGiven)
		{
			log_error("Failed to setup SSL with user-provided certificates: "
					  "options --ssl-ca-file --ssl-server-cert --ssl-server-key"
					  "are required.");
			return false;
		}

		/* when given all the files, check they exist */
		if (!file_exists(ssl->caFile))
		{
			log_error("--ssl-ca-file file does not exist at \"%s\"",
					  ssl->caFile);
			return false;
		}

		if (!IS_EMPTY_STRING_BUFFER(ssl->crlFile) && !file_exists(ssl->crlFile))
		{
			log_error("--ssl-crl-file file does not exist at \"%s\"",
					  ssl->crlFile);
			return false;
		}

		if (!file_exists(ssl->serverCert))
		{
			log_error("--server-crt file does not exist at \"%s\"",
					  ssl->serverCert);
			return false;
		}

		if (!file_exists(ssl->serverKey))
		{
			log_error("--server-key file does not exist at \"%s\"",
					  ssl->serverKey);
			return false;
		}

		/* install a default value for --ssl-mode, use verify-full */
		if (ssl->sslMode == SSL_MODE_UNKNOWN)
		{
			ssl->sslMode = SSL_MODE_VERIFY_FULL;
			strlcpy(ssl->sslModeStr,
					pgsetup_sslmode_to_string(ssl->sslMode), SSL_MODE_STRLEN);
			log_info("Using default --ssl-mode \"%s\"", ssl->sslModeStr);
		}

		return true;
	}

	/*
	 * When --ssl-self-signed is used, we default to using sslmode=require.
	 * Setting higher than that are wrong, false sense of security.
	 */
	if (ssl->createSelfSignedCert)
	{
		/* in that case we want an sslMode of require at most */
		if (ssl->sslMode > SSL_MODE_REQUIRE)
		{
			log_error("--ssl-mode \"%s\" is not compatible with self-signed "
					  "certificates, please provide certificates signed by "
					  "your trusted CA.",
					  pgsetup_sslmode_to_string(ssl->sslMode));
			log_info("See https://www.postgresql.org/docs/current/libpq-ssl.html"
					 " for details");
			return false;
		}

		if (ssl->sslMode == SSL_MODE_UNKNOWN)
		{
			/* install a default value for --ssl-mode */
			ssl->sslMode = SSL_MODE_REQUIRE;
			strlcpy(ssl->sslModeStr,
					pgsetup_sslmode_to_string(ssl->sslMode), SSL_MODE_STRLEN);
			log_info("Using default --ssl-mode \"%s\"", ssl->sslModeStr);
		}

		log_info("Using --ssl-self-signed: pg_autoctl will "
				 " create self-signed certificates, allowing for "
				 "encrypted network traffic");
		log_warn("Self-signed certificates provide protection against "
				 "eavesdropping; this setup does NOT protect against "
				 "Man-In-The-Middle attacks nor Impersonation attacks.");
		log_warn("See https://www.postgresql.org/docs/current/libpq-ssl.html "
				 "for details");

		return true;
	}

	/* --no-ssl is ok */
	if (ssl->active == 0)
	{
		log_warn("No encryption is used for network traffic! This allows an "
				 "attacker on the network to read all replication data.");
		log_warn("Using --ssl-self-signed instead of --no-ssl is recommend to "
				 "achieve more security with the same ease of deployment.");
		log_warn("See https://www.postgresql.org/docs/current/libpq-ssl.html "
				 "for details on how to improve");
		return true;
	}

	return false;
}


/*
 * pg_setup_sslmode_to_string parses a string representing the sslmode into an
 * internal enum value, so that we can easily compare values.
 */
SSLMode
pgsetup_parse_sslmode(const char *sslMode)
{
	SSLMode enumArray[] = {	SSL_MODE_DISABLE,
							SSL_MODE_ALLOW,
							SSL_MODE_PREFER,
							SSL_MODE_REQUIRE,
							SSL_MODE_VERIFY_CA,
							SSL_MODE_VERIFY_FULL };

	char *sslModeArray[] = {"disable", "allow", "prefer", "require",
							"verify-ca", "verify-full", NULL};

	int sslModeArrayIndex = 0;

	for (sslModeArrayIndex = 0;
		 sslModeArray[sslModeArrayIndex] != NULL;
		 sslModeArrayIndex++)
	{
		if (strcmp(sslMode, sslModeArray[sslModeArrayIndex]) == 0)
		{
			return enumArray[sslModeArrayIndex];
		}
	}

	return SSL_MODE_UNKNOWN;
}


/*
 * pgsetup_sslmode_to_string returns the string representation of the enum.
 */
char *
pgsetup_sslmode_to_string(SSLMode sslMode)
{
	switch (sslMode)
	{
		case SSL_MODE_UNKNOWN:
			return "unknown";
		case SSL_MODE_DISABLE:
			return "disable";

		case SSL_MODE_ALLOW:
			return "allow";

		case SSL_MODE_PREFER:
			return "prefer";

		case SSL_MODE_REQUIRE:
			return "require";

		case SSL_MODE_VERIFY_CA:
			return "verify-ca";

		case SSL_MODE_VERIFY_FULL:
			return "verify-full";
	}

	/* This is a huge bug */
	log_error("BUG: some unknown SSL_MODE enum value was encountered");
	return "unknown";
}
