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
import collections
import threading
import time


class LoggerMock(object):
  def __init__(self, test_lib):
    self.test_lib = test_lib
    self.runtimes = collections.defaultdict(list)
    self.exit_codes = collections.defaultdict(list)
    self.last_execution_times = collections.defaultdict(list)
    self.execution_numbers = collections.defaultdict(list)

  def log_exit(self, task):
    self.runtimes[task.test_id].append(task.runtime_ms)
    self.exit_codes[task.test_id].append(task.exit_code)
    self.last_execution_times[task.test_id].append(task.last_execution_time)
    self.execution_numbers[task.test_id].append(task.execution_number)

  def assertRecorded(self, test_id, expected, retries):
    self.test_lib.assertIn(test_id, self.runtimes)
    self.test_lib.assertListEqual(expected['runtime_ms'][:retries],
                                  self.runtimes[test_id])
    self.test_lib.assertListEqual(expected['exit_code'][:retries],
                                  self.exit_codes[test_id])
    self.test_lib.assertListEqual(expected['last_execution_time'][:retries],
                                  self.last_execution_times[test_id])
    self.test_lib.assertListEqual(expected['execution_number'][:retries],
                                  self.execution_numbers[test_id])


class TestTimesMock(object):
  def __init__(self, test_lib, test_data=None):
    self.test_lib = test_lib
    self.test_data = test_data or {}
    self.last_execution_times = collections.defaultdict(list)

  def record_test_time(self, test_binary, test_name, last_execution_time):
    test_id = (test_binary, test_name)
    self.last_execution_times[test_id].append(last_execution_time)

  def get_test_time(self, test_binary, test_name):
    test_group, test = test_name.split('.')
    return self.test_data.get(test_binary, {}).get(test_group,
                                                   {}).get(test, None)

  def assertRecorded(self, test_id, expected, retries):
    self.test_lib.assertIn(test_id, self.last_execution_times)
    self.test_lib.assertListEqual(expected['last_execution_time'][:retries],
                                  self.last_execution_times[test_id])


class TestResultsMock(object):
  def __init__(self, test_lib):
    self.results = []
    self.test_lib = test_lib

  def log(self, test_name, runtime_ms, actual_result):
    self.results.append((test_name, runtime_ms, actual_result))

  def assertRecorded(self, test_id, expected, retries):
    test_results = [
        (test_id[1], runtime_ms / 1000.0, exit_code)
        for runtime_ms, exit_code in zip(expected['runtime_ms'][:retries],
                                         expected['exit_code'][:retries])
    ]
    for test_result in test_results:
      self.test_lib.assertIn(test_result, self.results)


class TaskManagerMock(object):
  def __init__(self):
    self.running_groups = []
    self.check_lock = threading.Lock()

    self.had_running_parallel_groups = False
    self.total_tasks_run = 0
    self.started = {}

  def __register_start(self, task):
    self.started[task.task_id] = task

  def register_exit(self, task):
    self.started.pop(task.task_id)

  def run_task(self, task, timeout_per_test):
    self.__register_start(task)
    test_group = task.test_name.split('.')[0]

    with self.check_lock:
      self.total_tasks_run += 1
      if test_group in self.running_groups:
        self.had_running_parallel_groups = True
      self.running_groups.append(test_group)

    # Delay as if real test were run.
    time.sleep(0.001)

    with self.check_lock:
      self.running_groups.remove(test_group)


class TaskMockFactory(object):
  def __init__(self, test_data):
    self.data = test_data
    self.passed = []
    self.failed = []

  def get_task(self, test_id, execution_number=0):
    task = TaskMock(test_id, execution_number, self.data[test_id])
    if task.exit_code == 0:
      self.passed.append(task)
    else:
      self.failed.append(task)
    return task

  def __call__(self, test_binary, test_name, test_command, execution_number,
               last_execution_time, output_dir):
    return self.get_task((test_binary, test_name), execution_number)


class TaskMock(object):
  def __init__(self, test_id, execution_number, test_data):
    self.test_id = test_id
    self.execution_number = execution_number

    self.runtime_ms = test_data['runtime_ms'][execution_number]
    self.exit_code = test_data['exit_code'][execution_number]
    self.last_execution_time = (
        test_data['last_execution_time'][execution_number])
    if 'log_file' in test_data:
      self.log_file = test_data['log_file'][execution_number]
    else:
      self.log_file = None
    self.test_command = None
    self.output_dir = None

    self.test_binary = test_id[0]
    self.test_name = test_id[1]
    self.task_id = (test_id[0], test_id[1], execution_number)

  def run(self, timeout_per_test):
    pass


class SubprocessMock(object):
  def __init__(self, test_data=None):
    self._test_data = test_data
    self.last_invocation = None

  def __call__(self, command, **kwargs):
    self.last_invocation = command
    binary = command[0]
    test_list = []
    tests_for_binary = sorted(self._test_data.get(binary, {}).items())
    for test_group, tests in tests_for_binary:
      test_list.append(test_group + ".")
      for test in sorted(tests):
        test_list.append("  " + test)
    return '\n'.join(test_list)
