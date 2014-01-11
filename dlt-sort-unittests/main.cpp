//
//  main.cpp
//  dlt-sort-unittests
//
//  Created by Matthias Behr on 11.01.14.
//  Copyright (c) 2014 Matthias Behr. All rights reserved.
//

#include "dlt-sort.h"
#include "gtest/gtest.h"

TEST(BASIC_ASSUMPTIONS, size_of_dlt_structs) {
    ASSERT_EQ(sizeof(char), 1);
    ASSERT_EQ(sizeof(int32_t), 4);
    ASSERT_EQ(sizeof(DltStorageHeader), 16) << "DltStorageHeader has wrong size. New dlt version or wrong compiler settings?";
    ASSERT_EQ(sizeof(DltStandardHeader), 4);
	ASSERT_EQ(sizeof(DltStandardHeaderExtra), 12);
	ASSERT_EQ(sizeof(DltExtendedHeader), 10);
}

TEST(Lifecycle_Tests, basic_tests) {
    Lifecycle lc;
    ASSERT_EQ(lc.min_tmsp, 0);
    ASSERT_EQ(lc.max_tmsp, 0);
    ASSERT_EQ(lc.usec_begin, 0);
    ASSERT_EQ(lc.usec_end, 0);
    ASSERT_EQ(lc.msgs.size(), 0);
    ASSERT_FALSE(lc.rel_offset_valid);
}

TEST(Lifecycle_Tests, from_DltMessage) {
    DltMessage m;
    init_DltMessage(m);
    m.storageheader->seconds = 61;
    m.storageheader->microseconds = 2;
    m.headerextra.tmsp=0;
    // case 1 init from DltMessage without tmsp:
    Lifecycle lc(m);
    ASSERT_EQ(lc.min_tmsp, 0);
    ASSERT_EQ(lc.max_tmsp, 0);
    ASSERT_EQ(lc.usec_begin, (61LL*usecs_per_sec)+2LL);
    ASSERT_EQ(lc.usec_end, lc.usec_begin);
    ASSERT_EQ(lc.msgs.size(), 1);
    ASSERT_EQ(lc.rel_offset_valid, false);
    // case 2 init from DltMessage with tmsp:
    m.headerextra.tmsp = 50;
    Lifecycle lc2(m);
    ASSERT_EQ(50, lc2.min_tmsp);
    ASSERT_EQ(50, lc2.max_tmsp);
    ASSERT_EQ(lc2.usec_begin, ((61LL*usecs_per_sec)+2LL)-(50LL*100LL));
    ASSERT_EQ(lc2.usec_end, ((61LL*usecs_per_sec)+2LL));
    ASSERT_EQ(1, lc2.msgs.size());
    ASSERT_TRUE(lc2.rel_offset_valid);

}

TEST(Lifecycle_Tests, fitsin) {
    Lifecycle lc;
    ASSERT_EQ(0, lc.calc_min_time());
    lc.usec_begin = 2LL*usecs_per_sec;
    lc.usec_end = 3LL*usecs_per_sec;
    // LC now begins at second 2 and ends at second 3
    DltMessage m;
    init_DltMessage(m);
    // simulate a msg that gets received at second 42
    // and has tmsp like 39.5s -> was transmitted at second 2.5
    m.storageheader->seconds=42;
    m.storageheader->microseconds=0;
    m.headerextra.tmsp = 395 * 1000;
    
    ASSERT_TRUE(lc.fitsin(m));
    // now the lifecycle should still start at sec 2 but end at second 42
    // and have 1 msg more
    ASSERT_EQ(1, lc.msgs.size());
    ASSERT_EQ(2LL*usecs_per_sec, lc.usec_begin);
    ASSERT_EQ(42LL*usecs_per_sec, lc.usec_end);
    ASSERT_EQ(395*1000, lc.min_tmsp);
    ASSERT_EQ(lc.min_tmsp, lc.max_tmsp);
    
    // now use a msg that extends the begin:
    // it get's received at sec 10 (so within [2,42])
    // with a timestamp of 9s -> so lc start was atleast 1s abs
    m.storageheader->seconds = 10;
    m.storageheader->microseconds=0;
    m.headerextra.tmsp = 90 * 1000;
    ASSERT_TRUE(lc.fitsin(m));
    // now the lifecycle should start at sec 1, still end at second 42
    // and have 2 msg
    ASSERT_EQ(2, lc.msgs.size());
    ASSERT_EQ(1LL*usecs_per_sec, lc.usec_begin);
    ASSERT_EQ(42LL*usecs_per_sec, lc.usec_end);
    ASSERT_EQ(90*1000, lc.min_tmsp);
    ASSERT_EQ(395*1000, lc.max_tmsp);

    // now check one msg that doesn't fit it: 0,5s recvd, tmsp 50 so abs start 0.495s
    // doesn't fit because it's recvd outside (before) the current lifecycle and
    m.storageheader->seconds = 0;
    m.storageheader->microseconds = 5000;
    m.headerextra.tmsp = 50;
    ASSERT_FALSE(lc.fitsin(m));
    ASSERT_EQ(2, lc.msgs.size());
    
    // now check one msgs that is after the current one and even the abs start is not within:
    // recv at 43, tmsp 50 so abs start at 42.995s
    m.storageheader->seconds = 43;
    m.storageheader->microseconds = 0;
    m.headerextra.tmsp = 50;
    ASSERT_FALSE(lc.fitsin(m));
    ASSERT_EQ(2, lc.msgs.size());

    // now check one msgs that is after the current one and even the abs start is not within:
    // recv at 43, tmsp 0.9999s so abs start at 42.0001s
    m.storageheader->seconds = 43;
    m.storageheader->microseconds = 0;
    m.headerextra.tmsp = 9999;
    ASSERT_FALSE(lc.fitsin(m));
    ASSERT_EQ(2, lc.msgs.size());
    
    // now check one msgs that is after the current one and even the abs start is not within:
    // recv at 43, tmsp 1s so abs start at 42.000s
    // this is a corner case: the lifecycle currently is from [1,42]
    // this msg was send at abs 42s so exactly at the border
    // it will be accepted. the rational is that the
    // ECU was still able to transmit so the lifecycle was active
    // this case is treated as a really delayed msg (delayed for 41s)
    m.storageheader->seconds = 43;
    m.storageheader->microseconds = 0;
    m.headerextra.tmsp = 9999;
    ASSERT_FALSE(lc.fitsin(m));
    ASSERT_EQ(2, lc.msgs.size());

}

TEST(Lifecycle_Tests, calc_min_time) {
    Lifecycle lc;
    ASSERT_EQ(lc.calc_min_time(), 0);
    lc.usec_begin = 1LL*usecs_per_sec;
    ASSERT_EQ(lc.calc_min_time(), 1LL*usecs_per_sec);
    lc.min_tmsp = 50; // should not matter
    ASSERT_EQ(lc.calc_min_time(), 1LL*usecs_per_sec);
    // LC now begins and ends at second 1
    // now add one message, this should matter as well:
    DltMessage m;
    init_DltMessage(m);
    // todo
}



TEST(Lifecycle_Tests, expand_if_intersects) {
    
}



int main(int argc, char **argv){
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
