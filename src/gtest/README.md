## Valkey Google Unit Test Framework
This directory contains the Google Test (gtest) framework integration for
Valkey unit testing. The framework uses Google Test and Google Mock (gmock)
to provide C++ testing capabilities. To use this framework to write unit
tests, we have modified Valkey to build as a library that can link against
other test executables.

For more information on Google Test, see: https://google.github.io/googletest/

This framework uses the GNU C++ linker, which implements 'wrap'
functionality to rename function calls to foo() to a method __wrap_foo()
and renames the real foo() method to __real_foo().

Using this trick, we define the Valkey wrappers we wish to mock in
'wrappers.h'. Note that these functions can only be mocked if they include
calls between source files.

Using this set of functions, we run 'generate-wrappers.py' to generate the
C++ glue code to hook up to googlemock. Specifically, this generates an
interface named Valkey containing all the desired methods and two
implementations, MockValkey and RealValkey.

MockValkey uses googlemock definitions to define a mock class. RealValkey
uses the __real_foo() methods to call the renamed methods. The script also
implements every __wrap_foo() command that delegates to the last MockValkey
instance initialized.

To extend the Valkey classes for mocking further methods, simply add your
method to 'wrappers.h' and re-run 'make test-gtest' to regenerate the
Valkey glue code and run the tests.

## Tricks in running unit tests
Sometimes the developer might want to run only one google unit test, or
only a subset of all unit tests for debugging. We have a few different
flavors of unit tests that you can filter/play with:

1. Running all unit tests (C unit and google unit)

   ```bash
   make test-unit
   ```

2. Running all google unit tests

   ```bash
   make test-gtest
   ```

3. Running all google unit tests in the test class, replace TEST_CLASS_NAME with
   expected test class name

   ```bash
   make valkey-unit-gtests
   ./src/gtest/valkey-unit-gtests --gtest_filter=<TEST_CLASS_NAME>
   ```

4. Running a subset of google unit tests in the test class, replace TEST_CLASS_NAME
   with expected test class name, and replace TEST_NAME_PREFIX with test name

   ```bash
   make valkey-unit-gtests
   ./src/gtest/valkey-unit-gtests --gtest_filter=<*TEST_CLASS_NAME.TEST_NAME_PREFIX>
   ```

5. Building and running with CMake

   ```bash
   mkdir build-release && cd $_
   cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/valkey -DBUILD_UNIT_GTESTS=yes
   make valkey-unit-gtests
   ./bin/valkey-unit-gtests
   ```
