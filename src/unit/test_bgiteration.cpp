//#include <algorithm>
#include "generated_wrappers.hpp"
#include <vector>
//#include "amz_assert.h"

//                                                                                                        
//                                                                                                        
//                                                ##                                                      
//     ######:                                    ##                                                      
//     #######:                                   ##                                                      
//     ##   :##                                                                                           
//     ##    ##   ##.####   .####:   ##:  :##   ####      .####:  ##      ##                              
//     ##   :##   #######  .######:   ##  ##    ####     .######: ##.    .##                              
//     #######:   ###.     ##:  :##  :##  ##:     ##     ##:  :##  #: ## :#                               
//     ######:    ##       ########   ##..##      ##     ######## :#:.##.:#:                              
//     ##         ##       ########   ##::##      ##     ########  # :##:##                               
//     ##         ##       ##         :####:      ##     ##        ## ## ##                               
//     ##         ##       ###.  :#    ####       ##     ###.  :#  ###::##                                
//     ##         ##       .#######    ####    ########  .#######  :##..##:                               
//     ##         ##        .#####:    :##:    ########   .#####:  .##  ##                                
//                                                                                                        
//                                                                                                        
//                                                                                                        
//                                                                                                        
//                                                                                                        
//                                                                                                        
//                                                                                                        
//                .####.             ####                                                                 
//                ######             ####                                                                 
//               :##  ##:              ##                                                                 
//               ##:  :##  ##.####     ##      ##    ##                                                   
//               ##    ##  #######     ##      :##  ##                                                    
//               ##    ##  ###  :##    ##       ##: ##.                                                   
//               ##    ##  ##    ##    ##       ###:##                                                    
//               ##    ##  ##    ##    ##       .## #                                                     
//               ##:  :##  ##    ##    ##        ####.                                                    
//               :##  ##:  ##    ##    ##:       :###                                                     
//                ######   ##    ##    #####      ##                                                      
//                .####.   ##    ##    .####      ##.                                                     
//                                               :##                                                      
//                                              ###:                                                      
//                                              ###                                                       
//                                                                                                        
//                                                                                                        
//                                                                                                        
//                                                                                                        
//     ###   ##                                                                    ##                     
//     ###   ##              ##                                                    ##                     
//     ###:  ##              ##                                                    ##                     
//     ####  ##   .####.   #######              ##.####   .####:    :####     :###.##  ##    ##           
//     ##:#: ##  .######.  #######              #######  .######:   ######   :#######  :##  ##            
//     ## ## ##  ###  ###    ##                 ###.     ##:  :##   #:  :##  ###  ###   ##: ##.           
//     ## ## ##  ##.  .##    ##                 ##       ########    :#####  ##.  .##   ###:##            
//     ## :#:##  ##    ##    ##                 ##       ########  .#######  ##    ##   .## #             
//     ##  ####  ##.  .##    ##                 ##       ##        ## .  ##  ##.  .##    ####.            
//     ##  :###  ###  ###    ##.                ##       ###.  :#  ##:  ###  ###  ###    :###             
//     ##   ###  .######.    #####              ##       .#######  ########  :#######     ##              
//     ##   ###   .####.     .####              ##        .#####:    ###.##   :###.##     ##.             
//                                                                                       :##              
//                                                                                      ###:              
//                                                                                      ###               
//                                                                                                        
//                                                                                                        
//                                                                                                        
//                                                                              ##                        
//       :####                                                                  ##                        
//       #####                                                                  ##                        
//       ##                                                                                               
//     #######    .####.    ##.####             ##.####   .####:   ##:  :##   ####      .####:  ##      ##
//     #######   .######.   #######             #######  .######:   ##  ##    ####     .######: ##.    .##
//       ##      ###  ###   ###.                ###.     ##:  :##  :##  ##:     ##     ##:  :##  #: ## :# 
//       ##      ##.  .##   ##                  ##       ########   ##..##      ##     ######## :#:.##.:#:
//       ##      ##    ##   ##                  ##       ########   ##::##      ##     ########  # :##:## 
//       ##      ##.  .##   ##                  ##       ##         :####:      ##     ##        ## ## ## 
//       ##      ###  ###   ##                  ##       ###.  :#    ####       ##     ###.  :#  ###::##  
//       ##      .######.   ##                  ##       .#######    ####    ########  .#######  :##..##: 
//       ##       .####.    ##                  ##        .#####:    :##:    ########   .#####:  .##  ##  
//                                                                                                        
//                                                                                                        
//                                                                                                        
//                                                                                                        



using namespace ::testing;

extern "C" {
    #include "stdlib.h"
    #include "bgiteration.h"
    #include "server.h"
    //#include "serverassert.h"
    #define using usingvar // compile hack
    #include "module.h"
    #undef using
    extern hashtableType commandSetType;
    extern dictType keylistDictType;
    bool iteratorRepldoneFn(void *privdata);
    void iteratorCleanupFn(bool terminated, void *privdata);
    void bgIteration_feedIterators(void);
    void createSharedObjects(void);
    void hashtableDump(hashtable *ht);
    void rehashStepExpand(hashtable *ht); // in hashtable.c (non-API)
    void bgIteration_unitTestDisableCloning(void);
    void bgIteration_unitTestEnableCloning(int item_bytes, int pool_bytes);
}


// The private data is a pointer to arbitrary data.  This value is used just to
//  test that the correct value is passed through.
#define PRIVDATA reinterpret_cast<void*>(12345)

// A bgIteration cleanup function used for testing.
int cleanupCount;
bool cleanupTerminated;
void iteratorCleanupFn(bool terminated, void *privdata) {
    EXPECT_EQ(privdata, PRIVDATA);
    cleanupCount++;
    cleanupTerminated = terminated;
}


// A bgIteration repldone function used for testing.
int repldoneCount;
bool iteratorRepldoneFn(void *privdata) {
    EXPECT_EQ(privdata, PRIVDATA);
    repldoneCount++;
    return true;
}


// A more complicated repldone function that can delay the replcation done condition.
bool isReplDoneReady;
bool iteratorRepldoneFnNotBeingReadyInitially(void *privdata) {
    EXPECT_EQ(privdata, PRIVDATA);
    // This is to test the behavior when Repl Done function is not ready to be executed.
    if (!isReplDoneReady) {
        isReplDoneReady = true;
        return false;
    }
    repldoneCount++;
    return true;
}


static const char *logfile = "";

/* Most of the bgIteration unit tests are based on a CMD instance with 2 DBs.  There are 8 keys in
 * each DB.  The keys are named A0, B0, C0, D0, E0, F0, G0, H0 for DB-0 and A1, B1, C1, D1, E1, F1,
 * G1, H1 for DB-1.  There are a number of helper functions to simulate certain key modification
 * actions within our test configuration.  Note that this is isolated from the actual call to
 * processCommand.
 * 
 * Because most of bgIteration is based on an ordered processing of keys, it doesn't matter if we
 * are simulating CMD or CME, full scan, or slot-based.  The majority of tests are independent of
 * these concerns.
 * 
 * However, there are some tests which are are unique to these configurations and use a specialized
 * derived class to handle the differences.  We do not want to duplicate all of the tests for
 * the different configurations, but we do want to ensure that each configuration works properly.
 *   - bgIterationTestCluster - handles tests unique to full scan in cluster mode
 *   - bgIterationTestClusterSlots - handles tests unique to cluster slot-based iteration
 */
class BgIterationTest : public ::testing::Test {
    private:
        static const int DB_COUNT = 2;
        static const int ITEMS_PER_DB = 8;

        // This is the expected order of the keys when hashed
        const char *keys[DB_COUNT][ITEMS_PER_DB] = {{"D0", "G0", "H0", "C0", "F0", "A0", "B0", "E0"},
                                                    {"B1", "C1", "F1", "G1", "E1", "D1", "A1", "H1"}};

    protected:
        static const int TOTAL_ITEMS = DB_COUNT * ITEMS_PER_DB;
        static const int LAST_ITEM = TOTAL_ITEMS - 1;

        MockValkey mock;
        RealValkey real;

        struct serverCommand dummy_cmd = {0};

        // Helper functions for accessing the keys.  We can access by db(0..1) and seq(0..4)
        //  or by item number (0..9).
        // NOTE: These virtual functions can be overridden in subclasses which may have different item layout.
        virtual const char * getKeyAtDbSeq(int db, int seq) {
            assert(db < DB_COUNT);
            assert(seq < ITEMS_PER_DB);
            return keys[db][seq];
        }

        virtual int getDbFromItemNum(int itemNum) {
            assert(itemNum < DB_COUNT * ITEMS_PER_DB);
            return itemNum / ITEMS_PER_DB;
        }

        virtual int getSeqFromItemNum(int itemNum) {
            assert(itemNum < DB_COUNT * ITEMS_PER_DB);
            return itemNum % ITEMS_PER_DB;
        }

        const char * keyStr(int itemNum) {
            return getKeyAtDbSeq(getDbFromItemNum(itemNum), getSeqFromItemNum(itemNum));
        }

        int itemNumFromKey(const char * key) {
            for (int itemNum = 0;  itemNum < DB_COUNT * ITEMS_PER_DB;  itemNum++) {
                if (strcmp(key, keyStr(itemNum)) == 0) return itemNum;
            }
            return -1;
        }


        // Do some general initialization before starting the suite.  Normally, the tests are run in
        //  isolation - and this isn't much different than SetUp().  But if running the
        //  entire test suite together (just manually running the test executable), this gets called
        //  only once.
        static void SetUpTestSuite() {
            monotonicInit();
            
            bzero(&server, sizeof(server));
            server.hz = 100;
            server.logfile = const_cast<char*>(logfile);
            createSharedObjects();

            moduleInitModulesSystem();

            server.commands = hashtableCreate(&commandSetType);
            server.orig_commands = hashtableCreate(&commandSetType);
            populateCommandTable();
        }


        static void TearDownTestSuite() {
            hashtableRelease(server.commands);
            hashtableRelease(server.orig_commands);
        }


        void initializeServerDb(int dbid, int slot_count_bits = 0) {
            server.db[dbid] = static_cast<serverDb *>(zcalloc(sizeof(serverDb)));
            server.db[dbid]->id = dbid;
            server.db[dbid]->keys = kvstoreCreate(&kvstoreKeysHashtableType, slot_count_bits, 0);
            server.db[dbid]->expires = kvstoreCreate(&kvstoreExpiresHashtableType, slot_count_bits, 0);
            server.db[dbid]->watched_keys = dictCreate(&keylistDictType);
            kvstoreExpand(server.db[dbid]->keys, 8, 0, NULL);
        }


        void addKeyAndValObjsToDb(int dbid, sds key, sds val) {
            robj *key_obj = createStringObjectFromSds(key);
            robj *val_obj = createStringObjectFromSds(val);
            dbAdd(server.db[dbid], key_obj, &val_obj);
            decrRefCount(key_obj);
        }


        void addKeyToDb(int dbid, const char *key, const char *val) {
            addKeyAndValObjsToDb(dbid, sdsnew(key), sdsnew(val));
        }


        virtual void setupDatabase() {
            // For these unit tests, a standard database is constructed.  The order of items in the
            //  hash table is important, and this is validated here.  If the hash table
            //  implementation changes, we will find out quickly at this point.  All other tests
            //  will become invalid!

            server.dbnum = 2;
            server.cluster_enabled = false;
            server.db = static_cast<serverDb **>(zcalloc(sizeof(serverDb *) * server.dbnum));

            for (int dbid = 0;  dbid < server.dbnum;  dbid++) {
                initializeServerDb(dbid);
            }

            // With hashtable, it can be difficult to get our keys spread across different buckets.
            //  Here we play with hashtable size and rehashing to get comfortable scenarios for testing.
            // NOTE: If the hashtable bucketization changes, we'll need to evaluate the tests for
            //       changes.  Since bgIteration processes a bucket at a time, we need to evaluate
            //       all the tests when bucketization changes.
            // As an alternative, we could mock all of the hashtable activity, but it's better if we
            //  can use the real functionality as much as possible.

            kvstoreExpand(server.db[0]->keys, 16, 0, NULL);
            addKeyToDb(0, "A0", "A0");
            addKeyToDb(0, "B0", "B0");
            addKeyToDb(0, "C0", "C0");
            addKeyToDb(0, "D0", "D0");
            addKeyToDb(0, "E0", "E0");
            addKeyToDb(0, "F0", "F0");
            addKeyToDb(0, "G0", "G0");
            addKeyToDb(0, "H0", "H0");
            hashtable *ht = kvstoreGetHashtable(server.db[1]->keys, 0);
            hashtablePauseRehashing(ht);

            kvstoreExpand(server.db[1]->keys, 16, 0, NULL);
            addKeyToDb(1, "A1", "A1");
            addKeyToDb(1, "B1", "B1");
            addKeyToDb(1, "C1", "C1");
            addKeyToDb(1, "D1", "D1");
            addKeyToDb(1, "E1", "E1");
            addKeyToDb(1, "F1", "F1");
            addKeyToDb(1, "G1", "G1");
            addKeyToDb(1, "H1", "H1");
            // Now, let's increase the size and start a rehash on the 2nd DB.  This ensures that
            //  iteration is working even if a hashtable is in the middle of rehashing.  We choose
            //  a 128 size so that rehashed keys all get unique buckets.
            kvstoreExpand(server.db[1]->keys, 128, 0, NULL);
            ht = kvstoreGetHashtable(server.db[1]->keys, 0);
            rehashStepExpand(ht); // in hashtable.c (non-API)
            rehashStepExpand(ht); // and rehash the 2nd bucket also
            hashtablePauseRehashing(ht);

            // The bucketization should look like this.  Remember that DB-1 is in
            //  the middle of a rehash, so it has 2 tables.
            //
            // DB: 0  SLOT: 0
            // Table 0, used 8, exp 2, top-level buckets 4, child buckets 0
            //   Bucket 0:1 level:0
            //     0 h2 63, key "D0"
            //     1 h2 a5, key "G0"
            //     2 h2 ca, key "H0"
            //   Bucket 0:2 level:0
            //     0 h2 91, key "C0"
            //     1 h2 88, key "F0"
            //   Bucket 0:3 level:0
            //     0 h2 b8, key "A0"
            //     1 h2 f5, key "B0"
            //     2 h2 13, key "E0"
            // Table 1, used 0, exp -1, top-level buckets 0, child buckets 0
            //
            // DB: 1  SLOT: 0
            // Table 0, used 3, exp 2, top-level buckets 4, child buckets 0
            //   Bucket 0:0 level:0   <- rehashed into table 1
            //   Bucket 0:1 level:0   <- rehashed into table 1
            //   Bucket 0:2 level:0
            //     0 h2 18, key "B1"
            //     1 h2 fd, key "C1"
            //   Bucket 0:3 level:0
            //     0 h2 6f, key "F1"
            // Table 1, used 5, exp 5, top-level buckets 32, child buckets 0
            //   Bucket 1:1 level:0
            //     0 h2 ad, key "G1"
            //   Bucket 1:5 level:0
            //     0 h2 0c, key "E1"
            //   Bucket 1:12 level:0
            //     0 h2 e9, key "D1"
            //   Bucket 1:17 level:0
            //     0 h2 36, key "A1"
            //   Bucket 1:29 level:0
            //     0 h2 9e, key "H1"
            //   Bucket 1:30 level:0


            // In case we need to debug...
            // Used to generate comment above, showing bucketization.
            if (0) debugPrintBucketInfo();

            // Validate that the iteration order matches the expected order
            for (int db = 0;  db < server.dbnum;  db++) {
                ht = kvstoreGetHashtable(server.db[db]->keys, 0);
                hashtableIterator *it = hashtableCreateIterator(ht, 0);
                robj *next;
                int i = 0;
                while (hashtableNext(it, reinterpret_cast<void**>(&next))) {
                    ASSERT_THAT(next, robjEqualsStr(getKeyAtDbSeq(db, i++)));
                }
                hashtableReleaseIterator(it);
            }
        }


        void SetUp() override {
            server.main_thread_id = pthread_self();
            server.forkless_options_supported = 1;
            objectSetMetadataSize(sizeof(bgIterationEntryMetadata));

            bgIteration_unitTestDisableCloning();

            setupDatabase();

            EXPECT_CALL(mock, aeCreateTimeEvent(_,_,_,_,_)).WillRepeatedly(Return(0));
            bgIteration_init();

            cleanupCount = 0;
            repldoneCount = 0;
            isReplDoneReady = false;

            // By default, in tests, we treat items as not having an expiration
            //JHB EXPECT_CALL(mock, getExpire(_,_)).WillRepeatedly(Return(-1));

            // By default, do nothing for these
            EXPECT_CALL(mock, blockClientInUseOnKeys(_,_,_)).WillRepeatedly(Return());
            EXPECT_CALL(mock, unblockClientsInUseOnKey(_)).WillRepeatedly(Return());

            // By default, expect no permission issues
            EXPECT_CALL(mock, ACLCheckAllUserCommandPerm(_,_,_,_,_,_)).WillRepeatedly(Return(ACL_OK));

            //JHB EXPECT_CALL(mock, lookupCommandOrOriginal(_)).WillRepeatedly(Return(&dummy_cmd));
        }


        void TearDown() override {
            bgIteration_feedIterators();    // process returning stuff before deleting DB
            bgIteration_feedIterators();    // in case an iterator was closed there might be more
            for (int i = 0;  i < server.dbnum;  i++) {
                if (server.db[i]->keys) kvstoreRelease(server.db[i]->keys);
                if (server.db[i]->expires) kvstoreRelease(server.db[i]->expires);
                dictRelease(server.db[i]->watched_keys);
                zfree(server.db[i]);
            }
            zfree(server.db);
        }


        // void update_keys(const char **new_keys, int db, int len) {
        //     memcpy(keys[db], new_keys, len * sizeof(const char *));
        // }






        // Deletes an item from the DB (often at the start of a test) - but does NOT notify
        //  bgIteration.  bgIteration_keyDelete() should be explicitly called where needed.
        void simpleDelItem(int itemNum) {
            int db = getDbFromItemNum(itemNum);

            sds delKey = sdsnew(keyStr(itemNum));
            int rc = kvstoreHashtableDelete(server.db[db]->keys, 0, delKey);
            ASSERT_EQ(rc, 1);
            sdsfree(delKey);
        }


        // Find the actual dbEntry object by itemNum
        dbEntry * getItem(int itemNum) {
            int db = getDbFromItemNum(itemNum);
            sds key = sdsnew(keyStr(itemNum));
            dbEntry *de = dbFind(server.db[db], key);
            sdsfree(key);
            return de;
        }


        // The test expects that the next item read will be BGITERATOR_ITEM_COMPLETE
        void expectReadComplete(bgIterator *iter) {
            bgIteration_feedIterators();
            bgIteratorItem *item = bgIteratorRead(iter);
            EXPECT_EQ(item->type, BGITERATOR_ITEM_COMPLETE);
            bgIteratorClose(iter);

            int oldCleanupCount = cleanupCount;
            bgIteration_feedIterators();
            EXPECT_EQ(cleanupCount, oldCleanupCount + 1);
        }


        // The test is cleaning up and isn't validating the remaining cleanup
        void expectAnythingCleanup(bgIterator *iter) {
            while (true) {
                bgIteration_feedIterators();
                bgIteratorItem *item = bgIteratorRead(iter);
                if ((item->type == BGITERATOR_ITEM_COMPLETE
                        || item->type == BGITERATOR_ITEM_TERMINATED)) {
                    bgIteratorClose(iter);
                    break;
                }
            }
            bgIteration_feedIterators();    // Recognize the closed iterator
            EXPECT_EQ(cleanupCount, 1);
        }


        void expectDictEntryMetadataMatch(dbEntry *de1, dbEntry *de2) {
            bgIterationEntryMetadata *dm1 = static_cast<bgIterationEntryMetadata *>(objectGetMetadata(de1));
            bgIterationEntryMetadata *dm2 = static_cast<bgIterationEntryMetadata *>(objectGetMetadata(de2));

            EXPECT_NE(dm1, nullptr);
            EXPECT_NE(dm2, nullptr);
            EXPECT_EQ(dm1->iterator_epoch, dm2->iterator_epoch);
        }


        // Useful when debugging new tests.  It reads/prints all remaining items then crashes.
        void cleanupIteratorDebugPrint(bgIterator *iter) {
            bool done = false;
            printf("[DEBUG] Printing bgIterator '%s' items:\n", bgIteratorName(iter));
            while (!done) {
                bgIteration_feedIterators();
                bgIteratorItem *item = bgIteratorRead(iter);
                switch (item->type) {
                    case BGITERATOR_ITEM_DBENTRY:
                        {
                            auto obj = item->u.dbe.de;
                            const char * keyStr = objectGetKey(obj);
                            printf("Entry: %s -> %s [itemNum: %i]\n",
                                   keyStr,
                                   static_cast<char *>(objectGetVal(obj)),
                                   itemNumFromKey(keyStr));
                            break;
                    }
                    case BGITERATOR_ITEM_REPLICATION:
                        printf("Repl: DB=%d : ", item->dbid);
                        for (int i = 0;  i < item->u.repl.argc;  i++)
                            printf("%s ", static_cast<char*>(objectGetVal(item->u.repl.argv[i])));
                        printf("\n");
                        break;
                    case BGITERATOR_ITEM_COMPLETE:
                    case BGITERATOR_ITEM_TERMINATED:
                        bgIteratorClose(iter);
                        done = true;
                        break;
                    default:
                        printf("unhandled: %d\n", item->type);
                }
            }
            bgIteration_feedIterators();    // Recognize the closed iterator
            ASSERT_TRUE(false); // Halt the test here
        }


        // Make a copy of the metadata
        void * cloneMetadata(dbEntry *de) {
            int size = objectGetMetadataSize(de);
            void *metadata = zmalloc(size);
            memcpy(metadata, objectGetMetadata(de), size);
            return metadata;
        }


        // Compare a previous metadata copy to an existing entry
        void compareAndFreeClonedMetadata(dbEntry *de, void *metadata) {
            EXPECT_EQ(memcmp(objectGetMetadata(de), metadata, objectGetMetadataSize(de)), 0);
            zfree(metadata);
        }


        // The test expects the next item will be a specific key
        //  The item value is verified against the default unless provided as a parameter.
        void expectReadKey(bgIterator *iter, int itemNum, const char *value=nullptr) {
            int db = getDbFromItemNum(itemNum);

            bgIteration_feedIterators();
            bgIteratorItem *item = bgIteratorRead(iter);
            bgIteration_feedIterators();

            ASSERT_EQ(item->type, BGITERATOR_ITEM_DBENTRY);
            EXPECT_EQ(item->dbid, db);
            EXPECT_FALSE(item->u.dbe.is_cloned);
            // if (item->u.dbe.is_cloned) {  // JHB - wrong place to check this.
            //     // If the entry is cloned, make sure we copied the metadata
            //     dbEntry *cloned_dbEntry = item->u.dbe.de;
            //     dbEntry *original_dbEntry = getItem(itemNum);
            //     expectDictEntryMetadataMatch(original_dbEntry, cloned_dbEntry);
            // }
            EXPECT_STREQ(objectGetKey(item->u.dbe.de), keyStr(itemNum));
            if (value) {
                EXPECT_THAT(item->u.dbe.de, robjEqualsStr(value));
            } else {
                EXPECT_THAT(item->u.dbe.de, robjEqualsStr(keyStr(itemNum)));
            }
        }


        // The test expects the next item will be a specific key amd that the item is cloned.
        //  Metadata is tested (to make sure the clone includes the proper metadata).
        //  The item value is verified against the default unless provided as a parameter.
        void expectReadClonedKey(bgIterator *iter, int itemNum, void *metadata, const char *value=nullptr) {
            int db = getDbFromItemNum(itemNum);

            bgIteration_feedIterators();
            bgIteratorItem *item = bgIteratorRead(iter);
            bgIteration_feedIterators();

            ASSERT_EQ(item->type, BGITERATOR_ITEM_DBENTRY);
            EXPECT_EQ(item->dbid, db);
            EXPECT_TRUE(item->u.dbe.is_cloned);
            compareAndFreeClonedMetadata(item->u.dbe.de, metadata);
            EXPECT_STREQ(objectGetKey(item->u.dbe.de), keyStr(itemNum));
            if (value) {
                EXPECT_THAT(item->u.dbe.de, robjEqualsStr(value));
            } else {
                EXPECT_THAT(item->u.dbe.de, robjEqualsStr(keyStr(itemNum)));
            }
        }


        // Test expects the next key, but specified by key name, not itemNum.
        void expectReadDbKeyValue(bgIterator *iter, int db, const char *key, const char *value) {
            bgIteration_feedIterators();
            bgIteratorItem *item = bgIteratorRead(iter);
            bgIteration_feedIterators();

            ASSERT_EQ(item->type, BGITERATOR_ITEM_DBENTRY);
            EXPECT_EQ(item->dbid, db);
            EXPECT_STREQ(objectGetKey(item->u.dbe.de), key);
            EXPECT_THAT(item->u.dbe.de, robjEqualsStr(value));
        }


        // Test expect to read a sequence of key items
        void expectReadKeySequence(bgIterator *iter, int startItem, int endItem) {
            for (int i = startItem;  i <= endItem;  i++) expectReadKey(iter, i);
        }


        // Just like expectReadKey, but also tests that a previous item is becoming unblocked.
        void expectReadKeyWithUnblock(bgIterator *iter, int itemNum, int unblockItem, const char *value=nullptr) {
            bool blocked = true;
            EXPECT_CALL(mock, unblockClientsInUseOnKey(robjEqualsStr(keyStr(unblockItem))))
                    .WillOnce(Assign(&blocked, false));
            expectReadKey(iter, itemNum, value);
            EXPECT_FALSE(blocked);
        }


        // Test expects to read a replication item matching the command help by client 'c'
        void expectReadReplication(bgIterator *iter, client *c) {
            bgIteration_feedIterators();
            bgIteratorItem *item = bgIteratorRead(iter);
            bgIteration_feedIterators();

            ASSERT_EQ(item->type, BGITERATOR_ITEM_REPLICATION);
            EXPECT_EQ(item->dbid, c->db->id);
            EXPECT_EQ(item->u.repl.cmd, c->cmd);
            EXPECT_EQ(item->u.repl.argc, c->argc);
            for (int i = 0;  i < c->argc;  i++) {
                EXPECT_STREQ(static_cast<char*>(objectGetVal(item->u.repl.argv[i])),
                             static_cast<char*>(objectGetVal(c->argv[i])));
            }
        }


        // We expect to read a MULTI command which should have been inserted.
        void expectReadMultiReplication(bgIterator *iter) {
            bgIteration_feedIterators();
            bgIteratorItem *item = bgIteratorRead(iter);
            bgIteration_feedIterators();

            ASSERT_EQ(item->type, BGITERATOR_ITEM_REPLICATION);
            EXPECT_EQ(item->u.repl.cmd, lookupCommandByCString("multi"));
        }


        // We expect to read an EXEC command which should have been inserted.
        void expectReadExecReplication(bgIterator *iter) {
            bgIteration_feedIterators();
            bgIteratorItem *item = bgIteratorRead(iter);
            bgIteration_feedIterators();

            ASSERT_EQ(item->type, BGITERATOR_ITEM_REPLICATION);
            EXPECT_EQ(item->u.repl.cmd, lookupCommandByCString("exec"));
        }


        // Expecting that a DEL command should have been replicated.
        void expectReadReplicationDel(bgIterator *iter, int itemNum) {
            int db = getDbFromItemNum(itemNum);

            bgIteration_feedIterators();
            bgIteratorItem *item = bgIteratorRead(iter);
            bgIteration_feedIterators();

            ASSERT_EQ(item->type, BGITERATOR_ITEM_REPLICATION);
            EXPECT_EQ(item->dbid, db);
            EXPECT_EQ(item->u.repl.cmd, lookupCommandByCString("DEL"));
            EXPECT_EQ(item->u.repl.argc, 2);
            EXPECT_THAT(item->u.repl.argv[0], robjEqualsStr("DEL"));
            EXPECT_THAT(item->u.repl.argv[1], robjEqualsStr(keyStr(itemNum)));
        }


        // Expecting that a special SWAPDB item has been inserted.
        void expectReadSwapDB(bgIterator *iter, int db1, int db2) {
            bgIteration_feedIterators();
            bgIteratorItem *item = bgIteratorRead(iter);
            bgIteration_feedIterators();

            ASSERT_EQ(item->type, BGITERATOR_ITEM_SWAPDB);
            EXPECT_EQ(item->dbid, db1);
            EXPECT_EQ(item->u.dbid2, db2);
        }


        // Used to examine the physical bucket layout in the hash table.  Generated the comment
        //  above which shows each item in each bucket.  Necessary if hash table layout changes.
        void debugPrintBucketInfo(int num_slots = -1) {
            for (int db = 0;  db < server.dbnum;  db++) {
                int n = (num_slots == -1) ? kvstoreNumHashtables(server.db[db]->keys) : num_slots;
                for (int slot = 0;  slot < n;  slot++) {
                    hashtable *ht = kvstoreGetHashtable(server.db[db]->keys, slot);
                    printf("DB: %d  SLOT: %d\n", db, slot);
                    hashtableDump(ht);
                }
            }
        }


        // Creates a client with a write command (SET) for the given itemNum
        client * getWriteClient(int itemNum, const char *value) {
            int db = getDbFromItemNum(itemNum);

            client *c = static_cast<client*>(zcalloc(sizeof(client)));

            c->cmd = lookupCommandByCString("set");
            c->db = server.db[db];

            c->argc = 3;
            c->argv = static_cast<robj**>(zcalloc(sizeof(robj*) * c->argc));
            c->argv[0] = createStringObjectFromSds(sdsnew(c->cmd->fullname));
            c->argv[1] = createStringObjectFromSds(sdsnew(keyStr(itemNum)));
            c->argv[2] = createStringObjectFromSds(sdsnew(value));

            return c;
        }


        // Create a client with a write command that touches multiple keys
        client * getWriteMultiKeysClient(
                const char * cmdName,
                int dstItemNum,
                const std::vector<int> & srcItemsNum) {

            assert(!srcItemsNum.empty());

            const int db = getDbFromItemNum(dstItemNum);
            std::for_each(srcItemsNum.cbegin(), srcItemsNum.cend(), [&db, this](int srcItemNum) {
                assert(db == getDbFromItemNum(srcItemNum));
            });

            client *c = static_cast<client*>(zcalloc(sizeof(client)));

            c->cmd = lookupCommandByCString(cmdName);
            assert(c->cmd != nullptr);
            c->db = server.db[db];

            c->argc = 2 + srcItemsNum.size();
            c->argv = static_cast<robj**>(zcalloc(sizeof(robj*) * c->argc));
            c->argv[0] = createStringObjectFromSds(sdsnew(c->cmd->fullname));
            c->argv[1] = createStringObjectFromSds(sdsnew(keyStr(dstItemNum)));
            for (unsigned int i = 0;  i < srcItemsNum.size();  i++) {
                c->argv[2 + i] = createStringObjectFromSds(sdsnew(keyStr(srcItemsNum[i])));
            }

            return c;
        }


        client * getWrite2KeysClient(const char * cmdName, int dstItemNum, int srcItemNum) {
            return getWriteMultiKeysClient(cmdName, dstItemNum, {srcItemNum});
        }


        client * getWrite3KeysClient(
                const char * cmdName, int dstItemNum, int src1ItemNum, int src2ItemNum) {
            return getWriteMultiKeysClient(cmdName, dstItemNum, {src1ItemNum, src2ItemNum});
        }


        // Create a client with a MULTI/EXEC block.
        //  This parses a series of commands separated by ';'
        //  Example: getMultiClient("SET A0 xxx; SELECT 1; SET A1 xxx; SET B1 xxx")
        client * getMultiClient(const char *commands, int dbid = 0) {
            char *commandsCopy = zstrdup(commands);  // a mutable copy
            char *commandStr, *commandStrSave;
            char *token, *tokenSave;

            client *c = static_cast<client*>(zcalloc(sizeof(client)));
            c->db = server.db[dbid];
            initClientMultiState(c);
            c->flag.multi = 1;
            c->mstate->cmd_flags |= CMD_WRITE;

            commandStr = strtok_r(commandsCopy, ";", &commandStrSave);
            while (commandStr != NULL) {

                token = strtok_r(commandStr, " ", &tokenSave);
                c->cmd = lookupCommandByCString(token);

                c->argv = static_cast<robj**>(zcalloc(sizeof(robj*) * 5));   // command + 4 args

                for (int i = 0;  token != NULL;  i++) {
                    c->argv[i] = createStringObject(token, strlen(token));
                    c->argc = i+1;
                    token = strtok_r(NULL, " ", &tokenSave);
                }

                queueMultiCommand(c, 0);
                freeClientArgv(c);

                commandStr = strtok_r(NULL, ";", &commandStrSave);
            }

            c->cmd = lookupCommandByCString("exec");
            c->argc = 1;
            c->argv = static_cast<robj**>(zcalloc(sizeof(robj*) * c->argc));
            c->argv[0] = createStringObjectFromSds(sdsnew("EXEC"));

            zfree(commandsCopy);
            return c;
        }


        // Initially, a MULTI client is set up to execute the EXEC command (which examines the
        //  contents of the multi/exec block).  This function advances the client to begin executing
        //  the individual commands within the multi/exec block.
        void advanceMultiClientToCommand(client *c, int cmdNum) {
            assert(cmdNum >= 0 && cmdNum < c->mstate->count);
            c->argc = c->mstate->commands[cmdNum].argc;
            c->argv = c->mstate->commands[cmdNum].argv;
            c->argv_len = c->mstate->commands[cmdNum].argv_len;
            c->cmd = c->realcmd = c->mstate->commands[cmdNum].cmd;
        }


        // A client with a fictional command:
        //  SETGET <write_key> <value> <read_key>
        //  - writes a value to the first key (making this CMD_WRITE | CMD_WRITE_FIRSTKEY_ONLY)
        //  - reads a second key
        client * getSetGetClient(int itemNum1, const char *value1, int itemNum2) {
            // Fictional command which writes to 1st key and reads the 2nd
            int db = getDbFromItemNum(itemNum1);
            assert(db == getDbFromItemNum(itemNum2));   // (this would be a testcase error)

            client *c = static_cast<client*>(zcalloc(sizeof(client)));
            struct serverCommand *cmd
                    = static_cast<struct serverCommand*>(zcalloc(sizeof(struct serverCommand)));

            cmd->fullname = const_cast<char*>("SETGET");
            cmd->arity = 4;
            cmd->flags = CMD_WRITE | CMD_WRITE_FIRSTKEY_ONLY;

            cmd->legacy_range_key_spec.begin_search_type = KSPEC_BS_INDEX;
            cmd->legacy_range_key_spec.bs.index.pos = 1;    // firstkey
            cmd->legacy_range_key_spec.fk.range.lastkey = -1;
            cmd->legacy_range_key_spec.fk.range.keystep = 2;

            c->cmd = cmd;
            c->db = server.db[db];

            c->argc = 4;
            c->argv = static_cast<robj**>(zcalloc(sizeof(robj*) * c->argc));
            c->argv[0] = createStringObjectFromSds(sdsnew(cmd->fullname));
            c->argv[1] = createStringObjectFromSds(sdsnew(keyStr(itemNum1)));
            c->argv[2] = createStringObjectFromSds(sdsnew(value1));
            c->argv[3] = createStringObjectFromSds(sdsnew(keyStr(itemNum2)));

            return c;
        }


        // Client with a fictional write command with no keys specified
        client * getNoKeysWriteClient() {
            // Fictional command which is marked WRITE, but has no keys.
            client *c = static_cast<client*>(zcalloc(sizeof(client)));
            struct serverCommand *cmd
                    = static_cast<struct serverCommand*>(zcalloc(sizeof(struct serverCommand)));

            cmd->fullname = const_cast<char*>("NOKEYSWRITE");
            cmd->arity = 1;
            cmd->flags = CMD_WRITE;

            cmd->legacy_range_key_spec.begin_search_type = KSPEC_BS_INVALID;    // No keys

            c->cmd = cmd;
            c->db = server.db[0];

            c->argc = 1;
            c->argv = static_cast<robj**>(zcalloc(sizeof(robj*) * c->argc));
            c->argv[0] = createStringObjectFromSds(sdsnew(cmd->fullname));

            return c;
        }


        void freeClientArgv(client *c) {
            for (int i = 0; i < c->argc; i++) decrRefCount(c->argv[i]);
            zfree(c->argv);
            c->argv = NULL;
            c->argc = 0;
        }


        // During testing, we create some fake commands.  This checks if the command is real or fake.
        //  A fake command is dynamically allocated and can be freed.  Real commands are static.
        bool isRealValkeyCommand(struct serverCommand *cmd) {
            return lookupCommandByCString(cmd->declared_name);
        }


        void freeTestClient(client *c) {
            freeClientMultiState(c);
            freeClientArgv(c);

            if (!isRealValkeyCommand(c->cmd)) zfree(c->cmd);

            zfree(c);
        }


        // Simulate what happens when a write command is blocked
        void simulateBlockedWrite(client *c, int expectedNumberBlockedKeys = 1) {
            EXPECT_CALL(mock, blockClientInUseOnKeys(c,expectedNumberBlockedKeys,_)).Times(1);
            bool blocked = bgIteration_blockClientIfRequired(c);
            EXPECT_TRUE(blocked);
        }


        // Simulate what happens when a write command isn't blocked
        void simulateUnblockedWrite(client *c) {
            EXPECT_CALL(mock, blockClientInUseOnKeys(c,_,_)).Times(0);
            bool blocked = bgIteration_blockClientIfRequired(c);
            EXPECT_FALSE(blocked);
        }


        // Simulate what happens when a write command is NOT blocked, because the key can be cloned
        //  and expedited.  This requires a scenario where we would normally need to block the
        //  client so that bgIteration can process the item.
        void simulateClonedWrite(bgIterator *it, client *c) {
            bgIteratorStatus status;
            bgIteratorGetStatus(it, &status);
            unsigned long initialClones = status.dbentry_clones_queued;

            // Client should not get blocked
            EXPECT_CALL(mock, blockClientInUseOnKeys(c,_,_)).Times(0);
            bool blocked = bgIteration_blockClientIfRequired(c);
            EXPECT_FALSE(blocked);

            // Ensure that cloning took place
            bgIteratorGetStatus(it, &status);
            EXPECT_EQ(status.dbentry_clones_queued, (initialClones + 1));

            // Ensure that the real item isn't inuse (because we cloned it instead)
            dbEntry *de = dbFind(c->db, static_cast<sds>(objectGetVal(c->argv[1])));
            ASSERT_FALSE(bgIteration_isEntryInuse(de));
        }           


        // Simulates what happens when a write command (SET) actually executes.  This requires a
        //  scenario where we would NOT be blocked on the write.  It actually alters the value of
        //  the key and updates the metadata.
        void simulateUnblockedWriteWithModification(client *c) {
            EXPECT_CALL(mock, blockClientInUseOnKeys(c,_,_)).Times(0);
            bool blocked = bgIteration_blockClientIfRequired(c);
            EXPECT_FALSE(blocked);

            //dbFind(c->db, static_cast<sds>(objectGetVal(c->argv[1])));  JHB

            // Fake execution of the command - touch the iterator_epoch counter and swap the value
            // We need to duplicate the value because setKey() can reallocate it.
            robj *value = dupStringObject(c->argv[2]);
            setKey(c, c->db, c->argv[1], &value, SETKEY_ADD_OR_UPDATE);

            // Let's make sure that setKey updated the iteration epoch (as it should have)
            dbEntry *de = dbFind(c->db, static_cast<sds>(objectGetVal(c->argv[1])));
            bgIterationEntryMetadata *md = static_cast<bgIterationEntryMetadata*>(objectGetMetadata(de));
            EXPECT_EQ(md->iterator_epoch, bgIteration_getEpoch());

            bgIteration_handleCommandReplication(c->db->id, c->cmd, c->argc, c->argv);
        }


        // Simulate execution of a MULTI/EXEC transaction for a client `c` without blocking.
        //  It replays all queued commands and ensures replication matches a real transaction.
        //  command replication flag is revalidated when exec command is processed.
        //  This requires a scenario where we don't expect the client to be blocked.
        void simulateUnblockedMultiExec(client *c) {

            // simulate EXEC command of the multi/exec client
            simulateUnblockedWrite(c);
            server.in_exec = 1;
  
            // If there are other commands, call both blockClientIfRequired and handleCommandReplication for each of the command.
            for (int i = 0;  i < c->mstate->count;  i++) {
                advanceMultiClientToCommand(c, i);
                simulateUnblockedWrite(c);
                
                // Replicate MULTI if this is the first instruction inside MULTI/EXEC
                if (i == 0) {
                    robj *argv[1];
                    argv[0] = createStringObjectFromSds(sdsnew("multi"));
                    bgIteration_handleCommandReplication(c->db->id, lookupCommandByCString("multi"), 1, argv);
                    decrRefCount(argv[0]);
                }
                bgIteration_handleCommandReplication(c->db->id, c->cmd, c->argc, c->argv);
            }

            // Call handleCommandReplication for EXEC
            robj *argv[1];
            argv[0] = createStringObjectFromSds(sdsnew("EXEC"));
            bgIteration_handleCommandReplication(c->db->id, lookupCommandByCString("exec"), 1, argv);
            server.in_exec = 0;
            decrRefCount(argv[0]);
        }


        // Simulate the expiration (active expiration) of a key.  This is independent of command execution.
        void simulateExpiration(int itemNum) {
            ASSERT_NE(getItem(itemNum), nullptr); // Should be there before expire

            // NOTE: This seems weird, but Valkey propagates the delete before actually expiring the
            //       key.  BgIterator expects this behavior and expects the key to exist when the
            //       DEL is received for propagation.

            // Send bgIteration the DEL
            int db = getDbFromItemNum(itemNum);
            sds sdsKey = sdsnew(keyStr(itemNum));
            robj *argv[2];
            argv[0] = createStringObjectFromSds(sdsnew("DEL"));
            argv[1] = createStringObjectFromSds(sdsdup(sdsKey));
            serverCommand *cmd = lookupCommandByCString("DEL");
            bgIteration_handleCommandReplication(db, cmd, 2, argv);
            decrRefCount(argv[0]);
            decrRefCount(argv[1]);

            bgIteration_keyDelete(db, sdsKey);
            simpleDelItem(itemNum);     // Simulate the actual del

            EXPECT_EQ(getItem(itemNum), nullptr);
            sdsfree(sdsKey);
        }


        // Simulates an expiration, but validates behavior for an item inuse by bgIteration.
        void simulateExpirationOfInuse(int itemNum) {
            // An inuse item will have a refcount > 1.  BgIteration should have incremented the
            //  refcount while it is inuse.
            dbEntry *de = getItem(itemNum);
            ASSERT_NE(de, nullptr); // Should be there before expire
            EXPECT_TRUE(bgIteration_isEntryInuse(de));
            EXPECT_EQ(de->refcount, 2u);

            simulateExpiration(itemNum);

            // At this point, the item is removed from the DB, but still exists, and the refcount
            //  has been reduced to 1.  This allows a background thread to continue using the item.
            EXPECT_EQ(de->refcount, 1u);
        }


        // Simulates an expiration, but the item is a future item which will be expedited.
        void simulateExpirationWithExpedite(int itemNum) {
            // An inuse item will have a refcount > 1.  BgIteration should have incremented the
            //  refcount while it is inuse.
            dbEntry *de = getItem(itemNum);
            ASSERT_NE(de, nullptr); // Should be there before expire
            EXPECT_FALSE(bgIteration_isEntryInuse(de)); // Not yet inuse
            EXPECT_EQ(de->refcount, 1u);

            simulateExpiration(itemNum);

            // At this point, the item is removed from the DB, but still exists, and the refcount
            //  has been reduced to 1.  This allows a background thread to continue using the item.
            EXPECT_TRUE(bgIteration_isEntryInuse(de)); // It's inuse now
            EXPECT_EQ(getItem(itemNum), nullptr);      // but it's not in the DB anymore
            EXPECT_EQ(de->refcount, 1u);
        }


        // Simulate execution of a SWAPDB command
        void simulateSwapDB(int dbid0, int dbid1) {
            char dbStr[2] = {0};

            client *c = static_cast<client*>(zcalloc(sizeof(client)));

            c->cmd = lookupCommandByCString("swapdb");
            c->db = server.db[0];

            c->argc = 3;
            c->argv = static_cast<robj**>(zcalloc(sizeof(robj*) * c->argc));
            c->argv[0] = createStringObjectFromSds(sdsnew(c->cmd->fullname));
            dbStr[0] = '0' + dbid0;
            c->argv[1] = createStringObjectFromSds(sdsnew(dbStr));
            dbStr[0] = '0' + dbid1;
            c->argv[2] = createStringObjectFromSds(sdsnew(dbStr));

            bool blocked = bgIteration_blockClientIfRequired(c);
            EXPECT_FALSE(blocked);  // SWAPDB should never block

            // The real SWAP does more than this, but this is enough for unit tests
            serverDb *aux = server.db[dbid0];
            server.db[dbid0] = server.db[dbid1];
            server.db[dbid1] = aux;

            bgIteration_handleCommandReplication(0, c->cmd, c->argc, c->argv);

            freeTestClient(c);
        }


        // Simulate execution of a FLUSHDB or FLUSHALL command
        void simulateFlushDB(int db, int anInUseItem) {
            client *c = static_cast<client*>(zcalloc(sizeof(client)));

            if (db == -1) {
                c->cmd = lookupCommandByCString("flushall");
                c->db = server.db[0];
            } else {
                c->cmd = lookupCommandByCString("flushdb");
                c->db = server.db[db];
            }

            c->argc = 1;
            c->argv = static_cast<robj**>(zcalloc(sizeof(robj*) * c->argc));
            c->argv[0] = createStringObjectFromSds(sdsnew(c->cmd->fullname));

            dbEntry *de_in_use = getItem(anInUseItem);
            EXPECT_EQ(de_in_use->refcount, 2u);

            bool blocked = bgIteration_blockClientIfRequired(c);
            EXPECT_FALSE(blocked);  // FLUSHDB should never block

            // The real FLUSH does more than this, but this is enough for unit tests

            // Now flush the items
            for (int d = 0;  d < server.dbnum;  d++) {
                if (db == -1 || db == d) {
                    kvstoreRelease(server.db[d]->keys);
                    server.db[d]->keys = NULL;
                }
            }

            EXPECT_EQ(de_in_use->refcount, 1u);

            // and replicate

            bgIteration_handleCommandReplication(0, c->cmd, c->argc, c->argv);

            freeTestClient(c);
        }
};

using BgIterationDeathTest = BgIterationTest;


TEST_F(BgIterationTest, dbIsOK) {
    // Just run the setup/teardown code to make sure the DB is OK.
}


/////////////////////////////////////////////////////
// Simple Full-scan iterator tests
/////////////////////////////////////////////////////

// A simple full scan that just checks basic flow.
TEST_F(BgIterationTest, createAndCleanup) {
    bgIterator *it = bgIteratorCreateFullScanIter("simple",
            0, NULL, iteratorCleanupFn, PRIVDATA);
    EXPECT_EQ(bgIteratorFind("simple"), it);
    EXPECT_STREQ(bgIteratorName(it), "simple");

    bgIteratorStatus status;
    bgIteratorGetStatus(it, &status);

    EXPECT_EQ(status.dbentries_queued, 0u);
    EXPECT_EQ(status.dbentries_processed, 0u);
    EXPECT_EQ(status.replication_queued, 0u);
    EXPECT_EQ(status.replication_processed, 0u);
    EXPECT_EQ(status.swapdb_queued, 0u);
    EXPECT_EQ(status.swapdb_processed, 0u);
    EXPECT_EQ(status.flushdb_queued, 0u);
    EXPECT_EQ(status.flushdb_processed, 0u);

    EXPECT_EQ(status.queue_length, 0u);
    EXPECT_GT(status.queue_length_target, 0u);

    EXPECT_LT(status.runtime_ms, 5u);
    EXPECT_EQ(status.current_item_ms, 0u);

    expectAnythingCleanup(it);

    EXPECT_EQ(bgIteratorFind("simple"), nullptr);
}


// Close client before reading anything
TEST_F(BgIterationTest, testClientCloseBeforeRead) {
    bgIterator *it = bgIteratorCreateFullScanIter("simple",
            0, NULL, iteratorCleanupFn, PRIVDATA);
    bgIteration_feedIterators();
 
    bgIteratorClose(it); // Immediately close before reading
 
    bgIteration_feedIterators(); // Recognize the closed iterator

    // Check that the cleanup callback was executed properly
    EXPECT_EQ(cleanupCount, 1);
    EXPECT_TRUE(cleanupTerminated);
}


// Test that the full scan hits each item in the expected sequence.
TEST_F(BgIterationTest, orderedIteration) {
    bgIterator *it = bgIteratorCreateFullScanIter("simple",
            0, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKeySequence(it, 0, LAST_ITEM);

    // Quick status check.  At this point, item #9 hasn't been returned yet.
    bgIteratorStatus status;
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentries_queued, static_cast<unsigned int>(TOTAL_ITEMS));
    EXPECT_EQ(status.dbentries_processed, static_cast<unsigned int>(TOTAL_ITEMS) - 1);

    expectReadComplete(it); // Returns item #9, and reads the completion item

    // Check that the cleanup callback was executed properly
    EXPECT_EQ(cleanupCount, 1);
    EXPECT_FALSE(cleanupTerminated);
}


// Test that two simultaneous iterations work properly.
TEST_F(BgIterationTest, twoOrderedIterations) {
    bgIterator *it1 = bgIteratorCreateFullScanIter("simple1",
            0, NULL, iteratorCleanupFn, PRIVDATA);
    bgIterator *it2 = bgIteratorCreateFullScanIter("simple2",
            0, NULL, iteratorCleanupFn, PRIVDATA);
    EXPECT_EQ(bgIteratorFind("simple1"), it1);
    EXPECT_EQ(bgIteratorFind("simple2"), it2);

    int it1Count = 0;
    int it2Count = 0;
    while (it1Count < TOTAL_ITEMS || it2Count < TOTAL_ITEMS) {
        // Randomly read from either iterator
        if ((rand() % 2) == 0) {
            if (it1Count < TOTAL_ITEMS) expectReadKey(it1, it1Count++);
        } else {
            if (it2Count < TOTAL_ITEMS) expectReadKey(it2, it2Count++);
        }
    }

    // Nothing left but to read the final completions
    expectReadComplete(it1);
    EXPECT_EQ(cleanupCount, 1);
    EXPECT_FALSE(cleanupTerminated);
    expectReadComplete(it2);
    EXPECT_EQ(cleanupCount, 2);
    EXPECT_FALSE(cleanupTerminated);
}


/////////////////////////////////////////////////////
// MODIFY A FUTURE ITEM
// The next tests validate the basic pattern when a key, not yet iterated, is modified.
// Each variation of iteration flags is tested.
// Note that these tests execute without cloning (cloning is tested elsewhere).
/////////////////////////////////////////////////////

// Modify a future item, without replication or consistency.
// Our expectation for this case is that the modification should proceed without blocking, the item
//  shouldn't be expedited, and we will see the modified item once the iterator reaches it.
TEST_F(BgIterationTest, modFutureItem_NoReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter", 0, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1 & 2 are queued (they are all in the same bucket).
    // Fake a modification to a later key so that we can see if it gets processed out of order.
    client *c = getWriteClient(6, "xxx");

    // We DONT expect the client to be blocked - not consistent
    simulateUnblockedWriteWithModification(c);

    // Now continue reading, 1, 2, 3, 4, 5
    expectReadKeySequence(it, 1, 5);

    // Let's validate that key 6 shows the new value
    expectReadKey(it, 6, "xxx");

    // Continue...
    expectReadKeySequence(it, 7, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// Modify a future item, without replication but with consistency.  (Like a SAVE operation)
// Our expectation for this case is that the modification SHOULD be blocked, as we have to save the
//  the item in it's state before the modification.  To reduce blocking time, the item should be
//  moved to the head of the queue - there's no replication in this case, so out-of-order processing
//  isn't a concern.
TEST_F(BgIterationTest, modFutureItem_NoReplication_YesConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1 & 2 are queued (they are all in the same bucket).
    // Fake a modification to a later key so that we can see if it gets processed out of order.
    client *c = getWriteClient(6, "xxx");
    // Since this is consistent, we will block the client, disallowing the write.
    simulateBlockedWrite(c);

    // On a consistent iterator, the event is expedited in-front of items already in queue!
    //  Read key 6 out of order.
    expectReadKey(it, 6);

    // Now, when we read key 1, key 6 is released back to Valkey, and the client will be unblocked.
    expectReadKeyWithUnblock(it, 1, 6);
    simulateUnblockedWriteWithModification(c); // Now the write can proceed

    // Continue...
    expectReadKeySequence(it, 2, 5);
    // 6 has already been processed
    expectReadKeySequence(it, 7, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// Modify a future item, with replication but without consistency.  (Like a Threadsave Full Sync operation)
// Our expectation for this case is that the modification should proceed without blocking, as the
//  mode is inconsistent.  We don't expect replication, as we haven't reached the item yet.  We'll
//  see the modified item later.
TEST_F(BgIterationTest, modFutureItem_YesReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1 & 2 are queued (they are all in the same bucket).
    // Fake a modification to a later key so that we can see if it gets processed out of order.
    client *c = getWriteClient(6, "xxx");

    // We DONT expect the client to be blocked - not consistent
    simulateUnblockedWriteWithModification(c);

    // NOTE:  Since we haven't reached this item yet, and consistency is not required, there's no
    //        need to replicate this command.  So everything should wrap up just fine - we will see
    //        the new value when we get to it.

    // Now continue reading, 1, 2, 3, 4, 5
    expectReadKeySequence(it, 1, 5);

    // Let's validate that key 6 shows the new value
    expectReadKey(it, 6, "xxx");

    // Continue...
    expectReadKeySequence(it, 7, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// There's no current use case for CONSISTENT with REPLICATION.  It's included for completeness
//  and to clarify the functionality of the design.  However, if this combination were to be used,
//  it would be invalid in the presence of SWAPDB.
TEST_F(BgIterationDeathTest, modFutureItem_YesReplication_YesConsistent_fail) {
    // Note:  This configuration (CONSISTENT with REPLICATION) is invalid unless in cluster mode.
    //        The issue is that with multiple database supporting SWAPDB creates a problem.  How is it
    //        possible to maintain a CONSISTENT view with a SWAPDB impacting the values seen in the
    //        replication stream?  (Cluster mode doesn't support SWAPDB, so no issue there.)
    EXPECT_DEATH(bgIteratorCreateFullScanIter("iter", BGITERATOR_FLAG_REPLICATION | BGITERATOR_FLAG_CONSISTENT,
            NULL, NULL, NULL), "");
}


/////////////////////////////////////////////////////
// MODIFY A CURRENT ITEM
// The next tests validate the basic pattern when a key, currently in use, is modified.
// Each variation of iteration flags is tested.
// Note that these tests execute without cloning (cloning is tested elsewhere).
/////////////////////////////////////////////////////

// Modify a current item, without replication or consistency.
// Our expectation for this case is that the modification SHOULD be blocked, the item shouldn't
//  be expedited (it's already in use).
TEST_F(BgIterationTest, modCurrentItem_NoReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter", 0, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1 & 2 are queued (they are all in the same bucket).
    client *c = getWriteClient(2, "xxx");

    // Must be blocked since key is queued
    simulateBlockedWrite(c);

    // Now continue reading
    expectReadKey(it, 1);
    expectReadKey(it, 2);
    expectReadKeyWithUnblock(it, 3, 2);
    simulateUnblockedWriteWithModification(c);     // the actual write won't affect anything (past key, no replication)

    // Continue...
    expectReadKeySequence(it, 4, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// Modify a current item, without replication but with consistency.  (Like a SAVE operation)
// Our expectation for this case is that the modification SHOULD be blocked, the item shouldn't
//  be expedited (it's already in use).
TEST_F(BgIterationTest, modCurrentItem_NoReplication_YesConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1 & 2 are queued (they are all in the same bucket).
    client *c = getWriteClient(2, "xxx");

    // Must be blocked since key is queued
    simulateBlockedWrite(c);

    // Now continue reading
    expectReadKey(it, 1);
    expectReadKey(it, 2);
    expectReadKeyWithUnblock(it, 3, 2);
    simulateUnblockedWriteWithModification(c);     // the actual write won't affect anything (past key, no replication)

    // Continue...
    expectReadKeySequence(it, 4, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// Modify a current item, with replication but without consistency.  (Like a Threadsave Full Sync operation)
// Our expectation for this case is that the modification SHOULD be blocked.  After the key is processed,
//  the write will proceed, and the replication will be sent.
TEST_F(BgIterationTest, modCurrentItem_YesReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1 & 2 are queued (they are all in the same bucket).
    client *c = getWriteClient(2, "xxx");

    // Must be blocked since key is queued
    simulateBlockedWrite(c);

    // Now continue reading
    expectReadKey(it, 1);
    expectReadKey(it, 2);
    expectReadKeyWithUnblock(it, 3, 2);
    simulateUnblockedWriteWithModification(c);     // the actual write will cause replication

    expectReadKey(it, 4);  // 4 got put in queue when 3 was read

    expectReadReplication(it, c);

    // Continue...
    expectReadKeySequence(it, 5, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


#ifdef CODE_NOT_READY_YET
TEST_F(BgIterationTestCluster, modCurrentItem_YesReplication_YesConsistent_cluster) {
    // Cluster test.  REPLICATION + CONSISTENT only supported in cluster mode
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION | BGITERATOR_FLAG_CONSISTENT,
            NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read. All other keys are queued.
    client *c = getWriteClient(1, "xxx");

    // Since this is consistent, we will block the client, disallowing the write.
    simulateBlockedWrite(c);

    // Not expedited because item is already in queue
    expectReadKey(it, 1);
    expectReadKeyWithUnblock(it, 2, nullptr, 1);  // reading original/unmodified item
    simulateUnblockedWriteWithModification(c);

    expectReadKey(it, 3);  // 2, 3 & 4 are in the same bucket, so the replication comes after
    expectReadKey(it, 4);
    expectReadReplication(it, c);

     // Continue...
    expectReadComplete(it);
    freeTestClient(c);
}
#endif


/////////////////////////////////////////////////////
// MODIFY A PAST ITEM
// The next tests validate the basic pattern when a key, not yet iterated on, is modified.
// Each variation of iteration flags is tested.
// Note that these tests execute without cloning (cloning is tested elsewhere).
/////////////////////////////////////////////////////

// Modify a past item, without replication or consistency.
// Our expectation for this case is that the modification should proceed without blocking.
//  No replication is generated and keys are processed similar to no modification.
TEST_F(BgIterationTest, modPastItem_NoReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter", 0, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // This read returns key 0 (making it a past item)
    expectReadKey(it, 1);

    // At this point, key 0 is returned.
    client *c = getWriteClient(0, "xxx");
    simulateUnblockedWriteWithModification(c);

    // Continue...
    expectReadKeySequence(it, 2, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// Modify a past item, without replication but with consistency.  (Like a SAVE operation)
// Our expectation for this case is that the modification should proceed without blocking.
//  No replication is generated and keys are processed similar to no modification.
TEST_F(BgIterationTest, modPastItem_NoReplication_YesConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // This read returns key 0 (making it a past item)
    expectReadKey(it, 1);

    // At this point, key 0 is returned.
    client *c = getWriteClient(0, "xxx");
    simulateUnblockedWriteWithModification(c);

    // Continue...
    expectReadKeySequence(it, 2, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// Modify a past item, with replication but without consistency.  (Like a Threadsave Full Sync operation)
// Our expectation for this case is that the modification should proceed without blocking.
//  Replication will be sent.
TEST_F(BgIterationTest, modPastItem_YesReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // This read returns key 0 (making it a past item)
    expectReadKey(it, 1);

    // At this point, key 0 is returned.
    client *c = getWriteClient(0, "xxx");
    simulateUnblockedWriteWithModification(c);

    // Key 2 was already in queue (same bucket as key 1).  The replication will follow.
    expectReadKey(it, 2);
    expectReadReplication(it, c);

    // Continue...
    expectReadKeySequence(it, 3, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


#ifdef CODE_NOT_READY_YET
TEST_F(BgIterationTestCluster, modPastItem_YesReplication_YesConsistent_cluster) {
    // Cluster test.  REPLICATION + CONSISTENT only supported in cluster mode
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION | BGITERATOR_FLAG_CONSISTENT,
            NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // This read returns key 0 (making it a past item)
    expectReadKey(it, 1);

    // At this point, key 0 is returned.
    client *c = getWriteClient(0, "xxx");
    simulateUnblockedWriteWithModification(c);

    // Keys 2, 3, and 4 were already in queue.  The replication will follow.
    expectReadKey(it, 2);
    expectReadKey(it, 3);
    expectReadKey(it, 4);
    expectReadReplication(it, c);

    expectReadComplete(it);
    freeTestClient(c);
}
#endif


/////////////////////////////////////////////////////
// TESTS FOR ITEM CLONING
/////////////////////////////////////////////////////

// In a consistent iteration, verify that a simple string is properly cloned, and that a write can
//  occur without blocking.  Validate the cloned item and metadata.
TEST_F(BgIterationTest, modFutureItem_NoReplication_YesConsistent_CloneExpeditedItem) {
    // Initialize cloning configurations.
    bgIteration_unitTestEnableCloning(50, 100);

    bgIteratorStatus status;
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1 & 2 are queued (they are all in the same bucket).
    // Fake a modification to a later key so that we can see if it gets processed out of order.
    client *c = getWriteClient(6, "xxx");

    // Quick status check.  At this point, no clones exist yet.
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentry_clones_queued, 0u);

    // Since item 6 should be cloned, it will not block the client, allowing the write.
    void *de6_md = cloneMetadata(getItem(6));
    simulateClonedWrite(it, c); // This wouldn't block, and queues the cloned value
    simulateUnblockedWriteWithModification(c); // This modifies the real entry in the de (touching metadata)

    // At this point, one clone is in the queue.
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentry_clones_queued, 1u);

    // On a consistent iterator, the event is expedited in-front of items already in queue!
    //  Read key 6 (which is cloned) out of order.  The value will still match the key.
    expectReadClonedKey(it, 6, de6_md); // Also validates and frees the metadata

    // Quick status check.  At this point, cloned items have not been marked as processed yet.
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentry_clones_processed, 0u);

    // Reading key 1 will release key 6, and the clone will finish processing.
    expectReadKey(it, 1);
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentry_clones_processed, 1u);

    // Now, when we read key 2 should not have an impact on number of processed clones.
    expectReadKey(it, 2);
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentry_clones_processed, 1u);

    // Continue...
    expectReadKeySequence(it, 3, 5);
    // 6 has already been processed
    expectReadKeySequence(it, 7, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// Check that cloning for simple strings is respecting the size limits and pool size.  On a
//  consistent iteration, we expect to block or clone on all future keys.  We validate that we can
//  clone if the item is small enough and the cloning pool has more space left.
TEST_F(BgIterationTest, modFutureItem_NoReplication_YesConsistent_LargeItemOrClonePoolFull) {
    // Initialize cloning configurations to test the clone pool functionality first.
    bgIteration_unitTestEnableCloning(50, 50);

    bgIteratorStatus status;
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1 & 2 are queued (they are all in the same bucket).
    // Fake a modification to a later key so that we can see if it gets processed out of order.
    client *c6 = getWriteClient(6, "xxx");
    client *c7 = getWriteClient(7, "xxx");
    client *c8 = getWriteClient(8, "xxx");

    // Quick status check.  At this point, no clones exist yet.
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentry_clones_queued, 0u);

    // Since item 6 should be cloned, it will not block the client, allowing the write.
    void *de6_md = cloneMetadata(getItem(6));
    simulateClonedWrite(it, c6);
    simulateUnblockedWriteWithModification(c6);

    // At this point, one clone is in the queue.
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentry_clones_queued, 1u);

    // Now that cloning pool is full, item 7 will not be cloned and the client will be blocked.
    simulateBlockedWrite(c7);
    ASSERT_TRUE(bgIteration_isEntryInuse(getItem(7)));

    // There is still only one cloned item in the queue.
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentry_clones_queued, 1u);

    // Now change cloning configurations to test that large items will not be cloned. We adjust
    //  the clone pool size to allow two items, but set the maximum item size to be smaller than
    //  the size of item 8. The clone pool size must be larger than the total size of the existing 
    //  clones plus the maximum item clone size. 
    bgIteration_unitTestEnableCloning(1, 101);

    // This write will pass the clone pool check but fail the item size check, blocking the client.
    simulateBlockedWrite(c8);
    ASSERT_TRUE(bgIteration_isEntryInuse(getItem(8)));

    // On a consistent iterator, the expedited item in-front of items already in queue!
    //  Read key 6 out of order.
    expectReadClonedKey(it, 6, de6_md);

    // Now, when we expect to read key 7, which was expedited, key 6 will be released back to Valkey
    //  and the clone will be deallocated here.
    expectReadKey(it, 7);

    // Now, when we read key 8, which was expedited, key 7 is released back to Valkey, and the client 
    // will be unblocked.
    // (actually, unblock is called after every key [just in case] - but functionally we only care
    //  about this one)
    expectReadKeyWithUnblock(it, 8, 7);
    simulateUnblockedWriteWithModification(c7);

    // Now, when we read key 1, key 8 is released back to Valkey, and the client will be unblocked.
    expectReadKeyWithUnblock(it, 1, 8);
    simulateUnblockedWriteWithModification(c8);

    // Since only one item was cloned, there should be one clone processed
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentry_clones_processed, 1u);

    // Continue...
    expectReadKeySequence(it, 2, 5);
    // 6, 7, and 8 have already been processed
    expectReadKeySequence(it, 9, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c6);
    freeTestClient(c7);
    freeTestClient(c8);
}


/////////////////////////////////////////////////////
// TESTS RELATED TO MODIFICATION OF TWO ITEMS
// When 2 keys are modified, we need to ensure that both keys have been sent before we can send
//  replication.  This means that if replication is present, we may have to block/expedite for
//  future keys, even in the inconsistent scenario.
/////////////////////////////////////////////////////

// Replication enabled, but NOT consistent.  In this case, if ANY of the keys have been iterated,
//  ALL of the keys must be replicated so that the command can be processed properly on the replica.
TEST_F(BgIterationTest, modPastFutureItem_YesReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    // In this test, we need a past and future key IN THE SAME DB (they're used in the same command).
    //  DB1 has lots of buckets.  After reading item 9,
    //    8 will be past, 10 will be in queue, 11-15 will be future.
    expectReadKeySequence(it, 0, 9);

    // We're going to write to key 8 (past) and read from key 12 (future)
    // Even though key 12 is for READ in this command, it must be expedited so that it exists before
    //  the associated replication is sent.
    client *c = getSetGetClient(8, "xxx", 12);
    simulateBlockedWrite(c);

    // Key 12 will be expedited, but not in front of existing items in queue (can only do that for
    //  consistent iterators) - JHB How about cluster mode?

    expectReadKey(it, 10);
    expectReadKey(it, 12); // expedited
    expectReadKeyWithUnblock(it, 11, 12); // 13 is now in queue

    simulateUnblockedWriteWithModification(c);

    // Continue...
    expectReadKey(it, 13);
    expectReadReplication(it, c);

    expectReadKeySequence(it, 14, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// Replication NOT enabled.  A read-only key doesn't need to be expedited, even if other keys have
//  been processed already.  (This should work identically for both consistent/non-consistent.
TEST_F(BgIterationTest, modPastFutureItem_NoReplication_YesConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter1",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // In this test, we need a past and future key IN THE SAME DB (they're used in the same command).
    //  DB1 has lots of buckets.  After reading item 9,
    //    8 will be past, 10 will be in queue, 11-15 will be future.
    expectReadKeySequence(it, 0, 9);

    // We're going to write to key 8 (past) and read from key 12 (future)
    // Since there's no replication, we don't have to worry about expediting 12.  The write will
    //  proceed without blocking.
    client *c = getSetGetClient(8, "xxx", 12);
    simulateUnblockedWriteWithModification(c);

    // Key 12 will not be expedited.  Remaining keys should be received in normal order.
    expectReadKeySequence(it, 10, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}

TEST_F(BgIterationTest, modPastFutureItem_NoReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter2",
            0, NULL, iteratorCleanupFn, PRIVDATA);

    // In this test, we need a past and future key IN THE SAME DB (they're used in the same command).
    //  DB1 has lots of buckets.  After reading item 9,
    //    8 will be past, 10 will be in queue, 11-15 will be future.
    expectReadKeySequence(it, 0, 9);

    // We're going to write to key 8 (past) and read from key 12 (future)
    // Since there's no replication, we don't have to worry about expediting 12.  The write will
    //  proceed without blocking.
    client *c = getSetGetClient(8, "xxx", 12);
    simulateUnblockedWriteWithModification(c);

    // Key 9 will not be expedited.  Remaining keys should be received in normal order.
    expectReadKeySequence(it, 10, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


/////////////////////////////////////////////////////
// TESTS RELATED TO MISSING ITEMS
// Missing items are tricky.  A missing item might be logically located in the past or future, in
//  relation to the current iteration position.  The command may (or may not) create the "missing"
//  key.  Some general considerations:
//    * In a consistent iteration, a missing key didn't exist at the time of consistency, or it was
//      already processed (saved) at the time of the deletion.  If the missing key gets created, we
//      must be sure to skip it if we later iterate over it.
//    * In a non-consistent iteration with replication:
//        * If the key location is already passed, the replication is sent, allowing the key to be
//          created (or not) based on the replication.
//        * If the key location is in the furure, we can allow the command to proceed, without
//          replication.  If the key is created, we will process it when the iterator gets to it.
//
// We expect:
//  no-repl, no-consist:  past items are ignored - future items are processed when iterated
//  no-repl, yes-consist:  past items are ignored - future items are ignored
//  yes-repl, no-consist:  past item skipped, but replicated - future items are created by replication and skipped later
//  yes-repl, yes-consist:  past item skipped, but replicated - future items are processed when iterated
/////////////////////////////////////////////////////

// no-repl, no-consist: creation of PAST item has no impact
TEST_F(BgIterationTest, missingPastItem_NoReplication_NoConsistent) {
    simpleDelItem(0); // Delete the item before iterator creation
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            0, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 1);
    expectReadKey(it, 2);

    client *c = getWriteClient(0, "xxx");
    simulateUnblockedWriteWithModification(c);

    expectReadKeySequence(it, 3, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// no-repl, yes-consist: creation of PAST item has no impact
TEST_F(BgIterationTest, missingPastItem_NoReplication_YesConsistent) {
    simpleDelItem(0); // Delete the item before iterator creation
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 1);
    expectReadKey(it, 2);

    client *c = getWriteClient(0, "xxx");
    simulateUnblockedWriteWithModification(c);

    expectReadKeySequence(it, 3, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// yes-repl, no-consist: creation of a PAST item will be replicated
TEST_F(BgIterationTest, missingPastItem_YesReplication_NoConsistent) {
    simpleDelItem(0); // Delete the item before iterator creation
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 1);
    expectReadKey(it, 2);
    expectReadKey(it, 3);

    client *c = getWriteClient(0, "xxx");
    simulateUnblockedWriteWithModification(c); // replication will be added after item 4 (3,4 in same bucket)

    expectReadKey(it, 4);

    expectReadReplication(it, c);

    expectReadKeySequence(it, 5, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


#ifdef CODE_NOT_READY_YET
// yes-repl, yes-consist: creation of a PAST item will be replicated
TEST_F(BgIterationTestCluster, missingPastItem_YesReplication_YesConsistent) {
    // Cluster test.  REPLICATION + CONSISTENT only supported in cluster mode
    simpleDelItem(0); // Delete the item before iterator creation
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION | BGITERATOR_FLAG_CONSISTENT,
            NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 1);
    expectReadKey(it, 2);

    client *c = getWriteClient(0, "xxx");
    simulateUnblockedWriteWithModification(c);     // replication will be added after item 4 (2, 3, and 4 in same bucket)

    expectReadKey(it, 3);
    expectReadKey(it, 4);
    expectReadReplication(it, c);

    expectReadComplete(it);
    freeTestClient(c);
}
#endif


// no-repl, no-consist: creation of FUTURE item is seen when reached by the iteration.
TEST_F(BgIterationTest, missingFutureItem_NoReplication_NoConsistent) {
    // Using DB1 so we have lots of buckets
    // Note:  Choosing item 14 because it's in the portion of DB1 that's already rehashed.  So we
    //  know that the item won't be moving when we re-add it.
    simpleDelItem(14); // Delete the item before iterator creation
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            0, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);

    const char * newValue = "xxx";
    client *c = getWriteClient(14, newValue);
    simulateUnblockedWriteWithModification(c);

    expectReadKeySequence(it, 1, 13);

    // We expect to see item 14.
    //  Note that for an inconsistent DB view, it is logically undefined if this value is seen (or not).
    //  But as implemented, we should see it and the test is helpful to understand if/when the
    //  functionality changes.
    expectReadKey(it, 14, newValue);

    expectReadKey(it, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// no-repl, yes-consist: creation of FUTURE item is ignored by consistent iteration.
TEST_F(BgIterationTest, missingFutureItem_NoReplication_YesConsistent) {
    // Using DB1 so we have lots of buckets
    // Note:  Choosing item 14 because it's in the portion of DB1 that's already rehashed.  So we
    //  know that the item won't be moving when we re-add it.
    simpleDelItem(14); // Delete the item before iterator creation
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);

    client *c = getWriteClient(14, "xxx");
    simulateUnblockedWriteWithModification(c);

    expectReadKeySequence(it, 1, 13);
    // Key 14 is missing - it didn't exist at start of consistent iteration
    expectReadKey(it, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// yes-repl, no-consist: creation of FUTURE item is handled by the replication, and then the key is
//  later skipped (treated like an early iteration case).
TEST_F(BgIterationTest, missingFutureItem_YesReplication_NoConsistent) {
    // Using DB1 so we have lots of buckets
    // Note:  Choosing item 14 because it's in the portion of DB1 that's already rehashed.  So we
    //  know that the item won't be moving when we re-add it.
    simpleDelItem(14); // Delete the item before iterator creation
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0); // Items 1 & 2 are in queue (same bucket)

    client *c = getWriteClient(14, "xxx");
    simulateUnblockedWriteWithModification(c);

    expectReadKeySequence(it, 1, 2);

    expectReadReplication(it, c); // Here's the replication creating item 14

    expectReadKeySequence(it, 3, 13);
    // We expect item 14 to be skipped, because it was created by the earlier replication
    expectReadKey(it, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}

#ifdef CODE_NOT_READY_YET
TEST_F(BgIterationTestCluster, missingFutureItem_YesReplication_YesConsistent) {
    // Cluster test.  REPLICATION + CONSISTENT only supported in cluster mode
    simpleDelItem(4);
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION | BGITERATOR_FLAG_CONSISTENT,
            NULL, iteratorCleanupFn, PRIVDATA);

    bgIteration_feedIterators();    // Make sure we get key 0 and 1 into the queue

    client *c = getWriteClient(4, "xxx");
    simulateUnblockedWriteWithModification(c);

    expectReadKey(it, 0);
    expectReadKey(it, 1);

    expectReadReplication(it, c);

    expectReadKey(it, 2);
    expectReadKey(it, 3);

    // The replication was read - we don't want to see the key now - #4 should be skipped

    expectReadComplete(it);
    freeTestClient(c);
}
#endif


/////////////////////////////////////////////////////
// TESTS RELATED TO EXPIRATION
// Expiration can be tricky.  When pre-evaluating a command with bgIteration_blockClientIfRequired,
//  a key might exist, but be ready for expiration.  Then, as the command executes, the key expires
//  and gets deleted before the write operation.  Consider SET K V.
//  In the unexpired case, this appears to bgIteration as a single SET command (which replaces the value).
//  In the expired case, bgIteration will receive a DEL followed by a SET.
//
// Another case is a READ command.  A read command won't cause the client to be blocked.  However,
//  if the key is expired, this will cause a DEL.  For consistent processing, this key might need to
//  be expedited so that it can be processed before it gets deleted.  In this case, the key is
//  unlinked from the main Valkey dictionary, but the actual deletion is deferred.
/////////////////////////////////////////////////////

TEST_F(BgIterationTest, expireKeys_NoReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            0, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);
    expectReadKey(it, 1);

    // At this point, key 1 is active, key 2 is in queue.

    simulateExpiration(0);        // Past - we no longer care
    simulateExpirationOfInuse(2); // Current - it's inuse
    simulateExpiration(5);        // Future - we don't care (non-consistent)

    expectReadKeySequence(it, 2, 4);
    // key 5 has been deleted
    expectReadKeySequence(it, 6, LAST_ITEM);
    expectReadComplete(it);
}


TEST_F(BgIterationTest, expireKeys_Replication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);
    expectReadKey(it, 1);

    // At this point, key 1 is active, key 2 is in queue.

    simulateExpiration(0);        // Past - we expect replication
    simulateExpirationOfInuse(2); // Current - it's inuse, but we expect replication
    simulateExpiration(5);        // Future - we don't care (non-consistent)

    expectReadKey(it, 2); // this was already queued
    expectReadReplicationDel(it, 0); // Past item should replicate
    expectReadReplicationDel(it, 2); // Current item should replicate
    // Item 5 is a future item and doesn't need to replicate

    expectReadKeySequence(it, 3, 4);
    // Item 5 has been deleted
    expectReadKeySequence(it, 6, LAST_ITEM);
    expectReadComplete(it);
}


TEST_F(BgIterationTest, expireKeys_NoReplication_YesConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);
    expectReadKey(it, 1);

    // At this point, key 1 is active, key 2 is in queue.

    simulateExpiration(0);             // Past - we no longer care
    simulateExpirationOfInuse(2);      // Current - we must defer
    simulateExpirationWithExpedite(5); // Future - will become inuse and expedited for consistency

    expectReadKey(it, 5);  // Expedited to front

    expectReadKeySequence(it, 2, 4);
    // Item 5 has been deleted
    expectReadKeySequence(it, 6, LAST_ITEM);
    expectReadComplete(it);
}


// Special case during a non-consistent iteration with replication and expiration.
//  1. A future key is created (and processed by its replication) - considered early iterated
//  2. Later the key is expired and deleted during command processing (causes DEL to be sent) - no longer early iterated
//  3. The key is recreated as part of the command processing (and this command was replicated) - again early iterated
//  4. Finally, when we iterate to the key, it shouldn't be sent, because it was replicated in step 3.
TEST_F(BgIterationTest, expireKeys_Replication_NoConsistent_FutureKeyCreatedThenExipredDuringSet) {
    simpleDelItem(8);   // Start with a missing future item
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);  // Get the iterator started

    client *c = getWriteClient(8, "xxx");
    simulateUnblockedWriteWithModification(c); // Not blocked because this is a future key (but we expect repl)

    // Now do it again, but break out the steps so that we can simulate an expiration
    bool blocked = bgIteration_blockClientIfRequired(c);
    EXPECT_FALSE(blocked); // Shouldn't be blocked because this is a future key

    // Now, as the SET command tries to execute, simulate that the key is expired.  Expiration
    //  processing sends the replication FIRST!
    robj *argv[2];
    argv[0] = createStringObjectFromSds(sdsnew("DEL"));
    argv[1] = c->argv[1];
    serverCommand *cmd = lookupCommandByCString("DEL");
    bgIteration_handleCommandReplication(getDbFromItemNum(8), cmd, 2, argv);
    decrRefCount(argv[0]);

    // Now the call to keyDelete happens (after the replication).
    bgIteration_keyDelete(getDbFromItemNum(8), static_cast<sds>(objectGetVal(c->argv[1])));
    simpleDelItem(8);     // Simulate the actual del

    // Now the SET will run, re-creating the item (which is still a future item)
    // We need to duplicate the value because setKey() can reallocate it.
    robj *value = dupStringObject(c->argv[2]);
    setKey(c, c->db, c->argv[1], &(value), SETKEY_ADD_OR_UPDATE);

    // Finally, replication will be sent because this is creating a new key
    bgIteration_handleCommandReplication(getDbFromItemNum(8), c->cmd, c->argc, c->argv);

    // Test that everything comes as expected
    expectReadKeySequence(it, 1, 2);    // All one bucket - queued after key 0 read

    expectReadReplication(it, c);       // Repl from the first SET command
    expectReadReplicationDel(it, 8);    // This is the expected replication of the DEL from expire
    expectReadReplication(it, c);       // Repl from the second SET command (recreating deleted key)

    expectReadKeySequence(it, 3, 7);    // continue with normal iteration
    // KEY 8 SHOULD BE OMITTED - This was already replicated
    expectReadKeySequence(it, 9, LAST_ITEM);

    expectReadComplete(it);
}


#ifdef CODE_NOT_READY_YET
/////////////////////////////////////////////////////
// THE REMAINING TESTS ARE GENERAL / UNCATEGORIZED
/////////////////////////////////////////////////////

// Iteration can be terminated from the main thread or from the child client.
//  This tests termination driven from the main thread.
TEST_F(BgIterationTest, earlyTerminationFromMain) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter", 0, NULL, iteratorCleanupFn, PRIVDATA);
    expectReadKey(it, 0);

    // At this point, keys 1 & 2 are in queue.  A termination should release those keys.
    bool blocked1 = true;
    bool blocked2 = true;
    // We expect no general unblocks, we account for each specific unblock below.
    EXPECT_CALL(mock, unblockClientsInUseOnKey(_)).Times(0);
    // We should expect to see unblock called for items 1-4, as they are released from the queue.
    EXPECT_CALL(mock, unblockClientsInUseOnKey(robjEqualsStr(keyStr(1))))
            .WillOnce(Assign(&blocked1, false));
    EXPECT_CALL(mock, unblockClientsInUseOnKey(robjEqualsStr(keyStr(2))))
            .WillOnce(Assign(&blocked2, false));
    bgIteratorTerminate(it);        // queues the items for release
    EXPECT_TRUE(bgIteratorIsTerminating(it));
    bgIteration_feedIterators();    // actually performs the release
    EXPECT_FALSE(blocked1);
    EXPECT_FALSE(blocked2);

    bool blocked0 = true;
    EXPECT_CALL(mock, unblockClientsInUseOnKey(robjEqualsStr(keyStr(0))))
            .WillOnce(Assign(&blocked0, false));
    bgIteratorItem *item = bgIteratorRead(it);
    EXPECT_FALSE(blocked0);
    EXPECT_EQ(item->type, BGITERATOR_ITEM_TERMINATED);

    bgIteratorClose(it);            // background thread completes the termination

    EXPECT_EQ(cleanupCount, 0);
    bgIteration_feedIterators();    // main thread, cleans up iterator and calls cleanup function
    EXPECT_EQ(cleanupCount, 1);
    EXPECT_TRUE(cleanupTerminated);
}


// Iteration can be terminated from the main thread or from the child client.
//  This tests termination driven from the child client (the background thread).
TEST_F(BgIterationTest, earlyTerminationFromChild) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter", 0, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);

    // At this point, keys 1 & 2 are in queue.  A termination should release those keys.
    bgIteratorClose(it);            // background thread initiates the termination
    EXPECT_TRUE(bgIteratorIsTerminating(it));

    bool blocked0 = true;
    bool blocked1 = true;
    bool blocked2 = true;
    // Expecting no extra unblocks
    EXPECT_CALL(mock, unblockClientsInUseOnKey(_)).Times(0);
    // We expect item 0 (the in progress item) to be released
    EXPECT_CALL(mock, unblockClientsInUseOnKey(robjEqualsStr(keyStr(0))))
            .WillOnce(Assign(&blocked0, false));
    // We expect items 1-4 (the queued items) to be released
    EXPECT_CALL(mock, unblockClientsInUseOnKey(robjEqualsStr(keyStr(1))))
            .WillOnce(Assign(&blocked1, false));
    EXPECT_CALL(mock, unblockClientsInUseOnKey(robjEqualsStr(keyStr(2))))
            .WillOnce(Assign(&blocked2, false));
    bgIteration_feedIterators();
    EXPECT_FALSE(blocked0);
    EXPECT_FALSE(blocked1);
    EXPECT_FALSE(blocked2);
    EXPECT_EQ(cleanupCount, 1);
    EXPECT_TRUE(cleanupTerminated);
}


// Edge case.  Executing a command (like SUNIONSTORE) which REPLACES the first key and reads the
//  second key.  In this case, bgIteration will get notified of the key deletion during execution of
//  SETUNIONSTORE.  Given that both keys are in the future (not iterated yet), we'll allow the
//  command to execute, unblocked.  We won't replicate as we'll pick up the key when we get to it.
TEST_F(BgIterationTest, writeWith2Keys_Replication_NoConsistent_keyDeletedDuringSetReplace) {
    // Using DB1 so we have lots of buckets
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKeySequence(it, 0, 8); // 9 is in queue

    // Write command that has 2 keys. 1 existing key that we write to and 1 dependant future key.
    client *c = getWrite2KeysClient("sunionstore", 12, 13);

    simulateUnblockedWrite(c);

    // Now the call to keyDelete happens
    bgIteration_keyDelete(getDbFromItemNum(12), keyStr(12));
    simpleDelItem(12);     // So simulate the actual del

    // Now the write will run, re-creating the item (which is still a future item)
    const char * const newValueStr = "new value";
    robj *newValueRobj = createStringObjectFromSds(sdsnew(newValueStr));
    setKey(c, c->db, c->argv[1], &newValueRobj, SETKEY_ADD_OR_UPDATE);

    // Finally, we are letting bgIteration know that the write command was executed
    bgIteration_handleCommandReplication(getDbFromItemNum(12), c->cmd, c->argc, c->argv);

    // Since the write command was not replicated, we expect all the keys to be read in the normal
    //  order from the dictionary.
    expectReadKeySequence(it, 9, 11);
    expectReadKey(it, 12, newValueStr);
    expectReadKeySequence(it, 13, LAST_ITEM);

    expectReadComplete(it);
    freeTestClient(c);
}


// Edge case.  When we have a new key which is created by a command, AND replication is enabled, we
//  expect that we will replicate the command rather than serializing the key/value later.  As an
//  example, consider SUNIONSTORE A B.  We want to create A by replicating the command.  We don't
//  want to have to process A as a key later on.  But in this case, we can't run the command until
//  B has been sent.  We expect the command to be blocked while we send B.
TEST_F(BgIterationTest, writeWith2Keys_Replication_NoConsistent_setNewKey_DependantFuture) {
    // Using DB1 so we have lots of buckets
    simpleDelItem(12);   // Deleting key 12 to then create it with a write command
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKeySequence(it, 0, 8); // 9 is in queue

    // Write command that has 2 keys. 1 new key and 1 dependant future key.
    client *c = getWrite2KeysClient("sunionstore", 12, 13);

    // We are simulating a new key in the dict. This command should block on the dependant key.
    // This adds key 13 in the queue since the command depends on it.
    simulateBlockedWrite(c);

    // Key 9 was already in the queue
    expectReadKey(it, 9);

    // Key 13 is processed out of order since the write depends on it
    expectReadKey(it, 13);

    // Reading key 10 will unblock key 13, allowing us to write.
    expectReadKey(it, 10);

    // Now that key 13 was processed and released by the iterator, the write command can be executed.
    simulateUnblockedWriteWithModification(c);

    // Key 11 was queued when we read key 10
    expectReadKey(it, 11);

    // The replication of the write command was enqueued after key 11
    expectReadReplication(it, c);

    // We shouldn't see key 12 - as that was processed via replication.
    // We shouldn't see key 13 - as that was expedited earlier

    // Now resuming processing of dict entries
    expectReadKeySequence(it, 14, LAST_ITEM);

    expectReadComplete(it);
    freeTestClient(c);
}


// A new key is being created, but is dependent on another key which has already been processed.
//  In this case, the command shouldn't be blocked.
TEST_F(BgIterationTest, writeWith2Keys_Replication_NoConsistent_setNewKey_DependantPast) {
    // Using DB1 so we have lots of buckets
    simpleDelItem(12);   // Deleting key 12 to then create it with a write command
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKeySequence(it, 0, 9); // 10 is in queue, done with 8

    // Write command that has 2 keys. 1 new key and 1 dependant past key.
    client *c = getWrite2KeysClient("sunionstore", 12, 8);

    // We are simulating a new key in the dict.
    // This command should not block since the dependant key has already been processed.
    simulateUnblockedWriteWithModification(c);

    // Key 10 was put in the queue before the write
    expectReadKey(it, 10);

    expectReadReplication(it, c);

    expectReadKey(it, 11);

    // Key 12 should be missing - it was processed by replication

    expectReadKeySequence(it, 13, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}


// A new key is being created, and has dependencies on 2 other keys - one already processed, one not.
//  In this case, the command should be blocked so that the future key can be sent first.
TEST_F(BgIterationTest, writeWith3Keys_Replication_NoConsistent_setNewKey_1DependantPast1DependantFuture) {
    // Using DB1 so we have lots of buckets
    simpleDelItem(12);   // Deleting key 12 to then create it with a write command
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKeySequence(it, 0, 9); // 8 has been returned, 9 is active, 10 is in queue

    // Write command that has 1 new key and 2 dependencies (past/future)
    client *c = getWrite3KeysClient("sunionstore", 12, 8, 13);

    // The write should be blocked, so that item 13 can be processed.
    simulateBlockedWrite(c);

    expectReadKey(it, 10); // 10 was already in queue
    expectReadKey(it, 13); // 13 was expedited since the write depends on it
    EXPECT_CALL(mock, unblockClientsInUseOnKey(robjEqualsStr(keyStr(13)))).Times(1);
    expectReadKey(it, 11); // Releases 13 so the command can execute

    simulateUnblockedWriteWithModification(c);

    expectReadKey(it, 14); // was queued when reading 11 (12 is missing, 13 was expedited)

    expectReadReplication(it, c);

    expectReadKey(it, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}

// Test an edge case with the same (future) key being repeated in the command, like:
//  SUNIONSTORE A B B
// In this test, A is a previously handled key, and B is a future key.  We expect the future key B to
//  be expedited (once).
TEST_F(BgIterationTest, writeWith3Keys_Replication_NoConsistent_repeatedKey_1DependantPast1RepeatedFuture) {
    // Using DB1 so we have lots of buckets
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKeySequence(it, 0, 9); // We're done with 8, and 10 is in queue

    // Write command that has 3 keys. 1 past key and 1 repeated key in the future.
    client *c = getWrite3KeysClient("sunionstore", 8, 12, 12);

    // This command should block because 12 needs to be expedited.
    simulateBlockedWrite(c);

    expectReadKey(it, 10); // was already in queue
    expectReadKey(it, 12); // expedited
    expectReadKey(it, 11); // releases 12 (unblocking the command)

    // Now that key 12 was processed and released by the iterator, the write command can be executed.
    simulateUnblockedWriteWithModification(c);

    expectReadKey(it, 13); // queued when we read 11

    expectReadReplication(it, c);

    // Now resuming processing of dict entries.
    expectReadKeySequence(it, 14, LAST_ITEM);
    expectReadComplete(it);
    freeTestClient(c);
}
#endif
#ifdef CODE_NOT_READY_YET


TEST_F(BgIterationTest, writeWith3Keys_Replication_NoConsistent_repeatedKey_1newKey1RepeatedFuture) {
    // This tests the replication of a write command that creates a new key and depends on 1 other
    //  key which is repeated in the command. The repeated key is in the future.
    // This test is meant to replicate this bug: https://issues.amazon.com/ELMO-46572

    // Expected sequence of event for this test:
    //  ITEM: (0)'D0' : 'D0'
    //  BLCK?: (0)'sunionstore' 'E0' 'C0' 'C0'
    //  EARLY: (0)'C0' : 'C0'
    //   (blocked)
    //  ITEM: (0)'B0' : 'B0'
    //  ITEM: (0)'A0' : 'A0'
    //  BLCK?: (0)'sunionstore' 'E0' 'C0' 'C0'
    //  REPL?: (0)'sunionstore' 'E0' 'C0' 'C0'
    //   (queued)
    //  SKIPPING ITEM(early iterate): (0)'C0' : 'C0'
    //  ITEM: (1)'E1' : 'E1'
    //  ITEM: (1)'C1' : 'C1'
    //  ITEM: (1)'B1' : 'B1'
    //  ITEM: (1)'A1' : 'A1'
    //  ITEM: (1)'D1' : 'D1'
    //  SENDING COMPLETE
    //  CLEANUP FN (success)

    simpleDelItem(1);   // Deleting key 1 to then create it with a write command
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    // Start with this to load 0 into the queue - but don't read 0 as that would load 1,2,3 into the queue!
    bgIteration_feedIterators();

    // Write command that has 3 keys. 1 new key and 1 repeated key in the future.
    client *c = getWrite3KeysClient(1, 4, 4);

    // This command should block on key 4.
    // This adds key 4 in the queue because:
    // - the command depends on key 4 which hasn't been processed yet
    // - the command depends on a new key (key 1).
    simulateBlockedWrite(c);

    // Key 0 was already enqueued.
    expectReadKey(it, 0);

    // Key 4 is processed out of order since the write depends on it
    expectReadKey(it, 4);

    // Keys 2,3 are next in the queue (they are all in the same bucket).
    // Only reading key 2 for now to release key 4 from the iterator.
    expectReadKey(it, 2);

    // Now that key 4 was processed and released by the iterator, the write command can be executed.
    simulateUnblockedWriteWithModification(c);

    // Key 3 is next in the queue (it was put in the queue at the same time as key 2).
    expectReadKey(it, 3);

    // The replication of the write command was enqueued after keys 1,2,3.
    expectReadReplication(it, c);

    // Now resuming processing of dict entries.
    expectReadKeySequence(it, 5, 9);

    expectReadComplete(it);
    freeTestClient(c);
}


TEST_F(BgIterationTest, writeWith3Keys_NoReplication_Consistent_repeatedKey_1DependantPast1RepeatedFuture) {
    // This tests the replication of a write command that updates multiple keys and depends on a key
    //  which is repeated in the command. The repeated key is in the future and the other key is in
    //  the past.

    // Expected sequence of event for this test:
    //  ITEM: (0)'D0' : 'D0'
    //  BLCK?: (0)'blpop' 'D0' 'C0' 'C0' 'D0'
    //  EARLY_1: (0)'C0' : 'C0'
    //   (blocked)
    //  ITEM: (0)'E0' : 'E0'
    //  ITEM: (0)'B0' : 'B0'
    //  ITEM: (0)'A0' : 'A0'
    //  BLCK?: (0)'blpop' 'D0' 'C0' 'C0' 'D0'
    //  REPL?: (0)'blpop' 'D0' 'C0' 'C0' 'D0'
    //  SKIPPING ITEM(early iterate): (0)'C0' : 'C0'
    //  ITEM: (1)'E1' : 'E1'
    //  ITEM: (1)'C1' : 'C1'
    //  ITEM: (1)'B1' : 'B1'
    //  ITEM: (1)'A1' : 'A1'
    //  ITEM: (1)'D1' : 'D1'
    //  SENDING COMPLETE
    //  CLEANUP FN (success)

    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Start with this to load 0 into the queue - but don't read 0 as that would load 1,2,3 into the queue!
    bgIteration_feedIterators();

    // Write command that has 3 keys. 1 past key and 1 repeated key in the future.
    // How BLPOP works exactly is not relevant to bgIterator, we just chose BLPOP because it's a
    //  multi-key command that (potentially) modifies all of its keys (ie is not CMD_WRITE_FIRSTKEY_ONLY).
    client *c = getWriteMultiKeysClient(0, {4, 4, 0}, "blpop");

    // This command should block on 2 keys (0 and 4), since:
    //  - key 0 is in use by the iterator (still in the queue since it has not been processed by the consumer yet)
    //  - key 4 is in the future
    // This adds key 4 in the queue since the command depends on it and it hasn't been processed yet.
    simulateBlockedWrite(c, 2);

    // Key 4 is processed out of order since the write depends on it.
    // Key 4 is processed before key 0 even though key 0 was already in the queue
    //  because key 4 was enqueued as a priority item.
    expectReadKey(it, 4);

    // Key 0 was already enqueued.
    // Reading key 0 releases key 4 from the iterator.
    expectReadKey(it, 0);

    // Keys 1,2,3 are next in the queue (they are all in the same bucket).
    // Only reading key 1 for now to release key 0 from the iterator.
    expectReadKey(it, 1);

    // Now that keys 4 and 0 were processed and released by the iterator, the write command can be executed.
    simulateUnblockedWriteWithModification(c);

    // Keys 2, 3 are next in the queue (it was put in the queue at the same time as key 1).
    expectReadKeySequence(it, 2, 3);

    // Now resuming processing of dict entries.
    expectReadKeySequence(it, 5, 9);

    expectReadComplete(it);
    freeTestClient(c);
}


TEST_F(BgIterationTest, writeWith3Keys_NoReplication_NoConsistent_repeatedKey_1repeatedNewKey) {
    // This tests a write command that creates a new key where the new key is repeated in the
    //  command. The repeated key is in the future.

    // Expected sequence of event for this test:
    // ITEM: (0)'D0' : 'D0'
    // ITEM: (0)'A0' : 'A0'
    // ITEM: (0)'B0' : 'B0'
    // ITEM: (0)'E0' : 'E0'
    // BLCK?: (0)'blpop' 'C0' 'D0' 'C0' 'D0'
    // REPL?: (0)'blpop' 'C0' 'D0' 'C0' 'D0'
    // ITEM: (0)'C0' : 'D0'
    // ITEM: (1)'B1' : 'B1'
    // ITEM: (1)'C1' : 'C1'
    // ITEM: (1)'D1' : 'D1'
    // ITEM: (1)'A1' : 'A1'
    // ITEM: (1)'E1' : 'E1'
    // SENDING COMPLETE
    //  CLEANUP FN (success)

    server.db[0]->keys->dtype->resizeAllowed = NULL;
    kvstoreExpand(server.db[0]->keys, 32, 0, NULL);
    hashtableRehash(server.db[0]->keys->hashtables[0], 32);

    // The table looks this way now:
    // Table 0, used 5, exp 3, top-level buckets 8, child buckets 0
    // Bucket 0:0 level:0
    //   0 (empty)
    //   1 (empty)
    //   2 (empty)
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)
    // Bucket 0:1 level:0
    //   0 h2 63, key "D0"
    //   1 (empty)
    //   2 (empty)
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)
    // Bucket 0:2 level:0
    //   0 (empty)
    //   1 (empty)
    //   2 (empty)
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)
    // Bucket 0:3 level:0
    //   0 h2 b8, key "A0"
    //   1 h2 f5, key "B0"
    //   2 h2 13, key "E0"
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)
    // Bucket 0:4 level:0
    //   0 (empty)
    //   1 (empty)
    //   2 (empty)
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)
    // Bucket 0:5 level:0
    //   0 (empty)
    //   1 (empty)
    //   2 (empty)
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)
    // Bucket 0:6 level:0
    //   0 h2 91, key "C0"
    //   1 (empty)
    //   2 (empty)
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)
    // Bucket 0:7 level:0
    //   0 (empty)
    //   1 (empty)
    //   2 (empty)
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)

    const char *new_keys[5] = {"D0", "A0", "B0", "E0", "C0"};
    update_keys(new_keys, 0, 5);

    simpleDelItem(4);   // Deleting key 4 to then create it with a write command
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            0, NULL, iteratorCleanupFn, PRIVDATA);

    // Getting started
    // The first bucket is empty
    bgIteration_feedIterators();
    expectReadKey(it, 0);

    // Key 1 is the next in the queue.
    // Reading key 1 to release key 0 from the iterator.
    expectReadKey(it, 1);

    // Write command that has 3 keys. 1 new repeated key and 1 key in the past.
    // How BLPOP works exactly is not relevant to bgIterator, we just chose BLPOP because it's a
    //  multi-key command that (potentially) modifies all of its keys (ie is not CMD_WRITE_FIRSTKEY_ONLY).
    client *c = getWriteMultiKeysClient(4, {0, 4, 0}, "blpop");

    // The write command is not blocked since key 0 is not in use by the iterator
    simulateUnblockedWriteWithModification(c);

    // Keys 2, 3 are next in the queue (it was put in the queue at the same time as key 1).
    expectReadKeySequence(it, 2, 3);

    // Key 4 is now in the dict with the value of key 0.
    expectReadKey(it, 4, keyStr(0));

    // Processing the rest of the dict entries.
    expectReadKeySequence(it, 5, 9);

    expectReadComplete(it);
    freeTestClient(c);
}

TEST_F(BgIterationTest, copyHandlesProperDb_Replication_NoConsistent) {
    // In this test, the COPY command is copying from one DB to another.  We will create the
    //  same key in both DBs.  We make sure that the proper key is created via replication, and
    //  the proper key is created by iteration.

    // NOTE:  Adding E0 to dict 1.  Now there is a E0 in both dict 0 and dict 1.
    addKeyToDb(1, "E0", "E0");

    // The test:
    //  We will simulate (with DB0 selected): COPY D0 C0 DB 1 REPLACE
    //  This will overwrite DB1:C0 that was created above.
    //  Since DB0:D0 is the first iterated key we expect that DB1:C0 will be expedited.
    //  After DB1:C0 is "overwritten", it should be marked early iterate.
    //  We expect DB0:C0 to NOT be marked early iterate, and should get processed normally.

    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    // Start with this to load 0 (C0) into the queue - but don't read 0 as that would load 1,2,3 into the queue!
    bgIteration_feedIterators();

    // COPY C0 E0 DB 1 REPLACE
    client *c = static_cast<client*>(zcalloc(sizeof(client)));
    c->cmd = lookupCommandByCString("copy");
    c->db = server.db[0];
    c->argc = 6;
    c->argv = static_cast<robj**>(zcalloc(sizeof(robj*) * c->argc));
    c->argv[0] = createStringObjectFromSds(sdsnew(c->cmd->fullname));
    c->argv[1] = createStringObjectFromSds(sdsnew("C0"));
    c->argv[2] = createStringObjectFromSds(sdsnew("E0"));
    c->argv[3] = createStringObjectFromSds(sdsnew("DB"));
    c->argv[4] = createStringObjectFromSds(sdsnew("1"));
    c->argv[5] = createStringObjectFromSds(sdsnew("REPLACE"));

    // This should block on 2 keys.  DB0:C0 is in queue.  DB1:E0 needs to be expedited.
    simulateBlockedWrite(c, 2);
    expectReadKey(it, 0);              // DB0:C0
    expectReadDbKeyValue(it, 1, "E0", "E0");    // DB1:E0 is expedited
    expectReadKey(it, 1);              // (to release DB1:E0)
    // Now keys 2 & 3 & 4 are in the queue

    simulateUnblockedWrite(c);  // We shouldn't be blocked this time

    // Now, we'll simulate the actual activity of the COPY.  DB1:C0 will be deleted in order to
    //  be overwritten.
    bgIteration_keyDelete(1, sdsnew("E0"));
    // At this point the key would actually be deleted and recreated by COPY (no need to actually do this)

    // And finally the replication (this should queue replication)
    bgIteration_handleCommandReplication(c->db->id, c->cmd, c->argc, c->argv);

    // Now let's read everything...
    expectReadKeySequence(it, 2, 4);    // These were in queue already
    expectReadReplication(it, c);       // This is the new replication (creating DB1:C0)

    expectReadKeySequence(it, 5, 9);    // These are all normal

    expectReadComplete(it);     // At this point, we should be done.  We should NOT see DB1:C0.
    freeTestClient(c);
}


// Just check that termination with replication in queue works OK.
TEST_F(BgIterationTest, terminateWithReplication) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);
    expectReadKey(it, 1);  // makes sure we are done with key 0 (don't want to block)

    client *c = getWriteClient(0, "xxx");
    simulateUnblockedWriteWithModification(c);     // Should replicate
    freeTestClient(c);

    bgIteratorTerminate(it);

    bgIteratorItem *item = bgIteratorRead(it);
    ASSERT_EQ(item->type, BGITERATOR_ITEM_TERMINATED);

    bgIteratorClose(it);            // background thread completes the termination

    bgIteration_feedIterators();    // main thread, cleans up iterator and calls cleanup function
    EXPECT_EQ(cleanupCount, 1);
    EXPECT_TRUE(cleanupTerminated);
}


// SWAPDB tests - Get ready for the mind-bend...

TEST_F(BgIterationTest, swapDB_NoReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            0, NULL, iteratorCleanupFn, PRIVDATA);
    bgIteratorStatus status;

    // In the non-consistent iterator (without replication), items are identified with the DBID at
    //  the time they are placed into the queue.  The SWAPDB event signals the change to the
    //  iterating process - and this is properly sequenced with the DB info for each item.

    expectReadKey(it, 0);

    // Keys 1,2,3, and 4 are in queue
    simulateSwapDB(0, 1);       // The swap event will be queued after item 3
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.swapdb_queued, 1u);
    EXPECT_EQ(status.swapdb_processed, 0u);

    expectReadKey(it, 1);
    expectReadKey(it, 2);
    expectReadKey(it, 3);
    expectReadKey(it, 4);

    expectReadSwapDB(it, 0, 1);
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.swapdb_queued, 1u);
    EXPECT_EQ(status.swapdb_processed, 0u);     // still processing it...

    // Since we've seen the swap event, items now have the new DBID
    expectReadDbKeyValue(it, 0, keyStr(5), keyStr(5));  // item 5 is in DB0
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.swapdb_queued, 1u);
    EXPECT_EQ(status.swapdb_processed, 1u);     // done processing the swapdb

    // Keys 6 & 7 are in the queue - let's swap back!
    simulateSwapDB(1, 0);       // The swap event will be queued after item 7
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.swapdb_queued, 2u);        // 2nd one queued
    EXPECT_EQ(status.swapdb_processed, 1u);

    expectReadDbKeyValue(it, 0, keyStr(6), keyStr(6));  // Still appears as DB0
    expectReadDbKeyValue(it, 0, keyStr(7), keyStr(7));  // Still appears as DB0

    expectReadSwapDB(it, 1, 0); // Now the iterator knows about the 2nd swap
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.swapdb_queued, 2u);
    EXPECT_EQ(status.swapdb_processed, 1u);     // still processing it...

    expectReadKey(it, 8);
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.swapdb_queued, 2u);
    EXPECT_EQ(status.swapdb_processed, 2u);     // done processing all swaps

    expectReadKey(it, 9);
    expectReadComplete(it);
}

TEST_F(BgIterationTest, swapDB_NoReplication_YesConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // In the consistent iterator (without replication) all items are presented to the iterating
    //  process using the DBID at the time of the iterator creation.  No changes are evident.

    expectReadKey(it, 0);

    // Keys 1,2,3,4 are in queue
    simulateSwapDB(0, 1);       // The swap occurs, but the iterator sees no change

    expectReadKey(it, 1);
    expectReadKey(it, 2);
    expectReadKey(it, 3);
    expectReadKey(it, 4);

    // Heck, let's go crazy with those swaps...
    for (int itemNum = 5;  itemNum <= 9;  itemNum++) {
        simulateSwapDB(0, 1);
        expectReadKey(it, itemNum);
    }

    expectReadComplete(it);
}

TEST_F(BgIterationTest, swapDB_YesReplication_NoConsistent) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    // In the non-consistent iterator WITH replication, items are identified with the DBID at the
    //  time they are placed into the queue.  The SWAPDB event signals the change to the iterating
    //  process - and this is properly sequenced with the DB info for each item.

    expectReadKey(it, 0);

    // Keys 1,2,3,4 are in queue
    simulateSwapDB(0, 1);       // The swap event will be queued after item 3

    expectReadKey(it, 1);
    expectReadKey(it, 2);
    expectReadKey(it, 3);
    expectReadKey(it, 4);

    expectReadSwapDB(it, 0, 1);                         // We should see a SWAPDB event
    bgIteratorItem *item = bgIteratorRead(it);          // followed by the associated replication
    ASSERT_EQ(item->type, BGITERATOR_ITEM_REPLICATION);
    bgIteration_feedIterators();

    // Since we've seen the swap event, items now have the new DBID
    expectReadDbKeyValue(it, 0, keyStr(5), keyStr(5));  // item 5 is in DB0

    // Keys 6 & 7 are in the queue - let's swap back!
    simulateSwapDB(1, 0);       // The swap event will be queued after item 7

    expectReadDbKeyValue(it, 0, keyStr(6), keyStr(6));  // Still appears as DB0
    expectReadDbKeyValue(it, 0, keyStr(7), keyStr(7));  // Still appears as DB0

    expectReadSwapDB(it, 1, 0); // Now the iterator knows about the 2nd swap
    item = bgIteratorRead(it);
    ASSERT_EQ(item->type, BGITERATOR_ITEM_REPLICATION);
    bgIteration_feedIterators();

    expectReadKey(it, 8);
    expectReadKey(it, 9);
    expectReadComplete(it);
}

// There is no test for swapDB_YesReplication_YesConsistent because this configuration is not
//  permitted with multiple DBs (not permitted with swaps).


// FLUSHDB & FLUSHALL Tests
TEST_F(BgIterationTest, flushDB_flushAll) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter", 0, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);
    expectReadKey(it, 1);

    // key 1 is active in the iterator - this key will be removed from the DB before flush.
    // keys 2 & 3 & 4 are in queue - but will be returned to Valkey before the flush.  These are yanked
    //  back by Valkey and will not be seen by iterator.
    simulateFlushDB(-1, 1);

    bgIteratorItem *item = bgIteratorRead(it);
    ASSERT_EQ(item->type, BGITERATOR_ITEM_TERMINATED);

    bgIteratorClose(it);            // background thread completes the termination

    bgIteration_feedIterators();    // main thread, cleans up iterator and calls cleanup function
    EXPECT_EQ(cleanupCount, 1);
    EXPECT_TRUE(cleanupTerminated);
}

TEST_F(BgIterationTest, flushDB_flushOne) {
    bgIterator *it1 = bgIteratorCreateFullScanIter("iter1",
            0, NULL, iteratorCleanupFn, PRIVDATA);
    bgIterator *it2 = bgIteratorCreateFullScanIter("iter2",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);
    bgIteratorStatus status;

    // The test flushes DB0.  This is half the data.  Since <= half, a non-consistent iterator is
    //  allowed to proceed.  But the consistent iterator will be terminated.

    expectReadKey(it1, 0);
    expectReadKey(it2, 0);
    expectReadKey(it1, 1);
    expectReadKey(it2, 1);

    // key 1 is active in the iterator - this key will be removed from the DB before flush.
    // keys 2 & 3 & 4 are in queue - but will be returned to Valkey before the flush.  These are yanked
    //  back by Valkey and will not be seen by iterator.
    simulateFlushDB(0, 1);
    bgIteratorGetStatus(it1, &status);
    EXPECT_EQ(status.flushdb_queued, 1u);
    EXPECT_EQ(status.flushdb_processed, 0u);

    // Testing the non-consistent one continues...
    // Everything already on the iterator queue should be preserved (deleted from the DB).
    //  Keys 2 & 3 & 4 are already queued (and preserved).
    expectReadKey(it1, 2);
    expectReadKey(it1, 3);
    expectReadKey(it1, 4);

    bgIteratorItem *item = bgIteratorRead(it1);
    ASSERT_EQ(item->type, BGITERATOR_ITEM_FLUSHDB);
    ASSERT_EQ(item->dbid, 0);
    bgIteratorGetStatus(it1, &status);
    EXPECT_EQ(status.flushdb_queued, 1u);
    EXPECT_EQ(status.flushdb_processed, 0u);    // still processing it

    expectReadKey(it1, 5);
    bgIteratorGetStatus(it1, &status);
    EXPECT_EQ(status.flushdb_queued, 1u);
    EXPECT_EQ(status.flushdb_processed, 1u);    // done with all flushdb's
    expectReadKey(it1, 6);
    expectReadKey(it1, 7);
    expectReadKey(it1, 8);
    expectReadKey(it1, 9);
    expectReadComplete(it1);
    EXPECT_EQ(cleanupCount, 1);
    EXPECT_FALSE(cleanupTerminated);

    // But the consistent iterator should be terminated
    item = bgIteratorRead(it2);
    ASSERT_EQ(item->type, BGITERATOR_ITEM_TERMINATED);
    bgIteratorClose(it2);           // background thread completes the termination
    bgIteration_feedIterators();    // main thread, cleans up iterator and calls cleanup function
    EXPECT_EQ(cleanupCount, 2);
    EXPECT_TRUE(cleanupTerminated);
}

// Cluster mode, 2 iterators, CONSISTENT+REPLICATION and NONCONSISTENT+REPLICATION
//  Modify a missing key.
TEST_F(BgIterationTestCluster, modMissingKey_2iter_cluster) {
    // Cluster test.  REPLICATION + CONSISTENT only supported in cluster mode
    // For this test, we only have 5 keys since not using DB[1].  Remove the last one.
    simpleDelItem(4);

    bgIterator *it1 = bgIteratorCreateFullScanIter("iter1",
            BGITERATOR_FLAG_REPLICATION | BGITERATOR_FLAG_CONSISTENT,
            NULL, iteratorCleanupFn, PRIVDATA);
    bgIterator *it2 = bgIteratorCreateFullScanIter("iter2",
            BGITERATOR_FLAG_REPLICATION,
            NULL, iteratorCleanupFn, PRIVDATA);

    client *c = getWriteClient(4, "xxx");
    simulateUnblockedWriteWithModification(c);     // Wouldn't be blocked since key doesn't exist

    bgIteration_feedIterators();    // Prime the feed - key 0 and 1 are now enqueued

    // Process the consistent iteration
    expectReadReplication(it1, c);  // replication happened before feeding (should be 1st)
    expectReadKeySequence(it1, 0, 3);
    expectReadComplete(it1);

    // Process the non-consistent iteration
    expectReadReplication(it2, c);  // replication happened before feeding (should be 1st)
    expectReadKeySequence(it2, 0, 3);
    expectReadComplete(it2);

    freeTestClient(c);
}

TEST_F(BgIterationTest, twoKeys_firstFuture) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION,
            NULL, iteratorCleanupFn, PRIVDATA);

    bgIteration_feedIterators();    // Prime the feed - key 0
    expectReadKey(it, 0);  // Causes keys 1, 2, 3, 4 to be queued (same bucket)
    expectReadKey(it, 1);  // Causes key 0 to be released

    // This must replicate, because A0 is in the past.  B1 (future) wouldn't need replication except
    //  for the modification to B1.  We try to trip up bgIterator by giving a key that doesn't need
    //  replication except for the later command that does.  Make this a little trickier by adding
    //  the set for A1 - unnecessary, but more clearly shows the expediting in progress.
    client *c = getMultiClient("SET A0 xxx; SELECT 1; SET A1 xxx; SET B1 xxx", 1);

    // The EXEC should block on 2 keys, because B1(5) & A1(8) should be expedited
    simulateBlockedWrite(c, 2);

    expectReadKeySequence(it, 2, 4);    // These were already in queue

    // Note - it would be OK if these 2 were reversed, but this is how the current algorithm works.
    expectReadKey(it, 8);  // Key 8 (A1) was expedited
    expectReadKey(it, 5);  // Key 5 (B1) was expedited

    // and clean up the rest...
    expectReadKeySequence(it, 6, 7);
    // Key 8 was already read above (expedited)
    expectReadKey(it, 9);
    expectReadComplete(it);
}

TEST_F(BgIterationTest, multiBlocksOnFutureKey) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1,2,3,4 are queued (they are all in the same bucket).
    // If we fake a modification to key 5, we won't know if it's handled out of order.
    // So we fake a modification to key 6
    // Dummy up a MULTI...
    client *c = getMultiClient("SET C1 xxx", 1);

    // Since this is consistent, we will block the client, disallowing the write.
    simulateBlockedWrite(c);
    freeTestClient(c);

    // C1 (key 6) will be expedited to the front of the list
    expectReadKey(it, 6);

    // Now that we've read key 5, key 0 (C0) is passed and should not block
    client *c2 = getMultiClient("SET C0 xxx");
    simulateUnblockedWrite(c2);
    freeTestClient(c2);


    expectReadKeySequence(it, 1, 5);
    expectReadKeySequence(it, 7, 9);
    expectReadComplete(it);
}

TEST_F(BgIterationTest, multiNotReplicatedButDelRecreateAccess) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    // Scenario.  We have a multi that doesn't need to be replicated because all of the keys exist
    //  but are all future keys.  Note that missing keys are considered already-iterated, so all
    //  must exist for this test.  Then:
    //   - we delete a key
    //   - we re-create the deleted (future) key - normally this would be replicated
    //   - we access another (future) key - we don't expect to get blocked!

    // We use DB 1 only because the hash table buckets are better broken up there.
    client *c = getMultiClient("DEL A1; SET A1 xxx; SET E1 yyy", 1);

    // For DB[1]:
    // Bucket 0:0 level:0
    //   0 h2 18, key "B1"
    //   1 h2 fd, key "C1"
    //   2 h2 e9, key "D1"
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)
    // Bucket 0:1 level:0
    //   0 h2 36, key "A1"
    //   1 h2 0c, key "E1"
    //   2 (empty)
    //   3 (empty)
    //   4 (empty)
    //   5 (empty)
    //   6 (empty)

    // Read through DB 0 and into DB 1
    expectReadKeySequence(it, 0, 5);    // D0, E0, B0, A0, C0, B1
    // Now, C1 and D1 are in the queue (in use) and A1 & E1 are future

    // Now let's process the multi.  Since A1 & D1 are both future (existing) items, we shouldn't
    //  block or replicate.
    simulateUnblockedWrite(c);  // the EXEC

    // Simulate the DEL A1
    server.in_exec = 1;    // Simulate actual execution of the MULTI/EXEC
    advanceMultiClientToCommand(c, 0);  // DEL A1
    EXPECT_CALL(mock, blockClientInUseOnKeys(c,_,_)).Times(0);
    bool blocked = bgIteration_blockClientIfRequired(c);
    EXPECT_FALSE(blocked);
    simpleDelItem(8);
    sds delKey = sdsnew(keyStr(8));
    bgIteration_keyDelete(1, delKey);
    sdsfree(delKey);
    bgIteration_handleCommandReplication(c->db->id, c->cmd, c->argc, c->argv); // shouldn't replicate

    // Simulate SET A1 - the key doesn't exist, and would normally replicate and mark early iterate,
    //  but this is in a transaction, and we are not replicating this transaction.
    advanceMultiClientToCommand(c, 1);  // SET A1 xxx
    simulateUnblockedWriteWithModification(c);

    // Now write to another existing future key - this should work if we weren't confused by the DEL
    advanceMultiClientToCommand(c, 2);  // SET E1 yyy
    simulateUnblockedWriteWithModification(c);
    server.in_exec = 0;

    // Now we can continue iterating, and we should pick up keys 6-9.  (and no replication!)
    expectReadKeySequence(it, 6, 7);
    expectReadKey(it, 8, "xxx");
    expectReadKey(it, 9, "yyy");
    expectReadComplete(it);
}

TEST_F(BgIterationTest, multiHandlesSelectProperly) {
    // For this test, C0 is added into DB1 - so it exists in both DB 0 and 1.  We will process it
    //  in DB0, but it will be unprocessed in DB1.  See if we track select properly.
    addKeyToDb(1, "C0", "C0");

    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - C0 in DB 0.
    expectReadKey(it, 0);

    // Now, we are done with C0 in DB0, but not in DB1
    expectReadKey(it, 1);

    // These cases should NOT block...  (they access C0 in DB0)
    client *c;
    c = getMultiClient("SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SELECT 0; SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SET C0 xxx; SELECT 1");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SELECT 1; SELECT 0; SET C0 xxx; SELECT 1");
    simulateUnblockedWrite(c);
    freeTestClient(c);

    // These cases SHOULD block...  (they access C0 in DB1)
    c = getMultiClient("SET C0 xxx");
    c->db = server.db[1];
    simulateBlockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SELECT 1; SET C0 xxx");
    simulateBlockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SELECT 1; SET C0 xxx; SELECT 0");
    simulateBlockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SELECT 0; SELECT 1; SET C0 xxx; SELECT 1");
    simulateBlockedWrite(c);
    freeTestClient(c);

    expectAnythingCleanup(it);
}


TEST_F(BgIterationTest, multiHandlesSelectNoPermissionProperly) {
    // For this test, C0 is added into DB1 - so it exists in both DB 0 and 1.  We will process it
    //  in DB0, but it will be unprocessed in DB1.  See if we track select properly.
    addKeyToDb(1, "C0", "C0");

    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - C0 in DB 0.
    expectReadKey(it, 0);

    // Now, we are done with DC00 in DB0, but not in DB1
    expectReadKey(it, 1);

    // No permission for any commands (specifically select/swapdb)
    EXPECT_CALL(mock, amzCanClientExecuteCommand(_,_,_,_))
        .Times(AtLeast(1)).WillRepeatedly(Return(false));

    // These cases should NOT block...  (they access C0 in DB0)
    //  The SELECTs below are inconsequential - with/without select, same result.
    client *c;
    c = getMultiClient("SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SELECT 0; SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SET C0 xxx; SELECT 1");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SELECT 1; SELECT 0; SET C0 xxx; SELECT 1");
    simulateUnblockedWrite(c);
    freeTestClient(c);

    // These cases SHOULD block IF SELECT IS WORKING...  (they access C0 in DB1)
    c = getMultiClient("SET C0 xxx");
    c->db = server.db[1];      // already starting on DB1
    simulateBlockedWrite(c);    // will block, no select
    freeTestClient(c);
    c = getMultiClient("SELECT 1; SET C0 xxx");
    simulateUnblockedWrite(c);  // will not block because accessing DB0 (select fails)
    freeTestClient(c);
    c = getMultiClient("SELECT 1; SET C0 xxx; SELECT 0");
    simulateUnblockedWrite(c);  // will not block because accessing DB0 (select fails)
    freeTestClient(c);
    c = getMultiClient("SELECT 0; SELECT 1; SET C0 xxx; SELECT 1");
    simulateUnblockedWrite(c);  // will not block because accessing DB0 (select fails)
    freeTestClient(c);

    expectAnythingCleanup(it);
}


TEST_F(BgIterationTest, multiHandlesSwapdbProperly) {
    // For this test, C0 is added into DB1 - so it exists in both DB 0 and 1.  We will process it
    //  in DB0, but it will be unprocessed in DB1.  See if we track select properly.
    addKeyToDb(1, "C0", "C0");

    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - C0 in DB 0.
    expectReadKey(it, 0);

    // Now, we are done with C0 in DB0, but not in DB1
    expectReadKey(it, 1);

    // These cases should NOT block...  (they access C0 in DB0)
    client *c;
    c = getMultiClient("SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SET C0 xxx; SWAPDB 0 1");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SET C0 xxx; SWAPDB 0 1; SWAPDB 0 1; SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SWAPDB 0 1; SELECT 1; SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);

    // These cases SHOULD block...  (they access C0 in DB1)
    c = getMultiClient("SET C0 xxx");
    c->db = server.db[1];
    simulateBlockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SWAPDB 1 0; SET C0 xxx; SWAPDB 0 1");
    simulateBlockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SWAPDB 1 0; SELECT 0; SET C0 xxx; SWAPDB 0 1");
    simulateBlockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SWAPDB 1 0; SWAPDB 1 0; SELECT 1; SET C0 xxx; SELECT 1");
    simulateBlockedWrite(c);
    freeTestClient(c);

    expectAnythingCleanup(it);
}


TEST_F(BgIterationTest, multiHandlesSwapdbNoPermissionProperly) {
    // For this test, C0 is added into DB1 - so it exists in both DB 0 and 1.  We will process it
    //  in DB0, but it will be unprocessed in DB1.  See if we track select properly.
    addKeyToDb(1, "C0", "C0");

    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - C0 in DB 0.
    expectReadKey(it, 0);

    // Now, we are done with C0 in DB0, but not in DB1
    expectReadKey(it, 1);

    // No permission for any commands (specifically select/swapdb)
    EXPECT_CALL(mock, amzCanClientExecuteCommand(_,_,_,_))
        .Times(AtLeast(1)).WillRepeatedly(Return(false));

    // These cases should NOT block...  (they access C0 in DB0)
    //  The SELECTs & SWAPDBs below are inconsequential - with/without select/swapdb, same result.
    client *c;
    c = getMultiClient("SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SET C0 xxx; SWAPDB 0 1");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SET C0 xxx; SWAPDB 0 1; SWAPDB 0 1; SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SWAPDB 0 1; SELECT 1; SET C0 xxx");
    simulateUnblockedWrite(c);
    freeTestClient(c);

    // These cases SHOULD block IF SELECT/SWAPDB IS WORKING...  (they access C0 in DB1)
    c = getMultiClient("SET C0 xxx");
    c->db = server.db[1];
    simulateBlockedWrite(c);
    freeTestClient(c);
    c = getMultiClient("SWAPDB 1 0; SET C0 xxx; SWAPDB 0 1");
    simulateUnblockedWrite(c);  // will not block because accessing DB0 (swapdb fails)
    freeTestClient(c);
    c = getMultiClient("SWAPDB 1 0; SELECT 0; SET C0 xxx; SWAPDB 0 1");
    simulateUnblockedWrite(c);  // will not block because accessing DB0 (swapdb/select fails)
    freeTestClient(c);
    c = getMultiClient("SWAPDB 1 0; SWAPDB 1 0; SELECT 1; SET C0 xxx; SELECT 1");
    simulateUnblockedWrite(c);  // will not block because accessing DB0 (swapdb/select fails)
    freeTestClient(c);

    expectAnythingCleanup(it);
}

void * pthreadWait200msAndReadTwoKeys(void *arg) {
    bgIterator *it = static_cast<bgIterator*>(arg);

    usleep(200000);
    bgIteratorRead(it);
    bgIteratorRead(it);
    return nullptr;
}

void asyncWait200msAndReadTwoKeys(bgIterator *it) {
    int rc;
    pthread_attr_t attr;
    pthread_t thread;

    rc = pthread_attr_init(&attr);
    assert(rc == 0);
    rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    assert(rc == 0);

    rc = pthread_create(&thread, &attr, pthreadWait200msAndReadTwoKeys, it);
    assert(rc == 0);

    rc = pthread_attr_destroy(&attr);
    assert(rc == 0);
}


TEST_F(BgIterationTest, testLuaWithUndeclaredKey) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_CONSISTENT, NULL, iteratorCleanupFn, PRIVDATA);

    // Read the 1st key - let's get the party started
    expectReadKey(it, 0);

    // At this point, key 0 is read.  Keys 1,2,3 are queued (they are all in the same bucket).
    // If we fake a modification to key 4, we won't know if it's handled out of order.
    // So we fake a modification to key 5
    client *c = getWriteClient(5, "xxx");
    c->flag.script = 1;

    // Now for a LUA script, we have already blocked (on the eval/evalsha) for any declared keys
    //  But here, we're about to modify an undeclared key.  We can't actually block in the middle
    //  of the LUA script.  So this will behave as unblocked, but incur a synchronous wait.

    // Key 5 will get expedited when we simulate the write.  After reading key 5, key 1 will need
    //  to be read to return key 5 to Valkey, unbloking the synchronous wait.
    asyncWait200msAndReadTwoKeys(it);

    monotime blockTimer;
    elapsedStart(&blockTimer);
    simulateUnblockedWrite(c);
    // Must have delayed at least 150ms (some time may have passed before timer start)
    EXPECT_GT(elapsedMs(blockTimer), 150u);

    // Continue...
    expectReadKeySequence(it, 2, 4);
    // 5 has already been processed
    expectReadKeySequence(it, 6, 9);
    expectReadComplete(it);
    freeTestClient(c);
}


TEST_F(BgIterationTest, replicationReceivedWhileProcessingLastKey) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    client *c = getWriteClient(0, "xxx");

    expectReadKeySequence(it, 0, 9);
    simulateUnblockedWriteWithModification(c);         // Wouldn't be blocked because done with key 0
    expectReadReplication(it, c);   // Replication happened while processing key 9, should be here.

    simulateUnblockedWriteWithModification(c);         // This won't replicate because we are done processing key 9
    expectReadComplete(it);         // We expect to see the completion instead

    freeTestClient(c);
}


TEST_F(BgIterationTest, repldoneFunctionCalled) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, iteratorRepldoneFn, iteratorCleanupFn, PRIVDATA);

    client *c = getWriteClient(0, "xxx");

    expectReadKeySequence(it, 0, 9);
    simulateUnblockedWriteWithModification(c);         // Wouldn't be blocked because done with key 0
    expectReadReplication(it, c);   // Replication happened while processing key 9, should be here.
    EXPECT_EQ(repldoneCount, 1);    // Last key released, now done feeding replication

    simulateUnblockedWriteWithModification(c);         // This won't replicate because we are done processing key 9
    expectReadComplete(it);         // We expect to see the completion instead

    freeTestClient(c);
}


TEST_F(BgIterationTest, repldoneFunctionCalledTwice) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, iteratorRepldoneFnNotBeingReadyInitially, iteratorCleanupFn, PRIVDATA);

    client *c = getWriteClient(0, "xxx");

    expectReadKeySequence(it, 0, 9);
    simulateUnblockedWriteWithModification(c);         // Wouldn't be blocked because done with key 0
    expectReadReplication(it, c);   // Replication happened while processing key 9, should be here.
    EXPECT_EQ(repldoneCount, 0);    // Last key released, now done feeding replication
    EXPECT_EQ(isReplDoneReady, 1);
    bgIteration_feedIterators();    // Need to call it as RepldoneFnNotBeingReadyInitially returns false in first call
    EXPECT_EQ(repldoneCount, 1);

    simulateUnblockedWriteWithModification(c);         // This won't replicate because we are done processing key 9
    expectReadComplete(it);         // We expect to see the completion instead

    freeTestClient(c);
}


TEST_F(BgIterationTest, queuingitemFunctionCalled) {
    bgIterator *it = bgIteratorCreateFullScanIter("simple",
            0, NULL, iteratorCleanupFn, iteratorBeforeAndAfterQueuingItemFn, PRIVDATA);
    EXPECT_EQ(beforeQueuingItemCount, 0);
    EXPECT_EQ(afterQueuingItemCount, 0);
    expectReadKeySequence(it, 0, 9);
    expectReadComplete(it);
    // Callback is invoked when item is fed to and returned from an iterator
    EXPECT_EQ(beforeQueuingItemCount, 10);
    EXPECT_EQ(afterQueuingItemCount, 10);
}

TEST_F(BgIterationTest, checkReplicationByteCount) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, iteratorRepldoneFn, iteratorCleanupFn, PRIVDATA);

    client *c = getWriteClient(0, "xxx");
    int expectedReplicationSize = sizeof(bgIteratorItem);
    for (int i = 0;  i < c->argc;  i++) {
        expectedReplicationSize += objectComputeSize(NULL, c->argv[i], 0, 0);
    }

    expectReadKey(it, 0);
    expectReadKey(it, 1);  // Releases and unblocks 0
    EXPECT_EQ(bgIteration_memoryInuseForReplication(), 0u);

    simulateUnblockedWriteWithModification(c);         // Wouldn't be blocked because done with key 0
    EXPECT_EQ(bgIteration_memoryInuseForReplication(), expectedReplicationSize);
    simulateUnblockedWriteWithModification(c);         // and write again (2nd replication)
    EXPECT_EQ(bgIteration_memoryInuseForReplication(), 2 * expectedReplicationSize);

    expectReadKeySequence(it, 2, 4);    // Keys 1..4 all in same bucket

    expectReadReplication(it, c);
    // After reading the 1st replication, it hasn't been returned yet (it's the active item)
    EXPECT_EQ(bgIteration_memoryInuseForReplication(), 2 * expectedReplicationSize);
    expectReadReplication(it, c);
    // After reading the 2nd replication, the 1st has been returned
    EXPECT_EQ(bgIteration_memoryInuseForReplication(), expectedReplicationSize);

    expectReadKey(it, 5);
    // Now all replication has been returned/freed
    EXPECT_EQ(bgIteration_memoryInuseForReplication(), 0u);

    expectReadKeySequence(it, 6, 9);
    expectReadComplete(it);

    freeTestClient(c);
}

// Test that for an arbitrary write command having no keys, replication should occur.
TEST_F(BgIterationTest, checkNoKeysWriteIsReplicated) {
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION, NULL, iteratorCleanupFn, PRIVDATA);

    expectReadKey(it, 0);

    client *c = getNoKeysWriteClient();
    EXPECT_CALL(mock, blockClientInUseOnKeys(c,_,_)).Times(0);
    bool blocked = bgIteration_blockClientIfRequired(c);
    EXPECT_FALSE(blocked);
    bgIteration_handleCommandReplication(c->db->id, c->cmd, c->argc, c->argv);

    expectReadKeySequence(it, 1, 4);    // These were already in queue

    expectReadReplication(it, c);

    expectReadKeySequence(it, 5, 9);
    expectReadComplete(it);
    freeTestClient(c);
}
TEST_F(BgIterationTestClusterSlots, testAmzKeyIsLogicallyDeletedInOrderedIteration3Slots) {
    bgIterator *it = bgIteratorCreateSlotsIter("simple",
            0, slots_to_iterate, slots_to_iterate_size, NULL, iteratorCleanupFn, PRIVDATA);
    EXPECT_CALL(mock, amzKeyIsLogicallyDeleted(_)).WillOnce(Return(true)).WillRepeatedly(Return(false));
    expectReadKeySequence(it, 1, n_keys_to_read - 1);

    // Quick status check.  At this point, the last item hasn't been returned yet.
    bgIteratorStatus status;
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentries_queued, n_keys_to_read - 1); // The first item should be skipped from the queue
    EXPECT_EQ(status.dbentries_processed, n_keys_to_read - 2);

    expectReadComplete(it);
    EXPECT_FALSE(cleanupTerminated);
}

TEST_F(BgIterationTest, testAmzKeyIsLogicallyDeletedInOrderedFullScanIteration) {
    bgIterator *it = bgIteratorCreateFullScanIter("simple",
            0, NULL, iteratorCleanupFn, PRIVDATA);
    EXPECT_CALL(mock, amzKeyIsLogicallyDeleted(_)).WillOnce(Return(true)).WillRepeatedly(Return(false));
    expectReadKeySequence(it, 1, 9);

    // Quick status check.  At this point, item #9 hasn't been returned yet.
    bgIteratorStatus status;
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.dbentries_queued, 9u); // The first item should be skipped from the queue
    EXPECT_EQ(status.dbentries_processed, 8u);

    expectReadComplete(it);
    EXPECT_FALSE(cleanupTerminated);
}
#endif

#ifdef CODE_NOT_READY_YET
class BgIterationTestCluster : public BgIterationTest {
    private:
        // This is the expected order of the keys when hashed into a single dict at slot 0 having size 8.
        //  The "{06S}" prefix ensures use of only slot 0.
        const char *keys[1][5] = {{"{06S}C0", "{06S}D0", "{06S}A0", "{06S}B0", "{06S}E0"}};

    protected:
        // Furthermore, the bucketization will look like this:
        // db 0 slot 0
        // Table 0, used 5, exp 1, top-level buckets 2, child buckets 0
        // Bucket 0:0 level:0
        //   0 h2 1a, key "{06S}C0"
        //   1 h2 7b, key "{06S}D0"
        //   2 (empty)
        //   3 (empty)
        //   4 (empty)
        //   5 (empty)
        //   6 (empty)
        // Bucket 0:1 level:0
        //   0 h2 5c, key "{06S}A0"
        //   1 h2 bf, key "{06S}B0"
        //   2 h2 57, key "{06S}E0"
        //   3 (empty)
        //   4 (empty)
        //   5 (empty)
        //   6 (empty)

        virtual const char * getKeyAtDbSeq(int db, int seq) override {
            assert(db == 0);
            return keys[db][seq];
        }


        virtual void setupDatabase() override {
            // For these unit tests, a standard database is constructed.  The order of items in the
            //  hash table is important, and this is validated here.  If the hash table
            //  implementation changes, we will find out quickly at this point.  All other tests
            //  will become invalid!

            // Note that the cluster_enabled tests are designed for the purpose of testing
            //  CONSISTENT iteration WITH REPLICATION.  This type of iteration is not supported
            //  in non-cluster-mode.  At the time of writing, there is no-known use-case for this
            //  combination.  But it is tested for completeness and to ensure future availability.

            // Note also that the cluster_enabled tests are not designed to address issues specific
            //  to per-slot-dictionaries.  The tests are simplified by ensuring that all keys are
            //  mapped to slot-0.  It is assumed that iteration would progress in slot order, and
            //  failure in this regard will be caught in integration tests (amztests).

            server.dbnum = 1;   // cluster-mode means 1 DB
            server.cluster_enabled = true;
            server.db = static_cast<serverDb **>(zcalloc(sizeof(serverDb *) * server.dbnum));

            // Yes, it's cluster mode, but we're mapping all keys to slot 0 - so we cheat and create only 1 dict (just like CMD).
            initializeServerDb(0, CLUSTER_SLOT_MASK_BITS);

            // Note "06S" is a prefix that maps to slot 0.  We're not testing slots here.

            addKeyToDb(0, "{06S}A0", "{06S}A0");
            addKeyToDb(0, "{06S}B0", "{06S}B0");
            addKeyToDb(0, "{06S}C0", "{06S}C0");
            addKeyToDb(0, "{06S}D0", "{06S}D0");
            addKeyToDb(0, "{06S}E0", "{06S}E0");

            // In case we need to debug...
            if (0) debugPrintBucketInfo();

            // Validate that the iteration order matches the expected order
            hashtableIterator *it = hashtableCreateIterator(server.db[0]->keys->hashtables[0], 0);
            for (int i = 0;  i < 5;  i++) {
                void *nextEntry;
                hashtableNext(it, &nextEntry);
                dbEntry *de = static_cast<dbEntry *>(nextEntry);
                ASSERT_STREQ(static_cast<const char*>(objectGetKey(de)), getKeyAtDbSeq(0, i));
            }
            hashtableReleaseIterator(it);
        }
};
#endif

#ifdef CODE_NOT_READY_YET
TEST_F(BgIterationTestCluster, dictIsOK) {
    // Just run the setup/teardown code to make sure the dict is OK.
}


TEST_F(BgIterationTestCluster, modFutureItem_YesReplication_YesConsistent_cluster) {
    // Cluster test.  REPLICATION + CONSISTENT only supported in cluster mode
    bgIterator *it = bgIteratorCreateFullScanIter("iter",
            BGITERATOR_FLAG_REPLICATION | BGITERATOR_FLAG_CONSISTENT,
            NULL, iteratorCleanupFn, PRIVDATA);
    bgIteratorStatus status;

    // For this test, don't read the 1st key - we only have 5 keys since not using DB[1]
    bgIteration_feedIterators();    // Prime the feed - key 0 and 1 are now enqueued

    // At this point, key 0, and 1 are queued.  Fake a modification to key 2 & 4 - two keys to ensure
    //  that replication is ordered
    client *c1 = getWriteClient(2, "xxx");
    client *c2 = getWriteClient(4, "yyy");

    // Since this is consistent, we will block the client, disallowing the write.
    simulateBlockedWrite(c1);
    simulateBlockedWrite(c2);

    // On a consistent iterator, the event is expedited in-front of items already in queue!
    //  Read keys 2&4 out of order.
    expectReadKey(it, 2);  // reading original/unmodified item

    // This call is expected to unblock the client waiting on #2
    expectReadKeyWithUnblock(it, 4, nullptr, 2);  // reading original/unmodified item
    simulateUnblockedWriteWithModification(c1);
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.replication_queued, 1u);
    EXPECT_EQ(status.replication_processed, 0u);

    // Now read items 0 and 1 - these were actually already queued before keys 1 & 4 were expedited.
    // This call is expected to unblock the client waiting on #4
    expectReadKeyWithUnblock(it, 0, nullptr, 4);
    simulateUnblockedWriteWithModification(c2);
    expectReadKey(it, 1);
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.replication_queued, 2u);
    EXPECT_EQ(status.replication_processed, 0u);

     // And now the 2 replications are queued
    expectReadReplication(it, c1);
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.replication_queued, 2u);       // 1st replication still being processed
    EXPECT_EQ(status.replication_processed, 0u);    //  (no change in these metrics yet)

    expectReadReplication(it, c2);
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.replication_queued, 2u);
    EXPECT_EQ(status.replication_processed, 1u);    // Done with 1st, processing 2nd

     // Continue...
    expectReadKey(it, 3);
    bgIteratorGetStatus(it, &status);
    EXPECT_EQ(status.replication_queued, 2u);
    EXPECT_EQ(status.replication_processed, 2u);    // Done processing both repl items
    expectReadComplete(it);
    freeTestClient(c1);
    freeTestClient(c2);
}
#endif



// JHB - need test that hashing is paused when an entry is in use.
