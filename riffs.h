#if !defined(riffs_h)
#define riffs_h

#include <string>
#include <vector>

class RiffFile;
class VideoFrame;

extern std::vector<RiffFile *> gRiffFiles;
extern std::vector<VideoFrame> gFrames;

void load_all_riffs(std::string const &path);

#endif // riffs_h
