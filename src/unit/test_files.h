/* Do not modify this file, it's automatically generated from utils/generate-unit-test-header.py */
/* clang-format off */
typedef int unitTestProc(int argc, char **argv, int flags);

typedef struct unitTest {
    char *name;
    unitTestProc *proc;
} unitTest;

int test_crc64(int argc, char **argv, int flags);
int test_crc64combine(int argc, char **argv, int flags);
int test_dictCreate(int argc, char **argv, int flags);
int test_dictAdd16Keys(int argc, char **argv, int flags);
int test_dictDisableResize(int argc, char **argv, int flags);
int test_dictAddOneKeyTriggerResize(int argc, char **argv, int flags);
int test_dictDeleteKeys(int argc, char **argv, int flags);
int test_dictDeleteOneKeyTriggerResize(int argc, char **argv, int flags);
int test_dictEmptyDirAdd128Keys(int argc, char **argv, int flags);
int test_dictDisableResizeReduceTo3(int argc, char **argv, int flags);
int test_dictDeleteOneKeyTriggerResizeAgain(int argc, char **argv, int flags);
int test_dictBenchmark(int argc, char **argv, int flags);
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
int test_listpackCreateIntList(int argc, char **argv, int flags);
int test_listpackCreateList(int argc, char **argv, int flags);
int test_listpackLpPrepend(int argc, char **argv, int flags);
int test_listpackLpPrependInteger(int argc, char **argv, int flags);
int test_listpackGetELementAtIndex(int argc, char **argv, int flags);
int test_listpackPop(int argc, char **argv, int flags);
int test_listpackGetELementAtIndex2(int argc, char **argv, int flags);
int test_listpackIterate0toEnd(int argc, char **argv, int flags);
int test_listpackIterate1toEnd(int argc, char **argv, int flags);
int test_listpackIterate2toEnd(int argc, char **argv, int flags);
int test_listpackIterateBackToFront(int argc, char **argv, int flags);
int test_listpackIterateBackToFrontWithDelete(int argc, char **argv, int flags);
int test_listpackDeleteWhenNumIsMinusOne(int argc, char **argv, int flags);
int test_listpackDeleteWithNegativeIndex(int argc, char **argv, int flags);
int test_listpackDeleteInclusiveRange0_0(int argc, char **argv, int flags);
int test_listpackDeleteInclusiveRange0_1(int argc, char **argv, int flags);
int test_listpackDeleteInclusiveRange1_2(int argc, char **argv, int flags);
int test_listpackDeleteWitStartIndexOutOfRange(int argc, char **argv, int flags);
int test_listpackDeleteWitNumOverflow(int argc, char **argv, int flags);
int test_listpackBatchDelete(int argc, char **argv, int flags);
int test_listpackDeleteFooWhileIterating(int argc, char **argv, int flags);
int test_listpackReplaceWithSameSize(int argc, char **argv, int flags);
int test_listpackReplaceWithDifferentSize(int argc, char **argv, int flags);
int test_listpackRegressionGt255Bytes(int argc, char **argv, int flags);
int test_listpackCreateLongListAndCheckIndices(int argc, char **argv, int flags);
int test_listpackCompareStrsWithLpEntries(int argc, char **argv, int flags);
int test_listpackLpMergeEmptyLps(int argc, char **argv, int flags);
int test_listpackLpMergeLp1Larger(int argc, char **argv, int flags);
int test_listpackLpMergeLp2Larger(int argc, char **argv, int flags);
int test_listpackLpNextRandom(int argc, char **argv, int flags);
int test_listpackLpNextRandomCC(int argc, char **argv, int flags);
int test_listpackRandomPairWithOneElement(int argc, char **argv, int flags);
int test_listpackRandomPairWithManyElements(int argc, char **argv, int flags);
int test_listpackRandomPairsWithOneElement(int argc, char **argv, int flags);
int test_listpackRandomPairsWithManyElements(int argc, char **argv, int flags);
int test_listpackRandomPairsUniqueWithOneElement(int argc, char **argv, int flags);
int test_listpackRandomPairsUniqueWithManyElements(int argc, char **argv, int flags);
int test_listpackPushVariousEncodings(int argc, char **argv, int flags);
int test_listpackLpFind(int argc, char **argv, int flags);
int test_listpackLpValidateIntegrity(int argc, char **argv, int flags);
int test_listpackNumberOfElementsExceedsLP_HDR_NUMELE_UNKNOWN(int argc, char **argv, int flags);
int test_listpackStressWithRandom(int argc, char **argv, int flags);
int test_listpackSTressWithVariableSize(int argc, char **argv, int flags);
int test_listpackBenchmarkInit(int argc, char *argv[], int flags);
int test_listpackBenchmarkLpAppend(int argc, char **argv, int flags);
int test_listpackBenchmarkLpFindString(int argc, char **argv, int flags);
int test_listpackBenchmarkLpFindNumber(int argc, char **argv, int flags);
int test_listpackBenchmarkLpSeek(int argc, char **argv, int flags);
int test_listpackBenchmarkLpValidateIntegrity(int argc, char **argv, int flags);
int test_listpackBenchmarkLpCompareWithString(int argc, char **argv, int flags);
int test_listpackBenchmarkLpCompareWithNumber(int argc, char **argv, int flags);
int test_listpackBenchmarkFree(int argc, char **argv, int flags);
int test_sds(int argc, char **argv, int flags);
int test_typesAndAllocSize(int argc, char **argv, int flags);
int test_sdsHeaderSizes(int argc, char **argv, int flags);
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
int test_zipmapIterateWithLargeKey(int argc, char *argv[], int flags);
int test_zipmapIterateThroughElements(int argc, char *argv[], int flags);
int test_zmallocInitialUsedMemory(int argc, char **argv, int flags);
int test_zmallocAllocReallocCallocAndFree(int argc, char **argv, int flags);
int test_zmallocAllocZeroByteAndFree(int argc, char **argv, int flags);

unitTest __test_crc64_c[] = {{"test_crc64", test_crc64}, {NULL, NULL}};
unitTest __test_crc64combine_c[] = {{"test_crc64combine", test_crc64combine}, {NULL, NULL}};
unitTest __test_dict_c[] = {{"test_dictCreate", test_dictCreate}, {"test_dictAdd16Keys", test_dictAdd16Keys}, {"test_dictDisableResize", test_dictDisableResize}, {"test_dictAddOneKeyTriggerResize", test_dictAddOneKeyTriggerResize}, {"test_dictDeleteKeys", test_dictDeleteKeys}, {"test_dictDeleteOneKeyTriggerResize", test_dictDeleteOneKeyTriggerResize}, {"test_dictEmptyDirAdd128Keys", test_dictEmptyDirAdd128Keys}, {"test_dictDisableResizeReduceTo3", test_dictDisableResizeReduceTo3}, {"test_dictDeleteOneKeyTriggerResizeAgain", test_dictDeleteOneKeyTriggerResizeAgain}, {"test_dictBenchmark", test_dictBenchmark}, {NULL, NULL}};
unitTest __test_endianconv_c[] = {{"test_endianconv", test_endianconv}, {NULL, NULL}};
unitTest __test_intset_c[] = {{"test_intsetValueEncodings", test_intsetValueEncodings}, {"test_intsetBasicAdding", test_intsetBasicAdding}, {"test_intsetLargeNumberRandomAdd", test_intsetLargeNumberRandomAdd}, {"test_intsetUpgradeFromint16Toint32", test_intsetUpgradeFromint16Toint32}, {"test_intsetUpgradeFromint16Toint64", test_intsetUpgradeFromint16Toint64}, {"test_intsetUpgradeFromint32Toint64", test_intsetUpgradeFromint32Toint64}, {"test_intsetStressLookups", test_intsetStressLookups}, {"test_intsetStressAddDelete", test_intsetStressAddDelete}, {NULL, NULL}};
unitTest __test_kvstore_c[] = {{"test_kvstoreAdd16Keys", test_kvstoreAdd16Keys}, {"test_kvstoreIteratorRemoveAllKeysNoDeleteEmptyDict", test_kvstoreIteratorRemoveAllKeysNoDeleteEmptyDict}, {"test_kvstoreIteratorRemoveAllKeysDeleteEmptyDict", test_kvstoreIteratorRemoveAllKeysDeleteEmptyDict}, {"test_kvstoreDictIteratorRemoveAllKeysNoDeleteEmptyDict", test_kvstoreDictIteratorRemoveAllKeysNoDeleteEmptyDict}, {"test_kvstoreDictIteratorRemoveAllKeysDeleteEmptyDict", test_kvstoreDictIteratorRemoveAllKeysDeleteEmptyDict}, {NULL, NULL}};
unitTest __test_listpack_c[] = {{"test_listpackCreateIntList", test_listpackCreateIntList}, {"test_listpackCreateList", test_listpackCreateList}, {"test_listpackLpPrepend", test_listpackLpPrepend}, {"test_listpackLpPrependInteger", test_listpackLpPrependInteger}, {"test_listpackGetELementAtIndex", test_listpackGetELementAtIndex}, {"test_listpackPop", test_listpackPop}, {"test_listpackGetELementAtIndex2", test_listpackGetELementAtIndex2}, {"test_listpackIterate0toEnd", test_listpackIterate0toEnd}, {"test_listpackIterate1toEnd", test_listpackIterate1toEnd}, {"test_listpackIterate2toEnd", test_listpackIterate2toEnd}, {"test_listpackIterateBackToFront", test_listpackIterateBackToFront}, {"test_listpackIterateBackToFrontWithDelete", test_listpackIterateBackToFrontWithDelete}, {"test_listpackDeleteWhenNumIsMinusOne", test_listpackDeleteWhenNumIsMinusOne}, {"test_listpackDeleteWithNegativeIndex", test_listpackDeleteWithNegativeIndex}, {"test_listpackDeleteInclusiveRange0_0", test_listpackDeleteInclusiveRange0_0}, {"test_listpackDeleteInclusiveRange0_1", test_listpackDeleteInclusiveRange0_1}, {"test_listpackDeleteInclusiveRange1_2", test_listpackDeleteInclusiveRange1_2}, {"test_listpackDeleteWitStartIndexOutOfRange", test_listpackDeleteWitStartIndexOutOfRange}, {"test_listpackDeleteWitNumOverflow", test_listpackDeleteWitNumOverflow}, {"test_listpackBatchDelete", test_listpackBatchDelete}, {"test_listpackDeleteFooWhileIterating", test_listpackDeleteFooWhileIterating}, {"test_listpackReplaceWithSameSize", test_listpackReplaceWithSameSize}, {"test_listpackReplaceWithDifferentSize", test_listpackReplaceWithDifferentSize}, {"test_listpackRegressionGt255Bytes", test_listpackRegressionGt255Bytes}, {"test_listpackCreateLongListAndCheckIndices", test_listpackCreateLongListAndCheckIndices}, {"test_listpackCompareStrsWithLpEntries", test_listpackCompareStrsWithLpEntries}, {"test_listpackLpMergeEmptyLps", test_listpackLpMergeEmptyLps}, {"test_listpackLpMergeLp1Larger", test_listpackLpMergeLp1Larger}, {"test_listpackLpMergeLp2Larger", test_listpackLpMergeLp2Larger}, {"test_listpackLpNextRandom", test_listpackLpNextRandom}, {"test_listpackLpNextRandomCC", test_listpackLpNextRandomCC}, {"test_listpackRandomPairWithOneElement", test_listpackRandomPairWithOneElement}, {"test_listpackRandomPairWithManyElements", test_listpackRandomPairWithManyElements}, {"test_listpackRandomPairsWithOneElement", test_listpackRandomPairsWithOneElement}, {"test_listpackRandomPairsWithManyElements", test_listpackRandomPairsWithManyElements}, {"test_listpackRandomPairsUniqueWithOneElement", test_listpackRandomPairsUniqueWithOneElement}, {"test_listpackRandomPairsUniqueWithManyElements", test_listpackRandomPairsUniqueWithManyElements}, {"test_listpackPushVariousEncodings", test_listpackPushVariousEncodings}, {"test_listpackLpFind", test_listpackLpFind}, {"test_listpackLpValidateIntegrity", test_listpackLpValidateIntegrity}, {"test_listpackNumberOfElementsExceedsLP_HDR_NUMELE_UNKNOWN", test_listpackNumberOfElementsExceedsLP_HDR_NUMELE_UNKNOWN}, {"test_listpackStressWithRandom", test_listpackStressWithRandom}, {"test_listpackSTressWithVariableSize", test_listpackSTressWithVariableSize}, {"test_listpackBenchmarkInit", test_listpackBenchmarkInit}, {"test_listpackBenchmarkLpAppend", test_listpackBenchmarkLpAppend}, {"test_listpackBenchmarkLpFindString", test_listpackBenchmarkLpFindString}, {"test_listpackBenchmarkLpFindNumber", test_listpackBenchmarkLpFindNumber}, {"test_listpackBenchmarkLpSeek", test_listpackBenchmarkLpSeek}, {"test_listpackBenchmarkLpValidateIntegrity", test_listpackBenchmarkLpValidateIntegrity}, {"test_listpackBenchmarkLpCompareWithString", test_listpackBenchmarkLpCompareWithString}, {"test_listpackBenchmarkLpCompareWithNumber", test_listpackBenchmarkLpCompareWithNumber}, {"test_listpackBenchmarkFree", test_listpackBenchmarkFree}, {NULL, NULL}};
unitTest __test_sds_c[] = {{"test_sds", test_sds}, {"test_typesAndAllocSize", test_typesAndAllocSize}, {"test_sdsHeaderSizes", test_sdsHeaderSizes}, {NULL, NULL}};
unitTest __test_sha1_c[] = {{"test_sha1", test_sha1}, {NULL, NULL}};
unitTest __test_util_c[] = {{"test_string2ll", test_string2ll}, {"test_string2l", test_string2l}, {"test_ll2string", test_ll2string}, {"test_ld2string", test_ld2string}, {"test_fixedpoint_d2string", test_fixedpoint_d2string}, {"test_version2num", test_version2num}, {"test_reclaimFilePageCache", test_reclaimFilePageCache}, {NULL, NULL}};
unitTest __test_ziplist_c[] = {{"test_ziplistCreateIntList", test_ziplistCreateIntList}, {"test_ziplistPop", test_ziplistPop}, {"test_ziplistGetElementAtIndex3", test_ziplistGetElementAtIndex3}, {"test_ziplistGetElementOutOfRange", test_ziplistGetElementOutOfRange}, {"test_ziplistGetLastElement", test_ziplistGetLastElement}, {"test_ziplistGetFirstElement", test_ziplistGetFirstElement}, {"test_ziplistGetElementOutOfRangeReverse", test_ziplistGetElementOutOfRangeReverse}, {"test_ziplistIterateThroughFullList", test_ziplistIterateThroughFullList}, {"test_ziplistIterateThroughListFrom1ToEnd", test_ziplistIterateThroughListFrom1ToEnd}, {"test_ziplistIterateThroughListFrom2ToEnd", test_ziplistIterateThroughListFrom2ToEnd}, {"test_ziplistIterateThroughStartOutOfRange", test_ziplistIterateThroughStartOutOfRange}, {"test_ziplistIterateBackToFront", test_ziplistIterateBackToFront}, {"test_ziplistIterateBackToFrontDeletingAllItems", test_ziplistIterateBackToFrontDeletingAllItems}, {"test_ziplistDeleteInclusiveRange0To0", test_ziplistDeleteInclusiveRange0To0}, {"test_ziplistDeleteInclusiveRange0To1", test_ziplistDeleteInclusiveRange0To1}, {"test_ziplistDeleteInclusiveRange1To2", test_ziplistDeleteInclusiveRange1To2}, {"test_ziplistDeleteWithStartIndexOutOfRange", test_ziplistDeleteWithStartIndexOutOfRange}, {"test_ziplistDeleteWithNumOverflow", test_ziplistDeleteWithNumOverflow}, {"test_ziplistDeleteFooWhileIterating", test_ziplistDeleteFooWhileIterating}, {"test_ziplistReplaceWithSameSize", test_ziplistReplaceWithSameSize}, {"test_ziplistReplaceWithDifferentSize", test_ziplistReplaceWithDifferentSize}, {"test_ziplistRegressionTestForOver255ByteStrings", test_ziplistRegressionTestForOver255ByteStrings}, {"test_ziplistRegressionTestDeleteNextToLastEntries", test_ziplistRegressionTestDeleteNextToLastEntries}, {"test_ziplistCreateLongListAndCheckIndices", test_ziplistCreateLongListAndCheckIndices}, {"test_ziplistCompareStringWithZiplistEntries", test_ziplistCompareStringWithZiplistEntries}, {"test_ziplistMergeTest", test_ziplistMergeTest}, {"test_ziplistStressWithRandomPayloadsOfDifferentEncoding", test_ziplistStressWithRandomPayloadsOfDifferentEncoding}, {"test_ziplistCascadeUpdateEdgeCases", test_ziplistCascadeUpdateEdgeCases}, {"test_ziplistInsertEdgeCase", test_ziplistInsertEdgeCase}, {"test_ziplistStressWithVariableSize", test_ziplistStressWithVariableSize}, {"test_BenchmarkziplistFind", test_BenchmarkziplistFind}, {"test_BenchmarkziplistIndex", test_BenchmarkziplistIndex}, {"test_BenchmarkziplistValidateIntegrity", test_BenchmarkziplistValidateIntegrity}, {"test_BenchmarkziplistCompareWithString", test_BenchmarkziplistCompareWithString}, {"test_BenchmarkziplistCompareWithNumber", test_BenchmarkziplistCompareWithNumber}, {"test_ziplistStress__ziplistCascadeUpdate", test_ziplistStress__ziplistCascadeUpdate}, {NULL, NULL}};
unitTest __test_zipmap_c[] = {{"test_zipmapIterateWithLargeKey", test_zipmapIterateWithLargeKey}, {"test_zipmapIterateThroughElements", test_zipmapIterateThroughElements}, {NULL, NULL}};
unitTest __test_zmalloc_c[] = {{"test_zmallocInitialUsedMemory", test_zmallocInitialUsedMemory}, {"test_zmallocAllocReallocCallocAndFree", test_zmallocAllocReallocCallocAndFree}, {"test_zmallocAllocZeroByteAndFree", test_zmallocAllocZeroByteAndFree}, {NULL, NULL}};

struct unitTestSuite {
    char *filename;
    unitTest *tests;
} unitTestSuite[] = {
    {"test_crc64.c", __test_crc64_c},
    {"test_crc64combine.c", __test_crc64combine_c},
    {"test_dict.c", __test_dict_c},
    {"test_endianconv.c", __test_endianconv_c},
    {"test_intset.c", __test_intset_c},
    {"test_kvstore.c", __test_kvstore_c},
    {"test_listpack.c", __test_listpack_c},
    {"test_sds.c", __test_sds_c},
    {"test_sha1.c", __test_sha1_c},
    {"test_util.c", __test_util_c},
    {"test_ziplist.c", __test_ziplist_c},
    {"test_zipmap.c", __test_zipmap_c},
    {"test_zmalloc.c", __test_zmalloc_c},
};
