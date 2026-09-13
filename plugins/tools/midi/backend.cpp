#include "backend.hpp"
#include <algorithm>
#include <memory>
namespace srz80::midi {
Backend::Backend(Factory factory) : factory_(std::move(factory)), worker_([this]{run();}) { refresh(); }
Backend::~Backend() {
    { std::lock_guard lock(mutex_); stopping_=true; }
    wake_.notify_one(); worker_.join();
}
void Backend::post(Command command) {
    std::lock_guard lock(mutex_);
    if(commands_.size()>=256) { view_.error="MIDI backend command queue full"; return; }
    commands_.push_back(std::move(command)); wake_.notify_one();
}
void Backend::refresh() { post({Command::refresh,{},{}}); }
void Backend::connect(bool input, std::string name) { post({input?Command::input:Command::output,std::move(name),{}}); }
void Backend::disconnect() {
    std::lock_guard lock(mutex_);
    ++route_epoch_; accept_input_=false; view_.input=view_.output=false;
    commands_.clear(); queued_bytes_=0;
    received_.clear(); received_bytes_=0;
    commands_.push_back({Command::close,{},{}});
    wake_.notify_one();
}
bool Backend::send(Bytes bytes) {
    std::lock_guard lock(mutex_);
    if(!view_.output) return false;
    if(commands_.size()>=256 || bytes.size()>262144-queued_bytes_) {
        view_.error="MIDI output queue overflow"; return false;
    }
    queued_bytes_+=bytes.size(); commands_.push_back({Command::send,{},std::move(bytes)});
    wake_.notify_one(); return true;
}
void Backend::incoming(const Bytes &bytes) {
    std::lock_guard lock(mutex_);
    if(stopping_ || !accept_input_) return;
    if(received_.size()>=256 || bytes.size()>262144-received_bytes_) {
        view_.error="MIDI input queue overflow"; return;
    }
    received_bytes_+=bytes.size(); received_.push_back(bytes);
}
std::vector<Bytes> Backend::receive() {
    std::lock_guard lock(mutex_);
    std::vector<Bytes> result;
    size_t bytes=0;
    while(!received_.empty() && result.size()<32 && bytes<65536) {
        bytes+=received_.front().size(); result.push_back(std::move(received_.front())); received_.pop_front();
    }
    received_bytes_-=bytes; return result;
}
Backend::View Backend::view() { std::lock_guard lock(mutex_); return view_; }
void Backend::run() {
    std::unique_ptr<Driver> driver;
    std::string input_name, output_name;
    while(true) {
        Command command;
        uint64_t route_epoch=0;
        {
            std::unique_lock lock(mutex_);
            if(input_name.empty() && output_name.empty())
                wake_.wait(lock,[&]{return stopping_ || !commands_.empty();});
            else if(!wake_.wait_for(lock,std::chrono::seconds(1),[&]{return stopping_ || !commands_.empty();}))
                commands_.push_back({Command::refresh,{},{}});
            if(stopping_) break;
            command=std::move(commands_.front()); commands_.pop_front(); queued_bytes_-=command.bytes.size();
            route_epoch=route_epoch_;
        }
        try {
            if(!driver) driver=factory_([this](const Bytes &bytes){incoming(bytes);});
            if(command.kind==Command::refresh) {
                auto in=driver->enumerate(true), out=driver->enumerate(false);
                if((!input_name.empty() && std::count(in.begin(),in.end(),input_name)!=1) ||
                   (!output_name.empty() && std::count(out.begin(),out.end(),output_name)!=1))
                    throw std::runtime_error("MIDI device disappeared; reconnect explicitly");
                std::lock_guard lock(mutex_); view_.inputs=std::move(in); view_.outputs=std::move(out);
            } else if(command.kind==Command::close) {
                driver->close(); input_name.clear(); output_name.clear();
                std::lock_guard lock(mutex_); view_.input=view_.output=false;
                received_.clear(); received_bytes_=0;
                std::erase_if(commands_,[](const auto &c){return c.kind==Command::send;}); queued_bytes_=0;
            } else if(command.kind==Command::send) {
                driver->send(command.bytes);
            } else {
                const bool input=command.kind==Command::input;
                const auto ports=driver->enumerate(input);
                if(std::count(ports.begin(),ports.end(),command.name)!=1)
                    throw std::runtime_error("MIDI device missing or name ambiguous; select a port again");
                const auto index=std::find(ports.begin(),ports.end(),command.name)-ports.begin();
                if(input) { std::lock_guard lock(mutex_); if(route_epoch!=route_epoch_) continue; accept_input_=true; }
                driver->open(input,static_cast<unsigned>(index));
                (input?input_name:output_name)=command.name;
                std::lock_guard lock(mutex_);
                if(route_epoch!=route_epoch_) continue;
                if(input) view_.input=true; else view_.output=true;
                view_.error.clear();
            }
        } catch(const std::exception &e) {
            if(driver) { try { driver->close(); } catch (...) {} }
            input_name.clear(); output_name.clear();
            std::lock_guard lock(mutex_); view_.error=e.what(); view_.input=view_.output=false; accept_input_=false;
        }
    }
    // Driver destruction stops callback delivery before the owned queues die.
    driver.reset();
}
}
