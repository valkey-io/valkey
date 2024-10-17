#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import platform
import string
import random
import subprocess
import os
import signal


def RandomString(stringLength=10):
    """Generate a random string of fixed length """
    letters = string.ascii_lowercase
    return ''.join(random.choice(letters) for i in range(stringLength))


def PrintSuccCaseResult(info):
    print("\033[0;32;40m " + info + "\033[0m")


def GetAllRedisPids():
    pids = []
    try:
        command='ps aux | grep valkey-server | grep 9... | grep -v grep'
        processInfos = subprocess.check_output(command, shell=True)
    except Exception as e:
        return pids
    infos = processInfos.strip().decode('utf-8').split('\n')
    if (infos) == 0:
        return pids
    for info in infos:
        pids.append(int(info.split()[1]))
    return pids


def KillAllRedis():
    pids = GetAllRedisPids()
    if len(pids) == 0:
        return
    for pid in pids:
        os.kill(pid, signal.SIGKILL)


def StopAllRedis():
    pids = GetAllRedisPids()
    if len(pids) == 0:
        return
    for pid in pids:
        os.kill(pid, signal.SIGTERM)


def sed_helper(before, after, filepath):
    f"""
    A helper function that do "sed -i 's/{before}/{after}/g' {filepath}"
    """
    if platform.system() == "Linux":
        os.system(f"sed -i 's/{before}/{after}/g' " + filepath)
    elif platform.system() == "Darwin":
        # Mac's sed require a parameter to indicate what extension to add to the file name
        # when making a backup, like "sed -i '.bak' 's/before/after' filename" will produce
        # a backup file named "filename.bak" which is the original version. Using empty string
        # since we don't want a backup.
        os.system(f"sed -i '' 's/{before}/{after}/g' " + filepath)
    else:
        raise Exception(f"unsupported system: {platform.system()}")
