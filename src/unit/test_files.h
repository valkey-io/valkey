
typedef int unitTestProc(int argc, char **argv, int flags);

typedef struct unitTest {
    char *name;
    unitTestProc *proc;
} unitTest;

extern int test_intsetTest(int argc, char **argv, int flags);
extern int test_crc64(int argc, char *argv[], int flags);
extern int test_crc642(int argc, char *argv[], int flags);
unitTest __test_intset_c[] = {{"test_intsetTest", test_intsetTest}, {NULL, NULL}};
unitTest __test_crc64_c[] = {{"test_crc64", test_crc64}, {"test_crc642", test_crc642}, {NULL, NULL}};

struct unitTestSuite {
    char *filename;
    unitTest *tests;
} unitTestSuite[] = {
    {"test_intset_c", __test_intset_c},
    {"test_crc64_c", __test_crc64_c},
};
