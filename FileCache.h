#pragma once

#include <ctime>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class FileCache{
public:
    explicit FileCache(size_t maxBytes = 64*1024*1024);
    std::shared_ptr<const std::string> get(const std::string& path);
    void put(const std::string& path, std::string content);

private:
    struct CacheEntry{
        std::string content;
        time_t mtime;   // 入库时的文件修改时间（stat 的原生类型）
        std::list<std::string>::iterator lruIt;  //标识位置的list的指针
    };

    void evictIfNeeded();
    void removeEntryLocked(const std::string& path, const CacheEntry* expected);

    std::unordered_map<std::string, std::shared_ptr<CacheEntry>> map_; //文件路径与文件缓存项对应表
    std::list<std::string> lru_;  //根据文件路径的文件未使用时间排序表
    size_t maxBytes_; //容量上限
    size_t usedBytes_ = 0; //已占用字节数
    static constexpr size_t kMaxFileToCache = 128*1024; //允许缓存的最大文件
    std::mutex cacheMutex_;


};