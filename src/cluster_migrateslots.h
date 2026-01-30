#ifndef __CLUSTER_MIGRATESLOTS_H
#define __CLUSTER_MIGRATESLOTS_H

#include "server.h"
#include "cluster.h"
#include "cluster_legacy.h"

/* Forward declaration to allow use as an argument below */
typedef struct slotMigrationJob slotMigrationJob;

bool isImportSlotMigrationJob(slotMigrationJob *job);
void clusterHandleSlotMigrationClientClose(slotMigrationJob *job);
void clusterHandleSlotMigrationClientOOM(slotMigrationJob *job);
void clusterFeedSlotExportJobs(int dbid, robj **argv, int argc, int slot);
bool clusterIsSlotImporting(int slot);
bool clusterIsSlotExporting(int slot);
bool clusterIsAnySlotImporting(void);
bool clusterIsAnySlotExporting(void);
void clusterMarkImportingSlotsInDb(serverDb *db);
bool clusterSlotMigrationShouldInstallWriteHandler(client *c);
void initClusterSlotMigrationJobList(void);
void clusterSlotMigrationCron(void);
void clusterCommandMigrateSlots(client *c);
void clusterCommandSyncSlots(client *c);
void clusterCommandGetSlotMigrations(client *c);
void clusterCommandCancelSlotMigrations(client *c);
void backgroundSlotMigrationDoneHandler(int exitcode, int bysignal);
void clusterUpdateSlotExportsOnOwnershipChange(void);
void clusterUpdateSlotImportsOnOwnershipChange(void);
void clusterHandleFlushDuringSlotMigration(void);
size_t clusterGetTotalSlotExportBufferMemory(void);
bool clusterSlotFailoverGranted(int slot);
void clusterFailAllSlotExportsWithMessage(char *message);
void clusterHandleSlotMigrationErrorResponse(slotMigrationJob *job);
void killSlotMigrationChild(void);
void clusterCleanSlotImportsOnFullSync(void);
void clusterCleanSlotImportsOnPromotion(void);
void clusterCleanSlotImportsBeforeLoad(void);
void clusterCleanSlotImportsAfterLoad(void);
int clusterRDBSaveSlotImports(rio *rdb, int rdbver);
int clusterRDBLoadSlotImport(rio *rdb);

#endif /* __CLUSTER_MIGRATESLOTS_H */
