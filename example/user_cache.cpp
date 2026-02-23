/// User cache example with multiple indices

#include <multi_index_lru/container.hpp>

#include <iostream>
#include <string>

#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index/member.hpp>

// User structure with multiple searchable fields
struct User {
    int id;
    std::string email;
    std::string name;
    int age;
};

// Tags for different indices
struct IdTag {};
struct EmailTag {};
struct NameTag {};

// User cache with multiple unique and non-unique indices
using UserCache = multi_index_lru::Container<
    User,
    boost::multi_index::indexed_by<
        // Primary key: user ID (unique)
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<IdTag>,
            boost::multi_index::member<User, int, &User::id>>,
        // Email index (unique)
        boost::multi_index::ordered_unique<
            boost::multi_index::tag<EmailTag>,
            boost::multi_index::member<User, std::string, &User::email>>,
        // Name index (non-unique, multiple users can have same name)
        boost::multi_index::ordered_non_unique<
            boost::multi_index::tag<NameTag>,
            boost::multi_index::member<User, std::string, &User::name>>>>;

int main() {
    // Create a user cache with capacity of 100 users
    UserCache cache(100);

    // Add some users
    cache.emplace(User{1, "alice@example.com", "Alice", 30});
    cache.emplace(User{2, "bob@example.com", "Bob", 25});
    cache.emplace(User{3, "charlie@example.com", "Charlie", 35});
    cache.emplace(User{4, "alice2@example.com", "Alice", 28});  // Another Alice

    std::cout << "User cache contains " << cache.size() << " users\n\n";

    // Find by ID
    auto by_id = cache.find<IdTag>(1);
    if (by_id != cache.end<IdTag>()) {
        std::cout << "Found by ID 1: " << by_id->name << " (" << by_id->email << ")\n";
    }

    // Find by email
    auto by_email = cache.find<EmailTag>(std::string("bob@example.com"));
    if (by_email != cache.end<EmailTag>()) {
        std::cout << "Found by email: " << by_email->name << ", age " << by_email->age << "\n";
    }

    // Find by name (returns first match for non-unique index)
    auto by_name = cache.find<NameTag>(std::string("Alice"));
    if (by_name != cache.end<NameTag>()) {
        std::cout << "Found by name 'Alice': ID " << by_name->id << "\n";
    }

    // Check existence
    std::cout << "\nContains user ID 2: " << cache.contains<IdTag>(2) << "\n";
    std::cout << "Contains email 'nobody@example.com': " 
              << cache.contains<EmailTag>(std::string("nobody@example.com")) << "\n";

    // Erase by ID
    cache.erase<IdTag>(2);
    std::cout << "\nAfter erasing ID 2, cache size: " << cache.size() << "\n";

    // Change capacity
    cache.set_capacity(2);
    std::cout << "After reducing capacity to 2, size: " << cache.size() << "\n";

    return 0;
}
