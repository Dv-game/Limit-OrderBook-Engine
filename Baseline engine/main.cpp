#include <iostream>
#include <algorithm> // For std::sort
#include <map>
#include <list>
#include <unordered_map>
#include "../rdtsc_timer.h"
#include <vector>    // For std::vector

namespace {

    // Represents a single order. Unlike the optimized engine, this does not manage its own
    // next/prev pointers. The C++ Standard Template Library (STL) does it for us.
    struct OrderNode {
        int id;
        int price;
        int volume;
        bool is_buy_order;
    };

    // A PriceLevel represents a specific price tick in the book.
    // std::list is used as it is a standard heap-allocated doubly-linked list.
    // Every time an order is added here, the OS must find free RAM on the heap (which is slow).
    struct PriceLevel {
        std::list<OrderNode> orders;
    };

    // std::map is implemented as a Red-Black Tree under the hood.
    // It automatically sorts its elements based on the key (the price).
    // std::greater<int> forces the Bid Book to sort Highest-to-Lowest price (best bids first).
    std::map<int, PriceLevel, std::greater<int>> bid_book;

    // std::less<int> forces the Ask Book to sort Lowest-to-Highest price (best asks first).
    std::map<int, PriceLevel, std::less<int>> ask_book;

    // To cancel an order quickly, we need to know exactly where it lives in the heap memory.
    // We store an iterator (a smart pointer) to the exact node inside the std::list.
    struct OrderLocation { // This struct is stored in the Hash Table for O(1) lookups.
        bool is_buy;
        int price;
        std::list<OrderNode>::iterator list_iterator; // Points to the exact location of the order in the linked list (RAM) for O(1) lookup and deletion.
    };

    // An unordered_map (Hash Table) allows us to look up an order's location using its ID.
    // This provides average O(1) lookups, but calculating the hash takes CPU cycles.
    std::unordered_map<int, OrderLocation> order_tracker;

    int next_order_id = 0; // Simple sequential ID generator for baseline testing
}


// function declarations
static int submit_order(int price, int volume, bool is_buy);
static void cancel_order(int order_id);
static void run_synthetic_burst_test();


int main() {
    run_synthetic_burst_test();
    return 0;
}

static void run_synthetic_burst_test() {
    const int num_orders = 10000;

    // Pre-allocate memory so vector resizing doesn't skew our timer
    std::vector<unsigned long long> latencies;
    latencies.reserve(num_orders);

    std::cout << "Starting Baseline Burst Load Test (" << num_orders << " orders)...\n";

    for (int i = 0; i < num_orders; ++i) {
        // Generate simulated order flow tightly clustered around a price
        int price = 3000 + (i % 10);
        bool is_buy = (i % 2 == 0);
        int volume = 100;

        // --- THE HOT PATH ---
        unsigned long long start_cycles = Profiling::get_cpu_cycles();

        submit_order(price, volume, is_buy); // Triggers heap allocations in baseline

        unsigned long long end_cycles = Profiling::get_cpu_cycles();
        // --------------------

        latencies.push_back(end_cycles - start_cycles);
    }

    // Sort the latencies from fastest to slowest to calculate percentiles
    std::sort(latencies.begin(), latencies.end());

    // Array indices for percentiles
    size_t p50_index = num_orders * 0.50;
    size_t p99_index = num_orders * 0.99;
    size_t p99_9_index = num_orders * 0.999;

    std::cout << "--- Latency Results (CPU Cycles) ---\n";
    std::cout << "Median (P50): " << latencies[p50_index] << " cycles\n";
    std::cout << "99th Percentile (P99): " << latencies[p99_index] << " cycles\n";
    std::cout << "99.9th Percentile (P99.9): " << latencies[p99_9_index] << " cycles\n";
    std::cout << "Worst Case (Max): " << latencies.back() << " cycles\n";
}

static int submit_order(int price, int volume, bool is_buy) {
    int current_id = ++next_order_id; // Generate a new ID
    OrderNode new_order = { current_id, price, volume, is_buy }; // Create the order object on the stack

    // 1. MATCHING PHASE
    if (is_buy) {
        // Buys match against the Ask book.
        // ask_book.begin() gives us the lowest available ask price because of std::less.
        auto it = ask_book.begin();

        // Loop while:
        // 1. There are still price levels in the book.
        // 2. The incoming order still has volume left to fill.
        // 3. The best ask price is less than or equal to the incoming buy's limit price.
        while (it != ask_book.end() && new_order.volume > 0 && it->first <= new_order.price) {
            auto& level_orders = it->second.orders; // Get the linked list of orders at this price
            auto order_it = level_orders.begin();   // Start at the front of the line (time priority)

            // Traverse the queue of orders at this specific price
            while (order_it != level_orders.end() && new_order.volume > 0) {
                // Determine how many shares can actually be traded
                int fill_qty = std::min(new_order.volume, order_it->volume);

                new_order.volume -= fill_qty;
                order_it->volume -= fill_qty;

                if (order_it->volume == 0) {
                    // Resting order is fully filled.
                    // Erase it from the hash map first so we don't have dangling trackers.
                    order_tracker.erase(order_it->id);

                    // Erase it from the list.
                    // CRITICAL LATENCY HIT: std::list::erase calls 'delete' to free heap memory, asking the OS to intervene.
                    order_it = level_orders.erase(order_it);
                } else {
                    break; // Resting order is only partially filled; incoming order must be completely empty.
                }
            }

            // If we wiped out every order at this price, the price level is empty.
            if (level_orders.empty()) {
                // Remove the empty price level from the Red-Black Tree.
                // CRITICAL LATENCY HIT: Triggers tree rebalancing and memory deallocation.
                it = ask_book.erase(it);
            } else {
                ++it; // Move to the next worse price tick in the tree
            }
        }
    } else {
        // Sells match against the Bid book.
        // bid_book.begin() gives us the highest available bid price because of std::greater.
        auto it = bid_book.begin();

        // Loop while the best bid price is greater than or equal to the incoming sell's limit price.
        while (it != bid_book.end() && new_order.volume > 0 && it->first >= new_order.price) {
            auto& level_orders = it->second.orders;
            auto order_it = level_orders.begin();

            while (order_it != level_orders.end() && new_order.volume > 0) {
                int fill_qty = std::min(new_order.volume, order_it->volume);
                new_order.volume -= fill_qty;
                order_it->volume -= fill_qty;

                if (order_it->volume == 0) {
                    order_tracker.erase(order_it->id);
                    order_it = level_orders.erase(order_it);
                } else {
                    break;
                }
            }

            if (level_orders.empty()) {
                it = bid_book.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 2. RESTING PHASE
    // If the incoming order did not fully fill, it must be added to the book to wait for a match.
    if (new_order.volume > 0) {
        if (is_buy) {
            // bid_book[price] traverses the Red-Black Tree to find the price level (O(log N)).
            // If the price doesn't exist, it creates a new PriceLevel node.
            // push_back allocates new heap memory for the order node.
            bid_book[price].orders.push_back(new_order);

            // Save the exact memory location to the tracker hash map for O(1) cancellations later.
            // std::prev gets an iterator pointing to the very last element we just pushed.
            order_tracker[current_id] = { is_buy, price, std::prev(bid_book[price].orders.end()) };
        } else {
            ask_book[price].orders.push_back(new_order);
            order_tracker[current_id] = { is_buy, price, std::prev(ask_book[price].orders.end()) };
        }
    }

    return current_id;
}

static void cancel_order(int order_id) {
    // 1. Look up the order in the Hash Table
    auto tracker_it = order_tracker.find(order_id);
    if (tracker_it == order_tracker.end()) {
        return; // Order ID not found (it was likely already fully matched and erased)
    }

    // Extract the saved location data
    const OrderLocation& loc = tracker_it->second;

    // 2. Erase the order from the correct linked list
    if (loc.is_buy) {
        // We use the saved iterator to delete the node directly in O(1) time without looping.
        bid_book[loc.price].orders.erase(loc.list_iterator);

        // If that was the last order at this price, delete the price level from the tree.
        if (bid_book[loc.price].orders.empty()) {
            bid_book.erase(loc.price);
        }
    } else {
        ask_book[loc.price].orders.erase(loc.list_iterator);
        if (ask_book[loc.price].orders.empty()) {
            ask_book.erase(loc.price);
        }
    }

    // 3. Remove the tracking record from the Hash Table
    order_tracker.erase(tracker_it);
}