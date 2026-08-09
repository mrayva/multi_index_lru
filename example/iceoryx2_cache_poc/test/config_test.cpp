#include "../src/config.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace poc::config {
namespace {

// setenv/unsetenv touch real process environment -- scope each test to its
// own env var name so tests can't interfere with each other or leak state.
class ConfigTest : public ::testing::Test {
protected:
    void TearDown() override { unsetenv("MIL_TEST_CONFIG_XYZ"); }
};

// --- take_flag ---------------------------------------------------------

TEST(TakeFlag, TwoTokenFormIsFoundAndRemoved) {
    std::vector<std::string> args{"get", "--service-name", "custom", "name", "alice"};
    auto value = take_flag(args, "--service-name");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "custom");
    EXPECT_EQ(args, (std::vector<std::string>{"get", "name", "alice"}));
}

TEST(TakeFlag, EqualsFormIsFoundAndRemoved) {
    std::vector<std::string> args{"get", "--service-name=custom", "name", "alice"};
    auto value = take_flag(args, "--service-name");
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, "custom");
    EXPECT_EQ(args, (std::vector<std::string>{"get", "name", "alice"}));
}

TEST(TakeFlag, AbsentFlagReturnsNulloptAndLeavesArgsUntouched) {
    std::vector<std::string> args{"get", "name", "alice"};
    auto value = take_flag(args, "--service-name");
    EXPECT_FALSE(value.has_value());
    EXPECT_EQ(args, (std::vector<std::string>{"get", "name", "alice"}));
}

TEST(TakeFlag, MissingValueThrows) {
    std::vector<std::string> args{"get", "--service-name"};
    EXPECT_THROW(take_flag(args, "--service-name"), std::runtime_error);
}

TEST(TakeFlag, OnlyMatchesExactFlagNotAPrefixOfAnotherFlag) {
    // "--name-bucket" must not be matched when looking for "--id-bucket".
    std::vector<std::string> args{"--name-bucket", "mil_by_name"};
    auto value = take_flag(args, "--id-bucket");
    EXPECT_FALSE(value.has_value());
    EXPECT_EQ(args, (std::vector<std::string>{"--name-bucket", "mil_by_name"}));
}

// --- resolve_* precedence: CLI flag > env var > default -----------------

TEST_F(ConfigTest, ResolveStrPrefersFlagOverEnvOverDefault) {
    setenv("MIL_TEST_CONFIG_XYZ", "from-env", 1);
    std::vector<std::string> args{"--x", "from-flag"};
    EXPECT_EQ(resolve_str(args, "--x", "MIL_TEST_CONFIG_XYZ", "from-default"), "from-flag");
}

TEST_F(ConfigTest, ResolveStrFallsBackToEnvWhenNoFlag) {
    setenv("MIL_TEST_CONFIG_XYZ", "from-env", 1);
    std::vector<std::string> args{};
    EXPECT_EQ(resolve_str(args, "--x", "MIL_TEST_CONFIG_XYZ", "from-default"), "from-env");
}

TEST_F(ConfigTest, ResolveStrFallsBackToDefaultWhenNeitherGiven) {
    std::vector<std::string> args{};
    EXPECT_EQ(resolve_str(args, "--x", "MIL_TEST_CONFIG_XYZ", "from-default"), "from-default");
}

TEST_F(ConfigTest, ResolveU16ParsesFlagAndEnv) {
    std::vector<std::string> args{"--port", "4222"};
    EXPECT_EQ(resolve_u16(args, "--port", "MIL_TEST_CONFIG_XYZ", 9999), 4222);

    setenv("MIL_TEST_CONFIG_XYZ", "1234", 1);
    std::vector<std::string> empty_args{};
    EXPECT_EQ(resolve_u16(empty_args, "--port", "MIL_TEST_CONFIG_XYZ", 9999), 1234);
}

TEST_F(ConfigTest, ResolveSizeParsesFlagAndEnv) {
    std::vector<std::string> args{"--capacity", "5000"};
    EXPECT_EQ(resolve_size(args, "--capacity", "MIL_TEST_CONFIG_XYZ", 1000u), 5000u);

    std::vector<std::string> empty_args{};
    EXPECT_EQ(resolve_size(empty_args, "--capacity", "MIL_TEST_CONFIG_XYZ", 1000u), 1000u);
}

TEST_F(ConfigTest, ResolveIntParsesFlagAndEnv) {
    std::vector<std::string> args{"--threshold", "7"};
    EXPECT_EQ(resolve_int(args, "--threshold", "MIL_TEST_CONFIG_XYZ", 3), 7);
}

TEST_F(ConfigTest, ResolveMillisParsesFlagAndEnv) {
    std::vector<std::string> args{"--timeout-ms", "500"};
    EXPECT_EQ(resolve_millis(args, "--timeout-ms", "MIL_TEST_CONFIG_XYZ", 3000), std::chrono::milliseconds(500));

    std::vector<std::string> empty_args{};
    EXPECT_EQ(resolve_millis(empty_args, "--timeout-ms", "MIL_TEST_CONFIG_XYZ", 3000), std::chrono::milliseconds(3000));
}

}  // namespace
}  // namespace poc::config
