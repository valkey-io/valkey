# This test suite requires a fixture RDB file named "set_listpack_fixture.rdb".
# This fixture must contain a Redis Set object (e.g., key "myset_lp_test_key")
# that was saved using the RDB_TYPE_SET_LISTPACK encoding by a newer Redis version (e.g., 7.2+).
#
# Example to generate set_listpack_fixture.rdb:
# 1. In a Redis 7.2+ CLI:
#    SADD myset_lp_test_key "alpha" "beta" "gamma" "123" "sml"
#    SAVE
# 2. Copy the generated dump.rdb to "set_listpack_fixture.rdb" in the test directory.

tags {"rdb external:skip"} {

# Copy RDB with listpack encoded set to server path
set server_path [tmpdir "server.rdb-encoding-listpack-test"]
exec cp -f tests/assets/set_listpack_fixture.rdb $server_path

start_server [list overrides [list "dir" $server_path "dbfilename" "set_listpack_fixture.rdb" "rdb-version-check" "relaxed"]] {

    # Test case: Load an RDB file containing a set encoded as RDB_TYPE_SET_LISTPACK.
    # This tests the server's ability to correctly parse RDB_TYPE_SET_LISTPACK
    # and convert it into its native set representation (e.g., hashtable or intset).
   test "Patched Redis should successfully load RDB_TYPE_SET_LISTPACK" {
           set test_key_name "myset_lp_test_key"
           set expected_elements {"alpha" "beta" "gamma" "123" "sml"}
           set expected_cardinality [llength $expected_elements]

           r select 0

           # 1. Verify the key NOW EXISTS because the patch should handle it.
           assert_equal 1 [r exists $test_key_name] "Key '$test_key_name' SHOULD exist after loading"

           # 2. Verify the set's cardinality.
           assert_equal $expected_cardinality [r scard $test_key_name] "Cardinality mismatch for '$test_key_name' with patched Redis"

           # 3. Verify individual members.
           foreach element $expected_elements {
               assert_equal 1 [r sismember $test_key_name $element] "Element '$element' missing from '$test_key_name' with patched Redis"
           }
           assert_equal 0 [r sismember $test_key_name "nonexistent_element"] "Non-existent element check failed for '$test_key_name' with patched Redis"

           # 4. Verify all members using SMEMBERS.
           set loaded_members [lsort [r smembers $test_key_name]]
           set expected_members_sorted [lsort $expected_elements]
           assert_equal $expected_members_sorted $loaded_members "SMEMBERS content mismatch for '$test_key_name' with patched Redis"

           # 5. Verify the internal encoding of the loaded set.
           # After being loaded by the (patched) older Redis, the set should now be
           # in one of the older Redis's native encodings (e.g., "hashtable" or "intset").
           # For the mixed elements "alpha", "123", etc., it should be "hashtable".
           set encoding_info [r debug object $test_key_name]
           assert_match {*encoding:hashtable*} $encoding_info "Set '$test_key_name' encoding is not 'hashtable' after RDB load by patched Redis. Actual: $encoding_info"

           # 6. Ensure no critical errors were logged regarding this type (optional, but good).
           # This is the opposite of the incompatibility test. We expect no "Unknown RDB type 18" error.
           # This might require a helper like `assert_log_does_not_contain` or checking log length.
           # For simplicity, this explicit check is omitted here, but successful loading implies no fatal RDB error.

           # Clean up the key for subsequent tests if any.
           r del $test_key_name
       }
}
}