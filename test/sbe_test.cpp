// Copyright 2026 multi_index_lru contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <multi_index_lru/container.hpp>
#include <multi_index_lru/expirable_container.hpp>
#include <multi_index_lru/sbe_cache.hpp>

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/ordered_index.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <tuple>
#include <vector>

namespace {

struct MockSbeView {
    explicit MockSbeView(std::span<const uint8_t> bytes)
        : bytes(bytes)
    {}

    std::uint16_t template_id() const {
        return static_cast<std::uint16_t>(bytes[0]) |
               (static_cast<std::uint16_t>(bytes[1]) << 8U);
    }

    std::uint32_t security_id() const {
        return static_cast<std::uint32_t>(bytes[2]) |
               (static_cast<std::uint32_t>(bytes[3]) << 8U) |
               (static_cast<std::uint32_t>(bytes[4]) << 16U) |
               (static_cast<std::uint32_t>(bytes[5]) << 24U);
    }

    std::span<const uint8_t> bytes;
};

MockSbeView MakeMockSbeView(std::span<const uint8_t> payload) {
    return MockSbeView{payload};
}

std::vector<uint8_t> MakePayload(std::uint16_t template_id, std::uint32_t security_id) {
    return {
        static_cast<uint8_t>(template_id & 0xFFU),
        static_cast<uint8_t>((template_id >> 8U) & 0xFFU),
        static_cast<uint8_t>(security_id & 0xFFU),
        static_cast<uint8_t>((security_id >> 8U) & 0xFFU),
        static_cast<uint8_t>((security_id >> 16U) & 0xFFU),
        static_cast<uint8_t>((security_id >> 24U) & 0xFFU),
    };
}

struct TemplateTag {};
struct SecurityTag {};
struct CompositeTag {};

using Entry = multi_index_lru::SbeEntryWithKeys_t<std::uint16_t, std::uint32_t>;

using Cache = multi_index_lru::Container<
    Entry,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<TemplateTag>,
            multi_index_lru::sbe_key<0, Entry>>,
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<SecurityTag>,
            multi_index_lru::sbe_key<1, Entry>>,
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<CompositeTag>,
            boost::multi_index::composite_key<
                Entry,
                multi_index_lru::sbe_key<0, Entry>,
                multi_index_lru::sbe_key<1, Entry>>>>>;

using ExpirableCache = multi_index_lru::ExpirableContainer<
    Entry,
    boost::multi_index::indexed_by<
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<TemplateTag>,
            multi_index_lru::sbe_timestamped_key<0, Entry>>,
        boost::multi_index::hashed_unique<
            boost::multi_index::tag<SecurityTag>,
            multi_index_lru::sbe_timestamped_key<1, Entry>>>>;

TEST(SbeCacheTest, BuilderCopiesBytesAndIsLifetimeSafe) {
    auto builder = multi_index_lru::make_sbe_entry_builder<Entry>(
        MakeMockSbeView,
        multi_index_lru::make_sbe_field<std::uint16_t>(&MockSbeView::template_id),
        multi_index_lru::make_sbe_field<std::uint32_t>(&MockSbeView::security_id));

    auto payload = MakePayload(7, 1234);
    auto entry = builder.build(payload);

    payload[0] = 0x55;
    payload[1] = 0x55;
    payload[2] = 0xAA;

    auto view = entry.view(MakeMockSbeView);
    EXPECT_EQ(view.template_id(), 7);
    EXPECT_EQ(view.security_id(), 1234U);
}

TEST(SbeCacheTest, ContainerIndexesOverExtractedKeys) {
    auto builder = multi_index_lru::make_sbe_entry_builder<Entry>(
        MakeMockSbeView,
        multi_index_lru::make_sbe_field<std::uint16_t>(&MockSbeView::template_id),
        multi_index_lru::make_sbe_field<std::uint32_t>(&MockSbeView::security_id));

    Cache cache(3);
    cache.emplace(builder.build(MakePayload(7, 1001)));
    cache.emplace(builder.build(MakePayload(8, 1002)));

    auto by_template = cache.find<TemplateTag>(7);
    ASSERT_NE(by_template, cache.end<TemplateTag>());
    EXPECT_EQ(std::get<1>(by_template->keys), 1001U);

    auto by_security = cache.find<SecurityTag>(1002U);
    ASSERT_NE(by_security, cache.end<SecurityTag>());
    EXPECT_EQ(std::get<0>(by_security->keys), 8);

    auto by_composite = cache.find<CompositeTag>(std::make_tuple<std::uint16_t, std::uint32_t>(8, 1002));
    ASSERT_NE(by_composite, cache.end<CompositeTag>());
    EXPECT_EQ(by_composite->raw_data().size(), 6U);
}

TEST(SbeCacheTest, ExpirableContainerWorksWithTimestampedSbeKeys) {
    auto builder = multi_index_lru::make_sbe_entry_builder<Entry>(
        MakeMockSbeView,
        multi_index_lru::make_sbe_field<std::uint16_t>(&MockSbeView::template_id),
        multi_index_lru::make_sbe_field<std::uint32_t>(&MockSbeView::security_id));

    ExpirableCache cache(2, std::chrono::hours(1));
    cache.emplace(builder.build(MakePayload(11, 2001)));

    auto it = cache.find<SecurityTag>(2001U);
    ASSERT_NE(it, cache.end<SecurityTag>());
    EXPECT_EQ(std::get<0>(it->keys), 11);
}

}  // namespace
