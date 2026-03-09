#!/usr/bin/perl

# A tool for simulating the traffic of a Valkey node.
#
# Copyright 2020 Ericsson Software Technology <viktor.soderqvist@est.tech>
#
# Copying and distribution of this file, with or without modification,
# are permitted in any medium without royalty provided the copyright
# notice and this notice are preserved.  This file is offered as-is,
# without any warranty.

use strict;
use warnings;
use Socket;
use JSON;

my $port = 7000;
my $debug = 0;
my ($sig, $sig_pid);
my $use_ipv6 = 0;

# Parse command line args
while ($_ = shift) {
    if (/^(?:-p|--port)$/) {
        $port = shift;
        die "Bad port: $port\n" unless $port > 0;
    } elsif (/^--sig(cont|alrm|int|hup|term)$/) {
        $sig = uc $1;
        $sig_pid = shift;
        die "Pid expected after $_\n" unless $sig_pid;
    } elsif (/^--ipv6$/) {
        $use_ipv6 = 1;
    } elsif (/^(?:-d|--debug)$/) {
        $debug = 1;
    } elsif (/^(?:-h|--help)$/) {
        map { print "$_\n" }
            "Usage: $0 [ OPTIONS ]",
            "",
            "Acts as a Valkey node, communicating with clients according to",
            "expected traffic provided as events on stdin. Multiple connections",
            "are accepted, but data can only be sent to and received from the",
            "last accepted client, referred to as \"the client\" below.",
            "",
            "Options:",
            "",
            "  -p PORT, --port PORT   TCP port to use.",
            "           --ipv6        Use IPv6, bind to address '::'.",
            "           --sigSIG PID  Send SIG to PID when ready to accept a",
            "                         client connection.",
            "  -d,      --debug       Enable debug printouts.",
            "  -h,      --help        Help.",
            "",
            "Expected traffic is to be provided on stdin, one event per line,",
            "where the following events are accepted:",
            "",
            "  EXPECT CONNECT         Wait for a client to connect.",
            "  EXPECT CLOSE           Wait for the client to close the connection.",
            "  EXPECT command         Receive expected command from the client.",
            "  SEND response          Send response to the client.",
            "  CLOSE                  Close the connection to the client.",
            "  SLEEP n                Sleep n seconds.",
            "",
            "The command and response in the events above is provided in a subset",
            "of Perl syntax. Examples:",
            "",
            "  EXPECT [\"GET\", \"foo\"]",
            "  SEND \"bar\"",
            "",
            "The response (after SEND) can also be represented in raw backslash-",
            "escaped Valkey protocol data, optionally without the final '\\r\\n'.",
            "Examples:",
            "",
            "  SEND +OK",
            "  SEND -ERR Some error",
            "  SEND \$3\\r\\nfoo\\r\\n";
        exit;
    } else {
        die "Bad option: $_\n";
    }
}

# Use IPv4 default
my $af = AF_INET;
my $sockaddr = \&sockaddr_in;
my $server_addr = INADDR_ANY;

if ($use_ipv6) {
    $af = AF_INET6;
    $sockaddr = \&sockaddr_in6;
    $server_addr = Socket::IN6ADDR_ANY;
}

# Listener socket. Close it on SIGTERM, etc. and at normal exit.
my $listener;
END {
    close $listener if $listener;
}

socket($listener, $af, SOCK_STREAM, getprotobyname("tcp"))
    or die "socket: $!\n";
setsockopt($listener, SOL_SOCKET, SO_REUSEADDR, pack("l", 1))
    or die "setsockopt: $!\n";
bind($listener, &$sockaddr($port, $server_addr))
    or die "bind: $!\n";
listen($listener, 5)
    or die "listen: $!\n";
print "(port $port) Listening.\n" if $debug;
kill $sig, $sig_pid if $sig_pid;

# Accept multiple connections, but only the last one is used.
my @connections = ();
my $connection; # Active connection = the last element of @connections

# Loop over events on stdin.
while (<>) {
    s/#.*//; # trim trailing comments
    s/^\s+//; # trim leading whitespace
    s/\s+$//; # trim trailing whitespace
    next if /^$/; # skip empty lines
    print "(port $port) $_\n" if $debug;
    if (/^SEND (.*)/) {
        my $data = $1;
        if ($data =~ /^[-+\$\*:]/) {
            # Valkey protocol with character escapes
            # e.g. '-ERR Unknown command: FOO\r\n'
            $data = unescape($data);
            $data .= "\r\n" unless $data =~ /\r\n$/;
        } else {
            # e.g. '["foo", "bar", 42]'
            $data = valkey_encode(JSON->new->allow_nonref->decode($1));
        }
        print $connection $data;
        flush $connection;
    } elsif (/^CLOSE$/) {
        close $connection;
        pop @connections;
        $connection = $connections[-1];
    } elsif (/^EXPECT CLOSE$/) {
        my $buffer;
        my $bytes_read = read $connection, $buffer, 1;
        die "(port $port) Data received from peer when close is expected.\n"
            if $bytes_read;
        print "(port $port) Client disconnected.\n" if $debug;
        close $connection;
        pop @connections;
        $connection = $connections[-1];
    } elsif (/^EXPECT CONNECT$/) {
        undef $connection;
        my $peer_addr = accept($connection, $listener);
        push @connections, $connection;
        my($client_port, $client_addr) = &$sockaddr($peer_addr);
        my $name = gethostbyaddr($client_addr, $af);
        print "(port $port) Connection from $name [", Socket::inet_ntop($af, $client_addr),
            "] on client port $client_port.\n" if $debug;
    } elsif (/^EXPECT ([\[\"].*)/) {
        my $expected = eval $1;
        my $received = recv_command($connection);
        my $expected_str = pretty_format_command($expected);
        my $received_str = pretty_format_command($received);
        if ($expected_str ne $received_str) {
            unexpected($port, "$received_str received.\nExpected $expected_str");
        }
    } elsif (/^SLEEP (\d+)$/) {
        sleep $1;
    } else {
        unexpected($port, "event: $_");
    }
}
close $listener;
print "(port $port) Done.\n" if $debug;
exit;

sub unexpected {
    my ($port, $unexpected) = @_;
    print "(port $port) Unexpected $unexpected\n";
    die "Unexpected communication\n";
}

sub valkey_encode {
    my $x = shift;
    return ("*" . @$x . "\r\n" . join '', map { valkey_encode($_) } @$x)
        if ref $x eq "ARRAY";
    return ":$x\r\n"
        unless ($x ^ $x) ne "0"; # hack to check if int or string
    utf8::encode $x;
    return "\$" . length($x) . "\r\n$x\r\n";
}

sub recv_command {
    my $connection = shift;
    my $old_rec_sep = $/;
    $/ = "\r\n";
    $_ = <$connection>;
    die "(port $port) The peer has closed the connection\n" if !defined $_;
    my $result;
    if (/^\*(\d+)\r\n$/) {
        my $n = $1;
        $result = [];
        for (my $i = 0; $i < $n; $i++) {
            push @$result, recv_command($connection);
        }
    } elsif (/^\$0\r\n$/) {
        $result = "";
        $_ = <$connection>;
        die "(port $port) Expected \\r\\n after empty string\n"
            unless /^\r\n$/;
    } elsif (/^\$(\d+)\r\n$/) {
        my $remaining = $1;
        $result = "";
        my $buffer;
        do {
            my $read = read $connection, $buffer, $remaining;
            unexpected($port, "EOF while receiving command")
                unless $read;
            $result .= $buffer;
            $remaining -= $read;
        } while ($remaining > 0);
        $_ = <$connection>;
        unexpected($port, "Expected \\r\\n after string") unless /^\r\n$/
    } else {
        die "Unexpected command: $_\n";
    }
    $/ = $old_rec_sep;
    return $result;
}

# ["GET", "foo\tbar"] => '["GET", "foo\tbar"]'
sub pretty_format_command {
    my $x = shift;
    return "[" . join(", ", map { pretty_format_command($_) } @$x) . "]"
        if ref $x eq "ARRAY";
    return '"' . escape($x) . '"'
        if ref $x eq "";
    die "Can't serialize " . ref $x . " $x\n";
}

# Escape special chars to binary data readable
# "hello\r\n" => "hello\\r\\n"
sub escape {
    $_ = shift;
    s/([^A-Za-z0-9\/\-_.,:;!?~\*'(){}\^\$])/ escape_char($1) /eg;
    return $_;
}
sub escape_char {
    $_ = shift;
    return "\\r" if /\r/; # nicer than \x0d, etc.
    return "\\n" if /\n/;
    return "\\t" if /\t/;
    return '\\"' if /\"/;
    return "\\\\" if /\\/;
    return sprintf "\\x%02x", ord $_;
}

# "hello\\r\\n" => "hello\r\n"
sub unescape {
    $_ = shift;
    s/\\([tnrfbae\\]|x\{[0-9a-fA-F]+\}|x[0-9a-fA-F]{1,2}|0[0-7]{0,2})/eval "\"\\$1\""/eg;
    return $_;
}
