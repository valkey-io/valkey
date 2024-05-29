/* Do not modify this file, it's automatically generated from utils/generate-unit-test-header.py */
/* clang-format off */
typedef int unitTestProc(int argc, char **argv, int flags);

typedef struct unitTest {
    char *name;
    unitTestProc *proc;
} unitTest;

int test_crc64(int argc, char **argv, int flags);
int test_crc64combine(int argc, char **argv, int flags);
int test_endianconv(int argc, char *argv[], int flags);
int test_intsetValueEncodings(int argc, char **argv, int flags);
int test_intsetBasicAdding(int argc, char **argv, int flags);
int test_intsetLargeNumberRandomAdd(int argc, char **argv, int flags);
int test_intsetUpgradeFromint16Toint32(int argc, char **argv, int flags);
int test_intsetUpgradeFromint16Toint64(int argc, char **argv, int flags);
int test_intsetUpgradeFromint32Toint64(int argc, char **argv, int flags);
int test_intsetStressLookups(int argc, char **argv, int flags);
int test_intsetStressAddDelete(int argc, char **argv, int flags);
int test_kvstoreAdd16Keys(int argc, char **argv, int flags);
int test_kvstoreIteratorRemoveAllKeysNoDeleteEmptyDict(int argc, char **argv, int flags);
int test_kvstoreIteratorRemoveAllKeysDeleteEmptyDict(int argc, char **argv, int flags);
int test_kvstoreDictIteratorRemoveAllKeysNoDeleteEmptyDict(int argc, char **argv, int flags);
int test_kvstoreDictIteratorRemoveAllKeysDeleteEmptyDict(int argc, char **argv, int flags);
int test_sds(int argc, char **argv, int flags);
int test_sha1(int argc, char **argv, int flags);
int test_string2ll(int argc, char **argv, int flags);
int test_string2l(int argc, char **argv, int flags);
int test_ll2string(int argc, char **argv, int flags);
int test_ld2string(int argc, char **argv, int flags);
int test_fixedpoint_d2string(int argc, char **argv, int flags);
int test_version2num(int argc, char **argv, int flags);
int test_reclaimFilePageCache(int argc, char **argv, int flags);
int test_ziplistCreateIntList(int argc, char **argv, int flags);
int test_ziplistPop(int argc, char **argv, int flags);
int test_ziplistGetElementAtIndex3(int argc, char **argv, int flags);
int test_ziplistGetElementOutOfRange(int argc, char **argv, int flags);
int test_ziplistGetLastElement(int argc, char **argv, int flags);
int test_ziplistGetFirstElement(int argc, char **argv, int flags);
int test_ziplistGetElementOutOfRangeReverse(int argc, char **argv, int flags);
int test_ziplistIterateThroughFullList(int argc, char **argv, int flags);
int test_ziplistIterateThroughListFrom1ToEnd(int argc, char **argv, int flags);
int test_ziplistIterateThroughListFrom2ToEnd(int argc, char **argv, int flags);
int test_ziplistIterateThroughStartOutOfRange(int argc, char **argv, int flags);
int test_ziplistIterateBackToFront(int argc, char **argv, int flags);
int test_ziplistIterateBackToFrontDeletingAllItems(int argc, char **argv, int flags);
int test_ziplistDeleteInclusiveRange0To0(int argc, char **argv, int flags);
int test_ziplistDeleteInclusiveRange0To1(int argc, char **argv, int flags);
int test_ziplistDeleteInclusiveRange1To2(int argc, char **argv, int flags);
int test_ziplistDeleteWithStartIndexOutOfRange(int argc, char **argv, int flags);
int test_ziplistDeleteWithNumOverflow(int argc, char **argv, int flags);
int test_ziplistDeleteFooWhileIterating(int argc, char **argv, int flags);
int test_ziplistReplaceWithSameSize(int argc, char **argv, int flags);
int test_ziplistReplaceWithDifferentSize(int argc, char **argv, int flags);
int test_ziplistRegressionTestForOver255ByteStrings(int argc, char **argv, int flags);
int test_ziplistRegressionTestDeleteNextToLastEntries(int argc, char **argv, int flags);
int test_ziplistCreateLongListAndCheckIndices(int argc, char **argv, int flags);
int test_ziplistCompareStringWithZiplistEntries(int argc, char **argv, int flags);
int test_ziplistMergeTest(int argc, char **argv, int flags);
int test_ziplistStressWithRandomPayloadsOfDifferentEncoding(int argc, char **argv, int flags);
int test_ziplistCascadeUpdateEdgeCases(int argc, char **argv, int flags);
int test_ziplistInsertEdgeCase(int argc, char **argv, int flags);
int test_ziplistStressWithVariableSize(int argc, char **argv, int flags);
int test_BenchmarkziplistFind(int argc, char **argv, int flags);
int test_BenchmarkziplistIndex(int argc, char **argv, int flags);
int test_BenchmarkziplistValidateIntegrity(int argc, char **argv, int flags);
int test_BenchmarkziplistCompareWithString(int argc, char **argv, int flags);
int test_BenchmarkziplistCompareWithNumber(int argc, char **argv, int flags);
int test_ziplistStress__ziplistCascadeUpdate(int argc, char **argv, int flags);
int test_zmallocInitialUsedMemory(int argc, char **argv, int flags);
int test_zmallocAllocReallocCallocAndFree(int argc, char **argv, int flags);
int test_zmallocAllocZeroByteAndFree(int argc, char **argv, int flags);

unitTest __test_crc64_c[] = {{"test_crc64", test_crc64}, {NULL, NULL}};
unitTest __test_crc64combine_c[] = {{"test_crc64combine", test_crc64combine}, {NULL, NULL}};
unitTest __test_endianconv_c[] = {{"test_endianconv", test_endianconv}, {NULL, NULL}};
unitTest __test_intset_c[] = {{"test_intsetValueEncodings", test_intsetValueEncodings}, {"test_intsetBasicAdding", test_intsetBasicAdding}, {"test_intsetLargeNumberRandomAdd", test_intsetLargeNumberRandomAdd}, {"test_intsetUpgradeFromint16Toint32", test_intsetUpgradeFromint16Toint32}, {"test_intsetUpgradeFromint16Toint64", test_intsetUpgradeFromint16Toint64}, {"test_intsetUpgradeFromint32Toint64", test_intsetUpgradeFromint32Toint64}, {"test_intsetStressLookups", test_intsetStressLookups}, {"test_intsetStressAddDelete", test_intsetStressAddDelete}, {NULL, NULL}};
unitTest __test_kvstore_c[] = {{"test_kvstoreAdd16Keys", test_kvstoreAdd16Keys}, {"test_kvstoreIteratorRemoveAllKeysNoDeleteEmptyDict", test_kvstoreIteratorRemoveAllKeysNoDeleteEmptyDict}, {"test_kvstoreIteratorRemoveAllKeysDeleteEmptyDict", test_kvstoreIteratorRemoveAllKeysDeleteEmptyDict}, {"test_kvstoreDictIteratorRemoveAllKeysNoDeleteEmptyDict", test_kvstoreDictIteratorRemoveAllKeysNoDeleteEmptyDict}, {"test_kvstoreDictIteratorRemoveAllKeysDeleteEmptyDict", test_kvstoreDictIteratorRemoveAllKeysDeleteEmptyDict}, {NULL, NULL}};
unitTest __test_sds_c[] = {{"test_sds", test_sds}, {NULL, NULL}};
unitTest __test_sha1_c[] = {{"test_sha1", test_sha1}, {NULL, NULL}};
unitTest __test_util_c[] = {{"test_string2ll", test_string2ll}, {"test_string2l", test_string2l}, {"test_ll2string", test_ll2string}, {"test_ld2string", test_ld2string}, {"test_fixedpoint_d2string", test_fixedpoint_d2string}, {"test_version2num", test_version2num}, {"test_reclaimFilePageCache", test_reclaimFilePageCache}, {NULL, NULL}};
unitTest __test_ziplist_c[] = {{"test_ziplistCreateIntList", test_ziplistCreateIntList}, {"test_ziplistPop", test_ziplistPop}, {"test_ziplistGetElementAtIndex3", test_ziplistGetElementAtIndex3}, {"test_ziplistGetElementOutOfRange", test_ziplistGetElementOutOfRange}, {"test_ziplistGetLastElement", test_ziplistGetLastElement}, {"test_ziplistGetFirstElement", test_ziplistGetFirstElement}, {"test_ziplistGetElementOutOfRangeReverse", test_ziplistGetElementOutOfRangeReverse}, {"test_ziplistIterateThroughFullList", test_ziplistIterateThroughFullList}, {"test_ziplistIterateThroughListFrom1ToEnd", test_ziplistIterateThroughListFrom1ToEnd}, {"test_ziplistIterateThroughListFrom2ToEnd", test_ziplistIterateThroughListFrom2ToEnd}, {"test_ziplistIterateThroughStartOutOfRange", test_ziplistIterateThroughStartOutOfRange}, {"test_ziplistIterateBackToFront", test_ziplistIterateBackToFront}, {"test_ziplistIterateBackToFrontDeletingAllItems", test_ziplistIterateBackToFrontDeletingAllItems}, {"test_ziplistDeleteInclusiveRange0To0", test_ziplistDeleteInclusiveRange0To0}, {"test_ziplistDeleteInclusiveRange0To1", test_ziplistDeleteInclusiveRange0To1}, {"test_ziplistDeleteInclusiveRange1To2", test_ziplistDeleteInclusiveRange1To2}, {"test_ziplistDeleteWithStartIndexOutOfRange", test_ziplistDeleteWithStartIndexOutOfRange}, {"test_ziplistDeleteWithNumOverflow", test_ziplistDeleteWithNumOverflow}, {"test_ziplistDeleteFooWhileIterating", test_ziplistDeleteFooWhileIterating}, {"test_ziplistReplaceWithSameSize", test_ziplistReplaceWithSameSize}, {"test_ziplistReplaceWithDifferentSize", test_ziplistReplaceWithDifferentSize}, {"test_ziplistRegressionTestForOver255ByteStrings", test_ziplistRegressionTestForOver255ByteStrings}, {"test_ziplistRegressionTestDeleteNextToLastEntries", test_ziplistRegressionTestDeleteNextToLastEntries}, {"test_ziplistCreateLongListAndCheckIndices", test_ziplistCreateLongListAndCheckIndices}, {"test_ziplistCompareStringWithZiplistEntries", test_ziplistCompareStringWithZiplistEntries}, {"test_ziplistMergeTest", test_ziplistMergeTest}, {"test_ziplistStressWithRandomPayloadsOfDifferentEncoding", test_ziplistStressWithRandomPayloadsOfDifferentEncoding}, {"test_ziplistCascadeUpdateEdgeCases", test_ziplistCascadeUpdateEdgeCases}, {"test_ziplistInsertEdgeCase", test_ziplistInsertEdgeCase}, {"test_ziplistStressWithVariableSize", test_ziplistStressWithVariableSize}, {"test_BenchmarkziplistFind", test_BenchmarkziplistFind}, {"test_BenchmarkziplistIndex", test_BenchmarkziplistIndex}, {"test_BenchmarkziplistValidateIntegrity", test_BenchmarkziplistValidateIntegrity}, {"test_BenchmarkziplistCompareWithString", test_BenchmarkziplistCompareWithString}, {"test_BenchmarkziplistCompareWithNumber", test_BenchmarkziplistCompareWithNumber}, {"test_ziplistStress__ziplistCascadeUpdate", test_ziplistStress__ziplistCascadeUpdate}, {NULL, NULL}};
unitTest __test_zmalloc_c[] = {{"test_zmallocInitialUsedMemory", test_zmallocInitialUsedMemory}, {"test_zmallocAllocReallocCallocAndFree", test_zmallocAllocReallocCallocAndFree}, {"test_zmallocAllocZeroByteAndFree", test_zmallocAllocZeroByteAndFree}, {NULL, NULL}};

struct unitTestSuite {
    char *filename;
    unitTest *tests;
} unitTestSuite[] = {
    {"test_crc64.c", __test_crc64_c},
    {"test_crc64combine.c", __test_crc64combine_c},
    {"test_endianconv.c", __test_endianconv_c},
    {"test_intset.c", __test_intset_c},
    {"test_kvstore.c", __test_kvstore_c},
    {"test_sds.c", __test_sds_c},
    {"test_sha1.c", __test_sha1_c},
    {"test_util.c", __test_util_c},
    {"test_ziplist.c", __test_ziplist_c},
    {"test_zmalloc.c", __test_zmalloc_c},
};
