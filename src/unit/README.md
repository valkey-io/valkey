## Introduction
Valkey uses a very simple C testing framework, built up over time but now based loosely off of [Unity](https://www.throwtheswitch.org/unity).

All test files being test_, and must contain a list of tests that take the form `int test_(test_name)(int test_crc64(int argc, char *argv[], int flags) {`.
A test will be marked as failed if returns 1, and will be marked successful in all other cases.

## Assertions

There are a few built in assertions that can be used, that will automatically return from the current function and return the correct error code.
Assertions are also useful as they will print out the line number that they failed on.

* `TEST_ASSERT(condition)`: Will evaluate the condition.
* `TEST_ASSERT_MESSAGE(message, condition`): Will evaluate the condition, and if on failure will also evaluate and return the result of message.

## Example test

```
int test_example(int test_crc64(int argc, char *argv[], int flags) {
    TEST_ASSERT(5 == 5);
    TEST_ASSERT_MESSAGE("This should pass", 6 == 6);
    return 0;
} 
```

## Running tests
Tests can be run by executing:

```
make unit-test
./valkey-unit-tests <file-name | test-name | file-name:test-name> --flags
```

If no arguments are specificied, by default all tests are executed.