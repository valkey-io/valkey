start_server {} {
    test "info command - version and get-sha1" {
	set i [r info]
	regexp {redis_version:(.*?)\r\n} $i - version
	regexp {server_git_sha1:(.*?)\r\n} $i - sha1
	puts "Testing Redis version $version ($sha1)"
    }
}
