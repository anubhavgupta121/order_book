#include <bits/stdc++.h>
#include <random>
#include <variant>
#include <ext/pb_ds/assoc_container.hpp> // Common file
#include <ext/pb_ds/tree_policy.hpp>     // Tree-based structures



using namespace std;


template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

enum class Side{Bid,Ask};


struct Trade{
    double price;
    int quantity;
    int aggressor_id;
    int rest_id;

};

struct Order {
    int id;
    int quantity;
    Side side;
    bool cancelled;
};

struct NewOrderRequest{
    double price;
    int quantity;
    Side side;
};

struct CancelOrderRequest{
    int id;
};


struct MarketOrderRequest{
    int quantity;
    Side side;
};

double Min_price = 0;
double max_price = 105.00;
double tick_size = 1.0;


int conv_price_to_idx(double price){
     return (price - Min_price)/tick_size;
}
double conv_idx_to_price(int idx){
    return (idx*tick_size) + Min_price;
}
int id_gen = 0;
map<int,Order> Order_info;
vector<vector<Order>> ask(conv_price_to_idx(max_price) + 5 );
vector<vector<Order>> bid(conv_price_to_idx(max_price) + 5);
vector<int> ask_actv_count(conv_price_to_idx(max_price) + 5);
vector<int> bid_actv_count(conv_price_to_idx(max_price) + 5);
unordered_map<int,pair<double,int>> Order_index;

vector<Trade> Trade_log;


using Request = variant<NewOrderRequest, CancelOrderRequest,MarketOrderRequest>;
int best_ask_indx = 0;
int best_bid_index = bid.size() - 1;



int add_order(NewOrderRequest req){
     Order new_order = {id_gen,req.quantity,req.side,false};
     Order_info[new_order.id] = new_order;
    
     if(new_order.side == Side::Bid){
            int idx = conv_price_to_idx(req.price);
            bid[idx].push_back(new_order);
            Order_index[new_order.id] =  {req.price,bid[idx].size() - 1};
            bid_actv_count[idx]++;
            if(idx > best_bid_index || bid[best_bid_index].empty()){
                best_bid_index = idx;
            }
            
     }
     else if(new_order.side == Side::Ask){
            int idx = conv_price_to_idx(req.price);
            ask[idx].push_back(new_order);
            Order_index[new_order.id] =  {req.price,ask[idx].size() - 1};
            ask_actv_count[idx]++;
            if(idx < best_ask_indx || ask[best_ask_indx].empty()){
                best_ask_indx = idx;
            }
            
     }
     return new_order.id;
     
}
int cancel_order(CancelOrderRequest req){
    
    Order to_cancel = Order_info[req.id];
    if(to_cancel.side == Side::Bid){
        auto [price,idx] =  Order_index[req.id];
        bid[conv_price_to_idx(price)][idx].cancelled = true;
        bid_actv_count[conv_price_to_idx(price)]--;
    }
    else if(to_cancel.side == Side::Ask){
        auto [price,idx] =  Order_index[req.id];
        ask[conv_price_to_idx(price)][idx].cancelled = true;
        ask_actv_count[conv_price_to_idx(price)]--;  
    }
    Order_info[req.id].cancelled = true;
    return - 1;
    
}



optional<NewOrderRequest> matching_loop(NewOrderRequest req){
        if(req.side == Side::Bid){
              for(int idx = best_ask_indx;idx < ask.size();++idx){
                 double price = conv_idx_to_price(idx) ;
                 if(ask_actv_count[idx] == 0){
                    continue;
                 }
                 if(price <= req.price ){
                      for(auto &item : ask[idx]){
                         if(item.cancelled){   //break if item cancelled
                            continue;
                         }
                         if(req.quantity == 0){
                            break;
                         }
                         if(item.quantity > req.quantity){
                            item.quantity -= req.quantity;
                            //Trade_log.push_back(Trade{price,req.quantity,id_gen,item.id});
                            req.quantity = 0;
                            
                            break;
                         }
                         else{
                            req.quantity -= item.quantity;
                           // Trade_log.push_back(Trade{price,item.quantity,id_gen,item.id});
                            item.cancelled = true;
                            ask_actv_count[idx]--;
                         }
                      }
                      
                 }
                 else{
                    break;
                 }
                
              }
        }

        else if(req.side == Side::Ask){
             for(int idx = best_bid_index;idx >= 0;--idx){
                  double price = conv_idx_to_price(idx);
                 if(bid_actv_count[idx] == 0){
                    continue;;
                 }
                 if(price >= req.price ){
                      for(auto &item : bid[idx]){
                         if(item.cancelled){   //move to next order if item cancelled
                            continue;
                         }
                         if(req.quantity == 0){
                            break;
                         }
                         if(item.quantity > req.quantity){
                            item.quantity -= req.quantity;

                          //  Trade_log.push_back(Trade{price,req.quantity,id_gen,item.id});
                            req.quantity = 0;
                            
                            break;
                         }
                         else{
                            req.quantity -= item.quantity;
                            int cancel_id = item.id;

                           // Trade_log.push_back(Trade{price,item.quantity,id_gen,item.id});
                            item.cancelled = true;
                            bid_actv_count[idx]--;
                         }
                      }
                      
                 }
                 else {
                    break;
                 }

                 
                
              }
        }


        if(ask_actv_count[best_ask_indx] == 0){
            for(int idx = best_ask_indx;idx < ask.size();++idx){
                if(ask_actv_count[idx] != 0){
                    best_ask_indx = idx;
                    break;
                }
            }
        }

        if(bid_actv_count[best_bid_index] == 0){
            for(int idx = best_bid_index;idx >= 0;--idx){
                if(bid_actv_count[idx] != 0){
                    best_bid_index = idx;
                    break;
                }
            }
        }

        if(req.quantity > 0){
            return NewOrderRequest(req);
        }
        return nullopt;

        
    
}
int process_order(Request request) {
    int id = visit([](auto&& req) {
        using T = decay_t<decltype(req)>;
        if constexpr (is_same_v<T, NewOrderRequest>) {
           id_gen++;
           auto matched_ptr = matching_loop(req);
           if(matched_ptr){
            return add_order(*matched_ptr);
           } 
           return 0;
        } else if constexpr (is_same_v<T, CancelOrderRequest>) {
            if(Order_info.find(req.id) == Order_info.end()) return -1;
            return cancel_order(req);
        }else if constexpr (is_same_v<T, MarketOrderRequest>) {
           id_gen++;
           NewOrderRequest Sentinel_req;
           if(req.side == Side::Bid){
                  Sentinel_req = {999999999,req.quantity,req.side};
           }else if(req.side == Side::Ask){
               Sentinel_req = {0,req.quantity,req.side};
           }
           auto matched_ptr = matching_loop(Sentinel_req);
           if(matched_ptr){
           return matched_ptr -> quantity;
           }
           return 0;
           
        }
    }, request);
    return id;
}


void benchmark(int n) {
    mt19937 rng(42);
    uniform_int_distribution<int> price_dist(95, 105);
    uniform_int_distribution<int> qty_dist(1, 100);
    uniform_int_distribution<int> type_dist(1, 10);
    
    vector<int> active_ids;
    
    auto start = chrono::high_resolution_clock::now();
    
    for (int i = 0; i < n; i++) {
        int type = type_dist(rng);
        Side side = (rng() % 2) ? Side::Bid : Side::Ask;
        
        if (type <= 7 || active_ids.empty()) {
            int id = process_order(NewOrderRequest{
                (double)price_dist(rng),
                qty_dist(rng),
                side
            });
            if (id > 0) active_ids.push_back(id);
        } else if (type <= 9) {
            process_order(MarketOrderRequest{qty_dist(rng), side});
        } else {
            uniform_int_distribution<int> id_dist(0, active_ids.size() - 1);
            int idx = id_dist(rng);
            int cancel_id = active_ids[idx];
            active_ids.erase(active_ids.begin() + idx);
            process_order(CancelOrderRequest{cancel_id});
        }
    }
    
    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
    
    cout << "Orders: " << n << endl;
    cout << "Total time: " << duration.count() << " microseconds" << endl;
    cout << "Avg latency: " << (double)duration.count() / n << " microseconds/order" << endl;
    cout << "Throughput: " << (long long)n * 1000000 / duration.count() << " orders/sec" << endl;
}



int main(){

   benchmark(100000);
   cout<<sizeof(Order)<<endl;
}
