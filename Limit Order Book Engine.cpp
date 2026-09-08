#include <iostream>
#include <vector>
#include <algorithm>

struct Order
{
    int id;
    int price; // Fixed-point integer (e.g., in cents) to avoid double precision bugs
    int quantity;
    bool is_buy_order;
};

// Function declaration above main
void process_order(Order new_order, std::vector<Order>& bid_book, std::vector<Order>& ask_book, size_t max_orders);

int main() 
{
    constexpr size_t max_orders = 100;
    std::vector<Order> bid_book;
    std::vector<Order> ask_book;

    bid_book.reserve(max_orders);
    ask_book.reserve(max_orders);

    // Test Buy Order: Price = $10.50 (1050 cents), Qty = 10
    Order incoming_buy{ 1, 1050, 10, true };
    process_order(incoming_buy, bid_book, ask_book, max_orders);

    return 0;
}

void process_order(Order new_order, std::vector<Order>& bid_book, std::vector<Order>& ask_book, size_t max_orders)
{
    // Determine target books using references
    auto& match_book = new_order.is_buy_order ? ask_book : bid_book;
    auto& target_book = new_order.is_buy_order ? bid_book : ask_book;

    // 1. MATCHING PHASE
    for (size_t i = 0; i < match_book.size() && new_order.quantity > 0; ) 
    {
        bool is_match = new_order.is_buy_order ? (new_order.price >= match_book[i].price)
            : (new_order.price <= match_book[i].price);

        if (is_match) 
        {
            // Clean, bug-free fill calculation
            int fill_qty = std::min(new_order.quantity, match_book[i].quantity);

            new_order.quantity -= fill_qty;
            match_book[i].quantity -= fill_qty;

            std::cout << "Matched " << fill_qty << " units between Order #"
                << new_order.id << " and Order #" << match_book[i].id << "\n";

            // If passive order is fully filled, remove it safely
            if (match_book[i].quantity == 0)
            {
                match_book.erase(match_book.begin() + i);
                // Do NOT increment 'i' because elements shifted left
            }
            else
            {
                ++i; // Move to next order only if we didn't erase
            }
        }
        else
        {
            ++i; // No price match, move to next order
        }
    }

    // 2. RESTING PHASE (Only store if quantity remains!)
    if (new_order.quantity > 0) 
    {
        if (target_book.size() >= max_orders)
        {
            std::cout << "Book full! Cannot add resting order #" << new_order.id << "\n";
        }
        else
        {
            target_book.push_back(new_order);
            std::cout << "Resting Order #" << new_order.id << " added with remaining Qty: "
                << new_order.quantity << "\n";
        }
    }
}