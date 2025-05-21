set testmodule [file normalize tests/modules/configaccess.so]
set othermodule [file normalize tests/modules/moduleconfigs.so]

start_server {tags {"modules"}} {
    r module load $testmodule
    r module loadex $othermodule CONFIG moduleconfigs.mutable_bool yes

    test {Test module config get with standard Redis configs} {
        # Test getting standard Redis configs of different types
        set maxmemory [r config get maxmemory]
        assert_equal [lindex $maxmemory 1] [r configaccess.getnumeric maxmemory]

        set port [r config get port]
        assert_equal [lindex $port 1] [r configaccess.getnumeric port]

        set appendonly [r config get appendonly]
        assert_equal [string is true [lindex $appendonly 1]] [r configaccess.getbool appendonly]

        # Test string config
        set logfile [r config get logfile]
        assert_equal [lindex $logfile 1] [r configaccess.getstring logfile]

        # Test SDS config
        set requirepass [r config get requirepass]
        assert_equal [lindex $requirepass 1] [r configaccess.getstring requirepass]

        # Test special config
        set oom_score_adj_values [r config get oom-score-adj-values]
        assert_equal [lindex $oom_score_adj_values 1] [r configaccess.getstring oom-score-adj-values]

        # Test getting enum configs by value and name
        set maxmemory_policy_val [r configaccess.getenumval maxmemory-policy]
        assert {$maxmemory_policy_val >= 0}

        set maxmemory_policy_name [r configaccess.getenumname maxmemory-policy]
        assert_equal [lindex [r config get maxmemory-policy] 1] $maxmemory_policy_name
    }

    test {Test module config get with non-existent configs} {
        # Test getting non-existent configs
        catch {r configaccess.getnumeric nonexistent_config} err
        assert_match "ERR*" $err

        catch {r configaccess.getbool nonexistent_config} err
        assert_match "ERR*" $err

        catch {r configaccess.getstring nonexistent_config} err
        assert_match "ERR*" $err

        catch {r configaccess.getenumval nonexistent_config} err
        assert_match "ERR*" $err
        
        catch {r configaccess.getenumname nonexistent_config} err
        assert_match "ERR*" $err
    }

    test {Test module config set with standard Redis configs} {
        # Test setting numeric config
        set old_maxmemory_samples [r config get maxmemory-samples]
        r configaccess.setnumeric maxmemory-samples 10
        assert_equal "maxmemory-samples 10" [r config get maxmemory-samples]
        r config set maxmemory-samples [lindex $old_maxmemory_samples 1]

        # Test setting bool config
        set old_protected_mode [r config get protected-mode]
        r configaccess.setbool protected-mode 0
        assert_equal "protected-mode no" [r config get protected-mode]
        r config set protected-mode [lindex $old_protected_mode 1]

        # Test setting string config
        set old_masteruser [r config get masteruser]
        r configaccess.setstring masteruser "__newmasteruser__"
        assert_equal "__newmasteruser__" [lindex [r config get masteruser] 1]
        r config set masteruser [lindex $old_masteruser 1]

        # Test setting enum config by value
        set old_loglevel [r config get loglevel]
        r config set loglevel "notice" ; # Set to some value we are sure is different than the one tested
        r configaccess.setenumval loglevel 3
        assert_equal "loglevel warning" [r config get loglevel]

        # Test setting enum config by name
        r configaccess.setenumname loglevel notice
        assert_equal "loglevel notice" [r config get loglevel]
        r config set loglevel [lindex $old_loglevel 1]
    }

    test {Test module config set with module configs} {
        # Test setting module bool config
        assert_equal "OK" [r configaccess.setbool configaccess.bool no]
        assert_equal "configaccess.bool no" [r config get configaccess.bool]

        # Test setting module bool config from another module
        assert_equal "OK" [r configaccess.setbool moduleconfigs.mutable_bool no]
        assert_equal "moduleconfigs.mutable_bool no" [r config get moduleconfigs.mutable_bool]

        # Test setting module numeric config
        assert_equal "OK" [r configaccess.setnumeric moduleconfigs.numeric 100]
        assert_equal "moduleconfigs.numeric 100" [r config get moduleconfigs.numeric]

        # Test setting module enum config by value
        assert_equal "OK" [r configaccess.setenumval moduleconfigs.enum 2]
        assert_equal "moduleconfigs.enum two" [r config get moduleconfigs.enum]
        
        # Test setting module enum config by name
        assert_equal "OK" [r configaccess.setenumname moduleconfigs.enum "five"]
        assert_equal "moduleconfigs.enum five" [r config get moduleconfigs.enum]
    }

    test {Test module config set with error cases} {
        # Test setting a non-existent config
        catch {r configaccess.setbool nonexistent_config 1} err
        assert_match "*ERR*" $err

        # Test setting a read-only config
        catch {r configaccess.setbool moduleconfigs.immutable_bool 1} err
        assert_match "*ERR*" $err

        # Test setting an enum config with invalid value
        catch {r configaccess.setenumval moduleconfigs.enum 999} err
        assert_match "*ERR*" $err
        
        # Test setting an enum config with invalid name
        catch {r configaccess.setenumname moduleconfigs.enum "invalid_value"} err
        assert_match "*ERR*" $err

        # Test setting a numeric config with out-of-range value
        catch {r configaccess.setnumeric moduleconfigs.numeric 5000} err
        assert_match "*ERR*" $err
    }
}
