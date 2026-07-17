#include <gtest/gtest.h>

#include "server_utils.h"

TEST(SafeNameTest, AcceptsNormalNames) {
    EXPECT_TRUE(safeName("backup.bak.enc"));
    EXPECT_TRUE(safeName("test_01.bin"));
}

TEST(SafeNameTest, RejectsEmptyName) {
    EXPECT_FALSE(safeName(""));
}

TEST(SafeNameTest, RejectsDangerousPaths) {
    EXPECT_FALSE(safeName("../backup.bak.enc"));
    EXPECT_FALSE(safeName("folder/backup.bak.enc"));
    EXPECT_FALSE(safeName("folder\\backup.bak.enc"));
    EXPECT_FALSE(safeName("backup..enc"));
}

TEST(UrlDecodeTest, DecodesPercentEncodingAndPlus) {
    EXPECT_EQ(urlDecode("course%20backup.bak.enc"),
              "course backup.bak.enc");
    EXPECT_EQ(urlDecode("hello+world"), "hello world");
}

TEST(QueryParamTest, ReturnsExistingParameters) {
    const std::string target =
        "/upload?filename=test%20file.bin&maxBackups=3";

    EXPECT_EQ(queryParam(target, "filename"), "test file.bin");
    EXPECT_EQ(queryParam(target, "maxBackups"), "3");
}

TEST(QueryParamTest, ReturnsEmptyWhenParameterIsMissing) {
    EXPECT_EQ(queryParam("/upload?filename=test.bin", "maxBackups"), "");
    EXPECT_EQ(queryParam("/health", "filename"), "");
}
