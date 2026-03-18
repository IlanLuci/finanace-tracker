#ifndef FILE_UTILS_HPP
#define FILE_UTILS_HPP

#include <string>
#include <ctime>
#include <vector>

// Utility functions for working with dates and formatting
namespace FileUtils
{
    // Date and time utilities
    std::string timeToString(time_t timestamp);
    time_t stringToTime(const std::string& datestr);
    std::string getCurrentDateString();
    time_t getCurrentTime();

    // Formatting utilities
    std::string formatCurrency(double amount);
    std::string formatPercentage(double percentage);

    // Path utilities
    bool fileExists(const std::string& filepath);
    bool directoryExists(const std::string& dirpath);
    bool createDirectory(const std::string& dirpath);
    std::string getFileName(const std::string& filepath);
    std::string getDirectoryPath(const std::string& filepath);

    // Data file utilities
    std::vector<std::string> listFilesInDirectory(const std::string& dirpath);
    std::vector<std::string> listDirectories(const std::string& dirpath);
    bool deleteFile(const std::string& filepath);
}

#endif

