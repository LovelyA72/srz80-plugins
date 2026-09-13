#include "backend.hpp"
#include <RtMidi.h>
namespace srz80::midi {
namespace {
class RtMidiDriver final : public Backend::Driver {
    Backend::Incoming incoming_;
    RtMidiIn input_;
    RtMidiOut output_;
public:
    explicit RtMidiDriver(Backend::Incoming incoming) : incoming_(std::move(incoming)) {
        input_.ignoreTypes(false,false,false);
        input_.setCallback([](double,std::vector<unsigned char> *bytes,void *context) {
            try { static_cast<RtMidiDriver *>(context)->incoming_(*bytes); } catch (...) {}
        },this);
    }
    ~RtMidiDriver() override { input_.cancelCallback(); input_.closePort(); output_.closePort(); }
    std::vector<std::string> enumerate(bool input) override {
        RtMidi &port=input?static_cast<RtMidi &>(input_):static_cast<RtMidi &>(output_);
        std::vector<std::string> names;
        for(unsigned i=0;i<port.getPortCount();++i) names.push_back(port.getPortName(i));
        return names;
    }
    void open(bool input,unsigned index) override {
        if(input) { input_.closePort(); input_.openPort(index,"SRZ80 input"); }
        else { output_.closePort(); output_.openPort(index,"SRZ80 output"); }
    }
    void close() override { input_.closePort(); output_.closePort(); }
    void send(const Bytes &bytes) override { if(output_.isPortOpen()) output_.sendMessage(&bytes); }
};
}
Backend::Backend() : Backend([](Incoming incoming){return std::make_unique<RtMidiDriver>(std::move(incoming));}) {}
}
