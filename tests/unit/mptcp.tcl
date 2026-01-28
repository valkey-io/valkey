start_server {tags {"mptcp" "external:skip"} overrides {mptcp yes}} {
    test "MPTCP - Server starts with mptcp yes and responds to commands" {
        assert_equal "PONG" [r ping]
        assert_equal {mptcp yes} [r config get mptcp]
        r set test_key "test_value"
        assert_equal "test_value" [r get test_key]
        r del test_key
        assert_equal 0 [r exists test_key]
    }
}
