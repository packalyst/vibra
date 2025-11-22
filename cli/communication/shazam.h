#ifndef CLI_COMMUNICATION_SHAZAM_H_
#define CLI_COMMUNICATION_SHAZAM_H_

#include <string>
#include <vector>
#include <cstdint>

// forward declaration
struct Fingerprint;
//

struct SegmentResult {
    uint32_t offset_ms;
    std::string response;
    std::string track_id;
    std::string title;
    std::string artist;
    int match_count;
};

class Shazam
{
    static constexpr char HOST[] = "https://amp.shazam.com/discovery/v5/fr/FR/android/-/tag/";

public:
    static std::string Recognize(const Fingerprint *fingerprint, const std::string& proxy = "");
    static std::string RecognizePrecise(const std::vector<Fingerprint*>& fingerprints, const std::string& proxy = "");
    static std::string RecognizeContinuous(const std::string& file_path, const std::string& proxy = "", int consecutive_required = 3);
    static std::string FetchExitIP(const std::string& proxy = "");
    static bool RequestNewTorCircuit(const std::string& password = "");
    static std::string FetchAppleMusicMetadata(const std::string& response, const std::string& proxy = "");
    static std::string extractAppleMusicId(const std::string& response);
    static std::string BuildUnifiedResponse(const std::string& response);

private:
    static std::string getShazamHost();
    static std::string getUserAgent();
    static std::string getRequestContent(const std::string &uri, unsigned int sample_ms);
    static std::string getTimezone();
    static std::string extractTrackId(const std::string& response);
    static std::string extractTitle(const std::string& response);
    static std::string extractArtist(const std::string& response);
    static int extractMatchCount(const std::string& response);
};

#endif // CLI_COMMUNICATION_SHAZAM_H_
