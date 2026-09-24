# The fixture is built from extensions/gcra/gcra.c with its Makefile.
start_server {tags {"wasm scripting external:skip"}} {
    if {![dict exists [r function stats] engines WASM]} { return }
}
set gcra_extension [file normalize extensions/gcra/gcra.wasm]
test {WASM GCRA - invalid startup files fail clearly} {
    set tempdir [file normalize [tmpdir gcra-startup]]
    foreach path [list "$tempdir/missing.wasm" [file normalize extensions/gcra/README.md]] {
        set failed [catch {exec src/redis-server --port 0 --unixsocket "$tempdir/redis.sock" \
            --dir $tempdir --save "" --loadextension $path 2>@1} output]
        assert_equal 1 $failed
        assert_match {*extension*} $output
    }
    # Same exports, different module bytes: content identity must not silently
    # replace an already configured function.
    set fp [open $gcra_extension rb]
    set bytes [read $fp]
    close $fp
    append bytes [binary format H* 00050362616458]
    set changed "$tempdir/changed.wasm"
    set fp [open $changed wb]
    puts -nonewline $fp $bytes
    close $fp
    set failed [catch {exec src/redis-server --port 0 --unixsocket "$tempdir/redis.sock" \
        --dir $tempdir --save "" --loadextension $gcra_extension --loadextension $changed 2>@1} output]
    assert_equal 1 $failed
    assert_match {*already exists*} $output
}
start_server [list tags {wasm scripting external:skip} overrides [list loadextension $gcra_extension]] {
    test {WASM GCRA - preloaded function consumes burst then rejects} {
        assert_equal {0 3 2 -1 60} [r fcall GCRA 1 rl:burst 2 1 60]
        assert_equal {0 3 1 -1 120} [r fcall GCRA 1 rl:burst 2 1 60]
        assert_equal {0 3 0 -1 180} [r fcall GCRA 1 rl:burst 2 1 60]
        set before [r dump rl:burst]
        assert_equal {1 3 0 60 180} [r fcall GCRA 1 rl:burst 2 1 60]
        assert_equal $before [r dump rl:burst]
        assert_equal wasm-blob [r type rl:burst]
        assert_morethan [r pttl rl:burst] 0
    }
    test {WASM GCRA - weighted requests and impossible cost} {
        assert_equal {0 5 2 -1 180} [r fcall GCRA 1 rl:weighted 4 1 60 TOKENS 3]
        assert_equal {1 5 5 -1 0} [r fcall GCRA 1 rl:impossible 4 1 60 TOKENS 6]
        assert_equal 0 [r exists rl:impossible]
    }
    test {WASM GCRA - rejected and invalid calls preserve state} {
        r set rl:string value
        assert_error {*WRONGTYPE*} {r fcall GCRA 1 rl:string 2 1 60}
        assert_equal value [r get rl:string]
        assert_error {*Invalid GCRA*} {r fcall GCRA 1 rl:invalid 1 0 1}
        assert_error {*overflow*} {r fcall GCRA 1 rl:invalid 9223372036854775806 1 60}
        assert_error {*TOKENS*} {r fcall GCRA 1 rl:invalid 1 1 1 TOKENS 0}
        assert_equal 0 [r exists rl:invalid]
    }
    test {WASM GCRA - expiration refills capacity} {
        assert_equal 0 [lindex [r fcall GCRA 1 rl:short 0 1 0.05] 0]
        wait_for_condition 100 10 {![r exists rl:short]} else {fail "GCRA key failed to expire"}
        assert_equal 0 [lindex [r fcall GCRA 1 rl:short 0 1 0.05] 0]
    }
    test {WASM GCRA - partial refill permits retry while key remains alive} {
        r fcall GCRA 1 rl:refill 2 1 0.1 TOKENS 3
        wait_for_condition 100 10 {
            [lindex [r fcall GCRA 1 rl:refill 2 1 0.1] 0] == 0
        } else {fail "GCRA failed to refill"}
        assert_morethan [r pttl rl:refill] 0
    }
    test {WASM GCRA - native implementation parity when available} {
        if {[llength [r command info GCRA]] && [lindex [r command info GCRA] 0] ne {}} {
            for {set i 0} {$i < 5} {incr i} {
                assert_equal [r GCRA rl:native 2 1 60] [r fcall GCRA 1 rl:wasm 2 1 60]
            }
        }
    }
    test {WASM GCRA - state survives RDB restart and preload is idempotent} {
        r save
        restart_server 0 true false
        assert_equal 1 [llength [r function list]]
        assert_equal 1 [lindex [r fcall GCRA 1 rl:burst 2 1 60] 0]
    }
    test {WASM GCRA - CONFIG REWRITE retains preload configuration} {
        assert_equal OK [r config rewrite]
        restart_server 0 true false
        assert_equal 1 [llength [r function list]]
        assert_equal 0 [lindex [r fcall GCRA 1 rl:rewrite 0 1 60] 0]
    }
    test {WASM GCRA - ACL failure leaves no rate-limit key} {
        r acl setuser limiter on >pass ~rl:* +fcall +time +exists +type
        set c [redis [srv 0 host] [srv 0 port]]
        $c auth limiter pass
        assert_error {*permission*} {$c fcall GCRA 1 rl:acl 0 1 60}
        $c close
        assert_equal 0 [r exists rl:acl]
    }
}

start_server [list tags {wasm scripting external:skip} overrides [list loadextension $gcra_extension appendonly yes aof-use-rdb-preamble no]] {
    test {WASM GCRA - AOF restart restores state with configured extension} {
        assert_equal 0 [lindex [r fcall GCRA 1 rl:aof 0 1 600] 0]
        restart_server 0 true false
        assert_equal 1 [lindex [r fcall GCRA 1 rl:aof 0 1 600] 0]
        r bgrewriteaof
        waitForBgrewriteaof r
        restart_server 0 true false
        assert_equal 1 [lindex [r fcall GCRA 1 rl:aof 0 1 600] 0]
    }
}

start_server [list tags {wasm scripting external:skip} overrides [list loadextension $gcra_extension]] {
    start_server {tags {wasm scripting external:skip}} {
        test {WASM GCRA - module and state replicate} {
            r replicaof [srv -1 host] [srv -1 port]
            wait_for_condition 100 100 {[s master_link_status] eq "up"} else {fail "Replica failed to connect"}
            r -1 fcall GCRA 1 rl:repl 0 1 600
            wait_for_condition 100 100 {[r exists rl:repl]} else {fail "Blob failed to replicate"}
            assert_equal [r -1 dump rl:repl] [r dump rl:repl]
            assert_morethan [r pttl rl:repl] 0
            assert_error {*read only replica*} {r fcall GCRA 1 rl:readonly 0 1 600}
            assert_equal 0 [r exists rl:readonly]
            r replicaof no one
            assert_equal 1 [lindex [r fcall GCRA 1 rl:repl 0 1 600] 0]
        }
    }
}
