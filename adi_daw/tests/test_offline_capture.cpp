// SPDX-License-Identifier: GPL-3.0-or-later
#include "adi/engine/graph.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

namespace {
bool counting=false;
int allocations=0;
}
void* operator new(std::size_t bytes) {
    if(counting) ++allocations;
    if(void* p=std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}

namespace {
using namespace adi::engine;
int checks=0,failures=0;
void check(bool good,const char* name) {++checks;if(!good){++failures;std::printf("FAIL: %s\n",name);}}
struct Source final : Node {
    bool silent=false;
    int latency=0;
    bool processesDouble() const noexcept override {return true;}
    std::int32_t latencySamples() const noexcept override {return latency;}
    void process(const NodeIo& io) noexcept override {
        for(int c=0;c<io.channels;++c) for(int i=0;i<io.frames;++i) {
            const auto at=io.blockOffset+i;
            const double value=silent ? 0.0 : 1.0+c/16.0+at/4096.0+0x1p-40;
            if(io.wide()) io.out64[c][at]=value;
            else io.out[c][at]=static_cast<float>(value);
        }
    }
};
struct Buffer {
    std::vector<float> f=std::vector<float>(65*129,-7.0f);
    std::vector<double> d=std::vector<double>(65*129,-7.0);
    std::vector<float*> fp=std::vector<float*>(65);
    std::vector<double*> dp=std::vector<double*>(65);
    Buffer() {for(std::size_t c=0;c<65;++c){fp[c]=f.data()+c*129;dp[c]=d.data()+c*129;}}
    AudioIo io(int frames) {AudioIo result;result.out=fp.data();result.out64=dp.data();result.numOut=65;result.frames=frames;return result;}
};
void capture(Precision width) {
    Source source,other; other.latency=43;
    SumNode stem,master;
    Graph graph;graph.setChannels(64);graph.setPrecision(width);
    const auto s=graph.addNode(source),p=graph.addNode(stem),o=graph.addNode(other),m=graph.addNode(master);
    graph.connect(s,p);graph.connect(p,m);graph.connect(o,m);graph.setOutput(m);graph.prepare(48000,128);
    check(graph.ok() && graph.outputLatencyOf(p)==0 && graph.outputLatencyOf(m)==43,"endpoint latency is independent of master compensation");
    Buffer output,copied,repeated;
    check(!graph.copyNodeOutput(p,copied.io(128)) && copied.d[0]==0,"no stale capture before the first block");
    Event split{};split.frame=70;graph.pushInputEvent(s,split);
    allocations=0;counting=true;
    graph.process(output.io(128));
    const bool first=graph.copyNodeOutput(p,copied.io(128));
    const bool second=graph.copyNodeOutput(m,repeated.io(128));
    counting=false;
    check(first && second && allocations==0,"processing and full-width captures allocate nothing");
    check(graph.stats().segments>1,"capture covers a genuinely segmented block");
    bool exact=true;
    for(int c=0;c<64;++c) for(int i=0;i<128;++i) {
        const double expected=1.0+c/16.0+i/4096.0+0x1p-40;
        const double wide=width==Precision::Float64 ? expected : static_cast<float>(expected);
        exact=exact && copied.dp[static_cast<std::size_t>(c)][i]==wide &&
              copied.fp[static_cast<std::size_t>(c)][i]==static_cast<float>(expected);
    }
    check(exact,"all 64 channels preserve the selected node, including sub-float bits and segment offsets");
    check(copied.dp[64][127]==0 && copied.fp[64][127]==0,"destination channels outside the graph are cleared");
    check(output.d==repeated.d && output.f==repeated.f,"capturing a stem leaves the master block exactly unchanged");
    const auto blocks=graph.stats().blocks;
    (void)graph.copyNodeOutput(p,copied.io(128));
    check(graph.stats().blocks==blocks && graph.outputLatencyOf(m)==43,"capture performs no extra render or PDC changes");
    source.silent=true;
    graph.process(output.io(128));
    const auto slept=graph.stats().nodesSuspended;
    check(graph.copyNodeOutput(p,copied.io(128)) && copied.dp[63][127]==0 && slept>0,
          "a sleeping stem captures silence after loud audio");
    source.silent=false;
    graph.process(output.io(31));
    check(graph.copyNodeOutput(p,copied.io(31)),"a short final callback is a complete capture");
    check(!graph.copyNodeOutput(p,copied.io(128)) && copied.dp[63][127]==0,
          "reading beyond the last block refuses and clears stale tail samples");
    graph.process(output.io(129));
    check(!graph.copyNodeOutput(p,copied.io(31)),"an invalid process call invalidates the previous capture");
    check(!graph.copyNodeOutput(kInvalidNode,copied.io(128)) && !graph.outputLatencyOf(kInvalidNode),"invalid nodes are refused");
    graph.prepare(48000,128);
    check(!graph.copyNodeOutput(p,copied.io(128)),"prepare invalidates prior capture");
    graph.process(output.io(128));graph.release();
    check(!graph.copyNodeOutput(p,copied.io(128)),"release invalidates prior capture");
}
void sidechainCapture() {
    struct Keyed final : Node {
        bool processesDouble() const noexcept override {return true;}
        void process(const NodeIo& io) noexcept override {
            for(int c=0;c<io.channels;++c)for(int i=0;i<io.frames;++i) {
                const int at=io.blockOffset+i;
                io.out64[c][at]=io.sidechain64 ? 0.5*io.sidechain64[c][at] : 0.0;
            }
        }
    } keyed;
    Source key,master;master.silent=true;
    Graph graph;graph.setPrecision(Precision::Float64);
    const auto keyId=graph.addNode(key),stemId=graph.addNode(keyed),masterId=graph.addNode(master);
    graph.connect(keyId,stemId,Bus::Sidechain);graph.setOutput(masterId);graph.prepare(48000,128);
    Buffer output,stem;
    graph.process(output.io(128));
    check(graph.copyNodeOutput(stemId,stem.io(128)) && stem.dp[1][70]==0.5*(1.0+1/16.0+70/4096.0+0x1p-40) &&
          output.dp[0][70]==0.0,"capturing a stem outside the master path retains its unexported live sidechain source");
}
void floatNodeInWideGraph() {
    struct Narrow final : Node {
        void process(const NodeIo& io) noexcept override {
            for(int c=0;c<io.channels;++c)for(int i=0;i<io.frames;++i) {
                const int at=io.blockOffset+i;
                io.out[c][at]=io.in[c][at];
            }
        }
    } narrow;
    Source source;
    Graph graph;graph.setPrecision(Precision::Float64);
    const auto a=graph.addNode(source),b=graph.addNode(narrow);
    graph.connect(a,b);graph.setOutput(b);graph.prepare(48000,128);
    Buffer out,captured;
    graph.process(out.io(128));
    check(graph.copyNodeOutput(b,captured.io(128)) && captured.dp[0][0]==1.0 && captured.dp[1][127]==
          static_cast<double>(static_cast<float>(1.0+1/16.0+127/4096.0+0x1p-40)),
          "a float-only node in a double graph captures its widened processed output");
}
}
int main() {
    counting=true;void* control=::operator new(1);counting=false;::operator delete(control);
    check(allocations>0,"allocation counter positive control");
    capture(Precision::Float32);capture(Precision::Float64);
    sidechainCapture();
    floatNodeInWideGraph();
    std::printf("%s -- %d checks, %d failure(s)\n",failures ? "FAIL" : "PASS",checks,failures);
    return failures ? 1 : 0;
}
