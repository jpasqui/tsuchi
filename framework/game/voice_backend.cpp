// The voice service: the microphone as Opus packets, and other players' packets played (DESIGN.md, "Voice").
//
// Everything is the game's own SDK, found by name in the system library: nn::audio AudioIn and AudioOut, and
// nn::codec's software Opus. One thread of the framework's, on another core than the game thread and below
// it, does the audio and the Opus work; the game thread only moves packets through fixed rings.
#include <string.h>

#include "frame.h"
#include "game.h"
#include "modules.h"
#include "mods.h"
#include "registry.h"
#include "services.h"
#include "shared.h"
#include "voice_service.h"

namespace tsuchi {
namespace {
namespace t = game::table;

constexpr uint32_t kRate = 48000, kFrame = 960;        // 20 ms
constexpr uint32_t kBitRate = 24000;
constexpr uint32_t kPacketMost = 512;
constexpr uint32_t kCaptured = 50;                     // a second of packets waits to be read
constexpr uint32_t kStreams = 4, kQueued = 25;         // half a second a stream
constexpr uint32_t kStartAt = 3;                       // packets a stream holds before it plays
constexpr uint32_t kBuffers = 4, kBufferBytes = 0x1000;
constexpr uint64_t kObjectBytes = 0x1000;

// ---- The SDK ----------------------------------------------------------------------------------------------

using Result = uint32_t;
struct Sdk {
    int (*list_ins)(void* infos, int count);
    void (*in_parameter)(void* parameter);
    Result (*open_in)(void* in, const char* name, const void* parameter);
    Result (*open_default_in)(void* in, const void* parameter);
    Result (*start_in)(void* in);
    void (*stop_in)(void* in);
    void (*close_in)(void* in);
    void (*in_buffer)(void* buffer, void* data, uint64_t size, uint64_t data_size);
    bool (*append_in)(void* in, void* buffer);
    void* (*released_in)(void* in);
    void* (*in_data)(const void* buffer);
    uint64_t (*in_data_size)(const void* buffer);
    int (*in_channels)(const void* in);
    int (*in_rate)(const void* in);
    int (*in_format)(const void* in);
    void (*out_parameter)(void* parameter);
    Result (*open_out)(void* out, const void* parameter);
    Result (*start_out)(void* out);
    void (*stop_out)(void* out);
    void (*close_out)(void* out);
    void (*out_buffer)(void* buffer, void* data, uint64_t size, uint64_t data_size);
    bool (*append_out)(void* out, void* buffer);
    void* (*released_out)(void* out);
    int (*out_channels)(const void* out);
    int (*out_rate)(const void* out);
    int (*out_format)(const void* out);
    uint64_t (*encoder_work)(int rate, int channels);
    Result (*encoder_start)(void* encoder, int rate, int channels, void* work, uint64_t size);
    Result (*encoder_bit_rate)(void* encoder, int bit_rate);
    Result (*encode)(void* encoder, uint64_t* written, void* out, uint64_t capacity, const int16_t* pcm, int samples);
    uint64_t (*decoder_work)(int rate, int channels);
    Result (*decoder_start)(void* decoder, int rate, int channels, void* work, uint64_t size);
    Result (*decode)(void* decoder, uint64_t* consumed, int* samples, int16_t* out, uint64_t capacity, const void* in, uint64_t size);
    uint64_t (*available_cores)();
} sdk;
bool looked = false, capture_found = false, playback_found = false, threads_found = false;
constexpr int kPcm16 = 2;   // nn::audio::SampleFormat_PcmInt16

// A line in the framework's log: the text, then up to three numbers.
void note(const char* text, int64_t a = 0, int64_t b = 0, int64_t c = 0, int numbers = 0) {
    char line[160];
    size_t used = 0;
    for (const char* t = text; *t && used < 120; ++t) line[used++] = *t;
    const int64_t values[3] = {a, b, c};
    for (int i = 0; i < numbers && i < 3; ++i) {
        line[used++] = ' ';
        int64_t v = values[i];
        if (v < 0) { line[used++] = '-'; v = -v; }
        char digits[20];
        int n = 0;
        do { digits[n++] = char('0' + v % 10); v /= 10; } while (v && n < 20);
        while (n && used < sizeof line - 1) line[used++] = digits[--n];
    }
    line[used] = 0;
    mod_note(kFrameworkLogId, line);
}

template <class F>
bool find(uint64_t library, F& slot, const char* name) {
    slot = reinterpret_cast<F>(game::module_symbol(library, name));
    return slot != nullptr;
}

void look_up() {
    if (looked) return;
    looked = true;
    const uint64_t library = game::module_containing(game::global<uint64_t>(t::data::system_library_anchor));
    if (!library) return;
    bool in = true, out = true, opus = true;
    in &= find(library, sdk.list_ins, "_ZN2nn5audio12ListAudioInsEPNS0_11AudioInInfoEi");
    in &= find(library, sdk.in_parameter, "_ZN2nn5audio26InitializeAudioInParameterEPNS0_16AudioInParameterE");
    in &= find(library, sdk.open_in, "_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE");
    in &= find(library, sdk.open_default_in, "_ZN2nn5audio18OpenDefaultAudioInEPNS0_7AudioInERKNS0_16AudioInParameterE");
    in &= find(library, sdk.start_in, "_ZN2nn5audio12StartAudioInEPNS0_7AudioInE");
    in &= find(library, sdk.stop_in, "_ZN2nn5audio11StopAudioInEPNS0_7AudioInE");
    in &= find(library, sdk.close_in, "_ZN2nn5audio12CloseAudioInEPNS0_7AudioInE");
    in &= find(library, sdk.in_buffer, "_ZN2nn5audio20SetAudioInBufferInfoEPNS0_13AudioInBufferEPvmm");
    in &= find(library, sdk.append_in, "_ZN2nn5audio19AppendAudioInBufferEPNS0_7AudioInEPNS0_13AudioInBufferE");
    in &= find(library, sdk.released_in, "_ZN2nn5audio24GetReleasedAudioInBufferEPNS0_7AudioInE");
    in &= find(library, sdk.in_data, "_ZN2nn5audio27GetAudioInBufferDataPointerEPKNS0_13AudioInBufferE");
    in &= find(library, sdk.in_data_size, "_ZN2nn5audio24GetAudioInBufferDataSizeEPKNS0_13AudioInBufferE");
    in &= find(library, sdk.in_channels, "_ZN2nn5audio22GetAudioInChannelCountEPKNS0_7AudioInE");
    in &= find(library, sdk.in_rate, "_ZN2nn5audio20GetAudioInSampleRateEPKNS0_7AudioInE");
    in &= find(library, sdk.in_format, "_ZN2nn5audio22GetAudioInSampleFormatEPKNS0_7AudioInE");
    out &= find(library, sdk.out_parameter, "_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE");
    out &= find(library, sdk.open_out, "_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE");
    out &= find(library, sdk.start_out, "_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE");
    out &= find(library, sdk.stop_out, "_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE");
    out &= find(library, sdk.close_out, "_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE");
    out &= find(library, sdk.out_buffer, "_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm");
    out &= find(library, sdk.append_out, "_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE");
    out &= find(library, sdk.released_out, "_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE");
    out &= find(library, sdk.out_channels, "_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE");
    out &= find(library, sdk.out_rate, "_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE");
    out &= find(library, sdk.out_format, "_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE");
    opus &= find(library, sdk.encoder_work, "_ZN2nn5codec28GetOpusEncoderWorkBufferSizeEii");
    opus &= find(library, sdk.encoder_start, "_ZN2nn5codec21InitializeOpusEncoderEPNS0_15OpusEncoderTypeEiiPvm");
    opus &= find(library, sdk.encoder_bit_rate, "_ZN2nn5codec21SetOpusEncoderBitRateEPNS0_15OpusEncoderTypeEi");
    opus &= find(library, sdk.encode, "_ZN2nn5codec21EncodeOpusInterleavedEPNS0_15OpusEncoderTypeEPmPvmPKsi");
    opus &= find(library, sdk.decoder_work, "_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii");
    opus &= find(library, sdk.decoder_start, "_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
    opus &= find(library, sdk.decode, "_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
    find(library, sdk.available_cores, "_ZN2nn2os26GetThreadAvailableCoreMaskEv");
    threads_found = t::bound(t::fn::os_create_thread) && t::bound(t::fn::os_start_thread) && t::bound(t::fn::os_sleep_thread) &&
        t::bound(t::fn::os_current_thread) && t::bound(t::fn::os_thread_priority);
    capture_found = in && opus && threads_found;
    playback_found = out && opus && threads_found;
    if (!in) note("voice: the game has no microphone functions");
    if (!out) note("voice: the game has no sound output functions");
    if (!opus) note("voice: the game has no Opus codec");
    if (!threads_found) note("voice: no thread functions");
}

uint32_t load(const uint32_t& value) { return __atomic_load_n(&value, __ATOMIC_ACQUIRE); }
void store(uint32_t& value, uint32_t to) { __atomic_store_n(&value, to, __ATOMIC_RELEASE); }

// ---- Packets ----------------------------------------------------------------------------------------------

struct Packet { uint32_t size; uint8_t bytes[kPacketMost]; };
// One writer, one reader: the writer moves `head`, the reader `tail`.
template <uint32_t N>
struct Ring {
    Packet slots[N];
    uint32_t head = 0, tail = 0;
    uint32_t count() const { return load(head) - load(tail); }
    bool push(const void* data, uint32_t size) {
        if (count() >= N || !size || size > kPacketMost) return false;
        Packet& slot = slots[load(head) % N];
        memcpy(slot.bytes, data, size); slot.size = size;
        store(head, load(head) + 1);
        return true;
    }
    const Packet* front() const { return count() ? &slots[load(tail) % N] : nullptr; }
    void pop() { if (count()) store(tail, load(tail) + 1); }
    void clear() { store(tail, load(head)); }
};

// ---- Capture ----------------------------------------------------------------------------------------------

struct Capture {
    TSUCHI_Mod* owner = nullptr;
    uint32_t on = 0, busy = 0;      // the thread works on it while `on`; `busy` while it is inside
    alignas(16) uint8_t in[kObjectBytes];
    alignas(16) uint8_t buffers[kBuffers][64];
    int channels = 0;
    int16_t frame[kFrame];
    uint32_t filled = 0;
    alignas(16) uint8_t encoder[kObjectBytes];
    uint32_t level_bits = 0;        // the latest frame's loudness, as float bits
} capture;
alignas(4096) uint8_t capture_data[kBuffers][kBufferBytes];
alignas(16) uint8_t encoder_work[0x20000];   // the SDK asks 73,674 bytes for 48 kHz mono
Ring<kCaptured> captured;

// ---- Playback ---------------------------------------------------------------------------------------------

struct Stream {
    TSUCHI_Mod* owner = nullptr;
    uint32_t on = 0, playing = 0, reset = 0;
    uint32_t volume_bits = 0;
    alignas(16) uint8_t decoder[kObjectBytes];
    bool decoder_ready = false;
    Ring<kQueued> queue;
};
Stream streams[kStreams];
alignas(16) uint8_t decoder_work[kStreams][0x10000];
struct Output {
    uint32_t on = 0, busy = 0;
    alignas(16) uint8_t out[kObjectBytes];
    alignas(16) uint8_t buffers[kBuffers][64];
    int channels = 0;
} output;
alignas(4096) uint8_t output_data[kBuffers][kBufferBytes];

float bits_float(uint32_t bits) { float value; memcpy(&value, &bits, 4); return value; }
uint32_t float_bits(float value) { uint32_t bits; memcpy(&bits, &value, 4); return bits; }

// ---- The thread -------------------------------------------------------------------------------------------

// A captured frame: its loudness, then Opus with the SDK's 8-byte header when the encoder left none.
void frame_done() {
    double sum = 0;
    for (uint32_t i = 0; i < kFrame; ++i) sum += double(capture.frame[i]) * capture.frame[i];
    double mean = sum / kFrame, root = 0;
    if (mean > 0) { root = mean / 32768.0 / 32768.0; double r = root; for (int i = 0; i < 20; ++i) r = 0.5 * (r + root / (r > 0 ? r : 1)); root = r; }
    store(capture.level_bits, float_bits(float(root)));
    uint8_t packet[kPacketMost];
    uint64_t written = 0;
    if (sdk.encode(capture.encoder, &written, packet + 8, kPacketMost - 8, capture.frame, int(kFrame)) || !written || written > kPacketMost - 8) return;
    uint32_t size = uint32_t(written);
    const uint8_t* body = packet + 8;
    const uint32_t own = uint32_t(body[0]) << 24 | uint32_t(body[1]) << 16 | uint32_t(body[2]) << 8 | body[3];
    if (size >= 8 && own == size - 8) {
        memmove(packet, body, size);   // the encoder wrote the header itself
    } else {
        packet[0] = uint8_t(size >> 24); packet[1] = uint8_t(size >> 16); packet[2] = uint8_t(size >> 8); packet[3] = uint8_t(size);
        memset(packet + 4, 0, 4);
        size += 8;
    }
    if (captured.count() >= kCaptured) captured.pop();   // the oldest goes; the reader is on the game thread
    captured.push(packet, size);
}

void capture_step() {
    if (!load(capture.on)) return;
    store(capture.busy, 1);
    if (load(capture.on)) {
        for (void* buffer; (buffer = sdk.released_in(capture.in)) != nullptr;) {
            const auto* pcm = static_cast<const int16_t*>(sdk.in_data(buffer));
            const uint64_t samples = sdk.in_data_size(buffer) / 2 / uint64_t(capture.channels);
            for (uint64_t i = 0; pcm && i < samples; ++i) {
                int32_t mixed = 0;
                for (int c = 0; c < capture.channels; ++c) mixed += pcm[i * uint64_t(capture.channels) + uint64_t(c)];
                capture.frame[capture.filled++] = int16_t(mixed / capture.channels);
                if (capture.filled == kFrame) { frame_done(); capture.filled = 0; }
            }
            sdk.append_in(capture.in, buffer);
        }
    }
    store(capture.busy, 0);
}

// Whether the decoder takes packets with the 8-byte header or without: found on the first packet that decodes.
int with_header = -1;

bool decode(Stream& stream, const Packet& packet, int16_t* pcm, int& samples) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool headered = with_header >= 0 ? with_header == 1 : attempt == 0;
        if (!headered && packet.size <= 8) return false;
        const uint8_t* data = headered ? packet.bytes : packet.bytes + 8;
        const uint64_t size = headered ? packet.size : packet.size - 8;
        uint64_t consumed = 0;
        samples = 0;
        if (!sdk.decode(stream.decoder, &consumed, &samples, pcm, kFrame * 2, data, size) && samples > 0) {
            with_header = headered ? 1 : 0;
            return true;
        }
        if (with_header >= 0) return false;
    }
    return false;
}

// One 20 ms buffer of every playing stream, mixed.
void mix(int16_t* out) {
    int32_t sum[kFrame] = {};
    int samples_of = 0;
    for (Stream& stream : streams) {
        if (!load(stream.on)) continue;
        if (load(stream.reset)) { stream.queue.clear(); stream.playing = 0; store(stream.reset, 0); }
        if (!stream.playing && stream.queue.count() >= kStartAt) stream.playing = 1;
        if (!stream.playing) continue;
        while (stream.queue.count() > kQueued / 2 + kStartAt) stream.queue.pop();   // far behind: catch up
        const Packet* packet = stream.queue.front();
        if (!packet) { stream.playing = 0; continue; }   // ran dry: wait for the jitter buffer again
        int16_t pcm[kFrame];
        const bool decoded = stream.decoder_ready && decode(stream, *packet, pcm, samples_of);
        stream.queue.pop();
        if (!decoded) continue;
        const float volume = bits_float(load(stream.volume_bits));
        for (int i = 0; i < samples_of && i < int(kFrame); ++i) sum[i] += int32_t(float(pcm[i]) * volume);
    }
    for (uint32_t i = 0; i < kFrame; ++i) {
        const int32_t v = sum[i] > 32767 ? 32767 : sum[i] < -32768 ? -32768 : sum[i];
        for (int c = 0; c < output.channels; ++c) out[i * uint32_t(output.channels) + uint32_t(c)] = int16_t(v);
    }
}

void output_step() {
    if (!load(output.on)) return;
    store(output.busy, 1);
    if (load(output.on)) {
        for (void* buffer; (buffer = sdk.released_out(output.out)) != nullptr;) {
            const uint32_t index = uint32_t((static_cast<uint8_t*>(buffer) - &output.buffers[0][0]) / 64);
            if (index >= kBuffers) continue;
            auto* data = reinterpret_cast<int16_t*>(output_data[index]);
            mix(data);
            const uint64_t bytes = uint64_t(kFrame) * uint64_t(output.channels) * 2;
            sdk.out_buffer(buffer, data, kBufferBytes, bytes);
            sdk.append_out(output.out, buffer);
        }
    }
    store(output.busy, 0);
}

void voice_thread(void*) {
    for (;;) {
        capture_step();
        output_step();
        t::fn::os_sleep_thread(int64_t(4) * 1000000);   // 4 ms
    }
}

// On another core than the game thread when there is one, and just below its priority.
bool thread_started() {
    static bool started = false, refused = false;
    if (started || refused) return started;
    refused = true;
    if (!threads_found) return false;
    alignas(16) static uint8_t thread[0x200];
    alignas(4096) static uint8_t stack[0x10000];
    void* now = t::fn::os_current_thread();
    int priority = t::fn::os_thread_priority(now) + 1;
    if (priority > 31) priority = 31;
    if (t::fn::os_create_thread(thread, reinterpret_cast<void*>(&voice_thread), nullptr, stack, sizeof stack, priority)) return false;
    if (sdk.available_cores && t::bound(t::fn::os_thread_core_mask) && t::bound(t::fn::os_set_thread_core_mask)) {
        int ideal = -1; uint64_t mask = 0;
        t::fn::os_thread_core_mask(&ideal, &mask, now);
        const uint64_t others = sdk.available_cores() & ~(ideal >= 0 ? (uint64_t(1) << ideal) : 0) & 0x7;
        for (int core = 2; core >= 0; --core)
            if ((others >> core) & 1) { t::fn::os_set_thread_core_mask(thread, core, uint64_t(1) << core); break; }
    }
    if (t::bound(t::fn::os_set_thread_name)) t::fn::os_set_thread_name(thread, "Tsuchi voice");
    t::fn::os_start_thread(thread);
    started = true; refused = false;
    return true;
}

void wait_idle(const uint32_t& busy) {
    for (int i = 0; i < 200 && load(busy); ++i) t::fn::os_sleep_thread(int64_t(1) * 1000000);
}

// ---- Opening and closing, on the game thread --------------------------------------------------------------

TSUCHI_Result open_capture() {
    memset(capture.in, 0, sizeof capture.in);
    alignas(16) uint8_t parameter[64] = {};
    sdk.in_parameter(parameter);
    // Eden and the console name the USB or headset microphone "Uac" when there is one; otherwise the default.
    alignas(16) static char names[8][256];
    const int count = sdk.list_ins(names, 8);
    if (count <= 0) { note("voice: no microphone is listed", count, 0, 0, 1); return TSUCHI_UNSUPPORTED; }
    Result opened = 1;
    for (int i = 0; i < count && opened; ++i)
        if (!strcmp(names[i], "Uac")) opened = sdk.open_in(capture.in, names[i], parameter);
    if (opened) opened = sdk.open_default_in(capture.in, parameter);
    if (opened) { note("voice: the microphone would not open", int64_t(opened), count, 0, 2); return TSUCHI_NOT_READY; }
    capture.channels = sdk.in_channels(capture.in);
    if (capture.channels < 1 || capture.channels > 6 || sdk.in_rate(capture.in) != int(kRate) || sdk.in_format(capture.in) != kPcm16) {
        note("voice: the microphone's channels, rate and format", capture.channels, sdk.in_rate(capture.in), sdk.in_format(capture.in), 3);
        sdk.close_in(capture.in);
        return TSUCHI_UNSUPPORTED;
    }
    const uint64_t work = sdk.encoder_work(int(kRate), 1);
    memset(capture.encoder, 0, sizeof capture.encoder);
    if (!work || work > sizeof encoder_work || sdk.encoder_start(capture.encoder, int(kRate), 1, encoder_work, sizeof encoder_work)) {
        note("voice: the Opus encoder would not start; work bytes", int64_t(work), int64_t(sizeof encoder_work), 0, 2);
        sdk.close_in(capture.in);
        return TSUCHI_UNSUPPORTED;
    }
    sdk.encoder_bit_rate(capture.encoder, int(kBitRate));
    const uint64_t bytes = uint64_t(kFrame) * uint64_t(capture.channels) * 2;
    if (bytes > kBufferBytes) { sdk.close_in(capture.in); return TSUCHI_UNSUPPORTED; }
    for (uint32_t i = 0; i < kBuffers; ++i) {
        memset(capture.buffers[i], 0, sizeof capture.buffers[i]);
        sdk.in_buffer(capture.buffers[i], capture_data[i], kBufferBytes, bytes);
        sdk.append_in(capture.in, capture.buffers[i]);
    }
    if (sdk.start_in(capture.in)) { note("voice: the microphone would not start"); sdk.close_in(capture.in); return TSUCHI_NOT_READY; }
    capture.filled = 0;
    captured.clear();
    store(capture.level_bits, 0);
    store(capture.on, 1);
    return TSUCHI_OK;
}

void close_capture() {
    if (!load(capture.on)) return;
    store(capture.on, 0);
    wait_idle(capture.busy);
    sdk.stop_in(capture.in);
    sdk.close_in(capture.in);
    captured.clear();
    capture.owner = nullptr;
}

TSUCHI_Result open_output() {
    if (load(output.on)) return TSUCHI_OK;
    memset(output.out, 0, sizeof output.out);
    alignas(16) uint8_t parameter[64] = {};
    sdk.out_parameter(parameter);
    if (sdk.open_out(output.out, parameter)) return TSUCHI_NOT_READY;
    output.channels = sdk.out_channels(output.out);
    if (output.channels < 1 || output.channels > 6 || sdk.out_rate(output.out) != int(kRate) || sdk.out_format(output.out) != kPcm16 ||
        uint64_t(kFrame) * uint64_t(output.channels) * 2 > kBufferBytes) {
        sdk.close_out(output.out);
        return TSUCHI_UNSUPPORTED;
    }
    const uint64_t bytes = uint64_t(kFrame) * uint64_t(output.channels) * 2;
    for (uint32_t i = 0; i < kBuffers; ++i) {
        memset(output_data[i], 0, kBufferBytes);
        memset(output.buffers[i], 0, sizeof output.buffers[i]);
        sdk.out_buffer(output.buffers[i], output_data[i], kBufferBytes, bytes);
        sdk.append_out(output.out, output.buffers[i]);
    }
    if (sdk.start_out(output.out)) { sdk.close_out(output.out); return TSUCHI_NOT_READY; }
    store(output.on, 1);
    return TSUCHI_OK;
}

void close_output_when_idle() {
    for (const Stream& stream : streams)
        if (load(stream.on)) return;
    if (!load(output.on)) return;
    store(output.on, 0);
    wait_idle(output.busy);
    sdk.stop_out(output.out);
    sdk.close_out(output.out);
}

void end_stream(Stream& stream) {
    store(stream.on, 0);
    wait_idle(output.busy);
    stream.queue.clear();
    stream.playing = 0;
    stream.owner = nullptr;
}

// ---- The service ------------------------------------------------------------------------------------------

TSUCHI_Result start_capture(TSUCHI_Mod* mod) {
    look_up();
    if (!capture_found) return TSUCHI_UNSUPPORTED;
    if (capture.owner && capture.owner != mod && mod_usable(capture.owner)) return TSUCHI_BUSY;
    if (capture.owner == mod && load(capture.on)) return TSUCHI_OK;
    if (!thread_started()) { note("voice: its thread could not start"); return TSUCHI_UNSUPPORTED; }
    close_capture();
    const TSUCHI_Result opened = open_capture();
    if (opened == TSUCHI_OK) capture.owner = mod;
    return opened;
}

TSUCHI_Result stop_capture(TSUCHI_Mod* mod) {
    if (capture.owner != mod || !load(capture.on)) return TSUCHI_NOT_FOUND;
    close_capture();
    return TSUCHI_OK;
}

TSUCHI_Result read_packet(TSUCHI_Mod* mod, void* buffer, uint64_t capacity, uint64_t* out) {
    if (capture.owner != mod || !load(capture.on)) return TSUCHI_NOT_FOUND;
    const Packet* packet = captured.front();
    if (!packet) return TSUCHI_NOT_READY;
    const uint64_t size = packet->size < capacity ? packet->size : capacity;
    memcpy(buffer, packet->bytes, size);
    *out = size;
    captured.pop();
    return TSUCHI_OK;
}

TSUCHI_Result level(TSUCHI_Mod* mod, float* out) {
    if (capture.owner != mod || !load(capture.on)) return TSUCHI_NOT_FOUND;
    *out = bits_float(load(capture.level_bits));
    return TSUCHI_OK;
}

TSUCHI_Result play_packet(TSUCHI_Mod* mod, uint32_t index, const void* packet, uint64_t size) {
    if (index >= kStreams || !size || size > kPacketMost) return TSUCHI_INVALID;
    look_up();
    if (!playback_found) return TSUCHI_UNSUPPORTED;
    Stream& stream = streams[index];
    if (stream.owner && stream.owner != mod && mod_usable(stream.owner)) return TSUCHI_BUSY;
    if (!load(stream.on)) {
        if (!thread_started()) return TSUCHI_UNSUPPORTED;
        const TSUCHI_Result opened = open_output();
        if (opened != TSUCHI_OK) return opened;
        if (!stream.decoder_ready) {
            const uint64_t work = sdk.decoder_work(int(kRate), 1);
            memset(stream.decoder, 0, sizeof stream.decoder);
            stream.decoder_ready = work && work <= sizeof decoder_work[0] &&
                !sdk.decoder_start(stream.decoder, int(kRate), 1, decoder_work[index], sizeof decoder_work[0]);
            if (!stream.decoder_ready) {
                note("voice: the Opus decoder would not start; work bytes", int64_t(work), int64_t(sizeof decoder_work[0]), 0, 2);
                return TSUCHI_UNSUPPORTED;
            }
        }
        stream.owner = mod;
        store(stream.volume_bits, float_bits(1.0f));
        store(stream.reset, 1);
        store(stream.on, 1);
    }
    if (stream.queue.count() < kQueued) stream.queue.push(packet, uint32_t(size));
    return TSUCHI_OK;
}

TSUCHI_Result set_volume(TSUCHI_Mod* mod, uint32_t index, float volume) {
    if (index >= kStreams || !(volume >= 0 && volume <= 2)) return TSUCHI_INVALID;
    Stream& stream = streams[index];
    if (stream.owner != mod || !load(stream.on)) return TSUCHI_NOT_FOUND;
    store(stream.volume_bits, float_bits(volume));
    return TSUCHI_OK;
}

TSUCHI_Result stop_stream(TSUCHI_Mod* mod, uint32_t index) {
    if (index >= kStreams) return TSUCHI_INVALID;
    Stream& stream = streams[index];
    if (stream.owner != mod || !load(stream.on)) return TSUCHI_NOT_FOUND;
    end_stream(stream);
    close_output_when_idle();
    return TSUCHI_OK;
}

// Each frame: what a stopped mod held ends.
void step() {
    if (capture.owner && !mod_usable(capture.owner)) close_capture();
    bool ended = false;
    for (Stream& stream : streams)
        if (stream.owner && !mod_usable(stream.owner) && load(stream.on)) { end_stream(stream); ended = true; }
    if (ended) close_output_when_idle();
}

const voice::Backend service{start_capture, stop_capture, read_packet, level, play_packet, set_volume, stop_stream};

} // namespace

void voice_start() {
    voice::set_backend(&service);
    each_frame(step);
}

} // namespace tsuchi
