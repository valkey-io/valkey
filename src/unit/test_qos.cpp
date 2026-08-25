/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "qos.h"
#include "zmalloc.h"
}

class QosTest : public ::testing::Test {};

TEST_F(QosTest, ParseSubnetIpv4) {
    qosSubnet subnet;

    // Valid IPv4 subnets
    EXPECT_EQ(parseQosSubnetSource("192.168.1.0/24", &subnet), 0);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 24);
    uint32_t expected_ip = 0;
    inet_pton(AF_INET, "192.168.1.0", &expected_ip);
    EXPECT_EQ(subnet.addr.ipv4.s_addr, expected_ip);

    EXPECT_EQ(parseQosSubnetSource("10.0.0.0/8", &subnet), 0);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 8);

    EXPECT_EQ(parseQosSubnetSource("192.168.1.5/32", &subnet), 0);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 32);

    EXPECT_EQ(parseQosSubnetSource("0.0.0.0/0", &subnet), 0);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 0);

    // Valid raw IPv4 (no slash)
    EXPECT_EQ(parseQosSubnetSource("192.168.1.1", &subnet), 0);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 32);
    inet_pton(AF_INET, "192.168.1.1", &expected_ip);
    EXPECT_EQ(subnet.addr.ipv4.s_addr, expected_ip);

    // Invalid IPv4 subnets
    EXPECT_EQ(parseQosSubnetSource("1.2.3.4/999", &subnet), -1);
    EXPECT_EQ(parseQosSubnetSource("invalid/24", &subnet), -1);
    EXPECT_EQ(parseQosSubnetSource("1.2.3.4.5/24", &subnet), -1);
    EXPECT_EQ(parseQosSubnetSource("/24", &subnet), -1);
    EXPECT_EQ(parseQosSubnetSource("192.168.1.0/", &subnet), -1);
    EXPECT_EQ(parseQosSubnetSource("192.168.1.0/-1", &subnet), -1);
    EXPECT_EQ(parseQosSubnetSource("192.168.1.0/33", &subnet), -1);
    EXPECT_EQ(parseQosSubnetSource("1.2.3.4/24a", &subnet), -1);
}

TEST_F(QosTest, ParseSubnetIpv6) {
    qosSubnet subnet;

    // Valid IPv6 subnets
    EXPECT_EQ(parseQosSubnetSource("2001:db8::/32", &subnet), 0);
    EXPECT_EQ(subnet.family, AF_INET6);
    EXPECT_EQ(subnet.prefix_len, 32);
    struct in6_addr expected_ip;
    inet_pton(AF_INET6, "2001:db8::", &expected_ip);
    EXPECT_EQ(memcmp(&subnet.addr.ipv6, &expected_ip, sizeof(struct in6_addr)), 0);

    EXPECT_EQ(parseQosSubnetSource("::1/128", &subnet), 0);
    EXPECT_EQ(subnet.family, AF_INET6);
    EXPECT_EQ(subnet.prefix_len, 128);

    EXPECT_EQ(parseQosSubnetSource("::/0", &subnet), 0);
    EXPECT_EQ(subnet.family, AF_INET6);
    EXPECT_EQ(subnet.prefix_len, 0);

    // Valid raw IPv6 (no slash)
    EXPECT_EQ(parseQosSubnetSource("2001:db8::1", &subnet), 0);
    EXPECT_EQ(subnet.family, AF_INET6);
    EXPECT_EQ(subnet.prefix_len, 128);
    inet_pton(AF_INET6, "2001:db8::1", &expected_ip);
    EXPECT_EQ(memcmp(&subnet.addr.ipv6, &expected_ip, sizeof(struct in6_addr)), 0);

    // Invalid IPv6 subnets
    EXPECT_EQ(parseQosSubnetSource("2001:db8::/129", &subnet), -1);
    EXPECT_EQ(parseQosSubnetSource("invalid/64", &subnet), -1);
    EXPECT_EQ(parseQosSubnetSource("2001:db8::/-1", &subnet), -1);
}

TEST_F(QosTest, ParseSubnetList) {
    qosSubnet *subnets = NULL;
    int count = 0;

    // Empty or NULL input
    EXPECT_EQ(parseQosSubnetSourceList(NULL, NULL, &count), -1);
    EXPECT_EQ(parseQosSubnetSourceList(NULL, NULL, NULL), -1);
    EXPECT_EQ(parseQosSubnetSourceList(NULL, &subnets, &count), 0);
    EXPECT_EQ(subnets, nullptr);
    EXPECT_EQ(count, 0);

    EXPECT_EQ(parseQosSubnetSourceList("", &subnets, &count), 0);
    EXPECT_EQ(subnets, nullptr);
    EXPECT_EQ(count, 0);

    // Valid list with different separators
    EXPECT_EQ(parseQosSubnetSourceList("192.168.1.0/24, 10.0.0.0/8\tfe80::/10", &subnets, &count), 0);
    ASSERT_NE(subnets, nullptr);
    EXPECT_EQ(count, 3);
    EXPECT_EQ(subnets[0].family, AF_INET);
    EXPECT_EQ(subnets[0].prefix_len, 24);
    EXPECT_EQ(subnets[1].family, AF_INET);
    EXPECT_EQ(subnets[1].prefix_len, 8);
    EXPECT_EQ(subnets[2].family, AF_INET6);
    EXPECT_EQ(subnets[2].prefix_len, 10);
    zfree(subnets);

    // Invalid token in the list
    subnets = NULL;
    count = 0;
    EXPECT_EQ(parseQosSubnetSourceList("192.168.1.0/24, invalid_ip", &subnets, &count), -1);
    EXPECT_EQ(subnets, nullptr);
    EXPECT_EQ(count, 0);
}

TEST_F(QosTest, MatchIpAgainstSubnetsIpv4) {
    qosSubnet subnets[3];
    ASSERT_EQ(parseQosSubnetSource("192.168.1.0/24", &subnets[0]), 0);
    ASSERT_EQ(parseQosSubnetSource("10.0.0.0/8", &subnets[1]), 0);
    ASSERT_EQ(parseQosSubnetSource("172.16.0.0/12", &subnets[2]), 0);

    // Matches
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("192.168.1.5", subnets, 3));
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("10.254.0.1", subnets, 3));
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("172.16.10.20", subnets, 3));
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("172.31.255.254", subnets, 3));

    // Mismatches
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("192.168.2.5", subnets, 3));
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("11.0.0.1", subnets, 3));
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("172.32.0.1", subnets, 3));
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("invalid-ip", subnets, 3));
}

TEST_F(QosTest, MatchIpAgainstSubnetsIpv6) {
    qosSubnet subnets[3];
    ASSERT_EQ(parseQosSubnetSource("2001:db8::/32", &subnets[0]), 0);
    ASSERT_EQ(parseQosSubnetSource("::1/128", &subnets[1]), 0);
    ASSERT_EQ(parseQosSubnetSource("fe80::/10", &subnets[2]), 0);

    // Matches
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("2001:db8:abcd::1", subnets, 3));
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("::1", subnets, 3));
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("fe80::1ff:fe23:4567:890a", subnets, 3));

    // Mismatches
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("2001:db9::1", subnets, 3));
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("::2", subnets, 3));
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("fec0::1", subnets, 3));
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("invalid-ip", subnets, 3));
}

TEST_F(QosTest, MatchIpAgainstSubnetsEdgeCases) {
    qosSubnet subnets[2];
    ASSERT_EQ(parseQosSubnetSource("0.0.0.0/0", &subnets[0]), 0);
    ASSERT_EQ(parseQosSubnetSource("::/0", &subnets[1]), 0);

    // Any IPv4 matches 0.0.0.0/0
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("192.168.1.1", &subnets[0], 1));
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("8.8.8.8", &subnets[0], 1));
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("::1", &subnets[0], 1));

    // Any IPv6 matches ::/0
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("2001:db8::1", &subnets[1], 1));
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("::1", &subnets[1], 1));
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("192.168.1.1", &subnets[1], 1));
}

TEST_F(QosTest, Ipv4MappedIpv6DualStack) {
    qosSubnet subnet;
    ASSERT_EQ(parseQosSubnetSource("192.168.1.0/24", &subnet), 0);

    // IPv4-mapped IPv6 address ::ffff:192.168.1.5 matches IPv4 subnet
    EXPECT_TRUE(matchIpAgainstQosSubnetSources("::ffff:192.168.1.5", &subnet, 1));
    EXPECT_FALSE(matchIpAgainstQosSubnetSources("::ffff:192.168.2.5", &subnet, 1));
}

TEST_F(QosTest, QosValidateAndUpdateSubnetSources) {
    const char *err = NULL;
    EXPECT_EQ(validateQosSubnetSources("192.168.1.0/24, 10.0.0.0/8", &err), 0);
    EXPECT_EQ(err, nullptr);

    EXPECT_EQ(validateQosSubnetSources("192.168.1.0/24, invalid-ip", &err), -1);
    EXPECT_NE(err, nullptr);

    EXPECT_EQ(updateQosSubnetSources("192.168.1.0/24 10.0.0.0/8"), 0);
    qosFree();
}

TEST_F(QosTest, IsIpQosPrioritized) {
    qosFree();
    EXPECT_FALSE(isIpQosPrioritized("192.168.1.1"));
    EXPECT_FALSE(isIpQosPrioritized(NULL));

    ASSERT_EQ(updateQosSubnetSources("192.168.1.0/24 2001:db8::/32"), 0);

    EXPECT_TRUE(isIpQosPrioritized("192.168.1.50"));
    EXPECT_TRUE(isIpQosPrioritized("2001:db8::1"));
    EXPECT_FALSE(isIpQosPrioritized("192.168.2.1"));
    EXPECT_FALSE(isIpQosPrioritized("2001:db9::1"));
    EXPECT_FALSE(isIpQosPrioritized(NULL));
    EXPECT_FALSE(isIpQosPrioritized("invalid-ip"));

    qosFree();
}

TEST_F(QosTest, HasQosSubnetSources) {
    qosFree();
    EXPECT_FALSE(hasQosSubnetSources());

    ASSERT_EQ(updateQosSubnetSources("192.168.1.0/24"), 0);
    EXPECT_TRUE(hasQosSubnetSources());

    qosFree();
    EXPECT_FALSE(hasQosSubnetSources());
}
