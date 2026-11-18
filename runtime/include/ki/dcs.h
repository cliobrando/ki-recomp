// Midway DCS sound board, ROM-based mono version ("DCS Audio 2K", as in
// Killer Instinct): an ADSP-2105 at 10 MHz, 2K words of external RAM shared
// between program and data space, 8 x 512 KiB sample ROMs and a 16-bit mono DAC
// fed by SPORT1 autobuffering.
//
// Behaviour follows MAME src/mame/shared/dcs.cpp (REV_DCS1 paths): memory map
// dcs_2k_program_map/dcs_2k_data_map, boot from the selected ROM bank, input
// latch with auto-acknowledge, internal timer, and the half-buffer transfer
// timer that drains the autobuffer into the DAC and raises IRQ1 on wrap.
// The DSP core is third_party/adsp2100 (derived from MAME's ADSP-21xx core).
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "adsp2105.h"

namespace ki {

class Dcs : public adsp::Bus {
public:
    static constexpr uint64_t kClock = 10000000;  // ADSP-2105 clock (Hz)

    // roms: the eight sample ROMs in order u10, u11, u12, u13, u33, u34, u35, u36.
    explicit Dcs(const std::vector<std::vector<uint8_t>>& roms);
    ~Dcs();

    // Host interface (kinst.cpp sound_reset_w / sound_control_w / sound_status_r).
    void reset_w(bool state);    // low holds the DSP in reset (and re-runs the board reset)
    void data_w(uint16_t data);  // a 16-bit command into the input latch, raises IRQ2
    bool input_empty() const { return latch_control_.load(std::memory_order_acquire) & kInputEmpty; }

    // Runs the DSP up to `dsp_cycles` (10 MHz cycles since power-on).
    void run_until(uint64_t dsp_cycles);
    uint64_t cycles() const { return dsp_.total_cycles(); }

    // Scheduling as the host CPU sees it. Without a thread these run the DSP
    // in place. With start_thread() the DSP runs on its own thread, behind the
    // host, and the host waits only where it must see the DSP's state; the DSP
    // still reaches exactly the same cycle before every host access, so the
    // output is the same sample for sample.
    void start_thread();
    void stop_thread();                     // parks the DSP and joins the thread
    bool threaded() const { return worker_.joinable(); }
    void advance(uint64_t dsp_cycles);      // the DSP may run up to here
    void sync(uint64_t dsp_cycles);         // on return the DSP is at dsp_cycles and idle
    void park();                            // waits until the DSP has done all it was allowed to
    bool input_empty_at(uint64_t dsp_cycles);  // input latch state at that cycle (sound status read)
    // Moves the samples produced so far to the end of `out` (safe while threaded).
    void take_samples(std::vector<int16_t>& out);
    uint32_t dsp_pc() const { return dsp_.pc(); }

    // DAC output: samples produced so far (16-bit signed, mono, silence while the
    // DAC is off, so the timeline starts at power-on) and their rate. Not while threaded.
    std::vector<int16_t>& samples() { return samples_; }
    double sample_rate() const { return sample_rate_; }

    // Save states: copies another board's complete state (DSP included) into
    // this one. Both must have been built from the same sound ROMs.
    void copy_state_from(const Dcs& o);

    // adsp::Bus
    uint16_t data_read(uint32_t addr) override;
    void data_write(uint32_t addr, uint16_t data) override;
    uint32_t program_read(uint32_t addr) override;
    void program_write(uint32_t addr, uint32_t data) override;
    void sport_tx(int port, uint32_t data) override;
    void timer_fired(bool enabled) override;

private:
    static constexpr uint16_t kOutputEmpty = 0x400, kInputEmpty = 0x800;
    // Control register offsets from 0x3fe0 (MAME dcs.cpp enum).
    enum { kProgFlagData = 5, kProgFlagControl = 6, kS1Autobuf = 15, kS1Rfsdiv = 16, kS1Sclkdiv = 17,
           kS1Control = 18, kTimerScale = 27, kTimerCount = 28, kTimerPeriod = 29, kSysControl = 31 };

    void board_reset();
    void boot();
    uint16_t control_r(uint32_t offset);
    void control_w(uint32_t offset, uint16_t data);
    void transfer_half_buffer();
    void stop_dac();
    void pad_silence(uint64_t sample_cycles);
    // internal timer (MAME update_timer_count / reset_timer / internal_timer_callback)
    void timer_reset();
    void timer_update_count();
    void timer_fire();

    std::unique_ptr<adsp::Adsp2105> owned_dsp_;  // constructed after the tables below
    adsp::Adsp2105& dsp_;

    std::vector<uint16_t> rom_;  // the "dcs" region as 16-bit words (0xffxx)
    uint32_t rom_banks_ = 0;
    std::array<uint32_t, 0x400> internal_pram_{};
    std::array<uint32_t, 0x800> external_pram_{};
    std::array<uint16_t, 0x200> internal_dram_{};
    std::array<uint16_t, 0x20> control_regs_{};
    uint32_t bank_ = 0;

    // Read by the host thread while the DSP thread runs (sound status).
    std::atomic<uint16_t> latch_control_{kInputEmpty | kOutputEmpty};
    uint16_t input_data_ = 0, output_data_ = 0;
    uint16_t progflags_ = 0;

    // autobuffer / DAC
    bool dac_on_ = false;
    int ireg_ = 0;
    uint32_t size_ = 0, ireg_base_ = 0;
    int32_t incs_ = 0;
    double sample_rate_ = 0;
    uint64_t half_period_ = 0;  // DSP cycles per half buffer
    uint64_t next_transfer_ = 0;
    uint64_t dac_off_since_ = 0;  // DSP cycle since which the DAC has been off
    bool irq1_pending_clear_ = false;
    std::vector<int16_t> samples_;
    std::mutex samples_mutex_;

    // DSP thread: it runs the DSP toward goal_ and publishes its position in done_.
    void worker_loop();
    void wake_worker();
    std::thread worker_;
    std::atomic<uint64_t> goal_{0}, done_{0};
    std::atomic<bool> stop_{false}, sleeping_{false};
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;

    // internal timer
    bool timer_enable_ = false, timer_ignore_ = false, timer_armed_ = false;
    uint32_t timer_scale_ = 1, timer_period_ = 0, timer_start_count_ = 0;
    uint64_t timer_start_cycles_ = 0, timers_fired_ = 0, next_timer_ = 0;
};

}  // namespace ki
