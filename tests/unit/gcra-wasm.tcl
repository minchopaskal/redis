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
        assert_equal {0 3 2 -1 60} [r GCRA rl:burst 2 1 60]
        assert_equal {0 3 1 -1 120} [r GCRA rl:burst 2 1 60]
        assert_equal {0 3 0 -1 180} [r GCRA rl:burst 2 1 60]
        set before [r dump rl:burst]
        assert_equal {1 3 0 60 180} [r GCRA rl:burst 2 1 60]
        assert_equal $before [r dump rl:burst]
        assert_equal wasm-blob [r type rl:burst]
        assert_morethan [r pttl rl:burst] 0
    }
    test {WASM GCRA - weighted requests and impossible cost} {
        assert_equal {0 5 2 -1 180} [r GCRA rl:weighted 4 1 60 TOKENS 3]
        assert_equal {1 5 5 -1 0} [r GCRA rl:impossible 4 1 60 TOKENS 6]
        assert_equal 0 [r exists rl:impossible]
    }
    test {WASM GCRA - rejected and invalid calls preserve state} {
        r set rl:string value
        assert_error {*WRONGTYPE*} {r GCRA rl:string 2 1 60}
        assert_equal value [r get rl:string]
        assert_error {*Invalid GCRA*} {r GCRA rl:invalid 1 0 1}
        assert_error {*overflow*} {r GCRA rl:invalid 9223372036854775806 1 60}
        assert_error {*TOKENS*} {r GCRA rl:invalid 1 1 1 TOKENS 0}
        assert_equal 0 [r exists rl:invalid]
    }
    test {WASM GCRA - expiration refills capacity} {
        assert_equal 0 [lindex [r GCRA rl:short 0 1 0.05] 0]
        wait_for_condition 100 10 {![r exists rl:short]} else {fail "GCRA key failed to expire"}
        assert_equal 0 [lindex [r GCRA rl:short 0 1 0.05] 0]
    }
    test {WASM GCRA - partial refill permits retry while key remains alive} {
        r GCRA rl:refill 2 1 0.1 TOKENS 3
        wait_for_condition 100 10 {
            [lindex [r GCRA rl:refill 2 1 0.1] 0] == 0
        } else {fail "GCRA failed to refill"}
        assert_morethan [r pttl rl:refill] 0
    }
    test {WASM GCRA - direct command and FCALL share the same implementation} {
        for {set i 0} {$i < 5} {incr i} {
            assert_equal [r GCRA rl:direct 2 1 60] [r fcall GCRA 1 rl:fcall 2 1 60]
        }
    }
    test {WASM GCRA - command metadata, key discovery, and script restrictions} {
        set info [lindex [r command info GCRA] 0]
        assert_equal gcra [lindex $info 0]
        assert_equal -5 [lindex $info 1]
        assert_equal {1 1 1} [lrange $info 3 5]
        assert_equal rl:key [r command getkeys GCRA rl:key 2 1 60]
        assert_error {*wrong number*} {r GCRA rl:key 2 1}
        assert_error {*not allowed from script*} {r eval {return redis.call('GCRA', KEYS[1], 2, 1, 60)} 1 rl:key}
    }
    test {WASM GCRA - deletion, restore, flush, replacement and queued calls} {
        set library [lindex [r function list withcode] 0]
        set name [dict get $library library_name]
        set code [dict get $library library_code]
        set dump [r function dump]
        r multi
        r function delete $name
        r GCRA rl:queued 2 1 60
        assert_error {*no longer registered*} {r exec}
        assert_equal {{}} [r command info GCRA]
        assert_equal OK [r function restore $dump]
        assert_equal 0 [lindex [r GCRA rl:restored 2 1 60] 0]
        assert_equal $name [r function load replace $code]
        assert_equal 0 [lindex [r GCRA rl:replaced 2 1 60] 0]
        # Changing GCRA to PING also changes its exported name, without changing
        # the binary section lengths. The built-in command must not be replaced.
        assert_error {*already exists*} {r function load replace [string map {GCRA PING} $code]}
        assert_equal PONG [r ping]
        assert_equal 0 [lindex [r GCRA rl:rollback 2 1 60] 0]
        r function flush async
        assert_equal {{}} [r command info GCRA]
        r function restore $dump flush
        assert_equal 0 [lindex [r GCRA rl:flush 2 1 60] 0]
    }
    test {WASM GCRA - state survives RDB restart and preload is idempotent} {
        r save
        restart_server 0 true false
        assert_equal 1 [llength [r function list]]
        assert_equal 1 [lindex [r GCRA rl:burst 2 1 60] 0]
    }
    test {WASM GCRA - CONFIG REWRITE retains preload configuration} {
        assert_equal OK [r config rewrite]
        restart_server 0 true false
        assert_equal 1 [llength [r function list]]
        assert_equal 0 [lindex [r GCRA rl:rewrite 0 1 60] 0]
    }
    test {WASM GCRA - ACL failure leaves no rate-limit key} {
        r acl setuser limiter on >pass ~rl:* +GCRA +time +exists +type
        set c [redis [srv 0 host] [srv 0 port]]
        $c auth limiter pass
        assert_error {*permission*} {$c GCRA rl:acl 0 1 60}
        $c close
        assert_equal 0 [r exists rl:acl]
        r acl setuser limiter +restore
        set c [redis [srv 0 host] [srv 0 port]]
        $c auth limiter pass
        assert_equal 0 [lindex [$c GCRA rl:acl 0 1 60] 0]
        assert_error {*NOPERM*} {$c GCRA forbidden 0 1 60}
        assert_error {*NOPERM*} {$c fcall GCRA 1 rl:acl 0 1 60}
        $c close
        # Named command ACL rules must resolve before dataset recovery on startup.
        r config rewrite
        restart_server 0 true false
        set c [redis [srv 0 host] [srv 0 port]]
        $c auth limiter pass
        assert_equal 0 [lindex [$c GCRA rl:acl-restart 0 1 60] 0]
        $c close
    }
}

start_server [list tags {wasm scripting external:skip} overrides [list loadextension $gcra_extension appendonly yes aof-use-rdb-preamble no]] {
    test {WASM GCRA - AOF restart restores state with configured extension} {
        assert_equal 0 [lindex [r GCRA rl:aof 0 1 600] 0]
        restart_server 0 true false
        assert_equal 1 [lindex [r GCRA rl:aof 0 1 600] 0]
        r bgrewriteaof
        waitForBgrewriteaof r
        restart_server 0 true false
        assert_equal 1 [lindex [r GCRA rl:aof 0 1 600] 0]
    }
}

start_server [list tags {wasm scripting external:skip} overrides [list loadextension $gcra_extension]] {
    start_server {tags {wasm scripting external:skip}} {
        test {WASM GCRA - module and state replicate} {
            r replicaof [srv -1 host] [srv -1 port]
            wait_for_condition 100 100 {[s master_link_status] eq "up"} else {fail "Replica failed to connect"}
            r -1 GCRA rl:repl 0 1 600
            wait_for_condition 100 100 {[r exists rl:repl]} else {fail "Blob failed to replicate"}
            assert_equal [r -1 dump rl:repl] [r dump rl:repl]
            assert_morethan [r pttl rl:repl] 0
            assert_error {*read only replica*} {r GCRA rl:readonly 0 1 600}
            assert_equal 0 [r exists rl:readonly]
            r replicaof no one
            assert_equal 1 [lindex [r GCRA rl:repl 0 1 600] 0]
        }
    }
}
