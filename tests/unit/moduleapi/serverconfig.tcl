set testmodule [file normalize tests/modules/serverconfig.so]
set othermodule [file normalize tests/modules/moduleconfigs.so]

start_server {tags {"modules"}} {
    r module load $testmodule
    r module loadex $othermodule CONFIG moduleconfigs.mutable_bool yes

    test {Test server config get with standard Redis configs} {
        # Test getting a standard Redis config
        set port [r config get port]
        assert_match [lindex $port 1] [r serverconfig.get port]

        # Test getting a numeric config
        set maxclients [r config get maxclients]
        assert_equal [lindex $maxclients 1] [r serverconfig.get maxclients]

        # Test getting a string config
        set bind [r config get bind]
        assert_equal [lindex $bind 1] [r serverconfig.get bind]

        # Test getting an enum config
        set appendfsync [r config get appendfsync]
        assert_equal [lindex $appendfsync 1] [r serverconfig.get appendfsync]
    }

    test {Test server config get with non-existent configs} {
        # Test getting a non-existent config
        catch {r serverconfig.get nonexistent_config} err
        assert_match "ERR*" $err
    }

    test {Test server config get with module configs} {
        # Test getting module configs registered by the module
        catch {r serverconfig.get serverconfig.bool} err
        assert_match "yes" $err

        # Test getting module configs registered by another module
        catch {r serverconfig.get moduleconfigs.mutable_bool} err
        assert_match "yes" $err

        # Sanity check
        assert_equal "moduleconfigs.mutable_bool yes" [r config get moduleconfigs.mutable_bool]
    }

    test {Test server config set with standard Redis configs} {
        # Test setting a standard Redis config
        set old_timeout [r config get timeout]
        r serverconfig.set timeout 100
        assert_equal "timeout 100" [r config get timeout]
        r config set timeout [lindex $old_timeout 1]

        # Test setting a numeric config
        set old_maxmemory_samples [r config get maxmemory-samples]
        r serverconfig.set maxmemory-samples 10
        assert_equal "maxmemory-samples 10" [r config get maxmemory-samples]
        r config set maxmemory-samples [lindex $old_maxmemory_samples 1]

        # Test setting an enum config
        set old_appendfsync [r config get appendfsync]
        r serverconfig.set appendfsync always
        assert_equal "appendfsync always" [r config get appendfsync]
        r config set appendfsync [lindex $old_appendfsync 1]
    }

    test {Test server config set with module configs} {
        # Test setting module configs
        assert_equal "OK" [r serverconfig.set serverconfig.bool no]

        # Test setting module configs
        assert_equal "OK" [r serverconfig.set moduleconfigs.mutable_bool no]
    }

    test {Test server config set with error cases} {
        # Test setting a non-existent config
        catch {r serverconfig.set nonexistent_config 123} err
        assert_match "*ERR*" $err

        # Test setting a read-only config
        catch {r serverconfig.set moduleconfigs.immutable_bool 0} err
        assert_match "*ERR*" $err

        # Test setting an enum config with invalid value
        catch {r serverconfig.set moduleconfigs.enum "invalid"} err
        assert_match "*ERR*" $err
    }
}
