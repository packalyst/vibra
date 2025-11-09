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

BulkProcessor::BulkProcessor(const std::string& directory_path, const std::string& output_json_path,
                             int num_threads, bool resume)
    : directory_path_(directory_path),
      output_json_path_(output_json_path),
      num_threads_(num_threads),
      resume_enabled_(resume),
      next_file_index_(0) {

    // Default supported formats
    supported_formats_ = {".mp3", ".wav", ".flac", ".ogg", ".m4a", ".aac"};

    if (resume_enabled_) {
        LoadCache();
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
    // Format: {"results": [{"file": "path", "response": {...}, "success": true}, ...]}
    size_t pos = 0;
    while ((pos = json_content.find("\"file\":", pos)) != std::string::npos) {
        size_t file_start = json_content.find("\"", pos + 7) + 1;
        size_t file_end = json_content.find("\"", file_start);
        std::string file_path = json_content.substr(file_start, file_end - file_start);

        // Find the corresponding response
        size_t response_start = json_content.find("\"response\":", file_end);
        size_t success_pos = json_content.find("\"success\":", file_end);

        if (response_start != std::string::npos && success_pos != std::string::npos) {
            BulkResult result;
            result.file_path = file_path;
            result.success = json_content.find("true", success_pos, 10) != std::string::npos;

            // Extract full response (simplified - assume it's the next object)
            size_t resp_obj_start = json_content.find("{", response_start);
            int brace_count = 1;
            size_t resp_obj_end = resp_obj_start + 1;

            while (brace_count > 0 && resp_obj_end < json_content.length()) {
                if (json_content[resp_obj_end] == '{') brace_count++;
                else if (json_content[resp_obj_end] == '}') brace_count--;
                resp_obj_end++;
            }

            result.json_response = json_content.substr(resp_obj_start, resp_obj_end - resp_obj_start);
            results_cache_[file_path] = result;
        }

        pos = file_end;
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
        // Serialize recognition calls to avoid threading issues in vibra library
        std::lock_guard<std::mutex> recog_lock(recognition_mutex_);

        // Generate fingerprint
        fingerprint = vibra_get_fingerprint_from_music_file(file_path.c_str());

        if (!fingerprint) {
            result.success = false;
            result.error_message = "Failed to generate fingerprint";
            stats_.failed++;
        } else {
            // Recognize with Shazam (with retry on rate limit)
            std::string response;
            int max_retries = 3;
            int retry_count = 0;
            bool success = false;

            while (retry_count < max_retries && !success) {
                response = Shazam::Recognize(fingerprint);

                // Validate response
                if (IsValidJSON(response)) {
                    success = true;
                } else {
                    // Check if it's a rate limit error
                    if (response.find("429") != std::string::npos ||
                        response.find("Too Many Requests") != std::string::npos) {
                        retry_count++;
                        if (retry_count < max_retries) {
                            // Wait before retry (exponential backoff)
                            std::this_thread::sleep_for(std::chrono::seconds(2 * retry_count));
                        }
                    } else {
                        // Other error, don't retry
                        break;
                    }
                }
            }

            if (success) {
                result.success = true;
                result.json_response = response;
                stats_.successful++;
            } else {
                result.success = false;
                if (response.find("429") != std::string::npos) {
                    result.error_message = "Rate limited by Shazam API";
                } else {
                    result.error_message = "Invalid response from Shazam";
                }
                stats_.failed++;
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
}

void BulkProcessor::WorkerThread() {
    while (true) {
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
            std::cout << "✓" << successful << " ✗" << failed;
            if (skipped > 0) std::cout << " ⊘" << skipped;
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
