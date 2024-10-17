#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import os
import sys
import time
import subprocess
import rediscluster
import redis
import signal
import shutil


class RedisCluster:
    def __init__(self, password, base_port=9000, shard_size=3, have_slave=True, diskless=False, repl_diskless_load="",
                 arbiter_size=0, slave_count=0):
        self.password = password
        self.base_port = base_port
        self.shard_size = shard_size
        self.have_slave = have_slave
        self.slave_count = slave_count
        self.diskless = diskless
        self.repl_diskless_load = repl_diskless_load
        self.arbiter_size = arbiter_size
        self.file_path = os.path.dirname(os.path.abspath(__file__))
        self.redis_server = self.file_path + "/../src/valkey-server"
        self.redis_cli = self.file_path + "/../src/valkey-cli"
        self.root_dir = self.file_path + '/tmp/' + time.strftime("%Y-%m-%d_%H-%M-%S")
        os.makedirs(self.root_dir)
        os.makedirs(self.root_dir + '/conf')
        os.makedirs(self.root_dir + '/log')
        self.masters = []  # port, nodeid, pid
        self.slaves = []
        self.arbiters = []
        self.ports = []
        self.instance_arbiter_enabled = False

    def start_cluster(self):
        self.__start_master_process()
        if self.have_slave:
            self.__start_slave_process()
        if self.arbiter_size > 0:
            self.__start_arbiter_process()
        time.sleep(1)
        self.__handshake()
        self.__add_slots()
        time.sleep(1)
        if self.have_slave:
            i = 0
            while i < 3:
                try:
                    self.__add_replicate()
                except Exception as e:
                    if "Unknown node" in str(e):
                        continue
                time.sleep(1)
                break
        if self.arbiter_size > 0:
            self.__enable_arbiter()
        self.__wait_cluster_ok()
        self.wait_cluster_join()

    # master:9000, arbiter: 9001、9002, slave:9003
    def start_ms_cluster(self):
        self.shard_size = 3
        self.__start_master_process(ms_version=True)
        if self.have_slave:
            self.__start_slave_process(ms_version=True)
        time.sleep(1)
        self.__handshake(ms_version=True)
        self.__add_slots(ms_version=True)
        time.sleep(1)
        if self.have_slave:
            self.__add_replicate()
        self.__set_arbiter()
        time.sleep(1)
        self.wait_cluster_join()

    def restart_ms_cluster(self):
        for master in self.masters:
            self.stop_redis_node(master['port'])
        for slave in self.slaves:
            self.stop_redis_node(master['port'])
        time.sleep(1)
        for master in self.masters:
            self.start_redis_node(master['port'])
        for slave in self.slaves:
            self.start_redis_node(master['port'])

    def start_ims_cluster(self):
        """
        start instance arbiter master-slave cluster
        master:9000
        instance arbiter:9001/9002/9003
        slave:9004
        """
        self.instance_arbiter_enabled = True
        self.shard_size = 4
        self.__start_master_process(ims_version=True)
        if self.have_slave:
            self.__start_slave_process(ims_version=True)
        time.sleep(1)
        self.__handshake(ims_version=True)
        self.__add_slots(ims_version=True)
        time.sleep(1)
        if self.have_slave:
            self.__add_replicate()
        self.__set_arbiter(instance_arbiter=True)
        time.sleep(1)
        self.wait_cluster_join()
        self.__wait_cluster_ok()

    def get_redis_cluster_conn(self):
        master_nodes = []
        for master in self.masters:
            master_node = {'host':'127.0.0.1', 'port': master['port']}
            master_nodes.append(master_node)
        try:
            print(master_nodes)
            if hasattr(rediscluster, "StrictRedisCluster"):
                conn = rediscluster.StrictRedisCluster(startup_nodes=master_nodes)
            else:
                # Use RedisCluster in the new version.
                conn = rediscluster.RedisCluster(startup_nodes=master_nodes)
        except Exception as e:
            print(e)
            sys.exit(1)
        return conn

    def get_master_nodes(self):
        return self.masters

    def get_slave_nodes(self):
        return self.slaves

    def signal_redis_node(self, port, sig):
        for master in self.masters:
            if master['port'] == port:
                os.kill(master['pid'], sig)
                return

        for slave in self.slaves:
            if slave['port'] == port:
                os.kill(slave['pid'], sig)
                return

    def stop_redis_node(self, port):
        self.signal_redis_node(port, signal.SIGTERM)

    def kill_redis_node(self, port):
        self.signal_redis_node(port, signal.SIGKILL)

    def add_new_node(self, myport, port): # myport表示新节点自己的端口, port表示与哪个节点握手
        process = self.start_newredis_process(myport, False)
        time.sleep(0.1)
        self.join_cluster(myport, port)
        master = {}
        master['host'] = "127.0.0.1"
        master['port'] = myport
        master['pid'] = process.pid
        master['nodeid'] = self.__get_node_id(myport)
        self.masters.append(master)
        print(self.ports)
        self.wait_cluster_join(5)
        return process.pid

    def add_new_instance_arbiter(self, myport, port, wait_join=True):
        """
        Add a new instance arbiter to the cluster.
        :param myport: The port of instance artbier.
        :param port: The port of a node in the cluster, the instance arbiter will
                     perform CLUSTER MEET on the target port node.
        :param wait_join: If true, will call wait_cluster_join to wait the node to join.
        """
        process = self.start_newredis_process(myport, False)
        conn = redis.StrictRedis(host="127.0.0.1", port=myport, password=self.password)
        conn.execute_command("cluster asinstancearbiter")
        time.sleep(0.1)
        self.join_cluster(myport, port)
        master = {}
        master["host"] = "127.0.0.1"
        master["port"] = myport
        master["pid"] = process.pid
        master["nodeid"] = self.__get_node_id(myport)
        self.masters.append(master)
        print(self.ports)
        if wait_join:
            self.wait_cluster_join(5)
        return process.pid

    def join_cluster(self, myport, port):
        if self.password != '':
            conn = redis.StrictRedis(host="127.0.0.1", port=myport, password=self.password)
        else:
            conn = redis.StrictRedis(host="127.0.0.1", port=myport)
        conn.execute_command("cluster meet 127.0.0.1 " + str(port))

    def get_nodes_conf_file(self, port):
        conf_name = self.root_dir + '/conf/' + str(port) + '-nodes.conf'
        return conf_name

    def get_redis_conf_file(self, port):
        conf_name = self.root_dir + '/conf/' + str(port) + '-redis.conf'
        return conf_name

    def start_redis_node(self, port):
        conf_name = self.root_dir + '/conf/' + str(port) + '-redis.conf'
        process = subprocess.Popen(self.redis_server + ' ' + conf_name, shell=True)
        for master in self.masters:
            if master['port'] == port:
                master['pid'] = process.pid
        for slave in self.slaves:
            if slave['port'] == port:
                slave['pid'] = process.pid

    def start_newredis_process(self, port, is_master=True):
        self.__make_node_conf(port, is_master)
        conf_name = self.root_dir + '/conf/' + str(port) + '-redis.conf'

        # When starting redis-server, an error may occur, such as port in use.
        # We give it some chance to try again.
        for _ in range(5):
            process = subprocess.Popen(self.redis_server + ' ' + conf_name, shell=True)
            polls = []
            for _ in range(5):
                poll = process.poll()
                polls.append(poll)
                if poll is not None:
                    # This means the subprocess is dead.
                    break
                time.sleep(0.1)
            if any(polls) is False:
                self.ports.append(port)
                return process

    def __get_default_conf(self, port, is_master=True):
        default_conf = {
            "enable-debug-command": "yes",
            'dir': self.root_dir,
            'port': str(port),
            'bind': '127.0.0.1',
            'save': '""',
            'dbfilename': str(port) + '-dump.rdb',
            'appendonly': 'no',
            'databases': '256',
            'cluster-enabled': 'yes',
            'cluster-node-timeout': '5000',
            'logfile': 'log/' + str(port) + '-redis.log',
            'cluster-config-file': 'conf/' + str(port) + '-nodes.conf'
        }
        if self.password:
            default_conf['requirepass'] = self.password
            default_conf['masterauth'] = self.password
        if self.diskless:
            if is_master:
                default_conf['repl-diskless-sync'] = 'yes'
            else:
                if self.repl_diskless_load != "":
                    default_conf['repl-diskless-load'] = self.repl_diskless_load
        if self.instance_arbiter_enabled:
            default_conf["cluster-instance-arbiter-enabled"] = "yes"
        return default_conf

    def __make_node_conf(self, port, is_master=True):
        conf_name = self.root_dir + '/conf/' + str(port) + '-redis.conf'
        print(conf_name)
        try:
            conf_file = open(conf_name, mode='w')
            conf = self.__get_default_conf(port, is_master)
            for key, value in conf.items():
                conf_file.write(key + ' ' + value + '\n')
            conf_file.close()
        except Exception as e:
            print(e)

    def __get_node_id(self,port):
        if self.password != '':
            cmd = self.redis_cli + " -a " + self.password
        else:
            cmd = self.redis_cli
        cmd += " -p " + str(port) + " cluster myid"
        nodeid = os.popen(cmd).read().strip()
        return nodeid

    def __handshake(self, ms_version=False, ims_version=False):
        """
        Doing CLUSTER MEET with other nodes.
        :param ms_version: master-slave version for arbiter.
        :param ims_version: instance master-slave version for instance arbiter.
        """
        if ms_version:
            node_cnt = self.shard_size + 1
        elif ims_version:
            node_cnt = self.shard_size + 1
        else:
            if self.have_slave:
                node_cnt = self.shard_size * 2
            else:
                node_cnt = self.shard_size
            if self.slave_count:
                node_cnt = self.shard_size + self.slave_count
        node_cnt += self.arbiter_size
        for idx in range(node_cnt - 1):
            port = self.base_port + idx + 1
            if self.password != '':
                conn = redis.StrictRedis(host="127.0.0.1", port=self.base_port, password=self.password)
            else:
                conn = redis.StrictRedis(host="127.0.0.1", port=self.base_port)
            conn.execute_command("cluster meet 127.0.0.1 " + str(port))

    def __add_slots(self, ms_version=False, ims_version=False):
        """
        Doing CLUSTER ADDSLOTS.
        :param ms_version: master-slave version for arbiter.
        :param ims_version: instance master-slave version for instance arbiter.
        """
        if ms_version:
            conn = redis.StrictRedis(host="127.0.0.1", port=self.base_port)
            conn.execute_command("cluster addslots " + ' '.join(str(slot) for slot in list(range(16384))))
        elif ims_version:
            conn = redis.StrictRedis(host="127.0.0.1", port=self.base_port)
            conn.execute_command("cluster addslots " + ' '.join(str(slot) for slot in list(range(16384))))
        else:
            step = int(16384 / self.shard_size)
            idx = 0
            for master in self.masters:
                if self.password != '':
                    conn = redis.StrictRedis(host="127.0.0.1", port=self.base_port + idx, password=self.password)
                else:
                    conn = redis.StrictRedis(host="127.0.0.1", port=self.base_port + idx)
                slots_list = list(range(idx*step, idx * step + step))
                slots = ' '.join(str(slot) for slot in slots_list)
                conn.execute_command("cluster addslots " + slots)
                idx = idx + 1

            remain_slots_list = list(range((idx-1)*step + step, 16384))
            if len(remain_slots_list) > 0:
                remain_slots = ' '.join(str(slot) for slot in remain_slots_list)
                if self.password != '':
                    conn = redis.StrictRedis(host="127.0.0.1", port=self.base_port + idx - 1, password=self.password)
                else:
                    conn = redis.StrictRedis(host="127.0.0.1", port=self.base_port + idx - 1)
                conn.execute_command("cluster addslots " + remain_slots)

    def __add_replicate(self):
        idx = 0
        masters_num = len(self.masters)
        for slave in self.slaves:
            if self.password != '':
                conn = redis.StrictRedis(host="127.0.0.1", port=slave['port'], password=self.password)
            else:
                conn = redis.StrictRedis(host="127.0.0.1", port=slave['port'])
            master_node_id = self.masters[idx % masters_num]['nodeid']
            conn.execute_command("cluster replicate " + master_node_id)
            idx = idx + 1

    def __wait_cluster_ok(self):
        """Dead loop waiting for the cluster_state to be ok."""
        if self.password != '':
            conn = redis.StrictRedis(host='127.0.0.1', port=self.base_port, password=self.password)
        else:
            conn = redis.StrictRedis(host='127.0.0.1', port=self.base_port)

        state = None  # cluster_state
        while state != "ok":
            print("wait cluster ok")
            info = conn.execute_command("cluster info")
            state = self.get_cluster_state(info)
            if state != "ok":
                time.sleep(0.5)

    def __start_master_process(self, ms_version=False, ims_version=False):
        """
        Start the master nodes.
        :param ms_version: master-slave version for arbiter.
        :param ims_version: instance master-slave version for instance arbiter.
        """
        if ms_version:
            # 1 master and 2 arbiter
            master_count = 3
        elif ims_version:
            # 1 master and 3 instance arbiter
            master_count = 4
        else:
            master_count = self.shard_size
        for idx in range(master_count):
            port = self.base_port + idx
            process = self.start_newredis_process(port)
            time.sleep(0.1)
            master = {}
            master['host'] = "127.0.0.1"
            master['port'] = port
            master['pid'] = process.pid
            master['nodeid'] = self.__get_node_id(port)
            self.masters.append(master)

    def __start_slave_process(self, ms_version=False, ims_version=False):
        """
        Start the slave nodes.
        :param ms_version: master-slave version for arbiter.
        :param ims_version: instance master-slave version for instance arbiter.
        """
        if ms_version:
            slave_count = 1
        elif ims_version:
            slave_count = 1
        else:
            slave_count = self.shard_size
        if self.slave_count:
            slave_count = self.slave_count
        for idx in range(slave_count):
            port = self.base_port + self.shard_size + idx
            process = self.start_newredis_process(port, False)
            time.sleep(0.1)
            slave = {}
            slave['host'] = "127.0.0.1"
            slave['port'] = port
            slave['pid'] = process.pid
            slave['nodeid'] = self.__get_node_id(port)
            self.slaves.append(slave)

    def __start_arbiter_process(self):
        for idx in range(self.arbiter_size):
            port = self.base_port + self.shard_size * 2 + idx
            if self.slave_count:
                port = self.base_port + self.shard_size + self.slave_count + idx
            process = self.start_newredis_process(port)
            time.sleep(0.1)
            arbiter = {}
            arbiter['host'] = "127.0.0.1"
            arbiter['port'] = port
            arbiter['pid'] = process.pid
            arbiter['nodeid'] = self.__get_node_id(port)
            self.arbiters.append(arbiter)

    def __enable_arbiter(self):
        for arbiter in self.arbiters:
            if self.password != '':
                conn = redis.StrictRedis(host="127.0.0.1", port=arbiter['port'], password=self.password)
            else:
                conn = redis.StrictRedis(host="127.0.0.1", port=arbiter['port'])
            conn.execute_command("cluster asarbiter")

    def __set_arbiter(self, instance_arbiter=False):
        for master in self.masters[1:]:
            if self.password != '':
                conn = redis.StrictRedis(host='127.0.0.1', port=master['port'], password=self.password)
            else:
                conn = redis.StrictRedis(host='127.0.0.1', port=master['port'])
            if not instance_arbiter:
                conn.execute_command("cluster asarbiter")
            else:
                conn.execute_command("cluster asinstancearbiter")
            time.sleep(0.1)

    def add_new_slave(self, master_port, slave_port):
        self.start_newredis_process(slave_port, False)
        time.sleep(1)
        self.join_cluster(slave_port, master_port)
        time.sleep(1)
        for master in self.masters:
            if master['port'] == master_port:
                node_id = master['nodeid']
                conn = redis.StrictRedis(host='127.0.0.1', port=slave_port, password=self.password)
                conn.execute_command("cluster replicate " + node_id)
        self.wait_cluster_join()

    def __get_config_signature(self, conn):
        config = list()
        try:
            for k, v in conn.cluster("nodes").items():
                if len(v["slots"]) == 0:
                    continue
                slot = str()

                skip = False
                for s in v["slots"]:
                    slot += ",".join(s)
                    if '<' in s:
                        # Ignore the slot that in importing mode
                        skip = True
                        break
                if skip:
                    continue
                config.append("{0:s}:{1:s}".format(k, slot))
            return "|".join(sorted(config))
        except Exception as e:
            print(e)

    def wait_cluster_join(self, timeout=60):
        conns = []
        for port in self.ports:
            if self.password != '':
                conn = redis.StrictRedis(host='127.0.0.1', port=port, decode_responses=True, password=self.password)
            else:
                conn = redis.StrictRedis(host='127.0.0.1', port=port, decode_responses=True)
            conns.append(conn)

        st = time.time()
        while (time.time() - st) < timeout:
            print('wait cluster join')
            signs = set()
            for conn in conns:
                signs.add(self.__get_config_signature(conn))
            if len(signs) == 1:
                break
            else:
                time.sleep(1)

    def cleanup(self):
        """
        Doing some cleanup when we are done testing.
        - Clean up temporary files generated during the test process, such as data, logs, etc.
        """
        print("rmtree %s" % self.root_dir)
        shutil.rmtree(self.root_dir)

    @classmethod
    def get_cluster_state(cls, info):
        """Helper function to get cluster_state from cluster info."""
        state = None
        if isinstance(info, dict):
            state = info["cluster_state"]
        elif isinstance(info, str):
            state = info.split("\r\n")[0].split(":")[1]
        elif isinstance(info, bytes):
            state = info.decode('utf-8').split("\r\n")[0].split(":")[1]
        return state

    @classmethod
    def get_cluster_known_nodes(cls, info):
        """Helper function to get cluster_known_nodes from cluster info."""
        num = 0
        if isinstance(info, dict):
            num = info["cluster_known_nodes"]
        elif isinstance(info, str):
            num = info.split("\r\n")[5].split(":")[1]
        elif isinstance(info, bytes):
            num = info.decode('utf-8').split("\r\n")[5].split(":")[1]
        return int(num)

    @classmethod
    def get_instance_arbiter_enabled(cls, info):
        enabled = False
        if isinstance(info, dict):
            enabled = info["instance_arbiter_enabled"]
        elif isinstance(info, str):
            enabled = info.split("\r\n")[8].split(":")[1]
        elif isinstance(info, bytes):
            enabled = info.decode('utf-8').split("\r\n")[8].split(":")[1]
        return bool(int(enabled))

    @classmethod
    def get_instance_arbiter_size(cls, info):
        size = 0
        if isinstance(info, dict):
            size = info["instance_arbiter_size"]
        elif isinstance(info, str):
            size = info.split("\r\n")[7].split(":")[1]
        elif isinstance(info, bytes):
            size = info.decode('utf-8').split("\r\n")[7].split(":")[1]
        return int(size)

    def wait_for_sync(self, conn, maxtries=10, delay=1):
        """ A wait waits for the master_link_status of the connection to be up. """
        while maxtries >= 0:
            info = conn.execute_command("info replication")
            if "master_link_status:up" in str(info):
                return
            else:
                time.sleep(delay)
                maxtries -= 1
