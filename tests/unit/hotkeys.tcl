#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

start_server {tags {"hotkeys"}} {
    test {HOTKEYS detection with biased key access} {
        r hello 3

        # Generate 100 random keys
        set all_keys {}
        for {set i 0} {$i < 100} {incr i} {
            lappend all_keys "key_[format %03d $i]"
        }

        # Choose 20 keys to bias towards. These will be out hot keys
        set hot_keys {}
        for {set i 0} {$i < 20} {incr i} {
            lappend hot_keys [lindex $all_keys $i]
        }

        assert_equal {OK} [r hotkeys start]

        # Biasing towards the 20 chosen keys when sending commands
        set total_commands 50000
        for {set i 0} {$i < $total_commands} {incr i} {
            set rand [expr {rand()}]
            if {$rand < 0.8} {
                set key [lindex $hot_keys [expr {int(rand() * 20)}]]
            } else {
                set key [lindex $all_keys [expr {20 + int(rand() * 80)}]]
            }
            r set $key "value_$i"
        }

        assert_equal {OK} [r hotkeys stop]

        set result [r hotkeys get]
        assert_not_equal $result {}

        set cpu_time_array [dict get $result "by-cpu-time"]
        set net_bytes_array [dict get $result "by-net-bytes"]

        set returned_cpu_keys {}
        for {set i 0} {$i < [llength $cpu_time_array]} {incr i 2} {
            lappend returned_cpu_keys [lindex $cpu_time_array $i]
        }

        # Check that all returned keys (based on cpu time) are from our hot_keys list
        set num_returned_cpu [llength $returned_cpu_keys]
        assert_lessthan_equal $num_returned_cpu 10
        assert_morethan $num_returned_cpu 0

        foreach key $returned_cpu_keys {
            assert_morethan_equal [lsearch -exact $hot_keys $key] 0
        }

        set returned_net_keys {}
        for {set i 0} {$i < [llength $net_bytes_array]} {incr i 2} {
            lappend returned_net_keys [lindex $net_bytes_array $i]
        }

        # Same as cpu-time but for net-bytes
        set num_returned_net [llength $returned_net_keys]
        assert_lessthan_equal $num_returned_net 10
        assert_morethan $num_returned_net 0

        foreach key $returned_net_keys {
            assert_morethan_equal [lsearch -exact $hot_keys $key] 0
        }
    } {} {resp3}
}

