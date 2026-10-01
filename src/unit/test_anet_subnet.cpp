/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "generated_wrappers.hpp"

extern "C" {
#include "anet.h"
#include "server.h"
#include "zmalloc.h"
}

class AnetSubnetTest : public ::testing::Test {};

TEST_F(AnetSubnetTest, ParseSubnetIpv4) {
    anetSubnet subnet;
    char err[ANET_ERR_LEN] = {0};

    /* Valid IPv4 subnets */
    EXPECT_EQ(anetParseSubnet(err, "192.168.1.0/24", &subnet), ANET_OK);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 24);
    uint32_t expected_ip = 0;
    inet_pton(AF_INET, "192.168.1.0", &expected_ip);
    EXPECT_EQ(subnet.addr.ipv4.s_addr, expected_ip);

    EXPECT_EQ(anetParseSubnet(err, "10.0.0.0/8", &subnet), ANET_OK);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 8);

    EXPECT_EQ(anetParseSubnet(err, "192.168.1.5/32", &subnet), ANET_OK);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 32);

    EXPECT_EQ(anetParseSubnet(err, "0.0.0.0/0", &subnet), ANET_OK);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 0);

    /* Valid raw IPv4 (no slash) */
    EXPECT_EQ(anetParseSubnet(err, "192.168.1.1", &subnet), ANET_OK);
    EXPECT_EQ(subnet.family, AF_INET);
    EXPECT_EQ(subnet.prefix_len, 32);
    inet_pton(AF_INET, "192.168.1.1", &expected_ip);
    EXPECT_EQ(subnet.addr.ipv4.s_addr, expected_ip);

    /* Invalid IPv4 subnets */
    EXPECT_EQ(anetParseSubnet(err, "1.2.3.4/999", &subnet), ANET_ERR);
    EXPECT_EQ(anetParseSubnet(err, "invalid/24", &subnet), ANET_ERR);
    EXPECT_EQ(anetParseSubnet(err, "1.2.3.4.5/24", &subnet), ANET_ERR);
    EXPECT_EQ(anetParseSubnet(err, "/24", &subnet), ANET_ERR);
    EXPECT_EQ(anetParseSubnet(err, "192.168.1.0/", &subnet), ANET_ERR);
    EXPECT_EQ(anetParseSubnet(err, "192.168.1.0/-1", &subnet), ANET_ERR);
    EXPECT_EQ(anetParseSubnet(err, "192.168.1.0/33", &subnet), ANET_ERR);
    EXPECT_EQ(anetParseSubnet(err, "1.2.3.4/24a", &subnet), ANET_ERR);
}

TEST_F(AnetSubnetTest, ParseSubnetIpv6) {
    anetSubnet subnet;
    char err[ANET_ERR_LEN] = {0};

    /* Valid IPv6 subnets */
    EXPECT_EQ(anetParseSubnet(err, "2001:db8::/32", &subnet), ANET_OK);
    EXPECT_EQ(subnet.family, AF_INET6);
    EXPECT_EQ(subnet.prefix_len, 32);
    struct in6_addr expected_ip;
    inet_pton(AF_INET6, "2001:db8::", &expected_ip);
    EXPECT_EQ(memcmp(&subnet.addr.ipv6, &expected_ip, sizeof(struct in6_addr)), 0);

    EXPECT_EQ(anetParseSubnet(err, "::1/128", &subnet), ANET_OK);
    EXPECT_EQ(subnet.family, AF_INET6);
    EXPECT_EQ(subnet.prefix_len, 128);

    EXPECT_EQ(anetParseSubnet(err, "::/0", &subnet), ANET_OK);
    EXPECT_EQ(subnet.family, AF_INET6);
    EXPECT_EQ(subnet.prefix_len, 0);

    /* Valid raw IPv6 (no slash) */
    EXPECT_EQ(anetParseSubnet(err, "2001:db8::1", &subnet), ANET_OK);
    EXPECT_EQ(subnet.family, AF_INET6);
    EXPECT_EQ(subnet.prefix_len, 128);
    inet_pton(AF_INET6, "2001:db8::1", &expected_ip);
    EXPECT_EQ(memcmp(&subnet.addr.ipv6, &expected_ip, sizeof(struct in6_addr)), 0);

    /* Invalid IPv6 subnets */
    EXPECT_EQ(anetParseSubnet(err, "2001:db8::/129", &subnet), ANET_ERR);
    EXPECT_EQ(anetParseSubnet(err, "invalid/64", &subnet), ANET_ERR);
    EXPECT_EQ(anetParseSubnet(err, "2001:db8::/-1", &subnet), ANET_ERR);
}

TEST_F(AnetSubnetTest, MatchIpSubnetIpv4) {
    anetSubnet subnets[3];
    ASSERT_EQ(anetParseSubnet(NULL, "192.168.1.0/24", &subnets[0]), ANET_OK);
    ASSERT_EQ(anetParseSubnet(NULL, "10.0.0.0/8", &subnets[1]), ANET_OK);
    ASSERT_EQ(anetParseSubnet(NULL, "172.16.0.0/12", &subnets[2]), ANET_OK);

    /* Matches */
    EXPECT_EQ(anetMatchIpSubnet("192.168.1.5", subnets, 3), 1);
    EXPECT_EQ(anetMatchIpSubnet("10.254.0.1", subnets, 3), 1);
    EXPECT_EQ(anetMatchIpSubnet("172.16.10.20", subnets, 3), 1);
    EXPECT_EQ(anetMatchIpSubnet("172.31.255.254", subnets, 3), 1);

    /* Mismatches */
    EXPECT_EQ(anetMatchIpSubnet("192.168.2.5", subnets, 3), 0);
    EXPECT_EQ(anetMatchIpSubnet("11.0.0.1", subnets, 3), 0);
    EXPECT_EQ(anetMatchIpSubnet("172.32.0.1", subnets, 3), 0);
    EXPECT_EQ(anetMatchIpSubnet("invalid-ip", subnets, 3), 0);
}

TEST_F(AnetSubnetTest, MatchIpSubnetIpv6) {
    anetSubnet subnets[3];
    ASSERT_EQ(anetParseSubnet(NULL, "2001:db8::/32", &subnets[0]), ANET_OK);
    ASSERT_EQ(anetParseSubnet(NULL, "::1/128", &subnets[1]), ANET_OK);
    ASSERT_EQ(anetParseSubnet(NULL, "fe80::/10", &subnets[2]), ANET_OK);

    /* Matches */
    EXPECT_EQ(anetMatchIpSubnet("2001:db8:abcd::1", subnets, 3), 1);
    EXPECT_EQ(anetMatchIpSubnet("::1", subnets, 3), 1);
    EXPECT_EQ(anetMatchIpSubnet("fe80::1ff:fe23:4567:890a", subnets, 3), 1);

    /* Mismatches */
    EXPECT_EQ(anetMatchIpSubnet("2001:db9::1", subnets, 3), 0);
    EXPECT_EQ(anetMatchIpSubnet("::2", subnets, 3), 0);
    EXPECT_EQ(anetMatchIpSubnet("fec0::1", subnets, 3), 0);
    EXPECT_EQ(anetMatchIpSubnet("invalid-ip", subnets, 3), 0);
}

TEST_F(AnetSubnetTest, MatchIpSubnetEdgeCases) {
    anetSubnet subnets[2];
    ASSERT_EQ(anetParseSubnet(NULL, "0.0.0.0/0", &subnets[0]), ANET_OK);
    ASSERT_EQ(anetParseSubnet(NULL, "::/0", &subnets[1]), ANET_OK);

    /* Any IPv4 matches 0.0.0.0/0 */
    EXPECT_EQ(anetMatchIpSubnet("192.168.1.1", &subnets[0], 1), 1);
    EXPECT_EQ(anetMatchIpSubnet("8.8.8.8", &subnets[0], 1), 1);
    EXPECT_EQ(anetMatchIpSubnet("::1", &subnets[0], 1), 0);

    /* Any IPv6 matches ::/0 */
    EXPECT_EQ(anetMatchIpSubnet("2001:db8::1", &subnets[1], 1), 1);
    EXPECT_EQ(anetMatchIpSubnet("::1", &subnets[1], 1), 1);
    EXPECT_EQ(anetMatchIpSubnet("192.168.1.1", &subnets[1], 1), 0);

    /* NULL IP (non-IP transports like UNIX domain sockets) safely returns 0 */
    EXPECT_EQ(anetMatchIpSubnet(NULL, subnets, 2), 0);
    EXPECT_EQ(anetMatchIpSubnet("192.168.1.1", NULL, 0), 0);
    EXPECT_EQ(anetMatchIpSubnet(NULL, NULL, 0), 0);
}

TEST_F(AnetSubnetTest, Ipv4MappedIpv6DualStack) {
    anetSubnet subnet;
    ASSERT_EQ(anetParseSubnet(NULL, "192.168.1.0/24", &subnet), ANET_OK);

    /* IPv4-mapped IPv6 address ::ffff:192.168.1.5 matches IPv4 subnet */
    EXPECT_EQ(anetMatchIpSubnet("::ffff:192.168.1.5", &subnet, 1), 1);
    EXPECT_EQ(anetMatchIpSubnet("::ffff:192.168.2.5", &subnet, 1), 0);
}

TEST_F(AnetSubnetTest, ValidateAndUpdatePrioritySubnets) {
    const char *err = NULL;
    EXPECT_EQ(validatePrioritySubnets("192.168.1.0/24, 10.0.0.0/8", &err), C_OK);
    EXPECT_EQ(err, nullptr);

    EXPECT_EQ(validatePrioritySubnets("192.168.1.0/24, invalid-ip", &err), C_ERR);
    EXPECT_NE(err, nullptr);

    /* Test updating global server configuration */
    EXPECT_EQ(updatePrioritySubnets("192.168.1.0/24 10.0.0.0/8"), C_OK);
    EXPECT_EQ(server.priority_subnets_count, 2);
    ASSERT_NE(server.priority_subnets_array, nullptr);
    EXPECT_EQ(anetMatchIpSubnet("192.168.1.100", server.priority_subnets_array, server.priority_subnets_count), 1);
    EXPECT_EQ(anetMatchIpSubnet("172.16.1.1", server.priority_subnets_array, server.priority_subnets_count), 0);

    /* Clear */
    EXPECT_EQ(updatePrioritySubnets(NULL), C_OK);
    EXPECT_EQ(server.priority_subnets_count, 0);
    EXPECT_EQ(server.priority_subnets_array, nullptr);
}
