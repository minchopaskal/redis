proc wasm_functions_payload {{name wasmlib}} {
    set fp [open "tests/assets/wasm/functions-poc.wasm" rb]
    set module [read $fp]
    close $fp
    return "#!wasm name=$name\n$module"
}

proc go_wasm_functions_payload {{name goexample} {modified 0}} {
    set fp [open "sdk/wasm/go/example/redis-go-example.wasm" rb]
    set module [read $fp]
    close $fp
    if {$modified} {
        # Append a valid custom section. Behavior and exports stay identical,
        # but the exact module payload (and therefore its owner hash) changes.
        append module [binary format H* 00050362616458]
    }
    return "#!wasm name=$name\n$module"
}

proc sdk_wasm_payload {path name} {
    set fp [open $path rb]
    set module [read $fp]
    close $fp
    return "#!wasm name=$name\n$module"
}

start_server {tags {"scripting wasm"}} {
    if {![dict exists [r function stats] engines WASM]} {
        return
    }

    test {WASM FUNCTION - load and list a binary library} {
        assert_equal wasmlib [r function load [wasm_functions_payload]]
        set library [lindex [r function list libraryname wasmlib] 0]
        assert_equal WASM [dict get $library engine]
        assert_equal 10 [llength [dict get $library functions]]
    }

    test {WASM FUNCTION - status and raw bulk replies} {
        assert_equal {hello from wasm} [r fcall hello 0]
        assert_equal hello [r fcall raw 0]
    }

    test {WASM FUNCTION - keys and arguments use the packed input ABI} {
        assert_equal {hello from wasm} [r fcall input 1 k v]
    }

    test {WASM FUNCTION - redis.call executes and propagates writes} {
        assert_equal {hello from wasm} [r fcall set_value 0]
        assert_equal {wasm-value} [r get wasm-key]
    }

    test {WASM FUNCTION - guest error replies reach the caller} {
        catch {r fcall failure 0} err
        assert_match {*wasm failure*} $err
    }

    test {WASM FUNCTION - traps and nonzero returns become errors} {
        catch {r fcall trap 0} trap_err
        assert_match {*WebAssembly function*trapped*unreachable*} $trap_err
        catch {r fcall bad_ptr 0} bounds_err
        assert_match {*WebAssembly function*trapped*out of bounds guest memory access*} $bounds_err
        catch {r fcall bad_return 0} return_err
        assert_match {*WebAssembly function*returned 7*} $return_err
    }

    test {WASM FUNCTION - instruction metering terminates runaway guests} {
        catch {r fcall spin 0} err
        assert_match {*WebAssembly function*trapped*instruction*} $err
    }

    test {WASM FUNCTION - RESP3-only raw reply is rejected for RESP2 clients} {
        catch {r fcall resp3 0} err
        assert_match {*RESP3-only reply for a RESP2 client*} $err
    }

    test {WASM FUNCTION - flagless functions cannot run through FCALL_RO} {
        catch {r fcall_ro hello 0} err
        assert_match {*write flag using *_ro command*} $err
    }

    test {WASM FUNCTION - library survives FUNCTION DUMP and RESTORE} {
        set payload [r function dump]
        r function flush
        r function restore $payload
        assert_equal {hello from wasm} [r fcall hello 0]
    }

    test {WASM FUNCTION - library survives RDB reload} {
        r debug reload
        assert_equal {hello from wasm} [r fcall hello 0]
    } {} {needs:debug}

    test {WASM FUNCTION - malformed modules fail without replacing the library} {
        catch {r function load replace "#!wasm name=wasmlib\nnot wasm"} err
        assert_match {*Error loading WebAssembly module*} $err
        assert_equal {hello from wasm} [r fcall hello 0]
    }

    test {WASM FUNCTION - TinyGo SDK sample executes Redis commands} {
        assert_equal goexample [r function load [go_wasm_functions_payload]]
        assert_equal {stored by Go} [r fcall go_set 1 go:key {value from go}]
        assert_equal {value from go} [r fcall go_get 1 go:key]
    }

    test {WASM BLOB - Go sample stores and updates a binary search tree} {
        assert_equal {tree created} [r fcall tree_create 1 tree 8 3 10 1 6 14 4 7 13]
        assert_equal wasm-blob [r type tree]
        assert_equal 1 [r fcall tree_contains 1 tree 7]
        assert_equal 0 [r fcall tree_contains 1 tree 9]
        assert_equal {tree updated} [r fcall tree_insert 1 tree 9]
        assert_equal 1 [r fcall tree_contains 1 tree 9]
    }

    test {WASM BLOB - COPY and DUMP/RESTORE preserve owner, type, and payload} {
        assert_equal 1 [r copy tree tree-copy]
        assert_equal 1 [r fcall tree_contains 1 tree-copy 14]
        assert_morethan [r memory usage tree] 0
        assert {[lsearch -exact [lindex [r scan 0 type wasm-blob] 1] tree] != -1}
        set payload [r dump tree]
        r del tree
        assert_equal OK [r restore tree 0 $payload]
        assert_equal wasm-blob [r type tree]
        assert_equal 1 [r fcall tree_contains 1 tree 9]
        assert_equal 1 [r copy tree tree-unlink]
        assert_equal 1 [r unlink tree-unlink]
        assert_equal none [r type tree-unlink]
    }

    test {WASM BLOB - blobs and their owning library survive RDB reload} {
        set digest [r debug digest-value tree]
        r debug reload
        assert_equal wasm-blob [r type tree]
        assert_equal 1 [r fcall tree_contains 1 tree 13]
        assert_equal $digest [r debug digest-value tree]
    } {} {needs:debug}

    test {WASM BLOB - module hash, not library name, controls ownership} {
        r function flush
        assert_equal wasm-blob [r type tree]
        assert_equal goexample [r function load [go_wasm_functions_payload goexample 1]]
        catch {r fcall tree_contains 1 tree 13} err
        assert_match {*owned by another module*} $err
        r function flush
        assert_equal renamed [r function load [go_wasm_functions_payload renamed]]
        assert_equal 1 [r fcall tree_contains 1 tree 13]
    }

    test {WASM C SDK - custom linked-list blob example} {
        set payload [sdk_wasm_payload "sdk/wasm/c/example/c-list.wasm" cexample]
        assert_equal cexample [r function load $payload]
        assert_equal {C list created} [r fcall c_list_create 1 c-list first second third]
        assert_equal wasm-blob [r type c-list]
        assert_equal 3 [r fcall c_list_len 1 c-list]
        assert_equal {C list updated} [r fcall c_list_push 1 c-list fourth]
        assert_equal 4 [r fcall c_list_len 1 c-list]
    }
}

start_server {tags {"scripting wasm repl external:skip"}} {
    if {![dict exists [r function stats] engines WASM]} {
        return
    }

    start_server {} {
        test {WASM FUNCTION - connect a replica} {
            r -1 replicaof [srv 0 host] [srv 0 port]
            wait_for_condition 150 100 {
                [s -1 master_link_status] eq {up}
            } else {
                fail "WASM replica did not connect"
            }
        }

        test {WASM FUNCTION - binary library replicates} {
            assert_equal wasmlib [r function load [wasm_functions_payload]]
            wait_for_condition 150 100 {
                [llength [r -1 function list libraryname wasmlib]] == 1
            } else {
                fail "WASM function library did not replicate"
            }
            set library [lindex [r -1 function list libraryname wasmlib] 0]
            assert_equal WASM [dict get $library engine]
        }

        test {WASM FUNCTION - command effects replicate} {
            assert_equal {hello from wasm} [r fcall set_value 0]
            wait_for_condition 150 100 {
                [r -1 get wasm-key] eq {wasm-value}
            } else {
                fail "WASM function effect did not replicate"
            }
        }

        test {WASM BLOB - type and payload replicate as a RESTORE effect} {
            assert_equal goexample [r function load [go_wasm_functions_payload]]
            assert_equal {tree created} [r fcall tree_create 1 repl-tree 5 2 8 1 3 7 9]
            wait_for_condition 150 100 {
                [r -1 type repl-tree] eq {wasm-blob}
            } else {
                fail "WASM blob did not replicate"
            }
            assert_equal [r dump repl-tree] [r -1 dump repl-tree]
        }
    }
}

start_server {tags {"scripting wasm needs:debug"} overrides {appendonly yes aof-use-rdb-preamble no}} {
    if {![dict exists [r function stats] engines WASM]} {
        return
    }

    test {WASM BLOB - AOF rewrite and replay preserve a tree blob} {
        assert_equal goexample [r function load [go_wasm_functions_payload]]
        assert_equal {tree created} [r fcall tree_create 1 aof-tree 20 8 30 4 12 25 40]
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal wasm-blob [r type aof-tree]
        assert_equal 1 [r fcall tree_contains 1 aof-tree 25]
        assert_equal 0 [r fcall tree_contains 1 aof-tree 99]
    }
}
