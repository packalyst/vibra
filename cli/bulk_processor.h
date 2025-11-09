#ifndef CLI_BULK_PROCESSOR_H_
#define CLI_BULK_PROCESSOR_H_

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>

struct BulkResult {
    std::string file_path;
    std::string json_response;
    bool success;
    std::string error_message;
};

struct BulkStats {
    std::atomic<int> total_files{0};
    std::atomic<int> processed{0};
    std::atomic<int> successful{0};
    std::atomic<int> failed{0};
    std::atomic<int> skipped{0};
};

class BulkProcessor {
public:
    BulkProcessor(const std::string& directory_path, const std::string& output_json_path,
                  int num_threads = 1, bool resume = false);

    // Main processing function
    void Process();

    // Configuration
    void SetThreadCount(int threads) { num_threads_ = threads; }
    void EnableResume(bool enable) { resume_enabled_ = enable; }
    void SetSupportedFormats(const std::vector<std::string>& formats);

    // Get statistics
    const BulkStats& GetStats() const { return stats_; }

private:
    // File discovery
    std::vector<std::string> ScanDirectory();
    bool IsSupportedFormat(const std::string& file_path);

    // Cache management
    void LoadCache();
    void SaveCache();
    bool IsAlreadyProcessed(const std::string& file_path);
    void AddToCache(const BulkResult& result);

    // Processing
    void ProcessFile(const std::string& file_path);
    void WorkerThread();

    // Progress display
    void DisplayProgress();
    void PrintFinalReport();

    // Member variables
    std::string directory_path_;
    std::string output_json_path_;
    int num_threads_;
    bool resume_enabled_;

    std::vector<std::string> supported_formats_;
    std::vector<std::string> files_to_process_;
    std::map<std::string, BulkResult> results_cache_;

    BulkStats stats_;
    std::mutex cache_mutex_;
    std::mutex queue_mutex_;
    std::mutex console_mutex_;

    size_t next_file_index_;
    std::atomic<bool> processing_complete_{false};
};

#endif // CLI_BULK_PROCESSOR_H_
