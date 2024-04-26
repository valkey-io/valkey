## Introduction
Valkey uses a very simple C testing framework, built up over time but now based loosely off of [Unity](https://www.throwtheswitch.org/unity).

All test files being test_, and must contain a list of tests that take the form `int test_<test_name>(int test_crc64(int argc, char *argv[], int flags) {`, where test_name is the name of the test.
The test name must be globally unique.
A test will be marked as failed if returns 1, and will be marked successful in all other cases.

Tests are allowed to be passed in additional arbitrary argv/argc, which they can access from the argc and argv arguments of the test.
The test framework also parses several flags passed in, and sets them based on the arguments to the tests.

Tests flags:
* UNIT_TEST_ACCURATE: Corresponds to the --accurate flag. This flag indicates the test should use extra computation to more accurately validate the tests.
* UNIT_TEST_LARGE_MEMORY: Corresponds to the --large-memory flag. this flag indicates the test should use large memory allocations where possible.

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
make unit-test
./valkey-unit-tests [--flags] argv1 argv2
```

Currently, all tests always run by default.