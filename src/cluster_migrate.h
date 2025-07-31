#ifndef __CLUSTER_MIGRATE_H
#define __CLUSTER_MIGRATE_H

#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"

/* Forward declaration to allow use as an argument below */
typedef struct slotMigrationLink slotMigrationLink;

int isImportSlotMigrationLink(slotMigrationLink *link);
void clusterHandleSlotMigrationLinkClientClose(slotMigrationLink *link);
void clusterHandleSlotMigrationLinkClientOOM(slotMigrationLink *link);
void clusterFeedSlotExportLinks(int dbid, robj **argv, int argc, int slot);
int clusterIsSlotImporting(int slot);
int clusterIsSlotExporting(int slot);
int clusterIsAnySlotImporting(void);
int clusterIsAnySlotExporting(void);
void clusterMarkImportingSlotsInDb(serverDb *db);
int clusterSlotMigrationShouldInstallWriteHandler(client *c);
void initClusterSlotMigrationLinkList(void);
void clusterSlotMigrationCron(void);
void clusterCommandMigrate(client *c);
void clusterCommandSyncSlots(client *c);
void clusterCommandMigrations(client *c);
void clusterCommandCancelMigration(client *c);
void clusterHandleSlotExportBackgroundSaveDone(int bgsaveerr);
void clusterUpdateSlotExportsOnOwnershipChange(void);
void clusterUpdateSlotImportsOnOwnershipChange(void);
void clusterCleanupSlotMigrationLog(void);
void clusterHandleFlushDuringSlotMigration(void);
size_t clusterGetTotalSlotExportBufferMemory(void);
char *getNameOfSlotExportTarget(int slot);

#endif /* __CLUSTER_MIGRATE_H */
