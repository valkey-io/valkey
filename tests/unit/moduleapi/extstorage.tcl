start_server {tags {"external_data external:skip"}} {
    test {Running EXTERNAL_DATA LOADED with switched off external data fails} {
        assert_error {ERR External data commands are unavailable with ext-data-mode off} {r external_data loaded storage}
    }
}

start_server [list overrides [list "ext-data-mode" test] tags [list "external:skip"]] {
    test {Running EXTERNAL_DATA LOADED with switched on external data succeeds} {
        assert_equal [list ] [r external_data loaded storage]
        assert_equal [list ] [r external_data loaded filter]
        assert_error {ERR Unknown module type (storage or filter expected)} {r external_data loaded anything}
    }
}
