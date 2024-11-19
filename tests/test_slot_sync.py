import redis
import rediscluster
import time
import unittest

import cluster
import util

util.KillAllRedis()
time.sleep(1)


def write_data():
    slot_a = 15495  # (9002)     {a} {d} {e}
    slot_b = 3300  # (9000)
    slot_c = 7365  # (9001)
    nodes = [{'host': '127.0.0.1', 'port': 9000},
             {'host': '127.0.0.1', 'port': 9001},
             {'host': '127.0.0.1', 'port': 9002}]
    if hasattr(rediscluster, "StrictRedisCluster"):
        conn = rediscluster.StrictRedisCluster(startup_nodes=nodes)
    elif hasattr(rediscluster, "RedisCluster"):
        # Use RedisCluster in the new version.
        conn = rediscluster.RedisCluster(startup_nodes=nodes)
    else:
        nodes = [redis.cluster.ClusterNode(node["host"], node["port"]) for node in nodes]
        conn = redis.cluster.RedisCluster(startup_nodes=nodes)
    for idx in range(0, 3):
        key = "{a}" + str(idx)
        value = "value-a" + str(idx)
        conn.set(key, value)
        key = "{b}" + str(idx)
        value = "value-b" + str(idx)
        conn.set(key, value)
        key = "{c}" + str(idx)
        value = "value-c" + str(idx)
        conn.set(key, value)
        key = "{d}" + str(idx)
        value = "value-d" + str(idx)
        conn.set(key, value)
        key = "{e}" + str(idx)
        value = "value-e" + str(idx)
        conn.set(key, value)


def wait_slotlink_status(conn, wait_count, wait_info, timeout):
    i = 0
    while timeout <= 0 or i < timeout:
        linkinfo_list = conn.cluster('slotlink list')
        if len(linkinfo_list) == wait_count:
            # Count the number of ready slotlink.
            ready_count = 0
            for linkinfo in linkinfo_list:
                if wait_info in linkinfo:
                    ready_count += 1
                else:
                    break

            # We can return directly if all the slotlinks are ready now.
            if ready_count == wait_count:
                print(linkinfo_list)
                return True

        # Some of the slotlinks are not ready, we need to retry.
        if i % 2 == 0:
            print(linkinfo_list)
        time.sleep(1)
        i += 1
    return False


def wait_slotlink_connected(conn, wait_count, timeout):
    return wait_slotlink_status(conn, wait_count, "state:connected", timeout)


def wait_slotlink_nolag(conn, wait_count, timeout):
    return wait_slotlink_status(conn, wait_count, "state:connected lag:0", timeout)


class TestSlotSync(unittest.TestCase):

    def setUp(self):
        util.KillAllRedis()
        util.PrintSuccCaseResult(str(redis.VERSION))

    def test_case01(self):
        # =================== TestCase 1 =============================
        # cluster slotsync命令接口，参数异常检验
        # ============================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        # 2. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(1)
        # 3. cluster slotsync命令后不带slot参数、slot参数不是2的倍数、slot不在[0~16383]范围内,slot跨节点,已下线的slot
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        try:
            nodes = conn.cluster('slotsync')
        except Exception as e:
            assert "wrong number of arguments" in str(e)
        try:
            nodes = conn.cluster('slotsync 0')
        except Exception as e:
            assert "wrong number of arguments" in str(e)
        try:
            nodes = conn.cluster('slotsync 16385 16388')
        except Exception as e:
            assert str(e) == "Invalid or out of range slot"
        try:
            nodes = conn.cluster('slotsync 16380 16388')
        except Exception as e:
            assert str(e) == "Invalid or out of range slot"
        try:
            nodes = conn.cluster('slotsync 0 16383')
        except Exception as e:
            assert "cross node" in str(e)
        conn.cluster('delslots 0')
        try:
            nodes = conn.cluster('slotsync 0 1')
        except Exception as e:
            assert "Slot 0 has no node served" in str(e)
        # 4. 同步自身已经分配的slot
        # conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # try:
        #     nodes = conn.cluster('slotsyncforce 0 1')
        # except Exception as e:
        #     assert "0 is served by my" in str(e)
        # 5. slave节点执行slotsync
        conn = redis.StrictRedis(host='127.0.0.1', port=9003, decode_responses=True)
        try:
            nodes = conn.cluster('slotsync 0 0')
        except Exception as e:
            assert "Myself should be a primary" in str(e)
        # 5. 在已分配slot的master节点上执行slotsync
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        try:
            nodes = conn.cluster('slotsync 0 0')
        except Exception as e:
            assert "Slot 0 is served by myself" in str(e)
        # 6. slotlink 命令语法检查
        try:
            conn.cluster('slotlink xxx')
        except Exception as e:
            assert str(e) == "syntax error"
        try:
            conn.cluster('slotlink list xxx')
        except Exception as e:
            assert str(e) == "syntax error"
        try:
            conn.cluster('slotlink kill')
        except Exception as e:
            assert str(e) == "syntax error"
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 1: OK")

    def test_case02(self):
        # =================== TestCase 2 =============================
        # cluster slotsync命令重复执行
        # ============================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='1234567', shard_size=3)
        redis_cluster.start_cluster()
        # 2. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(0.5)
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, password='1234567', decode_responses=True)
        conn.cluster('slotsync 0 1')
        try:
            conn.cluster('slotsync 0 0')
        except Exception as e:
            assert "in slot sync" in str(e)

        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 2: OK")

    def test_case05(self):
        # =================== TestCase 5 =============================
        # 新加节点向多个源节点同步slot
        # ============================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(1)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 15495 15495')
        conn.cluster('slotsync 7365 7375')
        time.sleep(2)
        # 5.切换slot
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        linkcount = 2
        flag = 1
        query = 1
        while query and flag:
            linkinfos = conn.cluster('slotlink list')
            finishedlink = 0
            for linkinfo in linkinfos:
                print(linkinfo)
                if 'state:connected lag:0' not in linkinfo:
                    time.sleep(1)
                    break
                else:
                    finishedlink = finishedlink + 1
                    if finishedlink == linkcount:
                        if flag == 0:
                            query = 0
                        flag = 0
                        break
        conn.cluster('slotfailover')
        time.sleep(2)
        # 6.检查新节点上的key
        keys = conn.keys()
        keys.sort()
        print(keys)
        assert keys[0] == '{a}0'
        assert keys[1] == '{a}1'
        assert keys[2] == '{a}2'
        assert keys[3] == '{c}0'
        assert keys[4] == '{c}1'
        assert keys[5] == '{c}2'
        # 7. 源节点上key已删除
        conn = redis.StrictRedis(host='127.0.0.1', port=9002, decode_responses=True)
        keys = conn.keys('{a}*')
        assert len(keys) == 0
        conn = redis.StrictRedis(host='127.0.0.1', port=9001, decode_responses=True)
        keys = conn.keys('{b}*')
        assert len(keys) == 0
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 5: OK")

    def test_case06(self):
        # =================== TestCase 6 =============================
        # 在增量同步slot的过程中, 目标节点出现OOM后，link->lag == -1,
        # CC会检查这种状态并抛出异常
        # ============================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(0.5)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        time.sleep(1)
        wait_slotlink_connected(conn, 1, 10)
        # 5. 更改目标节点maxmemory,模拟目标节点内存不足
        conn.config_set('maxmemory', 1)
        # 6. 在源节点上写入数据
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        conn.set('{b}xxx', 'yyyyy')
        time.sleep(1)
        # 7. 检查slotlink状态
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        linkinfo = conn.cluster('slotlink list')
        assert 'lag:-1' in linkinfo[0]
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 6: OK")

    def test_case07(self):
        # =================== TestCase 7 =============================
        # slot同步过程中将slotlink kill掉
        # ============================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(0.5)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        time.sleep(1)
        # 6. 检查slotlink状态
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        linkinfo = conn.cluster('slotlink list')
        print(linkinfo[0])
        print(type(linkinfo[0]))
        assert len(linkinfo) == 1
        # 7. kill slotlink
        try:
            conn.execute_command('cluster slotlink kill ' + "fake_link_name_xxx")
        except Exception as e:
            assert "no such link" in str(e)
        linkname = linkinfo[0].split(' ')[0].split(':')[1]
        conn.execute_command('cluster slotlink kill ' + linkname)
        time.sleep(2)
        # 8. 检查slotlink是否存在,再次发起切换是否成功
        linkinfo = conn.cluster('slotlink list')
        assert len(linkinfo) == 0
        try:
            conn.cluster('slotfailover')
        except Exception as e:
            assert "no slot link can failover" in str(e)
        # 9. 检查是否将已同步的数据清除
        keys = conn.keys()
        assert len(keys) == 0
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 7: OK")

    def test_case08(self):
        # =================== TestCase 8 =============================
        # 新加节点向多个源节点同步slot,并且使用diskless方式复制
        # ============================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3, diskless=True)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(2)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 15495 15495')
        conn.cluster('slotsync 7365 7365')
        try:
            conn.cluster('slotfailover')
        except Exception as e:
            assert "is not connected" in str(e)
        # 5.切换slot
        query = 1
        while query:
            linkinfos = conn.cluster('slotlink list')
            finishedlink = 0
            for linkinfo in linkinfos:
                print(linkinfo)
                if 'state:connected lag:0' not in linkinfo:
                    time.sleep(1)
                    break
                else:
                    finishedlink = finishedlink + 1
                    if finishedlink == len(linkinfos):
                        query = 0
                        break
        conn.cluster('slotfailover')
        try:
            conn.cluster('slotfailover')
        except Exception as e:
            assert "slot failover in progress" in str(e)
        time.sleep(1)
        # 6.检查新节点上的key
        keys = conn.keys()
        keys.sort()
        assert keys[0] == '{a}0'
        assert keys[1] == '{a}1'
        assert keys[2] == '{a}2'
        assert keys[3] == '{c}0'
        assert keys[4] == '{c}1'
        assert keys[5] == '{c}2'
        # 7. 源节点上key已删除
        conn = redis.StrictRedis(host='127.0.0.1', port=9002, decode_responses=True)
        keys = conn.keys('{a}*')
        assert len(keys) == 0
        conn = redis.StrictRedis(host='127.0.0.1', port=9001, decode_responses=True)
        keys = conn.keys('{b}*')
        assert len(keys) == 0
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 8: OK")

    def test_case09(self):
        # =================== TestCase 9 ====================================
        # 新加节点向多个源节点同步slot,并使用slotsync takeover方式切换 slot
        # ===================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3, diskless=True)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(2)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 15495 15495')
        conn.cluster('slotsync 7365 7365')
        # 5.切换slot
        query = 1
        while query:
            linkinfos = conn.cluster('slotlink list')
            finishedlink = 0
            for linkinfo in linkinfos:
                print(linkinfo)
                if 'state:connected lag:0' not in linkinfo:
                    time.sleep(1)
                    break
                else:
                    finishedlink = finishedlink + 1
                    if finishedlink == len(linkinfos):
                        query = 0
                        break
        try:
            conn.cluster('slotfailover takeove')
        except Exception as e:
            assert str(e) == 'syntax error'
        conn.cluster('slotfailover takeover')
        time.sleep(1)
        # 6.检查新节点上的key
        keys = conn.keys()
        keys.sort()
        assert keys[0] == '{a}0'
        assert keys[1] == '{a}1'
        assert keys[2] == '{a}2'
        assert keys[3] == '{c}0'
        assert keys[4] == '{c}1'
        assert keys[5] == '{c}2'
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 9: OK")

    def test_case10(self):
        # =================== TestCase 10 ====================================
        # 新加节点向多个源节点同步slot,然后reset cluster
        # ===================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3, diskless=True)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(2)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 15495 15495')
        conn.cluster('slotsync 7365 7365')
        time.sleep(3)
        # 5.在新节点上reset cluster
        try:
            conn.cluster('reset')
        except Exception as e:
            assert str(e) == "CLUSTER RESET can't be called with master nodes containing keys"
        conn.flushall()
        conn.cluster('reset')
        # 6. 检查slotlink是否被清除
        linkinfos = conn.cluster('slotlink list')
        assert len(linkinfos) == 0
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 10: OK")

    def test_case11(self):
        # =================== TestCase 11 ====================================
        # 在slot异步迁移的过程中，源节点故障,目标节点主动连接到源的新master节点上
        # ===================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3, diskless=True)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(1)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        time.sleep(3)
        # 5.干掉源节点
        redis_cluster.stop_redis_node(9000)
        # 6. 等待源端slave提为master
        while True:
            node_info = conn.cluster('nodes')
            if 'master' in (node_info['127.0.0.1:9003']['flags']):
                break
            time.sleep(1)
        # 7. 在新master上写入新key
        conn = redis.StrictRedis(host='127.0.0.1', port=9003, decode_responses=True)
        conn.set('{b}new', 'abc')
        time.sleep(1)
        # 8.切换slot
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        query = 1
        while query:
            linkinfos = conn.cluster('slotlink list')
            finishedlink = 0
            for linkinfo in linkinfos:
                print(linkinfo)
                if 'state:connected lag:0' not in linkinfo:
                    time.sleep(1)
                    break
                else:
                    finishedlink = finishedlink + 1
                    if finishedlink == len(linkinfos):
                        query = 0
                        break
        conn.cluster('slotfailover takeover')
        # 8. 检查新key是否写入到目标节点
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        keys = conn.keys()
        assert '{b}new' in keys
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 11: OK")

    def test_case12(self):
        # =================== TestCase 12 =============================
        # 向master节点发sync命令并指定slot区间,检验slot区间参数的合法性
        # ============================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        # 2. 检验slot区间参数的合法性
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        try:
            conn.execute_command('sync 0 10 15')
        except Exception as e:
            assert "wrong number of arguments" in str(e)
        try:
            conn.execute_command('sync x 10')
        except Exception as e:
            assert str(e) == 'Invalid or out of range slot'
        try:
            conn.execute_command('sync 0 x')
        except Exception as e:
            assert str(e) == 'Invalid or out of range slot'
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 12: OK")

    def test_case13(self):
        # =================== TestCase 13 ==========================================
        # slot异步迁移过程中，对于多keys命令，如果key属于同一个slot，则允许执行,
        # 如果keys不属于同一个slot则不允许执行.
        # =========================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(1)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 15495 15495')
        time.sleep(2)
        wait_slotlink_connected(conn, 1, 10)
        # 5. 在源节点通过MSET写入多个同slot key, 可以写成功
        conn = redis.StrictRedis(host='127.0.0.1', port=9002, decode_responses=True)
        keydict = {}
        keydict['{d}new1'] = 'value1'
        keydict['{d}new2'] = 'value2'
        conn.mset(keydict)
        keydict = {}
        keydict['{a}new1'] = 'value1'
        keydict['{a}new2'] = 'value2'
        conn.mset(keydict)  # hashslot({a})为正在迁移的slot
        time.sleep(2)
        # 6. 检查{a}new1, {a}new2 命令是否同步到目标节点
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        keys = conn.keys()
        assert '{a}new1' in keys
        assert '{a}new2' in keys
        # 7. 在源节点通过MSET写入多个跨slot的key, 不能写成功
        conn = redis.StrictRedis(host='127.0.0.1', port=9002, decode_responses=True)
        keydict = {}
        keydict['{a}new11'] = 'value1'
        keydict['{d}new22'] = 'value2'
        try:
            conn.mset(keydict)
        except Exception as e:
            assert "CROSSSLOT Keys in request don't hash to the same slot"
        # 8. 目标节点断开slot同步后，再次通过MSET写入跨slot的命令，可以成功
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        linkinfo = conn.cluster('slotlink list')
        linkname = linkinfo[0].split(' ')[0].split(':')[1]
        conn.execute_command('cluster slotlink kill ' + linkname)
        time.sleep(2)
        conn = redis.StrictRedis(host='127.0.0.1', port=9002, decode_responses=True)
        keydict = {}
        keydict['{a}new11'] = 'value1'
        keydict['{d}new22'] = 'value2'
        try:
            conn.mset(keydict)
        except Exception as e:
            # 无法跨 slot
            # assert 0
            assert "CROSSSLOT Keys in request don't hash to the same slot"
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 13: OK")

    def test_case14(self):
        # =================== TestCase 14 ==========================================
        # slot切换超时后，源节点上slot读写不受影响,slot切换可重试
        # =========================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(1)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        time.sleep(2)
        wait_slotlink_connected(conn, 1, 10)
        # 5. 在源与目标节点上注入slot切换超时时间，设为1ms
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        conn.config_set('debug-context', "crs-cluster-mf-timeout:1")
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.config_set('debug-context', "crs-cluster-mf-timeout:1")
        time.sleep(2)
        # 6. 发起slot切换
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotfailover')
        time.sleep(2)
        # 7. 切换超时，此时源上读写正常
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        conn.set('{b}new', '111')
        assert conn.get('{b}new') == '111'
        # 8.取消源与目标节点的1ms slot切换超时
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        conn.config_set('debug-context', "-")
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.config_set('debug-context', "-")
        # 6. 再次发起slot切换
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotfailover')
        time.sleep(1)
        # 7. 切换成功
        linkinfo = conn.cluster('slotlink list')
        print(linkinfo)
        assert len(linkinfo) == 0
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 14: OK")

    def test_case15(self):
        # =================== TestCase 15 ==========================================
        # slot迁移时，在源端不会重用正在进行的全量bgsave
        # =========================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(1)
        # 4. 给9000节点添加新的从节点
        conn.config_set('debug-context', 'crs-bgsave-pause-time:5')
        redis_cluster.add_new_slave(9000, 9007)
        time.sleep(1)
        conn.config_set('debug-context', 'crs-bgsave-pause-time:0')
        # 5. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        # 6. 等待同步成功
        time.sleep(2)
        wait_slotlink_connected(conn, 1, 10)
        # 7. 检查数据同步是否准确无误
        keys = conn.keys()
        print(keys)
        assert len(keys) == 3
        assert '{b}' in keys[0]
        assert '{b}' in keys[1]
        assert '{b}' in keys[2]
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 15: OK")

    def test_case16(self):
        # =================== TestCase 16 ====================================
        # 在采用社区slot迁移的过程中，源节点故障,目标节点角度看集群始终为ok
        # ===================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(1)
        # 4. 在新节点上将slot 3300标记为importing状态
        masters = redis_cluster.get_master_nodes()
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        node_id = masters[0]['nodeid']  # 9000
        conn.execute_command('cluster setslot 3300 importing ' + node_id)
        # 5.干掉源节点
        redis_cluster.stop_redis_node(9000)
        time.sleep(1)
        # 6. 等待源端slave提为master
        while True:
            node_info = conn.cluster('nodes')
            if 'master' in (node_info['127.0.0.1:9003']['flags']):
                break
            time.sleep(1)
        time.sleep(5)
        # 7. 从新节点上看，集群状态始终为ok，valkey 8.0 cluster setslot 会传播给从节点
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        info = conn.cluster('info')
        assert info['cluster_state'] == 'ok'
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 16: OK")

    def test_case17(self):
        # =================== TestCase 17 ====================================
        # 模拟源节点bgsave失败,目标节点能自动重试
        # ===================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        time.sleep(2)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(1)
        # 4. 在源节点上注入故障，模拟bgsave失败
        conn.config_set('debug-context', 'crs-bgsave-error')
        # 5. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        time.sleep(2)  # 期间slot同步失败，并不断重试
        # 6. 取消源节点上的故障注入
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        conn.config_set('debug-context', '')
        # 7. 等待自动重新发起同步成功
        time.sleep(2)
        wait_slotlink_connected(conn, 1, 10)
        # 8. 检查数据同步是否准确无误
        keys = conn.keys()
        assert len(keys) == 3
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 17: OK")

    def test_case18(self):
        # =================== TestCase 18 ====================================
        # 模拟目标节点rdbLoad错误,目标节点能自动重试
        # ===================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        time.sleep(2)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点,并注入rdbLoad错误
        redis_cluster.add_new_node(9006, 9000)
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.config_set('debug-context', 'crs-rdbload-error')
        time.sleep(1)
        # 5. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        time.sleep(4)  # 期间slot同步失败，并不断重试
        # 6. 取消故障注入
        conn.config_set('debug-context', '')
        time.sleep(2)
        # 7. 自动重新发起同步成功
        wait_slotlink_connected(conn, 1, 10)
        # 8. 检查数据同步是否准确无误
        keys = conn.keys()
        assert len(keys) == 3
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 18: OK")

    def test_case19(self):
        # =================== TestCase 19 ====================================
        # 模拟在slot同步过程中，发生IO错误，目标节点能自动重试
        # ===================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        time.sleep(2)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        time.sleep(1)
        # 4. 在新节点上注入IO error, 模拟发送sync命令时IO错误
        conn.config_set('debug-context', 'crs-io-error-beforce-send-sync')
        # 5. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        time.sleep(1)  # 期间slot同步失败，并不断重试
        # 6. 在新节点上注入IO error, 模拟接收rdb时IO错误
        conn.config_set('debug-context', 'crs-io-error-recv-rdb')
        time.sleep(1)  # 期间slot同步失败，并不断重试
        # 7. 在新节点上注入IO error, 模拟写rdb时IO错误
        conn.config_set('debug-context', 'crs-io-error-write-rdb')
        time.sleep(1)  # 期间slot同步失败，并不断重试
        # 8. 在新节点上注入IO error, 模拟在开始slot同步前IO错误
        conn.config_set('debug-context', 'crs-io-error-beforce-slot-sync')
        time.sleep(1)  # 期间slot同步失败，并不断重试
        # 9. 在新节点上注入IO error, 模拟在握手过程中IO错误
        conn.config_set('debug-context', 'crs-io-error-send-auth')
        time.sleep(1)  # 期间slot同步失败，并不断重试
        # 10. 取消故障注入
        conn.config_set('debug-context', '')
        time.sleep(2)
        # 11. 自动重新发起同步成功
        wait_slotlink_connected(conn, 1, 10)
        # 12. 检查数据同步是否准确无误
        keys = conn.keys()
        assert len(keys) == 3
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 19: OK")

    def test_case20(self):
        # =================== TestCase 20 ====================================
        # 模拟在采用无盘传输的slot同步过程中，出现IO错误，目标节点能自动重试
        # ===================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3, diskless=True)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        time.sleep(2)
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        time.sleep(1)
        # 4. 在新节点上注入IO error, 模拟truncate rdb文件时IO错误
        conn.config_set('debug-context', 'crs-io-error-truncate-rdb')
        # 5. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        time.sleep(10)  # 期间slot同步失败，并不断重试
        # 6. 在新节点上注入IO error, 模拟rename rdb文件时IO错误
        conn.config_set('debug-context', 'crs-io-error-rename-rdb')
        time.sleep(10)  # 期间slot同步失败，并不断重试
        # 7. 取消故障注入
        conn.config_set('debug-context', '')
        # 8. 等待同步成功
        time.sleep(2)
        wait_slotlink_nolag(conn, 1, 10)
        # 9. 检查数据同步是否准确无误
        keys = conn.keys()
        assert len(keys) == 3
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 20: OK")

    def test_case21(self):
        # =================== TestCase 21 ====================================
        # 大数据量情况下slot同步
        # ===================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        time.sleep(2)
        # 2. 写入数据
        value = util.RandomString(1024 * 1024)
        for idx in range(0, 20):
            key = "{b}" + str(idx)
            conn.set(key, value)
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        time.sleep(1)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn.cluster('slotsync 3300 3300')
        # 5. 等待同步完成
        time.sleep(5)
        wait_slotlink_nolag(conn, 1, -1)
        # 6. 检查数据同步是否准确无误
        keys = conn.keys()
        assert len(keys) == 20
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 21: OK")

    def test_case22(self):
        # =================== TestCase 22 ==========================================
        # 在进行slot迁移时，多个目标节点向同一个源节点并行发起同步时，源端不会重用
        # 已有的rdb文件，而是会重新生成相应slot的rdb文件
        # =========================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        # 2. 写入数据
        write_data()
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        redis_cluster.add_new_node(9007, 9000)
        time.sleep(1)
        # 4. 给9002节点添加新的从节点
        conn = redis.StrictRedis(host='127.0.0.1', port=9002, decode_responses=True)
        conn.config_set('debug-context', 'crs-bgsave-pause-time:5')
        redis_cluster.add_new_slave(9002, 9008)
        time.sleep(1)
        conn.config_set('debug-context', 'crs-bgsave-pause-time:0')
        # 5. 在新节点上发起slot同步
        conn1 = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        conn1.cluster('slotsync 15495 15495')  # {a}
        conn2 = redis.StrictRedis(host='127.0.0.1', port=9007, decode_responses=True)
        conn2.cluster('slotsync 11298 11298')  # {d}
        time.sleep(5)
        wait_slotlink_connected(conn1, 1, 10)
        wait_slotlink_connected(conn2, 1, 2)
        # 6. 两个新节点上都只同步各自slot的key
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        keys = conn.keys()
        keys.sort()
        print(keys)
        assert len(keys) == 3
        assert keys[0] == '{a}0'
        assert keys[1] == '{a}1'
        assert keys[2] == '{a}2'
        conn = redis.StrictRedis(host='127.0.0.1', port=9007, decode_responses=True)
        keys = conn.keys()
        keys.sort()
        print(keys)
        assert len(keys) == 3
        assert keys[0] == '{d}0'
        assert keys[1] == '{d}1'
        assert keys[2] == '{d}2'
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 22: OK")

    def test_case23(self):
        ##=================== TestCase 23 =============================
        # disable-expire-key这个参数打开后，会禁止所有过期key淘汰
        # ============================================================
        # 1. 启一个3分片集群
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()
        # 2. 向实例中写入测试数据
        nodes = [{'host': '127.0.0.1', 'port': 9000},
                 {'host': '127.0.0.1', 'port': 9001},
                 {'host': '127.0.0.1', 'port': 9002}]
        if hasattr(rediscluster, "StrictRedisCluster"):
            conn = rediscluster.StrictRedisCluster(startup_nodes=nodes)
        elif hasattr(rediscluster, "RedisCluster"):
            # Use RedisCluster in the new version.
            conn = rediscluster.RedisCluster(startup_nodes=nodes)
        else:
            nodes = [redis.cluster.ClusterNode(node["host"], node["port"]) for node in nodes]
            conn = redis.cluster.RedisCluster(startup_nodes=nodes)
        conn.set("key1", "value1")
        conn.set("key2", "value2")
        conn.set("key3", "value3")
        # 3. 设置key的过期时间
        conn.expire("key1", 1)
        conn.expire("key2", 1)
        time.sleep(2)
        # 4. 删除过期key
        try:
            conn.delete("key1")
            conn.delete("key2")
        except Exception as e:
            assert str(e) == ""

        util.StopAllRedis()
        util.PrintSuccCaseResult("TestCase 23: OK")

    def test_case24(self):
        # =================== TestCase 24 =============================
        # 新加节点向多个源节点同步slot,并且使用diskless方式复制, 开启on-empty-db配置
        # ============================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3, diskless=True, repl_diskless_load="on-empty-db")
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        for master in redis_cluster.masters:
            conn = redis.StrictRedis(host="127.0.0.1", port=master["port"], password=redis_cluster.password)
            body = "#!lua name=mylib \n redis.register_function('myfunc', function(keys, args) return args[1] end)"
            conn.execute_command("function", "load", body)
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(2)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        # conn.config_set('repl-diskless-load ', 'on-empty-db')
        conn.cluster('slotsync 15495 15495')
        conn.cluster('slotsync 7365 7365')
        try:
            conn.cluster('slotfailover')
        except Exception as e:
            assert "is not connected" in str(e)
        # 5.切换slot
        query = 1
        while query:
            linkinfos = conn.cluster('slotlink list')
            finishedlink = 0
            for linkinfo in linkinfos:
                print(linkinfo)
                if 'state:connected lag:0' not in linkinfo:
                    time.sleep(1)
                    break
                else:
                    finishedlink = finishedlink + 1
                    if finishedlink == len(linkinfos):
                        query = 0
                        break
        conn.cluster('slotfailover')
        try:
            conn.cluster('slotfailover')
        except Exception as e:
            assert "slot failover in progress" in str(e)
        time.sleep(1)
        # 6.检查新节点上的key
        keys = conn.keys()
        keys.sort()
        assert keys[0] == '{a}0'
        assert keys[1] == '{a}1'
        assert keys[2] == '{a}2'
        assert keys[3] == '{c}0'
        assert keys[4] == '{c}1'
        assert keys[5] == '{c}2'
        # 7. 源节点上key已删除
        conn = redis.StrictRedis(host='127.0.0.1', port=9002, decode_responses=True)
        keys = conn.keys('{a}*')
        assert len(keys) == 0
        conn = redis.StrictRedis(host='127.0.0.1', port=9001, decode_responses=True)
        keys = conn.keys('{b}*')
        assert len(keys) == 0
        for master in redis_cluster.masters:
            conn = redis.StrictRedis(host="127.0.0.1", port=master["port"], password=redis_cluster.password,
                                     decode_responses=True)
            assert (conn.execute_command("function", "delete", "mylib") == "OK")
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 24: OK")

    def test_case25(self):
        # =================== TestCase 24 =============================
        # 新加节点向多个源节点同步slot,并且使用diskless方式复制, 开启swapdb配置
        # ============================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3, diskless=True, repl_diskless_load="swapdb")
        redis_cluster.start_cluster()
        conn = redis.StrictRedis(host='127.0.0.1', port=9000, decode_responses=True)
        # 2. 写入数据
        write_data()
        for master in redis_cluster.masters:
            conn = redis.StrictRedis(host="127.0.0.1", port=master["port"], password=redis_cluster.password)
            body = "#!lua name=mylib \n redis.register_function('myfunc', function(keys, args) return args[1] end)"
            conn.execute_command("function", "load", body)
        # 3. 加入一个新节点
        redis_cluster.add_new_node(9006, 9000)
        time.sleep(2)
        # 4. 在新节点上发起slot同步
        conn = redis.StrictRedis(host='127.0.0.1', port=9006, decode_responses=True)
        # conn.config_set('repl-diskless-load ', 'swapdb')
        conn.cluster('slotsync 15495 15495')
        conn.cluster('slotsync 7365 7365')
        try:
            conn.cluster('slotfailover')
        except Exception as e:
            assert "is not connected" in str(e)
        # 5.切换slot
        query = 1
        while query:
            linkinfos = conn.cluster('slotlink list')
            finishedlink = 0
            for linkinfo in linkinfos:
                print(linkinfo)
                if 'state:connected lag:0' not in linkinfo:
                    time.sleep(1)
                    break
                else:
                    finishedlink = finishedlink + 1
                    if finishedlink == len(linkinfos):
                        query = 0
                        break
        conn.cluster('slotfailover')
        try:
            conn.cluster('slotfailover')
        except Exception as e:
            assert "slot failover in progress" in str(e)
        time.sleep(1)
        # 6.检查新节点上的key
        keys = conn.keys()
        keys.sort()
        assert keys[0] == '{a}0'
        assert keys[1] == '{a}1'
        assert keys[2] == '{a}2'
        assert keys[3] == '{c}0'
        assert keys[4] == '{c}1'
        assert keys[5] == '{c}2'
        # 7. 源节点上key已删除
        conn = redis.StrictRedis(host='127.0.0.1', port=9002, decode_responses=True)
        keys = conn.keys('{a}*')
        assert len(keys) == 0
        conn = redis.StrictRedis(host='127.0.0.1', port=9001, decode_responses=True)
        keys = conn.keys('{b}*')
        assert len(keys) == 0
        for master in redis_cluster.masters:
            conn = redis.StrictRedis(host="127.0.0.1", port=master["port"], password=redis_cluster.password,
                                     decode_responses=True)
            assert (conn.execute_command("function", "delete", "mylib") == "OK")
        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 25: OK")

    def test_case26(self):
        # =================== TestCase 26 ====================================
        # 源节点和目标节点之间断连后，重新生成和加载 slot RDB 时，检查目标节点的从节点数据
        # ====================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        redis_cluster = cluster.RedisCluster(password='', shard_size=3)
        redis_cluster.start_cluster()

        # 2. 加入一个新节点和发起slot同步，3300 属于 9000 这个节点，对应的 hashtag 是 {b}
        write_data()
        redis_cluster.add_new_node(9006, 9000)
        conn_9006 = redis.StrictRedis(host="127.0.0.1", port=9006, decode_responses=True)
        conn_9006.cluster("slotsync 3300 3300")
        wait_slotlink_connected(conn_9006, 1, 10)
        assert len(conn_9006.keys()) == 3  # 0 / 1 / 2

        # 3. 源节点新增，检查命令传播在目标节点里是否 ok
        conn_9000 = redis.StrictRedis(host="127.0.0.1", port=9000, decode_responses=True)
        conn_9000.set("{b}3", "value-b3")
        conn_9000.execute_command("set", "{b}3", "value-b3", "ex", 10000)
        conn_9000.execute_command("set", "{b}3", "value-b3", "exat", int(time.time()) + 10000)
        conn_9000.execute_command("set", "{b}3", "value-b3", "px", 10000000)
        conn_9000.execute_command("set", "{b}3", "value-b3", "pxat", int(time.time() * 1000) + 10000000)
        time.sleep(0.5)
        assert len(conn_9006.keys()) == 4  # 0 / 1 / 2 / 3

        # 4. 加入一个节点跟新主节点做主从，并且等待从完成同步，检查从节点数据
        redis_cluster.add_new_slave(9006, 9007)
        conn_9007 = redis.StrictRedis(host="127.0.0.1", port=9007, decode_responses=True)
        redis_cluster.wait_for_sync(conn_9007)
        keys = conn_9007.keys()
        assert len(keys) == 4

        # 5. 在 slotfailover 之前，新主跟源之间的主从断连了，并且重连后进行了全量同步，生成了新的 slot RDB
        # 在目标节点这里杀掉对应的客户端连接，在 free client 时会 reset link，之后在 cron 里会进行重连，
        # 此时会重新走 slot 的搬迁流程，源节点会新生成 slot RDB，目标节点会重新加载 slot RDB，检查从节点数据
        # 目标节点跟源节点的主从连接，在目标节点视角里是一个 normal client，所以这边暴力点直接杀掉所有 normal client
        conn_9006.execute_command("client kill type normal skipme yes")

        # 6. 杀掉连接后源节点马上设置一个新值，等待目标节点重新走 slot 搬迁流程，等待 link 状态
        conn_9000.set("{b}4", "value-b4")
        wait_slotlink_connected(conn_9006, 1, 10)
        assert len(conn_9000.keys()) == 5
        assert len(conn_9006.keys()) == 5

        # 7. 检查从节点数据
        redis_cluster.wait_for_sync(conn_9007)
        assert len(conn_9007.keys()) == 5

        util.StopAllRedis()
        time.sleep(1)
        util.PrintSuccCaseResult("TestCase 26: OK")

    def my_test_case27(self, diskless=False, repl_diskless_load="", dual_channel=False):
        # =================== TestCase 27 ====================================
        # 过期键在加载 slot RDB 的过程中被过期删除，导致增量来的命令无法续上
        # ====================================================================
        # 1. 启一个三分片集群(master, slave):(9000, 9003),(9001, 9004),(9002,9005).
        # 设置 cluster_node_timeout 的原因是避免目标节点在加载阻塞期间被判死触发被动故障转移
        redis_cluster = cluster.RedisCluster(password='', shard_size=3, cluster_node_timeout=20000, diskless=diskless,
                                             repl_diskless_load=repl_diskless_load, dual_channel=dual_channel)
        redis_cluster.start_cluster()

        # 3. 加入一个新节点作为目标节点
        redis_cluster.add_new_node(9006, 9000)
        conn_9006 = redis.StrictRedis(host="127.0.0.1", port=9006, decode_responses=True)

        # 4. 给目标节点增加一个从节点，在 slotsync 之前
        redis_cluster.add_new_slave(9006, 9007)
        conn_9007 = redis.StrictRedis(host="127.0.0.1", port=9007, decode_responses=True)

        # 5. 9000 源节点增加过期键，约定的 3300 slot 的 hashtag 是 {b}
        conn_9000 = redis.StrictRedis(host="127.0.0.1", port=9000, decode_responses=True)
        conn_9000.execute_command("set", "{b}1", "value-b1", "ex", 10)
        conn_9000.execute_command("set", "{b}2", "value-b2", "ex", 10)

        # 6. 发起 slot 同步，3300 属于 9000 这个节点，对应的 hashtag 是 {b}
        conn_9006.execute_command("config", "set", "key-load-delay", 10000000)  # 10s
        conn_9006.cluster("slotsync 3300 3300")

        # 7. 等待目标节点进入 slot RDB 加载状态，因为目标节点设置了 key-load-delay，所以它会需要
        # 加载 20s 左右，此时后面进来的增量命令都会堆积到缓冲区里
        time.sleep(5)

        # 8. 9000 源节点使用 expire 给键续期，这部分命令会堆积到缓冲区里
        conn_9000.execute_command("expire", "{b}1", 100)
        conn_9000.execute_command("expire", "{b}2", 100)
        conn_9000.execute_command("set", "{b}new", "value-new")

        # 9. 等待目标节点 online，此时增量命令都应该会消费完
        wait_slotlink_connected(conn_9006, 1, 20)
        conn_9006.execute_command("config", "set", "key-load-delay", 0)

        # 10. 给目标节点增加一个从节点，在 slotsync 之后
        redis_cluster.add_new_slave(9006, 9008)
        conn_9008 = redis.StrictRedis(host="127.0.0.1", port=9008, decode_responses=True)

        # 11. 等待主从连接正常，通常这里 offset 也应该是一致的
        redis_cluster.wait_for_sync(conn_9007)
        redis_cluster.wait_for_sync(conn_9008)

        # 12. slotfailover 之前确保键数据都正常，目标节点在 slotfailover 之前可以看到 slot RDB 的数据
        assert len(conn_9000.keys()) == 3  # ['{b}1', '{b}2', '{b}new']
        assert len(conn_9006.keys()) == 3  # ['{b}1', '{b}2', '{b}new']
        assert len(conn_9007.keys()) == 3  # ['{b}1', '{b}2', '{b}new']
        assert len(conn_9008.keys()) == 3  # ['{b}1', '{b}2', '{b}new']

        # 12. slotfailover 之前确保键数据都正常
        conn_9006.cluster('slotfailover')
        time.sleep(1)
        redis_cluster.wait_for_sync(conn_9007)
        redis_cluster.wait_for_sync(conn_9008)
        assert len(conn_9000.keys()) == 0
        assert len(conn_9006.keys()) == 3  # ['{b}1', '{b}2', '{b}new']
        assert len(conn_9007.keys()) == 3  # ['{b}1', '{b}2', '{b}new']
        assert len(conn_9008.keys()) == 3  # ['{b}1', '{b}2', '{b}new']

        # 13. 加载 slot RDB 现在不会返回 loading 错误
        for i in range(10):
            key = "{b}" + str(i)
            value = "value-" + key
            conn_9006.execute_command("set", key, value)

        # 从 9006 往 9000 里搬迁
        conn_9000.execute_command("config", "set", "key-load-delay", 1000000)  # 1s
        conn_9000.cluster("slotsync 3300 3300")

        time.sleep(5)
        conn_9000.execute_command("get", "1a")  # Won't return loading error.
        conn_9000.execute_command("config", "set", "key-load-delay", 0)
        time.sleep(2)
        conn_9000.cluster('slotfailover')
        time.sleep(2)

        assert len(conn_9000.keys()) == 11
        assert len(conn_9006.keys()) == 0  # ['{b}1', '{b}2', '{b}new']
        assert len(conn_9007.keys()) == 0  # ['{b}1', '{b}2', '{b}new']
        assert len(conn_9008.keys()) == 0  # ['{b}1', '{b}2', '{b}new']

        util.StopAllRedis()
        time.sleep(1)

    def test_case27(self):
        self.my_test_case27(diskless=False, repl_diskless_load="", dual_channel=False)
        util.PrintSuccCaseResult("TestCase 27: OK")

    def test_case28(self):
        self.my_test_case27(diskless=False, repl_diskless_load="", dual_channel=True)
        util.PrintSuccCaseResult("TestCase 28: OK")

    def test_case29(self):
        self.my_test_case27(diskless=True, repl_diskless_load="", dual_channel=False)
        util.PrintSuccCaseResult("TestCase 29: OK")

    def test_case30(self):
        self.my_test_case27(diskless=True, repl_diskless_load="", dual_channel=True)
        util.PrintSuccCaseResult("TestCase 30: OK")

    def test_case31(self):
        self.my_test_case27(diskless=True, repl_diskless_load="swapdb", dual_channel=False)
        util.PrintSuccCaseResult("TestCase 31: OK")

    def test_case32(self):
        self.my_test_case27(diskless=True, repl_diskless_load="swapdb", dual_channel=True)
        util.PrintSuccCaseResult("TestCase 32: OK")

    def tearDown(self):
        util.StopAllRedis()
        time.sleep(1)


if __name__ == "__main__":
    suite = unittest.TestSuite()
    test_cases = [
        TestSlotSync("test_case01"),
        TestSlotSync("test_case02"),
        TestSlotSync("test_case05"),
        TestSlotSync("test_case06"),
        TestSlotSync("test_case07"),
        TestSlotSync("test_case08"),
        TestSlotSync("test_case09"),
        TestSlotSync("test_case10"),
        TestSlotSync("test_case11"),
        TestSlotSync("test_case12"),
        TestSlotSync("test_case13"),
        TestSlotSync("test_case14"),
        TestSlotSync("test_case15"),
        TestSlotSync("test_case16"),
        TestSlotSync("test_case17"),
        TestSlotSync("test_case18"),
        TestSlotSync("test_case19"),
        TestSlotSync("test_case20"),
        TestSlotSync("test_case21"),
        TestSlotSync("test_case22"),
        TestSlotSync("test_case23"),
        TestSlotSync("test_case24"),
        TestSlotSync("test_case25"),
        TestSlotSync("test_case26"),
        TestSlotSync("test_case27"),
        TestSlotSync("test_case29"),
        TestSlotSync("test_case31"),

        # TestSlotSync("test_case28"),
        # TestSlotSync("test_case30"),
    ]
    suite.addTests(test_cases)
    unittest.TextTestRunner().run(suite)

    # unittest.main()
