#!/usr/bin/env python
# Copyright 2017 Google Inc. All rights reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import contextlib
import os.path
import random
import shutil
import sys
import tempfile
import threading
import unittest

import gtest_parallel

from gtest_parallel_mocks import LoggerMock
from gtest_parallel_mocks import SubprocessMock
from gtest_parallel_mocks import TestTimesMock
from gtest_parallel_mocks import TestResultsMock
from gtest_parallel_mocks import TaskManagerMock
from gtest_parallel_mocks import TaskMockFactory
from gtest_parallel_mocks import TaskMock


@contextlib.contextmanager
def guard_temp_dir():
  try:
    temp_dir = tempfile.mkdtemp()
    yield temp_dir
  finally:
    shutil.rmtree(temp_dir)


@contextlib.contextmanager
def guard_temp_subdir(temp_dir, *path):
  assert path, 'Path should not be empty'

  try:
    temp_subdir = os.path.join(temp_dir, *path)
    os.makedirs(temp_subdir)
    yield temp_subdir
  finally:
    shutil.rmtree(os.path.join(temp_dir, path[0]))


@contextlib.contextmanager
def guard_patch_module(import_name, new_val):
  def patch(module, names, val):
    if len(names) == 1:
      old = getattr(module, names[0])
      setattr(module, names[0], val)
      return old
    else:
      return patch(getattr(module, names[0]), names[1:], val)

  try:
    old_val = patch(gtest_parallel, import_name.split('.'), new_val)
    yield old_val
  finally:
    patch(gtest_parallel, import_name.split('.'), old_val)


class TestTaskManager(unittest.TestCase):
  def setUp(self):
    self.passing_task = (('fake_binary', 'Fake.PassingTest'), {
        'runtime_ms': [10],
        'exit_code': [0],
        'last_execution_time': [10],
    })
    self.failing_task = (('fake_binary', 'Fake.FailingTest'), {
        'runtime_ms': [20, 30, 40],
        'exit_code': [1, 1, 1],
        'last_execution_time': [None, None, None],
    })
    self.fails_once_then_succeeds = (('another_binary', 'Fake.Test.FailOnce'), {
        'runtime_ms': [21, 22],
        'exit_code': [1, 0],
        'last_execution_time': [None, 22],
    })
    self.fails_twice_then_succeeds = (('yet_another_binary',
                                       'Fake.Test.FailTwice'), {
                                           'runtime_ms': [23, 25, 24],
                                           'exit_code': [1, 1, 0],
                                           'last_execution_time':
                                           [None, None, 24],
                                       })

  def execute_tasks(self, tasks, retries, expected_exit_code):
    repeat = 1

    times = TestTimesMock(self)
    logger = LoggerMock(self)
    test_results = TestResultsMock(self)

    task_mock_factory = TaskMockFactory(dict(tasks))
    task_manager = gtest_parallel.TaskManager(times, logger, test_results,
                                              task_mock_factory, retries,
                                              repeat)

    for test_id, expected in tasks:
      task = task_mock_factory.get_task(test_id)
      task_manager.run_task(task, timeout_per_test=30)
      expected['execution_number'] = list(range(len(expected['exit_code'])))

      logger.assertRecorded(test_id, expected, retries + 1)
      times.assertRecorded(test_id, expected, retries + 1)
      test_results.assertRecorded(test_id, expected, retries + 1)

    self.assertEqual(len(task_manager.started), 0)
    self.assertListEqual(
        sorted(task.task_id for task in task_manager.passed),
        sorted(task.task_id for task in task_mock_factory.passed))
    self.assertListEqual(
        sorted(task.task_id for task in task_manager.failed),
        sorted(task.task_id for task in task_mock_factory.failed))

    self.assertEqual(task_manager.global_exit_code, expected_exit_code)

  def test_passing_task_succeeds(self):
    self.execute_tasks(tasks=[self.passing_task],
                       retries=0,
                       expected_exit_code=0)

  def test_failing_task_fails(self):
    self.execute_tasks(tasks=[self.failing_task],
                       retries=0,
                       expected_exit_code=1)

  def test_failing_task_fails_even_with_retries(self):
    self.execute_tasks(tasks=[self.failing_task],
                       retries=2,
                       expected_exit_code=1)

  def test_executing_passing_and_failing_fails(self):
    # Executing both a faling test and a passing one should make gtest-parallel
    # fail, no matter if the failing task is run first or last.
    self.execute_tasks(tasks=[self.failing_task, self.passing_task],
                       retries=2,
                       expected_exit_code=1)

    self.execute_tasks(tasks=[self.passing_task, self.failing_task],
                       retries=2,
                       expected_exit_code=1)

  def test_task_succeeds_with_one_retry(self):
    # Executes test and retries once. The first run should fail and the second
    # succeed, so gtest-parallel should succeed.
    self.execute_tasks(tasks=[self.fails_once_then_succeeds],
                       retries=1,
                       expected_exit_code=0)

  def test_task_fails_with_one_retry(self):
    # Executes test and retries once, not enough for the test to start passing,
    # so gtest-parallel should return an error.
    self.execute_tasks(tasks=[self.fails_twice_then_succeeds],
                       retries=1,
                       expected_exit_code=1)

  def test_runner_succeeds_when_all_tasks_eventually_succeeds(self):
    # Executes the test and retries twice. One test should pass in the first
    # attempt, another should take two runs, and the last one should take three
    # runs. All tests should succeed, so gtest-parallel should succeed too.
    self.execute_tasks(tasks=[
        self.passing_task, self.fails_once_then_succeeds,
        self.fails_twice_then_succeeds
    ],
                       retries=2,
                       expected_exit_code=0)


class TestSaveFilePath(unittest.TestCase):
  class StreamMock(object):
    def write(*args):
      # Suppress any output.
      pass

  def test_get_save_file_path_unix(self):
    with guard_temp_dir() as temp_dir, \
        guard_patch_module('os.path.expanduser', lambda p: temp_dir), \
        guard_patch_module('sys.stderr', TestSaveFilePath.StreamMock()), \
        guard_patch_module('sys.platform', 'darwin'):
      with guard_patch_module('os.environ', {}), \
          guard_temp_subdir(temp_dir, '.cache'):
        self.assertEqual(os.path.join(temp_dir, '.cache', 'gtest-parallel'),
                         gtest_parallel.get_save_file_path())

      with guard_patch_module('os.environ', {'XDG_CACHE_HOME': temp_dir}):
        self.assertEqual(os.path.join(temp_dir, 'gtest-parallel'),
                         gtest_parallel.get_save_file_path())

      with guard_patch_module('os.environ',
                              {'XDG_CACHE_HOME': os.path.realpath(__file__)}):
        self.assertEqual(os.path.join(temp_dir, '.gtest-parallel-times'),
                         gtest_parallel.get_save_file_path())

  def test_get_save_file_path_win32(self):
    with guard_temp_dir() as temp_dir, \
        guard_patch_module('os.path.expanduser', lambda p: temp_dir), \
        guard_patch_module('sys.stderr', TestSaveFilePath.StreamMock()), \
        guard_patch_module('sys.platform', 'win32'):
      with guard_patch_module('os.environ', {}), \
          guard_temp_subdir(temp_dir, 'AppData', 'Local'):
        self.assertEqual(
            os.path.join(temp_dir, 'AppData', 'Local', 'gtest-parallel'),
            gtest_parallel.get_save_file_path())

      with guard_patch_module('os.environ', {'LOCALAPPDATA': temp_dir}):
        self.assertEqual(os.path.join(temp_dir, 'gtest-parallel'),
                         gtest_parallel.get_save_file_path())

      with guard_patch_module('os.environ',
                              {'LOCALAPPDATA': os.path.realpath(__file__)}):
        self.assertEqual(os.path.join(temp_dir, '.gtest-parallel-times'),
                         gtest_parallel.get_save_file_path())


class TestSerializeTestCases(unittest.TestCase):
  def _execute_tasks(self, max_number_of_test_cases,
                     max_number_of_tests_per_test_case, max_number_of_repeats,
                     max_number_of_workers, timeout_per_test, serialize_test_cases):
    tasks = []
    for test_case in range(max_number_of_test_cases):
      for test_name in range(max_number_of_tests_per_test_case):
        # All arguments for gtest_parallel.Task except for test_name are fake.
        test_name = 'TestCase{}.test{}'.format(test_case, test_name)

        for execution_number in range(random.randint(1, max_number_of_repeats)):
          tasks.append(
              gtest_parallel.Task('path/to/binary', test_name,
                                  ['path/to/binary', '--gtest_filter=*'],
                                  execution_number + 1, None, 'path/to/output'))

    expected_tasks_number = len(tasks)

    task_manager = TaskManagerMock()

    gtest_parallel.execute_tasks(tasks, max_number_of_workers, task_manager,
                                 None, timeout_per_test, serialize_test_cases)

    self.assertEqual(serialize_test_cases,
                     not task_manager.had_running_parallel_groups)
    self.assertEqual(expected_tasks_number, task_manager.total_tasks_run)

  def test_running_parallel_test_cases_without_repeats(self):
    self._execute_tasks(max_number_of_test_cases=4,
                        max_number_of_tests_per_test_case=32,
                        max_number_of_repeats=1,
                        max_number_of_workers=16,
                        timeout_per_test=30,
                        serialize_test_cases=True)

  def test_running_parallel_test_cases_with_repeats(self):
    self._execute_tasks(max_number_of_test_cases=4,
                        max_number_of_tests_per_test_case=32,
                        max_number_of_repeats=4,
                        max_number_of_workers=16,
                        timeout_per_test=30,
                        serialize_test_cases=True)

  def test_running_parallel_tests(self):
    self._execute_tasks(max_number_of_test_cases=4,
                        max_number_of_tests_per_test_case=128,
                        max_number_of_repeats=1,
                        max_number_of_workers=16,
                        timeout_per_test=30,
                        serialize_test_cases=False)


class TestTestTimes(unittest.TestCase):
  def test_race_in_test_times_load_save(self):
    max_number_of_workers = 8
    max_number_of_read_write_cycles = 64
    test_times_file_name = 'test_times.pickle'

    def start_worker(save_file):
      def test_times_worker():
        thread_id = threading.current_thread().ident
        path_to_binary = 'path/to/binary' + hex(thread_id)

        for cnt in range(max_number_of_read_write_cycles):
          times = gtest_parallel.TestTimes(save_file)

          threads_test_times = [
              binary for (binary, _) in times._TestTimes__times.keys()
              if binary.startswith(path_to_binary)
          ]

          self.assertEqual(cnt, len(threads_test_times))

          times.record_test_time('{}-{}'.format(path_to_binary, cnt),
                                 'TestFoo.testBar', 1000)

          times.write_to_file(save_file)

        self.assertEqual(
            1000,
            times.get_test_time('{}-{}'.format(path_to_binary, cnt),
                                'TestFoo.testBar'))
        self.assertIsNone(
            times.get_test_time('{}-{}'.format(path_to_binary, cnt), 'baz'))

      t = threading.Thread(target=test_times_worker)
      t.start()
      return t

    with guard_temp_dir() as temp_dir:
      try:
        workers = [
            start_worker(os.path.join(temp_dir, test_times_file_name))
            for _ in range(max_number_of_workers)
        ]
      finally:
        for worker in workers:
          worker.join()


class TestTimeoutTestCases(unittest.TestCase):
  def test_task_timeout(self):
    timeout = 1
    pool_size = 1
    timeout_per_test = 30
    task = gtest_parallel.Task('test_binary', 'test_name', ['test_command'], 1,
                               None, 'output_dir')
    tasks = [task]

    task_manager = TaskManagerMock()
    gtest_parallel.execute_tasks(tasks, pool_size, task_manager, timeout, timeout_per_test, True)

    self.assertEqual(1, task_manager.total_tasks_run)
    self.assertEqual(None, task.exit_code)
    self.assertEqual(1000, task.runtime_ms)


class TestTask(unittest.TestCase):
  def test_log_file_names(self):
    def root():
      return 'C:\\' if sys.platform == 'win32' else '/'

    self.assertEqual(os.path.join('.', 'bin-Test_case-100.log'),
                     gtest_parallel.Task._logname('.', 'bin', 'Test.case', 100))

    self.assertEqual(
        os.path.join('..', 'a', 'b', 'bin-Test_case_2-1.log'),
        gtest_parallel.Task._logname(os.path.join('..', 'a', 'b'),
                                     os.path.join('..', 'bin'), 'Test.case/2',
                                     1))

    self.assertEqual(
        os.path.join('..', 'a', 'b', 'bin-Test_case_2-5.log'),
        gtest_parallel.Task._logname(os.path.join('..', 'a', 'b'),
                                     os.path.join(root(), 'c', 'd', 'bin'),
                                     'Test.case/2', 5))

    self.assertEqual(
        os.path.join(root(), 'a', 'b', 'bin-Instantiation_Test_case_2-3.log'),
        gtest_parallel.Task._logname(os.path.join(root(), 'a', 'b'),
                                     os.path.join('..', 'c', 'bin'),
                                     'Instantiation/Test.case/2', 3))

    self.assertEqual(
        os.path.join(root(), 'a', 'b', 'bin-Test_case-1.log'),
        gtest_parallel.Task._logname(os.path.join(root(), 'a', 'b'),
                                     os.path.join(root(), 'c', 'd', 'bin'),
                                     'Test.case', 1))

  def test_logs_to_temporary_files_without_output_dir(self):
    log_file = gtest_parallel.Task._logname(None, None, None, None)
    self.assertEqual(tempfile.gettempdir(), os.path.dirname(log_file))
    os.remove(log_file)

  def _execute_run_test(self, run_test_body, interrupt_test):
    def popen_mock(*_args, **_kwargs):
      return None

    class SigHandlerMock(object):
      class ProcessWasInterrupted(Exception):
        pass

      def wait(*_args):
        if interrupt_test:
          raise SigHandlerMock.ProcessWasInterrupted()

        return 42

    with guard_temp_dir() as temp_dir, \
        guard_patch_module('subprocess.Popen', popen_mock), \
        guard_patch_module('sigint_handler', SigHandlerMock()), \
        guard_patch_module('thread.exit', lambda: None):
      run_test_body(temp_dir)

  def test_run_normal_task(self):
    def run_test(temp_dir):
      task = gtest_parallel.Task('fake/binary', 'test', ['fake/binary'], 1,
                                 None, temp_dir)

      self.assertFalse(os.path.isfile(task.log_file))

      task.run(timeout_per_test=30)

      self.assertTrue(os.path.isfile(task.log_file))
      self.assertEqual(42, task.exit_code)

    self._execute_run_test(run_test, False)

  def test_run_interrupted_task_with_transient_log(self):
    def run_test(_):
      task = gtest_parallel.Task('fake/binary', 'test', ['fake/binary'], 1,
                                 None, None)

      self.assertTrue(os.path.isfile(task.log_file))

      task.run(timeout_per_test=30)

      self.assertTrue(os.path.isfile(task.log_file))
      self.assertIsNone(task.exit_code)

    self._execute_run_test(run_test, True)


class TestFilterFormat(unittest.TestCase):
  def _execute_test(self, test_body, drop_output):
    class StdoutMock(object):
      def isatty(*_args):
        return False

      def write(*args):
        pass

      def flush(*args):
        pass

    with guard_temp_dir() as temp_dir, \
        guard_patch_module('sys.stdout', StdoutMock()):
      logger = gtest_parallel.FilterFormat(None if drop_output else temp_dir)
      logger.log_tasks(42)

      test_body(logger)

      logger.flush()

  def test_no_output_dir(self):
    def run_test(logger):
      passed = [
          TaskMock(
              ('fake/binary', 'FakeTest'), 0, {
                  'runtime_ms': [10],
                  'exit_code': [0],
                  'last_execution_time': [10],
                  'log_file': [os.path.join(tempfile.gettempdir(), 'fake.log')]
              })
      ]

      open(passed[0].log_file, 'w').close()
      self.assertTrue(os.path.isfile(passed[0].log_file))

      logger.log_exit(passed[0])

      self.assertFalse(os.path.isfile(passed[0].log_file))

      logger.print_tests('', passed, print_try_number=True, print_test_command=True)
      logger.move_to(None, passed)

      logger.summarize(passed, [], [])

    self._execute_test(run_test, True)

  def test_with_output_dir(self):
    def run_test(logger):
      failed = [
          TaskMock(
              ('fake/binary', 'FakeTest'), 0, {
                  'runtime_ms': [10],
                  'exit_code': [1],
                  'last_execution_time': [10],
                  'log_file': [os.path.join(logger.output_dir, 'fake.log')]
              })
      ]

      open(failed[0].log_file, 'w').close()
      self.assertTrue(os.path.isfile(failed[0].log_file))

      logger.log_exit(failed[0])

      self.assertTrue(os.path.isfile(failed[0].log_file))

      logger.print_tests('', failed, print_try_number=True, print_test_command=True)
      logger.move_to('failed', failed)

      self.assertFalse(os.path.isfile(failed[0].log_file))
      self.assertTrue(
          os.path.isfile(os.path.join(logger.output_dir, 'failed', 'fake.log')))

      logger.summarize([], failed, [])

    self._execute_test(run_test, False)


class TestFindTests(unittest.TestCase):
  ONE_DISABLED_ONE_ENABLED_TEST = {
      "fake_unittests": {
          "FakeTest": {
              "Test1": None,
              "DISABLED_Test2": None,
          }
      }
  }
  ONE_FAILED_ONE_PASSED_TEST = {
      "fake_unittests": {
          "FakeTest": {
              # Failed (and new) tests have no recorded runtime.
              "FailedTest": None,
              "Test": 1,
          }
      }
  }
  ONE_TEST = {
      "fake_unittests": {
          "FakeTest": {
              "TestSomething": None,
          }
      }
  }
  MULTIPLE_BINARIES_MULTIPLE_TESTS_ONE_FAILURE = {
      "fake_unittests": {
          "FakeTest": {
              "TestSomething": None,
              "TestSomethingElse": 2,
          },
          "SomeOtherTest": {
              "YetAnotherTest": 3,
          },
      },
      "fake_tests": {
          "Foo": {
              "Bar": 4,
              "Baz": 4,
          }
      }
  }

  def _process_options(self, options):
    parser = gtest_parallel.default_options_parser()
    options, binaries = parser.parse_args(options)
    self.assertEqual(len(binaries), 0)
    return options

  def _call_find_tests(self, test_data, options=None):
    subprocess_mock = SubprocessMock(test_data)
    options = self._process_options(options or [])
    with guard_patch_module('subprocess.check_output', subprocess_mock):
      tasks = gtest_parallel.find_tests(test_data.keys(), [], options,
                                        TestTimesMock(self, test_data))
    # Clean transient tasks' log files created because
    # by default now output_dir is None.
    for task in tasks:
      if os.path.isfile(task.log_file):
        os.remove(task.log_file)
    return tasks, subprocess_mock

  def test_tasks_are_sorted(self):
    tasks, _ = self._call_find_tests(
        self.MULTIPLE_BINARIES_MULTIPLE_TESTS_ONE_FAILURE)
    self.assertEqual([task.last_execution_time for task in tasks],
                     [None, 4, 4, 3, 2])

  def test_does_not_run_disabled_tests_by_default(self):
    tasks, subprocess_mock = self._call_find_tests(
        self.ONE_DISABLED_ONE_ENABLED_TEST)
    self.assertEqual(len(tasks), 1)
    self.assertFalse("DISABLED_" in tasks[0].test_name)
    self.assertNotIn("--gtest_also_run_disabled_tests",
                     subprocess_mock.last_invocation)

  def test_runs_disabled_tests_when_asked(self):
    tasks, subprocess_mock = self._call_find_tests(
        self.ONE_DISABLED_ONE_ENABLED_TEST, ['--gtest_also_run_disabled_tests'])
    self.assertEqual(len(tasks), 2)
    self.assertEqual(sorted([task.test_name for task in tasks]),
                     ["FakeTest.DISABLED_Test2", "FakeTest.Test1"])
    self.assertIn("--gtest_also_run_disabled_tests",
                  subprocess_mock.last_invocation)

  def test_runs_failed_tests_by_default(self):
    tasks, _ = self._call_find_tests(self.ONE_FAILED_ONE_PASSED_TEST)
    self.assertEqual(len(tasks), 2)
    self.assertEqual(sorted([task.test_name for task in tasks]),
                     ["FakeTest.FailedTest", "FakeTest.Test"])
    self.assertEqual({task.last_execution_time for task in tasks}, {None, 1})

  def test_runs_only_failed_tests_when_asked(self):
    tasks, _ = self._call_find_tests(self.ONE_FAILED_ONE_PASSED_TEST,
                                     ['--failed'])
    self.assertEqual(len(tasks), 1)
    self.assertEqual(tasks[0].test_binary, "fake_unittests")
    self.assertEqual(tasks[0].test_name, "FakeTest.FailedTest")
    self.assertIsNone(tasks[0].last_execution_time)

  def test_does_not_apply_gtest_filter_by_default(self):
    _, subprocess_mock = self._call_find_tests(self.ONE_TEST)
    self.assertFalse(
        any(
            arg.startswith('--gtest_filter=SomeFilter')
            for arg in subprocess_mock.last_invocation))

  def test_applies_gtest_filter(self):
    _, subprocess_mock = self._call_find_tests(self.ONE_TEST,
                                               ['--gtest_filter=SomeFilter'])
    self.assertIn('--gtest_filter=SomeFilter', subprocess_mock.last_invocation)

  def test_applies_gtest_color_by_default(self):
    tasks, _ = self._call_find_tests(self.ONE_TEST)
    self.assertEqual(len(tasks), 1)
    self.assertIn('--gtest_color=yes', tasks[0].test_command)

  def test_applies_gtest_color(self):
    tasks, _ = self._call_find_tests(self.ONE_TEST, ['--gtest_color=Lemur'])
    self.assertEqual(len(tasks), 1)
    self.assertIn('--gtest_color=Lemur', tasks[0].test_command)

  def test_repeats_tasks_once_by_default(self):
    tasks, _ = self._call_find_tests(self.ONE_TEST)
    self.assertEqual(len(tasks), 1)

  def test_repeats_tasks_multiple_times(self):
    tasks, _ = self._call_find_tests(self.ONE_TEST, ['--repeat=3'])
    self.assertEqual(len(tasks), 3)
    # Test all tasks have the same test_name, test_binary and test_command
    all_tasks_set = set(
        (task.test_name, task.test_binary, tuple(task.test_command))
        for task in tasks)
    self.assertEqual(len(all_tasks_set), 1)
    # Test tasks have consecutive execution_numbers starting from 1
    self.assertEqual(sorted(task.execution_number for task in tasks), [1, 2, 3])

  def test_gtest_list_tests_fails(self):
    def exit_mock(*args):
      raise AssertionError('Foo')

    options = self._process_options([])
    with guard_patch_module('sys.exit', exit_mock):
      self.assertRaises(AssertionError, gtest_parallel.find_tests,
                        [sys.executable], [], options, None)


if __name__ == '__main__':
  unittest.main()
