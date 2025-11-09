#include "bulk_processor.h"
#include "communication/shazam.h"
#include "../include/vibra.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <curl/curl.h>

// Static instance for signal handler
BulkProcessor* BulkProcessor::current_instance_ = nullptr;

BulkProcessor::BulkProcessor(const std::string& directory_path, const std::string& output_json_path,
                             int num_threads, bool resume, int delay_seconds)
    : directory_path_(directory_path),
      output_json_path_(output_json_path),
      num_threads_(num_threads),
      resume_enabled_(resume),
      delay_seconds_(delay_seconds),
      proxy_rotation_timeout_(60),
      next_file_index_(0) {

    // Default supported formats
    supported_formats_ = {".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac"};

    if (resume_enabled_) {
        LoadCache();
    }

    // Register signal handler for graceful shutdown
    current_instance_ = this;
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
}

void BulkProcessor::SignalHandler(int) {
    if (current_instance_) {
        std::cout << "\n\n" << std::string(60, '=') << std::endl;
        std::cout << "INTERRUPTED - Shutting down gracefully..." << std::endl;
        std::cout << std::string(60, '=') << std::endl;

        // Signal threads to stop
        current_instance_->processing_complete_ = true;

        // Give threads a moment to finish current operations
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Save current progress
        current_instance_->SaveCache();

        // Print final report
        std::cout << "\nProgress at interruption:" << std::endl;
        std::cout << "  Total files:       " << current_instance_->stats_.total_files.load() << std::endl;
        std::cout << "  Processed:         " << current_instance_->stats_.processed.load() << std::endl;
        std::cout << "  Successful:        " << current_instance_->stats_.successful.load() << std::endl;
        std::cout << "  Failed:            " << current_instance_->stats_.failed.load() << std::endl;
        if (current_instance_->stats_.skipped.load() > 0) {
            std::cout << "  Skipped (cached):  " << current_instance_->stats_.skipped.load() << std::endl;
        }
        std::cout << "  Results saved to:  " << current_instance_->output_json_path_ << std::endl;
        std::cout << std::string(60, '=') << std::endl;
        std::cout << "\nUse --resume to continue from where you left off" << std::endl;

        exit(0);
    }
}

void BulkProcessor::SetSupportedFormats(const std::vector<std::string>& formats) {
    supported_formats_ = formats;
}

void ScanDirectoryRecursive(const std::string& path, std::vector<std::string>& files,
                            const std::vector<std::string>& supported_formats) {
    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string full_path = path + "/" + entry->d_name;

        struct stat statbuf;
        if (stat(full_path.c_str(), &statbuf) == 0) {
            if (S_ISDIR(statbuf.st_mode)) {
                // Recurse into subdirectory
                ScanDirectoryRecursive(full_path, files, supported_formats);
            } else if (S_ISREG(statbuf.st_mode)) {
                // Check if supported format
                std::string filename = entry->d_name;
                size_t dot_pos = filename.find_last_of('.');
                if (dot_pos != std::string::npos) {
                    std::string ext = filename.substr(dot_pos);
                    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

                    if (std::find(supported_formats.begin(), supported_formats.end(), ext) !=
                        supported_formats.end()) {
                        files.push_back(full_path);
                    }
                }
            }
        }
    }

    closedir(dir);
}

std::vector<std::string> BulkProcessor::ScanDirectory() {
    std::vector<std::string> files;
    ScanDirectoryRecursive(directory_path_, files, supported_formats_);
    std::sort(files.begin(), files.end());
    return files;
}

bool BulkProcessor::IsSupportedFormat(const std::string& file_path) {
    size_t dot_pos = file_path.find_last_of('.');
    if (dot_pos == std::string::npos) {
        return false;
    }

    std::string ext = file_path.substr(dot_pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    return std::find(supported_formats_.begin(), supported_formats_.end(), ext) != supported_formats_.end();
}

void BulkProcessor::LoadCache() {
    std::ifstream cache_file(output_json_path_);
    if (!cache_file.is_open()) {
        return; // No cache file exists yet
    }

    std::stringstream buffer;
    buffer << cache_file.rdbuf();
    std::string json_content = buffer.str();
    cache_file.close();

    // Simple JSON parsing for our cache format
    // Format: {"results": [{"file": "path", "success": true, "ip": "...", "response": {...}}, ...]}
    size_t pos = 0;
    while ((pos = json_content.find("\"file\":", pos)) != std::string::npos) {
        // Extract file path
        size_t file_start = json_content.find("\"", pos + 7) + 1;
        size_t file_end = json_content.find("\"", file_start);
        std::string file_path = json_content.substr(file_start, file_end - file_start);

        // Find the end of this result object (next "}" at same level)
        size_t result_start = json_content.rfind("{", pos);
        size_t result_end = result_start + 1;
        int brace_count = 1;
        while (brace_count > 0 && result_end < json_content.length()) {
            if (json_content[result_end] == '{') brace_count++;
            else if (json_content[result_end] == '}') brace_count--;
            result_end++;
        }

        // Extract result block for parsing
        std::string result_block = json_content.substr(result_start, result_end - result_start);

        BulkResult result;
        result.file_path = file_path;

        // Parse success field
        size_t success_pos = result_block.find("\"success\":");
        if (success_pos != std::string::npos) {
            result.success = result_block.find("true", success_pos) < result_block.find("false", success_pos);
        }

        // Parse IP field (optional)
        size_t ip_pos = result_block.find("\"ip\":");
        if (ip_pos != std::string::npos) {
            size_t ip_start = result_block.find("\"", ip_pos + 5) + 1;
            size_t ip_end = result_block.find("\"", ip_start);
            if (ip_end != std::string::npos) {
                result.ip_address = result_block.substr(ip_start, ip_end - ip_start);
            }
        }

        // Parse response field (for successful results)
        size_t response_pos = result_block.find("\"response\":");
        if (response_pos != std::string::npos) {
            size_t resp_obj_start = result_block.find("{", response_pos);
            if (resp_obj_start != std::string::npos) {
                int resp_brace_count = 1;
                size_t resp_obj_end = resp_obj_start + 1;

                while (resp_brace_count > 0 && resp_obj_end < result_block.length()) {
                    if (result_block[resp_obj_end] == '{') resp_brace_count++;
                    else if (result_block[resp_obj_end] == '}') resp_brace_count--;
                    resp_obj_end++;
                }

                result.json_response = result_block.substr(resp_obj_start, resp_obj_end - resp_obj_start);
            }
        }

        // Parse error field (for failed results)
        size_t error_pos = result_block.find("\"error\":");
        if (error_pos != std::string::npos) {
            size_t error_start = result_block.find("\"", error_pos + 8) + 1;
            size_t error_end = result_block.find("\"", error_start);
            if (error_end != std::string::npos) {
                result.error_message = result_block.substr(error_start, error_end - error_start);
            }
        }

        results_cache_[file_path] = result;
        pos = result_end;
    }

    std::cout << "Loaded " << results_cache_.size() << " cached results from " << output_json_path_ << std::endl;
}

void BulkProcessor::SaveCache() {
    // Create a copy of results to avoid holding lock during file I/O
    std::map<std::string, BulkResult> results_copy;
    int total, processed, successful, failed, skipped;

    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        results_copy = results_cache_;
        total = stats_.total_files.load();
        processed = stats_.processed.load();
        successful = stats_.successful.load();
        failed = stats_.failed.load();
        skipped = stats_.skipped.load();
    } // Lock released here

    std::ofstream out_file(output_json_path_);
    if (!out_file.is_open()) {
        std::cerr << "Failed to open output file: " << output_json_path_ << std::endl;
        return;
    }

    out_file << "{\n  \"results\": [\n";

    bool first = true;
    for (const auto& pair : results_copy) {
        if (!first) {
            out_file << ",\n";
        }
        first = false;

        const BulkResult& result = pair.second;
        out_file << "    {\n";
        out_file << "      \"file\": \"" << result.file_path << "\",\n";
        out_file << "      \"success\": " << (result.success ? "true" : "false") << ",\n";

        if (!result.ip_address.empty()) {
            out_file << "      \"ip\": \"" << result.ip_address << "\",\n";
        }

        if (result.success) {
            out_file << "      \"response\": " << result.json_response << "\n";
        } else {
            out_file << "      \"error\": \"" << result.error_message << "\"\n";
        }

        out_file << "    }";
    }

    out_file << "\n  ],\n";
    out_file << "  \"stats\": {\n";
    out_file << "    \"total\": " << total << ",\n";
    out_file << "    \"processed\": " << processed << ",\n";
    out_file << "    \"successful\": " << successful << ",\n";
    out_file << "    \"failed\": " << failed << ",\n";
    out_file << "    \"skipped\": " << skipped << "\n";
    out_file << "  }\n";
    out_file << "}\n";

    out_file.close();
}

bool BulkProcessor::IsAlreadyProcessed(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return results_cache_.find(file_path) != results_cache_.end();
}

void BulkProcessor::AddToCache(const BulkResult& result) {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    results_cache_[result.file_path] = result;
}

bool IsValidJSON(const std::string& response) {
    // Check if response starts with '{' or '[' (basic JSON check)
    if (response.empty()) return false;

    size_t first_char = response.find_first_not_of(" \t\r\n");
    if (first_char == std::string::npos) return false;

    char first = response[first_char];
    if (first != '{' && first != '[') return false;

    // Check for HTML error pages
    if (response.find("<!doctype") != std::string::npos ||
        response.find("<!DOCTYPE") != std::string::npos ||
        response.find("<html") != std::string::npos) {
        return false;
    }

    // Check for "track" field which should exist in successful Shazam responses
    if (response.find("\"track\"") == std::string::npos) {
        return false;
    }

    return true;
}

void BulkProcessor::ProcessFile(const std::string& file_path) {
    BulkResult result;
    result.file_path = file_path;
    Fingerprint* fingerprint = nullptr;

    try {
        // Check if we're in rate limit cooldown
        {
            std::lock_guard<std::mutex> rate_lock(rate_limit_mutex_);
            if (rate_limited_.load()) {
                auto now = std::chrono::steady_clock::now();
                if (now < rate_limit_until_) {
                    // Still in cooldown, skip this file for now
                    result.success = false;
                    result.error_message = "Skipped due to rate limiting";
                    stats_.failed++;
                    AddToCache(result);
                    stats_.processed++;
                    return;
                }
                // Cooldown expired, clear rate limit flag
                rate_limited_ = false;
            }
        }

        // Serialize recognition calls to avoid threading issues in vibra library
        std::lock_guard<std::mutex> recog_lock(recognition_mutex_);

        // Generate fingerprint
        fingerprint = vibra_get_fingerprint_from_music_file(file_path.c_str());

        if (!fingerprint) {
            result.success = false;
            result.error_message = "Failed to generate fingerprint";
            stats_.failed++;
        } else {
            // Recognize with Shazam (use proxy if configured)
            std::string proxy = GetCurrentProxy();
            std::string response = Shazam::Recognize(fingerprint, proxy);

            // Fetch current IP address
            result.ip_address = FetchCurrentIP();

            // Validate response
            if (IsValidJSON(response)) {
                result.success = true;
                result.json_response = response;
                stats_.successful++;

                // Reset rate limit counter on success
                rate_limit_retry_count_ = 0;
            } else {
                // Check if it's a rate limit error
                if (response.find("429") != std::string::npos ||
                    response.find("Too Many Requests") != std::string::npos) {

                    // If proxy rotation is configured, try rotating to a new proxy
                    if (!proxy_config_.rotation_url.empty()) {
                        std::lock_guard<std::mutex> console_lock(console_mutex_);
                        std::cout << "\n[!] RATE LIMITED (429) - Rotating to new proxy..." << std::endl;

                        // Rotate proxy will test and wait for working proxy with configured timeout
                        RotateProxy(proxy_rotation_timeout_);

                        // Check if rotation succeeded
                        if (processing_complete_.load()) {
                            result.success = false;
                            result.error_message = "Failed to rotate to working proxy";
                            stats_.failed++;
                        } else {
                            // Successfully rotated, mark as failed but will retry with new proxy
                            result.success = false;
                            result.error_message = "Rate limited - rotated proxy";
                            stats_.failed++;
                        }
                    } else {
                        // No proxy rotation - use standard backoff
                        std::lock_guard<std::mutex> rate_lock(rate_limit_mutex_);

                        int retry_count = rate_limit_retry_count_.fetch_add(1);
                        int backoff_seconds[] = {30, 60, 120};  // Progressive delays

                        if (retry_count < 3) {
                            int wait_time = backoff_seconds[retry_count];
                            rate_limited_ = true;
                            rate_limit_until_ = std::chrono::steady_clock::now() +
                                               std::chrono::seconds(wait_time);

                            {
                                std::lock_guard<std::mutex> console_lock(console_mutex_);
                                std::cout << "\n[!] RATE LIMITED - Pausing all threads for "
                                         << wait_time << " seconds (attempt " << (retry_count + 1)
                                         << "/3)..." << std::endl;
                            }

                            result.success = false;
                            result.error_message = "Rate limited - will retry";
                            stats_.failed++;
                        } else {
                            // Max retries exceeded
                            {
                                std::lock_guard<std::mutex> console_lock(console_mutex_);
                                std::cout << "\n[X] MAX RATE LIMIT RETRIES EXCEEDED - Stopping processing"
                                         << std::endl;
                            }
                            processing_complete_ = true;  // Signal threads to stop
                            result.success = false;
                            result.error_message = "Rate limit exceeded - max retries reached";
                            stats_.failed++;
                        }
                    }
                } else {
                    result.success = false;
                    result.error_message = "Invalid response from Shazam";
                    stats_.failed++;
                }
            }
        }

        // Free fingerprint while still holding lock
        if (fingerprint) {
            vibra_free_fingerprint(fingerprint);
            fingerprint = nullptr;
        }
    } catch (const std::exception& e) {
        if (fingerprint) {
            vibra_free_fingerprint(fingerprint);
            fingerprint = nullptr;
        }
        result.success = false;
        result.error_message = e.what();
        stats_.failed++;
    }

    AddToCache(result);
    stats_.processed++;

    // Apply delay after processing (helps avoid rate limiting)
    if (delay_seconds_ > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(delay_seconds_));
    }
}

void BulkProcessor::WorkerThread() {
    while (!processing_complete_.load()) {
        // Check if we're in rate limit cooldown
        if (rate_limited_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        std::string file_path;

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (next_file_index_ >= files_to_process_.size()) {
                break; // No more files to process
            }
            file_path = files_to_process_[next_file_index_++];
        }

        // Check if already processed (for resume functionality)
        if (resume_enabled_ && IsAlreadyProcessed(file_path)) {
            stats_.skipped++;
            stats_.processed++;
            continue;
        }

        ProcessFile(file_path);
    }
}

void BulkProcessor::AutoSaveThread() {
    int last_processed = 0;
    while (!processing_complete_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));

        int current_processed = stats_.processed.load();
        if (current_processed > last_processed) {
            SaveCache();
            last_processed = current_processed;
        }
    }
}

void BulkProcessor::DisplayProgress() {
    while (!processing_complete_.load()) {
        {
            std::lock_guard<std::mutex> lock(console_mutex_);
            int total = stats_.total_files.load();
            int processed = stats_.processed.load();
            int successful = stats_.successful.load();
            int failed = stats_.failed.load();
            int skipped = stats_.skipped.load();

            float percentage = total > 0 ? (processed * 100.0f / total) : 0.0f;

            std::cout << "\r[";
            int bar_width = 40;
            int pos = bar_width * processed / total;
            for (int i = 0; i < bar_width; ++i) {
                if (i < pos) std::cout << "=";
                else if (i == pos) std::cout << ">";
                else std::cout << " ";
            }
            std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "% ";
            std::cout << "(" << processed << "/" << total << ") ";
            std::cout << "OK:" << successful << " FAIL:" << failed;
            if (skipped > 0) std::cout << " SKIP:" << skipped;
            std::cout << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void BulkProcessor::PrintFinalReport() {
    std::cout << "\n\n" << std::string(60, '=') << std::endl;
    std::cout << "BULK RECOGNITION COMPLETE" << std::endl;
    std::cout << std::string(60, '=') << std::endl;
    std::cout << "Total files:       " << stats_.total_files.load() << std::endl;
    std::cout << "Processed:         " << stats_.processed.load() << std::endl;
    std::cout << "Successful:        " << stats_.successful.load() << std::endl;
    std::cout << "Failed:            " << stats_.failed.load() << std::endl;
    if (stats_.skipped.load() > 0) {
        std::cout << "Skipped (cached):  " << stats_.skipped.load() << std::endl;
    }
    std::cout << "Results saved to:  " << output_json_path_ << std::endl;
    std::cout << std::string(60, '=') << std::endl;
}

void BulkProcessor::Process() {
    auto start_time = std::chrono::steady_clock::now();

    std::cout << "Scanning directory: " << directory_path_ << std::endl;
    files_to_process_ = ScanDirectory();
    stats_.total_files = files_to_process_.size();

    if (files_to_process_.empty()) {
        std::cout << "No supported audio files found in directory." << std::endl;
        return;
    }

    std::cout << "Found " << files_to_process_.size() << " audio files" << std::endl;
    std::cout << "Processing with " << num_threads_ << " thread(s)..." << std::endl;
    if (resume_enabled_) {
        std::cout << "Resume mode enabled - skipping already processed files" << std::endl;
    }
    std::cout << std::endl;

    // Start worker threads
    std::vector<std::thread> workers;
    for (int i = 0; i < num_threads_; ++i) {
        workers.emplace_back(&BulkProcessor::WorkerThread, this);
    }

    // Start progress display thread
    std::thread progress_thread(&BulkProcessor::DisplayProgress, this);

    // Start auto-save thread (saves every 5 seconds)
    std::thread autosave_thread(&BulkProcessor::AutoSaveThread, this);

    // Wait for all workers to complete
    for (auto& worker : workers) {
        worker.join();
    }

    // Stop background threads
    processing_complete_ = true;
    progress_thread.join();
    autosave_thread.join();

    // Final save
    SaveCache();

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);

    PrintFinalReport();
    std::cout << "Time elapsed:      " << duration.count() << " seconds" << std::endl;
}

std::string BulkProcessor::GetCurrentProxy() {
    std::lock_guard<std::mutex> lock(proxy_mutex_);
    return current_proxy_;
}

void BulkProcessor::RotateProxy(int timeout_seconds) {
    std::lock_guard<std::mutex> lock(proxy_mutex_);

    if (proxy_config_.rotation_url.empty()) {
        // No rotation URL configured, keep current proxy
        return;
    }

    std::cout << "Fetching new proxy from rotation URL..." << std::endl;

    // Fetch new proxy from rotation URL ONCE
    std::string new_proxy = FetchProxyFromURL(proxy_config_.rotation_url);
    if (new_proxy.empty()) {
        std::cerr << "[X] Failed to fetch proxy from rotation URL" << std::endl;
        processing_complete_ = true;
        return;
    }

    std::cout << "Got new proxy: " << new_proxy << std::endl;
    std::cout << "Waiting for proxy to come online (timeout: " << timeout_seconds << "s)..." << std::endl;

    // Now test the proxy repeatedly until it works or timeout
    auto start_time = std::chrono::steady_clock::now();
    int test_interval = 3; // Test every 3 seconds

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time
        ).count();

        if (elapsed >= timeout_seconds) {
            std::cerr << "[X] Timeout (" << timeout_seconds << "s) - proxy never came online" << std::endl;
            processing_complete_ = true;
            return;
        }

        std::cout << "Testing proxy... (" << elapsed << "s elapsed)" << std::endl;

        // Test proxy with short timeout per test
        if (TestProxy(new_proxy, 10)) {
            current_proxy_ = new_proxy;
            std::cout << "[OK] Proxy is online and working!" << std::endl;
            return;
        }

        std::cout << "Proxy not responding yet, waiting " << test_interval << " seconds..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(test_interval));
    }
}

std::string BulkProcessor::FetchProxyFromURL(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize curl for proxy rotation" << std::endl;
        return "";
    }

    std::string response;

    // Callback function for curl
    auto writeCallback = +[](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        std::string* buffer = reinterpret_cast<std::string*>(userp);
        size_t realsize = size * nmemb;
        buffer->append(reinterpret_cast<char*>(contents), realsize);
        return realsize;
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // 10 second timeout

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        std::cerr << "Failed to fetch proxy from URL: " << curl_easy_strerror(res) << std::endl;
        curl_easy_cleanup(curl);
        return "";
    }

    curl_easy_cleanup(curl);

    // Trim whitespace from response
    response.erase(0, response.find_first_not_of(" \t\r\n"));
    response.erase(response.find_last_not_of(" \t\r\n") + 1);

    return response;
}

void BulkProcessor::SetProxyConfig(const ProxyConfig& config) {
    std::lock_guard<std::mutex> lock(proxy_mutex_);
    proxy_config_ = config;

    // If rotation URL is provided, just store it - we'll call it on 429 errors
    if (!proxy_config_.rotation_url.empty()) {
        std::cout << "Proxy rotation URL configured: " << proxy_config_.rotation_url << std::endl;
        std::cout << "Will fetch and test proxy on rate limit (429) errors" << std::endl;
        // Don't fetch or test yet - wait for 429

    } else if (!proxy_config_.host.empty()) {
        // Build proxy string from config
        std::stringstream proxy_ss;

        // Add type prefix
        if (!proxy_config_.type.empty()) {
            proxy_ss << proxy_config_.type << "://";
        } else {
            proxy_ss << "http://";
        }

        // Add authentication if provided
        if (!proxy_config_.username.empty()) {
            proxy_ss << proxy_config_.username;
            if (!proxy_config_.password.empty()) {
                proxy_ss << ":" << proxy_config_.password;
            }
            proxy_ss << "@";
        }

        // Add host and port
        proxy_ss << proxy_config_.host;
        if (proxy_config_.port > 0) {
            proxy_ss << ":" << proxy_config_.port;
        }

        current_proxy_ = proxy_ss.str();

        // Test static proxy on startup
        std::cout << "Testing proxy: " << proxy_config_.host << ":" << proxy_config_.port << std::endl;
        if (!TestProxy(current_proxy_, 10)) {
            std::cerr << "[X] Proxy test failed. Proxy is not working!" << std::endl;
            exit(1);
        }
        std::cout << "[OK] Proxy is working" << std::endl;
    }
}

bool BulkProcessor::TestProxy(const std::string& proxy, int timeout_seconds) {
    if (proxy.empty()) {
        return true; // No proxy means direct connection, always "works"
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    std::string response;

    auto writeCallback = +[](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        std::string* buffer = reinterpret_cast<std::string*>(userp);
        size_t realsize = size * nmemb;
        buffer->append(reinterpret_cast<char*>(contents), realsize);
        return realsize;
    };

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.country.is");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout_seconds);

    // Parse and apply proxy
    std::string proxy_str = proxy;
    curl_proxytype proxy_type = CURLPROXY_HTTP;

    if (proxy_str.find("socks5://") == 0) {
        proxy_type = CURLPROXY_SOCKS5;
        proxy_str = proxy_str.substr(9);
    } else if (proxy_str.find("http://") == 0) {
        proxy_str = proxy_str.substr(7);
    }

    std::string auth;
    size_t at_pos = proxy_str.find('@');
    if (at_pos != std::string::npos) {
        auth = proxy_str.substr(0, at_pos);
        proxy_str = proxy_str.substr(at_pos + 1);
    }

    curl_easy_setopt(curl, CURLOPT_PROXY, proxy_str.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxy_type);

    if (!auth.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, auth.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK);
}

std::string BulkProcessor::FetchCurrentIP() {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return "unknown";
    }

    std::string response;
    std::string proxy = GetCurrentProxy();

    // Callback function for curl
    auto writeCallback = +[](void* contents, size_t size, size_t nmemb, void* userp) -> size_t {
        std::string* buffer = reinterpret_cast<std::string*>(userp);
        size_t realsize = size * nmemb;
        buffer->append(reinterpret_cast<char*>(contents), realsize);
        return realsize;
    };

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.country.is");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // 5 second timeout

    // Apply proxy if configured
    if (!proxy.empty()) {
        std::string proxy_str = proxy;
        curl_proxytype proxy_type = CURLPROXY_HTTP;

        if (proxy_str.find("socks5://") == 0) {
            proxy_type = CURLPROXY_SOCKS5;
            proxy_str = proxy_str.substr(9);
        } else if (proxy_str.find("http://") == 0) {
            proxy_str = proxy_str.substr(7);
        }

        std::string auth;
        size_t at_pos = proxy_str.find('@');
        if (at_pos != std::string::npos) {
            auth = proxy_str.substr(0, at_pos);
            proxy_str = proxy_str.substr(at_pos + 1);
        }

        curl_easy_setopt(curl, CURLOPT_PROXY, proxy_str.c_str());
        curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxy_type);

        if (!auth.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXYUSERPWD, auth.c_str());
        }
    }

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return "unknown";
    }

    // Parse JSON response to extract IP
    // Response format: {"ip":"1.2.3.4","country":"US"}
    size_t ip_start = response.find("\"ip\":\"");
    if (ip_start != std::string::npos) {
        ip_start += 6; // Skip '{"ip":"'
        size_t ip_end = response.find("\"", ip_start);
        if (ip_end != std::string::npos) {
            return response.substr(ip_start, ip_end - ip_start);
        }
    }

    return "unknown";
}
