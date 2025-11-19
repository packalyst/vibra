#include "../include/vibra.h"
#include "algorithm/signature_generator.h"
#include "audio/downsampler.h"
#include "audio/wav.h"
#include "utils/ffmpeg.h"
#include <cstdio>
#include <cmath>

constexpr std::uint32_t MAX_DURATION_SECONDS = 12;

Fingerprint *_get_fingerprint_from_wav(const Wav &wav);

Fingerprint *_get_fingerprint_from_low_quality_pcm(const LowQualityTrack &pcm, std::uint32_t offset_seconds = 0);

// Get song duration using ffprobe
static double get_song_duration(const std::string &file_path)
{
    std::string ffprobe_cmd = "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 ";

#ifdef _MSC_VER
    // Windows: escape quotes
    std::string escaped_path = "\"" + file_path + "\"";
#else
    // Unix: use single quotes
    std::string escaped_path = "'" + file_path + "'";
#endif

    ffprobe_cmd += escaped_path;
    ffprobe_cmd += " 2>/dev/null";

    FILE *pipe = popen(ffprobe_cmd.c_str(), "r");
    if (!pipe)
    {
        return 0.0;
    }

    char buffer[128];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr)
    {
        result += buffer;
    }
    pclose(pipe);

    try
    {
        return std::stod(result);
    }
    catch (...)
    {
        return 0.0;
    }
}

// Calculate RMS (Root Mean Square) energy of audio segment
static double calculate_rms_energy(const LowQualityTrack &pcm)
{
    if (pcm.empty())
    {
        return 0.0;
    }

    double sum = 0.0;
    for (const auto &sample : pcm)
    {
        double normalized = static_cast<double>(sample) / 32768.0; // Normalize to -1.0 to 1.0
        sum += normalized * normalized;
    }

    return std::sqrt(sum / pcm.size());
}

// Calculate spectral complexity using simple variance
static double calculate_spectral_variance(const LowQualityTrack &pcm)
{
    if (pcm.size() < 2)
    {
        return 0.0;
    }

    // Calculate mean
    double mean = 0.0;
    for (const auto &sample : pcm)
    {
        mean += std::abs(sample);
    }
    mean /= pcm.size();

    // Calculate variance
    double variance = 0.0;
    for (const auto &sample : pcm)
    {
        double diff = std::abs(sample) - mean;
        variance += diff * diff;
    }

    return variance / pcm.size();
}

// Score a segment based on multiple factors
static double score_segment(const LowQualityTrack &pcm)
{
    double energy = calculate_rms_energy(pcm);
    double variance = calculate_spectral_variance(pcm);

    // Normalize variance to similar scale as energy
    double normalized_variance = variance / 1000000.0;

    // Weighted score: 60% energy, 40% variance
    return (energy * 0.6) + (normalized_variance * 0.4);
}

// Find best segment by testing multiple positions
static std::uint32_t calculate_start_offset(double duration, const std::string &file_path)
{
    if (duration <= MAX_DURATION_SECONDS)
    {
        return 0; // Use entire song
    }

    // Test segments at different positions
    std::vector<std::uint32_t> test_positions;

    // Skip first 5 seconds (fade-ins, silence)
    // Skip last 10 seconds (fade-outs, silence)
    std::uint32_t usable_duration = static_cast<std::uint32_t>(duration) - 10;

    if (usable_duration > 5)
    {
        // Test 3 positions: 5s, 30s, and 50% point
        test_positions.push_back(5);

        if (usable_duration > 30)
        {
            test_positions.push_back(30);
        }

        std::uint32_t middle = usable_duration / 2;
        if (middle > 30)
        {
            test_positions.push_back(middle);
        }
    }
    else
    {
        test_positions.push_back(0);
    }

    // Analyze each position and pick the best
    double best_score = -1.0;
    std::uint32_t best_offset = 0;

    for (auto offset : test_positions)
    {
        // Extract short sample (3 seconds) for analysis
        try
        {
            LowQualityTrack sample = ffmpeg::FFmpegWrapper::ConvertToLowQaulityPcm(
                file_path, offset, 3);

            double score = score_segment(sample);

            if (score > best_score)
            {
                best_score = score;
                best_offset = offset;
            }
        }
        catch (...)
        {
            // If extraction fails, skip this position
            continue;
        }
    }

    return best_offset;
}

Fingerprint *vibra_get_fingerprint_from_music_file(const char *music_file_path)
{
    std::string path = music_file_path;
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".wav")
    {
        Wav wav = Wav::FromFile(path);
        return _get_fingerprint_from_wav(wav);
    }

    // Get song duration and find optimal start offset using smart analysis
    double duration = get_song_duration(path);
    std::uint32_t start_offset = calculate_start_offset(duration, path);

    LowQualityTrack pcm =
        ffmpeg::FFmpegWrapper::ConvertToLowQaulityPcm(path, start_offset, MAX_DURATION_SECONDS);
    return _get_fingerprint_from_low_quality_pcm(pcm, start_offset);
}

Fingerprint *vibra_get_fingerprint_from_wav_data(const char *raw_wav, int wav_data_size)
{
    Wav wav = Wav::FromRawWav(raw_wav, wav_data_size);
    return _get_fingerprint_from_wav(wav);
}

Fingerprint *vibra_get_fingerprint_from_signed_pcm(const char *raw_pcm, int pcm_data_size,
                                                   int sample_rate, int sample_width,
                                                   int channel_count)
{
    Wav wav = Wav::FromSignedPCM(raw_pcm, pcm_data_size, sample_rate, sample_width, channel_count);
    return _get_fingerprint_from_wav(wav);
}

Fingerprint *vibra_get_fingerprint_from_float_pcm(const char *raw_pcm, int pcm_data_size,
                                                  int sample_rate, int sample_width,
                                                  int channel_count)
{
    Wav wav = Wav::FromFloatPCM(raw_pcm, pcm_data_size, sample_rate, sample_width, channel_count);
    return _get_fingerprint_from_wav(wav);
}

const char *vibra_get_uri_from_fingerprint(Fingerprint *fingerprint)
{
    return fingerprint->uri.c_str();
}

unsigned int vibra_get_sample_ms_from_fingerprint(Fingerprint *fingerprint)
{
    return fingerprint->sample_ms;
}

void vibra_free_fingerprint(Fingerprint *fingerprint)
{
    delete fingerprint;
}

Fingerprint *_get_fingerprint_from_wav(const Wav &wav)
{
    LowQualityTrack pcm = Downsampler::GetLowQualityPCM(wav);
    return _get_fingerprint_from_low_quality_pcm(pcm, 0);
}

Fingerprint *_get_fingerprint_from_low_quality_pcm(const LowQualityTrack &pcm, std::uint32_t offset_seconds)
{
    SignatureGenerator generator;
    generator.FeedInput(pcm);
    generator.set_max_time_seconds(MAX_DURATION_SECONDS);

    Signature signature = generator.GetNextSignature();

    Fingerprint *fingerprint = new Fingerprint;
    fingerprint->uri = signature.EncodeBase64();
    fingerprint->sample_ms = signature.num_samples() * 1000 / signature.sample_rate();
    fingerprint->offset_ms = offset_seconds * 1000;
    return fingerprint;
}
