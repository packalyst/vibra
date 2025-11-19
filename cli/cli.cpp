#include "../cli/cli.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <args.hxx>
#include "communication/shazam.h"
#include "bulk_processor.h"

int CLI::Run(int argc, char **argv)
{
    args::ArgumentParser parser("");
    parser.SetArgumentSeparations(true, true, true, true);

    parser.helpParams.width = 200;
    parser.helpParams.progindent = 0;
    parser.helpParams.progtailindent = 0;
    parser.helpParams.flagindent = 2;
    parser.helpParams.descriptionindent = 2;
    parser.helpParams.eachgroupindent = 4;
    parser.helpParams.showValueName = false;
    parser.helpParams.optionsString = "Options:";
    parser.helpParams.proglineOptions = "{COMMAND} [OPTIONS]";

    args::Group actions(parser, "Commands:", args::Group::Validators::Xor);
    args::Flag fingerprint_only(actions, "fingerprint", "Generate a fingerprint",
                                {'F', "fingerprint"});
    args::Flag recognize(actions, "recognize", "Recognize a song", {'R', "recognize"});
    args::Flag bulk_recognize(actions, "bulk", "Bulk recognize all audio files in a directory",
                              {'B', "bulk"});
    args::HelpFlag help(actions, "help", "Display this help menu", {'h', "help"});

    args::Group sources(parser, "Sources:", args::Group::Validators::Xor);

    args::Group file_sources(sources, "File sources:", args::Group::Validators::Xor);
    args::ValueFlag<std::string> music_file(file_sources, "file",
                                            "FFmpeg required for non-wav files", {'f', "file"});
    args::ValueFlag<std::string> directory(file_sources, "dir",
                                           "Directory path for bulk recognition", {'d', "dir"});

    args::Group raw_sources(sources, "Raw PCM sources:", args::Group::Validators::All);
    args::ValueFlag<int> chunk_seconds(raw_sources, "seconds", "Chunk seconds", {'s', "seconds"});
    args::ValueFlag<int> sample_rate(raw_sources, "rate", "Sample rate", {'r', "rate"});
    args::ValueFlag<int> channels(raw_sources, "channels", "Channels", {'c', "channels"});
    args::ValueFlag<int> bits_per_sample(raw_sources, "bits", "Bits per sample", {'b', "bits"});

    args::Group source_type(raw_sources, "Source type:", args::Group::Validators::AtMostOne);
    args::Flag signed_pcm(source_type, "signed", "Signed PCM (default)", {'S', "signed"});
    args::Flag float_pcm(source_type, "float", "Float PCM", {'D', "float"});

    args::Group bulk_options(parser, "Bulk options:");
    args::ValueFlag<std::string> output_json(bulk_options, "output",
                                             "Output JSON file path (default: results.json)",
                                             {'o', "output"});
    args::ValueFlag<int> threads(bulk_options, "threads",
                                 "Number of parallel threads (default: 1)",
                                 {'t', "threads"});
    args::ValueFlag<int> delay(bulk_options, "delay",
                               "Delay in seconds after each file (default: 2, helps avoid rate limiting)",
                               {'w', "delay"});
    args::Flag resume(bulk_options, "resume",
                     "Resume from previous run (skip already processed files)",
                     {"resume"});

    args::Group proxy_options(parser, "Proxy options:");
    args::ValueFlag<std::string> proxy_host(proxy_options, "host",
                                            "Proxy host address",
                                            {"proxy-host"});
    args::ValueFlag<int> proxy_port(proxy_options, "port",
                                    "Proxy port (default: 8080)",
                                    {"proxy-port"});
    args::ValueFlag<std::string> proxy_user(proxy_options, "user",
                                            "Proxy username",
                                            {"proxy-user"});
    args::ValueFlag<std::string> proxy_pass(proxy_options, "pass",
                                            "Proxy password",
                                            {"proxy-pass"});
    args::ValueFlag<std::string> proxy_type(proxy_options, "type",
                                            "Proxy type: http or socks5 (default: http)",
                                            {"proxy-type"});
    args::ValueFlag<std::string> proxy_rotation_url(proxy_options, "url",
                                                    "URL to fetch new proxy from for rotation",
                                                    {"proxy-rotation-url"});

    try
    {
        parser.ParseCLI(argc, argv);
    }
    catch (const args::Help &)
    {
        std::cout << parser;
        return 0;
    }
    catch (const std::runtime_error &e)
    {
        std::cerr << std::endl;
        std::cerr << e.what() << std::endl;
        std::cerr << std::endl;
        std::cerr << parser;
        return 1;
    }

    // Handle bulk recognition mode
    if (bulk_recognize)
    {
        if (!directory)
        {
            std::cerr << "Error: --dir/-d is required for bulk recognition" << std::endl;
            return 1;
        }

        std::string dir_path = args::get(directory);
        std::string json_path = output_json ? args::get(output_json) : "results.json";
        int num_threads = threads ? args::get(threads) : 1;
        int delay_seconds = delay ? args::get(delay) : 2;
        bool enable_resume = resume;

        if (num_threads < 1) num_threads = 1;
        if (num_threads > 16) num_threads = 16; // Reasonable upper limit
        if (delay_seconds < 0) delay_seconds = 0;

        BulkProcessor processor(dir_path, json_path, num_threads, enable_resume, delay_seconds);

        // Configure proxy if provided
        if (proxy_host || proxy_rotation_url)
        {
            // Validate: if rotation URL is provided, proxy host must also be provided
            if (proxy_rotation_url && !proxy_host)
            {
                std::cerr << "Error: --proxy-host is required when using --proxy-rotation-url" << std::endl;
                return 1;
            }

            ProxyConfig proxy_config;

            if (proxy_rotation_url)
            {
                // Use rotation URL
                proxy_config.rotation_url = args::get(proxy_rotation_url);
            }
            if (proxy_host)
            {
                // Use static proxy configuration
                proxy_config.host = args::get(proxy_host);
                proxy_config.port = proxy_port ? args::get(proxy_port) : 8080;
                proxy_config.type = proxy_type ? args::get(proxy_type) : "http";

                if (proxy_user)
                {
                    proxy_config.username = args::get(proxy_user);
                }
                if (proxy_pass)
                {
                    proxy_config.password = args::get(proxy_pass);
                }
            }

            processor.SetProxyConfig(proxy_config);
        }

        processor.Process();

        return 0;
    }

    // Handle single file recognition mode
    Fingerprint *fingerprint = nullptr;
    if (music_file)
    {
        std::string file = args::get(music_file);
        fingerprint = getFingerprintFromMusicFile(file);
    }
    else if (chunk_seconds && sample_rate && channels && bits_per_sample)
    {
        bool is_signed = signed_pcm || !float_pcm;
        fingerprint =
            getFingerprintFromStdin(args::get(chunk_seconds), args::get(sample_rate),
                                    args::get(channels), args::get(bits_per_sample), is_signed);
    }
    else
    {
        std::cerr << "Invalid arguments" << std::endl;
        return 1;
    }

    if (fingerprint_only)
    {
        std::cout << fingerprint->uri << std::endl;
    }
    else if (recognize)
    {
        std::string response = Shazam::Recognize(fingerprint);

        // Inject offsetms into the response JSON
        // Find the closing brace of the response and insert vibra_offset field before it
        size_t last_brace = response.rfind('}');
        if (last_brace != std::string::npos && fingerprint->offset_ms > 0)
        {
            std::string offset_field = ",\"vibra_offset_ms\":" + std::to_string(fingerprint->offset_ms);
            response.insert(last_brace, offset_field);
        }

        std::cout << response << std::endl;
    }
    return 0;
}

Fingerprint *CLI::getFingerprintFromMusicFile(const std::string &music_file)
{
    if (std::ifstream(music_file).good() == false)
    {
        std::cerr << "File not found: " << music_file << std::endl;
        throw std::ifstream::failure("File not found");
    }
    return vibra_get_fingerprint_from_music_file(music_file.c_str());
}

Fingerprint *CLI::getFingerprintFromStdin(int chunk_seconds, int sample_rate, int channels,
                                          int bits_per_sample, bool is_signed)
{
    std::size_t bytes = chunk_seconds * sample_rate * channels * (bits_per_sample / 8);
    std::vector<char> buffer(bytes);
    std::cin.read(buffer.data(), bytes);
    if (is_signed)
    {
        return vibra_get_fingerprint_from_signed_pcm(buffer.data(), bytes, sample_rate,
                                                     bits_per_sample, channels);
    }
    return vibra_get_fingerprint_from_float_pcm(buffer.data(), bytes, sample_rate, bits_per_sample,
                                                channels);
}
