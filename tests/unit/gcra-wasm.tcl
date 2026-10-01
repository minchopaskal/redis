# The fixture is built from extensions/gcra/gcra.c with its Makefile.
start_server {tags {"wasm scripting external:skip"}} {
    if {![dict exists [r function stats] engines WASM]} {
        test {GCRA autoload - non-WASM builds reject a nonempty extension-dir} {
            set failed [catch {exec src/redis-server --extension-dir /tmp 2>@1} output]
            assert_equal 1 $failed
            assert_match {*extension-dir requires BUILD_WASM=yes*} $output
        }
        return
    }
    test {WASM GCRA - empty extension-dir disables autoload} {
        assert_equal {} [r function list]
        assert_equal {{}} [r command info GCRA]
    }
}
set gcra_extension [file normalize extensions/gcra.wasm]
set gcra_dir [file dirname $gcra_extension]
set default_dir [file normalize [tmpdir wasm-default]]
file mkdir "$default_dir/extensions"
file copy $gcra_extension "$default_dir/extensions/gcra.wasm"
start_server [list tags {wasm scripting external:skip} omit {extension-dir} \
    overrides [list dir $default_dir]] {
    test {WASM autoload - default directory loads shipped extensions without configuration} {
        assert_equal ./extensions [lindex [r config get extension-dir] 1]
        assert_equal 1 [llength [r function list]]
        assert_equal 0 [lindex [r GCRA rl:default 0 1 600] 0]
        r config rewrite
        restart_server 0 true false
        assert_equal 0 [lindex [r GCRA rl:default-restart 0 1 600] 0]
    }
}
start_server [list tags {wasm scripting external:skip} \
    overrides [list dir $default_dir extension-dir extensions]] {
    test {WASM autoload - a relative directory override is accepted} {
        assert_equal extensions [lindex [r config get extension-dir] 1]
        assert_equal 0 [lindex [r GCRA rl:relative 0 1 600] 0]
    }
}
test {WASM autoload - invalid directories and binaries fail startup} {
    set tempdir [file normalize [tmpdir gcra-autoload-invalid]]
    # Missing directories and non-directory paths are errors, not empty scans.
    foreach invalid_dir [list "$tempdir/missing" [file normalize extensions/gcra/README.md]] {
        set failed [catch {exec src/redis-server --port 0 --unixsocket "$tempdir/redis.sock" \
            --dir $tempdir --save "" --extension-dir $invalid_dir 2>@1} output]
        assert_equal 1 $failed
        assert_match {*Cannot scan extension directory*} $output
    }
    file copy [file normalize extensions/gcra/README.md] "$tempdir/invalid.wasm"
    set failed [catch {exec src/redis-server --port 0 --unixsocket "$tempdir/redis.sock" \
        --dir $tempdir --save "" --extension-dir $tempdir 2>@1} output]
    assert_equal 1 $failed
    assert_match {*Cannot read valid WASM extension*invalid.wasm*} $output

    set fp [open "$tempdir/nul.conf" w]
    puts $fp "extension-dir \"$gcra_dir\\x00suffix\""
    close $fp
    set failed [catch {exec src/redis-server "$tempdir/nul.conf" 2>@1} output]
    assert_equal 1 $failed
    assert_match {*extension-dir must not contain NUL bytes*} $output
}

set empty_dir [file normalize [tmpdir wasm-empty]]
start_server [list tags {wasm scripting external:skip} overrides [list extension-dir $empty_dir]] {
    test {WASM autoload - an empty directory is valid and does not require GCRA} {
        assert_equal {} [r function list]
        assert_equal {{}} [r command info GCRA]
    }
}
test {WASM autoload - special files fail without blocking} {
    set tempdir [file normalize [tmpdir wasm-fifo]]
    exec mkfifo "$tempdir/pipe.wasm"
    set failed [catch {exec src/redis-server --port 0 --unixsocket "$tempdir/redis.sock" \
        --dir $tempdir --save "" --extension-dir $tempdir 2>@1} output]
    assert_equal 1 $failed
    assert_match {*Extension*pipe.wasm is not a regular file*} $output
}
test {WASM GCRA - invalid startup files fail clearly} {
    set tempdir [file normalize [tmpdir gcra-startup]]
    foreach path [list "$tempdir/missing.wasm" [file normalize extensions/gcra/README.md]] {
        set failed [catch {exec src/redis-server --port 0 --unixsocket "$tempdir/redis.sock" \
            --dir $tempdir --save "" --extension-dir "" --loadextension $path 2>@1} output]
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
        --dir $tempdir --save "" --extension-dir $gcra_dir --loadextension $changed 2>@1} output]
    assert_equal 1 $failed
    assert_match {*already exists*} $output
}
start_server [list tags {wasm scripting external:skip} overrides [list extension-dir $gcra_dir]] {
    test {WASM GCRA - autoload exposes GCRA without loadextension} {
        assert_equal 1 [llength [r function list]]
        assert_equal $gcra_dir [lindex [r config get extension-dir] 1]
        assert_error {*immutable*} {r config set extension-dir ""}
    }
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
        assert_equal $gcra_dir [lindex [r config get extension-dir] 1]
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

set installed_dir "[file normalize [tmpdir gcra-installed]]/extensions with spaces"
file mkdir $installed_dir
# Create files in reverse order; neither creation order nor a special GCRA
# basename should affect discovery. Ordering is bytewise, not case-insensitive.
file copy $gcra_extension "$installed_dir/z-limit.wasm"
file copy [file normalize tests/assets/wasm/functions-poc.wasm] "$installed_dir/a-functions.wasm"
file copy [file normalize tests/assets/wasm/commands.wasm] "$installed_dir/B-commands.wasm"
file link -symbolic "$installed_dir/duplicate.wasm" "$installed_dir/a-functions.wasm"
file copy [file normalize extensions/gcra/README.md] "$installed_dir/ignored.txt"
file copy [file normalize extensions/gcra/README.md] "$installed_dir/ignored.WASM"
file mkdir "$installed_dir/nested.wasm"
file copy [file normalize extensions/gcra/README.md] "$installed_dir/nested.wasm/invalid.wasm"
start_server [list tags {wasm scripting external:skip} overrides [list extension-dir "\"$installed_dir\""] \
    config_lines [list loadextension $gcra_extension loadextension $gcra_extension \
        loadextension [file normalize tests/assets/wasm/integration.wasm]]] {
    test {WASM autoload - multiple extensions load in bytewise filename order} {
        assert_equal 4 [llength [r function list]]
        assert_equal {hello from wasm} [r fcall hello 0]
        assert_equal OK [r wasm_zero]
        assert_equal OK [r abi_init]
        set loaded {}
        foreach line [split [exec cat [srv 0 stdout]] "\n"] {
            if {[regexp {Loaded extension (.*) as wasm_[0-9a-f]+} $line -> filename]} {
                lappend loaded [file tail $filename]
            }
        }
        assert_equal {B-commands.wasm a-functions.wasm duplicate.wasm z-limit.wasm} [lrange $loaded 0 3]
    }
    test {WASM autoload - duplicate and additional loadextension directives survive CONFIG REWRITE} {
        assert_equal 0 [lindex [r GCRA rl:installed 0 1 600] 0]
        r config rewrite
        restart_server 0 true false
        assert_equal $installed_dir [lindex [r config get extension-dir] 1]
        assert_equal 4 [llength [r function list]]
        assert_equal {hello from wasm} [r fcall hello 0]
        assert_equal OK [r wasm_zero]
        assert_equal OK [r abi_init]
        assert_equal 0 [lindex [r GCRA rl:installed-restart 0 1 600] 0]
    }
}

start_server [list tags {wasm scripting external:skip} overrides [list loadextension $gcra_extension]] {
    test {WASM GCRA - explicit loadextension still works with autoload disabled} {
        assert_equal {} [lindex [r config get extension-dir] 1]
        assert_equal 0 [lindex [r GCRA rl:explicit 0 1 600] 0]
        r config rewrite
        restart_server 0 true false
        assert_equal 1 [llength [r function list]]
        assert_equal 0 [lindex [r GCRA rl:explicit-restart 0 1 600] 0]
    }
}

start_server [list tags {wasm scripting external:skip} overrides [list extension-dir $gcra_dir appendonly yes aof-use-rdb-preamble no]] {
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

start_server [list tags {wasm scripting external:skip} overrides [list extension-dir $gcra_dir]] {
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
