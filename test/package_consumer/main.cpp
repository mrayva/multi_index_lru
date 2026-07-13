#include <multi_index_lru/container.hpp>

#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

struct IdTag {};

struct Entry {
    int id;
};

using Cache = multi_index_lru::Container<
    Entry,
    boost::multi_index::indexed_by<boost::multi_index::ordered_unique<
        boost::multi_index::tag<IdTag>,
        boost::multi_index::member<Entry, int, &Entry::id>>>>;

int main() {
    Cache cache(2);
    cache.emplace(Entry{1});
    cache.emplace(Entry{2});
    cache.emplace(Entry{3});

    return cache.contains_no_update<IdTag>(1) || !cache.contains_no_update<IdTag>(3);
}
