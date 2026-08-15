/// Integration test against the REAL zerialize library's BEVE backend --
/// its own translation unit and its own zerialize_beve_test executable
/// target (see test/CMakeLists.txt), compiled at C++23 via its own
/// target_compile_features(... cxx_std_23), since enabling BEVE links
/// glaze, which hard-requires C++23 in glaze's own CMakeLists.txt.
///
/// zerialize_real_test.cpp (the other 7 formats) also moves to C++23 once
/// this option is on, since it links the same `zerialize` target this one
/// does and CMake's required-standard computation is target-wide, not
/// per-file -- see that file's own header comment and test/CMakeLists.txt
/// for the full explanation. The thing that's actually guaranteed to stay
/// C++20 no matter what is multi_index_lru_test (every base test that
/// doesn't touch zerialize at all), which is the property that matters
/// for the base library and the C++20 compatibility-matrix CI jobs.
///
/// Only compiled when MULTI_INDEX_LRU_BUILD_ZERIALIZE_BEVE_TESTS is ON,
/// which also requires MULTI_INDEX_LRU_BUILD_ZERIALIZE_TESTS (see the
/// top-level CMakeLists.txt for the exact gating and a fetched-zerialize
/// explanation shared with zerialize_real_test.cpp).

#include "zerialize_test_utils.hpp"

#include <zerialize/protocols/beve.hpp>

namespace {

namespace z = zerialize;
using zerialize_test_utils::test_real_format;

TEST(ZerializeRealFormatsTest, BEVE) { test_real_format<z::Beve>(); }

}  // namespace
