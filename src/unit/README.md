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
to 'wrappers.h' and re-run 'make test-unit' to regenerate the Valkey glue code
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

Alternatively, you can build and install GoogleTest from source:

```bash
git clone https://github.com/google/googletest.git
cd googletest
mkdir build && cd build
cmake ..
make
sudo make install
```

## Tricks in running unit tests

Sometimes the developer might want to run only one gtest unit test, or only a
subset of all unit tests for debugging. We have a few different flavors of
gtest unit tests that you can filter/play with:

1. Running all unit tests

   ```bash
   make test-unit
   ```

3. Running all unit tests in the test class, replace TEST_CLASS_NAME with
   expected test class name

   ```bash
   make valkey-unit-gtests
   ./src/unit/valkey-unit-gtests --gtest_filter='TEST_CLASS_NAME.*'
   ```

4. Running a subset of gtest unit tests in the test class, replace
   TEST_CLASS_NAME with expected test class name, and replace TEST_NAME_PREFIX
   with test name

   ```bash
   make valkey-unit-gtests
   ./src/unit/valkey-unit-gtests --gtest_filter='TEST_CLASS_NAME.TEST_NAME_PREFIX*'
   ```

5. Building and running with CMake

   ```bash
   mkdir build-release && cd $_
   cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/valkey -DBUILD_UNIT_GTESTS=yes
   make valkey-unit-gtests
   ./bin/valkey-unit-gtests
   ```

6. Running disabled tests

   GoogleTest allows tests to be disabled by prefixing the test name with `DISABLED_`. These tests are skipped during normal test runs.
   Some tests are disabled by default because they take a long time to run (e.g., 1M iterations for performance benchmarks).
   To run a specific disabled test explicitly:

   ```bash
   make valkey-unit-gtests
   ./src/unit/valkey-unit-gtests --gtest_filter=TEST_CLASS_NAME.DISABLED_TEST_NAME --gtest_also_run_disabled_tests
   ```

## Test flags

The gtest framework supports several command-line flags to control test behavior:

* `--accurate`: Indicates the test should use extra computation to more accurately validate the tests.
* `--large-memory`: Indicates whether tests should use more than 100mb of memory.
* `--valgrind`: A hint passed to tests to indicate that we are running under valgrind.
* `--seed <number>`: Sets a specific random seed for reproducible test runs. All `rand()` calls will produce the same sequence with the same seed.

Example usage:

```bash
./src/unit/valkey-unit-gtests --accurate --large-memory --seed 12345
```