proc wasm_commands_payload {} {
    set fp [open tests/assets/wasm/commands.wasm rb]
    set code [read $fp]
    close $fp
    return "#!wasm name=commands\n$code"
}

start_server {tags {wasm scripting external:skip}} {
    if {![dict exists [r function stats] engines WASM]} {return}

    test {WASM commands - failed registration never publishes partial commands} {
        set bad [string map {wasm_two shutdown} [wasm_commands_payload]]
        assert_error {*already exists*} {r function load $bad}
        assert_equal {{}} [r command info wasm_zero]
        assert_equal 0 [llength [r function list]]
    }
    test {WASM commands - zero and multiple leading keys} {
        assert_equal commands [r function load [wasm_commands_payload]]
        assert_equal OK [r wasm_zero]
        assert_error {*wrong number*} {r wasm_zero extra}
        assert_equal OK [r wasm_two a b]
        assert_equal {a b} [r command getkeys wasm_two a b more]
        assert_error {*wrong number*} {r wasm_two a}
    }
    test {WASM commands - ACL categories and key permissions apply before execution} {
        r acl setuser limited on >pass ~allowed:* +wasm_two
        set c [redis [srv 0 host] [srv 0 port]]
        $c auth limited pass
        assert_equal OK [$c wasm_two allowed:a allowed:b]
        assert_error {*NOPERM*} {$c wasm_two allowed:a denied:b}
        assert_error {*NOPERM*} {$c wasm_zero}
        $c close
        r acl setuser category on >pass ~* +@scripting
        set dump [r function dump]
        r function flush
        r function restore $dump
        set c [redis [srv 0 host] [srv 0 port]]
        $c auth category pass
        assert_equal OK [$c wasm_two a b]
        $c close
    }
    test {WASM commands - replaced signatures cannot invalidate queued key checks} {
        # Change the unique i32.const -3 arity to -4, keeping the module valid.
        set bad [string map [list [binary format H* 417d] [binary format H* 417c]] [wasm_commands_payload]]
        assert_error {*cannot change until restart*} {r function load replace $bad}
        assert_equal OK [r wasm_two a b]
    }
    test {WASM commands - failed restore preserves current commands} {
        set dump [r function dump]
        assert_error {*already exists*} {r function restore $dump}
        assert_equal OK [r wasm_zero]
        r function restore $dump replace
        assert_equal OK [r wasm_zero]
        r function restore $dump flush
        assert_equal OK [r wasm_zero]
    }
}

set saved_singledb $::singledb
set ::singledb 1
start_server {tags {wasm scripting cluster external:skip} overrides {cluster-enabled yes}} {
    test {WASM commands - cluster routes declared keys and rejects CROSSSLOT} {
        r function load [wasm_commands_payload]
        set slots {}
        for {set i 0} {$i < 16384} {incr i} {lappend slots $i}
        r cluster addslots {*}$slots
        wait_for_condition 100 50 {
            [string match {*cluster_state:ok*} [r cluster info]]
        } else {fail "Cluster failed to become ready"}
        assert_equal OK [r wasm_two "{same}:a" "{same}:b"]
        assert_error {*CROSSSLOT*} {r wasm_two a b}
    }
}
set ::singledb $saved_singledb
