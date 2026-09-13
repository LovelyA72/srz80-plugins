#pragma once
#include "protocol.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
namespace srz80::midi {
// OS objects and potentially blocking driver calls belong exclusively to this
// tool-owned worker. GUI calls only exchange bounded, owned queue entries.
class Backend {
public:
    struct View { std::vector<std::string> inputs, outputs; std::string error; bool input=false, output=false; };
    struct Driver {
        virtual ~Driver() = default;
        virtual std::vector<std::string> enumerate(bool input) = 0;
        virtual void open(bool input, unsigned index) = 0;
        virtual void close() = 0;
        virtual void send(const Bytes &) = 0;
    };
    using Incoming = std::function<void(const Bytes &)>;
    using Factory = std::function<std::unique_ptr<Driver>(Incoming)>;
    Backend();
    explicit Backend(Factory factory);
    ~Backend();
    Backend(const Backend &) = delete;
    void refresh();
    void connect(bool input, std::string exact_name);
    void disconnect();
    bool send(Bytes bytes);
    std::vector<Bytes> receive();
    View view();
private:
    struct Command { enum Kind { refresh, input, output, close, send } kind; std::string name; Bytes bytes; };
    void post(Command);
    void run();
    void incoming(const Bytes &);
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Command> commands_;
    std::deque<Bytes> received_;
    size_t queued_bytes_=0, received_bytes_=0;
    View view_;
    bool stopping_=false, accept_input_=false;
    uint64_t route_epoch_=0;
    Factory factory_;
    std::thread worker_;
};
}
