#ifndef __CLUSTER_IMPORT_H
#define __CLUSTER_IMPORT_H

#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"

typedef enum slotMigrationLinkState {
    /* Importing states */
    SLOT_IMPORT_CONNECTING,
    SLOT_IMPORT_AUTHENTICATING,
    SLOT_IMPORT_START_SNAPSHOT,
    SLOT_IMPORT_RECEIVE_SNAPSHOT,
    SLOT_IMPORT_RECEIVE_STREAM,
    SLOT_IMPORT_FAILOVER_WAITING_FOR_PAUSED,
    SLOT_IMPORT_FAILOVER_REQUESTED,
    SLOT_IMPORT_FAILOVER_GRANTED,
    SLOT_IMPORT_FINISHED_WAITING_TO_CLEANUP,

    /* Exporting states */
    SLOT_EXPORT_WAITING_TO_SNAPSHOT,
    SLOT_EXPORT_SNAPSHOTTING,
    SLOT_EXPORT_STREAMING,
    SLOT_EXPORT_FAILOVER_PAUSED,
    SLOT_EXPORT_FAILOVER_GRANTED,

    /* Terminal states */
    SLOT_MIGRATION_LINK_FAILED,
    SLOT_MIGRATION_LINK_CANCELLED,
    SLOT_MIGRATION_LINK_SUCCESS,
} slotMigrationLinkState;

typedef enum slotMigrationLinkType {
    SLOT_MIGRATION_EXPORT,
    SLOT_MIGRATION_IMPORT,
} slotMigrationLinkType;

/* A slotMigrationLink represents a link to another node for an ongoing slot
 * migration. A link is created on either end of a migration during the
 * duration of a CLUSTER IMPORT operation. */
typedef struct slotMigrationLink {
    slotMigrationLinkType type;                /* Type of the migration link (either for import or export) */
    time_t ctime;                              /* Migration link creation time. */
    time_t last_update;                        /* Migration link last update time. */
    time_t last_ack;                           /* Migration link last ack time. */
    char nodename[CLUSTER_NAMELEN];            /* Name of the slot import source node, hex string, sha1-size. */
    char linkname[CLUSTER_NAMELEN];            /* Unique name for the link, hex string, sha1-size. */
    client *client;                            /* Client to other node. */
    connection *conn;                          /* (Import only) Connection to slot import source node. */
    slotMigrationLinkState state;              /* State of the slot migration link. */
    sds status_msg;                            /* Human readable status message with more details. */
    list *slot_ranges;                         /* List of the slot ranges we want to import. */
    sds slot_ranges_str;                       /* Precomputed string of the slot ranges, for logging and info. */
    int one_shot;                              /* One shot execution means that we will proceed through slot
                                                * replication and failover in sequence. If the failover does
                                                * not succeed, the import will be failed and replication will
                                                * terminate. */
    mstime_t mf_end;                           /* End time for the manual failover, after this we will unpause. */
    slotMigrationLinkState post_cleanup_state; /* Target state, after pending cleanup is done. */
} slotMigrationLink;

int isImportSlotMigrationLink(void *o);
void clusterHandleSlotMigrationLinkClientClose(void *o);
void clusterHandleSlotMigrationLinkClientOOM(void *o);
void clusterFeedSlotExportLinks(int dbid, robj **argv, int argc);
int clusterIsSlotImporting(int slot);
int clusterIsSlotExporting(int slot);
int clusterIsAnySlotImporting(void);
int clusterIsAnySlotExporting(void);
int clusterSlotMigrationShouldInstallWriteHandler(client *c);
void initClusterSlotMigrationLinkList(void);
void clusterSlotMigrationCron(void);
void clusterCommandImport(client *c, int prepare_only);
void clusterCommandSyncSlots(client *c);
void clusterCommandMigrations(client *c);
void clusterCommandImportCancel(client *c);
void clusterCommandImportCommit(client *c);
void clusterHandleSlotExportBackgroundSaveDone(int bgsaveerr);
void clusterUpdateSlotExportsOnOwnershipChange(void);
void clusterUpdateSlotImportsOnOwnershipChange(void);
void clusterCleanupSlotMigrationLog(void);
void clusterHandleFlushDuringSlotMigration(void);

#endif /* __CLUSTER_IMPORT_H */