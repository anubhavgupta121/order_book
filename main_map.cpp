#include <bits/stdc++.h>
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


int main(){
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
