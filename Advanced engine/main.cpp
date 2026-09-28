#include <iostream>
#include <algorithm>
#include <array>
#include <cassert> // Required for assert()
#include "../rdtsc_timer.h"
#include <vector>    // For std::vector


namespace {

    // Represents an order in the order book. Each order is a node in a doubly linked list.
    struct OrderNode {
        OrderNode* next = nullptr;
        OrderNode* prev = nullptr;
        int id;
        int price;
        int volume;
        bool is_buy_order;
    };

    // Constants for the order book
    constexpr int min_order_price = 1000000; // $1000.00
    constexpr int max_order_price = 5000000; // $5000.00
    constexpr int tick_size = 1;             // 1 cent

    // $5000 - $1000 = $4000 ticks. (+1 to include both ends)
    // Renamed from max_orders to separate the grid size from the physical memory limit
    constexpr size_t max_price_levels = ((max_order_price - min_order_price) / tick_size) + 1; // 4000001 price levels. $0 to $4000 inclusive. Each index corresponds to a price level.
    constexpr size_t max_pool_orders = 1000000; // The maximum number of active orders our memory pool can hold simultaneously

    struct PriceLevel { // Represents a price level in the order book, which can have multiple orders (linked list)
        OrderNode* head = nullptr;
        OrderNode* tail = nullptr;
    };

    // Statically allocated PriceLevel arrays (Data Segment)
    std::array<PriceLevel, max_price_levels> bid_book = {}; // Store the head and tail pointers for each buy order price level.
    std::array<PriceLevel, max_price_levels> ask_book = {}; // Store the head and tail pointers for each sell order price level.

    // Statically allocated OrderNode pool (Data Segment)
    std::array<OrderNode, max_pool_orders> order_pool = {}; // Pre-allocated pool of OrderNodes
    std::array<int, max_pool_orders> free_order_indices = {};
    int free_order_count = max_pool_orders; // Number of free orders available in the pool

}

// Function Declarations
static void process_order(OrderNode& new_order);
static int get_price_index(int price);
OrderNode* allocate_order(int price, int volume, bool is_buy);
void deallocate_order(OrderNode* resting_order);
static void cancel_order(int order_id);
static int submit_order(int price, int volume, bool is_buy);
static void run_synthetic_burst_test();


int main() {
    // 1. Warm the memory pool to prevent initial page faults
    for (int i = 0; i < max_pool_orders; ++i) {
        free_order_indices[i] = i;
    }

    // 2. Run the exact same burst test
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
        int price = 3000000 + (i % 10);
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

static int get_price_index(const int price) {
    if (price < min_order_price || price > max_order_price) return -1;
    return (price - min_order_price) / tick_size;
}

OrderNode* allocate_order(int price, int volume, bool is_buy) {
    if (free_order_count == 0) return nullptr; // Pool is completely full

    free_order_count--;
    size_t available_index = free_order_indices[free_order_count];

    OrderNode* new_node = &order_pool[available_index];

    // The physical index in the pool BECOMES the order ID
    new_node->id = available_index;
    new_node->price = price;
    new_node->volume = volume;
    new_node->is_buy_order = is_buy;
    new_node->next = nullptr;
    new_node->prev = nullptr;

    return new_node;
}

void deallocate_order(OrderNode* resting_order) {
    // Safety check: Ensure we aren't overflowing the free list
    // (This strips out completely in optimized release builds)
    assert(free_order_count < max_pool_orders && "Double-free detected: too many orders freed!");

    // Add the index of the deallocated order back to the free_order_indices stack
    size_t index = resting_order - &order_pool[0];
    free_order_indices[free_order_count] = index;
    free_order_count++;
}

static int submit_order(int price, int volume, bool is_buy) {
    // Check validity BEFORE touching the memory pool to prevent memory leaks if rejected
    if (get_price_index(price) == -1) {
        return -1;
    }

    // Safely pull a node from the permanent pool
    OrderNode* new_order = allocate_order(price, volume, is_buy);
    if (!new_order) {
        return -1;
    }

    // Pass the dereferenced pool node into your existing matching logic
    process_order(*new_order);

    // Hand the ID (ticket) back to the agent so they can track or cancel it later
    return new_order->id;

}

static void process_order(OrderNode& new_order) {

    int order_idx = get_price_index(new_order.price); // Convert price to index in the book. Each index corresponds to a price level.

    std::array<std::array<PriceLevel, max_price_levels>*, 2> books = { &bid_book, &ask_book };
    auto& match_book  = *books[new_order.is_buy_order]; // The book we are matching against (opposite side)
    auto& target_book = *books[!new_order.is_buy_order]; // The book we are adding to (same side)

    // 1. PRICE SWEEP SETUP
    // Buys sweep the ask book from lowest price (0) up to the limit price.
    // Sells sweep the bid book from highest price (max) down to the limit price.
    int current_idx = new_order.is_buy_order ? 0 : (max_price_levels - 1); // Start at the best price (index) for the side we are matching against
    int end_idx     = order_idx; // Stop at the limit price of the incoming order
    int step        = new_order.is_buy_order ? 1 : -1; // Step direction for the sweep, 1 for buys (up), -1 for sells (down)

    // 2. MATCHING PHASE
    while (new_order.volume > 0) {
        // Stop if we have swept past the order's limit price
        if ((new_order.is_buy_order && current_idx > end_idx) || // for buys, stops when current index exceeds the limit price index
            (!new_order.is_buy_order && current_idx < end_idx)) { // for sells, stops when current index is less than the limit price index
            break;
        }

        PriceLevel& level = match_book[current_idx]; // Gets the linked list (memory address) of resting orders at this price level
        OrderNode* resting_order = level.head; // Start at the head of the linked list for this price level

        // Traverse the linked list at this specific price level
        while (resting_order != nullptr && new_order.volume > 0) { // repeat till either the incoming order is fully filled or there are no
                                                                   // more resting orders at this price level (i.e., the level variable is finished)

            // Branchless volume reduction
            int fill_qty = std::min(new_order.volume, resting_order->volume);  // determine the fill quantity, which is the minimum of the
                                                                                    // incoming order's volume and the resting order's volume
            new_order.volume -= fill_qty;
            resting_order->volume -= fill_qty;

            if (resting_order->volume == 0) { // Resting order fully filled
                // Queue Pop: Move head pointer to the next order in the linked list. This effectively removes the filled order from the queue.
                level.head = resting_order->next;

                if (level.head != nullptr) { // If there is a next order, update its prev pointer to nullptr because prev order filled and removed
                    level.head->prev = nullptr;
                } else { // If there is no next order, the queue is now empty
                    level.tail = nullptr; // head and tail both point to nullptr, indicating an empty queue
                }

                // Return 'resting_order' memory back to your custom allocator
                deallocate_order(resting_order);

                resting_order = level.head; // Advance to next order in queue
            } else {
                break; // Resting order partially filled; incoming order must be empty. No change to the linked list, so we can break out of the loop.
            }
        }

        current_idx += step; // Move to the next worse price tick. Moves up for buys, down for sells.
    }

    // 3. RESTING PHASE
    if (new_order.volume > 0) { // If the incoming order is not fully filled, it becomes a resting order in the book
        PriceLevel& target_level = target_book[order_idx]; // Get the linked list (memory address) of resting orders at the price level of the new order's price

        // Queue Push: Append to the tail of the linked list
        if (target_level.tail == nullptr) { // If the queue is empty, set both head and tail to the new order
            target_level.head = &new_order;
            target_level.tail = &new_order;
        } else {
            target_level.tail->next = &new_order; // Link the current tail's next pointer to the new order
            new_order.prev = target_level.tail; // Link the new order's prev pointer to the current tail
            target_level.tail = &new_order; // Update the tail pointer to the new order
        }
    }
}

static void cancel_order(int order_id) {

    // order_id is exactly the index where the order lives.
    OrderNode* target = &order_pool[order_id];

    // Check if the order is already dead/filled before trying to cancel
    if (target->volume == 0) {
        return;
    }

    // Find which PriceLevel queue this order belongs to
    int price_idx = get_price_index(target->price);
    std::array<std::array<PriceLevel, max_price_levels>*, 2> books = { &bid_book, &ask_book };
    auto& book  = *books[!target->is_buy_order]; // Logical NOT ensures we target the correct side's book
    PriceLevel& level = book[price_idx];

    // Snip the Prev Pointer
    if (target->prev != nullptr) { // If the target order has a previous order, link that previous order's next pointer to the target's next order
        target->prev->next = target->next;
    } else {
        level.head = target->next; // If the target order is the head of the queue, update the head pointer to the next order in the queue
    }

    // Snip the Next Pointer
    if (target->next != nullptr) { // If the target order has a next order, link that next order's prev pointer to the target's previous order
        target->next->prev = target->prev;
    } else {
        level.tail = target->prev; // If the target order is the tail of the queue, update the tail pointer to the previous order in the queue
    }

    // Recycle the Memory
    target->volume = 0; // Mark the order as dead/filled to prevent double-freeing
    deallocate_order(target); // Return the order's memory back to the custom allocator
}