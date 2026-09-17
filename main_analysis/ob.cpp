#include <bits/stdc++.h>
#include <random>
#include <variant>
#include <thread>
#include <fstream>
#include <sstream>
 
 
 
using namespace std;
 
 
 
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
double max_price = 105;
double tick_size = 0.01;
 
 
int conv_price_to_idx(double price){
     // FIX: round instead of truncate to avoid float precision errors
     // mapping a price to the wrong tick (e.g. 100.01/0.01 -> 10000.9999 -> 10000)
     return (int)round((price - Min_price)/tick_size);
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
vector<int> activ_bid_idx;
vector<int> activ_ask_idx;
unordered_map<int,pair<double,int>> Order_index;
 
vector<Trade> Trade_log;
 
 
using Request = variant<NewOrderRequest, CancelOrderRequest,MarketOrderRequest>;
int best_ask_indx = 0;
int best_bid_index = bid.size() - 1;
struct SPSC_queue{
    array<Request,1000> arr = {};
    alignas(64) atomic<size_t> head;
    alignas(64) atomic<size_t> tail;
 
 
    bool push(Request req){
        size_t h = head.load(memory_order_acquire);
        size_t t = tail.load(memory_order_acquire);
        if(t - h == 1000){
            return false;
        }else{
            arr[t%1000] = req;
            tail.store(t+1, memory_order_release);
            return true;
        }
    }
    optional<Request> pop(){
        size_t t = tail.load(memory_order_acquire);
        size_t h = head.load(memory_order_acquire);
        if(t - h == 0){
            return nullopt;
        }else{
 
            Request new_req = arr[head%1000];
            head.store(h + 1,memory_order_release);
            return new_req;
        }
    
    }
};
 
 
int add_order(NewOrderRequest req){
     Order new_order = {id_gen,req.quantity,req.side,false};
     Order_info[new_order.id] = new_order;
    
     if(new_order.side == Side::Bid){
            int idx = conv_price_to_idx(req.price);
            bid[idx].push_back(new_order);
            Order_index[new_order.id] =  {req.price,bid[idx].size() - 1};
            bid_actv_count[idx]++;
            if(bid_actv_count[idx] == 1){
            auto ins = lower_bound(activ_bid_idx.begin(),activ_bid_idx.end(),idx);
            activ_bid_idx.insert(ins,idx);
            }
     }
     else if(new_order.side == Side::Ask){
            int idx = conv_price_to_idx(req.price);
            ask[idx].push_back(new_order);
            Order_index[new_order.id] =  {req.price,ask[idx].size() - 1};
            ask_actv_count[idx]++;
            if(ask_actv_count[idx] == 1){
            auto ins = lower_bound(activ_ask_idx.begin(),activ_ask_idx.end(),idx);
            activ_ask_idx.insert(ins,idx);
            }
            
     }
     return new_order.id;
     
}
int cancel_order(CancelOrderRequest req){
    
    Order to_cancel = Order_info[req.id];
    if(to_cancel.side == Side::Bid){
        auto [price,idx] =  Order_index[req.id];
        int price_index = conv_price_to_idx(price);
        bid[price_index][idx].cancelled = true;
        bid_actv_count[price_index]--;
        if(bid_actv_count[price_index] == 0){
        auto ins = lower_bound(activ_bid_idx.begin(),activ_bid_idx.end(),price_index);
        activ_bid_idx.erase(ins);
        }
        
    }
    else if(to_cancel.side == Side::Ask){
        auto [price,idx] =  Order_index[req.id];
        int price_index = conv_price_to_idx(price);
        ask[price_index][idx].cancelled = true;
        ask_actv_count[price_index]--;
        if(ask_actv_count[price_index] == 0){
        auto ins = lower_bound(activ_ask_idx.begin(),activ_ask_idx.end(),price_index);
        activ_ask_idx.erase(ins);
        }
    }
    Order_info[req.id].cancelled = true;
    return - 1;
    
}
 
 
 
optional<NewOrderRequest> matching_loop(NewOrderRequest req){
        if(req.side == Side::Bid){
              for(auto it = activ_ask_idx.begin();it != activ_ask_idx.end();){
                 int idx = (*it);
                 double price = conv_idx_to_price(idx) ;
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
                            Trade_log.push_back(Trade{price,req.quantity,id_gen,item.id});
                            req.quantity = 0;
                            
                            break;
                         }
                         else{
                            req.quantity -= item.quantity;
                            Trade_log.push_back(Trade{price,item.quantity,id_gen,item.id});
                            item.cancelled = true;
                            ask_actv_count[idx]--;
                         }
                      }
                      if(ask_actv_count[idx] == 0){
                          it = activ_ask_idx.erase(it);
                      }else{
                          it++;
                      }
                      
                 }
                 else{
                    break;
                 }
                
              }
        }
 
        else if(req.side == Side::Ask){
              for(auto it = activ_bid_idx.rbegin();it != activ_bid_idx.rend();){
                  int idx = (*it);
                  double price = conv_idx_to_price(idx);
                 if(price >= req.price ){
                      for(auto &item : bid[idx]){
                         if(item.cancelled){   
                            continue;
                         }
                         if(req.quantity == 0){
                            break;
                         }
                         if(item.quantity > req.quantity){
                            item.quantity -= req.quantity;
 
                            Trade_log.push_back(Trade{price,req.quantity,id_gen,item.id});
                            req.quantity = 0;
                            
                            break;
                         }
                         else{
                            req.quantity -= item.quantity;
                            int cancel_id = item.id;
 
                           Trade_log.push_back(Trade{price,item.quantity,id_gen,item.id});
                            item.cancelled = true;
                            bid_actv_count[idx]--;
                         }
                      }
                      if(bid_actv_count[idx] == 0){
                          it = decltype(it)(activ_bid_idx.erase(std::next(it).base()));
                      }else{
                          it++;
                      }
                      
                 }
                 else {
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
 
mt19937 rng;
 Request random_request() {
    
    uniform_real_distribution<double> price_dist(95.0,105.0);
    uniform_int_distribution<int> qty_dist(1, 100);
    uniform_int_distribution<int> type_dist(1, 10);
    
    vector<int> active_ids;
    
    
    
        int type = type_dist(rng);
        Side side = (rng() % 2) ? Side::Bid : Side::Ask;
        
        if (type <= 8 || active_ids.empty()) {
            return NewOrderRequest{
                price_dist(rng),
                qty_dist(rng),
                side
            };
        } else if (type <= 10) {
            return MarketOrderRequest{qty_dist(rng), side};
        } 
        return NewOrderRequest{0,0,Side::Bid};
    }
    
 
 
 
 
 
 
 
void consumer_func(SPSC_queue &spsc_q,int N){
      int count = 0;
      while(count != N){
          auto match_req = spsc_q.pop();
          if(match_req){
              process_order(*match_req);
              count++;
          }
      }
}
 
void producer_func(SPSC_queue &spsc_q,int N){
     int count = 0;
     
     while(count != N){
          Request req = random_request();
          while(!spsc_q.push(req));
          count++;
     }
}
 
 
// ---------------------------------------------------------------------
// Research replay mode: read orders.csv, feed sequentially through the
// real matching engine, and log fills + book snapshots for analysis.
// This bypasses the SPSC queue/threading on purpose - we want deterministic
// order and real timestamps from the file, not throughput measurement.
// ---------------------------------------------------------------------
 
struct TimedFill {
    double timestamp;
    int order_id;
    int counterparty_id;
    string side;
    double price;
    int quantity;
    int is_maker;
};
 
vector<TimedFill> Timed_fills;
 
string best_bid_str(){
    if(activ_bid_idx.empty()) return "";
    return to_string(conv_idx_to_price(activ_bid_idx.back()));
}
string best_ask_str(){
    if(activ_ask_idx.empty()) return "";
    return to_string(conv_idx_to_price(activ_ask_idx.front()));
}
 
void run_from_csv(const string& orders_path, const string& fills_path, const string& snapshots_path){
    ifstream in(orders_path);
    if(!in){
        cerr << "Could not open " << orders_path << endl;
        return;
    }
    ofstream snap_out(snapshots_path);
    snap_out << "timestamp,best_bid,best_ask\n";
 
    string line;
    getline(in, line); // skip header
 
    int n_processed = 0;
    while(getline(in, line)){
        stringstream ss(line);
        string order_id_s, ts_s, side_s, type_s, price_s, qty_s;
        getline(ss, order_id_s, ',');
        getline(ss, ts_s, ',');
        getline(ss, side_s, ',');
        getline(ss, type_s, ',');
        getline(ss, price_s, ',');
        getline(ss, qty_s, ',');
 
        double ts = stod(ts_s);
        Side side = (side_s == "BUY") ? Side::Bid : Side::Ask;
        int qty = stoi(qty_s);
 
        // snapshot BEFORE processing this order (pre-trade book state)
        snap_out << ts << "," << best_bid_str() << "," << best_ask_str() << "\n";
 
        size_t trades_before = Trade_log.size();
        int this_order_id;
 
        if(type_s == "MARKET"){
            id_gen++;
            this_order_id = id_gen;
            NewOrderRequest sentinel;
            if(side == Side::Bid) sentinel = {999999999.0, qty, side};
            else sentinel = {0.0, qty, side};
            matching_loop(sentinel); // leftover qty discarded - market orders don't rest
        } else {
            double price = stod(price_s);
            // clamp defensively to the engine's valid price range to avoid OOB access
            price = max(Min_price, min(max_price, price));
            id_gen++;
            this_order_id = id_gen;
            auto matched = matching_loop(NewOrderRequest{price, qty, side});
            if(matched) add_order(*matched);
        }
 
        // any Trade_log entries appended during this call belong to this order
        for(size_t i = trades_before; i < Trade_log.size(); i++){
            const auto& t = Trade_log[i];
            Timed_fills.push_back({ts, this_order_id, t.rest_id, side_s, t.price, t.quantity, 0});
        }
        n_processed++;
    }
 
    ofstream fout(fills_path);
    fout << "fill_id,order_id,timestamp,side,price,quantity,is_maker\n";
    for(size_t i = 0; i < Timed_fills.size(); i++){
        const auto& f = Timed_fills[i];
        fout << (i+1) << "," << f.order_id << "," << f.timestamp << "," << f.side << ","
             << f.price << "," << f.quantity << "," << f.is_maker << "\n";
    }
 
    cout << "Processed " << n_processed << " orders from " << orders_path << endl;
    cout << "Wrote " << Timed_fills.size() << " fills to " << fills_path << endl;
    cout << "Wrote " << n_processed << " snapshots to " << snapshots_path << endl;
}
 
 
int main(int argc, char** argv){
    if(argc >= 2 && string(argv[1]) == "--replay"){
        string orders_path = argc >= 3 ? argv[2] : "orders.csv";
        string fills_path = argc >= 4 ? argv[3] : "fills.csv";
        string snapshots_path = argc >= 5 ? argv[4] : "book_snapshots.csv";
        run_from_csv(orders_path, fills_path, snapshots_path);
        return 0;
    }
 
    // original throughput benchmark (unchanged)
    auto start = chrono::high_resolution_clock::now();
    SPSC_queue spsc_q{.head = 0,.tail = 0};
    int n = 100000;
    thread producer_t(producer_func,ref(spsc_q),n);
    thread consumer_t(consumer_func,ref(spsc_q),n);
    producer_t.join();
    consumer_t.join();
 
 
    auto end = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::microseconds>(end - start);
    
    cout << "Orders: " << n << endl;
    cout << "Total time: " << duration.count() << " microseconds" << endl;
    cout << "Avg latency: " << (double)duration.count() / n << " microseconds/order" << endl;
    cout << "Throughput: " << (long long)n * 1000000 / duration.count() << " orders/sec" << endl;
    
}
 


