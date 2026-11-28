// SPDX-License-Identifier: AGPL-3.0-or-later
#include "temp_directory.hpp"
#include "adi/sample_slots.hpp"
#include "adi/builtin_catalog.hpp"
#include "adi/audio/io_audit.hpp"
#include "adi/audio/wav_file.hpp"
#include "adi/dsp/oneshot.hpp"
#include "adi/engine/snapshot.hpp"
#include "adi/history.hpp"
#include "adi/store.hpp"
#include "juce/builtin_host.hpp"
#include "juce/builtin_samples.hpp"
#if ADI_WITH_PD
#include "adi/pd_builtins/oneshot_sample.hpp"
#endif
#include <SQLiteCpp/SQLiteCpp.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <cstdlib>
#include <new>
namespace {thread_local bool countAllocations=false;int callbackAllocations=0;}
#if defined(__GNUC__) && !defined(__clang__)
#define ADI_TEST_NOINLINE __attribute__((noinline))
#else
#define ADI_TEST_NOINLINE
#endif
ADI_TEST_NOINLINE void* operator new(std::size_t bytes){if(countAllocations)++callbackAllocations;if(void* p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc();}
ADI_TEST_NOINLINE void* operator new[](std::size_t bytes){return operator new(bytes);}
ADI_TEST_NOINLINE void operator delete(void* p)noexcept{std::free(p);}
ADI_TEST_NOINLINE void operator delete[](void* p)noexcept{std::free(p);}
ADI_TEST_NOINLINE void operator delete(void* p,std::size_t)noexcept{std::free(p);}
ADI_TEST_NOINLINE void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
#undef ADI_TEST_NOINLINE
namespace {
using namespace adi;using namespace adi::device;using namespace adi::engine;
int checks=0,failures=0;
void check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
#if ADI_WITH_PD
void writeSample(const std::filesystem::path& path) {
    std::vector<float> silence(256);audio::WavWriter writer(path,48000,1,audio::WavFormat::Float32);writer.write(silence.data(),256);writer.close();
}
std::unique_ptr<DeviceInstance> load() {
    rows::PluginRef ref;ref.format="internal";ref.uid="org.adidaw.builtin.color-cab";
    DeviceRequest request;request.ref=&ref;std::string error;auto result=makeBuiltinDevice(request,error);
    if(!result)throw std::runtime_error(error);
    return result;
}
float render(DeviceInstance& instance) {
    std::array<std::array<float,64>,2> input{},output{};for(auto& plane:input)plane.fill(0.25f);
    float* in[]={input[0].data(),input[1].data()};float* out[]={output[0].data(),output[1].data()};
    NodeIo io;io.in=in;io.out=out;io.channels=2;io.frames=64;
    audio::CallbackScope callback;
    countAllocations=true;
    for(int block=0;block<40;++block)instance.process(io);
    countAllocations=false;
    return output[0][63];
}
void assignmentAndPlayback() {
    test::TempDirectory directory{"sample_slots","project"};StoreError error{};auto store=Store::create(directory.path()/"song.adi",error);
    if(!store)throw std::runtime_error(toString(error));
    auto& db=store->db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'audio','Audio'),(9,'master','Main')");
    const auto insert=prepareBuiltinInsert(*store,"org.adidaw.builtin.color-cab",1);check(commitBuiltinInsert(*store,insert).ok,"sample fixture inserts Color Cab through public browser API");
    const auto file=directory.path()/"sample.wav";writeSample(file);
    auto instance=load();instance->prepare(48000,64);ParamOps glue;check(glue.attach(insert.deviceId,*instance),"existing parameter/state glue attaches sample roles");
    check(std::abs(render(*instance)-0.25f)<1e-6f,"empty slot is identity before assignment");
    const auto oldHead=OpJournal(*store).headSeq();const auto prepared=prepareSampleAssignment(*store,insert.deviceId,1,file,48000,7);
    if(!prepared.ok)std::printf("prepare: %s\n",prepared.error.c_str());
    check(prepared.ok && prepared.blobs.size()==2 && prepared.requests.size()==3 && OpJournal(*store).headSeq()==oldHead &&
        db.execAndGet("SELECT count(*) FROM media_files").getInt()==0 && db.execAndGet("SELECT count(*) FROM state_blobs").getInt()==0,
        "preparation imports/decodes/designs and captures empty baseline with no project writes");
    const auto committed=commitSampleAssignment(*store,prepared);if(!committed.ok)std::printf("commit: %s\n",committed.error.c_str());
    check(committed.ok && History(*store).nextUndo()->opCount==3 && History(*store).nextUndo()->label=="Assign device sample","import and slot state are one undo step");
    for(const auto& request:prepared.requests)if(request.opType=="device.loadState")glue.appliedState(*store,request.payload,0);
    check(std::abs(render(*instance))<1e-6f,"assigned derived silent profile changes real native processing");
    const auto saved=instance->saveState("sample:1");const auto state=Payload::from_cbor(saved);
    check(state.at("mediaHash").get<std::string>().size()==64 && state.at("derived").contains("target") && state.at("source_f32le").is_binary(),
        "state keeps media identity, derived profile and rate-portable source backup");
    auto first=SnapshotBuilder::fromStore(*store);auto shared=SnapshotBuilder::fromStore(*store,*first);
    check(first->sampleSlots.size()==1 && !first->sampleSlots[0]->empty && !first->sampleSlots[0]->missing && first->sampleSlots[0]->hasDerivedState &&
        first->sampleSlots[0]->mediaId==prepared.mediaId && first->sampleSlots[0]->name=="Impulse" && first->sampleSlots[0]==shared->sampleSlots[0],
        "snapshot exposes complete assigned slot and structurally shares unchanged metadata");
    std::filesystem::remove(file);auto missing=SnapshotBuilder::fromStore(*store,*first);
    check(missing->sampleSlots[0]->missing && !first->sampleSlots[0]->missing && missing->sampleSlots[0]!=first->sampleSlots[0],"missing original marks new snapshot without mutating old frames");
    auto restored=load();restored->prepare(44100,64);
    check(restored->loadState("sample:1",saved) && std::abs(render(*restored))<1e-6f,"derived state and rate-portable backup restore sound without the missing original at another engine rate");
    auto malformed=state;malformed["source_f32le"]=Payload::binary({0,0,128,127});
    check(!instance->loadState("sample:1",Payload::to_cbor(malformed)) && instance->saveState("sample:1")==saved,"nonfinite sample backup refuses atomically");
    malformed=state;malformed["slot"]=2;
    check(!instance->loadState("sample:1",Payload::to_cbor(malformed)) && !instance->loadState("sample:2",saved),"slot identity and role must match declarations");
    malformed=state;malformed["future"]={{"kept",true}};
    check(instance->loadState("sample:1",Payload::to_cbor(malformed)) && Payload::from_cbor(instance->saveState("sample:1")).at("future")==malformed.at("future"),"sample state retains future opaque fields");
    const auto logged=OpJournal(*store).recent(3);
    check(History(*store).undo().ok && db.execAndGet("SELECT count(*) FROM plugin_state").getInt()==0 && db.execAndGet("SELECT count(*) FROM media_files").getInt()==0,
        "undo restores original absent media and state rows");
    for(const auto& op:logged)if(op.opType=="device.loadState" && op.inverse)glue.appliedState(*store,*op.inverse,1);
    check(std::abs(render(*instance)-0.25f)<1e-6f,"undo's captured empty baseline clears the live sample as well as SQL");
    check(History(*store).redo().ok,"sample assignment redo restores content-addressed references");
    for(const auto& request:prepared.requests)if(request.opType=="device.loadState")glue.appliedState(*store,request.payload,2);
    check(std::abs(render(*instance))<1e-6f,"redo restores the sample without file access");
    writeSample(file);const auto again=prepareSampleAssignment(*store,insert.deviceId,1,file,48000,8);
    check(again.ok && again.mediaId==prepared.mediaId && again.requests.size()==1,"same media content deduplicates and existing baseline is not repeated");
    auto poolChanged=SnapshotBuilder::fromStore(*store,*first);
    db.exec("UPDATE media_files SET missing=1");auto flagged=SnapshotBuilder::fromStore(*store,*poolChanged);
    check(flagged->sampleSlots[0]->missing && !poolChanged->sampleSlots[0]->missing,"pool missing flag changes sharing even while the locator exists");
    db.exec("UPDATE media_files SET missing=0");
    db.exec("UPDATE plugin_refs SET uid='org.adidaw.builtin.utility'");auto wrongUid=SnapshotBuilder::fromStore(*store,*poolChanged);
    check(!wrongUid->sampleSlots[0]->problem.empty() && wrongUid->sampleSlots[0]!=poolChanged->sampleSlots[0],"cached state metadata still validates changed device UID");
    db.exec("UPDATE plugin_refs SET uid='org.adidaw.builtin.color-cab'");
    const auto clear=prepareSampleClear(*store,insert.deviceId,1);check(clear.ok && commitSampleAssignment(*store,clear).ok,"clear slot is an ordinary undoable state op");
    for(const auto& request:clear.requests)glue.appliedState(*store,request.payload,3);
    check(std::abs(render(*instance)-0.25f)<1e-6f && SnapshotBuilder::fromStore(*store)->sampleSlots[0]->empty,"clear restores identity and snapshot empty state");
    const auto stale=commitSampleAssignment(*store,again);check(!stale.ok && stale.stale,"stale prepared assignment refuses after another edit");
    check(!prepareSampleAssignment(*store,insert.deviceId,2,file,48000,9).ok && !prepareSampleAssignment(*store,999,1,file,48000,9).ok,
        "undeclared slot and missing device refuse before import");
    check(callbackAllocations==0,"assigned, missing-file, cleared and restored sample processing allocate nothing");
    {
        audio::CallbackScope callback;
        const auto denied=prepareSampleClear(*store,insert.deviceId,1);const auto deniedCommit=commitSampleAssignment(*store,clear);
        check(!denied.ok && !deniedCommit.ok,"sample command preparation and commit refuse on callback before SQLite");
    }
    check(audio::callbackFileIo.load(std::memory_order_relaxed)==0,"all live processing and undo/redo perform no callback file IO");
}
// SC-482: the floats OneShot's slot holds now, interleaved.
std::vector<float> slotAudio(Store& store, std::int64_t device) {
    SQLite::Statement row(store.db(), "SELECT state_hash FROM plugin_state WHERE device_id=? AND stream_role='sample:1'");
    row.bind(1, device);
    if (!row.executeStep()) return {};
    const auto blob = store.getStateBlob(row.getColumn(0).getString());
    if (!blob) return {};
    std::vector<std::uint8_t> bytes;
    for (const auto b : *blob) bytes.push_back(static_cast<std::uint8_t>(b));
    const auto state = Payload::from_cbor(bytes);
    const auto& raw = state.at("source_f32le").get_binary();
    std::vector<float> values(raw.size() / 4);
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::uint32_t bits = 0;
        for (unsigned k = 0; k < 4; ++k) bits |= static_cast<std::uint32_t>(raw[i * 4 + k]) << (8 * k);
        values[i] = std::bit_cast<float>(bits);
    }
    return values;
}
double storedParam(Store& store, std::int64_t device, const char* id) {
    SQLite::Statement row(store.db(), "SELECT normalized_value FROM plugin_params WHERE device_id=? AND param_id=?");
    row.bind(1, device);
    row.bind(2, id);
    return row.executeStep() ? row.getColumn(0).getDouble() : -1;
}
// SC-482: Simpler's Reverse and Crop make a changed copy of OneShot's sample, a
// new file beside the original, each one Undo step.
void reverseAndCrop() {
    test::TempDirectory directory{"sample_slots", "processed"};
    StoreError error{};
    auto store = Store::create(directory.path() / "song.adi", error);
    if (!store) throw std::runtime_error(toString(error));
    auto& db = store->db();
    db.exec("INSERT INTO tracks(id,kind,name) VALUES(1,'midi','Keys'),(9,'master','Main')");
    // A stereo ramp, 400 frames: left rises, right falls.
    const auto file = directory.path() / "Ramp.wav";
    {
        std::vector<float> ramp(800);
        for (std::size_t i = 0; i < 400; ++i) {
            ramp[2 * i] = static_cast<float>(i) / 400.f;
            ramp[2 * i + 1] = -static_cast<float>(i) / 400.f;
        }
        audio::WavWriter writer(file, 48000, 2, audio::WavFormat::Float32);
        writer.write(ramp.data(), 400);
        writer.close();
    }
    const auto loaded = prepareSampleInstrument(*store, 1, file, 48000, 7);
    if (!loaded.ok) std::printf("load: %s\n", loaded.error.c_str());
    check(loaded.ok && commitSampleAssignment(*store, loaded).ok, "OneShot loads a stereo ramp");
    const auto device = loaded.deviceId;
    const auto original = slotAudio(*store, device);
    check(original.size() == 800, "its slot holds both channels");
    const auto reversed = prepareSampleReverse(*store, device, 1, 48000, 8);
    if (!reversed.ok) std::printf("reverse: %s\n", reversed.error.c_str());
    check(reversed.ok && commitSampleAssignment(*store, reversed).ok &&
              History(*store).nextUndo()->label == "Reverse Sample",
          "Reverse is one Undo step");
    const auto backward = slotAudio(*store, device);
    bool mirrored = backward.size() == original.size();
    for (std::size_t f = 0; mirrored && f < 400; ++f)
        mirrored = backward[2 * f] == original[2 * (399 - f)] && backward[2 * f + 1] == original[2 * (399 - f) + 1];
    check(mirrored, "Reverse turns the sample around, each channel its own");
    check(std::filesystem::exists(directory.path() / "Samples" / "Processed" / "Reverse" / "Ramp Reverse.wav") &&
              std::filesystem::exists(file) && db.execAndGet("SELECT count(*) FROM media_files").getInt() == 2,
          "the copy is a new file in Samples/Processed/Reverse; the original and its media row stay");
    check(History(*store).undo().ok && slotAudio(*store, device) == original, "Undo brings the original back");
    // Crop: Start 25 % and End 75 % are frames 100 to 300.
    std::vector<OpRequest> region(2);
    for (std::size_t i = 0; i < 2; ++i) {
        region[i].opType = "device.setParam";
        region[i].label = "Region";
        region[i].targetKind = "device";
        region[i].targetId = device;
        region[i].payload = {{"dev", device}, {"param", i == 0 ? "2" : "3"}, {"norm", i == 0 ? .25 : .75},
                             {"real", i == 0 ? 25. : 75.}};
    }
    check(OpJournal(*store).commit(region, CommitOptions{}).ok, "Start 25 % and End 75 %");
    const auto cropped = prepareSampleCrop(*store, device, 1, 48000, 9);
    if (!cropped.ok) std::printf("crop: %s\n", cropped.error.c_str());
    check(cropped.ok && commitSampleAssignment(*store, cropped).ok &&
              History(*store).nextUndo()->label == "Crop Sample",
          "Crop is one Undo step");
    const auto cut = slotAudio(*store, device);
    check(cut.size() == 400 && cut.front() == original[200] && cut[1] == original[201] &&
              cut[398] == original[598] && cut[399] == original[599],
          "Crop keeps the region between Start and End");
    check(storedParam(*store, device, "2") == 0 && storedParam(*store, device, "3") == 1,
          "and moves Start to 0 % and End to 100 %, so the same region plays");
    check(std::filesystem::exists(directory.path() / "Samples" / "Processed" / "Crop" / "Ramp Crop.wav"),
          "the cropped copy is a new file in Samples/Processed/Crop");
    check(History(*store).undo().ok && slotAudio(*store, device) == original &&
              storedParam(*store, device, "2") == .25 && storedParam(*store, device, "3") == .75,
          "Undo brings the whole sample and its region back");
    check(!prepareSampleReverse(*store, 999, 1, 48000, 10).ok, "a missing device refuses");
    // SC-482: slices placed by hand. Key 36 plays from 0, 37 from the first
    // marker; on the ramp a slice's first output sample is its frame / 400.
    const auto playKey = [&](int key) {
        SQLite::Statement row(db, "SELECT state_hash FROM plugin_state WHERE device_id=? AND stream_role='sample:1'");
        row.bind(1, device);
        if (!row.executeStep()) return -1.f;
        const auto blob = store->getStateBlob(row.getColumn(0).getString());
        std::vector<std::uint8_t> bytes;
        for (const auto b : *blob) bytes.push_back(static_cast<std::uint8_t>(b));
        auto restored = restoreBuiltinSample(Payload::from_cbor(bytes), 48000);
        const auto* sample = dynamic_cast<const PdOneShotSample*>(restored.get());
        if (sample == nullptr) return -1.f;
        OneShot voice;
        // Slice by Manual, Mono; no snap, filter or fade in; full level.
        const std::array<std::pair<const char*, double>, 7> settings{{{"mode", 1.}, {"slice_by", 1.}, {"snap", 0.},
            {"filter_on", 0.}, {"fade_in", 0.}, {"vel_to_vol", 1.}, {"volume", .5}}};
        for (const auto& [id, value] : settings) voice.setParam(id, ParamValue::fromNormalized(value));
        voice.prepare(48000, 64);
        std::array<float, 64> left{}, right{};
        float* out[]{left.data(), right.data()};
        const Event note{0, EventType::NoteOn, 0, static_cast<std::uint16_t>(key), 1, 0, 1};
        NodeIo io;
        io.out = out;
        io.channels = 2;
        io.frames = 64;
        io.events = {&note, 1};
        voice.processPrepared(io, sample->prepared.get(), 1);
        return left[0];
    };
    const std::array<std::size_t, 3> marks{300, 100, 100};
    const auto placed = prepareSampleSlices(*store, device, 1, marks);
    if (!placed.ok) std::printf("slices: %s\n", placed.error.c_str());
    check(placed.ok && commitSampleAssignment(*store, placed).ok && History(*store).nextUndo()->label == "Edit Slices",
          "placing slices by hand is one Undo step");
    check(playKey(36) == 0.f && playKey(37) == .25f && playKey(38) == .75f && playKey(39) == 0.f,
          "Slice by Manual plays the markers placed, in order, each once");
    const std::array<std::size_t, 2> moved{200, 300};
    check(commitSampleAssignment(*store, prepareSampleSlices(*store, device, 1, moved)).ok && playKey(37) == .5f,
          "moving a marker moves its slice");
    const std::array<std::size_t, 1> deleted{300};
    check(commitSampleAssignment(*store, prepareSampleSlices(*store, device, 1, deleted)).ok && playKey(37) == .75f,
          "deleting a marker joins its slice to the one before");
    const std::array<std::size_t, 1> past{400};
    check(!prepareSampleSlices(*store, device, 1, past).ok, "a marker past the sample's end refuses");
    check(History(*store).undo().ok && playKey(37) == .5f, "Undo puts the deleted marker back");
}
#endif
void callbackRefusal() {
    bool refused=false;audio::CallbackScope callback;
    try{(void)restoreBuiltinSample(emptyBuiltinSample("org.adidaw.builtin.color-cab",1),48000);}catch(const std::exception&){refused=true;}
    check(refused,"sample preparation refuses on audio thread before IO");
}
}
int main(){try{
#if ADI_WITH_PD
assignmentAndPlayback();
reverseAndCrop();
#endif
callbackRefusal();}catch(const std::exception& exception){countAllocations=false;check(false,exception.what());}std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;}
