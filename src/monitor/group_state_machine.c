/*-------------------------------------------------------------------------
 *
 * src/monitor/group_state_machine.c
 *
 * Implementation of the state machine for fail-over within a group of
 * PostgreSQL nodes.
 *
 * Copyright (c) Microsoft Corporation. All rights reserved.
 * Licensed under the PostgreSQL License.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"

#include "formation_metadata.h"
#include "group_state_machine.h"
#include "node_metadata.h"
#include "notifications.h"
#include "replication_state.h"
#include "version_compat.h"

#include "access/htup_details.h"
#include "catalog/pg_enum.h"
#include "commands/trigger.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_type.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"


/* private function forward declarations */
static bool ProceedGroupStateForPrimaryNode(AutoFailoverNode *primaryNode);
static void AssignGoalState(AutoFailoverNode *pgAutoFailoverNode,
							ReplicationState state, char *description);
static bool IsDrainTimeExpired(AutoFailoverNode *pgAutoFailoverNode);
static bool WalDifferenceWithin(AutoFailoverNode *secondaryNode,
								AutoFailoverNode *primaryNode,
								int64 delta);
static bool IsHealthy(AutoFailoverNode *pgAutoFailoverNode);
static bool IsUnhealthy(AutoFailoverNode *pgAutoFailoverNode);

/* GUC variables */
int EnableSyncXlogThreshold = DEFAULT_XLOG_SEG_SIZE;
int PromoteXlogThreshold = DEFAULT_XLOG_SEG_SIZE;
int DrainTimeoutMs = 30 * 1000;
int UnhealthyTimeoutMs = 20 * 1000;
int StartupGracePeriodMs = 10 * 1000;


/*
 * ProceedGroupState proceeds the state machines of the group of which
 * the given node is part.
 */
bool
ProceedGroupState(AutoFailoverNode *activeNode)
{
	char *formationId = activeNode->formationId;
	int groupId = activeNode->groupId;
	AutoFailoverFormation *formation = GetFormation(formationId);

	AutoFailoverNode *primaryNode = NULL;
	List *nodesGroupList = AutoFailoverNodeGroup(formationId, groupId);
	int nodesCount = list_length(nodesGroupList);

	/* when there's no other node anymore, not even one */
	if (nodesCount == 1 &&
		!IsCurrentState(activeNode, REPLICATION_STATE_SINGLE))
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to single as there is no other "
			"node.",
			activeNode->nodeName, activeNode->nodePort);

		/* other node may have been removed */
		AssignGoalState(activeNode, REPLICATION_STATE_SINGLE, message);

		return true;
	}

	/*
	 * We separate out the FSM for the primary server, because that one needs
	 * to loop over every other node to take decisions. That induces some
	 * complexity that is best managed in a specialized function.
	 */
	if (IsInPrimaryState(activeNode))
	{
		return ProceedGroupStateForPrimaryNode(activeNode);
	}

	primaryNode = GetPrimaryNodeInGroup(formationId, groupId);

	if (primaryNode == NULL)
	{
		/* that's a bug, really, maybe we could use an Assert() instead */
		ereport(ERROR,
				(errmsg("ProceedGroupState couldn't find the primary node "
						"in formation \"%s\", group %d",
						formationId, groupId),
				 errdetail("activeNode is %s:%d in state %s",
						   activeNode->nodeName, activeNode->nodePort,
						   ReplicationStateGetName(activeNode->goalState))));
	}

	if (IsUnhealthy(primaryNode))
	{
		/* TODO: Multiple Stanby Failover Logic */
	}

	/*
	 * when primary node is ready for replication:
	 *  prepare_standby -> catchingup
	 */
	if (IsCurrentState(activeNode, REPLICATION_STATE_WAIT_STANDBY) &&
		(IsCurrentState(primaryNode, REPLICATION_STATE_WAIT_PRIMARY) ||
		 IsCurrentState(primaryNode, REPLICATION_STATE_JOIN_PRIMARY)))
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to catchingup after %s:%d "
			"converged to wait_primary.",
			activeNode->nodeName, activeNode->nodePort,
			primaryNode->nodeName, primaryNode->nodePort);

		/* start replication */
		AssignGoalState(activeNode, REPLICATION_STATE_CATCHINGUP, message);

		return true;
	}

	/*
	 * when secondary caught up:
	 *      catchingup -> secondary
	 *  + wait_primary -> primary
	 */
	if (IsCurrentState(activeNode, REPLICATION_STATE_CATCHINGUP) &&
		(IsCurrentState(primaryNode, REPLICATION_STATE_WAIT_PRIMARY) ||
		 IsCurrentState(primaryNode, REPLICATION_STATE_JOIN_PRIMARY)) &&
		IsHealthy(activeNode) &&
		WalDifferenceWithin(activeNode, primaryNode, EnableSyncXlogThreshold))
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to primary and %s:%d to "
			"secondary after %s:%d caught up.",
			primaryNode->nodeName, primaryNode->nodePort,
			activeNode->nodeName, activeNode->nodePort,
			activeNode->nodeName, activeNode->nodePort);

		/* node is ready for promotion */
		AssignGoalState(activeNode, REPLICATION_STATE_SECONDARY, message);

		/* other node can enable synchronous commit */
		AssignGoalState(primaryNode, REPLICATION_STATE_PRIMARY, message);

		return true;
	}

	/*
	 * TODO:
	 *   Implement Multiple Standby failover logic.
	 *
	 * when primary fails:
	 *   secondary -> prepare_promotion
	 * +   primary -> draining
	 */
	if (IsCurrentState(activeNode, REPLICATION_STATE_SECONDARY) &&
		IsInPrimaryState(primaryNode) &&
		IsUnhealthy(primaryNode) && IsHealthy(activeNode) &&
		WalDifferenceWithin(activeNode, primaryNode, PromoteXlogThreshold))
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to draining and %s:%d to "
			"prepare_promotion after %s:%d became unhealthy.",
			primaryNode->nodeName, primaryNode->nodePort,
			activeNode->nodeName, activeNode->nodePort,
			primaryNode->nodeName, primaryNode->nodePort);

		/* keep reading until no more records are available */
		AssignGoalState(activeNode, REPLICATION_STATE_PREPARE_PROMOTION, message);

		/* shut down the primary */
		AssignGoalState(primaryNode, REPLICATION_STATE_DRAINING, message);

		return true;
	}

	/*
	 * when a worker blocked writes:
	 *   prepare_promotion -> wait_primary
	 */
	if (IsCurrentState(activeNode, REPLICATION_STATE_PREPARE_PROMOTION) &&
		IsCitusFormation(formation) && activeNode->groupId > 0)
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to wait_primary and %s:%d to "
			"demoted after the coordinator metadata was updated.",
			activeNode->nodeName, activeNode->nodePort,
			primaryNode->nodeName, primaryNode->nodePort);

		/* node is now taking writes */
		AssignGoalState(activeNode, REPLICATION_STATE_WAIT_PRIMARY, message);

		/* done draining, node is presumed dead */
		AssignGoalState(primaryNode, REPLICATION_STATE_DEMOTED, message);

		return true;
	}

	/*
	 * when node is seeing no more writes:
	 *  prepare_promotion -> stop_replication
	 */
	if (IsCurrentState(activeNode, REPLICATION_STATE_PREPARE_PROMOTION))
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to demote_timeout and %s:%d to "
			"stop_replication after %s:%d converged to "
			"prepare_promotion.",
			primaryNode->nodeName, primaryNode->nodePort,
			activeNode->nodeName, activeNode->nodePort,
			activeNode->nodeName, activeNode->nodePort);

		/* perform promotion to stop replication */
		AssignGoalState(activeNode, REPLICATION_STATE_STOP_REPLICATION, message);

		/* wait for possibly-alive primary to kill itself */
		AssignGoalState(primaryNode, REPLICATION_STATE_DEMOTE_TIMEOUT, message);

		return true;
	}

	/*
	 * when drain time expires or primary reports it's drained:
	 *  draining -> demoted
	 */
	if (IsCurrentState(activeNode, REPLICATION_STATE_STOP_REPLICATION) &&
		(IsCurrentState(primaryNode, REPLICATION_STATE_DEMOTE_TIMEOUT) ||
		 IsDrainTimeExpired(primaryNode)))
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to wait_primary and %s:%d to "
			"demoted after the demote timeout expired.",
			activeNode->nodeName, activeNode->nodePort,
			primaryNode->nodeName, primaryNode->nodePort);

		/* node is now taking writes */
		AssignGoalState(activeNode, REPLICATION_STATE_WAIT_PRIMARY, message);

		/* done draining, node is presumed dead */
		AssignGoalState(primaryNode, REPLICATION_STATE_DEMOTED, message);

		return true;
	}

	/*
	 * when a worker blocked writes:
	 *   stop_replication -> wait_primary
	 */
	if (IsCurrentState(activeNode, REPLICATION_STATE_STOP_REPLICATION) &&
		IsCitusFormation(formation) && activeNode->groupId > 0)
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to wait_primary and %s:%d to "
			"demoted after the coordinator metadata was updated.",
			activeNode->nodeName, activeNode->nodePort,
			primaryNode->nodeName, primaryNode->nodePort);

		/* node is now taking writes */
		AssignGoalState(activeNode, REPLICATION_STATE_WAIT_PRIMARY, message);

		/* done draining, node is presumed dead */
		AssignGoalState(primaryNode, REPLICATION_STATE_DEMOTED, message);

		return true;
	}

	/*
	 * when a new primary is ready:
	 *  demoted -> catchingup
	 */
	if (IsCurrentState(activeNode, REPLICATION_STATE_DEMOTED) &&
		IsCurrentState(primaryNode, REPLICATION_STATE_WAIT_PRIMARY))
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to catchingup after it "
			"converged to demotion and %s:%d converged to wait_primary.",
			activeNode->nodeName, activeNode->nodePort,
			primaryNode->nodeName, primaryNode->nodePort);

		/* it's safe to rejoin as a secondary */
		AssignGoalState(activeNode, REPLICATION_STATE_CATCHINGUP, message);

		return true;
	}

	return false;
}


/*
 * Group State Machine when a primary node contacts the monitor.
 */
static bool
ProceedGroupStateForPrimaryNode(AutoFailoverNode *primaryNode)
{
	List *otherNodesGroupList = AutoFailoverOtherNodesList(primaryNode);
	int otherNodesCount = list_length(otherNodesGroupList);

	/*
	 * when a first "other" node wants to become standby:
	 *  single -> wait_primary
	 */
	if (IsCurrentState(primaryNode, REPLICATION_STATE_SINGLE))
	{
		ListCell *nodeCell = NULL;

		foreach(nodeCell, otherNodesGroupList)
		{
			AutoFailoverNode *otherNode = (AutoFailoverNode *) lfirst(nodeCell);

			if (IsCurrentState(otherNode, REPLICATION_STATE_WAIT_STANDBY))
			{
				char message[BUFSIZE];

				LogAndNotifyMessage(
					message, BUFSIZE,
					"Setting goal state of %s:%d to wait_primary after %s:%d "
					"joined.", primaryNode->nodeName, primaryNode->nodePort,
					otherNode->nodeName, otherNode->nodePort);

				/* prepare replication slot and pg_hba.conf */
				AssignGoalState(primaryNode,
								REPLICATION_STATE_WAIT_PRIMARY,
								message);

				return true;
			}
		}
	}

	/*
	 * when another node wants to become standby:
	 *  primary -> join_primary
	 */
	if (IsCurrentState(primaryNode, REPLICATION_STATE_PRIMARY))
	{
		ListCell *nodeCell = NULL;

		foreach(nodeCell, otherNodesGroupList)
		{
			AutoFailoverNode *otherNode = (AutoFailoverNode *) lfirst(nodeCell);

			if (IsCurrentState(otherNode, REPLICATION_STATE_WAIT_STANDBY))
			{
				char message[BUFSIZE];

				LogAndNotifyMessage(
					message, BUFSIZE,
					"Setting goal state of %s:%d to join_primary after %s:%d "
					"joined.", primaryNode->nodeName, primaryNode->nodePort,
					otherNode->nodeName, otherNode->nodePort);

				/* prepare replication slot and pg_hba.conf */
				AssignGoalState(primaryNode,
								REPLICATION_STATE_JOIN_PRIMARY,
								message);

				return true;
			}
		}
	}

	/*
	 * when secondary unhealthy:
	 *   secondary ➜ catchingup
	 *     primary ➜ wait_primary
	 *
	 * We only swith the primary to wait_primary when there's no healthy
	 * secondary anymore. In other cases, there's by definition at least one
	 * candidate for failover.
	 */
	if (IsCurrentState(primaryNode, REPLICATION_STATE_PRIMARY))
	{
		int failoverCandidateCount = otherNodesCount;
		ListCell *nodeCell = NULL;

		foreach(nodeCell, otherNodesGroupList)
		{
			AutoFailoverNode *otherNode = (AutoFailoverNode *) lfirst(nodeCell);

			if (IsCurrentState(otherNode, REPLICATION_STATE_SECONDARY) &&
				IsUnhealthy(otherNode))
			{
				char message[BUFSIZE];

				--failoverCandidateCount;

				LogAndNotifyMessage(
					message, BUFSIZE,
					"Setting goal state of %s:%d to catchingup "
					"after it became unhealthy.",
					otherNode->nodeName, otherNode->nodePort);

				/* other node is behind, no longer eligible for promotion */
				AssignGoalState(otherNode,
								REPLICATION_STATE_CATCHINGUP, message);
			}
			else if (!otherNode->replicationQuorum ||
					 otherNode->candidatePriority == 0)
			{
				/* also not a candidate */
				--failoverCandidateCount;
			}

			/* disable synchronous replication to maintain availability */
			if (failoverCandidateCount == 0)
			{
				char message[BUFSIZE];

				LogAndNotifyMessage(
					message, BUFSIZE,
					"Setting goal state of %s:%d to wait_primary "
					"now that none of the standbys are healthy anymore.",
					primaryNode->nodeName, primaryNode->nodePort);

				AssignGoalState(primaryNode,
								REPLICATION_STATE_WAIT_PRIMARY, message);
			}
		}

		return true;
	}

	/*
	 * when a node has changed its replication settings:
	 *     apply_settings ➜ primary
	 */
	if (IsCurrentState(primaryNode, REPLICATION_STATE_APPLY_SETTINGS))
	{
		char message[BUFSIZE];

		LogAndNotifyMessage(
			message, BUFSIZE,
			"Setting goal state of %s:%d to primary "
			"after it applied replication properties change.",
			primaryNode->nodeName, primaryNode->nodePort);

		AssignGoalState(primaryNode, REPLICATION_STATE_PRIMARY, message);

		return true;
	}

	return false;
}


/*
 * AssignGoalState assigns a new goal state to a AutoFailover node.
 */
static void
AssignGoalState(AutoFailoverNode *pgAutoFailoverNode,
				ReplicationState state, char *description)
{
	if (pgAutoFailoverNode != NULL)
	{
		pgAutoFailoverNode->goalState = state;

		SetNodeGoalState(pgAutoFailoverNode->nodeName,
						 pgAutoFailoverNode->nodePort, state);

		NotifyStateChange(pgAutoFailoverNode->reportedState,
						  state,
						  pgAutoFailoverNode->formationId,
						  pgAutoFailoverNode->groupId,
						  pgAutoFailoverNode->nodeId,
						  pgAutoFailoverNode->nodeName,
						  pgAutoFailoverNode->nodePort,
						  pgAutoFailoverNode->pgsrSyncState,
						  pgAutoFailoverNode->reportedLSN,
						  pgAutoFailoverNode->candidatePriority,
						  pgAutoFailoverNode->replicationQuorum,
						  description);
	}
}


/*
 * WalDifferenceWithin returns whether the most recently reported relative log
 * position of the given nodes is within the specified bound. Returns false if
 * neither node has reported a relative xlog position
 */
static bool
WalDifferenceWithin(AutoFailoverNode *secondaryNode,
					AutoFailoverNode *otherNode, int64 delta)
{
	int64 walDifference = 0;
	XLogRecPtr secondaryLsn = 0;
	XLogRecPtr otherNodeLsn = 0;


	if (secondaryNode == NULL || otherNode == NULL)
	{
		return true;
	}

	secondaryLsn = secondaryNode->reportedLSN;
	otherNodeLsn = otherNode->reportedLSN;

	if (secondaryLsn == 0 || otherNodeLsn == 0)
	{
		/* we don't have any data yet */
		return false;
	}

	walDifference = Abs(otherNodeLsn - secondaryLsn);

	return walDifference <= delta;
}


/*
 * IsHealthy returns whether the given node is heathly, meaning it succeeds the
 * last health check and its PostgreSQL instance is reported as running by the
 * keeper.
 */
static bool
IsHealthy(AutoFailoverNode *pgAutoFailoverNode)
{
	if (pgAutoFailoverNode == NULL)
	{
		return false;
	}

	return pgAutoFailoverNode->health == NODE_HEALTH_GOOD &&
		   pgAutoFailoverNode->pgIsRunning == true;
}


/*
 * IsUnhealthy returns whether the given node is unhealthy, meaning it failed
 * its last health check and has not reported for more than UnhealthyTimeoutMs,
 * and it's PostgreSQL instance has been reporting as running by the keeper.
 */
static bool
IsUnhealthy(AutoFailoverNode *pgAutoFailoverNode)
{
	TimestampTz now = GetCurrentTimestamp();

	if (pgAutoFailoverNode == NULL)
	{
		return true;
	}

	/* if the keeper isn't reporting, trust our Health Checks */
	if (TimestampDifferenceExceeds(pgAutoFailoverNode->reportTime,
								   now,
								   UnhealthyTimeoutMs))
	{
		if (pgAutoFailoverNode->health == NODE_HEALTH_BAD &&
			TimestampDifferenceExceeds(PgStartTime,
									   pgAutoFailoverNode->healthCheckTime,
									   0))
		{
			if (TimestampDifferenceExceeds(PgStartTime,
										   now,
										   StartupGracePeriodMs))
			{
				return true;
			}
		}
	}

	/*
	 * If the keeper reports that PostgreSQL is not running, then the node
	 * isn't Healthy.
	 */
	if (!pgAutoFailoverNode->pgIsRunning)
	{
		return true;
	}

	/* clues show that everything is fine, the node is not unhealthy */
	return false;
}


/*
 * IsDrainTimeExpired returns whether the node should be done according
 * to the drain time-outs.
 */
static bool
IsDrainTimeExpired(AutoFailoverNode *pgAutoFailoverNode)
{
	bool drainTimeExpired = false;
	TimestampTz now = 0;

	if (pgAutoFailoverNode == NULL ||
		pgAutoFailoverNode->goalState != REPLICATION_STATE_DEMOTE_TIMEOUT)
	{
		return false;
	}

	now = GetCurrentTimestamp();
	if (TimestampDifferenceExceeds(pgAutoFailoverNode->stateChangeTime,
								   now,
								   DrainTimeoutMs))
	{
		drainTimeExpired = true;
	}

	return drainTimeExpired;
}
