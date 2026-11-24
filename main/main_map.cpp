#include <bits/stdc++.h>
#include <x86intrin.h>
#include <random>
#include <variant>

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

struct Order
{
    int id;
    double price;
    int quantity;
    Side side;
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


int id_gen = 0;
map<int,Order> Order_info;
map<double,list<Order*>> ask;
map<double,list<Order*>,greater<double>> bid;
unordered_map<int,list<Order*> :: iterator> Order_iterator ;

vector<Trade> Trade_log;


using Request = variant<NewOrderRequest, CancelOrderRequest,MarketOrderRequest>;

double Min_price = 0;
double max_price = 105;
double tick_size = 0.01;


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
     Order new_order = {id_gen,req.price,req.quantity,req.side};
     Order_info[new_order.id] = new_order;
    
     if(new_order.side == Side::Bid){
            bid[new_order.price].push_back(&Order_info[new_order.id]);
            Order_iterator[new_order.id] = --bid[new_order.price].end();
     }
     else if(new_order.side == Side::Ask){
            ask[new_order.price].push_back(&Order_info[new_order.id]);
            Order_iterator[new_order.id] = --ask[new_order.price].end();
     }
     return new_order.id;
     
}
int cancel_order(CancelOrderRequest req){
    Order to_cancel = Order_info[req.id];
    if(to_cancel.side == Side::Bid){
        bid[to_cancel.price].erase(Order_iterator[to_cancel.id]);
        if(bid[to_cancel.price].empty()){
            bid.erase(to_cancel.price);
        }
    }
    else if(to_cancel.side == Side::Ask){
        ask[to_cancel.price].erase(Order_iterator[to_cancel.id]);
        if(ask[to_cancel.price].empty()){
            ask.erase(to_cancel.price);
        }
    }
    Order_info.erase(to_cancel.id);
    Order_iterator.erase(to_cancel.id);
    return -1;
    
}



optional<NewOrderRequest> matching_loop(NewOrderRequest req){
        if(req.side == Side::Bid){
              for(auto item = ask.begin();item != ask.end();){
                 auto best_ask = item -> first;

                 if(best_ask <= req.price){
                      for(auto it =  (item -> second).begin();it != (item -> second).end();){
                         auto ptr = *it;

                         if(req.quantity == 0){
                            break;
                         }
                         if(ptr -> quantity > req.quantity){
                            ptr -> quantity -= req.quantity;
                            Trade_log.push_back(Trade{best_ask,req.quantity,id_gen,ptr -> id});
                            req.quantity = 0;
                            
                            break;
                         }
                         else{
                            req.quantity -= ptr -> quantity;
                            int cancel_id = ptr -> id;
                            Trade_log.push_back(Trade{best_ask,ptr->quantity,id_gen,ptr -> id});
                            it = (item -> second).erase(it);  
                            Order_info.erase(cancel_id);
                            Order_iterator.erase(cancel_id);
                         }
                      }
                      if((item -> second).empty()){
                        item = ask.erase(item);
                      }else{
                        item++;
                      }
                 }
                 else{
                    break;
                 }
                 
                
              }
        }

        else if(req.side == Side::Ask){
              for(auto item = bid.begin();item != bid.end();){
                 auto best_bid = item -> first;

                 if(best_bid >= req.price){
                      for(auto it =  (item -> second).begin();it != (item -> second).end();){
                         auto ptr = *it;

                         if(req.quantity == 0){
                            break;
                         }
                         if(ptr -> quantity > req.quantity){
                            ptr -> quantity -= req.quantity;
                            Trade_log.push_back(Trade{best_bid,req.quantity,id_gen,ptr -> id});
                            req.quantity = 0;
                            break;
                         }
                         else{
                            req.quantity -= ptr -> quantity;
                            int cancel_id = ptr -> id;
                            Trade_log.push_back(Trade{best_bid,ptr->quantity,id_gen,ptr -> id});
                            it = (item -> second).erase(it);  
                            Order_info.erase(cancel_id);
                            Order_iterator.erase(cancel_id);
                         }
                      }
                      if((item -> second).empty()){
                        item = bid.erase(item);
                      }else{
                        item++;
                      }
                 }
                 else{
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
                  Sentinel_req = {max_price + tick_size,req.quantity,req.side};
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


mt19937 rng(42);
vector<int> active_ids;
int gen_next_id = 0;

Request random_request() {
    uniform_real_distribution<double> price_dist(95.0, 105.0);
    uniform_int_distribution<int> qty_dist(1, 100);
    uniform_int_distribution<int> type_dist(1, 10);

    int type = type_dist(rng);
    Side side = (rng() % 2) ? Side::Bid : Side::Ask;

    if (type <= 6 || (type >= 8 && active_ids.empty())) {
        gen_next_id++;
        active_ids.push_back(gen_next_id);
        return NewOrderRequest{round(price_dist(rng) / tick_size) * tick_size, qty_dist(rng), side};
    }
    if (type == 7) {
        gen_next_id++;
        return MarketOrderRequest{qty_dist(rng), side};
    }
    uniform_int_distribution<size_t> pick(0, active_ids.size() - 1);
    size_t i = pick(rng);
    int id = active_ids[i];
    active_ids[i] = active_ids.back();
    active_ids.pop_back();
    return CancelOrderRequest{id};
}


vector<Request> reqs;
vector<unsigned long long> push_tsc;
vector<unsigned long long> engine_cycles;
vector<unsigned long long> e2e_cycles;

void consumer_func(SPSC_queue &spsc_q,int N){
      int count = 0;
      while(count != N){
          auto match_req = spsc_q.pop();
          if(match_req){
              unsigned long long c0 = __rdtsc();
              process_order(*match_req);
              unsigned long long c1 = __rdtsc();
              engine_cycles[count] = c1 - c0;
              e2e_cycles[count] = c1 - push_tsc[count];
              count++;
          }
      }
}

void producer_func(SPSC_queue &spsc_q,int N){
     for(int count = 0; count < N; ++count){
          do {
              push_tsc[count] = __rdtsc();
          } while(!spsc_q.push(reqs[count]));
     }
}


int main(){
    int n = 100000;

    reqs.reserve(n);
    for (int i = 0; i < n; ++i) {
        reqs.push_back(random_request());
    }
    push_tsc.resize(n);
    engine_cycles.resize(n);
    e2e_cycles.resize(n);
    Trade_log.reserve(4 * n);

    // benchmark
    SPSC_queue spsc_q{.head = 0,.tail = 0};
    auto start = chrono::steady_clock::now();
    unsigned long long tsc_start = __rdtsc();

    thread consumer_t(consumer_func,ref(spsc_q),n);
    thread producer_t(producer_func,ref(spsc_q),n);
    producer_t.join();
    consumer_t.join();

    unsigned long long tsc_end = __rdtsc();
    auto end = chrono::steady_clock::now();

    auto total_ns = chrono::duration_cast<chrono::nanoseconds>(end - start).count();
    double ns_per_cycle = (double)total_ns / (double)(tsc_end - tsc_start);

    unsigned long long first_push = push_tsc[0];
    unsigned long long last_done = push_tsc[n - 1] + e2e_cycles[n - 1];
    double run_ns = (last_done - first_push) * ns_per_cycle;

    sort(engine_cycles.begin(), engine_cycles.end());
    sort(e2e_cycles.begin(), e2e_cycles.end());
    auto pct = [&](const vector<unsigned long long>& v, double p) {
        return v[(size_t)(p * (n - 1))] * ns_per_cycle;
    };

    cout << fixed << setprecision(1);
    cout << "Orders: " << n << '\n';
    cout << "Pipeline time: " << run_ns / 1000.0 << " microseconds\n";
    cout << "Throughput: " << (long long)(n * 1e9 / run_ns) << " orders/sec\n";
    cout << "\nEngine latency (process_order only):\n";
    cout << "  p50: " << pct(engine_cycles, 0.50) << " ns   p95: " << pct(engine_cycles, 0.95)
         << " ns   p99: " << pct(engine_cycles, 0.99) << " ns   p99.9: " << pct(engine_cycles, 0.999)
         << " ns   max: " << pct(engine_cycles, 1.0) << " ns\n";
    cout << "End-to-end latency (push into queue -> processed):\n";
    cout << "  p50: " << pct(e2e_cycles, 0.50) << " ns   p95: " << pct(e2e_cycles, 0.95)
         << " ns   p99: " << pct(e2e_cycles, 0.99) << " ns   p99.9: " << pct(e2e_cycles, 0.999)
         << " ns   max: " << pct(e2e_cycles, 1.0) << " ns\n\n";

    long long total_qty = 0;
    for (const Trade& t : Trade_log) {
        total_qty += t.quantity;
    }
    cout << "Trades: " << Trade_log.size() << '\n';
    cout << "Total qty: " << total_qty << '\n';

    ofstream trade_file("map_trades.txt");
    trade_file << fixed << setprecision(2);
    for (const Trade& t : Trade_log) {
        trade_file << t.aggressor_id << ' ' << t.rest_id << ' ' << t.price << ' ' << t.quantity << '\n';
    }
}
