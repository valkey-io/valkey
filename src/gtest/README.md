## Valkey GoogleTest Unit Test Framework

To use this framework to write unit tests, we have modified Valkey to build as
a library that can link against other test executables. This framework uses the
GNU linker (ld), which implements 'wrap' functionality to rename function calls
to foo() to a method __wrap_foo() and renames the real foo() method to
__real_foo().

Using this trick, we define the Valkey wrappers we wish to mock in 'wrappers.h'.
Note that these functions can only be mocked if they include calls between
source files.

Using this set of functions, we run 'generate-wrappers.py' to generate the glue
code needed to mock functions. Specifically, this generates an interface named
Valkey containing all the desired methods and two implementations, MockValkey
and RealValkey.

MockValkey uses gtest definitions to define a mock class. RealValkey uses the
__real_foo() methods to call the renamed methods. The script also implements
every __wrap_foo() command that delegates to the last MockValkey instance
initialized.

To extend the Valkey classes for mocking further methods, simply add your method
to 'wrappers.h' and re-run 'make test-gtest' to regenerate the Valkey glue code
and run the tests.

Important: All mocking should occur at software boundaries where interfaces are
clearly defined. Your use of mocking will be denied if it is not at a well
defined boundary. Overuse of mocking turns the unit tests into a "change
detector" which will fail whenever the code is modified. Please also consider
whether other testing strategies like injecting fakes/stubs or integration
testing would yield similar test coverage.

This framework depends on GoogleTest and GoogleMock. You need to install them manually
before building the gtests (e.g., `libgtest-dev` / `libgmock-dev` on Debian/Ubuntu,
`gtest-devel` / `gmock-devel` on CentOS/Fedora, or `brew install googletest` on macOS).

## Tricks in running unit tests

Sometimes the developer might want to run only one gtest unit test, or only a
subset of all unit tests for debugging. We have a few different flavors of
gtest unit tests that you can filter/play with:

1. Running all unit tests (C unit tests and gtest unit tests)

   ```bash
   make test-unit
   ```

2. Running all gtest unit tests

   ```bash
   make test-gtest
   ```

3. Running all gtest unit tests in the test class, replace TEST_CLASS_NAME with
   expected test class name

   ```bash
   make valkey-unit-gtests
   ./src/gtest/valkey-unit-gtests --gtest_filter=TEST_CLASS_NAME.*
   ```

4. Running a subset of gtest unit tests in the test class, replace
   TEST_CLASS_NAME with expected test class name, and replace TEST_NAME_PREFIX
   with test name

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
