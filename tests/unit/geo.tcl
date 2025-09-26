# Helper functions to simulate search-in-radius in the Tcl side in order to
# verify the server implementation with a fuzzy test.
proc geo_degrad deg {expr {$deg*(atan(1)*8/360)}}
proc geo_raddeg rad {expr {$rad/(atan(1)*8/360)}}

proc geo_distance {lon1d lat1d lon2d lat2d} {
    set lon1r [geo_degrad $lon1d]
    set lat1r [geo_degrad $lat1d]
    set lon2r [geo_degrad $lon2d]
    set lat2r [geo_degrad $lat2d]
    set v [expr {sin(($lon2r - $lon1r) / 2)}]
    set u [expr {sin(($lat2r - $lat1r) / 2)}]
    expr {2.0 * 6372797.560856 * \
            asin(sqrt($u * $u + cos($lat1r) * cos($lat2r) * $v * $v))}
}

proc geo_random_point {lonvar latvar} {
    upvar 1 $lonvar lon
    upvar 1 $latvar lat
    # Note that the actual latitude limit should be -85 to +85, we restrict
    # the test to -70 to +70 since in this range the algorithm is more precise
    # while outside this range occasionally some element may be missing.
    set lon [expr {-180 + rand()*360}]
    set lat [expr {-70 + rand()*140}]
}

# Return elements non common to both the lists.
# This code is from http://wiki.tcl.tk/15489
proc compare_lists {List1 List2} {
   set DiffList {}
   foreach Item $List1 {
      if {[lsearch -exact $List2 $Item] == -1} {
         lappend DiffList $Item
      }
   }
   foreach Item $List2 {
      if {[lsearch -exact $List1 $Item] == -1} {
         if {[lsearch -exact $DiffList $Item] == -1} {
            lappend DiffList $Item
         }
      }
   }
   return $DiffList
}

# return true If a point in circle.
# search_lon and search_lat define the center of the circle,
# and lon, lat define the point being searched.
proc pointInCircle {radius_km lon lat search_lon search_lat} {
    set radius_m [expr {$radius_km*1000}]
    set distance [geo_distance $lon $lat $search_lon $search_lat]
    if {$distance < $radius_m} {
        return true
    }
    return false
}

# return true If a point in rectangle.
# search_lon and search_lat define the center of the rectangle,
# and lon, lat define the point being searched.
# error: can adjust the width and height of the rectangle according to the error
proc pointInRectangle {width_km height_km lon lat search_lon search_lat error} {
    set width_m [expr {$width_km*1000*$error/2}]
    set height_m [expr {$height_km*1000*$error/2}]
    set lon_distance [geo_distance $lon $lat $search_lon $lat]
    set lat_distance [geo_distance $lon $lat $lon $search_lat]

    if {$lon_distance > $width_m || $lat_distance > $height_m} {
        return false
    }
    return true
}

proc verify_geo_edge_response_bylonlat {expected_response expected_store_response} {
    catch {r georadius src{t} 1 1 1 km} response
    assert_match $expected_response $response

    catch {r georadius src{t} 1 1 1 km store dest{t}} response
    assert_match $expected_store_response $response

    catch {r geosearch src{t} fromlonlat 0 0 byradius 1 km} response
    assert_match $expected_response $response

    catch {r geosearchstore dest{t} src{t} fromlonlat 0 0 byradius 1 km} response
    assert_match $expected_store_response $response
}

proc verify_geo_edge_response_bymember {expected_response expected_store_response} {
    catch {r georadiusbymember src{t} member 1 km} response
    assert_match $expected_response $response

    catch {r georadiusbymember src{t} member 1 km store dest{t}} response
    assert_match $expected_store_response $response

    catch {r geosearch src{t} frommember member bybox 1 1 km} response
    assert_match $expected_response $response

    catch {r geosearchstore dest{t} src{t} frommember member bybox 1 1 m} response
    assert_match $expected_store_response $response
}

proc verify_geo_edge_response_generic {expected_response} {
    catch {r geodist src{t} member 1 km} response
    assert_match $expected_response $response

    catch {r geohash src{t} member} response
    assert_match $expected_response $response

    catch {r geopos src{t} member} response
    assert_match $expected_response $response
}


# The following list represents sets of random seed, search position
# and radius that caused bugs in the past. It is used by the randomized
# test later as a starting point. When the regression vectors are scanned
# the code reverts to using random data.
#
# The format is: seed km lon lat
set regression_vectors {
    {1482225976969 7083 81.634948934258375 30.561509253718668}
    {1482340074151 5416 -70.863281847379767 -46.347003465679947}
    {1499014685896 6064 -89.818768962202014 -40.463868561416803}
    {1412 156 149.29737817929004 15.95807862745508}
    {441574 143 59.235461856813856 66.269555127373678}
    {160645 187 -101.88575239939883 49.061997951502917}
    {750269 154 -90.187939661642517 66.615930412251487}
    {342880 145 163.03472387745728 64.012747720821181}
    {729955 143 137.86663517256579 63.986745399416776}
    {939895 151 59.149620271823181 65.204186651485145}
    {1412 156 149.29737817929004 15.95807862745508}
    {564862 149 84.062063109158544 -65.685403922426232}
    {1546032440391 16751 -1.8175081637769495 20.665668878082954}
}
set rv_idx 0

start_server {tags {"geo"}} {
    test {GEO with wrong type src key} {
        r set src{t} wrong_type

        verify_geo_edge_response_bylonlat "WRONGTYPE*" "WRONGTYPE*"
        verify_geo_edge_response_bymember "WRONGTYPE*" "WRONGTYPE*"
        verify_geo_edge_response_generic "WRONGTYPE*"
    }

    test {GEO with non existing src key} {
        r del src{t}

        verify_geo_edge_response_bylonlat {} 0
        verify_geo_edge_response_bymember {} 0
    }

    test {GEO BYLONLAT with empty search} {
        r del src{t}
        r geoadd src{t} 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"

        verify_geo_edge_response_bylonlat {} 0
    }

    test {GEO BYMEMBER with non existing member} {
        r del src{t}
        r geoadd src{t} 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"

        verify_geo_edge_response_bymember "ERR*" "ERR*"
    }

    test {GEOADD create} {
        r geoadd nyc -73.9454966 40.747533 "lic market"
    } {1}

    test {GEOADD update} {
        r geoadd nyc -73.9454966 40.747533 "lic market"
    } {0}

    test {GEOADD update with CH option} {
        assert_equal 1 [r geoadd nyc CH 40.747533 -73.9454966 "lic market"]
        lassign [lindex [r geopos nyc "lic market"] 0] x1 y1
        assert {abs($x1) - 40.747 < 0.001}
        assert {abs($y1) - 73.945 < 0.001}
    } {}

    test {GEOADD update with NX option} {
        assert_equal 0 [r geoadd nyc NX -73.9454966 40.747533 "lic market"]
        lassign [lindex [r geopos nyc "lic market"] 0] x1 y1
        assert {abs($x1) - 40.747 < 0.001}
        assert {abs($y1) - 73.945 < 0.001}
    } {}

    test {GEOADD update with XX option} {
        assert_equal 0 [r geoadd nyc XX -83.9454966 40.747533 "lic market"]
        lassign [lindex [r geopos nyc "lic market"] 0] x1 y1
        assert {abs($x1) - 83.945 < 0.001}
        assert {abs($y1) - 40.747 < 0.001}
    } {}

    test {GEOADD update with CH NX option} {
        r geoadd nyc CH NX -73.9454966 40.747533 "lic market"
    } {0}

    test {GEOADD update with CH XX option} {
        r geoadd nyc CH XX -73.9454966 40.747533 "lic market"
    } {1}

    test {GEOADD update with XX NX option will return syntax error} {
        catch {
            r geoadd nyc xx nx -73.9454966 40.747533 "lic market"
        } err
        set err
    } {ERR *syntax*}

    test {GEOADD update with invalid option} {
        catch {
            r geoadd nyc ch xx foo -73.9454966 40.747533 "lic market"
        } err
        set err
    } {ERR *syntax*}

    test {GEOADD invalid coordinates} {
        catch {
            r geoadd nyc -73.9454966 40.747533 "lic market" \
                foo bar "luck market"
        } err
        set err
    } {*valid*}

    test {GEOADD multi add} {
        r geoadd nyc -73.9733487 40.7648057 "central park n/q/r" -73.9903085 40.7362513 "union square" -74.0131604 40.7126674 "wtc one" -73.7858139 40.6428986 "jfk" -73.9375699 40.7498929 "q4" -73.9564142 40.7480973 4545
    } {6}

    test {Check geoset values} {
        r zrange nyc 0 -1 withscores
    } {{wtc one} 1791873972053020 {union square} 1791875485187452 {central park n/q/r} 1791875761332224 4545 1791875796750882 {lic market} 1791875804419201 q4 1791875830079666 jfk 1791895905559723}

    test {GEORADIUS simple (sorted)} {
        r georadius nyc -73.9798091 40.7598464 3 km asc
    } {{central park n/q/r} 4545 {union square}}

    test {GEORADIUS_RO simple (sorted)} {
        r georadius_ro nyc -73.9798091 40.7598464 3 km asc
    } {{central park n/q/r} 4545 {union square}}

    test {GEOSEARCH simple (sorted)} {
        r geosearch nyc fromlonlat -73.9798091 40.7598464 bybox 6 6 km asc
    } {{central park n/q/r} 4545 {union square} {lic market}}

    test {GEOSEARCH FROMLONLAT and FROMMEMBER cannot exist at the same time} {
        catch {r geosearch nyc fromlonlat -73.9798091 40.7598464 frommember xxx bybox 6 6 km asc} e
        set e
    } {ERR *syntax*}

    test {GEOSEARCH FROMLONLAT and FROMMEMBER one must exist} {
        catch {r geosearch nyc bybox 3 3 km asc desc withhash withdist withcoord} e
        set e
    } {ERR *exactly one of FROMMEMBER or FROMLONLAT*}

    test {GEOSEARCH BYRADIUS and BYBOX cannot exist at the same time} {
        catch {r geosearch nyc fromlonlat -73.9798091 40.7598464 byradius 3 km bybox 3 3 km asc} e
        set e
    } {ERR *syntax*}

    test {GEOSEARCH BYRADIUS and BYBOX one must exist} {
        catch {r geosearch nyc fromlonlat -73.9798091 40.7598464 asc desc withhash withdist withcoord} e
        set e
    } {ERR *exactly one of BYRADIUS, BYBOX and BYPOLYGON*}

    test {GEOSEARCH with STOREDIST option} {
        catch {r geosearch nyc fromlonlat -73.9798091 40.7598464 bybox 6 6 km asc storedist} e
        set e
    } {ERR *syntax*}

    test {GEORADIUS withdist (sorted)} {
        r georadius nyc -73.9798091 40.7598464 3 km withdist asc
    } {{{central park n/q/r} 0.7750} {4545 2.3651} {{union square} 2.7697}}

    test {GEOSEARCH withdist (sorted)} {
        r geosearch nyc fromlonlat -73.9798091 40.7598464 bybox 6 6 km withdist asc
    } {{{central park n/q/r} 0.7750} {4545 2.3651} {{union square} 2.7697} {{lic market} 3.1991}}

    test {GEORADIUS with COUNT} {
        r georadius nyc -73.9798091 40.7598464 10 km COUNT 3
    } {{central park n/q/r} 4545 {union square}}

    test {GEORADIUS with multiple WITH* tokens} {
        assert_match {{{central park n/q/r} 1791875761332224 {-73.97334* 40.76480*}} {4545 1791875796750882 {-73.95641* 40.74809*}}} [r georadius nyc -73.9798091 40.7598464 10 km WITHCOORD WITHHASH COUNT 2]
        assert_match {{{central park n/q/r} 1791875761332224 {-73.97334* 40.76480*}} {4545 1791875796750882 {-73.95641* 40.74809*}}} [r georadius nyc -73.9798091 40.7598464 10 km WITHHASH WITHCOORD COUNT 2]
        assert_match {{{central park n/q/r} 0.7750 1791875761332224 {-73.97334* 40.76480*}} {4545 2.3651 1791875796750882 {-73.95641* 40.74809*}}} [r georadius nyc -73.9798091 40.7598464 10 km WITHDIST WITHHASH WITHCOORD COUNT 2]
    }

    test {GEORADIUS with ANY not sorted by default} {
        r georadius nyc -73.9798091 40.7598464 10 km COUNT 3 ANY
    } {{wtc one} {union square} {central park n/q/r}}

    test {GEORADIUS with ANY sorted by ASC} {
        r georadius nyc -73.9798091 40.7598464 10 km COUNT 3 ANY ASC
    } {{central park n/q/r} {union square} {wtc one}}

    test {GEORADIUS with ANY but no COUNT} {
        catch {r georadius nyc -73.9798091 40.7598464 10 km ANY ASC} e
        set e
    } {ERR *ANY*requires*COUNT*}

    test {GEORADIUS with COUNT but missing integer argument} {
        catch {r georadius nyc -73.9798091 40.7598464 10 km COUNT} e
        set e
    } {ERR *syntax*}

    test {GEORADIUS with COUNT DESC} {
        r georadius nyc -73.9798091 40.7598464 10 km COUNT 2 DESC
    } {{wtc one} q4}

    test {GEORADIUS HUGE, issue #2767} {
        r geoadd users -47.271613776683807 -54.534504198047678 user_000000
        llength [r GEORADIUS users 0 0 50000 km WITHCOORD]
    } {1}

    test {GEORADIUSBYMEMBER simple (sorted)} {
        r georadiusbymember nyc "wtc one" 7 km
    } {{wtc one} {union square} {central park n/q/r} 4545 {lic market}}

    test {GEORADIUSBYMEMBER_RO simple (sorted)} {
        r georadiusbymember_ro nyc "wtc one" 7 km
    } {{wtc one} {union square} {central park n/q/r} 4545 {lic market}}
    
    test {GEORADIUSBYMEMBER member does not exist} {
        r del Sicily
        r geoadd Sicily 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"
        assert_error "ERR member none_exist_member does not exist" {r GEORADIUSBYMEMBER Sicily none_exist_member 300 KM} 
    } {}

    test {GEORADIUSBYMEMBER search areas contain satisfied points in oblique direction} {
        r del k1
        
        r geoadd k1 -0.15307903289794921875 85 n1 0.3515625 85.00019260486917005437 n2
        set ret1 [r GEORADIUSBYMEMBER k1 n1 4891.94 m]
        assert_equal $ret1 {n1 n2}
        
        r zrem k1 n1 n2
        r geoadd k1 -4.95211958885192871094 85 n3 11.25 85.0511 n4
        set ret2 [r GEORADIUSBYMEMBER k1 n3 156544 m]
        assert_equal $ret2 {n3 n4}
        
        r zrem k1 n3 n4
        r geoadd k1 -45 65.50900022111811438208 n5 90 85.0511 n6
        set ret3 [r GEORADIUSBYMEMBER k1 n5 5009431 m]
        assert_equal $ret3 {n5 n6}
    }

    test {GEORADIUSBYMEMBER crossing pole search} {
        r del k1
        r geoadd k1 45 65 n1 -135 85.05 n2
        set ret [r GEORADIUSBYMEMBER k1 n1 5009431 m]
        assert_equal $ret {n1 n2}
    }
    test {GEOSEARCH FROMMEMBER member does not exist} {
        r del Sicily
        r geoadd Sicily 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"
        assert_error "ERR member none_exist_member does not exist" {r GEOSEARCH Sicily FROMMEMBER none_exist_member BYRADIUS 300 KM} 
    } 
    
    test {GEOSEARCH FROMMEMBER simple (sorted)} {
        r geosearch nyc frommember "wtc one" bybox 14 14 km
    } {{wtc one} {union square} {central park n/q/r} 4545 {lic market} q4}

    test {GEOSEARCH vs GEORADIUS} {
        r del Sicily
        r geoadd Sicily 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"
        r geoadd Sicily 12.758489 38.788135 "edge1"   17.241510 38.788135 "eage2"
        set ret1 [r georadius Sicily 15 37 200 km asc]
        assert_equal $ret1 {Catania Palermo}
        set ret2 [r geosearch Sicily fromlonlat 15 37 bybox 400 400 km asc]
        assert_equal $ret2 {Catania Palermo eage2 edge1}
    }

    test {GEOSEARCH non square, long and narrow} {
        r del Sicily
        r geoadd Sicily 12.75 36.995 "test1"
        r geoadd Sicily 12.75 36.50 "test2"
        r geoadd Sicily 13.00 36.50 "test3"
        # box height=2km width=400km
        set ret1 [r geosearch Sicily fromlonlat 15 37 bybox 400 2 km]
        assert_equal $ret1 {test1}

        # Add a western Hemisphere point
        r geoadd Sicily -1 37.00 "test3"
        set ret2 [r geosearch Sicily fromlonlat 15 37 bybox 3000 2 km asc]
        assert_equal $ret2 {test1 test3}
    }

    test {GEOSEARCH corner point test} {
        r del Sicily
        r geoadd Sicily 12.758489 38.788135 edge1 17.241510 38.788135 edge2 17.250000 35.202000 edge3 12.750000 35.202000 edge4 12.748489955781654 37 edge5 15 38.798135872540925 edge6 17.251510044218346 37 edge7 15 35.201864127459075 edge8 12.692799634687903 38.798135872540925 corner1 12.692799634687903 38.798135872540925 corner2 17.200560937451133 35.201864127459075 corner3 12.799439062548865 35.201864127459075 corner4
        set ret [lsort [r geosearch Sicily fromlonlat 15 37 bybox 400 400 km asc]]
        assert_equal $ret {edge1 edge2 edge5 edge7}
    }

    test {GEORADIUSBYMEMBER withdist (sorted)} {
        r georadiusbymember nyc "wtc one" 7 km withdist
    } {{{wtc one} 0.0000} {{union square} 3.2544} {{central park n/q/r} 6.7000} {4545 6.1975} {{lic market} 6.8969}}

    test {GEOHASH is able to return geohash strings} {
        # Example from Wikipedia.
        r del points
        r geoadd points -5.6 42.6 test
        lindex [r geohash points test] 0
    } {ezs42e44yx0}

    test {GEOHASH with only key as argument} {
        r del points
        r geoadd points 10 20 a 30 40 b
        set result [r geohash points]
        assert {$result eq {}}
    } 

    test {GEOPOS simple} {
        r del points
        r geoadd points 10 20 a 30 40 b
        lassign [lindex [r geopos points a b] 0] x1 y1
        lassign [lindex [r geopos points a b] 1] x2 y2
        assert {abs($x1 - 10) < 0.001}
        assert {abs($y1 - 20) < 0.001}
        assert {abs($x2 - 30) < 0.001}
        assert {abs($y2 - 40) < 0.001}
    }

    test {GEOPOS missing element} {
        r del points
        r geoadd points 10 20 a 30 40 b
        lindex [r geopos points a x b] 1
    } {}

    test {GEOPOS with only key as argument} {
        r del points
        r geoadd points 10 20 a 30 40 b
        set result [r geopos points]
        assert {$result eq {}}
    }

    test {GEODIST simple & unit} {
        r del points
        r geoadd points 13.361389 38.115556 "Palermo" \
                        15.087269 37.502669 "Catania"
        set m [r geodist points Palermo Catania]
        assert {$m > 166274 && $m < 166275}
        set km [r geodist points Palermo Catania km]
        assert {$km > 166.2 && $km < 166.3}
        set dist [r geodist points Palermo Palermo]
        assert {$dist eq 0.0000}
    }

    test {GEODIST missing elements} {
        r del points
        r geoadd points 13.361389 38.115556 "Palermo" \
                        15.087269 37.502669 "Catania"
        set m [r geodist points Palermo Agrigento]
        assert {$m eq {}}
        set m [r geodist points Ragusa Agrigento]
        assert {$m eq {}}
        set m [r geodist empty_key Palermo Catania]
        assert {$m eq {}}
    }

    test {GEORADIUS STORE option: syntax error} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        catch {r georadius points{t} 13.361389 38.115556 50 km store} e
        set e
    } {*ERR*syntax*}

    test {GEOSEARCHSTORE STORE option: syntax error} {
        catch {r geosearchstore abc{t} points{t} fromlonlat 13.361389 38.115556 byradius 50 km store abc{t}} e
        set e
    } {*ERR*syntax*}

    test {GEORANGE STORE option: incompatible options} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        catch {r georadius points{t} 13.361389 38.115556 50 km store points2{t} withdist} e
        assert_match {*ERR*} $e
        catch {r georadius points{t} 13.361389 38.115556 50 km store points2{t} withhash} e
        assert_match {*ERR*} $e
        catch {r georadius points{t} 13.361389 38.115556 50 km store points2{t} withcoords} e
        assert_match {*ERR*} $e
    }

    test {GEORANGE STORE option: plain usage} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        r georadius points{t} 13.361389 38.115556 500 km store points2{t}
        assert_equal [r zrange points{t} 0 -1] [r zrange points2{t} 0 -1]
    }

    test {GEORADIUSBYMEMBER STORE/STOREDIST option: plain usage} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" 15.087269 37.502669 "Catania"

        r georadiusbymember points{t} Palermo 500 km store points2{t}
        assert_equal {Palermo Catania} [r zrange points2{t} 0 -1]

        r georadiusbymember points{t} Catania 500 km storedist points2{t}
        assert_equal {Catania Palermo} [r zrange points2{t} 0 -1]

        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 1] < 1}
        assert {[lindex $res 3] > 166}
    }

    test {GEOSEARCHSTORE STORE option: plain usage} {
        r geosearchstore points2{t} points{t} fromlonlat 13.361389 38.115556 byradius 500 km
        assert_equal [r zrange points{t} 0 -1] [r zrange points2{t} 0 -1]
    }

    test {GEORANGE STOREDIST option: plain usage} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        r georadius points{t} 13.361389 38.115556 500 km storedist points2{t}
        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 1] < 1}
        assert {[lindex $res 3] > 166}
        assert {[lindex $res 3] < 167}
    }

    test {GEOSEARCHSTORE STOREDIST option: plain usage} {
        r geosearchstore points2{t} points{t} fromlonlat 13.361389 38.115556 byradius 500 km storedist
        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 1] < 1}
        assert {[lindex $res 3] > 166}
        assert {[lindex $res 3] < 167}
    }

    test {GEORANGE STOREDIST option: COUNT ASC and DESC} {
        r del points{t}
        r geoadd points{t} 13.361389 38.115556 "Palermo" \
                           15.087269 37.502669 "Catania"
        r georadius points{t} 13.361389 38.115556 500 km storedist points2{t} asc count 1
        assert {[r zcard points2{t}] == 1}
        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 0] eq "Palermo"}

        r georadius points{t} 13.361389 38.115556 500 km storedist points2{t} desc count 1
        assert {[r zcard points2{t}] == 1}
        set res [r zrange points2{t} 0 -1 withscores]
        assert {[lindex $res 0] eq "Catania"}
    }

    test {GEOSEARCH the box spans -180° or 180°} {
        r del points
        r geoadd points 179.5 36 point1
        r geoadd points -179.5 36 point2
        assert_equal {point1 point2} [r geosearch points fromlonlat 179 37 bybox 400 400 km asc]
        assert_equal {point2 point1} [r geosearch points fromlonlat -179 37 bybox 400 400 km asc]
    }

    test {GEOSEARCH with small distance} {
        r del points
        r geoadd points -122.407107 37.794300 1
        r geoadd points -122.227336 37.794300 2
        assert_equal {{1 0.0001} {2 9.8182}} [r GEORADIUS points -122.407107 37.794300 30 mi ASC WITHDIST]
    }

    test {GEOSEARCH BYPOLYGON standard operations} {
        r geoadd points 151.20932132005692 -33.877723137822436 "146, Elizabeth Street, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia"
        r geoadd points 151.2119820713997 -33.87697539508043 "Connaught Centre, 187, Liverpool Street, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia"
        r geoadd points 151.2119820713997 -33.87156123068408 "St James, Archibald Fountain Plaza/Area, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia"
        r geoadd points 151.2229523062706 -33.883973760201364 "146, Oxford Street, Five Ways, Paddington, Eastern Suburbs, Sydney, Woollahra Municipal Council, New South Wales, 2021, Australia"
        r geoadd points 151.22852593660355 -33.88116782387797 "Goodhope Street, Five Ways, Paddington, Eastern Suburbs, Sydney, Woollahra Municipal Council, New South Wales, 2021, Australia"
        r geoadd points 151.2224105000496 -33.87544442350019 "Kings Cross Centre, 82-94, Darlinghurst Road, Potts Point, Sydney, Council of the City of Sydney, New South Wales, 2011, Australia"
        r geoadd points 151.22953444719315 -33.874838625143106 "The Reg Bartley Oval, Waratah Street, Rushcutters Bay, Sydney, Council of the City of Sydney, New South Wales, 2011, Australia"
        # Error Case - Polygon vertices need at least 3 coordinates provided.
        catch {r GEOSEARCH points BYPOLYGON 2 151.2039101016274 -33.87446472104431 151.2132606898065 -33.882942295615855} e
        assert_match {*ERR GEOSEARCH BYPOLYGON must have at least 3 vertices*} $e
        catch {r GEOSEARCH points BYPOLYGON 3 151.2039101016274 -33.87446472104431 151.2132606898065 -33.882942295615855} e
        assert_match {*ERR GEOSEARCH BYPOLYGON must have at least 3 vertices*} $e
        # Error Case - Users cannot use BYPOLYGON along with BYRADIUS or BYBOX.
        catch {r GEOSEARCH points BYPOLYGON 3 151.2039101016274 -33.87446472104431 151.2132606898065 -33.882942295615855 151.2132606898065 -33.882942295615855 BYBOX 400 400 km} e
        assert_match {*ERR syntax error*} $e
        catch {r GEOSEARCH points BYBOX 400 400 km BYPOLYGON 3 151.2039101016274 -33.87446472104431 151.2132606898065 -33.882942295615855 151.2132606898065 -33.882942295615855} e
        assert_match {*ERR syntax error*} $e
        catch {r GEOSEARCH points BYRADIUS 500 km BYPOLYGON 3 151.2039101016274 -33.87446472104431 151.2132606898065 -33.882942295615855 151.2132606898065 -33.882942295615855} e
        assert_match {*ERR syntax error*} $e
        # Error Case - Users cannot use BYPOLYGON along with FROMLONLAT or FROMMEMBER.
        catch {r GEOSEARCH points BYPOLYGON 3 151.2039101016274 -33.87446472104431 151.2132606898065 -33.882942295615855 151.2132606898065 -33.882942295615855 FROMLONLAT 151.2229523062706 -33.883973760201364} e
        assert_match {*ERR syntax error*} $e
        catch {r GEOSEARCH points BYPOLYGON 3 151.2039101016274 -33.87446472104431 151.2132606898065 -33.882942295615855 151.2132606898065 -33.882942295615855 FROMMEMBER memberA} e
        assert_match {*ERR syntax error*} $e
        # Success Case for GEOSEARCH
        assert_equal {{Goodhope Street, Five Ways, Paddington, Eastern Suburbs, Sydney, Woollahra Municipal Council, New South Wales, 2021, Australia} {The Reg Bartley Oval, Waratah Street, Rushcutters Bay, Sydney, Council of the City of Sydney, New South Wales, 2011, Australia} {146, Elizabeth Street, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia}} [r GEOSEARCH points BYPOLYGON 11 151.21619137411474 -33.869643764947696 151.20408853981647 -33.87845382042545 151.2123775086366 -33.87864711938855 151.21046943124387 -33.87588862400425 151.21701035939626 -33.874286511854834 151.2239557405701 -33.8735722632175 151.22495056246407 -33.87893571656996 151.2274447496748 -33.882307713036354 151.23212935868654 -33.88136096120022 151.23097291274465 -33.87456240592362 151.22475424781445 -33.87099633192101]
        assert_equal {{{146, Elizabeth Street, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia} 1035.7735 {151.20932132005691528 -33.87772313782243572}} {{Goodhope Street, Five Ways, Paddington, Eastern Suburbs, Sydney, Woollahra Municipal Council, New South Wales, 2021, Australia} 929.4979 {151.22852593660354614 -33.88116782387797343}} {{The Reg Bartley Oval, Waratah Street, Rushcutters Bay, Sydney, Council of the City of Sydney, New South Wales, 2011, Australia} 858.3013 {151.22953444719314575 -33.87483862514310573}}} [r GEOSEARCH points BYPOLYGON 11 151.21619137411474 -33.869643764947696 151.20408853981647 -33.87845382042545 151.2123775086366 -33.87864711938855 151.21046943124387 -33.87588862400425 151.21701035939626 -33.874286511854834 151.2239557405701 -33.8735722632175 151.22495056246407 -33.87893571656996 151.2274447496748 -33.882307713036354 151.23212935868654 -33.88136096120022 151.23097291274465 -33.87456240592362 151.22475424781445 -33.87099633192101 WITHDIST WITHCOORD DESC]
        set res [r GEOSEARCH points BYPOLYGON 11 151.21619137411474 -33.869643764947696 151.20408853981647 -33.87845382042545 151.2123775086366 -33.87864711938855 151.21046943124387 -33.87588862400425 151.21701035939626 -33.874286511854834 151.2239557405701 -33.8735722632175 151.22495056246407 -33.87893571656996 151.2274447496748 -33.882307713036354 151.23212935868654 -33.88136096120022 151.23097291274465 -33.87456240592362 151.22475424781445 -33.87099633192101 WITHDIST WITHCOORD ASC]
        assert_equal {{{The Reg Bartley Oval, Waratah Street, Rushcutters Bay, Sydney, Council of the City of Sydney, New South Wales, 2011, Australia} 858.3013 {151.22953444719314575 -33.87483862514310573}} {{Goodhope Street, Five Ways, Paddington, Eastern Suburbs, Sydney, Woollahra Municipal Council, New South Wales, 2021, Australia} 929.4979 {151.22852593660354614 -33.88116782387797343}} {{146, Elizabeth Street, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia} 1035.7735 {151.20932132005691528 -33.87772313782243572}}} $res
        # Success Case for GEOSEARCHSTORE
        r geosearchstore "{points}dest" points  BYPOLYGON 11 151.21619137411474 -33.869643764947696 151.20408853981647 -33.87845382042545 151.2123775086366 -33.87864711938855 151.21046943124387 -33.87588862400425 151.21701035939626 -33.874286511854834 151.2239557405701 -33.8735722632175 151.22495056246407 -33.87893571656996 151.2274447496748 -33.882307713036354 151.23212935868654 -33.88136096120022 151.23097291274465 -33.87456240592362 151.22475424781445 -33.87099633192101 ASC STOREDIST COUNT 2
        assert_equal {{The Reg Bartley Oval, Waratah Street, Rushcutters Bay, Sydney, Council of the City of Sydney, New South Wales, 2011, Australia} {Goodhope Street, Five Ways, Paddington, Eastern Suburbs, Sydney, Woollahra Municipal Council, New South Wales, 2021, Australia}} [r zrange "{points}dest" 0 -1]
    }

    test {GEOSEARCH BYPOLYGON across hemispheres} {
        r del points "{points}dest"
        r geoadd points -68.8798275589943 -44.61362078872198 "Departamento Paso de Indios, Chubut Province, 9207, Argentina"
        r geoadd points -63.42572718858719 -28.2735053489472 "Chañar Pozo, Departamento Avellaneda, Santiago del Estero, G4328, Argentina"
        r geoadd points -100.691137611866 24.238865478290585 "La Noria de Jesús, Vanegas, San Luis Potosí, 78514, Mexico"
        r geoadd points -118.6207178235054 43.85981245898269 "Harney County, Oregon, United States"
        r geoadd points -121.43320173025131 51.857438167842574 "Area G (Lac La Hache/108 Mile Ranch), Cariboo Regional District, British Columbia, Canada"
        r geoadd points -79.07508105039597 37.97053232436472 "Stuarts Draft, Augusta County, Virginia, 24477, United States"
        r geoadd points -0.1308193802833557 51.652445128802185 "Trent Park Golf Course, Bramley Road, Cockfosters, London Borough of Enfield, London, Greater London, England, N14 4TN, United Kingdom"
        r geoadd points -44.97371703386307 64.60993716062175 "Sermersooq, Greenland"
        r geoadd points 17.252480685710907 -21.646808926327388 "Otjozondjupa, Namibia"
        r geoadd points 35.04182428121567 -6.304554908397684 "Mwandila, Manyoni, Singida Region, Central Zone, Tanzania"
        r geoadd points 120.61124950647354 -27.712320618990717 "Sir Samuel, Shire Of Leonora, Western Australia, 6437, Australia"
        r geoadd points 145.04961222410202 -32.22031713143272 "Kulwin, Cobar Shire Council, New South Wales, 2835, Australia"
        r geoadd points 3.5416236519813538 26.716565550429046 "In Salah, d'In Salah District, In Salah, Algeria"
        r geoadd points 13.330726325511932 38.111391848631506 "Via Palchetto, Altarello, IV Circoscrizione, Palermo, Sicily, 90132, Italy"
        r geoadd points 34.81671720743179 32.05908351848717 "Korazin, South Givatayim, Tel Ganim, Givatayim, Tel Aviv Subdistrict, Tel-Aviv District, 5223403, Israel"
        r geoadd points 74.56694215536118 14.589623066168919 "Yana Caves, Yana Caves Road, Yana, Kumata taluk, Uttara Kannada, Karnataka, India"
        r geoadd points 11.964205205440521 57.70702963854761 "3, Köpmansgatan, North Town, Inom Vallgraven, Centrum, Gothenburg, Göteborgs Stad, Västra Götaland County, 411 14, Sweden"
        r geoadd points 106.83230191469193 15.117455935110826 "Ban Soak, Saysetha District, Attapeu, Laos"
        r geoadd points 90.02549439668655 39.614491635004136 "Lopnur, Ruoqiang County, Bayingolin, Xinjiang, China"
        r geoadd points 118.27697664499283 26.579067066419547 "Yanping District, Nanping City, Fujian, China"
        r geoadd points 118.27697664499283 36.02277654389389 "S234, Yiyuan County, Zibo, Shandong, China"
        assert_equal {{Via Palchetto, Altarello, IV Circoscrizione, Palermo, Sicily, 90132, Italy} {Yana Caves, Yana Caves Road, Yana, Kumata taluk, Uttara Kannada, Karnataka, India} {Ban Soak, Saysetha District, Attapeu, Laos} {Yanping District, Nanping City, Fujian, China} {Mwandila, Manyoni, Singida Region, Central Zone, Tanzania} {Harney County, Oregon, United States} {Trent Park Golf Course, Bramley Road, Cockfosters, London Borough of Enfield, London, Greater London, England, N14 4TN, United Kingdom}} [r geosearch points BYPOLYGON 33 -146.20062130262065 51.130912386593 -103.26379152330803 49.47193779943357 -63.315654670518995 50.93884489468163 -95.40642417874389 39.65534094349885 -77.4999298453865 26.73672845329922 -51.87745194229085 23.04646540450935 -8.42920299630615 41.60520661073045 -10.521785859954813 61.47846964761324 2.124111965940925 60.778434760758095 18.481092483619292 53.09061572914368 38.70573141092903 45.73303292470514 22.02205974562196 34.2426280008694 52.57520291900751 22.953137594767274 75.72278884992889 29.362381850764695 90.74676219754886 31.63413660453445 107.30623758289035 35.851039511158085 115.40629273520966 30.411873608761567 120.01593165018451 29.841092929180927 120.4535153988606 25.65037857240954 121.27924547967872 11.602225617909395 135.1015103851442 -16.60585394490754 129.87136213636785 -27.170064645188177 99.53888837265143 -26.498354216857965 37.56438677364021 -27.832463377991818 31.562753892887216 -13.63096249674378 20.629919303178035 9.515486567597444 11.209492263565085 32.02725659471969 5.4325413537034 35.55125756355285 -35.244014834951145 16.982781656720274 -75.35489645024853 11.812375133773651 -94.03343867618196 28.67577744862069 -124.16269338003593 36.64849855935148 -143.36084887326555 42.51835490888078]
    }

    test {GEOSEARCH BYPOLYGON overlapping edges} {
        r del points
        r geoadd points 151.20932132005692 -33.877723137822436 "146, Elizabeth Street, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia"
        r geoadd points 151.2119820713997 -33.87697539508043 "Connaught Centre, 187, Liverpool Street, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia"
        r geoadd points 151.2119820713997 -33.87156123068408 "St James, Archibald Fountain Plaza/Area, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia"
        r geoadd points 151.2229523062706 -33.883973760201364 "146, Oxford Street, Five Ways, Paddington, Eastern Suburbs, Sydney, Woollahra Municipal Council, New South Wales, 2021, Australia"
        r geoadd points 151.22852593660355 -33.88116782387797 "Goodhope Street, Five Ways, Paddington, Eastern Suburbs, Sydney, Woollahra Municipal Council, New South Wales, 2021, Australia"
        r geoadd points 151.2224105000496 -33.87544442350019 "Kings Cross Centre, 82-94, Darlinghurst Road, Potts Point, Sydney, Council of the City of Sydney, New South Wales, 2011, Australia"
        r geoadd points 151.22953444719315 -33.874838625143106 "The Reg Bartley Oval, Waratah Street, Rushcutters Bay, Sydney, Council of the City of Sydney, New South Wales, 2011, Australia"
        assert_equal {{146, Elizabeth Street, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia} {St James, Archibald Fountain Plaza/Area, Koreatown, Sydney, Sydney CBD, Sydney, Council of the City of Sydney, New South Wales, 2000, Australia} {Kings Cross Centre, 82-94, Darlinghurst Road, Potts Point, Sydney, Council of the City of Sydney, New South Wales, 2011, Australia}} [r GEOSEARCH points BYPOLYGON  5 151.2154285314146 -33.867438205607186 151.20024487595015 -33.88138572784347 151.2374118848648 -33.87601765283626 151.19695829592922 -33.87272872054441 151.2278336546125 -33.88345323146266]
    }

    test {GEOSEARCH with exact zero distances} {
        r del points
        # These are full precision coordinates, so the distance should 0.0000
        r geoadd points -122.40710645914077759 37.79430076631935975 position
        assert_equal {{position 0.0000}} [r GEOSEARCH points FROMMEMBER position BYRADIUS 0 mi ASC WITHDIST]
        assert_equal {{position 0.0000}} [r GEOSEARCH points FROMLONLAT -122.40710645914077759 37.79430076631935975 BYRADIUS 0 mi ASC WITHDIST]
    }

    foreach {type} {byradius bybox} {
    test "GEOSEARCH fuzzy test - $type" {
        if {$::accurate} { set attempt 300 } else { set attempt 30 }
        while {[incr attempt -1]} {
            set rv [lindex $regression_vectors $rv_idx]
            incr rv_idx

            set radius_km 0; set width_km 0; set height_km 0
            unset -nocomplain debuginfo
            set srand_seed [clock milliseconds]
            if {$rv ne {}} {set srand_seed [lindex $rv 0]}
            lappend debuginfo "srand_seed is $srand_seed"
            expr {srand($srand_seed)} ; # If you need a reproducible run
            r del mypoints

            if {[randomInt 10] == 0} {
                # From time to time use very big radiuses
                if {$type == "byradius"} {
                    set radius_km [expr {[randomInt 5000]+10}]
                } elseif {$type == "bybox"} {
                    set width_km [expr {[randomInt 5000]+10}]
                    set height_km [expr {[randomInt 5000]+10}]
                }
            } else {
                # Normally use a few - ~200km radiuses to stress
                # test the code the most in edge cases.
                if {$type == "byradius"} {
                    set radius_km [expr {[randomInt 200]+10}]
                } elseif {$type == "bybox"} {
                    set width_km [expr {[randomInt 200]+10}]
                    set height_km [expr {[randomInt 200]+10}]
                }
            }
            if {$rv ne {}} {
                set radius_km [lindex $rv 1]
                set width_km [lindex $rv 1]
                set height_km [lindex $rv 1]
            }
            geo_random_point search_lon search_lat
            if {$rv ne {}} {
                set search_lon [lindex $rv 2]
                set search_lat [lindex $rv 3]
            }
            lappend debuginfo "Search area: $search_lon,$search_lat $radius_km $width_km $height_km km"
            set tcl_result {}
            set argv {}
            for {set j 0} {$j < 20000} {incr j} {
                geo_random_point lon lat
                lappend argv $lon $lat "place:$j"
                if {$type == "byradius"} {
                    if {[pointInCircle $radius_km $lon $lat $search_lon $search_lat]} {
                        lappend tcl_result "place:$j"
                    }
                } elseif {$type == "bybox"} {
                    if {[pointInRectangle $width_km $height_km $lon $lat $search_lon $search_lat 1]} {
                        lappend tcl_result "place:$j"
                    }
                }
                lappend debuginfo "place:$j $lon $lat"
            }
            r geoadd mypoints {*}$argv
            if {$type == "byradius"} {
                set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat byradius $radius_km km]]
            } elseif {$type == "bybox"} {
                set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_km $height_km km]]
            }
            set res2 [lsort $tcl_result]
            set test_result OK

            if {$res != $res2} {
                set rounding_errors 0
                set diff [compare_lists $res $res2]
                foreach place $diff {
                    lassign [lindex [r geopos mypoints $place] 0] lon lat
                    set mydist [geo_distance $lon $lat $search_lon $search_lat]
                    set mydist [expr $mydist/1000]
                    if {$type == "byradius"} {
                        if {($mydist / $radius_km) > 0.999} {
                            incr rounding_errors
                            continue
                        }
                        if {$mydist < [expr {$radius_km*1000}]} {
                            # This is a false positive for the server since given the
                            # same points the higher precision calculation provided
                            # by TCL shows the point within range
                            incr rounding_errors
                            continue
                        }
                    } elseif {$type == "bybox"} {
                        # we add 0.1% error for floating point calculation error
                        if {[pointInRectangle $width_km $height_km $lon $lat $search_lon $search_lat 1.001]} {
                            incr rounding_errors
                            continue
                        }
                    }
                }

                # Make sure this is a real error and not a rounidng issue.
                if {[llength $diff] == $rounding_errors} {
                    set res $res2; # Error silenced
                }
            }

            if {$res != $res2} {
                set diff [compare_lists $res $res2]
                puts "*** Possible problem in GEO radius query ***"
                puts "Valkey: $res"
                puts "Tcl  : $res2"
                puts "Diff : $diff"
                puts [join $debuginfo "\n"]
                foreach place $diff {
                    if {[lsearch -exact $res2 $place] != -1} {
                        set where "(only in Tcl)"
                    } else {
                        set where "(only in Valkey)"
                    }
                    lassign [lindex [r geopos mypoints $place] 0] lon lat
                    set mydist [geo_distance $lon $lat $search_lon $search_lat]
                    set mydist [expr $mydist/1000]
                    puts "$place -> [r geopos mypoints $place] $mydist $where"
                }
                set test_result FAIL
            }
            unset -nocomplain debuginfo
            if {$test_result ne {OK}} break
        }
        set test_result
    } {OK}
    }

    test {GEOSEARCH box edges fuzzy test} {
        if {$::accurate} { set attempt 300 } else { set attempt 30 }
        while {[incr attempt -1]} {
            unset -nocomplain debuginfo
            set srand_seed [clock milliseconds]
            lappend debuginfo "srand_seed is $srand_seed"
            expr {srand($srand_seed)} ; # If you need a reproducible run
            r del mypoints

            geo_random_point search_lon search_lat
            set width_m [expr {[randomInt 10000]+10}]
            set height_m [expr {[randomInt 10000]+10}]
            set lat_delta [geo_raddeg [expr {$height_m/2/6372797.560856}]]
            set long_delta_top [geo_raddeg [expr {$width_m/2/6372797.560856/cos([geo_degrad [expr {$search_lat+$lat_delta}]])}]]
            set long_delta_middle [geo_raddeg [expr {$width_m/2/6372797.560856/cos([geo_degrad $search_lat])}]]
            set long_delta_bottom [geo_raddeg [expr {$width_m/2/6372797.560856/cos([geo_degrad [expr {$search_lat-$lat_delta}]])}]]

            # Total of 8 points are generated, which are located at each vertex and the center of each side
            set points(north) [list $search_lon [expr {$search_lat+$lat_delta}]]
            set points(south) [list $search_lon [expr {$search_lat-$lat_delta}]]
            set points(east) [list [expr {$search_lon+$long_delta_middle}] $search_lat]
            set points(west) [list [expr {$search_lon-$long_delta_middle}] $search_lat]
            set points(north_east) [list [expr {$search_lon+$long_delta_top}] [expr {$search_lat+$lat_delta}]]
            set points(north_west) [list [expr {$search_lon-$long_delta_top}] [expr {$search_lat+$lat_delta}]]
            set points(south_east) [list [expr {$search_lon+$long_delta_bottom}] [expr {$search_lat-$lat_delta}]]
            set points(south_west) [list [expr {$search_lon-$long_delta_bottom}] [expr {$search_lat-$lat_delta}]]

            lappend debuginfo "Search area: geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_m $height_m m"
            set tcl_result {}
            foreach name [array names points] {
                set x [lindex $points($name) 0]
                set y [lindex $points($name) 1]
                # If longitude crosses -180° or 180°, we need to convert it.
                # latitude doesn't have this problem, because it's scope is -70~70, see geo_random_point
                if {$x > 180} {
                    set x [expr {$x-360}]
                } elseif {$x < -180} {
                    set x [expr {$x+360}]
                }
                r geoadd mypoints $x $y place:$name
                lappend tcl_result "place:$name"
                lappend debuginfo "geoadd mypoints $x $y place:$name"
            }

            set res2 [lsort $tcl_result]

            # make the box larger by two meter in each direction to put the coordinate slightly inside the box.
            set height_new [expr {$height_m+4}]
            set width_new [expr {$width_m+4}]
            set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_new $height_new m]]
            if {$res != $res2} {
                set diff [compare_lists $res $res2]
                lappend debuginfo "res: $res, res2: $res2, diff: $diff"
                fail "place should be found, debuginfo: $debuginfo, height_new: $height_new width_new: $width_new"
            }

            # The width decreases and the height increases. Only north and south are found
            set width_new [expr {$width_m-4}]
            set height_new [expr {$height_m+4}]
            set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_new $height_new m]]
            if {$res != {place:north place:south}} {
                lappend debuginfo "res: $res"
                fail "place should not be found, debuginfo: $debuginfo, height_new: $height_new width_new: $width_new"
            }

            # The width increases and the height decreases. Only ease and west are found
            set width_new [expr {$width_m+4}]
            set height_new [expr {$height_m-4}]
            set res [lsort [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_new $height_new m]]
            if {$res != {place:east place:west}} {
                lappend debuginfo "res: $res"
                fail "place should not be found, debuginfo: $debuginfo, height_new: $height_new width_new: $width_new"
            }

            # make the box smaller by two meter in each direction to put the coordinate slightly outside the box.
            set height_new [expr {$height_m-4}]
            set width_new [expr {$width_m-4}]
            set res [r geosearch mypoints fromlonlat $search_lon $search_lat bybox $width_new $height_new m]
            if {$res != ""} {
                lappend debuginfo "res: $res"
                fail "place should not be found, debuginfo: $debuginfo, height_new: $height_new width_new: $width_new"
            }
            unset -nocomplain debuginfo
        }
    }
}
