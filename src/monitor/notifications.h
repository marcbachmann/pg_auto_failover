/*-------------------------------------------------------------------------
 *
 * src/monitor/notifications.h
 *
 * Declarations for public functions and types related to monitor
 * notifications.
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 *-------------------------------------------------------------------------
 */

#pragma once

#include "postgres.h"
#include "c.h"

#include "node_metadata.h"
#include "replication_state.h"

/*
 * pg_auto_failover notifies on different channels about every event it
 * produces:
 *
 * - the "state" channel is used when a node's state is assigned to something
 *   new
 *
 * - the "log" channel is used to duplicate message that are sent to the
 *   PostgreSQL logs, in order for a pg_auto_failover monitor client to subscribe to
 *   the chatter without having to actually have the privileges to tail the
 *   PostgreSQL server logs.
 */
#define CHANNEL_STATE "state"
#define CHANNEL_LOG "log"
#define BUFSIZE 8192


void LogAndNotifyMessage(char *message, size_t size, const char *fmt, ...) __attribute__(
	(format(printf, 3, 4)));


int64 NotifyStateChange(ReplicationState reportedState,
						ReplicationState goalState,
						const char *formationId,
						int groupId,
						int64 nodeId,
						const char *nodeName,
						int nodePort,
						SyncState pgsrSyncState,
						XLogRecPtr reportedLSN,
						int candidatePriority,
						bool replicationQuorum,
						char *description);

int64 InsertEvent(const char *formationId, int groupId, int64 nodeId,
				  const char *nodeName, int nodePort,
				  ReplicationState reportedState,
				  ReplicationState goalState,
				  SyncState pgsrSyncState,
				  XLogRecPtr reportedLSN,
				  int candidatePriority,
				  bool replicationQuorum,
				  char *description);
