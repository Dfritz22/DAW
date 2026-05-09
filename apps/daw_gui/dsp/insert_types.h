#pragma once

#ifdef DAW_PUBLIC_API
#error "This header is internal-only."
#endif

#include <array>
#include <cstdint>
#include <vector>

constexpr int kMaxInsertSlots = 8;
constexpr int kInsertEffectTypeCount = 8;
constexpr int kEqBandCount = 4;

// Effect type indices (must match InsertEffectName order)
constexpr int kFxEQ   = 0;
constexpr int kFxCMP  = 1;
constexpr int kFxSAT  = 2;
constexpr int kFxDLY  = 3;
constexpr int kFxREV  = 4;
constexpr int kFxGATE = 5;
constexpr int kFxDEE  = 6;
constexpr int kFxLIM  = 7;

using InsertEffectArray = std::array<std::uint8_t, kMaxInsertSlots>;
using InsertBypassArray = std::array<bool, kMaxInsertSlots>;

// EQ band filter types
constexpr int kEqPeak      = 0;
constexpr int kEqLowShelf  = 1;
constexpr int kEqHighShelf = 2;
constexpr int kEqLowPass   = 3;
constexpr int kEqHighPass  = 4;

struct EqBand {
    float freq_hz {1000.0f};
    float gain_db {0.0f};
    float q       {0.707f};
    int   type    {kEqPeak};
};

struct EqBandDspState {
    float bq_x1[2]{0.0f, 0.0f};
    float bq_x2[2]{0.0f, 0.0f};
    float bq_y1[2]{0.0f, 0.0f};
    float bq_y2[2]{0.0f, 0.0f};
};

struct InsertConfig {
    EqBand eq[kEqBandCount] = {
        {80.0f,    0.0f, 0.707f, kEqLowShelf },
        {300.0f,   0.0f, 1.0f,   kEqPeak     },
        {3000.0f,  0.0f, 1.0f,   kEqPeak     },
        {10000.0f, 0.0f, 0.707f, kEqHighShelf},
    };

    float cmp_threshold_db {-18.0f};
    float cmp_ratio        {4.0f};
    float cmp_attack_ms    {10.0f};
    float cmp_release_ms   {100.0f};
    float cmp_knee_db      {3.0f};
    float cmp_makeup_db    {0.0f};

    float sat_drive {0.3f};
    float sat_mix   {0.5f};

    float dly_time_ms   {250.0f};
    float dly_feedback  {0.3f};
    float dly_mix       {0.25f};

    float rev_room_size {0.5f};
    float rev_damping   {0.5f};
    float rev_mix       {0.25f};

    float gate_threshold_db {-40.0f};
    float gate_attack_ms    {5.0f};
    float gate_release_ms   {100.0f};
    float gate_hold_ms      {50.0f};

    float dee_threshold_db  {-20.0f};
    float dee_freq_hz       {7000.0f};
    float dee_bandwidth_hz  {3000.0f};
    float dee_reduction_db  {6.0f};

    float lim_ceiling_db  {-0.3f};
    float lim_release_ms  {50.0f};
};

struct InsertDspState {
    EqBandDspState eq[kEqBandCount];

    std::vector<float> dly_bufL, dly_bufR;
    int   dly_wpos{0};
    int   dly_lastFrames{-1};

    std::vector<float> rev_combBuf[4];
    std::vector<float> rev_apBuf[2];
    int   rev_combPos[4]{0,0,0,0};
    int   rev_apPos[2]{0,0};
    float rev_combFilt[4]{0.0f,0.0f,0.0f,0.0f};
    int   rev_lastCombLen[4]{-1,-1,-1,-1};

    float cmp_env{0.0f};
    float gate_env{0.0f};
    float gate_holdTimer{0.0f};
    float gate_gainState{0.0f};
    float dee_env{0.0f};
    float lim_env{0.0f};

    float dee_sc_x1[2]{0.0f,0.0f};
    float dee_sc_x2[2]{0.0f,0.0f};
    float dee_sc_y1[2]{0.0f,0.0f};
    float dee_sc_y2[2]{0.0f,0.0f};
};

using InsertConfigArray = std::array<InsertConfig, kMaxInsertSlots>;
using InsertDspStateArray = std::array<InsertDspState, kMaxInsertSlots>;
