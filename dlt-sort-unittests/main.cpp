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
    ASSERT_EQ(sizeof(DltStorageHeader), 16) << "DltStorageHeader has wrong size. New dlt version or wrong compiler settings?";
    ASSERT_EQ(sizeof(DltStandardHeader), 4);
	ASSERT_EQ(sizeof(DltStandardHeaderExtra), 12);
	ASSERT_EQ(sizeof(DltExtendedHeader), 10);
}

int main(int argc, char **argv){
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
