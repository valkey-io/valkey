## Introduction
Valkey uses a very simple C testing framework, built up over time but now based loosely off of [Unity](https://www.throwtheswitch.org/unity).

All test files being test_ in the unit directory.
A single test file can have multiple individual tests, and they must be of the form `int test_<test_name>(int argc, char *argv[], int flags) {`, where test_name is the name of the test.
The test name must be globally unique.
A test will be marked as successful if returns 0, and will be marked failed in all other cases.

The test framework also parses several flags passed in, and sets them based on the arguments to the tests.

Tests flags:
* UNIT_TEST_ACCURATE: Corresponds to the --accurate flag. This flag indicates the test should use extra computation to more accurately validate the tests.
* UNIT_TEST_LARGE_MEMORY: Corresponds to the --large-memory flag. This flag indicates whether or not tests should use more than 100mb of memory.
* UNIT_TEST_SINGLE: Corresponds to the --single flag. This flag indicates that a single test is being executed.
* UNIT_TEST_VALGRIND: Corresponds to the --valgrind flag. This flag is just a hint passed to the test to indicate that we are running it under valgrind.

Tests are allowed to be passed in additional arbitrary argv/argc, which they can access from the argc and argv arguments of the test.

## Assertions

There are a few built in assertions that can be used, that will automatically return from the current function and return the correct error code.
Assertions are also useful as they will print out the line number that they failed on.

* `TEST_ASSERT(condition)`: Will evaluate the condition, and if it fails it will return 1 and print out the condition that failed.
* `TEST_ASSERT_MESSAGE(message, condition`): Will evaluate the condition, and if it fails it will return 1 and print out the provided message.

## Other utilities

If you would like to print out additional data, use the `TEST_PRINT_INFO(info, ...)` option, which has arguments similar to printf.
This macro will also print out the function the code was executed from in addition to the line it was printed from.

## Example test

```
int test_example(int argc, char *argv[], int flags) {
    TEST_ASSERT(5 == 5);
    TEST_ASSERT_MESSAGE("This should pass", 6 == 6);
    return 0;
} 
```

## Running tests
Tests can be run by executing:

```
make valkey-unit-tests
./valkey-unit-tests
```

Running a single unit test file
```
./valkey-unit-tests --single test_crc64.c
```

Will just run the test_crc64.c file.