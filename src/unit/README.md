## Valkey GoogleTest Unit Test Framework

Although GTest is a C++ framework, unit tests should avoid C++ features and
remain easily readable to C developers without requiring C++ knowledge.

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
   make test-unit UNIT_TEST_PATTERN='TEST_CLASS_NAME.*'
   ```

4. Running a subset of gtest unit tests in the test class, replace
   TEST_CLASS_NAME with expected test class name, and replace TEST_NAME_PREFIX
   with test name

   ```bash
   make test-unit UNIT_TEST_PATTERN='TEST_CLASS_NAME.TEST_NAME_PREFIX*'
   ```

5. Building and running with CMake

   ```bash
   mkdir build-release && cd $_
   cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/valkey -DBUILD_UNIT_GTESTS=yes
   make test-unit
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

* `accurate=1`: Indicates the test should use extra computation to more accurately validate the tests.
* `large_memory=1`: Indicates whether tests should use more than 100mb of memory.
* `valgrind=1`: A hint passed to tests to indicate that we are running under valgrind.
* `seed=<number>`: Sets a specific random seed for reproducible test runs. All `rand()` calls will produce the same sequence with the same seed.

Example usage:

```bash
make test-unit accurate=1 large_memory=1 seed=12345
```

## Writing C-readable tests

Valkey is a C project. Unit tests use GoogleTest (C++) as a test runner, but the
test code itself should be written in a minimal C++ style that any C developer
can read and modify without C++ knowledge.

### Guidelines

1. **No STL containers.** Use fixed-size C arrays, `sds`, or project-specific
   structures instead of `std::vector`, `std::string`, `std::set`, etc.
2. **No STL algorithms.** Use `qsort`, hand-written loops, or project utilities
   instead of `std::sort`, `std::find`, `std::min`/`std::max`.
3. **No C++ memory management.** No `new`/`delete`, smart pointers, or RAII
   wrappers. Use `zmalloc`/`zfree` or stack allocation.
4. **No classes or inheritance** (except GTest's `TEST`/`TEST_F`/`TEST_P`
   macros which require them implicitly).
5. **No templates, lambdas, or `auto`.** Declare types explicitly.
6. **No C++ casts.** Use C-style casts where necessary.
7. **No `std::string`** except where GTest infrastructure requires it (e.g.
   custom test name generators for `INSTANTIATE_TEST_SUITE_P`).

### Examples

**Bad** — requires C++ knowledge to read:

```cpp
TEST_F(ZsetTest, RangeQuery) {
    std::vector<std::string> results;
    auto iter = orderedIndexInitIterator(oi, 0, 1);
    while (auto item = orderedIndexIteratorNext(&iter)) {
        results.push_back(std::string(orderedIndexGetElementRaw(item)));
    }
    std::sort(results.begin(), results.end());
    ASSERT_EQ(results.size(), 3u);
}
```

**Good** — C developer can read this immediately:

```cpp
TEST_F(ZsetTest, RangeQuery) {
    sds results[3];
    int count = 0;
    OrderedIndexIterator iter;
    orderedIndexInitIterator(&iter, oi, 0, 1);
    OrderedIndexItem *item;
    while ((item = orderedIndexIteratorNext(&iter)) != NULL) {
        const char *ele;
        size_t len;
        orderedIndexGetElementRaw(item, &ele, &len);
        results[count++] = sdsnewlen(ele, len);
    }
    EXPECT_EQ(count, 3);
    /* sort and verify */
    qsort(results, count, sizeof(sds), sdscmpptr);
    EXPECT_STREQ(results[0], "alpha");
    /* EXPECT (not ASSERT) so cleanup below still runs on failure */
    for (int i = 0; i < count; i++) sdsfree(results[i]);
}
```

**Bad** — C++ set for deduplication:

```cpp
std::set<std::string> seen;
for (int i = 0; i < n; i++) {
    seen.insert(entries[i]);
}
ASSERT_EQ(seen.size(), expected_unique);
```

**Good** — simple sorted array + manual dedup:

```cpp
qsort(entries, n, sizeof(sds), sdscmpptr);
int unique = 1;
for (int i = 1; i < n; i++) {
    if (sdscmp(entries[i], entries[i-1]) != 0) unique++;
}
ASSERT_EQ(unique, expected_unique);
```

### Rationale

Every contributor should be able to read and modify tests without learning C++.
GTest provides the assertion macros and test registration — everything else
should look like C with `.cpp` file extension.
